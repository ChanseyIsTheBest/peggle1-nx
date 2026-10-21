// Peggle 2: data checks along the path of a texture. See peggle1_texdiag.h.

#include "peggle1_texdiag.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <string>
#include <system_error>
#include <thread>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>

REXCVAR_DEFINE_BOOL(peggle1_diag_textures, false, "Peggle1",
                    "Diagnostic: log a CRC-32 of every stream the game inflates and of every texture of 1 MB or more "
                    "the renderer uploads, and write the first large textures to texdiag/ next to the executable "
                    "(tools/texdiag_check.py compares them with the archive)");

// The game's adler32 (zlib 1.1: r3 = adler, r4 = buffer or 0, r5 = length; returns r3). inflate reaches it only
// through the check function pointer of its block state, so replacing the function table's entry is enough.
extern "C" REX_FUNC(__imp__sub_824C4728);
// The decompress worker's thread routine (r3 = its job: the request and done flags, the input and output the main
// thread asked for, and the z_stream at +44). Reached through the function table when the thread starts.
extern "C" REX_FUNC(__imp__sub_8248F7E0);
// Bink's texture sets: the thunk of the routine that waits until the NEXT set's first plane texture is no longer in
// use by the GPU (D3DResource_BlockUntilNotBusy) before the next frame is decoded into that set. Called through a
// vtable, so the function table's entry is what runs.
extern "C" REX_FUNC(__imp__sub_824575F8);

#if REX_PLATFORM_WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace peggle1::texdiag {

namespace {

const std::array<uint32_t, 256>& Table() {
  static const std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  return table;
}

// zlib's crc32(): start with 0.
uint32_t Crc32(uint32_t crc, const uint8_t* data, size_t size) {
  const auto& t = Table();
  crc = ~crc;
  for (size_t i = 0; i < size; ++i) crc = t[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

// One inflate stream of a thread. `marks` holds the CRC-32 of everything up to each whole megabyte, so the first
// megabyte that differs from the archive can be named.
constexpr uint64_t kMark = uint64_t(1) << 20;
constexpr size_t kMarks = 96;
struct Stream {
  uint64_t bytes = 0;
  uint32_t crc = 0;
  uint32_t adler = 1;
  uint32_t calls = 0;
  uint32_t mark_count = 0;
  std::array<uint32_t, kMarks> marks{};
};
thread_local Stream t_stream;
// The job of the decompress worker running on this thread (0 on every other thread), and the request the last
// logged flush belonged to.
thread_local uint32_t t_job = 0;
thread_local uint32_t t_job_out = 0, t_job_in = 0, t_next_dst = 0;
std::atomic<uint32_t> g_jobs{0};
std::atomic<uint32_t> g_streams{0};
std::atomic<uint32_t> g_textures{0};
std::atomic<uint32_t> g_dumps{0};
std::atomic<uint64_t> g_dump_bytes{0};

// Host address of a guest address (the recompiled code's REX_RAW_ADDR: the 4 KB-page physical view at E0000000 sits
// one page further in the host mapping).
const uint8_t* Host(const uint8_t* base, uint32_t address) {
#if REX_PLATFORM_WIN32 || (REX_PLATFORM_MAC && REX_ARCH_ARM64)
  return base + address + (address >= 0xE0000000u ? 0x1000u : 0u);
#else
  return base + address;
#endif
}

uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, Host(base, address), 4);
  return __builtin_bswap32(v);
}

// A watcher for the first seconds of a video: the Y plane of both sets hashed every half millisecond, with a line
// when a plane starts to change and when it has been still for 3 ms (what was written, when, for how long).
std::atomic<bool> g_watch_started{false};
uint64_t HashBytes(const uint8_t* data, size_t bytes) {
  uint64_t h = 1469598103934665603ull;  // FNV over 8-byte words: only equality matters here
  for (size_t i = 0; i + 8 <= bytes; i += 8) {
    uint64_t w;
    std::memcpy(&w, data + i, 8);
    h = (h ^ w) * 1099511628211ull;
  }
  return h;
}
// Microseconds on one clock for the lines that are lined up against each other.
uint64_t Stamp() {
  static const auto zero = std::chrono::steady_clock::now();
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - zero)
                      .count());
}
std::atomic<const uint8_t*> g_frame_num{nullptr};  // host address of Bink's FrameNum for the video being watched
uint32_t FrameNum() {
  const uint8_t* f = g_frame_num.load(std::memory_order_relaxed);
  if (!f) return 99;
  uint32_t v;
  std::memcpy(&v, f, 4);
  return __builtin_bswap32(v);
}

