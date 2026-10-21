// PC sampling profiler (from futari-nx): which functions the CPU time goes to on the PC build, so native rewrites of hot guest functions
// can be chosen and measured without a console run. The Switch's own sampler (sdk/src/ui/switch_perf.cpp,
// tools/switch_hot_functions.py) stays the reference: x86 and ARM code differ, and so do the hot spots a little.
//
// peggle1_pc_profile_s = N: a time-critical thread samples every thread of the process at peggle1_pc_profile_hz and every
// N seconds appends a report to logs/pc_profile.log next to the executable: per thread its busy share and its hottest
// functions (self time), the source lines inside the biggest ones, then the hottest functions over all threads. A
// thread is sampled only when its cycle counter moved by a quarter of a tick at 1 GHz since the last tick
// (QueryThreadCycleTime; the suspension itself runs a little code in the thread, so "moved at all" is not enough):
// waiting threads cost nothing and do not fill the report with wait functions. A sample is SuspendThread, GetThreadContext (Rip), ResumeThread, and nothing that could take a lock in
// between (the suspended thread might hold it). Symbols come from the PDBs, through DbgHelp, on the sampler thread only,
// at report time. Recompiled functions print as <module>!sub_XXXXXXXX (the __imp__ entry name stripped): module
// "peggle1" is default.xex's code (the only code module). tools/pc_hot_functions.py adds windows up.

#include "peggle1_pc_profiler.h"

#include <rex/cvar.h>

REXCVAR_DEFINE_INT32(peggle1_pc_profile_s, 0, "Peggle1",
                     "PC: sample every thread and append its hottest functions to logs/pc_profile.log every N "
                     "seconds; 0 = off")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(peggle1_pc_profile_hz, 1000, "Peggle1", "PC: samples per second of peggle1_pc_profile_s")
    .range(10, 4000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// windows.h first
#include <dbghelp.h>
#include <psapi.h>
#include <timeapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <rex/filesystem.h>
#include <rex/logging.h>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "psapi.lib")

namespace peggle1::pc_profiler {
namespace {

struct Sampled {
  DWORD tid = 0;
  HANDLE handle = nullptr;
  std::string name;
  ULONG64 last_cycles = 0;
  uint64_t last_cpu_100ns = 0;
  uint64_t busy = 0;  // samples in this window
  std::unordered_map<uint64_t, uint32_t> pcs;
  // Per sample: the pc and the first return addresses found on the stack (callers of the sampled function; 12 deep
  // since 2026-10-21, so a wait inside the runtime still reaches the guest function that called it).
  std::vector<std::array<uint64_t, 12>> stacks;
  bool alive = true;
};

// Code of our own modules (peggle1*, rex*): return addresses are looked for in these ranges only.
std::vector<std::pair<uint64_t, uint64_t>> g_code_ranges;

void RefreshModules() {
  HMODULE modules[512];
  DWORD needed = 0;
  if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed)) {
    return;
  }
  g_code_ranges.clear();
  for (DWORD i = 0; i < std::min<DWORD>(needed / sizeof(HMODULE), 512); ++i) {
    wchar_t name[MAX_PATH];
    MODULEINFO info{};
    if (!GetModuleBaseNameW(GetCurrentProcess(), modules[i], name, MAX_PATH) ||
        !GetModuleInformation(GetCurrentProcess(), modules[i], &info, sizeof(info))) {
      continue;
    }
    if (_wcsnicmp(name, L"peggle1", 7) == 0 || _wcsnicmp(name, L"rex", 3) == 0) {
      const uint64_t start = reinterpret_cast<uint64_t>(info.lpBaseOfDll);
      g_code_ranges.emplace_back(start + 0x1000, start + info.SizeOfImage);
    }
  }
}

// A return address of our code: in one of the ranges and right after a call instruction.
bool ReturnAddress(uint64_t a) {
  bool inside = false;
  for (const auto& [lo, hi] : g_code_ranges) {
    inside |= a >= lo + 8 && a < hi;
  }
  if (!inside) {
    return false;
  }
  const uint8_t* p = reinterpret_cast<const uint8_t*>(a);
  return p[-5] == 0xE8 || (p[-6] == 0xFF && p[-5] == 0x15) || (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10) ||
         (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10) || (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10);
}

std::atomic<bool> g_stop{false};
std::thread g_thread;

uint64_t CpuTime100ns(HANDLE thread) {
  FILETIME created, exited, kernel, user;
  if (!GetThreadTimes(thread, &created, &exited, &kernel, &user)) {
    return 0;
  }
  return ((uint64_t(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime) +
         ((uint64_t(user.dwHighDateTime) << 32) | user.dwLowDateTime);
}

std::string ThreadName(HANDLE thread) {
  using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
  static const auto get = reinterpret_cast<GetThreadDescriptionFn>(
      reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription")));
  std::string out;
  PWSTR wide = nullptr;
  if (get && SUCCEEDED(get(thread, &wide)) && wide) {
    for (const wchar_t* c = wide; *c; ++c) {
      out.push_back(*c < 128 ? char(*c) : '?');
    }
    LocalFree(wide);
  }
  return out;
}

// Threads of this process, the sampler excluded; new ones are opened, exited ones marked.
void RefreshThreads(std::vector<std::unique_ptr<Sampled>>& threads) {
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    return;
  }
  const DWORD pid = GetCurrentProcessId();
  const DWORD self = GetCurrentThreadId();
  THREADENTRY32 entry{};
  entry.dwSize = sizeof(entry);
  for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
    if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self) {
      continue;
    }
    const bool known = std::any_of(threads.begin(), threads.end(),
                                   [&](const auto& t) { return t->alive && t->tid == entry.th32ThreadID; });
    if (known) {
      continue;
    }
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                          entry.th32ThreadID);
    if (!h) {
      continue;
    }
    auto t = std::make_unique<Sampled>();
    t->tid = entry.th32ThreadID;
    t->handle = h;
    QueryThreadCycleTime(h, &t->last_cycles);
    t->last_cpu_100ns = CpuTime100ns(h);
    threads.push_back(std::move(t));
  }
  CloseHandle(snapshot);
  for (auto& t : threads) {
    DWORD code = 0;
    if (t->alive && GetExitCodeThread(t->handle, &code) && code != STILL_ACTIVE) {
      t->alive = false;
    }
  }
}

