// Peggle's enhancements and their switches. The rule for every ReXGlue Switch port on this machine (the user's
// request, relayed from the Mushihimesama session together with the UnleashedRecomp-NX / MarathonRecomp-NX work):
//   - each enhancement has its own key (cvar) in peggle1.toml;
//   - it belongs to the CPU or the non-CPU group, and is active only when both its own key and its group's master
//     (peggle1_cpu_enhancements, peggle1_noncpu_enhancements) are on;
//   - one line at startup, [enhancements], lists every one and whether it is active, so a console log says exactly
//     what ran;
//   - what cannot be switched at run time (code generation options) is listed with how this build was made: the
//     only the optimised build is delivered (no A/B NRO pair; tools/codegen.ps1 -Classic is a local diagnostic);
//   - a verify configuration (peggle1_cpu_verify) proves the CPU enhancements give the same result as the original
//     paths: every self-check of the renderer's fast paths compares on every use and a "[verify]" line counts them.
// A master works by writing its members' "off" value before anything reads them, so the code behind a key only ever
// looks at its own key. The table is the one place to add an enhancement: tools/make_enhancement_configs.py writes
// the premade configs (app/switch/peggle1.toml, app/configs/*.toml) from it. Keep it in step with the notes.

#include "peggle1_enhancements.h"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(peggle1_cpu_enhancements, true, "Peggle1",
                    "Master switch of the CPU enhancements: off turns every one of them off whatever its own key says "
                    "(the [enhancements] line at startup lists them)");
REXCVAR_DEFINE_BOOL(peggle1_noncpu_enhancements, true, "Peggle1",
                    "Master switch of the non-CPU enhancements (rendering, loading): off turns every one of them off "
                    "whatever its own key says");
// From futari-nx: the verify configuration (app/configs/peggle1_cpu_verify.toml). The same enhancements, with every
// self-check of the renderer's fast paths comparing on EVERY use instead of the first masseffect_native_verify_n
// (2048) / masseffect_native_fast_copy_verify (256). Each comparison runs the original path too, so it is slower: for a
// test session, not for play. Every 10 s the log has "[verify] checks done: ... | differences: N"; N must be 0 (a
// difference also logs DIFFERENCE / MISMATCH and turns that one fast path off for the rest of the session).
REXCVAR_DEFINE_BOOL(peggle1_cpu_verify, false, "Peggle1",
                    "Verify mode: every fast path's self-check compares with the original path on every use; "
                    "\"[verify]\" log lines count the checks and the differences (must be 0). Slower: test only");

// The profiler, outside the enhancement table and its masters (the user's request, relayed by the SA2 session): on in
// the default peggle1.toml and every premade config except the verify one, so every console run leaves a profile and
// a master switched off cannot turn profiling off with it. Two plain keys:
//   switch_profile_stacks (here; sdk/src/ui/switch_perf.cpp reads it): the console CPU sampler, 1 ms stack samples of
//     every busy thread into logs/rex/rex_profile.log (as logs/rex/stacks_profile.flag does);
//   masseffect_gpu_marks_categories (the renderer's, default true): a GPU timestamp at every pass-category change, the
//     "GPU per Swap" lines with the time per category and the idle gap (the image does not change).
REXCVAR_DEFINE_BOOL(switch_profile_stacks, true, "Peggle1",
                    "Switch: the CPU sampler pauses busy threads 1000 times a second and logs every pc to "
                    "logs/rex/rex_profile.log (as logs/rex/stacks_profile.flag does); off only in the verify config");
// From sa2-nx: the console profile file at all (sdk/src/ui/switch_perf.cpp reads it by name). The release build's
// built-in settings turn it off (app/peggle1_switch_defaults.toml): no logs/rex/rex_profile.log, no sampling.
REXCVAR_DEFINE_BOOL(switch_profile_report, true, "Peggle1",
                    "Switch: write the console profile every 10 s to logs/rex/rex_profile.log (false = no file, no "
                    "sampling; the FPS overlay keeps going)");