#if REX_PLATFORM_WIN32
// The write trap of peggle1_diag_plane_trap_ms: the first write to a protected plane is noted here by the
// exception handler (which then makes the plane writable again), and the watcher thread logs it.
uint8_t* g_trap_host[3] = {};
uint32_t g_trap_guest[3] = {};
size_t g_trap_bytes = 0;
std::atomic<bool> g_trap_hit{false};
const uint8_t* g_trap_rip = nullptr;
uint32_t g_trap_frame_num = 0;
uint64_t g_trap_stamp = 0;
uint32_t g_trap_address = 0, g_trap_thread = 0;
size_t g_trap_offset = 0;
LONG CALLBACK TrapHandler(EXCEPTION_POINTERS* info) {
  if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
  const auto address = reinterpret_cast<uint8_t*>(info->ExceptionRecord->ExceptionInformation[1]);
  for (int k = 0; k < 3; ++k) {
    if (!g_trap_host[k] || address < g_trap_host[k] || address >= g_trap_host[k] + g_trap_bytes) continue;
    unsigned long old = 0;
    for (uint8_t* host : g_trap_host) VirtualProtect(host, g_trap_bytes, PAGE_READWRITE, &old);
    if (!g_trap_hit.exchange(true)) {
      g_trap_rip = reinterpret_cast<const uint8_t*>(info->ContextRecord->Rip);
      g_trap_offset = size_t(address - g_trap_host[k]);
      g_trap_address = g_trap_guest[k] + uint32_t(g_trap_offset);
      g_trap_thread = GetCurrentThreadId();
      g_trap_frame_num = FrameNum();
      g_trap_stamp = Stamp();
    }
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  return EXCEPTION_CONTINUE_SEARCH;
}
#endif

REXCVAR_DEFINE_INT32(peggle1_diag_plane_trap_ms, 0, "Peggle1",
                     "Diagnostic (PC, with peggle1_diag_textures): this many milliseconds after a video starts, the Y "
                     "plane of its first set is made read-only, so the next write to it faults and the log names the "
                     "host code that writes it (the run ends there). 0 = off");

void WatchPlanes(const uint8_t* plane0, const uint8_t* plane1, uint32_t address0, uint32_t address1, size_t bytes) {
#if REX_PLATFORM_WIN32
  if (const int32_t trap = REXCVAR_GET(peggle1_diag_plane_trap_ms); trap > 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(trap));
    // The same physical memory is mapped at A0000000, C0000000 and E0000000 (+ one page in the host): all three.
    const uint8_t* const membase = plane0 - address0 - (address0 >= 0xE0000000u ? 0x1000u : 0u);
    AddVectoredExceptionHandler(1, &TrapHandler);  // ahead of the SDK's handler, which would quietly unprotect
    const uint8_t* const module = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    const uint32_t addresses[2] = {address0, address1};
    for (int round = 0; round < 80; ++round) {
      const int set = round & 1;
      const uint32_t physical = addresses[set] & 0x1FFFFFFFu;
      g_trap_bytes = 0;
      int k = 0;
      for (uint32_t view : {0xA0000000u, 0xC0000000u, 0xE0000000u}) {
        g_trap_host[k] = const_cast<uint8_t*>(membase) + view + physical + (view == 0xE0000000u ? 0x1000u : 0u);
        g_trap_guest[k++] = view + physical;
      }
      g_trap_bytes = bytes;
      g_trap_hit.store(false, std::memory_order_release);
      unsigned long old = 0;
      for (uint8_t* host : g_trap_host) VirtualProtect(host, bytes, PAGE_READONLY, &old);
      for (int wait = 0; wait < 400 && !g_trap_hit.load(std::memory_order_acquire); ++wait) {
        std::this_thread::sleep_for(std::chrono::microseconds(500));
      }
      if (!g_trap_hit.load(std::memory_order_acquire)) {
        for (uint8_t* host : g_trap_host) VirtualProtect(host, bytes, PAGE_READWRITE, &old);
        REXLOG_INFO("[texdiag] trap {}: no write to plane set {} in 200 ms", round, set);
        continue;
      }
      REXLOG_INFO("[texdiag] trap {}: write to set {} at guest {:08X} (plane offset {}, row {}) by host thread {} at "
                  "peggle1.exe+0x{:X}, Bink's FrameNum {} at that moment, at {} us",
                  round, set, g_trap_address, g_trap_offset, g_trap_offset / 1280, g_trap_thread,
                  size_t(g_trap_rip - module), g_trap_frame_num, g_trap_stamp);
      std::this_thread::sleep_for(std::chrono::milliseconds(round % 3 ? 7 : 23));
    }
    return;
  }
#endif
  const uint8_t* planes[2] = {plane0, plane1};
  const uint32_t addresses[2] = {address0, address1};
  uint64_t last[2] = {HashBytes(plane0, bytes), HashBytes(plane1, bytes)};
  bool writing[2] = {false, false};
  auto began = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point since[2] = {began, began}, changed[2] = {began, began};
  uint32_t lines = 0;
  while (lines < 400 && std::chrono::steady_clock::now() - began < std::chrono::seconds(12)) {
    std::this_thread::sleep_for(std::chrono::microseconds(500));
    const auto now = std::chrono::steady_clock::now();
    for (int k = 0; k < 2; ++k) {
      const uint64_t h = HashBytes(planes[k], bytes);
      if (h != last[k]) {
        last[k] = h;
        changed[k] = now;
        if (!writing[k]) {
          writing[k] = true;
          since[k] = now;
          ++lines;
          REXLOG_INFO("[texdiag] watcher: plane set {} ({:08X}) is being written", k, addresses[k]);
        }
      } else if (writing[k] && now - changed[k] >= std::chrono::milliseconds(3)) {
        writing[k] = false;
        ++lines;
        REXLOG_INFO("[texdiag] watcher: plane set {} ({:08X}) still again; it was written for {:.1f} ms", k,
                    addresses[k],
                    std::chrono::duration<double, std::milli>(changed[k] - since[k]).count());
      }
    }
  }
  REXLOG_INFO("[texdiag] watcher: done");
}

