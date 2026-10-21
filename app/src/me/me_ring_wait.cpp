// Mass Effect - the game's D3D wait for the GPU without spinning (native renderer).
//
// WHY (Switch stack profile)
//   sub_8222FA98 is the D3D check "has the GPU read pointer moved since the last iteration":
//   32 db16cyc delays, then it compares the word the GPU writes the
//   read pointer back to ([[device+10768]]) with the value seen last time ([r3+8]), and after 5000 ticks
//   without change declares the GPU hung. Its callers loop on it. On the console it was ~22 % of the busy
//   game thread (plus ~6 % in the caller), on a 3-core machine where the ring thread needs that core.
//
// WHAT IT DOES
//   If the read pointer is unchanged, it waits at most masseffect_wait_ring_max_us for the ring thread
//   to write it back again (NotifyRingProgress), then calls the original, which decides as before. The game
//   sees the same values, only later. With bit 0x04 of [device+10813] (GPU already declared hung) or
//   without the native renderer, it does not wait.

#include "me_native_system.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>

// peggle2-nx: on by default. Peggle 2 polls its GPU wait (sub_8261D378, peggle1_me_hooks.cpp) about 5 million times
// a second without it (a whole core); with it ~200 checks and the same 30 fps, and recordings that differ from a run
// without it no more than two runs without it differ from each other (docs/peggle1-notes.md).
REXCVAR_DEFINE_BOOL(masseffect_wait_blocking_ring, true, "Mass Effect",
                    "Native renderer: the game's D3D sleeps while waiting for the ring thread to advance "
                    "instead of spinning in sub_8222FA98")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_notify_no_lock, false, "Mass Effect",
                    "Ring thread: NotifyRingProgress takes the progress mutex only when the game's D3D wait is "
                    "registered as sleeping (sequentially consistent handshake on both sides) instead of on every call "
                    "(one per fence write and per ring segment). Same wake-ups")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_wait_ring_max_us, 2000, "Mass Effect",
                     "Native renderer: longest sleep per iteration of the game's D3D GPU wait, in microseconds")
    .range(100, 100000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace me::native {
namespace {

std::mutex g_progress_mutex;
std::condition_variable g_progress_cv;
std::atomic<uint32_t> g_progress{0};
std::atomic<int> g_waiting{0};

}  // namespace

// The ring thread bumps the counter after writing the read pointer back, and checks for waiters under the
// lock (an unlocked Dekker handshake lost wake-ups on ARM).
void NotifyRingProgress() {
  static const bool no_lock = REXCVAR_GET(masseffect_notify_no_lock);
  if (no_lock) {
    // Dekker handshake, sequentially consistent on both sides: the waiter registers in g_waiting and then checks
    // g_progress (hook below); this side bumps g_progress and then reads g_waiting. One of the two sees the other's
    // write. If a waiter is registered, the lock/unlock below waits for it to be inside the condition variable (it holds
    // the mutex from registering until wait_for releases it), so the notify cannot be lost.
    g_progress.fetch_add(1, std::memory_order_seq_cst);
    if (g_waiting.load(std::memory_order_seq_cst) <= 0) return;
    { std::lock_guard<std::mutex> lock(g_progress_mutex); }
    g_progress_cv.notify_all();
    return;
  }
  g_progress.fetch_add(1, std::memory_order_acq_rel);
  bool notify;
  {
    std::lock_guard<std::mutex> lock(g_progress_mutex);
    notify = g_waiting.load(std::memory_order_acquire) > 0;
  }
  if (notify) g_progress_cv.notify_all();
}

// peggle2-nx: the wait half of masseffect-nx's ring-wait hook, without the game's addresses (the game's hooks file
// calls it with its own device offsets). While `unchanged` says the GPU read pointer the game polls is still the one
// it saw last time, the caller sleeps until the ring thread writes it back again (the next NotifyRingProgress) or
// masseffect_wait_ring_max_us pass; then the game's own check runs as before and decides with the same values.
// Returns at once with masseffect_wait_blocking_ring off or without the native renderer (nobody would notify).
void WaitRingProgress(const std::function<bool()>& unchanged) {
  static const bool blocking = REXCVAR_GET(masseffect_wait_blocking_ring);
  static const auto max_wait = std::chrono::microseconds(REXCVAR_GET(masseffect_wait_ring_max_us));
  // Counters, reported every 10 s: calls, sleeps, sleeps ended by the ring (the rest by the time limit), time slept.
  static std::atomic<uint64_t> calls{0}, sleeps{0}, woken{0}, slept_us{0};
  static std::atomic<int64_t> next_report{0};
  const auto now = std::chrono::steady_clock::now();
  const int64_t now_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
  calls.fetch_add(1, std::memory_order_relaxed);
  int64_t due = next_report.load(std::memory_order_relaxed);
  if (now_ms >= due && next_report.compare_exchange_strong(due, now_ms + 10000)) {
    if (due) {
      REXLOG_INFO("[ring-wait] 10 s: {} checks of the game's GPU wait, {} sleeps ({} ended by the ring, the rest by "
                  "the {} us limit), {:.1f} ms slept", calls.exchange(0), sleeps.exchange(0), woken.exchange(0),
                  max_wait.count(), slept_us.exchange(0) / 1000.0);
    }
  }
  if (!blocking || !Enabled()) return;
  const uint32_t seen = g_progress.load(std::memory_order_acquire);
  if (!unchanged()) return;
  sleeps.fetch_add(1, std::memory_order_relaxed);
  bool progressed;
  {
    std::unique_lock<std::mutex> lock(g_progress_mutex);
    g_waiting.fetch_add(1, std::memory_order_seq_cst);
    progressed = g_progress_cv.wait_for(lock, max_wait,
                                        [&] { return g_progress.load(std::memory_order_seq_cst) != seen; });
    g_waiting.fetch_sub(1, std::memory_order_seq_cst);
  }
  woken.fetch_add(progressed ? 1 : 0, std::memory_order_relaxed);
  slept_us.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                  std::chrono::steady_clock::now() - now).count()),
                     std::memory_order_relaxed);
}

}  // namespace me::native

// banjo-nx: Mass Effect's hooks on its D3D busy-waits (sub_8222FA98 ring space, sub_822FE760 render-thread
// handoff, sub_82811750 occlusion poll) were here, with its device offsets (+10768 read-pointer word, +10813
// state). Banjo's equivalents are still to be identified (its device layout is 280 bytes further on for the
// shader fields: +12696 vs +12416), so only NotifyRingProgress above is kept.