void SampleOnce(std::vector<std::unique_ptr<Sampled>>& threads, ULONG64 min_cycles) {
  for (auto& t : threads) {
    if (!t->alive) {
      continue;
    }
    ULONG64 cycles = 0;
    if (!QueryThreadCycleTime(t->handle, &cycles)) {
      continue;
    }
    const ULONG64 ran = cycles - t->last_cycles;
    t->last_cycles = cycles;
    if (ran < min_cycles) {
      continue;
    }
    if (SuspendThread(t->handle) == DWORD(-1)) {
      continue;
    }
    CONTEXT context{};
    context.ContextFlags = CONTEXT_CONTROL;
    const BOOL ok = GetThreadContext(t->handle, &context);
    // The top of the stack, copied while the thread stands still (a system call: no user-mode lock).
    uint64_t stack[512];
    SIZE_T got = 0;
    if (ok && !ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(context.Rsp), stack,
                                 sizeof(stack), &got)) {
      got = 0;
      ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(context.Rsp), stack, 256, &got);
    }
    ResumeThread(t->handle);
    if (ok) {
      ++t->pcs[context.Rip];
      ++t->busy;
      std::array<uint64_t, 12> sample{context.Rip};
      for (size_t i = 0, n = 1; i < got / 8 && n < sample.size(); ++i) {
        if (ReturnAddress(stack[i])) {
          sample[n++] = stack[i];
        }
      }
      t->stacks.push_back(sample);
    }
  }
}