// The fences the game's Direct3D compares in that wait: the texture's (stamped when a draw used it), the device's
// current one and the one the GPU has completed (a word the ring writes), before the wait and after it.
std::atomic<uint32_t> g_video_waits{0};
// Host address of the word Direct3D reads as "the fence the GPU has completed", once the device is known.
std::atomic<const uint8_t*> g_completed_fence{nullptr};
uint32_t CompletedFence() {
  const uint8_t* p = g_completed_fence.load(std::memory_order_relaxed);
  if (!p) return 0;
  uint32_t v;
  std::memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}
void VideoWait(PPCContext& ctx, uint8_t* base) {
  const uint32_t sets = ctx.r3.u32 + 76;
  const uint32_t count = Load32(base, sets), current_set = Load32(base, sets + 20);
  uint32_t next = current_set + 1;
  if (int32_t(next) >= int32_t(count) || !Load32(base, sets + 136 + next * 32)) next = 0;
  const uint32_t texture = Load32(base, sets + 136 + next * 32);
  // The device pointer of Direct3D (two globals, by process type: one of them is set).
  const uint32_t globals = (0x41000000u << 1) + 0x8E0u;
  // Which of the two is this thread's is decided by the process type. Only a value that looks like a guest pointer is
  // followed.
  const auto pointer = [](uint32_t v) { return v >= 0x30000000u && v < 0xFFFF0000u; };
  uint32_t device = 0;
  const uint32_t holder0 = Load32(base, globals), holder1 = Load32(base, globals + 4);
  const uint32_t device0 = pointer(holder0) ? Load32(base, holder0) : 0;
  const uint32_t device1 = pointer(holder1) ? Load32(base, holder1) : 0;
  for (uint32_t candidate : {device0, device1}) {
    if (!device && pointer(candidate) && pointer(Load32(base, candidate + 11024))) device = candidate;
  }
  if (g_video_waits.load(std::memory_order_relaxed) < 3) {
    REXLOG_INFO("[texdiag] video wait: device globals hold {:08X} and {:08X}, which point to {:08X} and {:08X}; "
                "device {:08X}: +11016 {:08X}, +11024 {:08X}, +11036 {:08X}, +13600 {:08X}",
                holder0, holder1, device0, device1, device, device ? Load32(base, device + 11016) : 0,
                device ? Load32(base, device + 11024) : 0, device ? Load32(base, device + 11036) : 0,
                device ? Load32(base, device + 13600) : 0);
  }
  if (device && !g_completed_fence.load(std::memory_order_relaxed)) {
    g_completed_fence.store(Host(base, Load32(base, device + 11024)), std::memory_order_relaxed);
  }
  const auto completed = [&] { return device ? Load32(base, Load32(base, device + 11024)) : 0u; };
  const uint32_t fence = texture ? Load32(base, texture + 8) : 0;
  const uint32_t before = completed(), device_fence = device ? Load32(base, device + 11036) : 0;
  const uint32_t flag = device ? Load32(base, device + 13600) : 0;
  g_frame_num.store(Host(base, sets + 20), std::memory_order_relaxed);
  if (count == 2 && !g_watch_started.exchange(true)) {
    const uint32_t y0 = Load32(base, sets + 136), y1 = Load32(base, sets + 136 + 32);
    if (y0 && y1) {
      const uint32_t a0 = Load32(base, y0 + 32) & 0xFFFFF000u, a1 = Load32(base, y1 + 32) & 0xFFFFF000u;
      const size_t bytes = size_t(Load32(base, sets + 4)) * Load32(base, sets + 8);
      if (a0 && a1 && bytes >= 4096 && bytes <= (size_t(8) << 20)) {
        REXLOG_INFO("[texdiag] watcher: Y planes {:08X} and {:08X}, {} bytes each", a0, a1, bytes);
        std::thread(WatchPlanes, Host(base, a0), Host(base, a1), a0, a1, bytes).detach();
      }
    }
  }
  if (g_video_waits.load(std::memory_order_relaxed) < 80) {
    REXLOG_INFO("[texdiag] video wait called for set {} (the set in use is {}) at {} us", next, current_set, Stamp());
  }
  const auto start = std::chrono::steady_clock::now();
  __imp__sub_824575F8(ctx, base);
  const uint64_t returned = Stamp();
  const uint32_t n = g_video_waits.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 80 || n % 64 == 0) {
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start);
    const uint32_t after = completed();
    REXLOG_INFO("[texdiag] video wait {}: set {} of {} (plane texture {:08X}, data {:08X}), its fence {}, device "
                "{:08X} fence {}, completed {} before and {} after ({} us), returned at {} us, device field 13600 = "
                "{:08X}{}",
                n, next, count, texture, texture ? Load32(base, texture + 32) & 0xFFFFF000u : 0, fence, device,
                device_fence, before, after, us.count(), returned, flag,
                device && fence && uint32_t(device_fence - fence) < uint32_t(device_fence - after)
                    ? " | RETURNED BEFORE THE GPU PASSED THE TEXTURE'S FENCE" : "");
  }
}

