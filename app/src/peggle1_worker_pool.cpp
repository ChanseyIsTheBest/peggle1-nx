// The game's job pool: its five workers sleep while there is no job (CPU enhancement peggle1_worker_pool_sleep).
//
// peggle1-nx (2026-10-21). The pool (constructor sub_8270D280) starts five workers (sub_8270D0F8, on hardware threads
// 1-5) that share one manual-reset event (CreateEvent(NULL, TRUE, FALSE, NULL), the pool's +0). The producer
// (sub_8270D330), under the pool's critical section (+4), queues a job, counts it (+36 queued, +44 pending) and sets the
// event. A worker:
//   A: WaitForSingleObject(event, INFINITE); taken (+40) >= queued (+36): back to A
//   B: under the critical section: a job left? take it (+40 += 1); none? NtClearEvent(event); then run the job and go
//      to B while jobs are left, else to A
// Only a worker that reaches B and finds no job clears the event. On the 360 the event wakes all five at once and the
// losers of the race for the job clear it. On the PC and the Switch the first worker to wake usually takes the job
// before the others look: they see nothing queued at A, wait again on the event that is still set, and spin - five
// threads at 100 % of a core each in the PC profile (WaitForSingleObject from 0x8270D140 about a million times a second
// per thread, every wait satisfied), the "two cores maxed" on the console.
//
// WHY EXACT: at the worker's wait (return address 0x8270D140) a satisfied wait that finds no job clears the event, as
//   the race's loser at B does, then looks at the queue again and waits again. The producer counts a job before it sets
//   the event, so the clear can only hide a set whose job the second look sees: a job queued before the clear is taken
//   (the worker returns to the game, which goes to B), one queued after it sets the event again. Jobs, their order and
//   who runs them are the game's: only the empty spins go.
// RISK: none known; a wait that is not satisfied (a timeout or an error) goes back to the game as it is.
// KEY: peggle1_worker_pool_sleep (CPU group).
//
// The diagnostic counter of the same waits (peggle1_diag_waits, which found this) lives here too: one hook of
// WaitForSingleObject (sub_82464DA8) for both.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/kernel/xboxkrnl/threading.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(peggle1_worker_pool_sleep, true, "Peggle1",
                    "The game's five job workers sleep while their queue is empty (a wait that finds no job clears the "
                    "job event, as the worker that lost the race for a job does) instead of spinning on the event left "
                    "set");
REXCVAR_DEFINE_BOOL(peggle1_diag_waits, false, "Peggle1",
                    "PC diagnostic: count WaitForSingleObject calls per thread, caller, handle and result; log the "
                    "busiest every 10 s");

namespace {

constexpr uint32_t kWorkerWaitReturn = 0x8270D140;  // sub_8270D0F8's WaitForSingleObject call returns here
constexpr uint32_t kPoolQueued = 36;                // jobs queued (the producer counts before it sets the event)
constexpr uint32_t kPoolTaken = 40;                 // jobs taken by the workers

// The pool of this guest thread when it is one of the pool's workers (the worker's start parameter), else 0.
thread_local uint32_t t_pool = 0;
std::atomic<uint64_t> g_empty_wakes{0};

uint32_t LoadGuest32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address + (address >= 0xE0000000u ? 0x1000u : 0u), 4);
  return __builtin_bswap32(v);
}

bool JobLeft(const uint8_t* base, uint32_t pool) {
  return int32_t(LoadGuest32(base, pool + kPoolTaken)) < int32_t(LoadGuest32(base, pool + kPoolQueued));
}

// peggle1_diag_waits: (thread, caller, handle, timeout, result) -> calls, reported every 10 s.
struct WaitKey {
  uint32_t thread, caller, handle, timeout, result;
  bool operator<(const WaitKey& o) const {
    return std::tie(thread, caller, handle, timeout, result) < std::tie(o.thread, o.caller, o.handle, o.timeout, o.result);
  }
};

std::mutex g_waits_mutex;
std::map<WaitKey, uint64_t> g_waits;
std::once_flag g_waits_reporter;

void StartWaitsReporter() {
  // Kept for the life of the process, never detached: on the Switch std::thread::detach throws std::system_error
  // (libnx has no pthread_detach), a std::terminate in the first emulator run with the counter on.
  new std::thread([] {
    for (;;) {
      std::this_thread::sleep_for(std::chrono::seconds(10));
      std::map<WaitKey, uint64_t> snapshot;
      {
        std::lock_guard lock(g_waits_mutex);
        snapshot.swap(g_waits);
      }
      std::multimap<uint64_t, WaitKey, std::greater<>> ranked;
      for (const auto& [k, n] : snapshot) ranked.emplace(n, k);
      std::string line;
      int shown = 0;
      for (const auto& [n, k] : ranked) {
        if (++shown > 10) break;
        line += fmt::format(" [thread {:08X} from {:08X} handle {:08X} timeout {:08X} -> {:08X}: {}]", k.thread,
                            k.caller, k.handle, k.timeout, k.result, n);
      }
      REXLOG_INFO("[waits] 10 s:{}; job pool waits that found no job (event cleared): {}",
                  line.empty() ? " none" : line, g_empty_wakes.exchange(0, std::memory_order_relaxed));
    }
  });
}

}  // namespace

extern "C" REX_FUNC(__imp__sub_8270D0F8);
REX_HOOK_RAW(sub_8270D0F8) {  // a job pool worker (thread entry; r3 = the pool), never returns
  t_pool = ctx.r3.u32;
  __imp__sub_8270D0F8(ctx, base);
}

extern "C" REX_FUNC(__imp__sub_82464DA8);
REX_HOOK_RAW(sub_82464DA8) {  // WaitForSingleObject(handle, timeout_ms)
  const uint32_t handle = ctx.r3.u32, timeout = ctx.r4.u32, caller = uint32_t(ctx.lr);
  const bool diag = REXCVAR_GET(peggle1_diag_waits);
  if (diag) std::call_once(g_waits_reporter, StartWaitsReporter);
  const bool pool_wait = t_pool && caller == kWorkerWaitReturn && REXCVAR_GET(peggle1_worker_pool_sleep);
  for (;;) {
    ctx.r3.u64 = handle;
    ctx.r4.u64 = timeout;
    __imp__sub_82464DA8(ctx, base);
    if (diag) {
      std::lock_guard lock(g_waits_mutex);
      ++g_waits[WaitKey{ctx.r13.u32, caller, handle, timeout, ctx.r3.u32}];  // r13: the guest PCR, one per thread
    }
    if (!pool_wait || ctx.r3.u32 != 0 || JobLeft(base, t_pool)) {
      return;
    }
    // Satisfied with no job queued: clear the event as the race's loser does (sub_8270D0F8 at 0x8270D1C4), then look
    // again - a job counted before the clear is the game's to take now; one counted after it sets the event again.
    rex::kernel::xboxkrnl::xeNtClearEvent(handle);
    g_empty_wakes.fetch_add(1, std::memory_order_relaxed);
    if (JobLeft(base, t_pool)) {
      ctx.r3.u64 = 0;  // WAIT_OBJECT_0, as the satisfied wait returned
      return;
    }
  }
}