// <module>!<function> for a code address, "__imp__" stripped (the recompiled function's real entry).
std::string Label(HANDLE process, uint64_t pc) {
  IMAGEHLP_MODULE64 module{};
  module.SizeOfStruct = sizeof(module);
  std::string out = SymGetModuleInfo64(process, pc, &module) ? module.ModuleName : "?";
  alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME]{};
  auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
  symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
  symbol->MaxNameLen = MAX_SYM_NAME;
  DWORD64 displacement = 0;
  if (SymFromAddr(process, pc, &displacement, symbol)) {
    std::string name(symbol->Name, symbol->NameLen);
    if (name.rfind("__imp__", 0) == 0) {
      name.erase(0, 7);
    }
    out += "!" + name;
  } else {
    char text[32];
    std::snprintf(text, sizeof(text), "+0x%llx", static_cast<unsigned long long>(pc - module.BaseOfImage));
    out += text;
  }
  return out;
}

// "file:line" of a code address (the PDB's line table), "" without one.
std::string Line(HANDLE process, uint64_t pc) {
  IMAGEHLP_LINE64 line{};
  line.SizeOfStruct = sizeof(line);
  DWORD displacement = 0;
  if (!SymGetLineFromAddr64(process, pc, &displacement, &line) || !line.FileName) {
    return {};
  }
  std::string file = line.FileName;
  const size_t slash = file.find_last_of("/\\");
  if (slash != std::string::npos) {
    file.erase(0, slash + 1);
  }
  return file + ":" + std::to_string(line.LineNumber);
}

using Ranked = std::vector<std::pair<std::string, uint64_t>>;

Ranked Rank(const std::unordered_map<std::string, uint64_t>& counts) {
  Ranked ranked(counts.begin(), counts.end());
  std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
    return a.second != b.second ? a.second > b.second : a.first < b.first;
  });
  return ranked;
}