void Worker(PPCContext& ctx, uint8_t* base) {
  t_job = ctx.r3.u32;
  REXLOG_INFO("[texdiag] decompress worker started, job {:08X}", t_job);
  __imp__sub_8248F7E0(ctx, base);
  t_job = 0;
}

// What the main thread asked the worker for, as the job holds it at a flush, and where zlib's next_out is. zlib
// takes a flush's bytes off avail_out before it calls the check function and moves next_out only after both parts of
// a flush, so the place a part really goes to is out + out_size - avail_out - length: tools/texdiag_check.py works
// with that, not with the "to" address printed here (the build of 2026-10-13 13:31 prints next_out and marks parts
// by it, so nearly every flush gets a line and the "not at the start" remark means nothing by itself).
void NoteFlush(const uint8_t* base, uint32_t length) {
  const uint32_t job = t_job;
  if (!job) return;
  const uint32_t in = Load32(base, job + 16), in_size = Load32(base, job + 24);
  const uint32_t out = Load32(base, job + 20), out_size = Load32(base, job + 28);
  const uint32_t next_in = Load32(base, job + 44), avail_in = Load32(base, job + 48);
  const uint32_t dst = Load32(base, job + 56), avail_out = Load32(base, job + 60);
  const bool inside = dst >= out && uint64_t(dst) + length <= uint64_t(out) + out_size;
  const bool new_request = out != t_job_out || in != t_job_in || dst != t_next_dst;
  if (new_request || !inside) {
    REXLOG_INFO("[texdiag] job {}: in {:08X}+{} (next_in {:08X}, {} left), out {:08X}+{}, flush of {} to {:08X} "
                "({} left){}{}",
                g_jobs.fetch_add(1, std::memory_order_relaxed) + 1, in, in_size, next_in, avail_in, out, out_size,
                length, dst, avail_out, inside ? "" : " OUTSIDE THE OUTPUT ASKED FOR",
                dst != out && dst != t_next_dst ? " (not at the start of the output, not after the last flush)" : "");
  }
  t_job_out = out;
  t_job_in = in;
  t_next_dst = dst + length;
}