// All the native replacements at once (each also has its own peggle1_cpu_native_* key), for A/B runs of one batch
// against the same build without it.
REXCVAR_DEFINE_BOOL(peggle1_cpu_natives, true, "Peggle1",
                    "The native replacements of hot guest functions (src/cpu): false turns all of them off");

// Written by tools/codegen.ps1 into app/generated/codegen_profile.txt and passed in by CMakeLists.txt.
#ifndef PEGGLE1_CODEGEN_PROFILE
#define PEGGLE1_CODEGEN_PROFILE "unknown"
#endif

namespace peggle1::enhancements {
namespace {

enum class Group { kCpu, kNonCpu };

// One row per enhancement: {name, group, key, on value, off value, what}. The on value is also Peggle 2's default when
// peggle1.toml (or the command line / environment) does not set the key; the off value is what the group's master
// writes. tools/make_enhancement_configs.py parses these rows: keep each one in this shape.
struct Enhancement {
  const char* name;
  Group group;
  const char* key;
  const char* on_value;
  const char* off_value;
  const char* what;
  // Never defaulted on: only a toml line (or the environment / command line) turns it on, and the premade configs
  // write its off value. For the ones other ports saw break (the shared notebook's known issues).
  bool explicit_only = false;
};
constexpr bool kExplicitOnly = true;

constexpr Enhancement kEnhancements[] = {
    // peggle1-nx (2026-10-21): the PM4-ring renderer's rows (masseffect_*) are not in this port.
    // sa2-nx's CPU rounds of 2026-10-12, the SDK's run-time keys (the rest is build-time: app/perf_overrides.toml and
    // the PEGGLE1_GEN_* / PEGGLE1_SWITCH_* options of app/CMakeLists.txt).
    {"critsec_pcr_owner", Group::kCpu, "kernel_critsec_pcr_owner", "true", "false",
     "critical sections take their owner from the guest context (r13 -> KPCR) instead of a thread lookup"},
    // The Switch build only (threading_posix.cpp): "not in this build" on the PC.
    {"thread_signal_skip_idle", Group::kCpu, "thread_signal_skip_idle", "true", "false",
     "no condition-variable notify (a syscall on Horizon) for an event or semaphore nobody is blocked on"},
    // peggle1-nx (2026-10-21): the game's five job workers spun on their job event (peggle1_worker_pool.cpp).
    {"worker_pool_sleep", Group::kCpu, "peggle1_worker_pool_sleep", "true", "false",
     "the game's five job workers sleep while their queue is empty instead of spinning on the job event"},
    {"asset_pack", Group::kNonCpu, "asset_pack", "true", "false",
     "game data read from game_root.nxpack instead of the folder (used anyway when only the pack exists)"},
    // ---- Renut's renderer (peggle1_renderer = "renut"; banjo-nx's keys, src/renut_engine/gpu), 2026-10-20 ----------
    // Its optimisations read their own keys, so without rows here a master set to false left them on (futari-nx's
    // finding; its classification). Each on value is the key's code default. Not rows: Renut's fixes and settings,
    // banjo-nx's game-specific keys (fur, light lists), store cards (none here), and the latency trim (off by default).
    // CPU: the render thread.
    {"renut_push_constants", Group::kCpu, "banjo_deko3d_push_constants", "true", "false",
     "Renut: float and shared constants pushed into fixed uniform buffers"},
    {"renut_ring_cleaner", Group::kCpu, "banjo_deko3d_ring_cleaner", "true", "false",
     "Renut: the upload ring's cache clean on the background core"},
    {"renut_capture_pool", Group::kCpu, "banjo_renut_capture_pool", "true", "false",
     "Renut: queued draws' captures from a pool"},
    {"renut_cpu_memos", Group::kCpu, "banjo_renut_cpu_memos", "true", "false",
     "Renut: the render thread's per-draw lookups memoised (targets, textures, streams, memexport)"},
    {"renut_pipeline_memo", Group::kCpu, "banjo_renut_pipeline_memo", "true", "false",
     "Renut: a memo of the pipeline each key gave"},
    {"renut_shared_dedupe", Group::kCpu, "banjo_renut_shared_dedupe", "true", "false",
     "Renut: shared constants equal to the previous draw's not uploaded again"},
    {"renut_queue_notify_idle", Group::kCpu, "banjo_renut_queue_notify_idle", "true", "false",
     "Renut: queued work wakes the render thread only while it waits"},
    {"renut_record_worker", Group::kCpu, "banjo_renut_record_worker", "true", "false",
     "Renut: command recording split across two threads"},
    {"renut_pmull_hash", Group::kCpu, "banjo_renut_pmull_hash", "true", "false",
     "Renut (ARM): content hashes with PMULL folding"},
    {"renut_neon_swap", Group::kCpu, "banjo_renut_neon_swap", "true", "false",
     "Renut (ARM): vertex byte swaps 16 bytes per NEON reverse"},
    {"renut_upload_cached", Group::kCpu, "banjo_renut_upload_cached", "true", "false",
     "Renut (Switch): the upload ring in CPU-cached memory"},
    {"renut_ring_core", Group::kCpu, "banjo_ring_core", "1", "-1", "Renut (Switch): the render thread pinned to core 1"},
    {"renut_compile_core", Group::kCpu, "banjo_background_compile_core", "2", "-1",
     "Renut (Switch): pipeline prewarm and specialisation threads pinned to core 2"},
    {"renut_record_worker_core", Group::kCpu, "banjo_renut_record_worker_core", "0", "-1",
     "Renut (Switch): the recording worker pinned to core 0"},
    // 2026-10-20 (the user: "wire up all of SA2's new CPU improvements"): sa2-nx's Renut CPU work
    // (D:\sonic adventure 2\sa2-nx\docs\renut-cpu-work-for-banjo.md; its keys keep sa2-nx's names).
    {"renut_snapshot_deltas", Group::kCpu, "sa2_renut_snapshot_deltas", "true", "false",
     "Renut: a draw sends only the 64-byte device chunks and constant groups that changed, not the whole 13 KB device"},
    {"renut_deferred_chain", Group::kCpu, "futari_renut_deferred_chain", "true", "false",
     "Renut: Begin*Vertices draws (nearly all of Peggle 2's) in the delta chain too (futari-nx's)"},
    {"renut_snapshot_deltas_constants", Group::kCpu, "sa2_renut_snapshot_deltas_constants", "true", "false",
     "Renut: the delta chain's draws push only their changed constant groups"},
    {"renut_snapshot_deltas_narrow", Group::kCpu, "sa2_renut_snapshot_deltas_narrow", "true", "false",
     "Renut: the delta chain compares only the 40 of 77 device chunks the render thread reads"},
    {"renut_neon_compare", Group::kCpu, "sa2_renut_neon_compare", "true", "false",
     "Renut (ARM): 64-byte lines compared with four 128-bit loads a side"},
    {"renut_capture_batches", Group::kCpu, "sa2_renut_capture_batches", "true", "false",
     "Renut: draw captures taken from and given back to their pool 32 at a time"},
    {"renut_callbacks_fast", Group::kCpu, "sa2_renut_callbacks_fast", "true", "false",
     "Renut: the hooks' first calls return without locks when nothing waits"},
    {"renut_wake_batch", Group::kCpu, "sa2_renut_wake_batch", "16", "1",
     "Renut: a sleeping render thread woken by the game's draws once 16 are queued (other items and waits at once)"},
    {"renut_prefetch_next", Group::kCpu, "sa2_renut_prefetch_next", "true", "false",
     "Renut: the next draw's capture prefetched while the render thread does the one before"},
    {"renut_done_notify_waiters", Group::kCpu, "sa2_renut_done_notify_waiters", "true", "false",
     "Renut: the render thread's done signal only when a thread waits for it"},
    {"renut_batch_clock", Group::kCpu, "sa2_renut_batch_clock", "true", "false",
     "Renut: one clock read per batch of render-thread work, not per item"},
    {"renut_chain_reuse", Group::kCpu, "sa2_renut_chain_reuse", "true", "false",
     "Renut: a chain draw reuses the previous draw's key, targets, viewport, textures and pipeline when unchanged"},
    {"renut_inline_line_compare", Group::kCpu, "sa2_renut_inline_line_compare", "true", "false",
     "Renut: the shared constants' lines compared inline, not by memcmp calls"},
    {"renut_stream_mask", Group::kCpu, "sa2_renut_stream_mask", "true", "false",
     "Renut: vertex streams walked by a mask of the used ones"},
    {"renut_texture_defaults_copy", Group::kCpu, "sa2_renut_texture_defaults_copy", "true", "false",
     "Renut: the default texture and sampler words as one block copy"},
    {"renut_fetch_slots_skip", Group::kCpu, "sa2_renut_fetch_slots_skip", "true", "false",
     "Renut: shared-constant lines known equal (textures, fetch slots) not compared"},
    {"renut_fetch_data_unread", Group::kCpu, "sa2_renut_fetch_data_unread", "true", "false",
     "Renut: a draw that fetches nothing by address keeps the fetch address 0 (no upload a draw)"},
    {"renut_shared_partial_clear", Group::kCpu, "sa2_renut_shared_partial_clear", "true", "false",
     "Renut: only the shared constants after the texture words zeroed a draw"},
    {"renut_lean_diagnostics", Group::kCpu, "sa2_renut_lean_diagnostics", "true", "false",
     "Renut: the per-draw diagnostic counts only while their 10 s lines are logged"},
    // peggle1-nx (2026-10-21): explicit only. On the user's console the game hung at the PopCap logo (white screen,
    // two cores at 100 %) right after this re-masked its threads at the first Swap, every guest thread silent while host
    // threads went on; one Ryujinx run stalled the same way. banjo-nx turned its own copy off for the same hang (4 of 12
    // emulator runs: RexSetGuestThreadCoreLayout re-masking running threads under the object table's lock).
    {"renut_core_layout", Group::kCpu, "sa2_renut_core_layout", "true", "false",
     "Renut (Switch): the guest main thread alone on core 0, the other guest threads on cores 1-2 preferring 2 (hung "
     "Peggle's first frames on the console)", kExplicitOnly},
    {"renut_texture_backoff", Group::kCpu, "peggle1_renut_texture_backoff", "32", "1",
     "Renut: an unchanged linear texture checked again after 2, 4 ... 32 frames (the native renderer's scheme)"},
    {"renut_hd_async", Group::kCpu, "peggle1_renut_hd_async", "true", "false",
     "Renut: the HD atlases read on a worker thread, not the render thread"},
    // Non-CPU: GPU work, memory, input latency.
    {"renut_alpha_spec", Group::kNonCpu, "banjo_deko3d_alpha_spec", "true", "false",
     "Renut deko3d: the alpha test's function in the pipeline (constant 903)"},
    {"renut_tess_spec", Group::kNonCpu, "banjo_deko3d_tess_spec", "true", "false",
     "Renut deko3d: no tessellation code in pipelines without it (constant 904)"},
    {"renut_early_hoist", Group::kNonCpu, "banjo_deko3d_early_hoist", "true", "false",
     "Renut deko3d: the early alpha test's texture fetch hoisted"},
    {"renut_early_z", Group::kNonCpu, "banjo_deko3d_early_z", "true", "false",
     "Renut deko3d: depth tested before a discarding shader in a draw that writes no depth (constant 909)"},
    {"renut_txq_constants", Group::kNonCpu, "banjo_deko3d_txq_constants", "true", "false",
     "Renut deko3d: texture sizes from a buffer, not a texture query (constant 908)"},
    {"renut_vs_outputs", Group::kNonCpu, "banjo_deko3d_vs_outputs", "true", "false",
     "Renut deko3d: vertex shaders write only what the pixel shader reads"},
    {"renut_uam_prmt", Group::kNonCpu, "banjo_deko3d_uam_prmt", "true", "false", "Renut deko3d: uam's byte swaps as PRMT"},
    {"renut_uam_gvn", Group::kNonCpu, "banjo_deko3d_uam_gvn", "true", "false",
     "Renut deko3d: uam's global value numbering"},
    {"renut_uam_uniform_branches", Group::kNonCpu, "banjo_deko3d_uam_uniform_branches", "true", "false",
     "Renut deko3d: uam's uniform branches without reconvergence"},
    {"renut_uam_wide_rows", Group::kNonCpu, "banjo_deko3d_uam_wide_rows", "true", "false",
     "Renut deko3d: uam's 128-bit bone-row loads"},
    {"renut_zcull", Group::kNonCpu, "banjo_deko3d_zcull", "true", "false", "Renut deko3d: hierarchical depth culling"},
    {"renut_depth_compression", Group::kNonCpu, "banjo_deko3d_depth_compression", "true", "false",
     "Renut deko3d: compressed depth targets"},
    {"renut_depth24", Group::kNonCpu, "banjo_deko3d_depth24", "true", "false", "Renut deko3d: D24S8 targets as 24-bit depth"},
    {"renut_depth_handover", Group::kNonCpu, "banjo_deko3d_depth_handover", "true", "false",
     "Renut deko3d: a whole depth resolve handed over, not drawn"},
    {"renut_draw_resolves", Group::kNonCpu, "banjo_deko3d_draw_resolves", "true", "false",
     "Renut deko3d: colour resolves drawn on the 3D engine"},
    {"renut_barrier_lean", Group::kNonCpu, "banjo_deko3d_barrier_lean", "true", "false",
     "Renut deko3d: write-only transitions keep the texture cache"},
    {"renut_barrier_merge", Group::kNonCpu, "banjo_deko3d_barrier_merge", "true", "false",
     "Renut deko3d: back-to-back barriers merged"},
    {"renut_depth_only", Group::kNonCpu, "banjo_gpu_depth_only", "true", "false",
     "Renut: depth-only pipelines for colourless draws"},
    {"renut_depth_words_view", Group::kNonCpu, "banjo_gpu_depth_words_view", "true", "false",
     "Renut: a depth resolve's words read through its own texture"},
    {"renut_output_trim", Group::kNonCpu, "banjo_gpu_output_trim", "true", "false",
     "Renut: colour outputs a pipeline never writes trimmed"},
    {"renut_bool_spec", Group::kNonCpu, "banjo_gpu_bool_spec", "true", "false",
     "Renut: bool constants as specialisation constants"},
    {"renut_tess_grid", Group::kNonCpu, "banjo_gpu_tess_grid", "true", "false",
     "Renut: tessellated patches as a precomputed grid"},
    {"renut_robustness_off", Group::kNonCpu, "banjo_gpu_robustness_off", "true", "false",
     "Renut (Vulkan): pipelines without buffer and image robustness"},
    {"renut_nvk_draw_contract", Group::kNonCpu, "banjo_gpu_nvk_draw_contract", "true", "false",
     "Renut on NVK: NVK's per-draw contract"},
    {"renut_blend_skip", Group::kNonCpu, "banjo_gpu_blend_skip", "true", "false",
     "Renut: pixels whose blend changes nothing discarded"},
    {"renut_precise_barriers", Group::kNonCpu, "banjo_renut_precise_barriers", "true", "false",
     "Renut (Vulkan): barriers' access masks from the layouts"},
    {"renut_push_point_only", Group::kNonCpu, "banjo_renut_push_point_only", "true", "false",
     "Renut (Vulkan): the push constant read only by point sprites"},
    {"renut_resolve_skip", Group::kNonCpu, "banjo_renut_resolve_skip", "true", "false",
     "Renut: a resolve of an unchanged source into the same texture not done again"},
    {"renut_depth_words_lazy", Group::kNonCpu, "banjo_renut_depth_words_lazy", "true", "false",
     "Renut: a depth resolve's words written only when read"},
#if REX_PLATFORM_SWITCH  // (off by default on the PC)
    {"renut_memory_policy", Group::kNonCpu, "banjo_renut_memory_policy", "true", "false",
     "Renut: memory limits sized for the console"},
#endif
    {"renut_texture_cap", Group::kNonCpu, "peggle1_renut_texture_cap_mb", "512", "0",
     "Renut (deko3d): host textures above 512 MB released when not bound for 120 frames"},
    {"renut_pipeline_cache", Group::kNonCpu, "banjo_renut_pipeline_cache", "true", "false",
     "Renut (Vulkan): the driver's pipelines kept on disk"},
    {"renut_cache_save_quiet", Group::kNonCpu, "banjo_renut_cache_save_quiet", "true", "false",
     "Renut: the pipeline cache saved once no pipeline was built for a while"},
    {"renut_linked_stages", Group::kNonCpu, "banjo_renut_linked_stages", "true", "false",
     "Renut on NVK (Switch): each pipeline's stages compiled together"},
    {"renut_kick_submit", Group::kNonCpu, "banjo_deko3d_kick_submit", "true", "false",
     "Renut deko3d, input lag: command lists handed to the GPU at the game's kicks"},
    {"renut_late_input", Group::kNonCpu, "banjo_late_input", "true", "false",
     "Renut, input lag: a frame's first controller read as late as the render thread's slack allows"},
    // 2026-10-20 (banjo-nx's input-latency set, on by default at the user's request through banjo-nx's session;
    // banjo_deko3d_latency_trim stays off). banjo_late_input_gpu (late input's GPU-queue slack) taken out again the same
    // day, the user's request through banjo-nx's session: on banjo-nx's console (package 36q) it ratcheted the
    // controller read's delay up with late input's own slack (29 ms a frame instead of 8-11) and cost 5 fps where the
    // GPU is the limit, nothing at the 30 fps cap. Its code and key were then removed (banjo-nx's 36s).
    {"renut_eager_kicks", Group::kNonCpu, "banjo_deko3d_eager_kicks", "true", "false",
     "Renut deko3d, input lag: lists of 100 draws, up to 6 a frame, each sent to the GPU at its own kick"},
    {"renut_display_sync", Group::kNonCpu, "banjo_display_sync", "true", "false",
     "Renut on the Switch, input lag: the frame limiter paced by the display's vsync event (phase auto, at most 12 ms)"},
};

// Settings of an enhancement that are not on/off themselves: Peggle 2's default when nothing sets them.
struct Parameter {
  const char* key;
  const char* value;
};

constexpr Parameter kParameters[] = {
    {"masseffect_native_deferred_recording_naps", "8"},  // masseffect-nx's value with deferred recording
    // The full game, not the trial (the user's request, 2026-10-13): the SDK's default licence mask is 0, which the
    // game reads as the trial version; with no toml line it is the purchased game.
    {"license_mask", "1"},
};

// Nothing set the key (no toml line, environment variable or command-line flag): its value is the code's default.
// By source, not by value: a toml line that sets a key to its code default (e.g. a flag = false) is the user's choice
// and must stay. An old peggle1.toml without the newer keys gets Peggle 2's defaults for them.
bool UnsetByUser(const char* key) {
  return !rex::cvar::GetFlagByName(key).empty() && rex::cvar::GetFlagSource(key) == rex::cvar::Source::kDefault;
}

bool IsOff(const Enhancement& e, std::string_view value) {
  return value.empty() || value == e.off_value || (std::string_view(e.off_value) == "false" && value == "0");
}

// The natives registered at static initialisation (PEGGLE1_NATIVE): a function-local static, so registration does not
// depend on the order in which translation units are initialised.
std::vector<Native*>& Natives() {
  static std::vector<Native*> natives;
  return natives;
}

bool g_verify = false;
std::atomic<uint64_t> g_native_checks{0}, g_native_differences{0};

std::mutex g_report_mutex;
std::condition_variable g_report_cv;
bool g_report_stop = false;
std::thread g_report_thread;

}  // namespace

bool RegisterNative(Native* n) {
  Natives().push_back(n);
  return true;
}

bool VerifyMode() { return g_verify; }

void VerifyCheck(uint64_t n) { g_native_checks.fetch_add(n, std::memory_order_relaxed); }
std::atomic<uint64_t> g_native_check_ns{0};
void CheckTime(uint64_t ns) { g_native_check_ns.fetch_add(ns, std::memory_order_relaxed); }

void VerifyDifference() {
  g_native_differences.fetch_add(1, std::memory_order_relaxed);
}

bool GuestCodeMatches(const uint8_t* base, uint32_t address, uint32_t bytes, uint64_t fnv, const char* what) {
  // The image is big-endian guest bytes, hashed as tools/guest_code_hash.py hashes the dumped image. Code lives in
  // the XEX range (0x82000000-), which no host offset applies to.
  uint64_t h = 0xCBF29CE484222325ull;
  const uint8_t* p = base + address;
  for (uint32_t i = 0; i < bytes; ++i) {
    h ^= p[i];
    h *= 0x100000001B3ull;
  }
  if (h != fnv) {
    REXLOG_WARN("[hot] {}: the game code at {:08X} ({} bytes) is not the code the native was written from (FNV "
                "{:016X}, expected {:016X}): left off",
                what, address, bytes, h, fnv);
    return false;
  }
  return true;
}

void StartReport() {
  if (Natives().empty() || g_report_thread.joinable()) return;
  g_report_stop = false;
  g_report_thread = std::thread([] {
    std::vector<uint64_t> last(Natives().size(), 0);
    uint64_t last_checks = 0, last_check_ns = 0;
    std::unique_lock<std::mutex> lock(g_report_mutex);
    while (!g_report_cv.wait_for(lock, std::chrono::seconds(10), [] { return g_report_stop; })) {
      std::string uses;
      for (size_t i = 0; i < Natives().size(); ++i) {
        const uint64_t now = Natives()[i]->uses.load(std::memory_order_relaxed);
        if (now != last[i]) uses += fmt::format(" {} {};", Natives()[i]->key, now - last[i]);
        last[i] = now;
      }
      const uint64_t checks = g_native_checks.load(std::memory_order_relaxed);
      const uint64_t check_ns = g_native_check_ns.load(std::memory_order_relaxed);
      if (!uses.empty() || checks != last_checks) {
        // The time in guarded calls: the native on a copy, the rollback, the original and the comparison, summed
        // over every thread (a guarded call of an audio mixing copy is on the audio thread's clock).
        REXLOG_INFO("[hot] 10 s: native calls:{} | checks {} (total {}), {:.1f} ms in guarded calls, differences {}",
                    uses.empty() ? " none" : uses, checks - last_checks, checks, (check_ns - last_check_ns) / 1e6,
                    g_native_differences.load(std::memory_order_relaxed));
      }
      last_checks = checks;
      last_check_ns = check_ns;
      CodegenChecksReport(10);  // sa2-nx's CPU rounds: the generated code's self-checks and censuses (PC)
    }
  });
}

void StopReport() {
  {
    std::lock_guard<std::mutex> lock(g_report_mutex);
    g_report_stop = true;
  }
  g_report_cv.notify_all();
  if (g_report_thread.joinable()) g_report_thread.join();
}

void Apply() {
  // Peggle 2's defaults first (anything the toml, command line or environment set wins), then the masters.
  int defaulted = 0;
  for (const Enhancement& e : kEnhancements) {
    if (UnsetByUser(e.key) && !e.explicit_only) {
      rex::cvar::SetFlagByName(e.key, e.on_value);
      ++defaulted;
    }
  }
  for (const Parameter& p : kParameters) {
    if (UnsetByUser(p.key)) {
      rex::cvar::SetFlagByName(p.key, p.value);
    }
  }
  const bool cpu = REXCVAR_GET(peggle1_cpu_enhancements);
  const bool noncpu = REXCVAR_GET(peggle1_noncpu_enhancements);
  std::string line = fmt::format("[enhancements] masters: cpu {}, non-cpu {}; {} of {} keys not set by the toml, "
                                 "environment or command line got Peggle's on value;",
                                 cpu ? "on" : "OFF", noncpu ? "on" : "OFF", defaulted, std::size(kEnhancements));
  for (const Enhancement& e : kEnhancements) {
    const bool master = e.group == Group::kCpu ? cpu : noncpu;
    std::string value = rex::cvar::GetFlagByName(e.key);
    if (value.empty()) {
      line += fmt::format(" {} not in this build;", e.name);
      continue;
    }
    if (!master && !IsOff(e, value)) {
      rex::cvar::SetFlagByName(e.key, e.off_value);
      value = rex::cvar::GetFlagByName(e.key);
    }
    line += fmt::format(" {} {} ({} = {}{});", e.name, IsOff(e, value) ? "off" : "on", e.key, value,
                        master ? "" : ", group master off");
  }
  // The natives (src/cpu/peggle1_hot_guest.cpp): CPU group, each with its own key (default on).
  g_verify = REXCVAR_GET(peggle1_cpu_verify);
  size_t natives_on = 0, natives_total = 0, ring_on = 0, ring_total = 0, gpu_on = 0, gpu_total = 0;
  std::string natives_off, ring_off, gpu_off;
  const bool natives = REXCVAR_GET(peggle1_cpu_natives);
  for (Native* n : Natives()) {
    // A renderer enhancement (PEGGLE1_RING_ENHANCEMENT) follows the CPU master, one of the non-CPU group
    // (PEGGLE1_RING_GPU_ENHANCEMENT) the non-CPU master; a native the CPU and natives masters.
    const bool master = n->gpu ? noncpu : (cpu && (n->ring || natives));
    if (!master && n->own()) rex::cvar::SetFlagByName(n->key, "false");
    n->active = n->own();
    std::string& off = n->gpu ? gpu_off : n->ring ? ring_off : natives_off;
    ++(n->gpu ? gpu_total : n->ring ? ring_total : natives_total);
    if (n->active) {
      ++(n->gpu ? gpu_on : n->ring ? ring_on : natives_on);
    } else {
      off += off.empty() ? n->key : std::string(", ") + n->key;
    }
  }
  line += fmt::format(" natives {} of {} on{}; ring enhancements {} of {} on{}; renderer GPU enhancements {} of {} on{};",
                      natives_on, natives_total, natives_off.empty() ? std::string() : " (off: " + natives_off + ")",
                      ring_on, ring_total, ring_off.empty() ? std::string() : " (off: " + ring_off + ")", gpu_on,
                      gpu_total, gpu_off.empty() ? std::string() : " (off: " + gpu_off + ")");
  // Build time: code generation options (app/perf_overrides.toml: CR, CTR, XER and the non-volatile registers as C++
  // locals), the same generated code for the PC and the Switch.
  const std::string_view profile = PEGGLE1_CODEGEN_PROFILE;
  // "+perf": tools/perf_codegen_post.py ran (direct calls, guest memory without volatile, loop barriers).
  line += fmt::format(" build: codegen {} (registers as locals {}; direct calls + plain guest memory + loop barriers {})",
                      profile, profile.starts_with("optimised") ? "on" : profile == "classic" ? "off" : "unknown",
                      profile.find("+perf") != std::string_view::npos ? "on" : "off");
  REXLOG_INFO("{}", line);
  // sa2-nx's CPU rounds: the compare-and-swap key's run-time side and the PC self-checks, then the forms self-check.
  CodegenChecksInit();
  FormsSelfCheck();
  for (const Enhancement& e : kEnhancements) {
    REXLOG_DEBUG("[enhancements] {} ({}, {}): {}", e.name, e.group == Group::kCpu ? "cpu" : "non-cpu", e.key,
                 e.what);
  }
  if (REXCVAR_GET(peggle1_cpu_verify)) {
    rex::cvar::SetFlagByName("masseffect_native_verify_n", "2147483647");
    rex::cvar::SetFlagByName("masseffect_native_fast_copy_verify", "2147483647");
    REXLOG_INFO("[verify] verify mode ON: every self-check of the fast paths compares on every use (slower; test "
                "sessions only). Look for \"[verify] ... differences: 0\" every 10 s (native renderer only)");
  }
}

}  // namespace peggle1::enhancements