void Report(const std::filesystem::path& path, std::vector<std::unique_ptr<Sampled>>& threads, double seconds,
            uint64_t ticks, int hz) {
  HANDLE process = GetCurrentProcess();
  SymRefreshModuleList(process);
  std::unordered_map<uint64_t, std::string> labels;
  auto label_of = [&](uint64_t pc) -> const std::string& {
    auto it = labels.find(pc);
    if (it == labels.end()) {
      it = labels.emplace(pc, Label(process, pc)).first;
    }
    return it->second;
  };
  FILE* f = _wfopen(path.c_str(), L"a");
  if (!f) {
    return;
  }
  std::unordered_map<std::string, uint64_t> all;
  uint64_t all_samples = 0;
  std::fprintf(f, "== pc profile window %.1f s, %llu ticks (%d Hz asked)\n", seconds,
               static_cast<unsigned long long>(ticks), hz);
  std::sort(threads.begin(), threads.end(), [](const auto& a, const auto& b) { return a->busy > b->busy; });
  for (auto& t : threads) {
    if (t->name.empty()) {
      t->name = ThreadName(t->handle);
    }
    const uint64_t cpu = CpuTime100ns(t->handle);
    const double cpu_s = double(cpu - t->last_cpu_100ns) / 1e7;
    t->last_cpu_100ns = cpu;
    if (!ticks || t->busy * 100 < ticks) {  // under 1 % busy: not listed
      t->busy = 0;
      t->pcs.clear();
      t->stacks.clear();
      continue;
    }
    std::unordered_map<std::string, uint64_t> fns;
    for (const auto& [pc, n] : t->pcs) {
      fns[label_of(pc)] += n;
    }
    std::fprintf(f, "thread %lu \"%s\": busy %.1f%% (%llu samples), cpu %.2f s\n", t->tid, t->name.c_str(),
                 double(t->busy) * 100.0 / double(ticks), static_cast<unsigned long long>(t->busy), cpu_s);
    const Ranked ranked = Rank(fns);
    for (size_t k = 0; k < ranked.size() && k < 30; ++k) {
      std::fprintf(f, "   %5.1f%%  %s x%llu\n", double(ranked[k].second) * 100.0 / double(t->busy),
                   ranked[k].first.c_str(), static_cast<unsigned long long>(ranked[k].second));
    }
    // Inside the functions that hold a fifth of the thread or more: where, by source line.
    for (size_t k = 0; k < ranked.size() && k < 3; ++k) {
      if (ranked[k].second * 5 < t->busy) {
        break;
      }
      std::unordered_map<std::string, uint64_t> lines;
      for (const auto& [pc, n] : t->pcs) {
        if (label_of(pc) == ranked[k].first) {
          const std::string where = Line(process, pc);
          lines[where.empty() ? "?" : where] += n;
        }
      }
      std::fprintf(f, "     lines of %s:\n", ranked[k].first.c_str());
      const Ranked by_line = Rank(lines);
      for (size_t j = 0; j < by_line.size() && j < 20; ++j) {
        std::fprintf(f, "        %5.1f%%  %s\n", double(by_line[j].second) * 100.0 / double(ranked[k].second),
                     by_line[j].first.c_str());
      }
    }
    // Who calls the hottest functions: the first return address on the stack that is not the function itself nor
    // library code (fmt, spdlog, std): finds the caller of a hot helper.
    auto library = [](const std::string& l) {
      return l.find("fmt::") != std::string::npos || l.find("spdlog::") != std::string::npos ||
             l.find("!std::") != std::string::npos || l.find("!__") != std::string::npos;
    };
    for (size_t k = 0; k < ranked.size() && k < 25; ++k) {
      if (ranked[k].second * 100 < t->busy) {
        break;
      }
      std::unordered_map<std::string, uint64_t> callers;
      uint64_t with = 0;
      for (const auto& sample : t->stacks) {
        if (label_of(sample[0]) != ranked[k].first) continue;
        for (size_t j = 1; j < sample.size() && sample[j]; ++j) {
          const std::string& caller = label_of(sample[j]);
          if (caller != ranked[k].first && !library(caller)) {
            // The call site's line (the return address is just after the call).
            const std::string where = Line(process, sample[j] - 1);
            ++callers[where.empty() ? caller : caller + " @" + where];
            ++with;
            break;
          }
        }
      }
      if (!with) continue;
      const Ranked by_caller = Rank(callers);
      std::fprintf(f, "     callers of %s:", ranked[k].first.c_str());
      for (size_t j = 0; j < by_caller.size() && j < 4; ++j) {
        std::fprintf(f, " %s %.0f%%;", by_caller[j].first.c_str(),
                     double(by_caller[j].second) * 100.0 / double(with));
      }
      std::fprintf(f, "\n");
    }
    // peggle1-nx (2026-10-21): the nearest recompiled guest function of each sample (the pc's own, or the first on
    // the stack): what a thread that spends its time in the runtime (waits, locks) is doing for the game.
    {
      std::unordered_map<std::string, uint64_t> guest;
      uint64_t with = 0;
      for (const auto& sample : t->stacks) {
        for (size_t j = 0; j < sample.size() && sample[j]; ++j) {
          const std::string& l = label_of(sample[j]);
          if (l.find("!sub_") != std::string::npos) {
            ++guest[l];
            ++with;
            break;
          }
        }
      }
      if (with) {
        const Ranked by_guest = Rank(guest);
        std::fprintf(f, "     nearest guest functions (%.0f%% of the samples):",
                     double(with) * 100.0 / double(std::max<uint64_t>(t->stacks.size(), 1)));
        for (size_t j = 0; j < by_guest.size() && j < 6; ++j) {
          std::fprintf(f, " %s %.0f%%;", by_guest[j].first.c_str(),
                       double(by_guest[j].second) * 100.0 / double(with));
        }
        std::fprintf(f, "\n");
      }
    }
    t->stacks.clear();
    for (const auto& [label, n] : fns) {
      all[label] += n;
    }
    all_samples += t->busy;
    t->busy = 0;
    t->pcs.clear();
  }
  std::fprintf(f, "== all threads: %llu samples\n", static_cast<unsigned long long>(all_samples));
  const Ranked ranked = Rank(all);
  for (size_t k = 0; k < ranked.size() && k < 80; ++k) {
    std::fprintf(f, "   %5.1f%%  %s x%llu\n", double(ranked[k].second) * 100.0 / double(all_samples ? all_samples : 1),
                 ranked[k].first.c_str(), static_cast<unsigned long long>(ranked[k].second));
  }
  std::fclose(f);
  // Exited threads are dropped once reported.
  threads.erase(std::remove_if(threads.begin(), threads.end(),
                               [](const auto& t) {
                                 if (t->alive) {
                                   return false;
                                 }
                                 CloseHandle(t->handle);
                                 return true;
                               }),
                threads.end());
}