// The check function of zlib 1.1's inflate. A call with no buffer starts a check (the reset of the block state:
// at inflateInit, and again when the last block of a stream is done), so it closes the stream before it.
void Adler(PPCContext& ctx, uint8_t* base) {
  const uint32_t buffer = ctx.r4.u32;
  Stream& s = t_stream;
  if (!buffer) {
    if (s.bytes) {
      std::string marks;
      for (uint32_t i = 0; i < s.mark_count; ++i) {
        fmt::format_to(std::back_inserter(marks), "{}{:08X}", i ? " " : "", s.marks[i]);
      }
      REXLOG_INFO("[texdiag] stream {}: {} bytes inflated in {} flushes, crc32 {:08X}, adler32 {:08X}; crc32 at each "
                  "MB: {}",
                  g_streams.fetch_add(1, std::memory_order_relaxed) + 1, s.bytes, s.calls, s.crc, s.adler,
                  marks.empty() ? "-" : marks);
    }
    s = Stream{};
    __imp__sub_824C4728(ctx, base);
    return;
  }
  // The bytes as they are when zlib checks them: before the original runs, from the same memory.
  const uint8_t* data = base + buffer;
  uint64_t left = ctx.r5.u32;
  // For the timeline against the "[io] crc" lines of the reads: when a stream starts, and every 4 MB of it.
  if (!s.calls) {
    REXLOG_INFO("[texdiag] stream starts: first flush of {} bytes from {:08X}", left, buffer);
  }
  NoteFlush(base, uint32_t(left));
  while (left) {
    const uint64_t piece = std::min(left, kMark - (s.bytes & (kMark - 1)));
    s.crc = Crc32(s.crc, data, size_t(piece));
    s.bytes += piece;
    data += piece;
    left -= piece;
    if (!(s.bytes & (kMark - 1))) {
      if (s.mark_count < kMarks) s.marks[s.mark_count++] = s.crc;
      if (!(s.bytes & (4 * kMark - 1))) REXLOG_INFO("[texdiag] stream at {} MB", s.bytes >> 20);
    }
  }
  ++s.calls;
  __imp__sub_824C4728(ctx, base);
  s.adler = ctx.r3.u32;
}

}  // namespace

bool Enabled() {
  static const bool on = REXCVAR_GET(peggle1_diag_textures);
  return on;
}

void Install() {
  static std::atomic<bool> installed{false};
  if (!Enabled() || installed.load(std::memory_order_acquire)) return;
  // Written as a sum: tools/direct_calls.py takes a function address spelled out in app code for a hooked function.
  const uint32_t adler = (0x41260000u << 1) + 0x4728u;
  auto* const runtime = rex::Runtime::instance();
  auto* const dispatcher = runtime ? runtime->function_dispatcher() : nullptr;
  // Called at several points of the start-up: the entry exists once the game's functions are registered.
  if (!dispatcher || !dispatcher->HasAnyFunctionTable() || !dispatcher->GetFunction(adler)) return;
  if (installed.exchange(true)) return;
  const bool set = dispatcher->SetFunction(adler, &Adler);
  const uint32_t worker = (0x41247000u << 1) + 0x17E0u;
  const bool set_worker = dispatcher->GetFunction(worker) && dispatcher->SetFunction(worker, &Worker);
  const uint32_t video_wait = (0x4122B000u << 1) + 0x15F8u;
  const bool set_video = dispatcher->GetFunction(video_wait) && dispatcher->SetFunction(video_wait, &VideoWait);
  REXLOG_INFO("[texdiag] on: inflate streams {} (function table entry {:08X}), the decompress worker's requests {} "
              "(entry {:08X}), the video's wait for its next plane set {} (entry {:08X}), textures of 1 MB or more, "
              "the first large ones written to texdiag/, and every texture read checked for a change during the read",
              set ? "checked" : "NOT checked: the entry could not be replaced", adler,
              set_worker ? "logged" : "NOT logged", worker, set_video ? "logged" : "NOT logged", video_wait);
}

void NoteTexture(uint32_t physical_address, uint32_t width, uint32_t height, uint32_t format, const uint8_t* raw,
                 size_t bytes, bool was_uploaded_before, uint32_t pool_block) {
  if (!Enabled() || bytes < (size_t(1) << 20) || !raw) return;
  Install();  // a no-op once done
  constexpr size_t kSlices = 64;
  std::string slices;
  slices.reserve(kSlices * 9);
  uint32_t whole = 0;
  size_t zeros = 0;
  for (size_t k = 0; k < kSlices; ++k) {
    const size_t from = bytes * k / kSlices, to = bytes * (k + 1) / kSlices;
    whole = Crc32(whole, raw + from, to - from);
    const uint32_t part = Crc32(0, raw + from, to - from);
    for (size_t i = from; i < to; ++i) zeros += raw[i] == 0;
    fmt::format_to(std::back_inserter(slices), "{}{:08X}", k ? " " : "", part);
  }
  const uint32_t n = g_textures.fetch_add(1, std::memory_order_relaxed) + 1;
  std::string file = "not written";
  // The first large textures as they are in guest memory, at most 6 files and 72 MB.
  if (bytes >= (size_t(4) << 20) && g_dumps.load(std::memory_order_relaxed) < 6 &&
      g_dump_bytes.load(std::memory_order_relaxed) + bytes <= (uint64_t(72) << 20)) {
    std::error_code ec;
    const auto folder = rex::filesystem::GetExecutableFolder() / "texdiag";
    std::filesystem::create_directories(folder, ec);
    const auto path = folder / fmt::format("tex{:02}_{:08X}_{}x{}_f{}.bin", n, physical_address, width, height, format);
    if (std::FILE* out = std::fopen(path.string().c_str(), "wb")) {
      const size_t written = std::fwrite(raw, 1, bytes, out);
      std::fclose(out);
      if (written == bytes) {
        g_dumps.fetch_add(1, std::memory_order_relaxed);
        g_dump_bytes.fetch_add(bytes, std::memory_order_relaxed);
        file = path.filename().string();
      }
    }
  }
  REXLOG_INFO("[texdiag] texture {}: {:08X} {}x{} format {}, {} bytes, {} ({} zero bytes), pool block {:08X}, crc32 "
              "{:08X}, file {}; slices: {}",
              n, physical_address, width, height, format, bytes, was_uploaded_before ? "CHANGED" : "first upload",
              zeros, pool_block, whole, file, slices);
}