void Run(int seconds, int hz) {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
  timeBeginPeriod(1);
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_LOAD_LINES);
  const auto folder = rex::filesystem::GetExecutableFolder();
  SymInitializeW(GetCurrentProcess(), folder.c_str(), TRUE);
  std::error_code ec;
  std::filesystem::create_directories(folder / "logs", ec);
  const auto path = folder / "logs" / "pc_profile.log";
  if (FILE* f = _wfopen(path.c_str(), L"w")) {
    std::fprintf(f, "peggle1 PC profile: every %d s, %d Hz, busy threads only (self time per function)\n", seconds, hz);
    std::fclose(f);
  }
  std::vector<std::unique_ptr<Sampled>> threads;
  LARGE_INTEGER freq, start, now, last_refresh;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&start);
  last_refresh = start;
  RefreshThreads(threads);
  RefreshModules();
  const LONGLONG period = freq.QuadPart / hz;
  const ULONG64 min_cycles = 250000000ull / ULONG64(hz);  // a quarter of a tick at 1 GHz
  LONGLONG next = start.QuadPart + period;
  uint64_t ticks = 0;
  while (!g_stop.load(std::memory_order_relaxed)) {
    QueryPerformanceCounter(&now);
    if (now.QuadPart < next) {
      const LONGLONG ms = (next - now.QuadPart) * 1000 / freq.QuadPart;
      if (ms > 0) {
        Sleep(DWORD(ms));
      } else {
        SwitchToThread();
      }
      continue;
    }
    next = std::max(next + period, now.QuadPart - period);
    SampleOnce(threads, min_cycles);
    ++ticks;
    if (now.QuadPart - last_refresh.QuadPart > freq.QuadPart / 2) {
      RefreshThreads(threads);
      RefreshModules();
      last_refresh = now;
    }
    const double elapsed = double(now.QuadPart - start.QuadPart) / double(freq.QuadPart);
    if (elapsed >= seconds) {
      Report(path, threads, elapsed, ticks, hz);
      ticks = 0;
      QueryPerformanceCounter(&start);
    }
  }
  for (auto& t : threads) {
    CloseHandle(t->handle);
  }
  SymCleanup(GetCurrentProcess());
  timeEndPeriod(1);
}

}  // namespace

void Start() {
  const int32_t seconds = REXCVAR_GET(peggle1_pc_profile_s);
  if (seconds <= 0 || g_thread.joinable()) {
    return;
  }
  const int32_t hz = REXCVAR_GET(peggle1_pc_profile_hz);
  g_stop = false;
  g_thread = std::thread([seconds, hz]() { Run(seconds, hz); });
  REXLOG_INFO("[pc profile] sampling every thread at {} Hz, report every {} s in logs/pc_profile.log", hz, seconds);
}

void Stop() {
  g_stop = true;
  if (g_thread.joinable()) {
    g_thread.join();
  }
}

void LogHostStack(const char* what) {
  static bool symbols = false;
  const HANDLE process = GetCurrentProcess();
  if (!symbols) {
    symbols = true;
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_LOAD_LINES);
    SymInitializeW(process, rex::filesystem::GetExecutableFolder().c_str(), TRUE);
  }
  void* frames[62];
  const USHORT count = CaptureStackBackTrace(1, 62, frames, nullptr);
  REXLOG_INFO("[host stack] {}: {} frames", what, count);
  for (USHORT i = 0; i < count; ++i) {
    REXLOG_INFO("[host stack]   {:2} {}", i, Label(process, reinterpret_cast<uint64_t>(frames[i])));
  }
}

}  // namespace peggle1::pc_profiler

#else

namespace peggle1::pc_profiler {
void Start() {}
void Stop() {}
void LogHostStack(const char*) {}
}  // namespace peggle1::pc_profiler

#endif