void NoteDifference(uint32_t physical_address, uint32_t width, uint32_t height, const uint8_t* copy,
                    const uint8_t* guest, size_t bytes) {
  static std::atomic<uint32_t> logged{0};
  if (logged.fetch_add(1, std::memory_order_relaxed) >= 40 || !width || !height) return;
  const size_t pitch = bytes / height;
  const auto count = [&](size_t& first, size_t& last, std::string& values) {
    size_t n = 0;
    first = bytes;
    last = 0;
    for (size_t i = 0; i < bytes; ++i) {
      if (copy[i] == guest[i]) continue;
      if (n < 6) fmt::format_to(std::back_inserter(values), " [row {} x {}: {:02X} -> {:02X}]", i / pitch, i % pitch,
                                copy[i], guest[i]);
      if (!n) first = i;
      last = i;
      ++n;
    }
    return n;
  };
  size_t first = 0, last = 0;
  std::string values;
  const size_t now = count(first, last, values);
  std::this_thread::sleep_for(std::chrono::milliseconds(4));
  size_t first_later = 0, last_later = 0;
  std::string values_later;
  const size_t later = count(first_later, last_later, values_later);
  REXLOG_INFO("[texdiag] texture {:08X} {}x{}: right after the copy {} of {} bytes differ from the copy (rows {} to "
              "{});{} | 4 ms later {} bytes differ (rows {} to {})",
              physical_address, width, height, now, bytes, now ? first / pitch : 0, now ? last / pitch : 0, values,
              later, later ? first_later / pitch : 0, later ? last_later / pitch : 0);
}

void NoteRead(uint32_t physical_address, uint32_t width, uint32_t height, uint32_t format, uint64_t frame,
              bool changed_meanwhile) {
  static std::atomic<uint64_t> reads{0}, torn{0}, plane_reads{0}, plane_torn{0};
  static std::atomic<int64_t> last_report{0};
  const bool plane = format == 2 && width >= 320;  // k_8 planes of a video
  reads.fetch_add(1, std::memory_order_relaxed);
  if (plane) plane_reads.fetch_add(1, std::memory_order_relaxed);
  if (changed_meanwhile) {
    const uint64_t n = torn.fetch_add(1, std::memory_order_relaxed) + 1;
    if (plane) plane_torn.fetch_add(1, std::memory_order_relaxed);
    if (n <= 60 || n % 64 == 0) {
      REXLOG_INFO("[texdiag] texture {:08X} {}x{} format {} CHANGED WHILE THE RENDERER READ IT (ring frame {}; {} "
                  "such reads so far)",
                  physical_address, width, height, format, frame, n);
    }
  }
  // The first plane reads of a video, with the fence the GPU has completed as Direct3D sees it at this moment: the
  // game's wait for this plane's set ("video wait" lines) must not be over before this read is.
  if (plane && width >= 1024) {
    static std::atomic<uint32_t> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 400) {
      REXLOG_INFO("[texdiag] plane {:08X} read (done at {} us) in ring frame {} with completed fence {}, Bink's "
                  "FrameNum {}{}",
                  physical_address, Stamp(), frame, CompletedFence(), FrameNum(),
                  changed_meanwhile ? " (changed during the read)" : "");
    }
  }
  const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::steady_clock::now().time_since_epoch()).count();
  int64_t last = last_report.load(std::memory_order_relaxed);
  if (now - last >= 10 && last_report.compare_exchange_strong(last, now)) {
    REXLOG_INFO("[texdiag] texture reads so far: {} ({} changed during the read); video planes {} ({} changed "
                "during the read)",
                reads.load(), torn.load(), plane_reads.load(), plane_torn.load());
  }
}

}  // namespace peggle1::texdiag
