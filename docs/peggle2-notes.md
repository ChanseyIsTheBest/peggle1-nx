# Notes on the game's code

Addresses are guest addresses in `default.xex` (title `58411436`, version 0.0.0.4, built 2014-04-08, image
`0x82000000`+`0xDF0000`, code `0x82140000`-`0x828AF6A8`, ~34,000 functions) unless they say otherwise. Every entry
says how it was found, so it can be checked again after a new code generation. `tools/ppc_dis.py` prints a listing of
any range of the image dump (`out/default_image.bin`, written by codegen with `REXGLUE_DUMP_IMAGE`); the portable
LLVM has no PowerPC target, so it uses `tools/ppc_mini.py`'s decoder.

## Modules

| Module | Image | Code | What it is |
|---|---|---|---|
| `default.xex` | `0x82000000`+`0xDF0000` | `0x82140000`-`0x828AF6A8`, 34,045 functions | the game (PE name `PeggleNext360.exe`) |

No other code module. The only `XexLoadImage` caller is `sub_82319130` (flags 9), called from two places with
`"xboxkrnl.exe"` (string at `0x8207C670`, from `sub_82628B20`) and `"xam.xex"` (`0x820009CC`, from `sub_82317C20`):
the XDK's dynamic import lookup for `XexGetProcedureAddress`, not a game DLL.

Libraries recognised by their strings: PopCap's SexyAppFramework (`Sexy::` classes), libpng, zlib 1.1.3
(`inflate 1.1.3`), Wwise (`WwiseSoundBank`, `WwiseInteractiveAudioDriver`), Bink 2 (`Bink2VideoHandle@Sexy@@`).

## Code generation (2026-10-05)

| Run | Result | Fix |
|---|---|---|
| 1 | "target not in any function": `0x823320D8` from `0x823346CC`, `0x823319DC` | 1 entry (a one-instruction `b` to a Release, reached by two this-adjustor thunks) |
| 2 | passes; 33,777 functions | |
| `pointer_fixpoint.py` | pass 1: 146 candidates, 65 accepted; pass 2: fixpoint | 65 entries, each with the address of its pointer |
| by hand | 2 rejected candidates are real entries | `0x825C0508`, `0x8275E178` (below) |
| `setjmp`/`longjmp` | found by shape | `setjmp_address`, `longjmp_address` (below) |
| final | 34,045 functions; `pointer_fixpoint.py --dry-run`: 76 candidates, 0 accepted | |

### Functions reachable only through pointers

`tools/pointer_fixpoint.py` accepted 65 (vtable entries, callbacks built with `lis`/`addi`). Of the 81 it rejected:

- **2 real entries**, added by hand. In both the generator had registered the continuation as a function of its
  own, so the entry looked like a fragment or did not fit its gap:
  - `0x825C0508`, in the vtable around `0x8207394C` (5 of the 6 neighbouring words are registered functions): a
    17-case switch on a byte argument (`cmplwi cr6,r8,16`, table `0x825C0548`) whose case labels `0x825C05B8`,
    `05DC`, `0600`, `0624`, `0660`, `066C` and the default `0678` had become functions. Once the entry is declared they
    are labels again.
  - `0x8275E178`, a `qsort` comparator built at `0x827773FC` (`lis`/`addi` into r6, then `bl 0x825F03C8` with width
    4); reads only r3/r4. Its tail `0x8275E1EC` (`subfc r11,r11,r8`) had become a function that reads r8/r11 at entry.
- 57 "tail branch into a function body": all referenced from `{toState, action}` pairs (the word before the pointer is
  a small state number): C++ EH unwind funclets ending in `b __restgprlr_N` (`0x825F0A80`-`0x825F0ABC`) or in a
  catch continuation. Only run while an exception unwinds.
- 6 "extends beyond its gap" at `0x82482BD0`-`0x82484C80` and 2 at `0x8244B904`/`0x8244B924`: (address, state)
  pairs of C++ EH IP-to-state maps (the table ends with the FuncInfo magic `0x19930522`).
- 11 "no code": the XEX import thunks (`0x8288444C`-`0x8288468C`).

`tools/pointer_audit.py` (every code pointer in data or built in code that is not a function start): 976 SEH scope
records, 7,062 EH state-map entries, 226 jump-table targets, 30 addresses built in code (catch continuations after a
`b`, and bases of halfword jump tables), 420 "other" (data that looks like code addresses), and 1 vtable-like hit,
`0x825C0508` (above).

### Jump tables

`tools/switch_check.py`: 1,016 `bctr` in the code range; 206 table jumps (104 absolute, 61 byte, 41 halfword), all
generated with the right cases (0 truncated, 0 wrong targets, 0 missed). Checked by hand:

- `sub_82690D40` `0x8269124C` (halfword table `0x820A55F8`), no bound before it: the index r9 is bounded upstream
  (`lbz r9,4(r30); cmplwi cr6,r9,14; bgt` at `0x826911BC`), then a byte switch (`0x826911F4`) whose cases reach this
  dispatch with r9 unchanged (no write to r9 in `0x826911C4`-`0x82691290`). 15 real cases; the generator made 17:
  2 extra, unreachable.
- `sub_8272D870` `0x8272DF94` (halfword table `0x820B6710`): the index is `rlwinm r29,r11,20,28,31` (4 bits) at
  `0x8272DC58`, the only write to r29 before it: 16 cases, as generated.
- `0x82816A98` and `0x82818808`: tail calls through function-pointer tables with a stride (12 bytes at `0x820C5F10`,
  48 entries; 8 bytes at `0x820C19F0`, 21 entries), rightly indirect; every non-null entry is a registered function.

### setjmp / longjmp

Found by shape (a scan of the image for runs of stores into r3 and the mirrored loads):

- `setjmp` `0x825F7760`: first tries a hook pointer at `0x82D990E0` (`bnectr`), then `stfd f14-f31`, `std r13-r31`,
  the 128 VMX registers, cr at +304, lr at +308, r1 at +144, 0 at +312; returns 0.
- `longjmp` `0x825F7C30`: the mirror image (r7 = r3): unwinds to the registration at +308/+144 (`bl 0x82606BA0`),
  `lfd f14-f31`, `ld r13-r31`, VMX, `mtlr`/`mtcrf`, r1 from +144, returns r4 (1 if r4 was 0).

32 call sites; libpng's error path is the expected user.

### Known risk: exceptions

`_CxxThrowException` is `sub_825F2630` (copies the 32-byte exception record template, then `bl 0x8260D798` =
`RaiseException`): **865 call sites** (sa2 had 105). `RaiseException` has 4 callers: the throw, two SetThreadName
raises (`0x406D1388`, handled by the SDK) and `sub_82600138` (raises a computed code; floating-point exception path).
The SDK's `RtlRaiseException` handles only SetThreadName and otherwise breaks into the debugger, so any C++ throw ends
the game. The 976 SEH scope records mean many `__try` blocks too. Watch the boot logs for it.

## First boots on PC, emulated GPU (2026-10-05)

| Run | Reached | Notes |
|---|---|---|
| 1 (45 s) | still running | no crash, no unregistered call, no throw; XMA audio decoding, reads of `main.rsb` |
| 2 (video from 5 s) | EA logo, PopCap/Bjorn, autosave notice, title screen ("PLAY") | the game presents **30 fps** (`D3DDevice_Swap` count: 600 per 20 s) |
| 3 (A every 5 s from 25 s) | title -> main menu (Single Player, Multiplayer, ...) -> Bjorn's intro -> first level, aiming tutorial | the first A creates the `PROFILE` save (XamContentCreate mode 4 -> create, 1054 B), later presses reopen it |
| 4-5 (4 min, stick + triggers + A) | Bjorn 1-1 played: the tutorial closes with a trigger ("OK" shows a trigger icon), shots, pegs lit and cleared, score 14,000, x2 multiplier, free ball | log clean apart from the stubs below |

Input for unattended runs comes from `app/src/peggle2_input_script.cpp` (cvar `peggle2_input_script`, or
`boot_test.ps1 -InputScript`): a synthetic pad that the SDK merges into player 1. The game reads the pad only through
`XamInputGetState` (1 call site; no keystroke API), so a scripted state is all it needs.

Seen in the logs and harmless so far:

- `NetDll_getsockname STUB` (once at start) and `XamVoiceSetMicArrayIdleUsers STUB` (4 times at start).
- Failed opens of `properties\partner.xml`, `locales\EN-US\properties\partner.xml`, `subtitles\*.json`,
  `ShaderDumpxe:\CompareBackEnds`: relative paths with no device. The game probes for loose override files (and a
  development shader dump path) and then reads `main.rsb`; nothing is missing from the package.
- The emulated GPU's "Texture fetch constant ... has invalid type" warning, ~30 per second (as in sa2).

## Direct3D (Xbox 360 XDK 21256, statically linked)

Found with `tools/d3d_match.py` twice: against Banjo-Kazooie N&B (2008 XDK, banjo-nx's `renut_gpu_funcs.toml`;
`out/d3d_match.txt`) and against SA2 (2012 XDK, the functions confirmed in sa2-nx's `sa2-notes.md`, written to
`out/sa2_d3d_names.toml`; `out/d3d_match_sa2.txt`). Against SA2 almost every function has the identical shape
(score 1.00): Peggle 2's Direct3D is SA2's with other addresses.

| Function | Peggle 2 | SA2 / Banjo score | Confirmed by |
|---|---|---|---|
| `D3DDevice_Swap` | `sub_8261FE50` | 1.00 / 0.57 | the only caller of the kernel's `VdSwap`; hooked to count frames for the video recorder (`app/src/peggle2_frame_hooks.cpp`) |
| draw-time shader load (FlushState part) | `sub_8262B248` | 1.00 / - | called by BeginVertices, DrawIndexedVertices and `sub_82620D88` (below) |
| `D3DDevice_BeginVertices` | `sub_826221A0` | 1.00 / 0.81 | 3 callers |
| `D3DDevice_DrawIndexedVertices` | `sub_826226A8` | 1.00 / 0.90 | 1 caller |
| `D3DDevice_DrawVerticesUP` | `sub_82622660` | 1.00 / 0.89 | 1 caller; in SA2 DrawVertices follows it, here DrawIndexedVertices does |
| `D3DDevice_DrawVertices` | not linked | 0.76 / 0.72 (both pointing at BeginVertices) | no other caller of the shader load has its shape |
| resolve draw | `sub_82620D88` | 1.00 (SA2's `sub_82564340`) | 3rd caller of the shader load; only called by the Resolve `sub_82620FE8`: Direct3D's own copy draw, which the ring handles itself |
| `D3DDevice_CreateVertexShader` / `CreatePixelShader` | `sub_82613480` / `sub_82613298` | 1.00 / 1.00, 0.59 | |
| `D3DDevice_CreateVertexDeclaration` | `sub_82611CA0` | 1.00 / 0.81 | |
| `D3DDevice_Clear` | `sub_8261EE00` | 1.00 / 0.96 | |
| `D3DDevice_CreateTexture` / `CreateVertexBuffer` / `CreateIndexBuffer` | `sub_8261CE48` / `sub_82616BE0` / `sub_82616CB8` | 1.00 | |
| GPU interrupt callback | `sub_82622E88` | 1.00 | the log: `SetInterruptCallback(82622E88, 40070F00)` |
| vblank flip (`D1GRPH_PRIMARY_SURFACE_ADDRESS`) | `sub_8261EF88` | 1.00 | |
| ring kick-off (`CP_RB_WPTR`) | `sub_82623620` | 1.00 | |
| `D3DDevice_Resolve` | `sub_82620FE8` (to confirm) | - / 0.51 | |
| `D3DResource_AddRef` / `Release` / `GetType` | `sub_82616B00` / `sub_82617860` / `sub_82616808` | 0.94-1.00 (Banjo) | |
| GPU wait ("has the ring read pointer moved?") | `sub_8261D378` | Mass Effect's `sub_8222FA98` shape | 32 `db16cyc`, compares `[[device+11024]]` with `[r3+8]`, calls the "GPU is hung" handler `sub_82628988` after the timeout; GPU-hung flag bit 0x02 of `[device+11069]`; callers: Swap, `sub_82622B48`, `sub_82622BE0`, `sub_82623150`, `sub_82634310`, `sub_826346C8` |

Device fields read by the draw-time shader load (`sub_8262B248`, device in r31): bound **vertex shader at +13072**
and **pixel shader at +13068**, the same as SA2 (the first six device loads, `13072, 12240, 13068, 10372, 10580,
13904`, are the same in both games' generated code), so `SetDeviceShaderOffsets(13072, 13068)` as in sa2-nx.

## Stage 4: native renderer on PC (2026-10-05)

`PEGGLE2_NATIVE_RENDERER=ON` builds `app/src/me` (masseffect-nx's PM4-ring Vulkan renderer through banjo-nx and
sa2-nx); `masseffect_renderer_native = true` selects it. The hooks are in `app/src/me/peggle2_me_hooks.cpp`:
draw-time shader load `sub_8262B248` (device +13072 / +13068), BeginVertices `sub_826221A0` (also reached from
DrawVerticesUP), DrawIndexedVertices `sub_826226A8`, Create*Shader dumps (`peggle2_dump_shaders`), the GPU wait.

**Shader library loop.** The game creates only 2 vertex and 4 pixel shaders (`peggle2_dump_shaders` through the
whole first level: the engine builds them at boot in `sub_8248D7C0`, `sub_824AEBA0`, `sub_824D2200`, plus two D3DX
callers). The first native run drew menus but the playfield had 16-row bands: a frame trace (`banjo_trace_frame_s`)
showed the first 15 draws of each frame skipped with `VS n-1` (no vertex shader identified). Discovery
(`MASSEFFECT_SHADER_DISCOVERY` / `_MISSING` / `_VERTEX_VARIANTS`) found what the ring loads that no container covers:

| Microcode | Loaded from | What it is | Library entry |
|---|---|---|---|
| VS 45 words | `0x1EC00000` | the game's `vs_6b6d1eda` rewritten by D3D for a vertex declaration: only 9 fetch words differ | `tools/make_vertex_variants.py` (real container + loaded microcode) and `repair_vertex_variant_declarations`: "ACCEPT: unique bijection" |
| VS 24 words | `0x1E4CB000` | the game's `vs_3caebedd` rewritten: 5 fetch words differ | same |
| VS 15, 24, 27 words; PS 9 words | immediate (`IM_LOAD_IMMEDIATE`) | Direct3D's own shaders (clear/resolve): 12-16 of the words differ from any game shader | `tools/wrap_discovered.py` (register-linked wrappers), only these four |

The rebuilt library (12 shaders: 6 dumped, 2 repaired variants, 4 wrapped) leaves **0 unidentified loads** in every
run since (title, menus, gameplay, a 15-minute soak: "unidentified loads: VS by address 0, PS by address 0,
immediate 0"). All draws are inside indirect buffers and none pairs with a Draw* record (the pairing is only a hint;
the ring identifies each IM_LOAD by its microcode). The "PS identity mismatch" lines are pairing candidates the
renderer rejects in favour of the ring's own loaded shader.

**Native against emulated.** Recordings aligned by game frame (`peggle2_video_from_frame`, a `.frames` file per
recording, `tools/compare_raw.py`) with the same input script: on matched frames the only differences are game state
(Peggle picks the orange pegs at random for every attempt; animation and fades run on wall-clock time, so two runs
drift), not rendering. Two native runs differ from each other as much (base vs base: 346 of 1200 frames identical, mean
difference 6.5/255), and static screens come out pixel-identical. Not covered yet: the other masters' powers,
Extreme Fever/level-end, trials, duels, Bink movies (none seen on this route).

## Stage 5: optimizations (measured on the PC only)

Other programs ran on this PC during these measurements, so CPU and frame times are noisy; each change was checked by
its own counters, and visually against the run-to-run noise above.

| Change | Before | After | Check |
|---|---|---|---|
| GPU wait hook (`masseffect_wait_blocking_ring`, now on by default; `sub_8261D378`) | the game polls it ~5,000,000 times a second: a whole core | ~200 checks a second, the thread sleeps until the ring writes the read pointer back (or 2 ms) | same 30 fps (~300 Swaps per 10 s); base vs hooked recording: 290 of 886 frames identical, mean difference 6.2 (base vs base: 6.5) |
| Event-driven `WAIT_REG_MEM` (`masseffect_native_wait_reg_mem_us` = 1000) | the ring thread waits ~29 ms of every frame for the game's frame-pacing word (`0x1F4A1004` == 0), polling every 50 us (a yield loop on Windows, 20,000 wake-ups a second on the Switch): 89 % of a core | rechecked after every interrupt, every new write pointer and every Swap, at the latest each 1 ms: ~500 rechecks a second | same 30 fps; process CPU over the same 110 s: 95 s -> 65 s |
| Registers as C++ locals (`app/perf_overrides.toml`: cr, xer, ctr, r14-r31) | process CPU over the same 120 s: ~65-76 s | ~46 s (noisy: other programs ran) | first try crashed 4.6 s after start (below); with the fix 2 x 120-150 s of play, 300 Swaps per 10 s, no error |

**The register-locals crash.** `tools/read_before_write.py --noreturn=825F7C30,825F2630` (longjmp and
`_CxxThrowException`; without the second, 92 false alarms after throw calls) lists 34 functions. 33 are harmless
(idioms, a CR save, and ordinary functions whose reads sit on infeasible paths, `app/perf_overrides.toml`). The 34th,
`sub_8231CEE4`, is the tail of `sub_8231C6D0`, the game's `__try { RaiseException }` wrapper used for SetThreadName,
entered from the parent with three conditional branches; the first classification only looked for `b`/`bl` and
called it an SEH filter. With r31 a local it computed the parent's frame as 0 and its handler `sub_8231CF38` read
guest 0x60. Marked `share_registers`; no other piece is entered from another function.

## The Switch build (2026-10-05)

`tools/win/build_switch.ps1` (devkitA64 GCC 16 through `tools/switch/cmake/switch-devkitA64.cmake`, libnx, static NVK
from `C:/devkitPro/mesa-switch-main-nfsmw-source/.../portlibs/switch`) builds `out/nx/peggle2.nro` (60 MB; 58.8 MB
of code). Two fixes were needed: `DEVKITPRO` is forced to `C:/devkitPro` (the installer's `/opt/devkitpro` breaks
CMake on Windows) and FFmpeg's `config.h` got the `__SWITCH__` branch nfsmw-nx has (`config_switch_aarch64.h`).
On the Switch the native renderer is the default (`peggle2_app.h`). `tools/win/package_switch.ps1 -WithGameData`
assembles the SD card folder in `out/dist/switch/peggle2-nx/`.

The Vulkan driver actually linked, checked 2026-10-06 after banjo-nx found it in theirs: every NRO until then
contained the generic portlibs driver, Mesa 26.2.1 (git-012c67677c), not the nfsmw NVK the build points at (Mesa
26.2.2, git-babaf52908). `sdk/cmake/rexglue_switch.cmake` linked it as `-lvulkan`, and the toolchain searches
`C:/devkitPro/portlibs/switch/lib` first. The console logs agree ("this NVK lacks patch_nvk_set4", "lacks the
nvk_switch_draw contract"). It is now linked by full path; `strings -n 8 out/nx/peggle2 | grep "Mesa 26"` gives
26.2.2. The renderer probes those driver features, so the switch changes its behaviour on the console: the 01:36
heap diagnostic in `out/dist` still has the old driver (same as the two crash logs it follows up).

Compiler flags of the generated code, checked in `build.ninja` on 2026-10-06 (banjo-nx found that the SDK's
directory-scoped `-ffp-contract=off -fno-strict-aliasing` does not reach code built in other directories): on the
Switch the generated code (`peggle2_recomp.dir`), the app (`peggle2.dir`) and the SDK all had both, so the console
heap crash is not a strict-aliasing or FMA effect of missing flags. On the PC the SDK had them but the generated code
and the app had neither (the loop in `app/CMakeLists.txt` was GCC-only); it now covers Clang too.

Guest loads and stores are `volatile` in the generated code (`REX_LOAD_*`/`REX_STORE_*` in `peggle2_pch.h`), so GCC
cannot hoist a polling load out of a loop (the Mushihimesama port's console hang came from a no-volatile pass; we have
none). `tools/find_self_loops.py out/nx/peggle2` (from futari-nx) counts branch-to-self instructions in the Switch
ELF: 18, 3 of them in recompiled functions. One is the game's own `b .` (`sub_8242E838`, guest `0x8242E850`); the
other two (`sub_82330028+0xF8`, `sub_8251D6A8+0x1924`) are paths of guest loops whose exit test only uses registers
that the path does not change (the null case of a list walk): they never end on the Xbox 360 either. This count is
the baseline to compare with if a no-volatile pass is ever added.

**First console run (the 23:05 build, without register locals; the user's logs, 2026-10-05).** NVK came up on the
Tegra X1 (Mesa 26.2.1), the 12-shader library loaded, `game_root` mounted from the SD card, `main.rsb` was being read,
then 4 s in the main thread faulted in the SDK's `RtlEnterCriticalSection` with a null critical section
(`rex_crash.log`, symbolized with `aarch64-none-elf-addr2line`): `xstart` -> `sub_822A8ED0` -> `sub_82195DD0` ->
`sub_82145670` -> `sub_822E8E60` (ref-counted string assignment) -> `sub_822E9420` (release of the old string). The
field released there, +96 of the singleton `**0x82D8D79C`, holds the empty-string sentinel `0x828F6340` on the PC
(probe in `app/src/peggle2_debug_hooks.cpp`); on the Switch it held a block whose lock pointer was 0. **Solved on
2026-10-13: the game's network start-up failed on the Switch (no socket service) and the game freed the object
itself; see "The console boot crash: cause and fix" at the end of this document.** What the investigation ruled
out before that, and what the console build of 2026-10-06 changed:

- Ruled out: memory barriers (`lwsync`/`sync`/`eieio` are real fences in the generated code), the SDK's critical
  sections (`__sync` builtins, full barriers on AArch64), `operator new` returning NULL (MSVC's throws `bad_alloc`, and
  the throw logger saw none), and recycled guest-heap memory as such: with `scribble_heap = true` (every new heap
  allocation filled with 0xCD) the PC still runs and the field holds the sentinel.
- `sub_822C5C08` (first call of `sub_82145670`) creates the singleton on the main thread: `new(36)` + `sub_822CAF38`,
  whose field 0 is a 392-byte object built by `sub_822E1B50`, which stores the sentinel at +96 itself. So between that
  constructor and the release a few instructions later, something on the Switch wrote into the object, or the
  singleton pointer at `0x82D8D79C` (zero-initialized data) was not zero when the check ran.
- Not the cause after all: `sdk/src/core/memory_switch.cpp` now zeroes released ranges (`RexGmZero`, through each
  chunk's always-writable alias) because on the Switch a release only removes access and the backing keeps its bytes.
  But the guest heap never goes through that path: `BaseHeap::Decommit`/`Release` (xmemory.cpp) only change the page
  table and protection on every platform ("mapped memory cannot be decommitted"), so the PC keeps old bytes on
  recommit too. Harmless, kept; it only acts in `BaseHeap::Dispose`.
- Current candidate: the singleton setup `sub_822C5C08` is not thread-safe (check `0x82D8D79C`, `new` + construct,
  store the pointer, no barrier) and is reached from two places through pointers (`sub_82140460`, `sub_82145670`).
  On x86 another thread always sees the constructor's stores before the pointer; on the Switch's ARM cores the
  pointer can become visible first and a reader finds field +96 unwritten. The hooks on the two constructors
  (`sub_822CAF38`, `sub_822E1B50`) end with a full fence, so the object is visible before any pointer to it is
  stored (the reader's loads through the pointer are address-dependent, which ARM keeps in order). On the PC all
  three probes log from the main thread.
- Probes (`app/src/peggle2_debug_hooks.cpp`): field +96 right after `sub_822E1B50`, the singleton and inner object
  after `sub_822C5C08`, and a release of a block with no lock is logged with the memory around it, the log flushed and
  the release skipped. On the PC: inner object `0x414B9AF0`, singleton `0x4155F990`, field = `0x828F6340`.

**Second console run (the 00:55 build with the fences and probes; the user's logs, 2026-10-06).** Same addresses as on
the PC (inner object `0x414B9AF0` built with the sentinel, singleton `0x4155F990`), so the guest heap had the same
state up to there; the fences changed nothing. Right after `sub_822C5C08` returned (same thread, same millisecond)
field +96 held `0x414B7470`, the release was skipped, and 1 s later the main thread read guest `0x00750064` through
the singleton, which by then held the UTF-16 text "Xbox 360 audio device": XAudio2's device name, copied from
`0x820026BC` into a 44-byte `XMemAlloc` buffer by `0x8232FB00`-`0x8232FBA0`. The words dumped around field +96 show
why: `00090005 03011000` at `0x414B9B30` is a heap entry header (16-byte entries: size 9 units, previous size 5, busy,
16 unused bytes) of a 128-byte block whose data starts at `0x414B9B40`, inside the live 392-byte object (26 units from
`0x414B9AE0`). So on the Switch the heap treated the object's block as free and split it again (5 + 9 units), and gave
the singleton's 36 bytes to XAudio2 (44 bytes, the same 4-unit size class).

- The heap is the game's own NT heap (xapilib): `malloc` (`sub_825EEA68`) and `XMemAlloc`'s heap path
  (`sub_823198C0` -> `sub_8231D030`) both call RtlAllocateHeap `sub_8231BAE0(heap, flags, size)` on the process heap
  `*0x82D92F04` (= `0x40000000`); RtlFreeHeap is `sub_8231C3D8`, RtlReAllocateHeap `sub_8231C6D0`, RtlSizeHeap
  `sub_8231AD58`. Entries have a 16-byte header (u16 size and previous size in 16-byte units, flags, unused bytes, then
  the free-list links); the lock is `RtlEnterCriticalSection(*(heap + 1408))` unless HEAP_NO_SERIALIZE (bit 0 of the
  flags at heap + 24); the free paths release it in SEH `__finally` blocks (`0x8231C398`, `0x8231C68C`, `0x8231CF44`).
- PC baseline from the tracker below (30-40 s boots, three runs): no overlap, no free of a block that is not live, no
  lock anomaly; around the object everything happens on the main thread (`F8000028`): `0x414B9A60` +0x80 from
  `0x822FD26C`, then the object `0x414B9AF0` +0x188 from `0x822CAF88` (header `001A0009`), `0x414B9C90` +0x1C from
  `0x822CAFFC`; the singleton `0x4155F990` +0x24 from `0x822C5C34` (header `00040004`).
- Race checked on the PC (the Mushihimesama port's Switch-only bug was a thread started running that read a member
  before its creator set it). The singleton's constructor starts two threads exactly that way, in `sub_822ECF80`:
  routine `0x822FC418` and `0x822F8D18` (the latter loops on its object every 10 ms while the creator goes on
  configuring it), and the setup's caller then starts a 10 ms ticker (`sub_8230A968`, routine `0x8230A740`). Neither
  order reproduces anything on the PC: the creator sleeping 200 ms after each start (`peggle2_debug_race_delay_ms`)
  or the two threads waiting 300 ms / 2 s before their first instruction (`peggle2_debug_thread_start_delay_ms`) left
  field +96 at the sentinel and the tracker silent. Thread creation is logged (`[thread]` lines) from the next build.
- Other Switch-only paths ruled out on the PC: XMA decoding on the decoder thread (`audio_xma_in_worker = 1`, the
  Switch default; the PC decodes on the kicking thread): 1,242 contexts decoded by the worker, no overlap, field +96
  intact; mode 2 the same. The Switch file read path (`host_path_file.cpp`: the read window goes to a host buffer
  and exactly the request is copied into guest memory; direct reads in chunks never pass the request) cannot write
  past a guest buffer.
- Diagnostic build (01:33): `app/src/peggle2_debug_hooks.cpp` hooks the three heap functions and keeps every live
  block on the host side. It reports an allocation overlapping a live block (both owners, callers, threads), a free of
  a block that is not live, and a heap call entered while the heap's critical section already names the calling
  thread although it is outside the heap (a leaked lock or two threads sharing a guest identity), and it logs every
  allocation and free touching the two objects with the caller of `operator new`/`malloc`/`free`/`XMemAlloc`/`XMemFree`
  and the guest stack. The first two tell a heap that broke (a lock or emulation problem) from memory that the game
  itself freed while still using it.

## Asset cache (the pack), 2026-10-06

Ported unchanged from sa2-nx at the user's request (relayed by the SA2 session), so all the ports share it: the SDK's
`PackDevice` (`sdk/include/rex/filesystem/devices/pack_device.h`, `pack_device.cpp`, `pack_storage_switch.cpp`; Fruit
Ninja NX's format version 2), `asset_pack` / `asset_pack_build` in `sdk/src/system/runtime.cpp`, the data root that
exists only as a pack in `sdk/src/ui/rex_app.cpp` and `app/src/peggle2_app.h`, and `tools/build_asset_pack.py` +
`tools/fnv64.c`. Peggle 2 gains little from it: the folder is 34 files and the game reads almost everything from one
918 MB `main.rsb`, so the console already opened few files. Measured on the PC (the console sees the pack the same
way):

- `python tools/build_asset_pack.py assets/game_root --verify`: 34 files, 879 MB, built and reread in 5 s.
- Tested the way the console sees it: no `game_root` folder next to the executable, only `out/pc/game_root.nxpack`
  and `.nxidx` (hard links to the ones in `assets/`). The pack mounted in 0.6-0.9 ms ("34 files, 0 directories,
  879.5 MB"), the same three lookups failed as with the folder (`properties\partner.xml`, its locale variant,
  `subtitles\wordmark_bjorn_sound.json`: not in the package), and the recording reached the title screen and, after
  A, the adventure map.
- The Switch build compiles with it (`out/nx/peggle2.nro` 01:45). Not run on the console yet: the NRO in
  `out/dist` is still the 01:36 heap diagnostic, without the pack, so that test changes one thing at a time.
- Fixed before it ever ran here (SA2's console find, relayed by the Mushihimesama session; files taken from sa2-nx
  again): Horizon's `fsFileRead` returns short reads for some large reads, and the first version took that for a
  failure (SA2: a 16.5 MB file came back as 0 bytes and the game re-read it forever). Peggle 2 reads `main.rsb` in
  pieces of up to 4.7 MB, so it would have hit it. Now both storages continue from where a read stopped and fail only
  on an error or 0 bytes; on the Switch a read that the file system refuses straight into guest memory is retried
  through a 1 MB host buffer; the builder refuses packs of 4 GB or more (FAT32). PC: boots from the pack alone as before.

## Enhancements and their switches (2026-10-06)

The user's rule for every ReXGlue Switch port (relayed by the Mushihimesama session with its report on the
UnleashedRecomp-NX / MarathonRecomp-NX optimisations, `D:\mushihemesama\futari-nx\docs\research\`): every
enhancement has its own key, belongs to the CPU or the non-CPU group, and runs only when its key and its group's
master are both on; one log line says what ran; build-time ones go into the single optimised build (no second NRO).
`app/src/peggle2_enhancements.cpp` does it: `peggle2_cpu_enhancements` and `peggle2_noncpu_enhancements` (default
on) write their members' "off" value at startup, before anything reads them, and the `[enhancements]` line lists
everything. Checked on the PC: with both masters on all are on; with both off all four keys were turned off ("group
master off"), the PC still booted, and the pack was used anyway because the folder is absent (the SDK falls back to
the pack and warns, so a master can never leave a pack-only card without game data).

| Enhancement | Group | Key (default) | What it does | Measured |
|---|---|---|---|---|
| gpu_wait_sleep | CPU | `masseffect_wait_blocking_ring` (true) | the game's GPU busy-wait `sub_8261D378` sleeps until the ring moves (native renderer only) | PC: 1,100-3,100 checks per 10 s instead of ~20 million a second |
| wait_reg_mem_events | CPU | `masseffect_native_wait_reg_mem_us` (1000; 0 = off) | a WAIT_REG_MEM the ring cannot pass parks until an interrupt, write pointer or swap, at most 1 ms | PC: part of the 0.59-core gameplay figure (stage 5) |
| vs_relaxed_match | non-CPU | `masseffect_native_vs_relaxed_match` (true) | vertex shaders whose fetch swizzles D3D patched are matched to the library | PC: identified both repaired variants when they were left out |
| asset_pack | non-CPU | `asset_pack` (true) | game data from `game_root.nxpack` (see above) | PC: mounts in under 1 ms |
| ring profile: `ring_deferred_recording`, `ring_deferred_fast`, `ring_deferred_update`, `ring_flat_identity`, `ring_fast_pair`, `ring_raw_microcode`, `ring_pm4_fast`, `ring_notify_no_lock`, `ring_compare_fast`, `ring_fast_copy`, `ring_fingerprint_xxh3`, `ring_dedupe_large`, `ring_indices_cache`, `ring_pipelines_prewarm` (GPU marks: now a profiler key, see below) | CPU | the renderer's `masseffect_native_*` / `masseffect_*` keys (off in code; Peggle 2 turns them on unless the toml sets them, plus `masseffect_native_deferred_recording_naps = 8`) | masseffect-nx's shipped ring-thread profile (its console runs: ring 28.0 -> 25.7 ms) | PC, native renderer, same scripted input, frame-aligned recordings: game frames 1000-1399 (menus, map) pixel-identical to the baseline (0 of 400 frames differ, as baseline vs baseline); the title screen's time-based animation ran ~6 frames out of phase (baseline vs baseline: in phase), which the comparison cannot separate from rendering; the 32 shader identity audit warnings are the same with and without |
| registers as locals | build | `tools/codegen.ps1` (always on in delivered builds; `-Classic` local diagnostic only) | CR/CTR/XER and the non-volatile registers as C++ locals (`app/perf_overrides.toml`) | not measured on the console |

Defaults by source, not by value (fixed 2026-10-06 after the Mushihimesama port's console run, where an old toml
without the newer keys left 21 enhancements off): a key nothing set (no toml line, environment variable or
command-line flag: `rex::cvar::GetFlagSource(key) == Source::kDefault`) gets Peggle 2's on value, and the
`[enhancements]` line counts how many did. The first version asked `HasNonDefaultValue`, which compares values: a toml
line setting a key to its code default (`masseffect_native_flat_identity = false`) was overridden back to on, and so
was `masseffect_renderer_native = false` on the Switch. Checked on the PC: with the environment setting
`masseffect_native_flat_identity=false`, 18 of 19 keys were defaulted and that one stayed off. The Mushihimesama port
saw `masseffect_native_deferred_recording` together with `masseffect_native_output_no_wait` stall ~100 ms per copy on
its PC (23 fps); Peggle 2's PC runs with both on recorded at 29.7 fps with no output waits. Deferred recording is
explicit-only since 2026-10-07: futari-nx's lockstep captures found it not pixel-identical (see "The direct renderer"
below).

**The profiler is on in every config but the verify one** (the user's request, relayed by the SA2 session,
2026-10-06): two plain keys outside the enhancement table, so no master switch (e.g. `cpu_off`) can turn profiling off
with it. `switch_profile_stacks` (default true, defined in `peggle2_enhancements.cpp`, read by
`sdk/src/ui/switch_perf.cpp`): the console CPU sampler, 1 ms stack samples into `logs/rex/rex_profile.log`.
`masseffect_gpu_marks_categories` (the renderer's, default true; it was the `gpu_marks_trimmed` enhancement before):
GPU time per pass category and the idle gap in the "GPU per Swap" lines; the image does not change. The generator
writes both explicitly into the default `peggle2.toml` and every premade config (on) and the verify config (off). The
measurements that cost GPU time or extra work stay off (`masseffect_native_nvk_measure_failures`,
`masseffect_native_count_changes_pipeline`); the cheap default ones stay as they are (per-draw NVK timing 1 in 64,
precise marks). The flag file `logs/rex/stacks_profile.flag` is no longer packaged (it would force sampling on even in
the verify config), and `package_switch.ps1` deletes old configs and an old flag before copying the new ones.

**Verify configuration** (the user's requirement relayed by the Mushihimesama session; futari-nx's implementation):
`peggle2_cpu_verify = true` makes every self-check of the renderer's fast paths compare on every use instead of the
first `masseffect_native_verify_n` (2048) / `masseffect_native_fast_copy_verify` (256), whose ranges were raised to
2147483647 for that; 23 of the renderer's 24 DIFFERENCE / MISMATCH reports count into `me::native::g_verify_differences`
(not "C6 prewarm: MISMATCH", a timing guess about the prewarm thread that changes no output; banjo-nx's correction)
(`app/src/me/me_verify.h`), and the renderer's 10 s report adds `[verify] checks done: ... | differences: D | VS identity
audit mismatches M` (both must be 0; the VS audit compares every ALU/CF word of a vertex shader with its library entry,
which covers `vs_relaxed_match`). PC, native renderer, 130 s of menus and play with the scripted pad, every CPU
enhancement on: identity memo 605,278 checks, record pairing 79,685, PM4 runs 154,207, object table 29,147, raw
microcode 0 (Peggle 2 never reloads a shader: that path did not run) | **differences: 0**, VS audit mismatches 0.
Again after the PC build got `-fno-strict-aliasing -ffp-contract=off` and the fast vertex copy was made to check one
snapshot of the guest words: identity memo 600,140, pairing 78,984, PM4 runs 152,830, object table 28,918 |
differences: 0. The
renderer's separate PS identity audit counted 172 mismatching candidate checks, with or without the enhancements
(Futari's runs show 0, so it is Peggle-specific). It is not a fast path and not a wrong draw: `AcceptPixelShaderIdentity`
rejects a candidate whose microcode differs from what the ring loaded, and the next identification path (by code, by
library) is used instead. The logged cases are candidates found by address (`identify-address`,
`pair-address-object`) whose entry belongs to another shader, e.g. a 9-word PS loaded where the address map still
named the 15-word n8: Peggle 2 reuses shader object addresses. The cost is the rejected compare. `gpu_wait_sleep` and
`wait_reg_mem_events` only delay: the game's own check, or the ring's WAIT_REG_MEM compare, decides afterwards with
the same values, so they cannot change a result.

Premade configs: `python tools/make_enhancement_configs.py` writes `app/switch/peggle2.toml` (the default, every
enhancement on) and `app/configs/peggle2_{cpu_on,cpu_off,cpu_verify,profile}.toml` from the table;
`package_switch.ps1` copies them to the SD folder's `configs/` (copy one over `peggle2.toml` to use it). All of them,
the default included, run the profiler except `peggle2_cpu_verify.toml` (above).

Console profiler (from futari-nx): with `switch_profile_stacks` on (or a `logs/rex/stacks_profile.flag`), the SDK sampler
(`sdk/src/ui/switch_perf.cpp`) now writes every sampled pc of a busy thread with its count into
`logs/rex/rex_profile.log`; the Switch link writes `out/nx/peggle2.map`, and `python tools/switch_hot_functions.py
rex_profile.log --map out/nx/peggle2.map` adds the samples up per function (self time), labelled default (the
recompiled game), renderer, app or library, overall and per thread. The map must come from the build that made the
NRO.

PC sampling profiler (from futari-nx; `peggle2_pc_profile_s = N` writes `logs/pc_profile.log`, `python
tools/pc_hot_functions.py out/pc/logs/pc_profile.log` adds the windows up). First profile, 2026-10-06: native
renderer, every enhancement on, 120 s of menus and play with the scripted pad, the heap tracker still compiled in.
- Threads with work: the main thread (game code: `sub_823F5DF0`, `sub_82151898`, `sub_823F8380`, ... each 2-4 %),
  Wwise's `AK::EventManager` (`sub_825BE6A0` 8.6 %, `sub_8251A320` 5.3 %), Bink's `BinkAsy3`, the ring thread.
- The ring thread is mostly waiting (63 % of its samples in waits); its busiest code is XXH3 fingerprinting (9.6 % of
  its samples) and memmove (3.2 %). None of the Futari ring findings (vertex repack read back from the upload buffer,
  `ProveRectangleList` copying the register file, per-draw `fmt::format` in diagnostics) shows up, and the renderer
  reports 0 rectangle lists proven or rejected: not changed without a console profile that shows them.
- `ucrtbase!fmal` is 3.9 % of the main thread and 3.3 % of Wwise's: the generated code computes `fmadd` and friends
  with `std::fma` for exactness, and x86-64-v2 has no FMA instruction, so the C runtime emulates it. PC only (the
  Switch's A57 has FMA in hardware).
- `heaptrack::NoteAlloc` (the diagnostic) is 1.6 % of the main thread: it goes when the heap diagnosis is done.

**Only the optimised build is delivered** (the user's decision, relayed by the SA2 session, 2026-10-06): build-time
changes go into the optimised codegen, and features are compared through the runtime keys and the premade configs,
not through a second NRO. `tools\codegen.ps1 -Classic` (the same code without `app/perf_overrides.toml`; the line then
says `build: codegen classic`) stays as a local diagnostic only; `tools\codegen.ps1` goes back. `-DirectCalls` (item
B3 of the report) adds `+direct-calls` to the profile.
On the PC the build itself re-runs codegen with `peggle2_manifest.toml` when an input in `generated/default/codegen.d`
changes (the Mushihimesama session's pitfall: it overwrites the generated files and drops anything done to them):
`-Classic` points `codegen.d` back at the real manifest, and a codegen re-run by the build writes `optimised` into the
marker (`app/CMakeLists.txt`), so the `[enhancements]` line never calls a regenerated tree classic. The Switch cross
build has no codegen step. Post-codegen passes (direct calls, no-volatile) must be appended to that same build step
when they are added.
Nothing in this table has been measured on the console yet: the game does not get past the heap crash there.
Planned (reply sent to the Mushihimesama session): after the crash, the console profiler, direct calls with LTO and
`-O2 -fipa-pta`, the handheld GPU boost, Mesa disk cache / pipeline cache, CRT natives with code-hash checks and a
verify mode, the ring thread's own cost; kernel sync, TLS, FPCR and relaxed `stwcx.` last, gated by an audio-heavy
console test. Each one joins the table with its key.

## Native replacements of hot guest functions (2026-10-06, PC)

sa2-nx's method (relayed by the SA2 session; the user asked for the work on the PC even though the PC profile is
flat): `app/src/cpu/peggle2_hot_guest.cpp` hooks a guest function with a native version that has its own key
(`peggle2_cpu_native_*`, CPU group, plus `peggle2_cpu_natives` for all of them), engages only when the loaded guest
code has the FNV-1a hash it was written from, and counts its calls (`[hot]` line every 10 s).

- Profile: 3 minutes of gameplay with `out/route2_script.txt` (menus, the first level with aiming, shots, a lost level,
  a retry; the plain A-only script of the README stalls at the aiming tutorial). Busy samples of the main thread are
  flat (top function 4.1 %, then 3.6, 3.3, 3.3, 3.1 ...; `ucrtbase!fmal` 4.9 % is the PC's software FMA); Wwise's
  `AK::EventManager` thread has one dominant mixer, `sub_825BE6A0` (17.7 % of its busy samples).
- C runtime: memcpy (`sub_825F0AE0`, 1160 bytes) and memset (`sub_825F10B0`, 160 bytes) are byte for byte SA2's
  routines (same FNV), so sa2-nx's natives apply unchanged (`src/cpu/hot/n_825F0AE0.h`, `n_825F10B0.h`). SA2's
  memmove has no identical routine here.
- Batch 1: 32 localized copies (`src/cpu/hot/localized.txt`: the 20 hottest main-thread functions and 12 of Wwise's
  thread), generated into `app/generated/hot/` by `tools/localize_native.py` (from sa2-nx), which also writes their hook
  lines (`localized_hooks.inc`, with each function's size and code hash) and is re-run by `tools/codegen.ps1`, by the
  build's own codegen step and at configure time. Additions to SA2's tool: a loop barrier at every loop header (the
  copies read guest memory without volatile); kernel imports treated as calls; and for the 13 leaf functions a second
  variant, `NativeLogged`, whose guest stores record the bytes they overwrite, so the guard needs no hand-written list
  of written ranges: it rolls the logged stores back, runs the recompiled original and compares every register and
  every byte written. The 19 functions that call others run unguarded (exact by construction).
- Verified on the PC: the default guard (first 256 calls of each thread, then 1 in 4096) made 37,047 checks with 0
  differences in 160 s of gameplay; **verify mode (`peggle2_cpu_verify`, every call checked): 90,658,372 checks, 0
  differences** in 200 s of gameplay; the game plays normally with all 34 on. The guard compares only the guest bits of
  the FPU control register (`FPSCRRegister::GuestMask`; sasrt-nx found the sticky MXCSR flags gave false differences).
- End-to-end check of the unguarded copies: not possible by recordings. The same scripted gameplay with the natives
  off and on, paired by game frame, differs in every frame (median 4.7 % of pixels, up to 98 %), but so do two runs
  with the natives off (median 4.9 %, worst identical): Peggle 2's gameplay is not frame-deterministic on the PC (the
  input is frame-locked; the game state is not). The unguarded copies stay "exact by construction" (the transformation
  the 13 guarded leaves passed 90.7 M checks with), and the game plays normally.
- Registers: MSVC's interprocedural allocation means a native must not change a register the original leaves alone.
  The CRT natives write none; `tools/call_site_liveness.py` (from sa2-nx) showed 4 callers of memcpy "reading" r7-r9
  after the call, all idioms that do not depend on the value (`subfe rD,rA,rA` from the carry, `mr r8,r8` no-op); the
  tool now knows them, and memcpy's 1,083 and memset's 665 call sites read no volatile register afterwards.
- **Effect (PC, gameplay, CPU time of the thread over the same 2 minutes of the route, natives off -> on): main thread
  43.8 -> 34.7 s (-21 %), Wwise's audio thread 30.2 -> 23.6 s (-22 %).** Other projects ran on the machine and the
  two gameplay runs are not identical, so these are approximate. Not measured on the console (the game does not run
  there yet).
- Switch: builds (NRO 54.0 MB); `tools/find_self_loops.py` finds the same 3 branch-to-self loops of the game's own code
  as before, none from the copies (their loop barriers work).

## Shader coverage on the console

- `peggle2_shader_misses` (on by default on the Switch: `shader_misses/` next to the NRO) points the renderer's
  discovery dumps (`MASSEFFECT_SHADER_DISCOVERY` / `_MISSING`, which the Switch cannot set) at the SD card, so a
  shader the library lacks in a mode not played on the PC is kept and can be added without rebuilding the NRO.
- Relaxed vertex-shader identification (from the Mushihimesama Futari port, `masseffect_native_shaders.cpp`): after no
  exact match, a library VS is accepted when every difference is a fetch whose patched destination swizzle the draw
  path can remap (`me_shader_identity.h`), the closest candidate first. Checked by building the library without the
  two repaired variants: both were identified this way ("VS n4 identified with destination swizzles patched by D3D
  (45 words, 6 components remapped)", n2 with 1), 0 unidentified loads, gameplay drawn in full. The full library keeps
  the repaired variants (exact matches win).

## Stage 5 summary (PC, 10-minute native soak with the scripted pad, 2026-10-06)

All optimizations together: 300-301 Swaps per 10 s, 0 unidentified loads, no error, **0.59 cores** of CPU on average
(351.7 s in 600 s), against ~1.4 cores for the emulated GPU and ~2.4 for the first native runs. Not measured on the
console: GPU time per frame, CPU per core, the 460.8 MHz handheld mode's effect.

## The direct renderer and the ring's emulation (2026-10-07, PC)

The user's orders (relayed by the SA2 session): remove the CPU-side emulation from the ring renderer, PM4 first, on
sa2-nx's direct renderer (`D:\sonic adventure 2\sa2-nx\docs\direct-renderer.md`); aim for 8-10 ms of ring thread per
frame at 1020 MHz; pixel-identical, proven by verify (0 differences) and lockstep captures; deferred recording on; then
steps 2/3. Nothing here is measured on the console: the game still stops at the heap crash there.

### Step 1: the draw state without PM4 (`peggle2_cpu_direct_state`, on)

Ported from sa2-nx (`app/src/me/me_native_system.cpp`, namespace `direct`; hooks in `app/src/me/peggle2_me_hooks.cpp`).
Around both draw functions, BeginDrawDirect / EndDrawDirect turn the state packets Direct3D wrote into a host record and
cover them with one NOP marker (`'PG2D'` + record number); the ring applies the record at the marker with the same
register functions. The device fields are SA2's (Peggle 2's Direct3D is SA2's), checked in Peggle 2's code:

- `D3DDevice_DrawIndexedVertices` (`sub_826226A8`): `lwz r3,48(r31)`, `lwz r11,56(r31)`, `cmplw`, `bl 0x82624590` (makes
  space, returns +48), then `stwu ...,4(r3)`: +48 is the last word written, +56 the soft limit. The draw packet
  `0xC0032201` is a predicated DRAW_INDX: it stays as PM4 (the converter stops at the draw packet before it checks the
  predicate bit).
- `sub_826233C8` (the vertex space allocator) pulls the limits in to force a submission: `stw +48+160 -> +52`,
  `stw +48 -> +56`. A draw during which they change keeps its packets (`segment` in the report).
- `D3DDevice_BeginVertices` (`sub_826221A0`) does not move +48: it keeps the last word it wrote in **device+13844**,
  and `EndVertices` (`sub_82622190`: `lwz r11,13844(r3)`, `stw r11,48(r3)`) publishes it. Its hook reads the end there
  (`DrawDirect::end_field`); r3 = 0 (no vertex space) converts nothing. Futari's XDK has the same design at +13652.
  `DrawVerticesUP` calls BeginVertices, so it is covered. The vertices themselves are in a separate allocation (fetch
  constant 0x48BA run), not inline.
- Replays: "tile replays reusing a pairing 0" in all 713 report lines of the PC logs: no predicated tiling to handle.
- Checks: the first 4,096 records (all in verify mode) are re-encoded and compared with the packets when made, as in
  SA2. New here: a record also keeps the three words its marker overwrites, and in verify mode the ring compares the
  record again with the packets under the marker when it applies it (a packet Direct3D changed after the call would
  show there).

Results, PC, scripted pad (menus, map, level), 130 s: **100 % of draw calls converted** (72,353 of 72,353), 0 lost,
0 left as PM4 for any reason. Verify mode: 37,078 records checked when made, 37,067 when applied, 0 differences. The
ring's time did not change (below): with ~33 draws a frame, parsing their PM4 was never the ring's cost.

### Step 1b and sa2-nx's ring rounds 2-5

The renderer files `masseffect_native_targets.cpp`, `masseffect_native_draws.cpp/.h`, `masseffect_deferred_recording.cpp`,
`me_stencil_clear_ownership.h`, `me_native_draw_extent_estimator.h` and `me_native_ps_no_kill.h` are now sa2-nx's of
2026-10-07, with SA2's keys renamed (`SA2_ENHANCEMENT` -> `PEGGLE2_RING_ENHANCEMENT`, `sa2_cpu_*` -> `peggle2_cpu_*`,
`sa2_native_*` -> `peggle2_native_*`) and Peggle 2's own changes kept: the vertex copy check on one snapshot (banjo-nx),
the prewarm MISMATCH outside the verify count, the profiler's GPU-marks default, `vs_relaxed_match`
(`masseffect_native_shaders.cpp` stays Peggle 2's), the event-driven WAIT_REG_MEM. The backup of the old files is not in
the repository. `me_native_system.cpp` got the same features hunk by hunk (it holds Peggle 2's own ring code).
`PEGGLE2_RING_ENHANCEMENT` registers a key with the CPU master only (`peggle2_enhancements.h`); the `[enhancements]`
line counts them ("ring enhancements 15 of 15 on") and `make_enhancement_configs.py` lists them.

| Key (default) | What | Check |
|---|---|---|
| `peggle2_cpu_ring_edram_memo` (on) | a draw with the previous draw's render-target state reuses its EDRAM preparation when the full path would do no work | 90 % of draws from the memo; every hit checked against the full path in verify mode: 0 differences |
| `peggle2_cpu_ring_compare_uploaded` (on) | constants / shared-block change tests read the uploaded (cached) block instead of a second copy | verify mode keeps the copies and compares |
| `masseffect_native_upload_memory` = 1 (`ring_upload_cached`, on) | the upload buffer in CPU-cached memory, flushed before each submit | SA2 console: 2x the CPU write speed |
| `peggle2_cpu_lazy_trace_text`, `peggle2_cpu_ring_target_memo`, `peggle2_cpu_ring_redirect_key`, `peggle2_cpu_ring_bias_seen`, `peggle2_cpu_ring_entry_hash`, `peggle2_cpu_ring_extent_key`, `peggle2_cpu_ring_estimator_reason`, `peggle2_cpu_ring_rect_list_in_place`, `peggle2_cpu_ring_identity_accept_fast` (on) | sa2-nx round 2-3: per-draw string formatting, map lookups, set inserts and an 80 KB register-file copy replaced (docs/enhancements.md in sa2-nx) | each exact by construction or self-checked (entry hash: recomputed for the first 2,048 uses / all in verify mode) |
| `peggle2_cpu_ring_vs_mismatch_memo` (on) | a (library candidate, loaded VS) pair that failed once is not compared again (keyless in SA2) | verify mode compares again on every hit |
| `peggle2_cpu_note_shader_fast` (on) | NoteShaderObject's per-call mutex and TLS lookup gone (game thread) | exact |
| `peggle2_cpu_hook_counter_shared` (on, under the natives master) | the native hooks count calls in one relaxed counter per hook instead of a thread_local (`__aarch64_read_tp` per hooked call on the Switch) | only which calls the guard samples changes |
| `peggle2_cpu_ring_dedupe_adaptive` (on, Peggle 2's own) | the vertex dedupe probes 32 frames; with no hit it rests 256 frames | exact by construction (a range is copied again instead of reused); see below |
| `peggle2_native_edram4_clear_color0`, `peggle2_native_constants_dedupe`, `peggle2_native_pass_narrow_end` (non-CPU, on) | sa2-nx's renderer GPU optimisations | sa2-nx's checks; PC verify 0 differences here |
| `peggle2_native_zcull_1x_depth` (non-CPU, explicit only) | ZCULL kept on 1x depth views | off in sa2-nx until a 30-minute play test |

Not taken: sa2-nx's round 6 (`direct_pair`, `note_shader_pair`, `identity_memo`): not documented there yet, still being
built. The SDK's vblank and swap counter came along (below). `NvkSwitchDrawContract` (with the files) makes the renderer
find NVK's per-draw contract under nfsmw-nx's name `nvk_switch_dibujo` too: before, the Switch build's weak reference
stayed null and NVK's per-draw paths were never used (console effect not measured).

**Deferred recording stays explicit-only** (against the order, for its pixel-identical rule). futari-nx's lockstep
captures (2026-10-07) found it NOT pixel-identical: with the three deferred keys on, a menu fade differed on 4 frames by
up to 65 levels, differently in each run, while five runs without it agreed; sa2-nx's own lockstep note lists "one fade
frame" for it. Peggle 2's captures cannot clear it (its fades do not reproduce, below). On the PC it moved ~0.4 ms of
ring CPU per Swap to the worker. The 100-160 ms PC frames other ports saw with it are fixed in sa2-nx's file (taken):
the queue's short naps yield on Windows instead of sleeping a timer period.

**The vertex dedupe never hits in Peggle 2**: 0 hits in every 10 s window of a 130 s run (menus, map, level) while
~19 MB per 10 s went through XXH3 for it. With `peggle2_cpu_ring_dedupe_adaptive` it fingerprinted 1.9-2.2 MB per 10 s
(269 of 300 frames rested). The ring's CPU did not move measurably (XXH3 does 19 MB in ~2 ms), so the profile's XXH3
share (14 % of the ring's samples) must come from elsewhere; not found yet.

### The ring timer

`[native] ring thread: X ms of work per Swap (Y us per draw, N draws per Swap), Z ms per Swap waiting for the game
(WAIT_REG_MEM); CPU C ms per Swap (D us per draw)` every 10 s. "Work" is sa2-nx's wall-clock figure (the parse loop less
the WAIT_REG_MEM waits; it includes the waits for the host GPU and the presentation). "CPU" is new: the thread's own CPU
time over the interval (`app/src/me/me_thread_cpu.cpp`: GetThreadTimes on Windows, `svcGetInfo(ThreadTickCount)` on
the Switch), which is what an emulation change moves.

| PC, ~33 draws per Swap | ring work per Swap | ring CPU per Swap |
|---|---|---|
| before (this session's start: masseffect-nx's set, no direct state) | 1.36-1.49 ms (41-45 us per draw) | not measured then |
| direct state on, nothing else new | 1.41-1.73 ms | not measured then |
| all of the above, deferred on | 1.39-1.75 ms | 0.36-0.78 ms (11-23 us per draw) |
| final defaults (deferred off, adaptive dedupe) | 1.31-1.50 ms | 0.93-1.46 ms (29-43 us per draw) |
| CPU master off | 1.5-2.1 ms | ~30 ms: the ring polls WAIT_REG_MEM without `wait_reg_mem_events` |

The PC figures move by +-0.3 ms between 10 s windows; the per-draw cost is dominated by the Vulkan driver and the
per-frame work (Present, copies), not by anything step 1 removed. The 8-10 ms target is a console figure: not
measurable until the console boots the game.

### Lockstep captures for Peggle 2

sa2-nx's `vblank_per_swap` (SDK, `xboxkrnl_video.cpp`) is taken, with `vblank_per_swap_count` (vblanks per swap: 2 for
Peggle 2's 30 fps). It was not enough: Peggle 2's animation runs on its clock. New in the SDK: **`clock_per_swap_hz`**
(`sdk/src/core/clock.cpp`, `Clock::NotifyGuestSwap` from VdSwap): the guest clock (mftb, KeQueryPerformanceCounter,
KeQuerySystemTime all go through `UpdateGuestClock`) advances 1/N s per swap; a thread's k-th read within a frame is the
frame's start + k us (a wait on the clock still ends); `clock_per_swap_timeout_ms` (200) of host time without a swap
advances one frame. The log says `[clock] frame clock on`. And **`tools/compare_hashes.py TEST BASE1 BASE2 ...`**:
futari-nx's criterion (only images identical in every baseline at the same game frame count) with sa2-nx's any-offset
lookup in the test run.

Settings for a capture: `vblank_per_swap = true`, `vblank_per_swap_count = 2`, `clock_per_swap_hz = 30`, the scripted
pad, `boot_test.ps1 -VideoFromFrame 1000 -VideoFrames 400`.

Even so, Peggle 2's runs are only partly reproducible: of game frames 1000-1399, 123 are identical in two CPU-off
baselines. The loading spinner's angle, the unicorn's idle pose and the HUD frame's slide-in follow host time (loading
lengths, probably the game's `NtSetTimerEx` timers), and the later frames shift by 1-6 game frames. Results against
the two CPU-off baselines:

- defaults with deferred on: 121 of the 123 reproducible images present (100 at the same game frame);
- defaults with deferred off: 122 of 123 (103 at the same game frame).

The missing images (game frames 1077-1078) differ only in the loading spinner (x 276-366, y 106-254, at most 34
levels), at the edge of the stretch where the two baselines themselves stop agreeing (1079 on). No fade is among the
reproducible frames, so these captures cannot prove or clear deferred recording. The exactness evidence for the rest is
the verify run: final defaults, 150 s, natives 63,342,156 checks, every renderer self-check (direct-state records made
32,244 / applied 32,216, identity memo 291,407, record pairing 36,703, PM4 runs 9,767, object table 15,430, EDRAM memo
every hit) with **0 differences**, and no DIFFERENCE / MISMATCH line in the log.

### Not done yet

- Steps 2/3 (native state flush from the device shadow, shader identity from the bound objects, coherency from the API,
  EDRAM ownership from SetRenderTarget / Resolve): sa2-nx has not built them either (its docs: "next"). For Peggle 2's
  ~33 draws a frame the profile does not point at them; the console profile should decide.
- A console measurement of all of this (blocked by the heap crash).

## Less CPU everywhere: codegen passes, diet options, smart sync (2026-10-07, PC + Switch ELF)

The user's goal relayed by the SA2 session (2026-10-07): 50-60 % less total CPU, by removing emulation, verified
pixel-identical, and judged on ARM (the Switch's A57: count operations and bytes; PC timings are a noisy proxy on a PC
that runs five ports' builds at once). Where the CPU goes (PC profile, gameplay windows, CPU seconds per thread,
`tools/pc_thread_cpu.py`): main thread ~62 %, Wwise's `AK::EventManager` ~23 %, the ring thread ~7 %, the rest small.
Two profiles of the same build and route differed by up to 17 % on the main thread: PC CPU-time A/B is noise here.

### Post-codegen passes (`tools/perf_codegen_post.py`, from futari-nx's tool)

Run by `tools/codegen.ps1` (default; `-NoPerf` / `-Classic` leave them out) and by the build's own codegen step
(`app/CMakeLists.txt`), before the localized copies are made. The profile marker says `optimised+perf` and the
`[enhancements]` line `direct calls + plain guest memory + loop barriers on`.

1. Direct calls: 96,354 `sub_X(ctx, base)` calls to unhooked functions become `__imp__sub_X` (inlinable). Any
   82xxxxxx address in the app/SDK sources, the tomls or the localized list counts as hooked (1,283). The old
   `tools/direct_calls.py` (`-DirectCalls`) never worked here: it looked for masseffect-nx's file names.
2. Guest memory without `volatile` (16 macros of `peggle2_pch.h`; MMIO keeps it; `REX_GUEST_VOLATILE` restores it).
3. Loop barriers at 24,826 loop headers (required with 2: futari-nx's 172 infinite loops).

Known race: the codegen step does not declare `peggle2_pch.h` as an output; after a codegen-option change the first
build can stop with "peggle2_pch.h has been modified since the precompiled header was built": build again.

### Diet codegen options (`app/perf_overrides.toml`)

`reserved_as_local`, `skip_msr`, `dform_disp_split`, `fctiwz_inline`: the set sa2-nx and futari-nx ship
(masseffect-nx: skip_msr + reserved_as_local -3.4 % total CPU). Checked in the generated code: no `ctx.msr` access or
global lock left, 1,498 inline fctiwz, D-form loads/stores everywhere. Left off: `elide_lr_stores` (the heap tracker
and the throw log read `ctx.lr`; the console heap diagnosis needs the callers), `non_argument_as_local`,
`args_in_registers`.

**Bug found and fixed on the way (affects any guard that rewrites the memory macros):** with `dform_disp_split` the
generated code stores through `REX_STORE_D_U*(reg, disp, value)`. The store-log guard's rewrite only matched
`REX_STORE_U*(`, so those stores were not logged: the rollback was incomplete, the original ran on memory the copy had
already changed, and verify mode reported 7 "differences" in the Wwise leaves (counters off by exactly 2x); in normal
runs every guard-checked call left the copy's stores in guest memory. `tools/localize_native.py` now maps them to
`PEGGLE2_LOG_STORE_D_U*` (`peggle2_hot_guest.cpp`): verify 0 differences over 129.7 M checks. Shared notebook: known
issue row. futari-nx's guard already covered the D-form macros.

### Smart sync in the localized copies (`peggle2_cpu_natives_smart_sync`, on)

sa2-nx's round-3 localizer: a call in a copy writes back and reloads only the localized registers its callee can reach
(every generated function indexed in `out/localize_index.json`; strongly connected components of the call graph);
unknown reach (indirect calls, traps, other imports, app hooks) keeps the full sync. Additions here:
- each smart call site keeps the full sync behind the key (A/B without a rebuild);
- **verify-mode check at every smart call site**: snapshot the context, make the call, compare every localized register
  outside the callee's reach (a write there = the analysis missed it); `SmartChecked()` counts them;
- kernel imports with known register use (`IMPORT_REACH`): KeTlsGetValue/SetValue, Rtl{Enter,Leave,TryEnter}CriticalSection
  (a contended enter waits non-alertably: no APC runs guest code), KeBugCheck(Ex);
- the heap tracker's hooks are transparent (`TRANSPARENT_HOOKS`: they read r1/r3/r5/r6 and ctx.lr, write nothing).
Result: 86 of the copies' 295 call sites sync less (937 register write-back/reload pairs fewer per pass); the rest reach
real indirect calls (the allocator's slow paths, virtual calls). Verify: ~4 M smart calls checked per 10 s, 0
differences.

### Checks

- Verify mode (final build: perf passes + diet + smart sync + the D-form fix), 150 s scripted route: 129,704,107 checks,
  **0 differences**, no DIFFERENCE / MISMATCH line.
- Lockstep against the two CPU-off baselines of the earlier build: 121 of 123 reproducible frames (only the loading
  spinner's edge frames 1077-1078, as before every change), 323 of 400 test frames found in the baselines. One run of the
  smart-sync build drifted (106 of 400; transitions and the unicorn ~6 frames out of phase, static frames identical); its
  repeat gave 119 of 123 and 319 of 400, and the CPU-off run found 100 of 101 frames that reproduce across the two.
- Switch ELF, perf passes (before the diet): branch-to-self loops in recompiled code 3 -> 1 (none new).
- ARM instruction counts (`tools/arm_insn_count.py`, the Switch ELFs): perf passes alone: recompiled code 5,953,801 ->
  5,388,081 instructions (-9.5 %, 2.1 MB less); hot non-copied functions -5 to -24 % instructions, loads -10 to -44 %,
  stores up to -38 %.
- PC CPU time: no change measurable above the run-to-run noise (x86 pays little for volatile; `ucrtbase!fmal` is 4.6 % of
  the main thread on the PC only: x86-64-v2 has no FMA, the A57 does).

### Ruled out for the console heap crash: ABA through CAS-emulated lwarx/stwcx.

The codegen emulates lwarx/stwcx. with a plain load and a compare-and-swap on the value, which cannot see an ABA a real
reservation would. All 157 lwarx/ldarx-to-stwcx./stdcx. sequences in the game are CAS-shaped (increments, adds,
exchanges, compare-with-expected; 24 64-bit ones in the allocator next to operator new): none loads through the
reserved value, so the code already relies on CAS semantics, as on x86.

### Later the same night: planted-bug test, the Switch LTO set, math flags, lockstep under load

- **Planted-bug test of the smart-sync check** (futari-nx's idea): `PEGGLE2_LOCALIZE_PLANT=1` makes the localizer
  pretend r3 is outside every callee's reach. Verify mode then reported 5 smart-sync DIFFERENCE lines within 22 s (and
  the game crashed, as it should with stale r3 values). The real build: 0. The verify-mode check now runs out of line
  (`peggle2::hot::SmartCall` with a static table of the registers to compare): the inline version had grown the copies
  from 37,339 to 46,887 instructions (I-cache on the A57).
- **Switch compiler set** (sa2-nx's round 3, the user's choice for the ports; `app/CMakeLists.txt` options, all ON):
  `PEGGLE2_SWITCH_LTO` (`-flto -fno-fat-lto-objects`, link `-flto=2 -flto-partition=balanced` repeating
  `-ffp-contract=off -fno-strict-aliasing`), `PEGGLE2_SWITCH_IPA_PTA`, `PEGGLE2_SWITCH_LINE_INFO` (-g1 DWARF 4 for the
  app's own code), `XXH_FORCE_MEMORY_ACCESS=0`. LTO ELF: .text 49.9 -> 53.5 MB (cross-file inlining: e.g.
  sub_824AD320 took in 3 of its 20 calls), branch-to-self loops in recompiled code still 1 (the game's own).
- **`-fno-math-errno -fno-trapping-math`** (`PEGGLE2_NO_MATH_ERRNO`, masseffect-nx ships them): OFF. Two captures with
  them missed the menu fade-out (game frames 1022-1047) that the CPU-off baselines reproduce, but so did a capture
  without them made under the same load (the Switch LTO build running). Not settled; small gain on ARM (the errno branch
  around sqrt), so left off.
- **Lockstep under load:** with a heavy build running on the same PC, captures miss stretches that otherwise reproduce
  (the 1020-1047 fade-out). The same build on a quiet PC: **122 of 123** reproducible frames (118 at the same game
  frame; only spinner frame 1078 missing), 325 of 400 frames found in the baselines: the best match so far. Compare on
  a quiet PC only.
- **Note for the CMake caches:** an `option()` keeps its cached value when its default changes: set it with
  `cmake -D<option>=<value> <build dir>` (done for out/pc and out/nx).

### sa2-nx round 7 ports (2026-10-07, morning)

- **`fp_moves_modeless`** (codegen, `app/perf_overrides.toml`; SA2's SDK change taken whole: `config.h/.cpp`,
  `builders/builder_context.h`, `builders/context.cpp`, `builders/floating_point.cpp`, `builders/memory.cpp`,
  `include/rex/ppc/diet_ops.h`): floating-point moves (lfd/stfd forms, stfiwx, fmr, fabs, fnabs, fneg) no longer switch
  the host flush mode, and lfs/stfs in VMX mode convert through `rex::ppc::SingleToDoubleAnyMode` /
  `DoubleToSingleAnyMode` (IEEE result in either mode; the denormal cases on a cold path with the mode cleared). On the
  A57 each switch is an `msr fpcr` (SA2's VMX-heavy thread spent 20-26 % of its time right after one); Peggle's Wwise
  mixer mixes VMX and scalar floats. 239 lfs/stfs sites use the any-mode conversions.
- **Dedupe sync epochs** (`peggle2_native_dedupe_sync_epochs`, non-CPU group: a correctness fix, Banjo's and SASRT's
  finding): `g_synchronizations_ring` was only ever read, so the vertex dedupe and the index cache never forgot their
  ranges at the game's synchronization points; the ring now advances it at EVENT_WRITE_SHD, MEM_WRITE, REG_TO_MEM,
  COND_WRITE (memory) and INTERRUPT.
- **Texture fingerprint sampling off** (`masseffect_native_fingerprints_sampling` = 0 in code; explicit-only row
  `ring_fingerprint_sampling`): not exact. Its guard tripped in 9 of Peggle's 121 PC logs (a 640x360 texture rewritten
  where the sample did not look); after the guard's double-checked rechecks a miss would be a stale texture.
- Checks: verify 133.2 M checks, 0 differences; quiet-PC lockstep 122 of 123 reproducible frames (118 at the same game
  frame, only spinner frame 1078 missing): the same as before these changes.
- Switch LTO ELF with `fp_moves_modeless`: `msr fpcr` 13,859 -> 10,495 instructions (-24 %, static), `_start` at 0,
  branch-to-self loops in recompiled code 1 (the game's own). Packaged (06:40 NRO).
- Not taken from round 6: direct_pair, identity memo, direct_state_refs, base_zero_r0 (per-draw ring costs; Peggle has
  ~33 draws a frame), elide_lr_stores (the heap tracker), the CRC32 fingerprint mode (PC/Switch A/B to come).

### Direct renderer step 2: not done in Peggle 2 (2026-10-07, decision)

sa2-nx did step 2 (Direct3D's state flush writes the direct record itself; its docs/direct-renderer.md "Step 2"): in
SA2, at ~1,650 draws a frame, the D3D draw path was ~16 % of the game thread. Peggle 2 draws ~33 times a frame: on the PC
gameplay profile none of Direct3D's functions (0x8260-0x8263) or the draw hooks (Begin/EndDrawDirect, NoteDrawCall,
NoteShaderObject) reaches the main thread's top 30 in any window (each under ~0.1 %). Step 2's placeholder and marking
concurrency would be real risk for ~0 gain here; to revisit if the console profile shows the D3D path. Also checked from
SA2's same message: its "Drawn() does not count redirected clears" trap is in its new resolve-alias code, which Peggle
does not have; its cause-109 fix (a linear fetch taken for a resolve) has nothing to fix here (0 native draws rejected in
the last 40 PC logs).

**Reversed the same afternoon by the user's order** ("all of SA2's CPU optimisations ... removing PM4 and EDRAM and
removing DirectX for a native ARM API, and codegen improvements"): see the next section.

## Everything sa2-nx has done for the CPU, and its native Direct3D layer (2026-10-07, afternoon)

The user's order of 2026-10-07: port all of sa2-nx's CPU work, including what was declined above for its small gain at
~33 draws a frame, plus the removal of PM4, EDRAM and the guest Direct3D. The order of the work, by what pays on the
Switch first: the codegen (game code is most of the CPU), the log writer, sa2-nx's ring rounds 6 and step 2, its GPU
rounds, then its native Direct3D layer (sa2-nx's docs/api-renderer.md) mapped to Peggle 2's addresses.

Not needed here: sa2-nx's thread reaper / stack pool. An 8-minute run over two level loads (`out/route3_long_script.txt`)
created 17 guest threads, all in the first two seconds, and no more.

### `non_argument_as_local` and `elide_lr_stores` (codegen, `app/perf_overrides.toml`)

The rest of the codegen "diet". The SDK took sa2-nx's generator change for the first (`non_argument_as_local_keep`:
`config.h/.cpp`, `builders/builder_context.h`, `builders/context.cpp`, `builders/control_flow.cpp`, applied as edits;
each file is now identical to sa2-nx's) and got one option of its own for the second (`lr_keep_callees`).

- **`non_argument_as_local`**: r0, r2, r11, r12, f0 and v32-v63 become C++ locals. MSVC uses r11, r12 and f0 as scratch
  everywhere. Every place where a value crosses a call in these registers was listed with sa2-nx's audit
  (`tools/non_argument_audit.py`, on code generated without the option, `--noreturn=825F7C30,825F2630,82714728,82318D20`):
  - arguments in r12 read on entry: the SDK's save/restore helpers (kept in ctx by the generator's rule); 4,273 EH/SEH
    funclets that are `share_registers` already; the stack probe `sub_825F1CE4`; and 177 unwind funclets of one function
    (`0x824D6028-0x824D7BA8`) that the generator does not mark. Nothing calls those directly: only the C++ EH dispatch
    `_CallSettingFrame` (`sub_82603730`: r12 = frame, then `blrl`). These, the dispatch and the continuation jump
    `sub_825F0AD4` (`mr r12,r5; bctr`) are in `non_argument_as_local_keep`;
  - one value returned in r11, by the `__finally` funclet `sub_82607EE0` for its owner `sub_82607E18`
    (`share_registers`: the generated copy-back takes it); 13 `subfe rX,r11,r11` after a call (the result depends only
    on the carry);
  - three functions read v57-v63 before writing them (`sub_823457F8`, `sub_823474A8`, `sub_82347C68`: the
    unaligned-load shift registers of software-pipelined loops): `tools/vec_shiftout.py` proves no entry byte reaches a
    store;
  - the two extra no-return functions: `sub_82714728` (an error exit: varargs, then a call that ends in an indirect
    call and never returns; the code after its 35 call sites reads r11 from another path) and `sub_82318D20` (ends in
    `ExTerminateThread`);
  - no hook, native or SDK runtime code reads `ctx.r0/r2/r11/r12/f0/v32-v63`; the setjmp callers write them again
    before reading them after `ppc_setjmp`.

  **Audit bug fixed (Peggle's copy):** after one run of element writes that covers a vector register (vlogefp128 v60,
  4 x `v60.f32[i] = log2f(...)`), the audit continued after the statements it had skipped past, which held the next
  register's run (vlogefp128 v59): those writes then counted as partial writes, i.e. reads. 9 false "read before
  written" reports (4 vlogefp, 5 vsraw functions); none was real.

  With the option, the audit's `--with-option` pass: the only localized registers read before written are the three
  shift-register loops; their callers' "not handed over" lines are the same dead bytes.
- **`elide_lr_stores`**: `ctx.lr = <return address>` is no longer stored before every call. The readers of `ctx.lr` are
  hooks that want their caller: the heap tracker and the string-release check (`peggle2_debug_hooks.cpp`) and the throw
  logger (`peggle2_throw_log.cpp`). New generator option **`lr_keep_callees`** (`config.h/.cpp`,
  `builders/control_flow.cpp`): a `bl` to a listed function keeps its store wherever it is. Listed: operator new,
  malloc, free, XMemAlloc, XMemFree, RtlAllocateHeap/FreeHeap/ReAllocateHeap, CreateThread, the string release and
  `_CxxThrowException`. Their immediate caller stays exact. **Caveat:** deeper frames of the heap tracker's guest stack
  walk (the saved LRs in the guest stack) and the console profiler's caller attribution may now show an older return
  address. The 06:40 diagnostic NRO (before this) is the one for the heap crash investigation.
- **Generated code:** `ctx.r11` uses 549,939 -> 13,488; `ctx.lr` stores 148,273 -> 10,681; 27,127 of 33,821 functions
  localize these registers.
- **Verify mode** (150 s scripted route): 111,917,830 checks, 0 differences, no DIFFERENCE / MISMATCH line.
- **Lockstep: the old baselines no longer apply (save state, not code).** The first capture of this build found only
  50 of the 123 reproducible frames: after Single Player the game went straight to the Adventure menu, where the
  baselines show a loading screen and the castle intro. The cause was the game's save: the 8-minute thread-count run
  had played on past the intro and saved (`Documents\peggle2\...\PROFILE\DATA`, and an achievement), so every later
  run skipped it. Checks:
  - the same capture with a copy of that save minus the achievement record: still no intro;
  - with a fresh, empty profile: the run follows the baselines' path; every reproducible frame from game frame 1048 on
    is present (68 at the same game frame), and 275 of 400 frames appear in the baselines. The rest (1000-1047) differ
    in the profile's progress counters on the menu (top right), plus the loading spinner's 1077-1078 as always.
  - **Test runs now never use the user's save:** each capture starts from a fixed seed folder copied into
    `out/pc/test_userdata/run_*` and passed as `user_data_root` (`REX_USER_DATA_ROOT`), so no run's progress can change
    the next one's menus. New CPU-off baselines were recorded from the empty seed (`video_f1000_ls_fresh_off1/2`).
  - For the other ports: a lockstep reference depends on the save as well as the code. Any longer run that saves
    progress invalidates the references made before it.
  - With the empty seed: the two CPU-off baselines agree on 123 frames; the defaults of this build (all CPU
    enhancements, the new codegen) have **122 of 123** (119 at the same game frame; only spinner frame 1077 missing),
    the same as every check before.

### The log writer (`log_file_writer`, SDK cvar, from futari-nx through sa2-nx)

`sdk/src/core/logging.cpp` and `sdk/src/core/exception_handler_switch.cpp` taken from sa2-nx (patched in; both files
are now identical to sa2-nx's). The logging thread only copies the formatted line to memory; a "Log writer" thread
writes the batch with one `fwrite` once a second (or at 64 KB, or on a warning). On the console a long log line was an
SD write of its own (newlib's 1 KB FILE buffer), 50-150 ms while music streams from the card: futari-nx's worst
stutters (a 1,022 ms frame) and 19 of sa2-nx's 54 stutter intervals. A Switch fatal error appends the lines not yet
written to `rex_crash.log`. Image and game unchanged (logging only).

### sa2-nx's renderer rounds 6-11, step 2 and the native Direct3D layer

**The files.** sa2-nx's current renderer (`me_native_system.cpp/.h`, `masseffect_native_draws.cpp/.h`,
`masseffect_native_targets.cpp`, `masseffect_deferred_recording.cpp`, `me_edram_ownership.h`) converted to Peggle 2's
names, then merged hunk by hunk with Peggle 2's own additions, which stay:
- the WAIT_REG_MEM parking (`masseffect_native_wait_reg_mem_us`, `SignalGuestProgress`) and its diagnostic line;
- the ring thread's CPU time in the 10 s line (`me_thread_cpu.cpp`);
- `peggle2_cpu_ring_vs_mismatch_memo` (sa2-nx has the memo without a key) and the relaxed VS match;
- the `[verify]` line (now with the step-2 flush checks); a lost direct record counts as a verify difference;
- BeginVertices' end field (+13844) for the step-1 path;
- the adaptive vertex dedupe (`peggle2_cpu_ring_dedupe_adaptive`), the single-snapshot fast-copy check and the prewarm
  guess not counted as a difference (both from banjo-nx; sa2-nx still lacks them);
- `masseffect_renderer_native` off by default on PC, the profiler's GPU-marks key on in every config.

The ring-side re-check of each record at apply time (Peggle's, from Transformed via futari-nx) is replaced by sa2-nx's
design: record references read the values in place and the three words under the marker are written back.

**New enhancement group.** sa2-nx registers GPU and fidelity keys in its renderer (`kNonCpu`). Peggle 2's framework
gained `PEGGLE2_RING_GPU_ENHANCEMENT` (under the non-CPU master; listed as "renderer GPU enhancements" in the
`[enhancements]` line; `tools/make_enhancement_configs.py` reads it). `peggle2_native_dedupe_sync_epochs` moved there
from the table.

**What is new for Peggle 2** (each key on by default, as in sa2-nx; descriptions in sa2-nx's docs/enhancements.md):

| Key | What |
|---|---|
| `peggle2_cpu_direct_pair` | the Draw* record rides in the call's direct record (no address-table lookup) |
| `peggle2_cpu_note_shader_pair` | both shader objects finished: two table loads instead of four calls |
| `peggle2_cpu_ring_identity_memo` | shader identity checks once per change, not per draw |
| `peggle2_cpu_direct_state_refs` | records hold the packet skeleton; the ring reads values in place |
| `peggle2_cpu_direct_state_restore` | the three words under a marker written back after the record is applied |
| `peggle2_cpu_ring_bound_plan_memo` | the EDRAM bound-range plan reused for an identical request |
| `peggle2_cpu_vertices_base_zero_r0` | one vertex-buffer binding per upload buffer, also for shaders that read r0 |
| `peggle2_cpu_direct_flush` | step 2: Direct3D's three flush functions write the record (hooks `sub_8262A4B0` / `sub_8262A358` / `sub_8262A118`) |
| `peggle2_api_draws` | native BeginVertices / DrawVerticesUP / window-scissor writer (`sub_826221A0` / `sub_82622660` / `sub_82617E58`) |
| `peggle2_api_clears` | native D3DDevice_Clear (`sub_8261EE00` and the five functions under it) |
| `peggle2_api_ring_zero_ahead` | the record ring's lines zeroed in the cache ahead of the head (DC ZVA on the Switch) |
| `masseffect_native_fingerprint_mode` = 2 | the A57's CRC32 pair (row `ring_fingerprint_crc32`; XXH3 on the PC) |
| `peggle2_gpu_resolve_draw`, `_blend_skip`, `_resolve_alias` | sa2-nx's GPU round (non-CPU group) |
| `peggle2_native_linear_fetch_not_resolved` | a linear fetch is never a resolve (fidelity fix, non-CPU) |
| `peggle2_gpu_push_once`, `_index_base_held`, `_structured_cf`, `_texcoords_unswapped`, `_no_texture_signs`, `_identity_remap` | GPU round 2 (non-CPU); all but push_once need the shader package built with sa2-nx's translator (taken: `shaders/XenosRecomp/shader_recompiler.cpp`, `shader_common.h`) |

`peggle2_cpu_main_thread_priority` is off by default, as in sa2-nx.

**The native Direct3D layer at Peggle 2's addresses.** A read-only mapping (`out/api_map.md`) paired 205 Direct3D
functions of the two games by following calls from 11 confirmed pairs and compared them instruction by instruction: all
identical in shape, equal lengths, the same branch layout, and 0 differing words over the layer's 32 functions in the
raw images. The only non-address immediates that differ are the D3D version number's (21076 in SA2, 21256 here). No
device offset differs; every constant table the layer reads from guest memory has identical contents. So the layer is
sa2-nx's code with an address table (`port_api.py` in the session's scratchpad: functions, the shader load's labels,
and the globals: interpolator flag/mask/declaration ids, primitive table, clear colour scale/bias, default z, clear
shaders, fast-clear constants, the alias-interior global). Differences from SA2:
- sa2-nx's DrawVertices is not linked in Peggle 2: its native entry is left out;
- DrawIndexedVertices (Peggle 2 uses it; sa2-nx never made it native) keeps the step-1/step-2 path;
- BeginVertices' verify-mode reference is the recompiled original (sa2-nx used its localized copy);
- the marker magic is 'PG2D'; sa2-nx's census tooling (`sa2_d3d_api.cpp`) is not taken.

**What the first runs found (PC, 2026-10-07 evening):**
- **Verify mode, native BeginVertices: a false difference in the comparison, fixed.** Device byte +12619 differed
  once per frame (native 02, 03, 04 ...; original 00; 00 before the call). Outside verify mode the native call never
  changes it. Direct3D's own draws (`sub_8261F310`, D3D's quads) bind a vertex shader object that lives inside the
  device struct; the shader load updates that object's binding state through the shader pointer, so in the shadow run
  the original's write lands in the real device instead of the copy. The layer already snapshots and compares the
  bound shader object as a region; the device-byte comparison now leaves bytes inside such a region to that check
  (`CompareShadow`, peggle2-nx). SA2 never binds such a shader during BeginVertices.
- **The native Clear (`peggle2_api_clears`) stalls Peggle 2: off by default (explicit only).** Bisected by keys: native
  draws alone run normally (19,755 native records in 60 s); the native Clear alone stops presentation about 20 s in
  (0 swaps, 0 packets per 10 s; the ring idle). The game thread then sits in Direct3D's GPU wait (`sub_8261D378`) on
  the second device (0x40074000), waiting for the GPU read pointer, which moves only every few seconds while Direct3D
  has more queued than it has kicked. Verify mode compared all 497 native clears with the original (ops, device bytes,
  regions, inline vertices): 0 differences. So whatever differs is in what the comparison treats as transport
  (submission bookkeeping, fences). Not found yet; reported to sa2-nx. Peggle 2 clears about once every two frames,
  so the CPU it would save is negligible here.

**Checks (PC, fresh-profile seed, against the fresh CPU-off baselines):**

| Build | Verify mode (150 s) | Lockstep (game frames 1000-1399) |
|---|---|---|
| sa2-nx renderer + native draws (Clear explicit-only), old shader package | 116,193,974 checks, 0 differences, no DIFFERENCE / MISMATCH line | **123 of 123** reproducible frames, all at the same game frame; 400 of 400 frames found in the baselines |
| the same, native draws off (step 1 + step 2 exercised, 120 s) | 28,799 direct-state records and 70,210 native flushes each compared with the original, 28,799 draw records carried in direct records: 0 differences | - |
| + the shader package built with sa2-nx's translator (GPU round 2's variants) | 144,244,960 checks, 0 differences | 121 of 123 (115 at the same game frame); missing: loading-spinner frames 1076-1077 (host-timed, as in earlier checks) |

The shader package with the new translator: 12 of 12 containers translated and compiled, 141,916 bytes (107,880
before); the old one is kept in `out/shader_package_backup/`. GPU round 2's keys are in use on this route (each
`peggle2_gpu_*` key counts thousands of uses per 10 s in the `[hot]` line).

Peggle 2's draws now take sa2-nx's native path: on this route every BeginVertices / DrawVerticesUP draw and every
window-scissor write is a native record (no fallback; `[api]` line), so none of their state goes through PM4 or the
ring's PM4 parser. DrawIndexedVertices (one caller) keeps step 1 + 2.

**Switch build (17:14):** NRO 54,671,557 -> 51,751,109 bytes; `_start` at 0; branch-to-self in recompiled code 1 (the
game's own `sub_8242E838`); `msr fpcr` 10,495 -> 10,365. Static AArch64 counts over every recompiled function (`sub_*` / `__imp__sub_*`, one
objdump pass per ELF; `tools/arm_insn_count.py --all-recompiled` launches objdump per function and takes hours on these
ELFs), 06:40 ELF -> this one, i.e. mostly `non_argument_as_local` + `elide_lr_stores`:

| | 06:40 | 17:14 | |
|---|---|---|---|
| instructions | 6,024,589 | 5,236,455 | -13.1 % |
| stores | 1,456,442 | 1,127,061 | -22.6 % |
| loads | 1,003,087 | 950,709 | -5.2 % |
| byte reverses (guest big-endian) | 508,201 | 471,831 | -7.2 % |
| calls | 185,900 | 184,140 | -0.9 % |

ELF kept as `out/nx/elf/peggle2_api_nonarg_20261007_1714.elf`;
the 06:40 NRO is archived (`out/nx/archive/peggle2_fpmodeless_20261007_0640.nro`). Packaged in
`out/dist/switch/peggle2-nx` with the new shader package and the regenerated configs.

**Still PM4 / still emulated after this:** resolves, the Swap / present, fences, locks and releases, device init, the
native Clear (explicit-only), DrawIndexedVertices' state (step 1 + 2), and EDRAM (render targets are still EDRAM tiles
in mode 4). sa2-nx's own order for these (docs/api-renderer.md "Status"): resolve, present, fences/locks, init, then
the transport switch (no ring, no markers), then EDRAM -> surfaces. They do not exist in sa2-nx yet; Peggle 2 takes
them as they land (its Direct3D is SA2's instruction for instruction, so the address table carries over).

**Candidate, not taken (2026-10-07 evening): sa2-nx's `sa2_gpu_vs_fetch_reorder`.** Direct3D's vertex fetch patcher
can move a VS's declared fetches into each other's slots for some declarations; such a load then matches no package
entry and its draws are dropped ("VS identity detail mismatch ... reason=unrepresentable-fetch-swizzle", then
"unidentified VS IM_LOAD"). sa2-nx proves the reorder at run time (`me_vertex_fetch_reorder.h`, `IdentifyReordered`).
Peggle 2 has no such case on any route so far: no "unrepresentable-fetch-swizzle" line in any of 147 PC logs, and all
1,534 "unidentified loads" lines since the shader library rebuild (2026-10-05) say 0 (the two older logs with
unidentified loads were Direct3D's own immediate shaders, wrapped since). Watch the console logs for that signature
(other masters' powers, Extreme Fever, trials and duels are not covered by the PC routes); port it if it appears.

**From futari-nx's port of the same GPU round (2026-10-08):** `BlendSkipBits` now also requires the pass's real color
attachment 0 to be R8G8B8A8_UNORM (`pass_formats_[0]`), not only the format the Xenos target maps to (a redirected
target could differ). It can only remove skips; on Peggle 2's route blend skip still applies in every 10 s window
(PC, 60 s run: 0 differences). Not taken: futari-nx's Switch shared font for CJK glyphs in the overlay (Peggle 2's
achievement names are Latin).


## sa2-nx's three CPU rounds, relaxed vsync and a review of all its optimisation keys (2026-10-13)

The user's request: port the three CPU rounds sa2-nx released as 1.0.1 (`D:\sonic adventure 2\sa2-nx\docs\cpu-rounds.md`;
its console result with a new PGO profile: "about 15 % better"), port its other CPU and GPU optimisations where they
are relevant to Peggle 2, and leave link-time optimisation and PGO for the release build. Everything below is measured
on the PC and with the Switch compiler; **nothing of it has run on the console yet**.

### How it was ported

- **SDK:** sa2-nx's own diff of the rounds (`git diff a6843d2 8be5b45 -- sdk/`, 189 KB) applied as a patch: 31 files
  changed, 10 unit tests added. Only `sdk/src/core/clock.cpp` conflicted (Peggle 2's frame clock; the census block was
  merged by hand). Peggle 2's `lr_keep_callees` stays. Originals: `out/src_backup/sdk_rounds_20261013`.
- **Tools:** `tools/post_codegen.py` (sa2-nx's pipeline, in its order, at Peggle 2's names) replaces
  `tools/perf_codegen_post.py`; sa2-nx's `direct_calls.py` (`--redo`) replaces the older one. Peggle 2's
  `tools/localize_native.py` (the store-log guard and smart sync are its own) learned the new text forms
  (`REX_CALL_INDIRECT_FUNC_AT`, `REX_PROMOTED_CALL`, `REX_GUEST_CAS`, leaf locals through sa2-nx's `unleaf`).
- **App:** `src/cpu/peggle2_codegen_checks.cpp` (the PC side of the generated code's self-checks, the census, the
  compare-and-swap key) and `src/cpu/peggle2_forms_selfcheck.cpp` (every helper that has a form for one processor
  only, against plain C++ references, at every start).

### What the rounds are, with Peggle 2's numbers

The generated tree: 33,821 functions from 1,939,533 PowerPC instructions.

| Round | Change | In Peggle 2 | Switch |
|---|---|---|---|
| 1 | `fp_compares_modeless`, `fp_mode_lookahead` without the bet past a branch (`fp_mode_lookahead_past_branch = false`, sa2-nx's finding) | 1,170 any-mode helper sites; 11,399 flush-mode checks in the tree | `app/perf_overrides.toml` |
| 1 | Guest compare-and-swap without the legacy builtin's trailing `dmb ish` (`tools/guest_cas.py`); the SDK's lock words inline (`__atomic`) | 210 sites in 65 files (161 `stwcx.`, 49 `stdcx.`) | `PEGGLE2_SWITCH_GUEST_CAS`; run time: `peggle2_cpu_guest_cas_no_barrier` |
| 1 | Flush-mode checks the control flow proves (`tools/flush_mode_flow.py --redo --image`) | 5,207 of 11,399 checks proven in 2,964 functions; 552 functions are entered in a known mode | `PEGGLE2_SWITCH_FLUSH_FLOW` (nothing on the Switch; the PC build still tests each) |
| 1 | No wake-up syscall when nobody is blocked | SDK | `thread_signal_skip_idle` |
| 1 | The hooks' call counters in the calling thread's context | all guarded hooks and the unguarded copies' use counters | `peggle2_cpu_hook_counter_ctx` |
| 1 | No frame pointer in the game code | a compiler flag | `PEGGLE2_SWITCH_OMIT_FRAME_POINTER` |
| 1 | Guest vector byte reversal as REV64 + EXT (`tools/vec_byteswap.py`), constant shuffles as one permutation (`tools/pch_neon_shuffles.py`), NEON forms of vperm and the float-to-integer vector conversions | 9,445 reversals, 1,782 table shuffles | `PEGGLE2_SWITCH_VEC_BYTESWAP`, `_NEON_SHUFFLES`, `_NEON_VMX_HELPERS` |
| 2 | `-fno-math-errno` on the game code and the localized copies' file (not `-fno-trapping-math`) | a compiler flag | `PEGGLE2_GAME_NO_MATH_ERRNO` |
| 2 | A loop barrier only where a guest load could be hoisted (`tools/loop_barriers.py`) | 20,202 of 24,826 loop headers keep theirs | - |
| 2 | Registers of leaf functions with loops as locals (`tools/leaf_locals.py`) | 1,113 functions, 10,150 registers (1,764 only read) | `PEGGLE2_GEN_LEAF_LOCALS` |
| 2 | A float copied through a register without a mode (`fp_copy_pairs`) | 352,000 loads and 12,000-15,000 stores a second run in that form | `app/perf_overrides.toml` |
| 2, 3 | Indirect calls with one to three usual targets as guarded direct calls (`tools/indirect_calls.py`, `tools/indirect_promotions.py`, `app/indirect_promotions.txt`) | 15,852 `bctrl` sites tagged; 182 promoted (23 with two or three targets, 9 with one dominant target) | `PEGGLE2_GEN_PROMOTED_CALLS` |
| 2 | `fctidz` as `fcvtzs` and a NaN select; `fctid` / `fctiw` round halves to even on ARM (`fcti_inline`) | 381 + 10 sites (the 10 are a correction of the Switch build: simde's portable conversion rounds halves away from zero) | `app/perf_overrides.toml` |
| 2, 3 | Loads from the image's read-only data as constants (`tools/ro_constants.py`) | 3,949 float, 1,356 double, 256 integer and 362 vector loads (82000600-821055C0) | `PEGGLE2_GEN_RO_CONSTANTS` |
| 2 | A single-precision load straight into a float register (`tools/float_loads.py`) | 13,309 loads (155 kept as integers) | `PEGGLE2_GEN_FLOAT_LOADS` |
| 2 | The trap helper out of line and cold (`tools/pch_cold_traps.py`) | the pch | `PEGGLE2_GEN_COLD_TRAPS` |
| 2 | Critical-section owner from the guest context (r13 -> KPCR) | 47,550 enters a second | SDK key `kernel_critsec_pcr_owner` |
| 2 | Pointer translation without a heap lookup off Windows; object reference counts inline | 4,891 and 26,023 a second | SDK |
| 3 | Registers of call-free loops as locals for the length of the loop (`tools/loop_locals.py`) | 6,977 loop regions in 2,916 functions, 29,666 registers (5,939 only read); 8,884 loops with a call left alone | `PEGGLE2_GEN_LOOP_LOCALS` |
| 3 | The divisions as the instruction (`div_inline`) | 3,304 sites | `app/perf_overrides.toml` |
| 3 | `kernel_state()` inline off Windows | 17,748 reads a second | SDK |
| 3 | The start-up self-check of every single-processor form | PC: 248 of 248 as expected (the x86 forms) | log line `[enh] instruction forms self-check`; a failure also writes `peggle2_selfcheck_failed.txt` |

Not taken, as in sa2-nx: `rotate_word_narrow` (no gain with devkitA64's GCC), `dcbt_prefetch` and
`fp_copy_only_functions` (off there), the three stack-frame elisions of `tools/frame_traffic.py` (not exact: SA2's
characters came out tinted), `-fno-trapping-math`.

### Peggle 2's census (PC, the scripted route, per second at 30 frames a second)

| | Peggle 2 | per frame | sa2-nx per frame | sasrt-nx per frame |
|---|---|---|---|---|
| flush-mode switches (an `msr fpcr` each on the console) | 107,500-137,200 | 3,600-4,600 | 1,368 | 60,800 |
| proven flush checks executed (nothing on the Switch) | 2,160,000 | 72,000 | | |
| any-mode helpers: conversions / compares / arithmetic | 39,000-49,000 / 13-35 / 2,200-3,200 | 1,300-1,600 / 1 / 100 | 29,813 conversions | |
| indirect calls before the promotions | 1,232,644 | 41,100 | 21,000 | |
| ... still indirect after them | 215,900-242,700 (the guards caught 80.3 %) | 7,200-8,100 | | |
| critical sections entered | 47,550 | 1,585 | 437 | 2,800 |
| events set / semaphores released / waits | 3,107 / 1,501 / 3,457 | 104 / 50 / 115 | | |
| handle lookups (29 found the global lock taken) | 13,015 | 434 | | |
| kernel exports called | 60,917 | 2,030 | | |

`RtlLeaveCriticalSection` is four fifths of the calls of the 16 most called kernel exports (7.66 million in 150 s). The flush-mode switches are few
next to sasrt-nx's. The console measured the cost later the same night (the profiler's header: a write that toggles
the flush mode takes 44.0 ns at 1020 MHz, about 45 cycles): 0.16-0.20 ms of a 33 ms frame. The bet past a branch
(`fp_mode_lookahead_past_branch = true`) was not tried. What weighs in Peggle 2 is the indirect calls (41,000 a frame,
twice SA2's) and the critical sections (1,585 a frame, 3.6 times SA2's): both are in this port.

### Three things found while porting

- **`tools/codegen.ps1` ran a stale generator.** It picked the newest file named `rexglue.exe`, which was a Release
  build of 2026-10-05; the PC build links `rexgluerd.exe` and re-runs it by itself. A tree written by the script had
  no `fp_mode_lookahead` (15,616 flush-mode checks instead of 11,399). The build's own step overwrote it every time,
  so no delivered build was affected. The script now takes the newest of `rexglue.exe` / `rexgluerd.exe` /
  `rexglued.exe`.
- **The store-log guard after the new steps** (sasrt-nx's caution). `float_loads.py`, `ro_constants.py`,
  `loop_locals.py` and `leaf_locals.py` write forms the localizer did not know. Audit of the 19 logged leaf copies:
  every statement that names guest memory is either one of the eight logged store forms (312 sites) or a known load
  (`tools/hot_store_forms.py`); the loop-local macros take the register as an expression, so the localizer's
  rename reaches them. Verify mode agrees (below).
- **Raw recordings.** Each lockstep run kept a 352 MB raw video under its own name: 49.7 GB in `out/pc/captures`.
  `tools/compare_hashes.py` now writes the per-frame hashes next to a recording the first time it reads it
  (`NAME.hash`, 14 KB) and reads that when the raw file is gone, and the run script keeps only `.hash`, `.frames`
  and `.txt` per run. Nothing was deleted (the user's decision).

### Relaxed vsync for a game that presents every second vblank (`peggle2_vsync_relaxed`, non-CPU group, on)

sa2-nx's `sa2_vsync_relaxed` gives the guest its vblank when the ring's flip wait asks for it instead of every
1/60 s. Peggle 2 presents with an interval of two, so the rule had to be read first. Direct3D's swap code
(`sub_8261F0B8`, the only writer of the device's flip queue) computes the flip's due vblank as

    due = last due + interval                    if that is still ahead of the vblank count
    due = the current vblank (flip at once)      else, when at most the swap's threshold per cent of the refresh passed
    due = the current vblank + 1                 else

and the vblank interrupt (`sub_8261EF88`) releases the queued flip, and the flag the ring waits on (memory 1F4A1006
on the PC), when the count reaches it. So a late GAME frame is already shown at once or at the next vblank by
Direct3D itself. A late RING frame is not: the flip was queued with its due vblank long before the ring reached its
wait, and with the fixed ticks the ring then waits for whatever tick comes next.

sa2-nx's version (every vblank on request) would cost Peggle 2 frames: a 20 ms frame would be shown after 36.7 ms
instead of 33.3. Here only the vblank that can release the flip follows the game; the ones before it stay plain
1/60 s ticks. Learned at start, with the regular ticks: which wait is the flip (sa2-nx's vote), then how many vblanks
a flip takes (the fewest of the last 8 flips, seen by the vblank thread: the flag set before the game's callback and
clear after it). Nobody asking for 50 ms: the regular ticks. Lockstep runs (`vblank_per_swap`) do not reach this code.

| Simulated load (PC, 80 s runs) | fixed vsync | relaxed vsync |
|---|---|---|
| none | 300-301 frames per 10 s | 300-301 |
| game thread +36 ms per frame (`diag_vdswap_delay_us = 36000`) | 229-273 (ring waits 7.2 ms per Swap) | 229-270 (waits 0.03 ms): the game thread is the limit |
| ring +36 ms per frame (`peggle2_diag_ring_swap_delay_us = 36000`) | 194-201: 20 frames a second (ring waits 12.4 ms per Swap) | 262-273: 26-27 frames a second (waits 0.03 ms), from the third window on (the first 20 s run on the regular ticks while it learns) |

The first 10 s window of each run (boot) is left out.

So it only pays when the ring thread or the GPU is the limit and a frame takes more than 33 ms there: 20 -> 27 frames a
second in the simulated case. Whether the console is ever in that case is not known (no console profile yet).
One run of the default settings made while another port's game used 2.6 cores had 10 s windows of 259-273 frames and
learned "3 vblanks" twice for a second each; the fixed-vsync run under the same kind of load was not made, so that
run says nothing either way. Off: `peggle2_vsync_relaxed = false`.

### sa2-nx's other optimisation keys, one by one

Every enhancement key and cvar of sa2-nx's sources was compared with Peggle 2's by name (546 against 524), and every
source file by content with the names normalised. 37 sa2-nx keys have no counterpart here:

| sa2-nx | Here |
|---|---|
| `sa2_vsync_relaxed` | **ported**, adapted to the interval of two (above); `diag_vdswap_delay_us`, `sa2_diag_ring_swap_delay_us` with it |
| `sa2_native_partial_resolve_inside` (+ `sa2_diag_stale_resolved`) | **ported** (`peggle2_native_partial_resolve_inside`, non-CPU). Peggle 2's logs show no partial resolve at all, so the check can only ever refuse the stale-texture case it was written for |
| `sa2_gpu_boost` | **ported** (`peggle2_gpu_boost`, non-CPU, on): the official 460.8 MHz handheld GPU profile. A clock, not a setting; more power draw in handheld mode; whether Peggle 2 needs it is not measured |
| `sa2_cpu_hook_counter_ctx`, `sa2_cpu_guest_cas_no_barrier` | **ported** (rounds) |
| `sa2_cpu_gpu_wait_sleep`, `sa2_cpu_ring_notify_lockfree` | already here under the table's names (`masseffect_wait_blocking_ring`, `masseffect_notify_no_lock`, both on) |
| `sa2_api_resolves` | not taken: off by default in sa2-nx too ("being verified") |
| `sa2_gpu_vs_fetch_reorder` | not taken: no such load in any Peggle 2 log (see above, 2026-10-07) |
| `sa2_gpu_render_scale`, `sa2_gpu_msaa`, `sa2_gpu_prewarm_current_keys`, `sa2_render_*`, `sa2_msaa` | not taken: a picture-quality feature (1080p docked, MSAA), not an optimisation; about 850 lines in the target code and 30 changed EDRAM shaders. The user's call |
| the thread reaper and the pool of thread stacks (SDK) | not taken: Peggle 2 creates 17 threads, all at boot |
| `log_to_file`, `switch_profile_report`, the built-in Switch defaults | for the release build: they turn the log and the profiler's report off, which Peggle 2 still needs (no console boot yet) |
| `SA2_SWITCH_HOT_ORDER`, the inliner limits, the PGO profile | held with LTO and PGO (the user); the first two need a console profile |
| `sasrt_cpu_index_no_fingerprint`, `sasrt_cpu_ring_draw_trims` (sasrt-nx's, offered by its session) | not taken: 26-34 draws per Swap here, 1.0-1.5 ms of ring work per Swap on the PC |
| sa2-nx's native replacements of game functions (Ninja matrices, vertex loaders, its localized copies) | SA2's game code; Peggle 2 has its own 47 localized copies |
| language, DLC, sideways Joy-Cons, video and profiler cvars, static modules | features and tools, not optimisations |

Also taken from sa2-nx's build: `peggle2_register.cpp` (one function of about 34,000 `SetFunction` calls) is compiled
with `-O0` on the Switch; the PC build defines `REX_DIET_CENSUS_ON` for the census.

### Checks (PC, the final build)

The runs are scripted: `tools\win\verify_lockstep.ps1 -Name X -Seed seed_empty -Base1 ls_fresh_off1 -Base2
ls_fresh_off2` (verify mode, then the lockstep capture and its comparison), `tools\win\census.ps1` (the census run)
and `tools\win\vsync_test.ps1 -Keys "key=value,..."` (frames per 10 s). Each starts from a copy of the same save
seed under `out/pc/test_userdata`; the user's own save is never touched.

| Check | Result |
|---|---|
| Verify mode, 150 s of the scripted route, fresh save | 127,234,712 checks of the natives and the renderer, 0 differences; generated code: 40,146,076 loads folded to constants, 61,431,834 promoted indirect calls and 5,679,356 critical-section owners compared with the original computation, 0 differences |
| Flush-mode proofs | 265-267 million proven checks executed per 150 s run, 0 had to switch the mode |
| Lockstep against the two CPU-off baselines (`ls_fresh_off1/2`) | run 2: 120 of 123 reproducible frames (111 at the same game frame; missing 1075-1077, the loading spinner's host-timed frames, as in every earlier check), 321 of 400 frames found. Run 1, made while another port's game used 2.6 cores: 90 of 123 (the menu fade-out 1022-1047 missing, the known effect of host load, 2026-10-07 night) |
| `[enh] instruction forms self-check` | 248 of 248 |
| DIFFERENCE / MISMATCH lines in the 12 runs of the day (logs 149-160) | 0 |
| The SDK's unit tests on this tree (`tools\win\run_sdk_tests.ps1`) | 9 tests, 0 failed: any-mode float helpers 600,081,920 comparisons (negative control 448,806 differences), narrow rotates 2,568,192, signals 19.4 million notified and 3.1 million skipped with no lost wake-up, byte reversal 4,004,081 vectors, NEON VMX helpers 19,006,688, the small helpers on all 2^32 inputs, constant shuffles 10,240,512, conversions and float copies 17,419,871,608, float loads on all 2^32 patterns; 0 differences each |

### The Switch build (no LTO, as asked; not run on the console)

`tools\win\build_switch.ps1 -Define "PEGGLE2_SWITCH_LTO=OFF"` (the new `-Define` sets CMake options; they stay in
`out/nx` until set back, so the release build needs `-Define "PEGGLE2_SWITCH_LTO=ON"`). A full rebuild took 8 minutes
10 seconds with 10 jobs. NRO 48,089,285 bytes (the LTO NRO of 2026-10-07: 51,751,109). ELF kept as
`out/nx/elf/peggle2_rounds_nolto_20261013_0243.elf`; packaged in `out/dist/switch/peggle2-nx` with the regenerated
configs, among them three one-key tests against `peggle2_cpu_on.toml`: `peggle2_test_vsync_relaxed_off.toml`,
`peggle2_test_gpu_boost_off.toml`, `peggle2_test_cpu_rounds_runtime_off.toml`.

- `tools/check_switch_build.py` (sa2-nx's, at Peggle 2's names): `_start` at 0, the optimised codegen profile named,
  Mesa 26.2.2 of the SDK, no vector fused multiply-add, one branch-to-self, in the one function whose PowerPC code has
  a `b .` halt (`sub_8242E838`: it calls `sub_8236EF40` and halts).
- `tools/arm_forms_check.py`: all claims hold with the Switch compiler (`fctid` is `frinti` + `fcvtzs`, the divisions
  one `sdiv` / `udiv`, vperm one `tbl`, the lock words `ldaxr` / `stlxr` with the `dmb` behind the key's test).

The recompiled functions in the ELF (one objdump pass, `tools/arm_recomp_totals.py`):

| | this build (no LTO) | 2026-10-07 17:14 (LTO, before the rounds) | 2026-10-07 04:15 (no LTO, before `non_argument_as_local`) |
|---|---|---|---|
| code bytes | 17,550,264 | 20,952,804 | 20,789,084 |
| instructions | 4,387,566 | 5,238,201 | 5,197,271 |
| loads / stores | 784,634 / 918,221 | 951,035 / 1,127,385 | 883,932 / 1,235,004 |
| `msr fpcr` sites | 6,327 | 10,173 | 13,128 |
| frame pointer set-ups | 31 | 25,754 | 25,415 |
| moves from a general to a float register | 4,789 | 19,727 | 19,924 |
| `fcvtas` (halves away from zero) | 0 | 12 | 10 |
| `dmb` | 323, each behind the key's test | 365 | 325 |
| direct / indirect call sites | 139,705 / 16,798 | 166,652 / 17,596 | 144,305 / 16,825 |

Neither older ELF is the same build without the rounds (one is an LTO link, which inlines and so repeats code; the
other lacks 2026-10-07's codegen options), so the table says where the code stands, not what the rounds alone did.
For that, five of the 122 generated files (13, 31, 57, 77, 104) were compiled with the Switch compiler twice, with the
rounds' build switches on and all off (`tools/switch_rounds_measure.py`, through
`tools/switch_compile_check.py`); the generator options are in both:

| 5 files | switches off | on | change |
|---|---|---|---|
| instructions | 197,424 | 190,475 | -3.5 % |
| loads | 41,749 | 38,622 | -7.5 % |
| stores | 50,452 | 47,951 | -5.0 % |
| `msr fpcr` sites | 407 | 267 | -34.4 % |
| frame pointer set-ups | 964 | 0 | |

sa2-nx measured -2.6 % instructions and -2.4 % loads and stores the same way on one of its files. These are static
counts: what runs less on the A57 is for the console profile to say. Not measurable on a PC at all: the `dmb`, the
wake-up syscalls, the frame pointer, the NEON forms.

**Still to do for the release build (held by the user's instruction):** LTO back on, the inliner limits and the
hot-function order sa2-nx found (they need a console profile), PGO with a profile recorded AFTER these rounds (sa2-nx
lost about 30 % of its functions' profile data to them), and the release settings (`log_to_file`,
`switch_profile_report`).

### First console run of this build (2026-10-13 02:57, `logs1 default config`): a black screen that was the pack build

The user reported a hang on a black screen. The log and the profiler say the game had not started yet:

- `[pack] no asset pack yet: building /switch/peggle2-nx/game_root.nxpack from the game folder (first boot, this takes a
  while)` at 02:57:26; packing 0-100 % took 67 s, verifying had reached 90 % after 16 s more when the log ends (02:58:49).
- The profiler's last 10 s report: the main thread 20.7 % busy and 78 % waiting in the kernel, in
  `PackDevice::Build` <- `Runtime::SetupVfs` (addresses resolved with the ELF of the 02:43 NRO,
  `out/nx/elf/peggle2_rounds_nolto_20261013_0243.elf`); game 0.0 fps, guest memory 26 MB backed, 7 MB mapped.

So the card held the loose `game_root` folder and no pack, the first boot built one with nothing on the screen, and
the game was closed two or three seconds before it finished. An unfinished build leaves only `.tmp` files, so the next
start would build again from the beginning. The ready-made pack (`out/dist/switch/peggle2-nx/game_root.nxpack` +
`.nxidx`, 922 MB, checked against the game folder with `tools/build_asset_pack.py --verify`: 34 files) copied next to the
NRO skips the build. Nothing shows the build's progress on the screen: a candidate, not built.

What the run does say about the rounds, before the game starts:

- `[enh] instruction forms self-check: 248 of 248 results as expected (the AArch64 forms)`: the first time the
  ARM-only forms ran on the console.
- This console's clocks are not the stock ones: GPU 1228.8 MHz and memory 1600 MHz from the start, CPU 2397 MHz and
  memory 2733 MHz in the last report. `peggle2_gpu_boost` therefore changed nothing there (`[apm] SET configuration
  0x92220008 ...: GPU 1228.8 MHz (was 1228.8)`).

Whether the earlier boot crash (the heap, about 4 s into the game's start) is still there is not known: this run ended
before the game's first instruction.

**After the run:** banjo-nx's correction of sa2-nx's partial-resolve rule arrived through the sa2-nx session (a later
rectangle may declare the height LEFT below its row, not only the whole texture's; with the first form alone Banjo's
shadow atlas bands became textures of their own). Taken: `height_fits` in `SearchResolvedPartial`. Peggle 2 has no
partial resolve and no refusal in any log, before or after (PC run of the new build: 0 and 0, 0 differences). The NRO
was rebuilt with it at 03:11 (48,089,285 bytes again; `check_switch_build.py` passes; ELF
`out/nx/elf/peggle2_rounds_nolto_20261013_0312.elf`) and packaged.

## The console boot crash: cause and fix (2026-10-13, `logs2`)

Every console boot since 2026-10-05 ended about four seconds into the game with the main thread reading guest
`0x00750064`. The third console log with the heap tracker (the 03:11 NRO, `out/dist/switch/peggle2-nx/logs2`) shows
what the first two could not, because this build logs every heap operation around the object:

    #1311838 alloc 414B9AF0 +188  lr 822CAF88     the 392-byte object (the network manager's first plugin)
    [dbg] 392-byte object constructed at 414B9AF0: field +96 = 828F6340
    #1311844 free  414B9AF0 +188  lr 822E1C64     the object's deleting destructor (sub_822E1C30), 0 ms later
    #1312629 free  4155F990 +24   lr 822C5C78     the singleton itself, in its own set-up function (sub_822C5C08)
    [dbg] singleton 4155F990: inner object 414B9AF0, its field +96 = 414B7470
    [dbg] string release with no lock ...          the freed block, reused by then
    Unhandled guest access violation: read of guest 0x00750064     one second later

All on the main thread. The heap was never wrong: **the game freed its own object**, on a path the Xbox 360 does not
take.

**The path.** `sub_822CAF38` is the constructor of the game's network manager. It creates a 1,568-byte object (RakNet's
`RakPeer`), attaches two plugins (the 392-byte and a 28-byte object, through the virtual function at +264) and calls the
virtual function at +4 with a 52-byte descriptor on the stack (a port from the configuration, an empty host address,
family 2, blocking 1) and the thread priority -99999: `RakPeer::Startup(maxConnections, SocketDescriptor*, 1, -99999)`.
A non-zero result is the error path: the constructor formats a message, sets a failed flag (+176 of its
configuration) and returns; its caller destroys the manager, and the rest of the game goes on using the pointer.

**Why it failed only on the Switch.** `Startup` creates a UDP socket. The SDK's `XSocket` calls the host's `socket()`;
on the Switch that is libnx's BSD socket service, which nothing starts (only `log_network` calls
`socketInitializeDefault`). So `socket()` returned -1 and `Startup` an error. On the PC Winsock works: the log there has
RakNet's two threads (`[thread] created ... routine 825FBDD8`, twice) and the `NetDll_getsockname STUB` line that follows
the bind; the console logs have neither. Starting the service would not have been enough: `bind` is handed
`N_XSOCKADDR_IN`, whose first field is Winsock's 16-bit family, and libnx's `sockaddr_in` starts with BSD's
`sin_len` / `sin_family` bytes.

**Reproduced on the PC.** `net_diag_socket_fail = true` (new, test only) makes every guest `socket()` fail. The PC then
does exactly what the console did, down to the tracker's operation number: `#1311844 free 414B9AF0 +188 ... lr
822E1C64`, `field +96 = 414B7470`, `string release with no lock: obj 414B9B50 handle 414B7470`, `read of guest
0x00750064`, 4.5 s after the start (`out/pc/logs/peggle2_162.log`).

**The fix: offline sockets** (`net_offline_sockets`, SDK cvar, true on the Switch, false elsewhere;
`sdk/src/system/xsocket.cpp`). A guest socket then has no host socket behind it and behaves like one on a network with
no traffic: `socket`, `setsockopt`, `ioctlsocket`, `bind` and `sendto` succeed, a non-blocking `recvfrom` reports
WSAEWOULDBLOCK, a blocking one waits until the socket is closed. A datagram sent to a port another offline socket of
the process is bound to is delivered to it (RakNet wakes its receive thread that way). The first 24 events are logged.
What the game does with it (PC, `net_offline_sockets = true`):

    [net] offline socket: created (family 2, type 2, protocol 254)
    [net] offline socket: blocking mode
    [net] offline socket: bound to port 1000
    __imp__NetDll_getsockname STUB                                    (as with host sockets)
    [net] offline socket: sendto of 4 bytes to port 0: nobody listens there     (RakNet's test send)
    [net] offline socket: a blocking recvfrom waits (nothing arrives from outside)
    [dbg] singleton 4155F990: inner object 414B9AF0, its field +96 = 828F6340

| Check (PC) | Result |
|---|---|
| `net_diag_socket_fail = true` | the console's crash, same addresses and operation number, after 4.5 s |
| `net_offline_sockets = true`, 60 s | both RakNet threads created, the manager intact, no crash |
| host sockets (the PC default), 40 s | the same two threads and probe lines |
| verify mode with offline sockets, 150 s | 161,536,425 checks, 0 differences |
| lockstep with offline sockets | 118 of 123 (missing 1073-1077, the loading spinner's host-timed frames) |

Not verified: the console itself (the NRO of 03:37, 48,101,573 bytes, `check_switch_build.py` passes; ELF
`out/nx/elf/peggle2_offline_net_nolto_20261013.elf`). The game has no network play on the Switch either way: there is
no Xbox Live behind it.

**What this closes.** Every earlier candidate was a consequence, not the cause: the "heap that split a live block"
(the block had been freed by the game), the singleton race, the barriers, the compare-and-swap ABA, recycled heap
memory. The heap tracker, the three probes and the thread-start delays in `app/src/peggle2_debug_hooks.cpp` can come
out of the release build; they stay for the next console run, which is the first that can reach the menu.

**For the other ports:** a game whose start-up needs a socket fails this way on the Switch with no message from the
SDK (a warning is logged now when a host `socket()` fails). Peggle 2 is the first of the ports to open one at boot.

## PS4 content in the Xbox 360 build (2026-10-13, PC only so far)

The user's request: the PS4 release (Peggle 2 Magical Masters Edition, CUSA00743, the user's own dump in
`D:/New folder`) has content the Xbox 360 version never got: two masters with their areas (Jimmy Lightning, Windy the
Fairy) and costume packs. Get it working in the 360 build, tested on the PC first. No game data is in the repository:
everything extracted or built is under `out/ps4_dlc/` (git-ignored).

### What the PS4 packages are (a background agent's survey, its listings in `out/ps4_dlc/listings/`)

- The five DLC packages (1 MB each) are unlock keys only: a header, `param.sfo`, an icon, and an empty file system.
  All content is in the base package's `game_data/main.rsb` (2.31 GB), read with the all-zero passcode by the
  agent's own Python (`out/ps4_dlc/scripts/ps4pfs.py`); nothing was downloaded.
- Both platforms use PopCap's RSB version 4 with the same layout: the 360 archive is big-endian (`rsb1`), the PS4's
  little-endian (`1bsr`). Levels, animations (`.PAM`), `.RTON` / `.JSON` settings, scripts and particle files are the
  same formats, many byte-identical (248 of the level files). Textures are BC3 on both, 720 art on the 360 and 1080 on
  the PS4 (in 8x8-block Morton tiles). Sound banks are the same Wwise version with the other byte order, and the audio
  is ATRAC9 on the PS4 where the 360 has XMA2.
- The 360 archive already holds a good part of Windy: her 10 level files, area script, store art, strings, intro
  videos of both masters, and an early `MultiBall` power (Jimmy's). The 360 executable registers six power classes;
  `PurplePegs` (Windy's) is not among them.
- "Shiver Me Timbers" (DLC 05) has an unlock key but no data in this package.

### Tools (`tools/ps4_dlc/`)

| File | What |
|---|---|
| `rsb.py` | RSB reader and writer for both byte orders. The writer copies existing packets untouched and can replace plain files, add packets, composites and textures |
| `rton.py` | RTON decoder and encoder that keep every value's stored type: an unchanged file encodes to the same bytes |
| `ptx.py` | BC3 textures: the PS4 tile order (found by scoring all 720 orders of the index bits), decode, alpha-weighted resize, encode (Pillow) |
| `build_mod.py` | builds the modded `main.rsb` from the two archives |
| `selftest.py` | every map, packet and the manifest rebuilt from what was parsed, and the whole archive written again |
| `frames.py`, `tools/win/mod_test.ps1` | a PC run on a modded game folder (`game_data_root`, no asset pack, its own fresh save) and PNG frames of it |

`selftest.py` on both archives: the three name maps rebuilt byte-identical (360: 1,539 files, 404 packets, 206
composites; PS4: 2,013 / 493 / 254), every packet without textures rebuilt byte-identical from its files (207 and
243), the manifests round-trip identical (3.6 MB and 6.5 MB), and the 360 archive written through the writer without a
change is the same 917,962,752 bytes (same SHA-256).

### How content is wired (read from the data, not guessed)

- `PROPERTIES/MASTERS.JSON`: per master its power class, costume list, music, art group (`Masters_Bjorn`). A costume
  `Costumes_X` of a master is the composite `Masters_<group>_Costumes_X`: a `_720` packet with one atlas and a
  `_Common` packet with one animation.
- `PROPERTIES/RESOURCES.RTON` (the manifest group; the executable names `resources.rton`): 589 groups, every
  resource with a slot number (21,551 slots), sprites as rectangles of their atlas. A sprite's file name carries its
  design size (`Bjorn_cyborg_100x86` is 31x27 pixels in the 720 atlas and 47x41 in the 1080 one: design size times
  resolution / 2304).
- `PROPERTIES/UNLOCKS.JSON`: what needs what. The PS4 file gates DLC with `requiredPurchase_ps4`; an item without an
  entry is unlocked.
- `LEVELS/LEVELS.JSON`: stories, areas (script, master power, 20 stages) and the progression.

### First result: the two costume packs for the five launch masters

`build_mod.py` takes each of the 10 composites (`Masters_{Bjorn,Jeffrey,Berg,Gnorman,Luna}_Costumes_{Garden,Spooky}`)
from the PS4 archive: the 1080 atlas is untiled and decoded, cut into its sprites (351 to 696 per costume), each
scaled by 2/3 with alpha-weighted Lanczos, packed again with a 2-pixel gap and its border repeated once, encoded as
BC3; the animation file goes in unchanged (PS4's are 60 frames a second, 1.8 to 4.7 MB); the manifest gets the three
groups per costume with new slots (21,551 -> 26,452); `MASTERS.JSON` lists the costumes. Result: 957,329,408 bytes,
424 packets, 216 composites, 247 textures; `selftest.py` passes on it.

**PC test** (`tools/win/mod_test.ps1`, the scripted route, a test archive in which Bjorn's list starts with the Garden
costume so no menu is needed): the game boots, plays the level intro and Bjorn walks in wearing the PS4 costume, at
the stock size and position (`out/ps4_dlc/shots/compare_bjorn_stock_vs_garden.png`: the unmodified game left, the
modded right, same moment). The log has no error beyond the three optional files every run lacks. So the 360 build
loads a new composite, new manifest slots, a converted atlas and a PS4 animation.

Not done yet for the costumes: the costume menu with six entries per master and their icons (the 360 interface was
laid out for four; untested), the unlock rules, a tighter atlas packer (the shelf packer needs 2048x2048 where the
original tool would fit 2048x1024), all 10 looked at in play, and the console (texture memory).

### The two masters, their powers and their areas (2026-10-13, later the same day; PC only)

The request grew: Jimmy Lightning and Windy with powers as close to the PS4's as possible, and every step of the
conversion. State at the end of the day: both areas are in the Adventure list with their portraits, intro cutscenes,
level thumbnails, their own backgrounds and side art, and both powers work in play on the PC. Sound of the new content
and a few menu and rules details are still open (the list is at the end).

**How the research was done.** Three background agents read the executables, each writing listings under
`out/ps4_dlc/` (not in the repository): the 360's power framework (class registration, the 53 virtual functions of a
power and the 22 of its renderer, the rules' calls), the PS4's two power classes function by function (the PS4
executable is the user's dump; its ELF was rebuilt from the SELF, 24,953 functions from `.eh_frame_hdr`), and the 360
engine's interface for drawing, particles, resources, targets and style shots. What the port's code relies on is named
at each use, with the 360 address it was read at.

**Where the code is** (`app/src/dlc/`, about 1,300 lines; key `peggle2_dlc_powers`, on):

| File | What |
|---|---|
| `peggle2_dlc_guest.h` | host code among the game's objects: big-endian loads and stores, calls of game functions with the PowerPC convention, stack space of the game thread, temporary `std::string`s, a new game-side address for a host function (the dispatcher's thunk pool) |
| `peggle2_dlc_game.h`, `peggle2_dlc_engine.cpp` | the game's functions and layouts in use, and the engine as the powers call it (resources by name, animations, particle effects, drawing, style shots) |
| `peggle2_dlc.cpp` | the keys, class registration (the sequence of the game's own `GetRtClass`), and with `peggle2_dlc_log` the game's own debug messages in the log |
| `peggle2_dlc_windy.cpp` | class `PurplePegs`, new: the 360 executable has none |
| `peggle2_dlc_jimmy.cpp` | class `MultiBall`: the game's early class with a larger object, three functions hooked (class getter, factory, build-symbols) and nine virtual functions replaced |

Both are classes of the game's own reflection system: the game creates them by name from `MASTERS.JSON`, loads
`PACKAGES/<NAME>.RTON` into their properties, and calls them through vtables whose entries are thunk addresses of host
functions. With the stock archive no master names either class, so nothing calls them.

**Windy, `PurplePegs` ("Fairy Flock"), as on the PS4:** always on; every green peg hit in normal play slows the
board to a stop over 0.5 s, four fairies appear around the peg one after the other, each picks a random blue peg that
may become purple and no other fairy has, flies to it (at least a second) and turns it purple; then the board speeds
up again. A fairy peg stays purple until hit (the per-turn purple draw skips it, the same flag test as the PS4);
hitting it sends the fairy away and counts towards "Big Purps" (3 in a shot) and "Fairy Fiesta" (8 in a level).
Drawn: the PS4's fairy animation with the game's trail effect, sparkles on the green peg, the board dimmed.
One 360 difference had to be bridged: the 360's rules switch off a power with `mTurns >= 0` at the end of a turn even
in the "always on" state, so the class holds `mTurns` at -1.

**Jimmy, `MultiBall`, as on the PS4:** a green peg spawns two balls at once (the PS4 data file: delay 1 tick,
above the peg, 0.8 rad either side of straight up, speed 0.75) and is destroyed a tick later; a lightning bolt from the
top of the board to the peg for 0.75 s and the effect `PIEFFECT_PI_JIMMY_POWER`; the background dims while the power is
on; slow motion (0.1) while a ball is about to reach an unhit green peg; "Free Ball Frenzy" from the second ball in
the bucket on ((count - 1) x 10,000); "Multiball Madness" with fever.

**Differences from the PS4 that remain, none in the rules of the two powers** (state after "The powers' screen
layers and motion" below; with the two keys named there off, the state is the one of the morning: plain dimming,
straight eased fairy paths, free-flight look-ahead):
- Windy: the ultraviolet screen is drawn as a mesh, not through the two shaders: its holes and alpha are the
  shader's, its colour is one darkening for the whole image where the PS4 cycles red and blue per pixel.
- Jimmy: the spotlights and the holes around the balls are one mesh (alpha interpolated between vertices up to 0.03
  off the shader's). The look-ahead bounces off targets taken as circles (bricks too) with one measured factor, where
  the PS4 steps a copy of the ball through the real physics. No controller rumble at the spawn.
- (Fever is game state 6: seen in a test, see "Fever" below.)
- In multiplayer rules classes other than the single-player `GameRules` the fever test is off (state unknown there).

**Three findings worth keeping:**
- A level's load list gets a null entry for every group it does not find (master shared group, costume, background),
  and the load crashes later (read of guest 0x1C in `sub_8238D540`). The game's own message names the group
  ("Error loading master shared group: %s not found"), but the shipped build throws its debug messages away:
  `peggle2_dlc_log` prints them. This was the crash of every first attempt with a new master.
- The 360 keeps a master's cannon art and power effects in a group `Masters_<X>_Shared`; the PS4 uses that name for
  the master's sound bank and keeps the art in `BOARD`. The builder makes the 360-style group from chosen PS4
  resources.
- The single-player game runs on the class `GameRules` (power list at +852, state at +1036, a green peg reported to
  the power only in state 2: `sub_821CD908`), not on `MasterPowerGameRules`, which the first survey had read.

**The archive builder** (`tools/ps4_dlc/build_mod.py --content costumes,jimmy,windy,levels,strings`) now also:
builds a 360-style composite from chosen PS4 resources and everything they reference (an animation's sprites from
its file, an effect's by name), gives PS4 resources new ids where the 360 uses the id for something else (Jimmy's
lightning images), attaches new subgroups to an existing 360 composite (the area list's portraits, 42 level
thumbnails), adds files with manifest entries to an existing packet (52 level files and scripts), merges the PS4's
JSON entries the 360 files lack (areas, progression, backgrounds, challenges, dialogs) and 102 strings per language,
and replaces the two powers' data files by the PS4's. The two areas are appended to the 360's one story as areas 7 and
8: the 360 executable has no paging between stories, and its area list scrolls. Result: 1,127,153,664 bytes, 539
packets, 415 textures (the 40 level backgrounds are most of the growth).

**PC tests** (`tools/win/mod_test.ps1`, scripted controller, muted and minimised; frames in `out/ps4_dlc/shots/`):
- Both masters as the master of the first area: level loads, the master with the real side art, no error.
- Windy's power with the test switches `anyhit+anypeg` (the first level has no purple pegs, so no blue peg is
  eligible there, as on the PS4): 3 green-peg events, 12 fairies, 12 pegs turned purple, the fairies sit on them over
  the following turns; the board stops and resumes (time scale 0 is safe in the 360 simulation).
- Jimmy's power with `greens+oneorange` (14 blue pegs made green): 8 chained green pegs in one shot through the
  game's own green-peg path, 16 balls, lightning and effect drawn, the power off at the end of the turn, Free Ball
  Frenzy awarded 10,000 to 50,000.
- The Adventure list: "Hallelujah Hollow" and "Jimmy's Jammin' Jaunt!" as areas 7 and 8 with portraits; both level
  lists with thumbnails, names and objectives; both intro cutscenes (the 360 disc has both videos); level 1 of each
  with its converted background; a real green peg hit on Jimmy's first level (a PS4-revision level file: the 360
  executable loads them).
- Level files: the 22 new or changed ones are header version 0x608 with object records 0x60E; the 360 archive has
  six 0x608 files and none with 0x60E objects. One of them plays; the other 21 were not each opened.

**Test switches** (`peggle2_dlc_test`, never in a delivered config): `anyhit` (the first peg of a shot acts as a
green one), `anypeg` (fairies take pegs of levels without purple pegs), `greens`, `oneorange`.

**Unlock rules** (`UNLOCKS.JSON`, done later the same day): the PS4 file's rules for everything the build adds
(40 levels, Jimmy's area, 2 trial sets, Jimmy, 20 costumes) without their store requirement (`requiredPurchase_ps4`:
the five packs are the user's own), so the rules of play stay: a level needs the one before, the trials need the
area's ten levels, costumes two to four need the area, its trials and 25 objectives. The 360 file already gated
Windy and her area with its own marketplace offer (`"requiredPurchase": 15`): removed the same way. Tested from a
save that has only passed the introduction, with no test option in the archive: areas 2 to 6 locked as before, areas
7 and 8 open with their first level, Windy's cutscene and level 1 play, a real green peg released four fairies and
four pegs turned purple (`out/ps4_dlc/shots/final1_3200.png`).

**The stock game with the module built in** (stock archive, PC): verify mode 170,483,122 checks and 0 differences
(generated-code self-checks 0 differences); lockstep 120 of 123 reference images, the same three fade frames missing
as in every run of this build before the module.

**Fever** (test switches `greens+oneorange`: the one orange peg left is the one straight below the cannon): the
game state goes 2 -> 6 when the last orange peg is hit, then 7, 8, 9, 14 through the end of the level. With Jimmy's
power: four green pegs hit during fever each spawned their balls (the 360's rules report a green peg to a power only
in state 2, so the class answers them itself in state 6, as the PS4's does) and each spawn awarded "Multiball
Madness" (25,000), the PS4's rule once fever was seen. In fever the measured ball step is half (0.835): the look-ahead
is off there, as on the PS4 (not while the game itself runs slowly).

**The Switch build** (no LTO, as for every development build): `out/nx/peggle2.nro`, 48,179,397 bytes (11:32; ELF
kept as `out/nx/elf/peggle2_dlc_powers_nolto_20261013_1132.elf`), `tools/check_switch_build.py` passes. Packaged:
`out/dist/switch/peggle2-nx/` (the NRO, with two new configs: `peggle2_test_dlc_log.toml`,
`peggle2_test_dlc_powers_off.toml`) and `out/dist/switch/peggle2-nx-dlc/` (the asset cache of the converted game
folder, 34 files, 1,131,434,416 bytes, checksum verified, with an install note). One NRO runs both the stock and the
converted data. Not run on the console.

### The powers' screen layers and motion (2026-10-13, afternoon; PC)

Two keys, both **default false** (off = the morning's behaviour): `peggle2_dlc_overlays` and `peggle2_dlc_motion`.
Both were seen working on the PC with the keys on; the defaults were left off because the last builds of the day
belonged to the console work.

**What the PS4 draws, read from its code and its compiled shaders** (the PS4's `.POPFX` files hold AMD GCN machine
code; a 150-line disassembler of my own read them, nothing was downloaded):
- `FAIRY_PUNCHOUT` and `JIMMY_PUNCHOUT` are the same file on the PS4 (`cmp`), and the same formula as the 360
  archive's `EFFECTS\FAIRY_PUNCHOUT.FX` source text, with 8 points instead of 5: alpha = saturate(Alpha x min(1,
  (distance to a point - Size) / (2 EdgeSmooth))), distances in units of the image's width and height.
- `FAIRY_MAGIC` as compiled for the PS4 has no texture-coordinate wobble (the 360 source text computes one and never
  uses it; that text also names a field that does not exist): red and blue become 0.25 c + (1 + sin(2 pi (c + Mod)))
  / 8 x 0.75, then all three channels x (1 - Darkness).
- `JIMMY_SPOTLIGHT` (PS4 only): two beams; for each, c = the cosine between (pixel - Anchor) and (Pt - Anchor); where
  |c| > SpotWidth the beam's value is (1 - |c|) / (1 - SpotWidth), else 1; alpha = saturate(Alpha x min of the two).
- Windy (0x1D4F90, 0x1D3D90): the ultraviolet image through FAIRY_MAGIC (Mod = update count / 100, Darkness 0.25)
  into an image of the board's size, that drawn over the board through FAIRY_PUNCHOUT with Alpha 0.65 x the dimming
  curve, Size 0.075, EdgeSmooth 0.01, the points the newest (up to 8) fairies of the current flock.
- Jimmy (0x1C8300): a black image of the board's size drawn twice with Alpha 128 / 255: through JIMMY_SPOTLIGHT
  (SpotWidth 0.99, anchors (0.3, 1.2) and (0.7, 1.2), aims (cos a, 0) and (sin a, 0), a swinging between 0 and pi / 2
  at a radian a second: 0x1C7BC0) and through JIMMY_PUNCHOUT (holes around the first eight balls).
- A fairy's path (0x1D6390): a Bezier (the framework's class, 0x3647E0 / 0x364F90) through a point every 10 units
  of the way, each segment's control points 1 to 2 units off to one side, the side alternating; when a ball knocked
  the fairy off its peg, four points along the ball's direction first (15 units in 0.7 s by a curve). Progress on the
  path goes through the CurvedVal "b;0,1,0.01,1,#-#D         ~~QPU".
- Jimmy's look-ahead (0x1C98B0): a temporary ball stepped through the physics for 6 steps; an unhit green peg it
  touches after the first step, other than the peg of the previous contact, turns the slow motion on.

**Why not the shaders.** The 360 archive ships its effects as HLSL source and the game compiles them at run time,
but the native renderer only draws with shaders that are in its prebuilt library (`out/pc/peggle2_shaders.mesp`);
a newly compiled pixel shader is an unidentified load there. The exact route is: draw through the game's effect
objects (as the PS4 code does), write `JIMMY_SPOTLIGHT.FX` from the formula above, and add the three pixel shaders
to the library (discovery dump, then the library tools). That is renderer work and was not done.

**What `peggle2_dlc_overlays` does instead** (`DrawOverlay`, `app/src/dlc/peggle2_dlc_engine.cpp`): the layer is a
mesh over the board drawn with the game's textured-triangle function (`sub_823BDD98` -> the interface's
`sub_824AD228` -> `sub_824ABFD0`), each vertex with the shader's alpha in its colour (a vertex colour of 0 means
"the Graphics colour" there, any other is taken as ARGB: `824AC13C`). 12 x 8 cells, a cell halved up to three times
where the alpha at its middle or the middle of a side is more than 0.03 off what its corners interpolate. The
function clips every triangle with a polygon clipper unless the Graphics' clip rectangle is the whole target, so the
rectangle is set to the whole target for the call (the mesh is inside the board).
- Windy: the ultraviolet image with the holes, in the renderer's slot 16 (where the PS4 class has it; the game calls
  the slots 14, 16, 17, 15 with the board's corner as origin, then 13 with the screen's: logged in a run). Colour:
  (81, 191, 81) / 255 for the whole image, no cycling.
- Jimmy: both layers as one, alpha = 1 - (1 - spotlight)(1 - punch-out), black. The texture is the ball's sprite
  with every vertex at its centre (`IMAGE_SOLID_WHITE` draws nothing through this function: seen). 1,140 triangles
  with 3 balls, 1,900 to 2,500 with 5 to 8 (log lines "[dlc] overlay"; Windy's count was not logged).
- Seen: `out/ps4_dlc/shots/w_new_seq.jpg`, `w_new_0705.png` (the purple screen, a soft hole following each new
  fairy, pegs above it; taken with the first, uniform 16 x 10 mesh), `j_dim_0440.png`, `j_dim_0500.png` (the adaptive
  mesh: dimmed board, holes at the balls, the beams).

**What `peggle2_dlc_motion` does:** the fairy path as above with the PS4's order of random numbers; Windy's curves
evaluated by the 360 executable's own CurvedVal (`sub_823E6680` makes one from the data string, `sub_823E5EC8`
evaluates it; both read at `sub_82156D90`); Jimmy's look-ahead with contacts: every target a circle the ball's
centre meets at 3.4 units, the velocity along the line of centres turned round times 0.81 (40 bounces logged in a
run: most between 0.80 and 0.84), the side walls the same. Runs with both keys: 16 pegs turned by fairies; 10 ball
spawns, Free Ball Frenzy to 60,000.

### Sound (2026-10-13, afternoon; PC, not listened to)

**Not in the delivered archive.** `build_mod.py --content ...,sound` makes it; the archive with it is
`out/ps4_dlc/game_root_mod_sound/main.rsb` (1,324,552,192 bytes, `selftest.py` passes). `game_root_mod/main.rsb`
is unchanged (rebuilt with the changed builder and the old content list: byte-identical). Every test run is muted,
so nobody has heard it: what is established is what the game does with it, below.

**How the two builds keep sound** (bank version 88 on both):
- Every sound object is in `GLOBAL_DATA.BNK` (360: 23,056 objects, PS4: 31,168); the other banks only carry media.
  The game hands Wwise each bank from memory (`sub_82470168` -> `sub_82520658`, then by name `sub_825221F8`); a
  refused bank is "Could not load SoundBank: %s." in the game's own messages (`peggle2_dlc_log`) and a crash after.
- The 360's media are XMA2 (3,695) and Vorbis (285); the PS4's ATRAC9 (format tag 0xFFFC) and four PCM.
- The 360 executable's Wwise makes its sources in `sub_8252FC18`: codec id 1 (PCM) -> `sub_8256FD78` for a bank's
  media, `sub_82570158` for a streamed file; 2 (ADPCM) -> `sub_825722A0` / `sub_82572898`; 3 = XMA; above 3 the
  registered ones (Vorbis). The PCM bank source's start (`sub_8256FEB8`) wants a RIFX file with format tag 0xFFFE,
  read big-endian, whose data chunk ends the file. So PCM needs no encoder.
- `C:\ffmpeg` decodes ATRAC9 once the Wwise header is rewritten as Sony's `.at9` header (`tools/ps4_dlc/at9.py`).

**Tools** (`tools/ps4_dlc/`): `wwise.py` (every object type of the two banks field by field: walks an object and
writes it in the other byte order), `wwise_check.py` (all 54,224 objects of both banks are walked to their last
byte; the 22,455 both have with equal size convert to the 360's bytes except in fields that hold values: source
records, ids, properties, limits), `wwise_pairs.py` (a pair's field widths, how the layouts were found),
`wwise_merge.py`, `sound.py`, `at9.py`, `audio_index.py`, `audio_extract.py`.

**What the game's sound engine refuses, each found by cutting the bank until it loaded** (5-second boots):
1. an event before one of its actions;
2. containers that are not one tree: mixing the two builds' versions of containers gave 15 children listed by two
   containers (both original banks: 0);
3. an object whose bus is not loaded: the PS4's `INIT.BNK` has 15 busses the 360's lacks.
So the merged bank is the PS4's tree of sounds and containers (the 360's bytes for the 10,179 objects with the same
size, children and parent, the PS4's converted for 3,497 new and 156 changed ones), the PS4's actions and events,
the 360's music objects exactly (9,130), both builds' states and effects; what used a PS4-only bus goes to its
nearest ancestor the 360 has (7 objects), 7 aux sends to such busses are removed; 274 objects only the 360 has are
left out (not in the PS4's tree). 26,279 objects, 4,965,525 bytes.

**Media:** the 923 sources the merged sound objects need, decoded and stored as 16-bit PCM at 24 kHz in the 360's
byte order (197,079,866 bytes): appended to the 360's `GLOBAL`, `BERG`, `BJORN`, `GNOME`, `LORT`, `ULNA` and
`FAERIE` banks, new `JIMMY.BNK` (group `JimmySounds`), `FairyArea.bnk` (`FAIRYAREA_COMMON`) and `Desert.bnk`
(`DESERT_COMMON`), 7 streamed files. `GLOBAL.BNK`'s own media move by 2,048 bytes; the 7,391 source records that
name them by file offset follow. With sound, the two areas name those two groups instead of Bjorn's area group and
Jimmy's master entry names `JimmySounds`. (An area group must be named like the stock ones: an area that named the
360's `FirefieldSoundBank` crashed on entering a level, read of guest 0x1C.)

**Seen on the PC** (log lines "[dlc] sound", the two hooks log only): Windy's area 7, level 1: 150 PCM voices from
banks started, 0 refused (136 from `FAERIE.BNK`, 24 from the area bank); Jimmy's area 8, level 1 on the archive
without test options: PCM voices from `DESERT` (39), `JIMMY` (4) and `GLOBAL`, 0 refused; the whole run without a
load error. A decoded medium read back from the built bank is a smooth waveform (mean step 993 at RMS 6,314).

**Not solved:**
- Jimmy's two power sounds and his music: `Play_Jimmy_Power_SlowDown_Start` and `_Stop` play a music container
  (PS4 type 13), and the PS4's music objects are not merged, because their media are not converted: 533 media, 143
  MB of ATRAC9 (`FAERIE` 70 MB, `GLOBAL` 39, `JIMMY` 25), which as 24 kHz PCM would be about 570 MB in banks the
  game loads into memory. There is no event `Play_Music_Set_G` in the PS4 bank either; Jimmy keeps set A.
- Size and memory: `GNOME.BNK` grows from 35.7 to 58.1 MB, `GLOBAL.BNK` from 35.8 to 52.2, `Desert.bnk` is 46.9;
  the 360's largest bank was 38 MB. Fine on the PC; not tried on the console. ADPCM (a quarter) is in the
  executable too, its block format on the 360 was not read; XMA2 would be smaller still but no encoder is on this
  PC and none was downloaded (the only ones known are in the Xbox 360 SDK and in Wwise's 360 authoring).
- Mix: 156 containers and all actions and events are the PS4's versions, with the PS4's volumes.
- With Jimmy as the master of another area (a test archive) his sounds from `Desert.bnk` are refused (the bank is
  loaded with his area only; the PS4 loads it always).

### Menus looked at (2026-10-13, afternoon; PC, test archive with every unlock)

- Main menu: Single Player, Multiplayer, five icons. The first icon and "Quick Match" answer "Unable to connect to
  Xbox LIVE" (`m2_sheet.jpg`, `m5_sheet.jpg`). The fifth icon is Exit: the process ends with 0xC0000374.
- Adventure: both new areas with portraits; level lists with thumbnails, names and objectives (`m3_0960.png`); on
  the archive without test options Jimmy's area shows its trials and levels 2 to 10 locked (`m8_sheet.jpg`).
  Selecting level 1 of each plays the intro video and the level (`m4_sheet.jpg`, `m8_sheet.jpg`). The level list
  has no costume choice (Y, X, RB and START do nothing there).
- Multiplayer -> Local Duel: the lobby (`m6_0700.png`) offers Bjorn, Jeffrey, Berg, Gnorman, Luna and "random":
  **the two new masters are not offered**. Y is "Change Costume" and changes the costume of the master in place
  (Berg's second one seen, `m7_0620.png`); the six entries were not stepped through. The lobby waits for a second
  controller, so the level list and a match were not reached.
- Not opened: the results screen of the new areas, trials, the costume cycle of the new masters.

**Still open:**
1. (done: see "Unlock rules" above)
2. Sound: in the delivered archive still as before (both new masters on the 360's empty `WendySounds` bank, Bjorn's
   area bank, Jimmy's music set A, his two power events silent). See "Sound" above: voices, effects and ambience are
   converted in an optional archive nobody has heard; Jimmy's power sounds and music need the PS4's music objects
   and a codec smaller than PCM.
3. The multiplayer lobby does not offer Windy and Jimmy (it lists five masters and "random"); the costume cycle of
   the new masters, the results screens and badge art of the new areas and the trials list are unseen.
4. `MULTIPLAYER.JSON` (22 PS4-only level entries) is not merged.
5. The stand-ins and test options are listed in `build_mod.py`; none may be in a delivered archive.
6. The console: not built or run with this module yet (texture memory with the larger archive, the thunk pool);
   the two keys of the afternoon and the sound archive (larger banks in memory) have not run there either.
7. The two keys `peggle2_dlc_overlays` and `peggle2_dlc_motion` are off by default: turn them on once a build of
   one's own can be tested (one word each in `app/src/dlc/peggle2_dlc.cpp`, or in a toml).


## Console: large atlases partly garbage (2026-10-13; cause found, fixed on the console)

First console run that drew anything (NRO of 11:32, the converted archive as asset pack, `peggle2_002.log`,
`rex_profile (12).log`, four screenshots): fonts, pegs and the HUD are right; the title screen, the main menu and the
level background show garbage in parts of their atlases, the same from frame to frame. The PC draws the same archive
right. No crash, no error line, natives and forms report no difference.

**What the logs and the code say (evidence in brackets):**
- Every atlas that shows garbage is a stock packet whose textures are zlib-compressed (BOARD_720 7.29 MB -> 41.9 MB,
  TitleScreen_720, InterfaceMainMenu_720, BG_PEGGLEGROUNDS_LEVEL01_720...; `map_reads.py` over the `[io] SLOW` lines).
  The game inflates them itself: the main thread reads 256 KB pieces into one buffer and a worker thread
  (`sub_8248F7E0`) inflates into a 1 MB staging buffer that the main thread copies into the texture (PC run with the
  diagnostic: `[texdiag] job` lines; the texture memory is in the 0xE0000000 physical view, addresses like 1CEB3000).
- The renderer converges to guest memory: one "stable texture changed" line in the whole log, fingerprint sampling
  off, no deferred copies, no upload-buffer overflow, the staging buffer is flushed whole before each submission
  (`BeforeSend`), the texture pool hands out exact non-overlapping ranges (`masseffect_native_textures_pool.cpp`).
  The textures are "order 0": no byte swap is involved.
- Guest memory on the console: 40 view faults and no emulated access in the whole run (profile), the heap tracker
  reported no overlap or bad free (0 of each in the log), direct pack reads that the file system refuses fall back to
  a host buffer correctly, the 0xE0000000 view and `TranslatePhysical` agree (+0x1000) in every path read.
- The enlarged garbage is whole BC3 blocks: zeros, flat blocks whose colour is what a 16-bit endpoint with one byte
  zero or both bytes equal gives, index bytes in regular patterns, and fragments of real artwork out of place. That is
  what misplaced or partly missing decompressed data looks like, not a wrongly decoded format.
- One concrete difference found in the console binary (objdump of the 11:32 ELF, `sub_82490160`, the routine that
  hands a request to the worker): GCC stores the request flag (`job+8 = 1`) BEFORE the output pointer, the output
  size and the "new input" flag, where the game's code stores it after them. The worker only reads the job after the
  event that follows these stores, so by the protocol this is harmless; it is recorded because it is the first place
  to look if the diagnostic shows a request served with the previous request's output.
  The worker's own stores (`produced`, then `request = 0`, then `done`) are in the game's order.

**Not found by reading.** So the build of 13:31 carries data checks, off by default (`peggle2_diag_textures`,
`io_diag_crc`, `pack_log_fallbacks`; configs `peggle2_test_texdiag.toml`, `..._verify.toml`, `..._safe.toml`):
a CRC-32 of every file read of 4 KB or more as the guest got it, of every inflated stream as zlib checks it (with the
CRC at every megabyte), each request of the worker, and of every texture of 1 MB or more as the renderer picks it up
(whole and in 64 slices), plus the first six large textures written to `texdiag/`. `tools/texdiag_check.py` compares
all of it with the archive. PC run with the keys on (`peggle2_210.log`, title -> menus -> level): 607 reads, 21
streams and 12 textures, all equal to the archive.
Also new: `pack_direct_reads = false` sends every pack read through a host buffer (the file system then never writes
guest memory); `peggle2_test_texdiag_safe.toml` turns that on together with the texture pool off and the game's own
memcpy/memset, as a second run if the first one's log does not settle it.

The 13:31 build's `[texdiag] job` lines print zlib's `next_out`, which only moves after both parts of a flush, so
nearly every flush gets a line and its "not at the start" remark means nothing by itself; the place a part goes to is
`out + out_size - avail_out - length`.

### What the data checks said (`logs3`, console run with `peggle2_test_texdiag_verify.toml`)

`tools/texdiag_check.py` on `logs3/logs/peggle2_004.log` and `logs3/texdiag/`: 163 file reads equal to the archive; 11
inflated streams equal; of 4 textures of 1 MB or more, Fonts_720 equal and three DIFFERENT in guest memory before the
renderer reads them (InterfaceCommon_720 from byte 1,822,464, TitleScreen_720 from 1,233,920, InterfaceMainMenu_720
from 921,472, each wrong to its end). CPU verify mode: 31,248,714 native checks, 0 differences; the picture is the
same in every mode and the same as in the 11:32 build (`3.jpg` against `5.jpg`: the garbage is pixel-identical). So:
deterministic, in the copy from the decompress worker's 1 MB staging buffer into the texture, and not in anything a
toml key switches.

From the break on, 86 % of the 16-byte units of `texdiag/tex04_...bin` hold one repeated byte and 92 % of 32-byte
windows hold at most two values; about one 128-byte line per megabyte is right. That is a vector permute whose index
is the same in every lane. The main thread copies each staging chunk with the game's streaming copy; for a source that
is not 16-byte aligned that is `sub_8260E9C0` (and its twin `sub_8260E164`): 65 `lvx128`, one `lvsl v0`, 72 `vperm128
vN,vN,vN+1,v0` (64 per 1024 bytes in the main loop, 8 in the tail loop), `stvlx128` stores, `dcbzl` 512 bytes ahead.

### Cause: GCC's early register allocation pass (`-mearly-ra`), devkitA64 GCC 16.1.0

`aarch64-none-elf-g++ -O2 -Q --help=target` prints `-mearly-ra=all` (`none` at -O1): a pass of the AArch64 back end
(dump `320r.early_ra`) that allocates vector and float registers before the general allocator, made for the lookups
that take two or more table registers in consecutive registers. The NEON form of the VMX permute
(`simde_mm_perm_epi8_` under `REX_VMX_HELPERS_NEON`, `sdk/include/rex/ppc/intrinsics.h`, the CPU round of 2026-10-12)
is such a lookup: `uint8x16x2_t` + `vqtbl2q_u8`.

Evidence, on `sub_8260E9C0` compiled alone with the build's own command (`tools/switch/compile_one_function.py`):
- The optimised GIMPLE (`-fdump-tree-optimized`) is right: 73 byte-reversal permutes, 72 two-register lookups, the
  `& 31` and `^ 15` constants, no undefined value but the two parameters.
- `-fdump-rtl-all` (reference form): the number of table-lookup patterns is the same in every dump up to and
  including `320r.early_ra`; in the next one (`321r.sched1`), right after the pass has assigned its registers, it is
  the final 107: 38 short of the 145 in the GIMPLE, and it stays there.
- The final code with the pass on: the register that holds the byte-reversal constant is loaded once before the loop
  and then overwritten by a data load inside it (`ldr q30, [x1, w15, uxtw]` ... `tbl v30, {v30}, v30`, and the loads
  after it are "reversed" with that); the 0x1F and 0x0F constants of the permute index are never set on the 1024-byte
  path (`and v19, v19, v28; eor v19, v19, v29` with two data registers).
- `tools/switch/check_lookup_constants.py` (a data-flow over the assembly: every one-register lookup must be indexed
  by the reversal constant, every two-register lookup by `(lvsl & 0x1F) ^ 0x0F`, on every path):

  | compiled how | tbl instructions | lookups with a wrong index |
  |---|---|---|
  | by-value vector locals, -O3, pass on (the builds that ran on the console) | 97 (71 two-register) | 90 |
  | by-value, -O2, pass on | 130 | 121 |
  | vector registers as references, -O3, pass on (the first attempted fix, built 14:44) | 107 | 99 |
  | by-value, -O3, `-mearly-ra=none` (the build of 15:15) | 157 (72) | 0 |
  | references, -O3, `-mearly-ra=none` | 152 | 0 |
  | by-value, -O2, `-mearly-ra=none` | 152 | 0 |
  | references, -O1 (pass off by default) | 145 | 0 |
  | by-value, -O3, pass on, `-fno-tree-sra` | 97 | 97 |
  | registers as references (`-DREX_LEAF_LOCALS_OFF`), -O3, pass on | 107 | 99 |
  | twin `sub_8260E164`: pass on / off | 97 / 159 | 90 / 0 |

- Without the two-register lookup (the permute written as `vqtbl1q_u8` + `vqtbx1q_u8`: 145 one-register lookups and
  72 `tbx`) the assembly is identical with the pass on and off, in the by-value and in the reference form: in this
  function the pass only acts through the two-register lookups.
- Clang (the PC build) compiles the same source right, so no PC test could see it; verify mode compares fast paths
  with originals built by the same compiler, and the streaming copy is the original.

**A wrong turn, recorded.** The first reading blamed the by-value vector locals of the leaf-locals step
(`tools/leaf_locals.py`: `PPCVRegister l_v0 = ctx.v0;`, written through a pointer cast) and GCC's scalar replacement,
because with `-DREX_LEAF_LOCALS_OFF` the function referred to the lvsl table again. That was one symptom of
several going away: by the data-flow check that form still has 99 lookups with a wrong index, and a full rebuild with
vector registers kept as references (14:44) failed `tools/check_switch_build.py` on both functions. The check then
was a count of references to the table, which is why it passed. The other port sessions were told the first reading
and, 25 minutes later, the correction. The locals are by value again (the measured form).

### Fix: the pass is off for everything built for the console

- `app/CMakeLists.txt`, right after the language standard and before the SDK is added: `add_compile_options(
  -mearly-ra=none)` and `add_link_options(-mearly-ra=none)` for the Switch with GCC 14 or later (the link line because
  an LTO build generates its code there).
- `tools/check_switch_build.py` (run before packaging) has three more checks: no `tbl vA, {vB}, vB` in a recompiled
  function (the tell-tale; it is what caught the failed first fix); every compile command and the link in
  `build.ninja` carry the flag; the two streaming copies compiled alone give every lookup its constant.
- Build of 15:15 (`out/nx/elf/peggle2_earlyra_none_nolto_20261013_1515.elf`, no LTO, 711 steps: the SDK was rebuilt
  too): all checks pass: 681 of 681 compile commands and the link have the flag, 0 self-indexed lookups, 0 wrong
  indices in 157 and 159 lookups.
- Cost, static, all 33,670 recompiled functions against the 13:31 build (`tools/switch/compare_elf_functions.py`: a
  hash of each function's mnemonic sequence): 660 functions differ, 4,397,819 -> 4,396,848 instructions (-971,
  -0.022 %). Of the 64 functions with two-register lookups 20 changed; the general allocator cannot keep a table pair
  across lookups that overwrite one of its registers, so it repeats the byte reversal and adds moves there
  (`sub_82519460` 154 -> 168 instructions, `sub_82518CB8` 156 -> 180, `sub_825AD4D0` 354 -> 388), and elsewhere it
  merges duplicated tails the pass had kept apart (`sub_82899100` 3138 -> 2592). No run-time measurement.

- PC exe rebuilt from the same tree (15:23; clang, which the flag does not concern): `mod_test.ps1` with the data
  checks on (`peggle2_257.log`): 12 textures of 1 MB or more, all equal to the archive; no DIFFERENCE or MISMATCH line.

**On the console** (the user's run of the 15:15 build, a photo of a level): the atlases are right. Two smaller faults showed then, with other causes: the next section.

**Not shown.** The logo videos, which were right only in the verify
configs in the `logs3` runs, are not explained; that run is their test too. Other functions in the earlier
builds, by sa2-nx's yardstick (a `vperm` statement has no encoding but a two-register lookup): of the 57 functions
with `vperm` statements (1,004 statements) the 13:31 ELF has fewer two-register lookups than statements in exactly
two, the streaming copies (71 for 72 each); the 15:15 ELF in none (1,027 lookups). So nothing else shows the loss,
which is evidence and not proof for the other 55 (of the six others whose one-register count changed, one was read,
`sub_82519460`: both forms are the same program). sa2-nx found the same damage in its two copies when its source is
compiled per file with the pass on; its released LTO + PGO builds escaped it. The NVK driver and the
other prebuilt portlibs are built elsewhere with their own flags.

## Console: a frame of the previous level at a level change, spots in videos, stuttering audio at the start (2026-10-13; fixes on the PC, console run pending)

Seen on the console with the 15:15 build: for one frame when a cleared level gives way to the next, rectangles of the
background show other artwork; and every video has small spots while it plays (green and black in the two new
masters' videos, yellow and blue in the logos). The videos were clean in the verify configs of the earlier runs. Both
are the renderer reading guest memory at another moment than a console GPU would.

### Level change: a reloaded atlas waited for its recheck interval (`peggle2_gpu_lock_rechecks`, non-CPU, on)

- The renderer keeps a copy of every texture and reads guest memory again only when a per-texture interval runs out:
  1 frame after a change, doubling to 32 (plus 0-7) while the bytes stay the same (`PrepareTexture`,
  `masseffect_native_texture_interval_max`, whose own text says a game that "streams new texel data into an
  already-sampled address" shows the old content that long).
- Peggle 2 loads the next level's atlas into the memory the last one had. PC log with `peggle2_diag_textures`:
  `ME stable texture changed at 161AF000 2048x4096 format 20 after interval 32` (the title screen's atlas address taken
  by the area select's). A level's background is drawn from pieces of its atlas, so the old image under the new
  layout is exactly "rectangles of other artwork"; the pieces both levels pack at the same place look right.
- What the game says about it: nothing the renderer could use. Its Direct3D lock (`sub_82617A50`) blocks on the
  resource's fence and then, by the code, either names the locked range to the GPU (COHER_STATUS/SIZE/BASE_HOST and
  a wait) or, when the global at 0x82D75098 is 0, only sets the device's "texture cache invalid" flag (+12256), which
  `sub_826238F0` turns into registers 05C8 / 0E00 / 0E40 at a later draw: no address. In the ring trace of the PC
  route the only ranges are a 4 MB area (`1EFA0000+4194304 (status 01000000)`) and `00001000+0 (status 00010000)`;
  none ever matched a texture of the renderer in 70 s. The global itself was not read.
- The fix takes the note where the game writes: the loader's row copy (`sub_8245E890`: LockRect, the streaming copy,
  UnlockRect; reached through a vtable, so its function table entry is replaced, `app/src/peggle2_texture_writes.cpp`)
  reports the texture's address after every call; the ring thread takes the reports before its next draw and marks the
  textures at that address to be read at their next use (`DrawsVulkan::NoteGuestWrite`). Direct3D's range message is
  honoured the same way for a build that sends it.
- PC, the route title -> menus -> level: `the loader wrote the texture at 161AF000: 1 texture(s) of the renderer read
  again at their next use`; 13 such reports matched a texture the renderer had; 12 large textures equal to the
  archive; no DIFFERENCE or MISMATCH line. **Not shown on the PC: the stale frame itself.** On this route the reused
  atlas is first drawn 13 frames after its load and was read again then with the key on and with it off (0 frames
  drawn from the old copy in three runs); the console's level change is the case that hits the interval, and no PC
  script clears a level.

### Videos: the plane textures were read while Bink was decoding into them (`peggle2_gpu_video_after_draws`, non-CPU, on)

- Bink 2 decodes on worker threads straight into its plane textures: two sets of three k_8 planes (1280x720 with two
  640x360, pitch 768; the two new masters' intro videos, taken from the PS4, are 1920x1080 with two 960x540). A
  write trap on the PC (`peggle2_diag_plane_trap_ms`: the plane read-only in all three views of its memory, a
  first-chance handler ahead of the SDK's, which would quietly undo it) named the writer: `sub_8289C180`, eight
  `dcbzl` that zero the cache lines of a block, called from Bink's block decoder `sub_828A0380` on a worker thread
  (`sub_8288B078`, the async worker loop, calls `sub_82889358`); 80 of 80 trapped first writes went to the set that
  is not `Frames[FrameNum]`.
- The game's video update, read from the code (`sub_8248CA60`, a virtual of its video handle; +32 the BINK, +76 its
  texture sets, RAD's BINKFRAMEBUFFERS then the textures: `FrameNum` at +20, the Y textures at +136 and +168):
  `BinkWait` (`sub_82889DA8`); if a frame is due and `BinkDoFrameAsyncWait(bink, 1000)` (`sub_8288B358`) says the
  last decode is done: `BinkNextFrame` (`sub_82889760`), the skip loop, then `BinkDoFrameAsync` (`sub_8288B1C8`),
  which starts the next decode. Only after that it calls its vtable +100, which is
  `D3DResource_BlockUntilNotBusy(textures[FrameNum + 1])` (`sub_824575F8` -> `sub_8248E290`), and +104. So the
  decode is started first and the wait for the GPU comes second.
- One microsecond clock (PC, `peggle2_294.log`): the wait returns at 1,666,568 us; the renderer has read plane
  1C060000 for the frame the game then submits by 1,667,019; the next update's decode writes that same plane first
  at 1,667,052. Every frame: one set is drawn, and the decode started right after the submission goes into that
  set. On the console the GPU has sampled the planes by then; the ring thread here was still fingerprinting and
  copying them. PC, the logo videos: 920 of 1,013 plane reads had other bytes after the copy than before it, and the
  copy held zeroed 16-byte runs where the plane was white a moment later (`[row 384 x 1024: 00 -> FF]`). Zero luma
  is black and zero chroma green: the console's bars and spots (the user's capture of Windy's intro). Verify mode
  slows the game thread enough for the read to win (PC: 3 of 901 reads changed): why the verify configs were clean.
- Two designs that did not hold on the console, both removed:
  1. reading the plane again when the frame's submission is closed, expecting the finished new frame (19:05 build,
     `logs4`: `1802 uploads ... 1650 had changed since the draw; still being written then: 4024 copies repeated, 348
     given up after 12 ms`: the decoder was usually still at work, the bars stayed);
  2. reading planes at the swap and letting their draws use that copy (19:53 build, `logs5/peggle2_001.log`: `3605
     copies taken, 1504 dropped because the bytes changed under the copy, 4325 draws used a plane as read at a
     swap`: with the 1080p video decoding longer than a frame, a set was often mid-decode at the swap, and a copy
     taken in a quiet moment of a running decode passed for a still picture; the bars stayed).
- The fix: the update waits, before anything else, until the ring thread has recorded every draw submitted so far
  (`me::native::WaitRingRecordedDraws`: the ring has taken a batch that includes the game's last write-pointer
  submission and is idle or asleep in a WAIT_REG_MEM; the flip wait ends a frame's draws), at most
  `peggle2_gpu_video_after_draws_us` (20 ms). `sub_8248CA60` has no direct caller, so its function table entry is
  replaced (`app/src/peggle2_texture_writes.cpp`). The renderer reads planes at the draw as it always did.
- PC, the route title -> menus -> level, key on against key off: 895 plane reads, 0 changed during the read,
  against 1,013 reads with 920 changed. `video update held for the ring: 300 updates, 2.21 ms on average, 7.44 ms at
  most, 0 gave up` (with the data checks on, which add a fingerprint and a compare to every read). 12 large textures
  equal to the archive, no DIFFERENCE or MISMATCH line.
- Not shown: the console. The hold is time the decode starts later each video frame; on the console the 1080p
  intro videos already run at 23-25 frames a second, and the ring's time for a plane set there was not measured (the
  log line above reports it).

### Startup audio stutter: the heap tracker of the boot-crash hunt was still on (`peggle2_debug_heap_track`, off)

- The user: the sound stutters for a while after start-up. `logs4/peggle2_006.log` has 4,900 `[heap] #...` lines in
  1.6 s and operation numbers past 1.8 million within 13 s of the start: the tracker of `peggle2_debug_hooks.cpp`
  (hooks on RtlAllocateHeap / RtlFreeHeap / RtlReAllocateHeap, written for the console boot crash of 2026-10-05,
  whose cause was the network start-up) ran in every build: one process-wide mutex and a `std::map` node (a host
  malloc or free) per heap call of the game.
- Console profile of the first 20 s, symbolized against the 19:05 ELF: main thread 231 samples, of them
  `heaptrack::NoteAlloc` 6.1 %, `NoteFree` 4.3 %, `mutexUnlock` 3.5 %, `mutexLock` 1.7 %; Wwise's event thread
  ("AK::EventManage") 28 samples, of them `_free_r` 25 %, `_malloc_r` 18 %, `mutexLock` 18 %, `mutexUnlock` 11 %.
- Now behind `peggle2_debug_heap_track` (default false): the three hooks call the original at once. PC route:
  0 `[heap]` lines (5,090 before), textures and checks as above. **Not shown**: that the stutter is gone on the
  console (the hooks are the measured cost; nothing else was looked for).

**Console, 19:53 build** (`logs5`): no `[heap]` line in the log (the tracker is off); the user reports the video corruption unchanged and nothing about the audio or the level change.

**Build**: Switch NRO of 20:37 (`out/nx/elf/peggle2_videohold_nolto_20261013.elf`, no LTO), `tools/check_switch_build.py` passes. **Not shown**: the video fix and the level change fix on the console.

## Docked 1080p and HD atlases from the PS4's 1080 art (2026-10-13, evening; PC only, console run pending)

The user's request: render above 720p when docked, and use the PS4 release's 1080 art (base game and the PS4
content) instead of the 720 art. Two independent parts; both are keys, both are on by default.

### Render resolution (`peggle2_gpu_render_scale`, non-CPU, on)

| Setting | Default | |
|---|---|---|
| `peggle2_render_height_handheld` | 720 | the game's own 1280x720, the same commands as before |
| `peggle2_render_height_docked` | 1080 | 1920x1080; any multiple of 180 from 360 to 2160 |
| `peggle2_render_docked`, `bind_peggle2_docked` (F9) | false | PC: start in / switch the simulated docked mode |
| `peggle2_render_test_toggle_at`, `_every` | 0 | PC tests: switch at presented swaps |
| `peggle2_render_half_pixel_guest` | false | comparison: sa2-nx's half-pixel rule (below) |

- sa2-nx's change of 2026-10-07 (its docs/enhancements.md, "Render resolution and MSAA"), ported: every render
  target, depth target and resolved texture is a VkImage q / 4 times the guest size (q = height / 180); guest sizes
  stay guest units; Vulkan commands go through `masseffect/me_resolution_scale.h`; the EDRAM transfer shaders take
  the tile size (20q x 4q) as a push constant; `RescaleAll` moves every image to the new scale at the end of a Swap
  (`me/peggle2_render_mode.cpp`: libnx's operation mode with Reverse-NX obeyed on the Switch).
- How: both renderers diffed with sa2's names mapped to this port's; of the hunks that touch the scale, 61 of 62 in
  the targets file, 18 of 22 in the draws file and 3 in the header were applied as exact one-match replacements,
  each MSAA line taken out by rule; 26 shader sources changed (a script that refuses any changed line not about the
  tile size). Not ported: sa2's MSAA option and its wider lines. A cross-check of every sa2 line with a scale word
  against this port's files afterwards found one line the hunk filter had missed (the compute dispatch of the
  guest-space depth resolve used the guest rectangle); fixed.
- **One rule differs from sa2-nx: Direct3D's half pixel is half a HOST pixel.** With sa2's guest half pixel, the
  1080p picture had a dark top row: 835 of 1,920 pixels of row 0 (`out/hd/shots/s1080_0000.png`); with half a host
  pixel 0 (`s1080b_0000.png`), and 0 at 720p either way. Peggle 2 draws its layers as screen-aligned quads at integer
  coordinates: a quad from y = 0 lands at 0.75 host pixels with sa2's rule (row 0, centre 0.5, uncovered), and a quad
  parked just above the screen (bottom edge 0) reaches into row 0. Half a host pixel keeps every integer guest edge
  on the side of the pixel centres it has at 720p.
- **PC** (AMD integrated GPU; lockstep runs to the first level, `peggle2_304.log` to `_308.log`): docked, 1920x1080
  output, `native: 71250 drawn, 0 rejected`, images 71.9 MB against 32.6 MB at 720p. 25 switches in one run (every
  97 presented swaps): `11 images recreated (6 with their content scaled, 0 cleared, 0 failed)` in 9.5 to 17.0 ms
  each. A 720p frame recorded after six switches against the same game frame of a run that never switched: no
  differing pixel outside the animated character (16x9 grid of difference counts, `out/hd/shots/diff_s720_tog.png`).
- **Not shown:** the console (speed at 1080p above all). That 720p frames are bit for bit those of the build before
  the change: q = 4 makes every conversion return its input and the shaders get 80 and 16, but the frame comparison
  was not conclusive (lockstep hashes against the baselines of 02:15, which predate today's other changes: 38 of 63
  reproducible images found, the missing ones in two consecutive runs of frames; only 63 of 400 frames reproduce
  between the two baseline runs themselves).

### HD atlases (`peggle2_gpu_hd_atlases`, non-CPU, on; `peggle2_hd_atlases` = 0 never, 1 above 720p (default), 2 always)

- The game cannot load the 1080 groups itself: its executable names 191 groups ending in `_720` (UTF-16) and none in
  `_1080`, and the atlas layout of the PS4 differs. So the renderer swaps textures: `tools/hd/build_hd_pack.py` makes
  for every 720 atlas an image 1.5 times the size with the same layout and files it under the XXH3-64 of the 720
  atlas's bytes; `PrepareTexture` already takes that fingerprint of every texture (the atlases are format 20, linear,
  byte order 0, one level: the archive's bytes) and uploads the pack's image when it has one
  (`me/peggle2_hd_pack.cpp`, `masseffect/masseffect_native_draws.cpp`). The shaders keep getting the guest size.
- **Builder.** Base = the 720 atlas enlarged (Lanczos on colours multiplied by alpha). Each sprite with a PS4 sprite
  of the same id: the PS4 rectangle resampled whole onto the 1.5x rectangle with 1.5 texels of its edge around it
  (rings written first, interiors second: rectangles overlap in some atlases).
  - Whole rectangle to whole rectangle is the mapping the art has (the earlier agent's measurement on three
    groups, mean squared error x 1e4 of the PS4 sprite scaled to 720 against the 720 sprite, sizes within half a
    texel of 1.5x: stretched 1.76; exactly 1.5 anchored top left 5.55, centred 2.96, bottom right 5.97).
  - The two releases number same-sized pieces of an animation differently: `BJORN_121X231_3` here is the PS4's
    `BJORN_121X230_3` (10.0 dB against its namesake, 35.2 dB against that one; nine such in Bjorn's default costume
    of 9 to 15 dB, each with a partner of 30 to 44 dB). A sprite under 30 dB is compared with the PS4 sprites of the
    same size in its group and takes one that is 3 dB better (150 sprites); under 20 dB the 720 sprite stays (215;
    1,182 more have no PS4 sprite of their id: font pages, interface pieces).
  - First pack: coloured fringes and broken edges in the frames (`out/hd/shots/cmp2_seam.png`, `cmp2_mane.png`,
    middle panels). Two causes, both fixed: colours resampled on their own pulled in what lies under transparent
    texels a few texels out (now premultiplied, then transparent texels take the nearest opaque colour, a pull-push
    fill); and Pillow's DXT5 alpha: 34.5 dB on Bjorn's HD atlas, 48.3 dB with the alpha half of `tools/hd/bc3.py`
    (two layouts per block, chosen by error). Colour blocks are still Pillow's (37.9 / 39.1 / 37.6 dB R / G / B on
    that atlas, transparent texels included).
  - Result (`out/hd/pack2`, from `out/ps4_dlc/game_root_mod/main.rsb`, so stock and PS4 content): 354 atlases,
    29,770 sprites from the PS4, 5,631,492,096 bytes of BC3 in 835,304,412 (LZ4 blocks of 256 KiB), read back and
    hashed by the builder; 27 minutes with 4 processes. 102 of the atlases belong to Windy, Jimmy and their areas.
- **Renderer.** A new texture of atlas shape is fingerprinted before its image is made, so the image is created at
  the HD size (no second image). When the content at a key changes, or HD goes on or off (mode 1 follows the render
  scale: docked / handheld), a texture whose image has the other size is dropped after a GPU drain and made again.
  One HD upload per draw fits the 64 MB upload buffer (the largest is 36 MB; a second one in the same draw waits for
  its next use). A pack that fails to read is closed and everything returns to the game's atlases.
- **PC:** docked, to the first level (`peggle2_311.log`): 12 atlases from the pack, 239 MB (106 MB as 720), 12.0 ms
  each to read and unpack, `0 rejected`; frames beside the 720-atlas ones: `out/hd/shots/cmp_castle.png`,
  `cmp_horse.png`. Switching every 97 swaps with mode 1 (`peggle2_310.log`): 23 rescales, 75 HD uploads, 143 textures
  made again, no error; the 720p frame afterwards equals the plain run's outside the animated character. Mode 2 at
  720p (`peggle2_312.log`): 12 atlases from the pack, 0 rejected.
- **Not shown:** the console (memory with several areas visited, load pauses from the SD card, speed); Windy's and
  Jimmy's areas with the pack (their atlases are built and verified, not seen in a frame); more than one HD atlas new
  in one draw (0 uploads put off in these runs).
- Delivery: `out/dist/switch/peggle2-nx-hd/` (the pack and a README), NRO of 21:48
  (`out/nx/elf/peggle2_hd_nolto_20261013_2148.elf`, no LTO, `tools/check_switch_build.py` passes), five configs
  `peggle2_hd_*.toml`.

## Windy's and Jimmy's music, the full sound archive, language, licence, controllers (2026-10-13, night; PC, console pending)

### Music of the two new masters (`build_mod.py --content ...,music`, `tools/ps4_dlc/music.py`)

- The user: no music on Windy's stages. Cause, from the banks: the music is one tree in `GLOBAL_DATA.BNK`; five
  switch containers (976670871, 766558537, 1003502939, 255188718, 583956619) pick a master's branch by a state whose
  values are the masters' keys of MASTERS.JSON (FNV-1 of "bjorn", "lort", "berg", "gnorman", "ulna", "fairy"; the
  PS4's also "jimmy": `out/ps4_dlc/scripts/music_keys.py`). The 360's containers have the Fairy key but no branch
  under it (6 children against the PS4's 8). The six `Play_Music_Set_A..F` events are the same two actions in both
  banks (they start the music, they do not pick a set), and neither bank has `Play_Music_Set_G`: Jimmy's set A was
  never a stand-in, his levels had no music either (PC recording of the delivered archive: -46 dB between effects).
- The PS4's two branches: Windy's 444 tracks on 218 media of its `FAERIE.BNK` (3,811 s of ATRAC9), Jimmy's 288 tracks
  on 101 media of `JIMMY.BNK` and 14 of `GLOBAL.BNK` (915 s). The 360's `FAERIE.BNK` is 2,032 bytes (empty).
- `wwise_merge.merge(sounds=False, music=...)`: the 360's objects, the PS4's version of exactly the containers above
  a new track with a wanted medium (the five), everything their new children list, the PS4-only states those hold.
  Result: 23,051 of the 360's 23,056 objects byte for byte (the five containers change; with the first rule, "any
  medium of FAERIE.BNK", also five playlists under Gnorman's branch that name a copy of a shared medium: now a medium
  counts only when no other PS4 bank holds it), 4,590 new objects, one tree (`tree_report` 0 0 0 0), 0 sources
  without media.
- Media: 464 files decoded with ffmpeg and stored as 16-bit PCM at 32 kHz in the 360's byte order, as STREAMED files
  (536,762,966 bytes; in a bank they would be memory: the 360 holds its music as XMA, a tenth the size, and no XMA2
  encoder is on this PC). The converted tracks get 300 ms of look-ahead (the 360's own streamed music tracks have
  100 to 600; in-memory ones 0).
- **PC** (Windy's and Jimmy's level 1, scripted; the game's mix recorded, below): Windy 152 PCM stream voices started,
  Jimmy 117, the delivered archive 0 in both; loudness per 5 s -25 to -30 dB with the music against gaps of -36 to
  -50 dB without; 9 of the first 14 media the engine started correlate with the recording at 0.25 to 0.57
  (normalised cross-correlation over 3 s; 0.05 to 0.16 against the control). **The user listened to both recordings:
  "the windy level 1 with music is perfect", "music sounds good it works well" (Jimmy).**
- Packs: `out/dist/switch/peggle2-nx-dlc-music/` (1.67 GB: the delivered pack plus the music) and
  `out/dist/switch/peggle2-nx-dlc-full/` (1.87 GB: with `--content sound` too). All 415 textures of both archives are
  byte for byte those of the delivered one, so the HD pack's keys hold.
- **Not shown:** the console (a dozen PCM streams read from the card while a level plays; the look-ahead's feel).

### The full sound archive (`--content ...,sound,music`)

- Jimmy's voice and power sounds, Windy's voice (the 360's `FAERIE.BNK` is empty: she had none either), the two
  areas' ambience: `sound.py` as of the afternoon, now with media over 400 KB of PCM as streamed files
  (`STREAM_OVER`): `Desert.bnk` 46.9 -> 9.1 MB (five ambience loops were 32.9 MB), `GLOBAL.BNK` 52.2 -> 46.4,
  `GNOME.BNK` 58.1 -> 50.5; 56 streamed files, 102.8 MB. `music.py` then merges the music into the bank `sound.py`
  made (`mod.sound_bank`).
- PC, Jimmy's level 1 with his power forced (`peggle2_dlc_test=greens+oneorange`): 350 PCM voices from banks started,
  0 refused, 121 PCM stream voices, no load error; recording at -19 dB. **The user listened: "Yeah sounds good".**
- **Not shown:** the console; memory there (GLOBAL 46 MB against the 360's 36, GNOME 51 against 36, FAERIE 30 and
  JIMMY 15 against none).

### Recording the game's mix in a muted test run (`audio_dump_output_s`, SDK, PC)

- Test runs are muted; the SDL driver zeroed the frame before the dump saw it. Now a dump records what the game mixed
  and the device still gets silence (`sdk/src/audio/sdl/sdl_audio_driver.cpp`).
- In these runs the driver hands the device three frames for each one the game mixes: the dump is 512 samples of
  data, 1,024 of zeros, and so on (5,627 frames per 10 s where 1,875 are real time), so `audio_dump_output_from_s` and
  `_s` count three times real time. `out/ps4_dlc/scripts/wav_degap.py` takes the all-zero frames out (the joins are
  as smooth as the inside of a frame: mean step 0.00530 against 0.00528). Why the driver pulls three times as many
  frames was not looked into (nobody listens on the PC).

### Language, licence, controllers (NRO of 23:39, `out/nx/elf/peggle2_input_lang_nolto_20261013_2339.elf`)

- `XGetLanguage` returned English whatever `user_language` said; it now returns the setting.
  `peggle2_language = "en" | "de" | "fr" | "es" | "it" | "pt"` sets it by name (`app/src/peggle2_app.h`). PC,
  `peggle2_language=de`: the level's panels read KUGELDEPOT and MULTIMETER (`out/ps4_dlc/shots/lang_de_sheet.png`).
  The other four were not run.
- Trial: the game is the trial version when the licence mask's bit 0 is clear (`sub_8242F050`); the SDK's default is
  0. The app's defaults now set `license_mask = 1` when no line does; every config already had the line.
- Controllers: sa2-nx's Switch driver in place of this port's (`sdk/src/input/switch/switch_input_driver.cpp` and
  its header, `OnDevicesAssigned` in the input system): one slot per controller (handheld, No1 to No4), face buttons
  by position by default (`input_xbox_layout = true`), single Joy-Cons sideways, the controller applet, and a button
  map per player, here for all four (`input_p1_*` to `input_p4_*`; sa2-nx has two). **Not run** in this port: the
  code builds for the Switch; the PC uses the SDL driver.

### Windy and Jimmy in the multiplayer lobby (not done; what is known)

- The PS4 offers them: its executable's multiplayer code lists seven master names where the 360's lists five
  (`sub_821BDA70`, the Peg Party mini board) or six, and its archive has `IMAGE_MASTERS_JIMMY_MULTIPLAYER_LOBBY_BUTTON_*`
  and Wendy's (40 ids each) that the 360's lacks.
- The 360 lobby (`sub_82206AC0`, MultiplayerLobbyLayout) makes one `MultiplayerMasterButton` (`sub_82203FD8`:
  "POPANIM_" + the master's group + "_MULTIPLAYER_LOBBY_BUTTON") per master in a loop that stops at 5, plus the
  random one. So it needs: the count, the two masters' lobby art in the archive, and the name tables of the mini
  board and tally screens (which fall back to Bjorn for a name they do not know).

## Multiplayer lobby with seven masters, guests, 60 pictures a second, HD census (2026-10-14)

All of this was seen on the PC only. The Switch NRO with it is the build of 2026-10-14; nothing of it has run on
the console.

### Windy and Jimmy in the multiplayer lobby (`peggle2_dlc_mp_masters`, on; `app/src/dlc/peggle2_dlc_lobby.cpp`)

- **Where the lobby's layout is.** `PACKAGES/LAYOUTS.RTON` of the archive, an object of class
  `MultiplayerLobbyLayout` (the 360 executable's property list for it: `sub_82202E08`). The PS4's file has what the
  360's lacks (`out/ps4_dlc/scripts/layouts_diff.py multiplayer`): `master7`, `master8` and `masterScale` = 0.8, and
  other values for `master1`..`6`, `pip1`..`4` and `optsButton` (the table is in the source file's header). The
  360's class has no fields for the three new entries, so the values are constants of the port, not data of the
  archive.
- **How the PS4 makes the buttons smaller.** Its placing function (`fn_288D90` of the eboot) sets, after placing a
  button, `GameAnim scale = masterScale * art lines / 2304` (`fn_16EC30`: the float at +0x198 of the object at
  +0x110 of the GameAnim). The 360's GameAnim constructor (`sub_8218CF10`) writes the same factor to +224 and +228
  of its PopAnim (+152); the port writes `0.8 * art scale` to +224 as each button of the row is placed.
- **What the port changes** (only when the master list has more than five masters whose lobby button the archive
  has; the id is `POPANIM_<group>_MULTIPLAYER_LOBBY_BUTTON`): `tools/mp_masters.py` puts two calls into the
  generated code: in the loop of `sub_82206AC0` (it stops at 5) and in the placing function `sub_82207658` after its
  six map entries (button number -> placeholder; a button without one was read through a null pointer). A hook at
  the entry of `sub_82206AC0` counts the buttons and makes two more placeholders. With six masters the seven
  buttons sit half a slot in.
- **The art**: `tools/ps4_dlc/build_mod.py --content ...,multiplayer` adds the PS4 ids with MULTIPLAYER_LOBBY and
  MULTIPLAYER_PEGPARTY of the two masters to the composites `Characters` and `MultiplayerInGameCharacters`
  (`out/ps4_dlc/game_root_mod_mp`, 419 textures; pack `out/dist/switch/peggle2-nx-dlc-full-mp`).
- **Seen** (PC, `mod_test.ps1` with a second scripted player, `out/ps4_dlc/shots/mp_duel1_*`, `mp_duel2_*`): the row
  of eight in the PS4's arc; Jimmy against Windy, and Windy against a random pick (Windy again, another costume),
  from the lobby through the VS screen into alternating turns; with the archive without the art
  (`game_root_mod_full`) the 360's lobby of five and one warning line.
- **Not the PS4's**: the order. The row follows MASTERS.JSON, where `build_mod.py` adds Jimmy before Windy; the PS4
  has Windy first. The profile is a binary blob without names (1,054 bytes), so whether it keeps anything by master
  number was not found out, and the order of the delivered packs was left alone.
- **Not looked at**: Peg Party and the online modes (they need Xbox Live; `sub_821BDA70`, the mini board, knows
  five master names and falls back to Bjorn), "change costume" in the lobby, four players.

### Guests: a second controller can join (`xam_guest_signin`, on; `sdk/src/kernel/xam/xam_user.cpp`)

- Before, no second player could join any lobby: the SDK had one signed-in user. The game asks
  `XUserGetSigninInfo` for each of the four users after a sign-in change (`sub_8237B948`: info flag 2 = guest is
  its player type 1) and opens the sign-in screen when an unknown controller presses A (`sub_8237A0B0` ->
  `XamShowSigninUI`).
- Now `XamShowSigninUI` signs in every further connected controller as a guest of the profile (flag 2, guest number,
  sponsor 0, name "Name(n)", its own XUID) and sends the sign-in change with all their bits; `XamUserGetSigninState`,
  `GetSigninInfo`, `GetXUID`, `GetName`, `GetGamerTag`, `CheckPrivilege` and the content restriction flags answer
  for guests. Profile settings and content stay the profile's only. Log: `[XAM] controller 2 signed in as guest`.
- The test harness got a second scripted player for this (`peggle2_input_script_p2`: an idle pad as player 1's real
  device and the script as player 2's).

### 60 pictures a second (`peggle2_fps` = 30 | 60, default 30; `app/src/peggle2_frame_hooks.cpp`)

- **Why the game is 30.** Its Direct3D device has the presentation interval 2 (the word at device + 14000, read by
  `sub_8261F990` when the swap queues the flip). And its frame step (`sub_8242F8A8`; the call chain of a swap is
  `sub_822A8ED0` -> `sub_82370B68` -> `sub_8242E478` -> `sub_8242F8A8` -> `sub_82479748` -> `sub_8261FE50`, from the
  host call stack: `peggle2_diag_swap_stack = N`) runs **two logic updates** (`sub_8242F2C8`, each with a fractional
  widget update of 1.0) and then draws. Game time is counted in updates of 1/60 s.
- **Interval 1 alone doubles the speed of play**: measured, 600 swaps per 10 s, and the first shot of Bjorn's level
  1 took about 7 s where it takes about 12. (The logos at the start did not get faster: those run on the clock.)
- **The key**: interval 1, and the frame step asks `peggle2_fps_updates()` before its second update
  (`tools/fps_updates.py`, one line after `loc_8242FA04`): one update for each 1/60 s of host time that passed
  since the last frame, at least one, at most two. So one update per frame at 60, two per frame at 30 (what the
  360 does), a mix between, and below 30 the slow-down the 360 game has.
- **Measured (PC)**, the same scripted presses at the same times, 30 s of Bjorn's level 1 from the level's start
  with a whole shot: brightness per frame of the two recordings fits best at time scale 1.00 (offset +0.15 s, mean
  square error 0.052; scale 2.0: 11.8) (`out/ps4_dlc/scripts/video_luma.py`, `luma_speed.py`); all 1,799 frames
  of the 60 recording differ from the frame before. With frames slowed to two vblanks
  (`peggle2_diag_ring_swap_delay_us = 21000`): 30.0 frames and 59.7-60.0 updates a second; with
  `diag_vdswap_delay_us = 12000`: 58.7-60.0 frames and 59.3-60.1 updates.
- The input script's seconds and default hold follow the key (60 game frames a second).
- **Not known**: whether the console reaches 60 at 720p or 1080p; whether anything in the game counts drawn frames
  (none was seen in that level; menus, the other masters' powers and the PS4 powers' layers were not watched at
  60). Configs: `peggle2_60fps.toml`, `peggle2_60fps_720.toml`; the log has a `[fps]` line every 10 s.

### What the HD pack covers (`out/hd/scripts/hd_census.py`)

- Textures of the archive (`game_root_mod_full`, 415): 407 have an HD version (354 distinct pictures; the others
  are the same picture in several language groups), 99.8 % of all texels. The 8 without: the trial splash screen in
  six languages (a 1280x720 picture in groups without a resolution), a 4x8 placeholder and one effect texture
  (739x720, not a multiple of 8). The archive with the lobby art has 4 more atlases the pack was not built from
  (`Characters_Dlc_720` and `MultiplayerInGameCharacters_Dlc_720`, two each).
- Sprites on those atlases (31,167): 29,770 are the PS4's 1080 art (95.5 %); 1,397 are the 720 art enlarged: 1,182
  with no PS4 sprite of their name and 215 whose PS4 sprite is another picture. By kind, without / different:
  masters 1,072 / 118, interface 67 / 31, level thumbnails 0 / 38, tutorial 14 / 9, fonts 22 / 0, characters
  0 / 16, backgrounds 5 / 2, board 2 / 0. (The pack's README said "mostly font pages": wrong, corrected.)
- Of the sprites that were compared, 99.8 % of the texel area is the PS4's; the 1,182 without a twin have no row in
  the builder's report, so their area is not in that figure.

### HD pack of 2026-10-14: sprites matched by picture (`tools/hd/matching.py`, `out/hd/pack4`)

Asked for: more of the PS4's art in place of the enlarged Xbox 360 art. Result: 30,864 of 31,231 sprites are the
PS4's (98.8 %; 29,770 of 31,167 before), 367 are left (`out/hd/scripts/hd_census.py`).

- **Why 1,182 sprites had no PS4 sprite of their id.** The ids carry a piece's size in design units
  (`..._LORT_STONE_93X82`), and the PS4's animations store the same pieces at other sizes: for Jeffrey's stone
  costume the PS4 group has 715 sprites against 524, the 360's 93X82 is there as 83X74 and as 111X99 (the ratios
  cluster at 1.125 and 0.84), and none of the 119 unpaired ids occurs in any other 1080 group
  (`out/hd/scripts/no_twin_look.py`). The picture is the same, the scale is not 1.5.
- **Tried first, of no use:** moving the PS4 sprite over the 720 one at the atlas scale (an FFT search for the
  offset, `tools/hd/register.py`): 3 of 116 at 30 dB. The pieces are not cut differently, they are scaled.
- **The match** (`matching.by_picture`): every PS4 sprite of the same-named 1080 group whose proportions fit, laid
  sprite on sprite ("whole") or solid content on solid content ("content": for a picture on another canvas), is
  scaled down to the 720 sprite and compared. Accepted with PSNR >= 22 dB, PSNR of both averaged over 3x3 texels
  >= 26 dB, and mean colours within 10 of 255; among the accepted ones within 3 dB of the best, the one stored
  largest. The limits were set by eye: the builder's `--match-previews` writes every match as a picture named by its
  scores (`out/hd/match_prev_final`: 1,026 by picture, 18 cut to another canvas, 13 with the shadow kept, 55
  rejected near the limits). Of the 70 accepted pairs nearest the limits none is another picture
  (`out/hd/match_prev_all/_sheet_pic_low.png`); the user looked at the sample sheets and agreed. What the colour
  rule keeps out: a pink and a blue burst of the same shape (PSNR 21.6, low-pass 22.6), the 360's blue loading
  sparkle against the PS4's white one (low-pass 33 to 37), the green A button against a black one in the PS4 archive.
- **Pictures on another canvas** (`matching.refit`, only for a PS4 sprite whose size is not 1.5 times the 720
  one): the masters' lobby pictures are 270x282 here and 668x698 there (2.5 to 2.9 texels per texel), the figure
  elsewhere on the canvas; laid by solid content they reach 27 to 31 dB where sprite on sprite gave 13 to 19.
  Gnorman's and Jeffrey's second costumes are other ids on the PS4 and were found by picture. Left as they are:
  Luna's four and three of Jeffrey's (15 to 21 dB: drawn anew).
- **Two mistakes of the first version, found in the game's picture at 1080p and not by the measures:**
  - The content fit was tried for every sprite with a low PSNR and could change its size by 3 per cent. For the
    bricks it won (25 to 29 dB against 2 dB less) with a size the glow around them gave: their blue reached x = 626
    where the game's own atlas and the pack before reach 629 (`out/hd/shots/ref_lvl`, `old_lvl`, `new_lvl`: the
    same frame with no HD pack, pack2, pack3). Now a sprite of the same id and 1.5 times the size keeps its
    rectangle (0 bricks refitted, 629 again in `p4_lvl`), and a content fit may change its size by 2 per cent only
    when that gains 0.75 dB.
  - The PS4's lobby pictures have no shadow under the figure; the 360's have (2 to 4 per cent of their alpha).
    `matching.extras` finds alpha the 720 sprite has and the PS4 picture lacks over more than a fringe, and the
    area is kept from the 720 sprite under the PS4 picture (13 lobby pictures; no other sprite of the archive
    triggers it). `out/hd/shots/_cmp_lobby_p4.png`: Gnorman in the lobby with pack2 and pack4.
- **Checked in the game** (PC, 1080p, lockstep frames of the map screen, the lobby with Gnorman, Bjorn's level 1,
  each with pack2 and pack4): a scan for thin lines (`out/hd/scripts/line_scan.py`: runs of 40 pixels that differ
  from the pixels two away on both sides while those agree) finds only lines the pack2 capture has with the same
  values (the level's frame art). The user had seen a line in the sky left of the castle in a picture shown as a
  scene reference: that capture (`hd1080_0000.png`) was made with the first pack of 2026-10-13 (x 669-670, y
  212-354, 143 pixels); the captures with pack2 and pack4 have none there.
- **What is left, 367 sprites** (0.36 % of the area of the sprite rows): 204 pieces of masters' animations (116 of
  them under 6 texels on a side, which are not matched by picture), 66 interface pieces (the Xbox button pictures,
  recoloured pieces), 38 early thumbnails of Windy's levels in the 360 archive, 22 font pages, 20 tutorial pieces,
  9 lobby pictures, 8 others.
- Build: `python -P -E tools/hd/build_hd_pack.py --x360 out/ps4_dlc/game_root_mod_mp/main.rsb --ps4
  out/ps4_dlc/extract/ps4_base/game_data/main.rsb --out out/hd/pack4 --jobs 4 --match-previews <folder>`, 1,793 s;
  358 atlases (the four multiplayer atlases are new), 836,528,783 bytes, verified. `out/hd/pack3` is the version
  with the two mistakes: not to be used. The pack in `out/dist/switch/peggle2-nx-hd` is pack4.
- **Not done:** nothing of it on the console; the masters' animations were only seen in the map screen and the
  first level (Bjorn), not Jeffrey's, Berg's or Gnorman's, whose costumes hold most of the 1,026 matches (the test
  saves have those masters locked). In their place: Jeffrey's default costume atlas, the 720 one enlarged beside
  the HD one (`--previews`, `out/hd/prev_jeff/_cmp*.png`): every piece at its place and of its size in two regions
  of about 150 pieces.

## Console run of 2026-10-14 and what it led to: one archive, Select a Master, intro videos, import records

The user's console run (log `peggle2_007.log`, config `peggle2_60fps_hd_always.toml`) and their report:

- **`peggle2_fps = 60` works on the console**: the `[fps]` lines read 32 to 60 pictures and 57 to 60 logic steps a
  second (speed of play held). First console confirmation of the method (presentation interval 1, one logic step
  per 1/60 s of host time).
- "No music in Windy's or Jimmy's levels, no voices, Windy's intro video silent": the log's mount line says 34 files,
  1079.0 MB: the card still had the first DLC pack (no sound, no music). Four variant folders stood side by side in
  `out/dist/switch`. The user: "I would prefer if we just had one pack. not two". From now on there is one archive
  (`out/ps4_dlc/game_root_one/main.rsb`, content `costumes,jimmy,windy,levels,strings,multiplayer,select,videos,
  sound,music`) and one pack, in `out/dist/switch/peggle2-nx/`.
- The intro videos were silent in every pack all the same: see below.
- "After beating a level and selecting it again ... the ps4 masters don't show up and the dlc costumes don't show up
  either by cycling through with Y": see below.
- Compression artefacts on the blue round pegs and the cannon with the HD pack: the HD pack's encoder, see the
  section after this one.

### Intro videos with sound (`build_mod.py --content ...,videos`)

`out/ps4_dlc/scripts/bk2_info.py`: the 360 archive's `VIDEO\WENDY_INTRO.BK2` and `JIMMY_INTRO.BK2` are 1920x1080
Bink 2 files with **0 audio tracks** (the 360 archive ships them that way). The PS4's have one
track (48 kHz, the same track word as the 360's other intro videos). Videos are plain files of the `Videos` packet,
played from their offset in the archive, so the step puts the PS4's two files in the place of the 360's and adds
`CHALLENGE_INTRO_WENDY/JIMMY.BK2` with their subtitle files (44,080,812 bytes in all; it asserts a track count of
at least 1).

PC, the muted recording of the game's mix (`audio_dump_output_s`), route from `seed_noach` to level 1 of each area:
during Windy's video the mix is at -26 to -34 dB where the old archive gave silence, during Jimmy's at -27 to
-32 dB (`out/ps4_dlc/audio/fmv_windy_one_listen.wav`, `fmv_jimmy_one_listen.wav`, sent to the user; sheets
`out/ps4_dlc/shots/fmvw2_sheet.jpg`, `fmvj2_sheet.jpg`). Not checked: lip sync by eye, the two challenge videos
(nothing in the 360 executable names them; they are in the archive for completeness), the console.

### Select a Master with seven masters and six costumes (`peggle2_dlc_master_select`, on)

The screen (`SingleplayerMasterSelectScene`) opens when a level that was beaten is chosen again (test: the save
`out/pc/test_userdata/seed_windy1`, made with the test switch `peggle2_dlc_test=firstorange`, which wins a level
with the first shot). Four things were missing; `out/ps4_dlc/shots/msel11_sheet.jpg` and `msel11_four.png` show
the result on the PC.

1. **Row layout** (`app/src/dlc/peggle2_dlc_master_select.cpp`, hook on the placing function 0x821F8AB0): the 360
   places the first `buttons - 1` buttons in a row and the last one (random) under it; the count is a function
   static (0x82D90000 + 10132, guard bit at + 10136). With more than six buttons the hook sets it to five and
   writes the PS4 layout's second row (origin (565, 1163), step (465, 0); the 360's `MasterSelectLayout` has
   (1255, 1163), (0, 0)) into the layout object: five masters above; Jimmy, Windy and the random button below.
2. **Buttons and side pictures of the new masters** (`build_mod.py --content ...,select`):
   `POPANIM_MASTERS_{WENDY,JIMMY}_MASTER_SELECT_{BUTTON,COSTUMES}` as a further subgroup of `Characters`. A master
   without them gets Bjorn's button and no picture.
3. **The name panel** ("MasterSelectBlades", the coloured panel with the master's name and power): one animation,
   `POPANIM_FRONT_END_UI_LEVEL_MASTER_SELECT_SLIDERS_LEVEL_MASTER_SELECT_MAINFULE2_SLIDERS`, with labels by master
   name (`bjorn_in/idle/out`, `berg_`, `lort_`, `gnorman_`, `ulna_`, `random_`). The PS4's file has `fairy_*` and
   `jimmy_*` and two more pictures: it takes the place of the 360's file and the two pictures come as a subgroup of
   `InterfaceMasterSelect`. Before: no panel for the two (the scene's background showed through).
4. **Costumes five and six** (the two costume packs). The side picture of a master is one animation with labels
   per costume (`n_in`, `n_idle`, `n_out`, `n_locked_in`, `n_locked_idle`, `n_locked_out`), driven by a state
   machine: states 6n-5 ... 6n, state 0 = "stop". Three facts decided the fix:
   - the 360's animations have four costumes, the PS4's six (`out/ps4_dlc/scripts/pam_look.py`, a full PAM reader
     that must end at the file's end): the PS4's files take the place of the 360's for the five launch masters;
   - the pictures they name must be loaded BEFORE the animations (a composite loads its subgroups in order; a
     picture that is not there yet is looked for as a file, `images\720\...\costumes_1024x2304_.png`, and the game
     stops): `extend_composite(..., before='Characters_Common')`; the same group name twice in the manifest stops
     the game at start ("Resource already defined"), so every added subgroup gets its own name (`_Dlc`, `_Dlc2`);
   - the 360 makes the states of four costumes (a loop counted down from 4 in `sub_821F8390`) and its state
     machine holds **32 states** (`std::bitset<32>`; every state number is compared with 32, and setting a bit
     beyond it throws "invalid bitset<N> position"). The PS4's function (`fn_26AA60`) loops six times with the same
     numbering and its machine has a `bitset<64>` (`cmp esi, 0x40`). Six costumes need 37 states.
   `tools/costume_states.py` (post-codegen, last) changes three recompiled functions: the loop count comes from
   `peggle2_costume_count` (6 when the archive has `POPANIM_MASTERS_WENDY_MASTER_SELECT_COSTUMES`, else 4), rounds
   five and six make the three unlocked states only (25-27, 28-30), and the two functions that turn a costume's
   picture on and off (`sub_821F9618`, `sub_821F95A0`: idle state 6c + 2, locked state + 3) map 26 and 32 to 26
   and 29 and use costume one's locked state (5) for them. Nothing locks the pack costumes but their master, and
   a locked master shows its silhouette whatever the costume.

PC result (the archive without test options, `seed_windy1`): Bjorn's six costumes by Y (2 to 4 as locked
silhouettes with their unlock text, 5 and 6 the Garden and Spooky ones), a locked master's silhouette with Y, Jimmy
and Windy with panel, picture and six costumes each, and a level started as Windy in the sixth costume
(`msel12_sheet.jpg`). Not checked: the console; the masters the test save has locked (Jeffrey, Berg, Gnorman, Luna)
with their own six costumes; the "costume unlocked" reveal after an area is finished.

### Import records are not constants (`tools/ro_constants.py --xex`; from futari-nx)

The futari-nx session reported (2026-10-14) that `ro_constants.py` folds loads of the XEX's import records: words
at the start of `.rdata` (0x82000600 onwards here) that the loader writes at run time; for an imported kernel
variable the address its kernel gave it. The generator's image dump holds the generator's own values.
`out/ps4_dlc/scripts/xex_import_records.py` on this tree: 436 records (xam.xex 144, xboxkrnl.exe 292), 12 of them
with a kernel address in the dump (0x3000x000, 0xFFCAx000), and **79 folded loads** of those 12 (of 5,561 folded
loads). On the PC a folded load is still a load compared with the constant (0 differences in every run so far);
on the Switch it is the constant, right only as long as the console's kernel hands out the same addresses in the
same order as the PC process that ran the generator. No wrong value was seen anywhere; the game runs on the
console. The tool now takes `--xex` (`post_codegen.py` passes it): it never folds a load that touches a record and
puts the 79 back as `REX_LOAD_U32(address)`; a second run changes nothing. PC build and run pass.

### HD pack of 2026-10-14 afternoon: its own colour encoder (`tools/hd/bc3.py`, `out/hd/pack5`)

The user, on the console at 1080p with `out/hd/pack4`: "blue round pegs look like they have some slight
rendering/compression artact issues ... as well as the cannon that shoots balls ... audit ps4 assets for those
artifacts". Reproduced on the PC (`out/hd/shots/cmp_pegs.png`, `cmp_cannon.png`, left halves: dark squares on the
blue peg, a ragged edge on the cannon's frame).

- **Cause:** not the PS4's art and not the renderer: the pack's encoder. `bc3.py` made the alpha half of each block
  itself and left the colour half to Pillow's DXT5 writer, which takes the block's extreme colours as endpoints.
  A block with three colours that are not on one line (a peg's highlight, body and rim) then gets one of them badly
  wrong, as a square of 4x4 texels.
- **Fix:** `bc3.encode_colour`: per block the principal axis of its colours (texels weighted by alpha, a
  transparent texel still counts 15 %), its ends as endpoints, then three rounds of "nearest of the four colours
  per texel, least-squares endpoints for those indices"; the best of the four tries is kept; colour0 > colour1
  always. All in numpy over slabs of blocks; `bc3.encode_pillow` keeps the old way for comparison
  (`python -P tools/hd/bc3.py <png>` prints both).
- **Audit** (the pictures the atlases are made from against what the game decodes; colour over the texels that
  show):
  - every atlas of the new pack (`peggle2_hd.txt` has a line per atlas, the build log a summary): lowest 31.8 dB,
    a tenth of the 362 under 38.5, median 44.8; 7 atlases under 34 dB (two level-thumbnail sheets, the pegs'
    `GameObjects`, the new masters' `Characters_Dlc`, one Gnorman costume sheet): sheets of many small detailed
    pictures;
  - block by block on the eight lowest atlases (`out/hd/scripts/canvas_audit.py` on the builder's `--previews`;
    a block is "bad" when its visible texels are off by a mean of 10 levels of 255 or more): bad blocks 64,695 of
    607,452 (10.65 %) with Pillow's colour half, 30,260 (4.98 %) with the new one; the worst block per atlas from
    64-113 levels down to 25-38. `GameObjects` alone: 27.9 -> 33.0 dB, bad blocks 2,014 -> 895, worst 69 -> 25
    (`out/hd/audit_sheets_new/GameObjects_720_GAMEOBJECTS_720_00_worst.png`: source, old, new of its worst blocks);
    two master sheets: 37.8 -> 41.7 and 37.3 -> 41.1 dB, bad blocks 5,143 -> 1,148;
  - in the game on the PC at 1080p, same frame, old pack against new: the squares on the pegs and the cannon's
    ragged frame are gone (right halves of the two pictures above).
  What is left is the format: a BC3 block holds four colours on one line, and the art was BC3 once already on the
  PS4 (at another block alignment, and most sprites are resampled by about 1 % to sit on 1.5 times the 720
  rectangle). BC7 would be the next step if the user still sees artefacts (it needs its own encoder and a renderer
  format; not started).
- **Build:** `python -P -E tools/hd/build_hd_pack.py --x360 out/ps4_dlc/game_root_one/main.rsb --ps4 ... --out
  out/hd/pack5 --jobs 5`: 4,291 s (the numpy encoder is about three times slower per atlas than Pillow's; other
  builds ran beside it), 362 atlases (the four new ones are the Select a Master art), 843,452,697 bytes, verified;
  sprites as before (31,210 replaced with 1,026 by picture, 171 without a twin, 197 of different art).
  `out/hd/scripts/encode_audit.py` (sprite against PS4 texel for texel) was a first try and is of little use: only
  sprites at exactly 1.5 times the size on even coordinates qualify, a handful per atlas.

### The delivery of 2026-10-14 afternoon (one folder)

`tools\win\package_switch.ps1 -Pack <game_root_one.nxpack> -MovePack -HdPack out\hd\pack5\peggle2_hd.pack` (the
two options are new; the README it writes lists every file with size and date and the MB figure the console's
`[pack] mounted` line must show):

| file in `out/dist/switch/peggle2-nx/` | bytes | built |
| --- | --- | --- |
| `peggle2.nro` (no LTO; ELF `out/nx/elf/peggle2_select_records_nolto_20261014_1229.elf`) | 48,564,421 | 12:29 |
| `game_root.nxpack` + `.nxidx` (the one archive; 34 files, 1,793.1 MB) | 1,880,232,368 | 12:25 |
| `peggle2_hd.pack` (pack5) | 843,452,697 | 13:22 |

`tools/check_switch_build.py` passes on the NRO (start at 0, optimised flavour, Mesa of the SDK, no self-indexed
lookups, `-mearly-ra=none` in 686 of 686 commands, the two streaming copies' lookups all right).

Rehearsal on the PC with exactly these files (`REX_GAME_DATA_ROOT` = the folder's `game_root`, so the pack is
mounted: "34 files, 0 directories, 1793.1 MB"; the HD pack by `peggle2_hd_pack`; `peggle2_fps = 60`,
`peggle2_hd_atlases = 2`, docked 1080p; save `seed_windy1`): 57.6 to 60.0 pictures and 58.1 to 60.0 logic steps a
second, Select a Master with eight buttons, Bjorn's costumes by Y up to the fifth, across to Jimmy with his panel
(`out/ps4_dlc/shots/final_sheet.jpg`). Windy's level 1 with shots and the fairy power on the folder archive: music
and effects in the recorded mix throughout (`out/ps4_dlc/audio/music_windy_one_listen.wav`).

**Not done:** nothing of this afternoon on the console. Not checked anywhere: the stock Xbox 360 archive with the
new program on the Select a Master screen (the loop count is the 360's 4 there by construction); the other four
launch masters' fifth and sixth costumes (locked in the test saves); a level played in a pack costume of Jimmy.
Superseded folders (`out/dist/switch/peggle2-nx-dlc*`, `-hd`, about 7.0 GB) and build stages
(`out/ps4_dlc/game_root_mod*`, `game_root_test`, `out/hd/pack` ... `pack4`: about 12 GB) are still on the disk
(8.3 GB free): the user's to delete (`out/dist/switch/SUPERSEDED_FOLDERS.txt`).

### Console run of 2026-10-14 afternoon: sound stutter, lobby places, Windy's thumbnails (PC-verified, console pending)

The user's run with the single pack (`peggle2_008.log`, 60 fps HD config, logging on; then `peggle2_all_on_quiet.toml`,
log off, `rex_profile (16).log`): "stuttery audio ... on the title screen for about 3 seconds with logging on and
off and on the world select", "the dlc characters are in wrong space in multiplayer menu" ("their position on the
screen not the icon"), "wendy's level icons also don't fit in the box". And: "it's a release build do we really
need self-checks?"

- **Sound stutter: the native replacements' self-checks** (`app/src/cpu/peggle2_hot_guest.cpp`). Twelve-plus Wwise
  functions run as localized native copies; each checked its first 256 calls per thread (then 1 in 4096) by running
  copy and original and comparing. The leaf copies' check (`CheckLogged`) kept the copy's final bytes in an
  `unordered_map<uint32_t, uint8_t>`: a node allocation per byte written, and the mixing leaves write kilobytes
  per call. Evidence: the morning profile's samples of `AK::EventManager` (the Wwise audio thread, 50.7 % CPU at the
  title screen) are `_free_r`, `_malloc_r`, `mutexLock/Unlock`, that very `_Hashtable<unsigned, pair<unsigned const,
  unsigned char>>` and `peggle2::hot::Check` (addresses mapped with the 01:45 ELF); the afternoon profile shows the
  thread at 40 to 70 % for the first 40 s; `peggle2_008.log` has the Wwise hooks' first calls ("native replacement
  ON") at 13:38:31-35 (title screen) and the main mixing leaves (825BE6A0, 825BCB88, 825BEED8, 825BF360) at
  13:38:56 (world select).
  Changes: `CheckLogged` copies the copy's bytes store by store into a kept buffer after the call and compares
  entry by entry (no allocation); every guarded call is timed (`[hot] 10 s: ... N ms in guarded calls`; a call over
  1 ms is named in a `[hot] slow guarded call` line); and the guard is **off by default**
  (`peggle2_hot_guard_calls = 0`, `peggle2_hot_guard_period = 0`: only the check at each counter's first call,
  which is free; `peggle2_cpu_verify` still checks every call). PC: with the cheaper check but the guard on, up to
  245-253 ms per 10 s in guarded calls in the windows where new hooks start; guard off: 0.0-1.8 ms; 0 differences.
- **Lobby, order**: the PS4's MASTERS.JSON has Wendy (5) before Jimmy (6); ours had Jimmy first (the builder's
  steps ran jimmy before windy). `build_mod.py` now adds Windy first. A save made with the old order
  (`seed_windy_cos`) opened with the new one: Windy's area progress, both masters unlocked, six costumes each; the
  profile save does not change when a costume is chosen (6 bytes differed after a costume pick and a level start:
  a counter and a time).
- **Lobby, the big picture** (`IMAGE_MASTERS_<M>_MULTIPLAYER_LOBBY_COSTUME<n>`): the 360 draws it from a 270x282
  canvas, the PS4's is 668x698 (445x465 at 720). `build_mod.py --content multiplayer` now scales the new masters'
  pictures whole to the 360's canvas (`convert.convert_selection(..., sizes=)`): 2.47 PS4 texels per texel, where
  the launch masters' 360 pictures sit at 2.42 to 2.89 of the PS4's. PC 720p and 1080p HD: Windy and Jimmy stand on
  the panel above "Host" like the others (`out/ps4_dlc/shots/fix_1014_before_after.png`).
- **Windy's thumbnails**: the 360 archive has `IMAGE_LEVEL_THUMBNAILS_{LARGE,SMALL}_WENDY_*` as early pictures at
  286x254 and 88x78 (every other area: 378x333 and 88x72); the PS4 has final ones under the same ids at the standard
  size. The levels step takes the PS4's as `<id>_PS4` and the stages name those (38 pictures: 10 levels, 9 trials).
- **HD pack**: only `Characters_Dlc_720` and `LevelThumbnails_Dlc_720` changed; `tools/hd/merge_packs.py` (new)
  lists the archive's 720 atlas keys, takes each entry from the packs given and reports groups without one:
  those two built alone (`--groups`, 190 s) and merged with pack5 -> `out/hd/pack6` (362 atlases, 845,996,772
  bytes, verified) instead of 70 minutes for all.
- Delivered (`out/dist/switch/peggle2-nx`): NRO 14:38 (48,568,517 bytes; `check_switch_build.py` passes; ELF
  `out/nx/elf/peggle2_guardoff_nolto_20261014_1438.elf`), pack 1,881,256,368 bytes (mounts as 1794.1 MB on the PC),
  HD pack6. Not checked on the console.

## Release 1.0.0 (2026-10-14 evening): built-in settings, icon, LTO + PGO, README, installer start

The user: "we are getting ready for a release build make this image the nro icon author ChanseyIsTheBest version is
1.0.0 Please hardcode all optimisations on in the toml all the ones in the current all on, turn logging off, enable
all DLC and force the 1080p assets always for handheld and docked ... only things I want to show up is controller
mapping settings, framerate settings make the default cap 30 like 360 version, and language setting. compile the
version with LTO and PGO I'll collect the PGO files after you finish building. and please work on the readme copy
SA2's as a start alongside a web installer for the game like sa2"; then "The only version of the game i will support
will be ps4 + xbox 360 combo with all dlc enabled", "plus 1080 always on only", and "when you make the web installer
base it on mushi futari because it fixed a web assembly shader issue".

- **Built-in settings (sa2-nx's setup, ported).** SDK: `rex::cvar::LoadConfigString`, `ReXApp::BuiltInConfig()` /
  `DefaultConfigFile()` (rex_app.cpp applies the built-in TOML first, then `peggle2.toml`; without a `peggle2.toml`
  it writes the default one first), `log_to_file` (false: no log file, no `logs/` folder; stderr is redirected to
  `logs/rex/rex_stderr.log` only with it on: `RexSwitchRedirectStderr`), log folders created only when something is
  written (`RexSwitchEnsureLogDir`: crash, assert, profile), and the console profile file behind the title's
  `switch_profile_report` key. App: `app/peggle2_switch_defaults.toml` (compiled in through
  `src/peggle2_builtin_config.h.in`: every enhancement of the all-on config, the PS4 content keys, `peggle2_hd_atlases
  = 2` with 1080 docked / 720 handheld, `log_level = "off"`, `log_to_file = false`, the profiler and its report off)
  and `app/peggle2_switch.toml` (the player's: `peggle2_fps = 30`, `peggle2_language`, the controller keys). Both are
  written by `tools/make_enhancement_configs.py` (`write_release`); the test configs in `app/configs/` still work as
  `peggle2.toml` and override key by key. `tools/win/package_switch.ps1` no longer puts a `peggle2.toml` in the
  folder.
- **NRO metadata**: "Peggle 2", "ChanseyIsTheBest", 1.0.0, `app/icon.jpg` (256x256 JPEG made from the user's
  picture; the picture itself is the user's, PopCap's artwork).
- **PGO (sa2-nx's setup, ported)**: `PEGGLE2_PGO=generate|use` in `app/CMakeLists.txt`, `src/peggle2_pgo.cpp`
  (gcov info in a section, streamed to `pgo/peggle2_pgo_<N>.stream` every 2 minutes, on HOME and at exit),
  `tools/win/build_switch.ps1 -Pgo`, `tools/win/pgo_merge.ps1`, `tools/pgo_summary.py`; the profile folder is `pgo/`
  at the repository root. The build script now also puts MSYS2's make on PATH, so the LTO link's LTRANS jobs run in
  parallel (sa2-nx: 17 minutes without). The PGO-use build gets sa2-nx's inliner limits (64 / 12), measured there,
  not here.
- **README.md** rewritten on sa2-nx's model (the old stage table and build steps moved to `docs/building.md`, which
  now also covers the PS4 conversion, the HD pack and its merge tool, the release build and PGO, and packaging).
- **Installer**: `D:\peggle 2\peggle2-nx-installer`, futari-nx-installer's code with sa2-nx-installer's STFS package
  reader and asset pack writer, the page icons from the user's picture, and a README with the design and its status.
  What it still needs: a PS4 PKG/PFS reader, the shader library (3 of Peggle 2's 12 library containers are stored
  whole in `default.xex`, `0x820528D0`-`0x82052D5C`; the others are made by the port's variant and wrapper tools,
  which need JavaScript ports, and the WebAssembly translator must be built from this port's `shaders/`), the PS4
  conversion in the browser (plan: Pyodide running `tools/ps4_dlc` with WebAssembly kernels), and the 1080p art from
  a recipe of placements made on the PC (the matching search is what takes most of the 70 minutes).

## Web installer: the conversion in the browser (2026-10-14)

- **Approach changed** from Pyodide to a JavaScript port of `tools/ps4_dlc`, module for module, in
  `peggle2-nx-installer/lib/conv/`. Proof: `test/conv_build_node.mjs` builds the whole archive (all ten content steps)
  and compares it with build_mod.py's packet by packet: all 550 packets identical (file lists, plain files, textures,
  the ten sound banks, GLOBAL_DATA.BNK, the 519 streamed sound files, manifest, composites, texture records). Node,
  one thread: 399 s; build_mod.py with its caches empty: 575 s. Only the zlib streams of the texture parts differ: this
  PC's Python has zlib-ng (`zlib.ZLIB_RUNTIME_VERSION` 1.3.1.zlib-ng), the browser has CompressionStream; the game
  inflates either.
- **Pillow's DXT5 encoder** (`ptx.encode_bc3`, the 720 atlases), reproduced by testing (no Pillow source here):
  colour endpoints = the block's largest and smallest RGB565 value (c0 = max, c1 = min), each texel's index from
  d = sum over the channels whose endpoints differ of |texel - low| / |high - low| (float32, low/high = the endpoints
  decoded), d *= 6 / count (integer division), then d < 1 -> 1, < 3 -> 3, < 5 -> 2, else 0; no channel differs -> 0.
  Alpha: a0 = min, a1 = max (the 6-value mode), 0 -> index 6, 255 -> 7, a flat block -> 2, else
  2 + clamp(floor(5 f + 1/2) - 1, 0, 3) with f = (a - min) / (max - min). Checked block for block on 393,216 random,
  stepped and smooth blocks and on every (min, max, value) triple. (So its colour index is a mean of per-channel
  positions, not a nearest colour; plausibly a cause of the dark squares bc3.py's docstring describes, not checked.)
- **at9.py runs ffmpeg with `-cpuflags 0`** now: with ffmpeg's AVX2/FMA3 code the PCM depended on the CPU (1 LSB on
  about one sample in 10,000); the plain C paths are what the installer's WebAssembly build of FFmpeg 7.1's decoder
  and resampler gives, byte for byte. The PCM cache of the delivered pack (`out/ps4_dlc/cache`) was made without it;
  the new reference cache is `out/ps4_dlc/cache_cpu0` (the delivered pack's sound differs from it by that 1 LSB).
- **Shader library: two blockers found** (installer's `lib/shaders.js`, `test/shaders_node.mjs`):
  1. three of the six shaders the game creates are compiled at boot from HLSL text in `default.xex` by the statically
     linked D3DXCompileShader (`sub_8248D7C0` calling `sub_82636DB8`: vs_3cae, 105 bytes at 0x820498D0; ps_23fb,
     542 bytes at 0x82049940, Bink's YCrCb -> RGB; ps_b79e, 608 bytes at 0x82049B60, the same with alpha). Their
     containers exist nowhere in the game's files, and 8 of the 12 library entries depend on them (vs_3df8 is vs_3cae
     rewritten by the fetch patcher; the four wrapped Direct3D shaders use ps_23fb / vs_3cae as wrapper templates);
  2. the installer's WebAssembly DXC is v2025.1, the tested library was built with dxcompiler 1.8.2407: 4 entries'
     SPIR-V differ, and v2025.1's vs_6b6d / vs_82e1 have input access chains, the pattern behind futari's garbled
     sprites (fixed in futari's renderer, `app/src/me/futari/me_rectangle_spirv.h`, not in its translator; this port's
     `app/src/me/masseffect/me_rectangle_spirv.h` lacks that fix).
  Everything else of the library is reproduced byte for byte (with the port's dumps standing in for the compiler, and
  the tested SPIR-V, the library and index are identical).
- **Decision (the user, 2026-10-14): the installer builds the shader library itself; the port changes after the PGO
  collection** (not before: no app/sdk change between the PGO generate and use builds). To do then:
  1. futari-nx's access-chain change in `app/src/me/masseffect/me_rectangle_spirv.h` (the rectangle-list expansion
     accepting access chains into inputs; futari's `app/src/me/futari/me_rectangle_spirv.h`), so the WebAssembly
     DXC v2025.1's SPIR-V draws sprites right;
  2. the NRO stops needing the three shaders the game compiles at boot (Bink's YCrCb -> RGB pair and its vertex
     shader, plus vs_3df8, vs_3cae's fetch-patched variant): port-written replacements in the NRO, keyed by the
     compiled containers' hashes (numbers only; no game shader code in the NRO);
  3. the four wrapped Direct3D shaders get wrapper templates from the containers stored in `default.xex`
     (vs_6b6d / ps_a17c) in `tools/wrap_discovered.py` and the installer's `lib/shaders.js` alike (the renderer
     finds them by microcode through the library's code index, so the keys do not change; the linking must be
     checked on the PC);
  4. the release library = the one the installer makes (`test/library_node.mjs`), tested on the PC (videos, sprites,
     every screen) and on the console with the final NRO; then `test/release_node.mjs --write`.
  The conversion in Chromium itself (test/browser_convert.html, the archives over HTTP range requests, OPFS for the
  683 MB of intermediate data) gave the same content fingerprint as Node: 2cdb0100b22d763f...
- **1080p art in the installer: byte for byte the delivered pack.** `tools/hd/make_recipe.py` (new; `matching.py`
  gained `whole()` and returns the rectangles of `refit` / `by_picture`, build_hd_pack.py only bookkeeping; its
  output checked unchanged on three groups) writes `out/hd/recipe/peggle2_hd_recipe.json.gz` (402,331 bytes: per HD
  texture its key, sizes, expected hash and placed patches; no texels), from game_root_one with `--names
  out/hd/pack6/peggle2_hd.txt` (12 textures are in several groups; build_hd_pack keeps whichever group finishes first,
  the recipe follows pack6's choice; the others give the same images). The installer's `wasm/hdpack` rebuilt all 362
  textures with every image hash equal and the file identical to `out/hd/pack6/peggle2_hd.pack` (845,996,772
  bytes). What had to be matched: Pillow's bicubic transform (rows in float32, columns in double, a = -1), NumPy
  2.5.3's summation orders (pairwise over a last axis of 16, sequential over middle axes; fill_colours' colours
  ((00+01)+10)+11, weights (00+01)+(10+11) except one-texel-wide levels), OpenBLAS 0.3.34's sgemm on this Zen CPU
  (an FMA chain over k for the 3x16 by 16x3 product; sgemv without FMA). Caveat: a Python rebuild of the pack on a CPU
  where OpenBLAS picks another kernel (AVX-512) may differ; rerun `test/hdpack_node.mjs pack` then. Node, 12 workers:
  132 s.
- **The installer end to end** (`test/package_node.mjs`, the tested shader library standing in for the browser-made
  one): zip CRCs good, `peggle2_hd.pack` identical to the tested one, game pack 34 files of which the 33 unchanged
  ones identical to the game's and `main.rsb` the same content as build_mod.py's (550 packets); 489 s on this PC.
- **Shader library without the run-time compiler: ready on the installer and tools side, port patch prepared**
  (2026-10-14). The page's library from `default.xex` alone: 8 entries (the 3 stored containers, vs_6b6d's
  fetch-patched variant vs_6950, Direct3D's 4 own shaders wrapped with vs_6b6d / ps_558c as templates; the wrapper
  overwrites every header word the translator and renderer read, and the 4 wrapped shaders' SPIR-V is the old entries'
  by microcode), library 84,072 bytes `ce8dab77...c64a`, index 3,688 bytes `212fcf68...2e84`. PC path:
  `tools/build_shader_library_static.sh` (new; `tools/extract_stored_shaders.py` new, `wrap_discovered.py` gained
  `--template-vs/--template-ps/--immediate-only` with the old behaviour unchanged by default, `build_shader_library.sh`
  takes `OUT=`; backups of the two edited files in `installer_work/shaders/tools_backup/`): the same 8 containers and
  HLSL; native DXC 1.8.2407 gives other SPIR-V for 4 entries (packing the page's containers with the PC's SPIR-V gives
  the PC's library byte for byte). The port patch, NOT applied (PGO collection pending), in
  `D:\peggle 2\installer_work\shaders\app_patch\` (README, two .patch files, `patch -p1 --binary`):
  a) futari-nx's four access-chain hunks of `me_rectangle_spirv.h` (the unpatched header declines v2025.1's vs_6b6d;
     patched, the expansion passes spirv-val);
  b) `peggle2_port_shaders.cpp`: the three compiled shaders (and vs_3cae's fetch-patched form) drawn with the port's
     own SPIR-V (`shaders/port/peggle2_movie_{vs,ps}.hlsl`, written from what the shaders do, no game code), found by
     the compiled containers' XXH3 after a library miss; entries numbered from 65536.
  Checked: compiles with both DXC versions, spirv-val, the interface equal to the translator's for those shaders.
  Not checked: no PC or Switch build or run yet (the patch README lists the PC test: intro videos with and without
  alpha, the PS4 videos, menus, sprites).

## Release 1.0.0: the final build (2026-10-14, evening)

- **Profile**: the user's one PGO session (`peggle2_pgo_1.stream`, 3,196,794 bytes; kept in
  `out/pgo_streams/1.0.0_20261014/`), merged by `tools/win/pgo_merge.ps1` into 164 `.gcda` files in `pgo/`.
- **Shader patch applied after the merge** (`installer_work/shaders/app_patch`, both parts; backups of the five
  changed files in `out/backup_20261014_final/`): futari-nx's access-chain change and the port's own movie shaders.
- **Switch**: `build_switch.ps1 -Jobs 8 -Pgo use` (LTO): 170 compile rules with `-fprofile-use`, none instrumented;
  `peggle2.nro` 51,871,306 bytes, SHA-256 `8586dcf7...0271` (ELF `out/nx/elf/peggle2_1.0.0_lto_pgouse_20261014_1842.elf`);
  `tools/check_switch_build.py`: all checks passed.
- **PC with the installer's library** (`mod_test.ps1` on game_root_one, and on a test archive whose boot logo video is
  a trial intro, `out/ps4_dlc/game_root_alphatest`, never for delivery): the library loads; the port's movie VS, the
  YCrCb PS and the YCrCb + alpha PS attach (n65536-65538); VS selection audit 0 missing, 0 draws without shaders;
  sprites, menus, play and the alpha video look right. One difference from the tested build: one immediate load of the
  movie VS's fetch-patched microcode at the first video frame is counted unidentified (`unidentified VS IM_LOAD ... 24
  words`) because the port registers the patched form at the object's second sighting, one frame later (the tested
  library had it from the start as vs_3df8); no draw went without its shaders. Not checked: the console.
- **Release package made by the installer's own code** (`test/package_node.mjs`) from the two ORIGINAL packages (the
  STFS package 2452FB99... read in place, the PS4 base .pkg decrypted in place), with the browser-made shader library
  and nothing standing in: `out/dist/release_1.0.0/peggle2-nx.zip` (2,783,496,712 bytes, 476 s). Checked: zip CRCs;
  NRO, library and index the manifest's; `peggle2_hd.pack` byte for byte pack6; the game pack's 33 other files the
  game's; its `main.rsb` the same content as build_mod.py's (550 packets). The installer's `release/manifest.json`
  now names this NRO, the library `ce8dab77...` and the recipe `239ebb2e...`.
- **To do by the user**: the console run of the zip; then publishing the peggle2-nx release with `peggle2.nro` and
  `peggle2_hd_recipe.json.gz` as assets (the installer's deployment downloads both and checks their SHA-256).

## banjo-nx's codegen round (2026-10-19)

The user asked for banjo-nx's new codegen improvements, all at once, then verified. Source: banjo-nx's
`docs/codegen-enhancements.md` (steps 15-33) and its SDK. Peggle's 1.0.0 had steps 1-14 (sa2-nx's rounds).

**Generator** (the SDK, ported by a helper agent; default output byte-identical to before, 241 files, old and new
generator; 13 SDK tests pass, the 4 new ones included: `vec_unaligned_test` 640,000, `memcalls_test` 403,559,
`fp_sign_ops_test` 96 M, `single_floats_test` 60 M comparisons, 0 differences):

| Change | In Peggle | Effect |
|---|---|---|
| `dcbt` operands decode as {CT, RA, RB} (`emit_prefetch`) | fix in; `dcbt_prefetch` stays off | (only with the option) |
| `rotate_word_shifts` | on | 8,171 functions; rlwinm / rlwimi as one shift / extract where the mask allows; all 54,370 sites' 12,343 distinct statements checked against the 64-bit definition |
| `fp_copy_forwarding` | on | copy-form stores 30 -> 5,328, loads 178 -> 3,648; flush-mode check statements 11,399 -> 10,296 |
| `setjmp_save_locals` (new option; sa2-nx's fix, always on in banjo-nx) | on | a CORRECTION: the 12 setjmp sites copy the non-volatile locals into the jump buffer and back after a longjmp |
| `non_volatile_as_local_keep` / `_exclude` | mechanism only | not used |
| FPSCR rounding census (`kCensusRoundNearest/Zero/UpDown`, `PPCFPSCR::storeFromGuest`) | in (PC) | the `[census] FPSCR rounding mode` line |

**Post-codegen steps** (`tools/post_codegen.py`; each with its CMake switch back, `PEGGLE2_GEN_*`; counts of the
fresh codegen):

| Step | Tool | Count |
|---|---|---|
| narrow loop barrier (the guest window, 4 GB + 4 KB, not all memory) | `loop_barriers.py` | 20,205 barriers |
| a vperm of one vector as one lookup | `vperm_one_source.py` | 173 (831 two-source left) |
| fneg / fabs / fnabs as float operations | `fp_sign_ops.py` | 688 |
| leaf write-backs no caller reads skipped | `leaf_dead_stores.py` | 2,344 of 22,161, in 373 functions |
| single-precision arithmetic in float where the inputs are proven singles | `single_floats.py` | 8,649 of 15,955 |
| memcpy / memset of 1-64 constant bytes inline | `inline_memcalls.py` | 278 of 1,095 memcpy, 154 of 673 memset |
| stvlx / stvrx without byte loops | `vec_unaligned_stores.py` | 650 + 159 |
| read-only constants over the control-flow graph | `ro_constants_cfg.py` | 1,463 loads |
| guest register writes nothing reads left out (`--abi-indirect`) | `dead_ctx_defs.py` | 4,556 plain + 1,782 record forms, 2,931 functions |
| loop-header flush checks hoisted in front of the loop | `flush_hoist.py` | 260 loops in 133 functions |
| link-register write-backs proven redundant (STRICT, see below) | `lr_preserve.py` (+ `frame_traffic.py` marks) | 2,617 of 13,098 sites |
| VMX vsel as one NEON bit select | `vsel_neon.py` | 87 |
| tiny leaves of other files as inline copies | `inline_leaves.py` | 4,779 calls to 2,669 copies |
| locals opt-out list (A57 cost, `locals_choice.py`) | `app/config/locals_optout.txt` | 82 loop + 27 leaf functions |
| r1 as a local mirror in the chosen functions (`app/config/r1_local_choice.json`) | `r1_preserve.py` + `r1_local.py` | 12,637 functions |
| unchanged generated files keep their times | `post_codegen.py` (`keep_unchanged_times`) | |

The localized copies (`localize_native.py`) now come after the round, so the 47 copies get it too.

**The two choice lists** (`tools/locals_choice.py`, banjo-nx's A57 cost model: every changed file compiled twice with
the Switch dev build's command line, per function the loop-weighted cost sum(w * (1 + is_mem)), w = 10 inside a cycle
of the control-flow graph; static, not samples). Peggle's copy got `--mode r1 | lr | round` (form B with that step's
switch back; the modes share form A with `--reuse`). Measured on a copy processed with `PEGGLE2_R1_LOCAL_ALL=1`
(`out/choice`):

- loop / leaf locals: the 4,029 changed functions cost 6.73 % less with the locals than without; 109 are cheaper
  without by more than 2 % (82 loop, 27 leaf; the worst `sub_8252B368` +15.8 %, `sub_825255E0` +40 %): opted out,
  -0.49 % more.
- r1 mirror (`r1_local.py`, 24,552 candidate functions): everywhere a loss, +0.41 % (banjo-nx: +0.33 %); cheaper in
  12,637 (`--threshold 0`), which are the list: -0.76 % against no mirror. On the PC every assumed call is checked
  (`REX_R1_CHECK`).
- `lr_write_same_choice.json`: not made (`--mode lr` exists, not run); every strictly proven site is kept.

A change of either list needs a fresh codegen (`tools\codegen.ps1`): the locals tools never undo a function.

**Not taken**: `hot_layout.py` (the release build's PGO already places the hot functions); `switch_goto.py` (Banjo's
script interpreters; none in Peggle's hot list); `limiter_lazy_log10.py` (XAudio's limiter; Peggle's audio is
Wwise); `store_cards.py` (Banjo's renderer); `dcbt_prefetch` (not measured); `HostToGuestFunction` always-inline
(runtime hook shim, not codegen).

**What had to change for Peggle** (beyond paths: `app/generated/default`, `peggle2_pch.h`, `out/default_image.bin`,
the entry point `xstart` 0x823186D0, the CRT memcpy `sub_825F0AE0` / memset `sub_825F10B0` in the new
`app/src/cpu/hot/hand.txt`):

- **The game patches are opaque.** `mp_masters.py`, `fps_updates.py` and `costume_states.py` insert host calls with
  `&ctx` (one runs guest code), gotos the shared parser misreads (`{ ... if (..) goto loc_X; }` taken for an
  unconditional goto) and a label it does not know (`peggle2_costume_unlocked_only`). They now run right after
  `leaf_locals.py`, and every function with their `// peggle2-nx:` line counts as opaque
  (`leaf_dead_stores.OPAQUE_MARK`): each of its instructions reads every register (so its callees' LiveOut and its
  own UE are everything and nothing in it is dead), the forward analyses skip it (`single_floats`,
  `ro_constants_cfg`: "6 functions not analysed"), `lr_preserve` / `r1_preserve` take it for not preserving, and
  `r1_local`, `flush_hoist`, `inline_leaves` leave it alone.
- **A second run gave other code (found by a two-run hash check on a copy).** banjo-nx's `post_codegen.py` takes
  only its late rewrites back out first. On Peggle the round's earlier forms changed what the base steps decided on
  a second run: `loop_barriers.py` did not read a return written `{ REX_LEAF_STORE(..); REX_LEAF_DEAD_R(..);
  return; }` (10 files got other barriers), `flush_mode_flow.py` proved 12 more checks and `ro_constants.py` folded
  17 more vector loads, and `flush_hoist.py` then hoisted 293 loops instead of 300. New `tools/round_restore.py`
  undoes the rest of the round (each the inverse of its tool's rewrite) before the base steps. And
  `inline_leaves.py --restore` left the empty line it had inserted, one more per run in every file (fixed). Now two
  runs give the same 122 files byte for byte.
- **`lr_preserve.py --assume-indirect` is not true here.** Functions that end through `__restgprlr` (the generator
  drops the helper) never write ctx.lr back, and indirect and hooked calls reach them. The PC check
  (`REX_LR_WRITE_SAME_CHECK`) found 1,473 of 68 M checked write-backs wrong in 150 s with the assumption (7,603
  sites). Strictly (no assumption about indirect, hooked or import calls): 2,617 sites, 0 of 35 M wrong. Little
  reads ctx.lr in Peggle (its logging hooks, whose call sites set it; the localized copies' guard compares it), but
  the proof has to hold.
- **The localized copies must not depend on when they are made.** CMake's configure step makes them again when
  `localize_native.py` or `localized.txt` is newer than the hooks file, on the final tree; the Switch cross build
  (no codegen step) failed on copies that called `inline_leaves.py`'s `inl_sub_X`. `localize_native.plain()` now
  takes every later form back out (REX_INL calls, REX_LR_WRITE_SAME, the r1 mirror, REX_VSEL, the memcall wrappers,
  REX_STVLX / REX_STVRX as the logged byte loops) and `unleaf()` drops REX_LEAF_DEAD_R / _F like the stores: copies
  made from the final tree equal those made in the pipeline (47 of 47, only the source line of the header differs).
- `direct_calls.py`: banjo-nx's `#` comments in tomls and CMakeLists.txt are no hooks; `non_argument_as_local_keep`
  in `perf_overrides.toml` neither. `leaf_locals.py` / `loop_locals.py`: banjo-nx's opt-out list (Peggle already had
  vector registers as values, banjo-nx's `REX_VECTOR_LOCALS_BY_VALUE`). The two audits got banjo-nx's fixes.
- PC checks of the late proofs: `REX_LR_WRITE_SAME_CHECK` and `REX_R1_CHECK` (non-Switch builds, as RexRoCheck),
  counted in `src/cpu/peggle2_codegen_checks.cpp` for a `[verify]` line.

**Verification (PC unless said)**. The final build (fresh codegen with both choice lists): verify mode 0 DIFFERENCE
/ MISMATCH lines; guard checks 137 M, flush checks 344 M, read-only folds 42.7 M, the r1 mirror checked after 67 M
calls and 38.6 M link-register write-backs, all 0 differences; lockstep 123 of 123 (117 at the same game frame) in a
second capture (the first, while another port's game and a build ran, missed 17 frames of the menu fade-out, the
host-timed frames the notes know). The PC build's own codegen re-run gave 297 of 297 generated files identical to
`tools\codegen.ps1`'s. Switch dev NRO: `out/dist/dev_20261019_banjo_round/peggle2.nro` (48,569,930 bytes, SHA-256
`a185b338...07f3`; ELF `out/nx/elf/peggle2_banjo_round_choice_nolto_20261019.elf`), `check_switch_build.py` all checks
passed. A57 census of the recompiled code against the last no-LTO build before 1.0.0: instructions -0.81 %, loads
-3.28 %, `msr fpcr` -11.0 %, `bl` -3.0 %, stores +0.04 %, sp loads + stores +2.2 % (the r1 mirror's spills). A release
needs a new PGO profile (the code changed) and the console run.
The earlier stages of the same work (before the choice lists):

- Verify mode, 150 s scripted route, fresh save: 0 DIFFERENCE / MISMATCH lines; guard checks 147 M, 0 differences;
  354 M proven flush checks executed (the hoisted ones included), 0 had to switch; 47 M read-only folds (both
  steps), 66 M promoted calls, 0 differences; 35 M link-register write-backs proven redundant, 0 differences.
  (A first run with the assumed lr proofs also showed one native WindowScissor record difference; it did not come
  back in the next run, 63,426 compared.)
- **Poison build** (`PEGGLE2_GEN_LEAF_DEAD_POISON=ON`, `PEGGLE2_GEN_DEAD_DEFS_POISON=ON`: every skipped write-back
  and left-out write gets 0xDEADBEEF... / a signalling NaN): verify mode 0 differences (121 M guard checks); lockstep
  against the CPU-off baselines `ls_fresh_off1/2`: **123 of 123** reproducible frames (107 at the same game frame),
  none missing.
- Normal build lockstep: **123 of 123** (119 at the same game frame), none missing, during a background compile
  (1.0.0's final build: 122 of 123).
- FPSCR census (fp_sign_ops is exact only without round-up / round-down): 54 guest writes, all to nearest; 0 toward
  zero, 0 up or down.
- PS4 content (`mod_test.ps1`, `game_root_mod_mp`): menus to Bjorn's first level, no DIFFERENCE. 60 fps
  (`peggle2_fps = 60`): 57.8-60.9 logic updates a second (the frame-step patch).
- Switch dev build (`out/nx_dev`, no LTO / PGO; `out/nx/elf/peggle2_banjo_round_nolto_20261019_2013.elf`):
  `check_switch_build.py` all checks passed (one `b .`, the known guest halt; no vector fused multiply-add; no
  self-indexed table lookup; `-mearly-ra=none` in all 688 compile commands).
- A57 census of the recompiled code (`sub_*`, `__imp__sub_*`, `inl_sub_*`; static counts, not weighted by how often
  they run), against the last no-LTO build before 1.0.0 (`peggle2_guardoff_nolto_20261014_1438.elf`): instructions
  -0.11 %, stores -0.30 %, sp loads + stores -0.29 %, `msr fpcr` 6,297 -> 5,614 (-10.9 %), `bl` -2.9 %; loads
  +0.55 % and `blr` +3.5 % (inline copies of memcpy / leaves). At run time the PC census shows the same ~140,000
  flush-mode switches a second on the route as on 2026-10-13: the removed `msr fpcr` are not on its hot paths.

## Renut's renderer on deko3d (2026-10-19/20, night)

The user asked for "a fully functional deko 3d native renderer with no ring emulation or edram emulation", based on
banjo-nx (its emulator code and its optimised shader generator), tested in the emulator banjo-nx tests in. The renderer
is **Renut's Direct3D-level renderer** — reNut by masterspike52 (the author permitted its reuse in the user's ports),
as banjo-nx carries and has optimised it (`app/src/renut_engine/gpu`, plume on Vulkan / deko3d). It hooks Peggle 2's
Direct3D functions and draws their calls directly: no PM4 ring is written or parsed (ring writes go to a sink), no
EDRAM is modelled (render targets are host textures), fences complete on the render thread.

**Choosing it** (one NRO, both renderers; the built-in default stays the native renderer):

| Key | Value | |
|---|---|---|
| `peggle2_renderer` | `"renut"` | read once at boot (`"native"`: the native renderer, as before) |
| `renut_gpu_backend` | `"deko3d"` (Switch), `"vulkan"` (PC) | |
| `masseffect_renderer_native` | `false` | |

`app/configs/peggle2_renut_deko3d.toml` (written by `tools/make_enhancement_configs.py`) is every enhancement on, logs
on, these three keys, and the release's picture: 720p handheld, 1080p docked, the HD atlases always. Renut follows
Peggle 2's own `peggle2_render_height_handheld` / `_docked` (`peggle2_renut_resolution_by_mode`) and shows the HD
atlases as the native renderer does (below).

### How it was brought in

- **Files from banjo-nx** (its copies, with banjo-nx's changes): `app/src/renut_engine/gpu/**`, `display_sync`,
  `perf/frame_stats`, `app/thirdparty/{plume,uam,reblue-XenosRecomp,imgui}`, `app/cmake/collect_captured_shaders.cmake`,
  the deko3d tools (`tools/deko3d_*.py`, `renut_early_alpha.py`, `renut_switch_shader_cache.py`), the SDK's
  `frame_latency`, `MarkStoreCardsAt` (unused: Peggle has no store cards, `g_banjo_store_cards = false`). Build glue:
  `app/cmake/peggle2_renut_gpu.cmake`, `app/src/renut_engine/peggle2_renut_glue.cpp` (banjo-nx's keys and symbols the
  tree expects), `option(PEGGLE2_RENUT_RENDERER ON)`.
- **Hooks** (`tools/renut_hook_remap.py`, `out/renut_port/hook_map.md`): Renut's 371 hooks are at N&B's addresses;
  319 were matched to Peggle's functions by shape, 42 have no Peggle counterpart (tiling, command-buffer recording,
  Present, ...: kept under `#if 0`), 10 are addresses Peggle's native renderer hooks too (`BANJO_RENUT_HOOK_SHARED`:
  the native hook calls Renut's side when it is chosen, `tools/renut_yield_hooks.py`). Swap keeps its frame count.
- **Device layout** (`out/renut_port/device_offsets.md`, a helper agent's re-derivation from 356 paired functions):
  Peggle's XDK 21256 has the same public device (0..0x2A70) as N&B's but MaxAnisotropy / ZFilter moved into it, and
  every private field moved by +128..+412 (no single delta). `tools/renut_device_offsets.py` rewrote the 176
  hard-coded offsets (152 lines; e.g. kDeviceAllocSize 0x5E00 -> 0x6080, VertexDeclaration 0x2E2C -> 0x2FD0,
  kDeviceSnapshotSize 0x31A8 -> 0x3320) and the guest data addresses (state tables 0x828DFDB8 / 0x828E0278, format
  table 0x8207A590, caps 0x8207AA80, HiZ owner 0x82D7509C, XMemFree sub_823189E8, GetCurrentThreadId sub_8260BF88).
- **Where Peggle's Direct3D differs from N&B's** (each fixed in Renut, marked `peggle2-nx`):

| What | Symptom / reason | Fix |
|---|---|---|
| The post-codegen direct-call pass had turned calls into `__imp__sub_X` before Renut's hooks existed | Renut never ran (the real CreateDevice called `VdInitializeRingBuffer`; "Record heap used before Init"; draws with vertex shader 0) | `tools/direct_calls.py` skips `#if 0` blocks (the disabled N&B hooks named unrelated Peggle functions); codegen run again; 0 calls bypass any of the 319 hooks |
| Public `D3DDevice_Resolve` is a wrapper (sub_82622140) around an internal resolve with two more arguments | every resolve argument shifted (destination 0): Swap found no front-buffer texture, nothing presented | the hook sits on the wrapper |
| SetFVF (sub_82613570) rebuilds the declaration inside the device (+0x3118) | "Draw dropped: vertex declaration ... not one the renderer knows" | `render/draw.cpp` DeviceDeclarationLocked: its elements from the draw's device snapshot |
| A 32-byte block at device+11028 (InitializeHardwareDevice) | unhooked fence / interrupt paths dereference it | Renut's device gets one |
| SetTexture(NULL) clears the fetch constant's type bits; BeginExport always draws the NaN vertex when no export is active | | as Peggle's code |
| Renut's SynchronizeToPresentationInterval does not wait | 600-1,200 frames a second with a minimised window | Peggle's Swap hook waits for the interval the game set (+14000; `peggle2_renut_swap_pacing`) |
| Renut never applies the game's display gamma ramp (present exponent 1) | brighter darks than the native renderer (19 -> 35 of 255) | the ramp's least-squares power (x^1.172 for Peggle) in the present (`peggle2_renut_display_ramp`) |
| Without store cards every never-unlocked linear texture is hashed in full each frame it is bound (Peggle's 4096x4096 atlases: 72 MB a frame) | 71 % of the render thread (PC profile: `XXH3_hashLong` in `ContentHash`) | rechecked after 2, 4 ... 32 frames while unchanged, as the native renderer does (`peggle2_renut_texture_backoff`); the loader's row copy reports its texture (`src/peggle2_texture_writes.cpp`) |
| Renut's render scale is `renut_resolution`, and deko3d is held at 1x (banjo-nx's shaders lose the scale code in its `scale1` / `fragscale` passes) | 720p docked | the scale follows Peggle's height for the console's mode (`render/surfaces.cpp`, `peggle2_renut_resolution_by_mode`: 720 -> 1x, 1080 -> 1.5x), deko3d included: Peggle's six shaders read no scale code and no `resolution_scale` (checked in their GLSL), and plume binds a texture word's low 20 bits only |
| plume's deko3d swapchain takes the default NWindow's size, 1280x720 in both modes | a 1080p frame scaled down to 720p, then up by the compositor | `plume::g_deko3dWindowSize` (set in `peggle2_renut_glue.cpp` from `peggle2::render::Docked()`): the window is given 1920x1080 docked before the swapchain is made |
| Bink decodes the next video frame into the plane set the frame just submitted draws; Renut reads texture data on its render thread after the Swap (one frame may still be queued) | the native renderer's "spots in videos" of 2026-10-13, here possible too | the video update (`src/peggle2_texture_writes.cpp` VideoUpdate) drains Renut's render thread first, as it waits for the native ring; videos only (the intro logos played frame by frame right at 1-second shots) |
| The HD atlases are the native renderer's (its texture upload) | the 720 art at 1080p | `render/textures.cpp` UploadMaybeHd: a 2D linear BC3 one-level texture's XXH3-64 (the same bytes as the native's key) looked up in `peggle2_hd.pack` at each content upload; the host texture is made at the HD size and filled from the pack in 8 MB bands, and back at the guest size for content the pack lacks |

- **Shaders**: Peggle 2 has 6 (4 pixel, 2 vertex); three are stored in the XEX, the Bink movie trio is compiled at
  boot by D3DXCompileShader (corrected later the same day; `tools/renut_shader_dump.py` dumps them). banjo-nx's
  generator: XenosRecomp -> SPIR-V (PC cache) -> SPIRV-Cross GLSL -> `deko3d_glsl_opt.py` -> `deko3d_shader_package.py`
  (`deko3d_glsl.bin`, in the NRO; its zstd dictionary is the shaders' own text when 6 samples are too few to train on).
  uam compiles the variants on the device: 9, kept in `deko3d_shaders.bin` next to the NRO (7.5 KB).
- **Ryujinx** (banjo-nx's fork, this port's own data folder `D:\New folder\ryu-data\peggle2`, SD folder a junction to
  the existing one): four SDK changes from banjo-nx let the NRO run without a loader — the NRO's usual SD path when
  argv is empty (`REX_SWITCH_FALLBACK_EXE`), the current-process pseudo-handle, guest-memory views mapped at commit and
  no page protection (no data aborts reach homebrew in Ryujinx), and the heap from all free memory. On a console
  hbloader passes argv, the handle and the heap, so nothing changes there.

### Results

- **PC, Vulkan** (`out/pc`, minimised, shots every N s through `peggle2_shots_every_s`): the scripted route from a fresh
  save (EA and PopCap videos, the autosave notice, title, Bjorn's map, the first level and its tutorial), and a play
  route from `seed_noach` (A every 2.5 s: menus, Adventure, Bjorn's map, a level played with shots, power wheels,
  "3 / 2 BALLS LEFT!", "LAST BALL!", the retry dialog). 30 pictures a second; no draw dropped, no Swap without an
  image. Render thread 0.5-0.9 ms and GPU 0.9-1.8 ms a frame in a level (the render thread was 8 ms before the
  texture back-off). Against the native renderer, same route, a static tutorial frame: mean difference 3.4-5.2 of
  255 per channel with the display ramp (14.5 without), the rest is animation timing.
- **Ryujinx, deko3d** (the NRO of `out/dist/dev_20261020_renut_deko3d`): the same two routes, frame for frame what
  the PC shows. The 9 shader variants are compiled once by uam in the NRO (4-23 s each under emulation at first; ~0.4 s
  on a console by banjo-nx's measurements, ~0.6 s in its emulator runs) and read from `deko3d_shaders.bin` after. 15-30 pictures a second and a render
  thread of 1-5 ms a frame under emulation (16 ms before the back-off). Vertex colours were red/blue-swapped in the
  emulator only (it ignored Maxwell's vertex-format swap bit, deko3d's `isBgra`, which plume sets for D3DCOLOR;
  banjo-nx patched the shared fork at 01:44 and the frames now match the PC's). One of six runs ended with Ryujinx
  itself failing (0xC0000005 in its JIT, right after Single Player, while three other ports ran instances): not seen
  again in the next runs of the same route. In its HostMappedUnsafe mode a guest access to an unmapped page ends the
  emulator, where a console's fault handler maps the page (RexGmFaultIn) — so a stale read of freed guest memory
  would look like this; not investigated further.
- **The native renderer is unchanged**: verify mode 0 differences (142.7 M checks; 0 DIFFERENCE / MISMATCH lines);
  lockstep against the CPU-off baselines **123 of 123** (105 at the same game frame), none missing (a first run
  that overlapped a Ryujinx run gave 89 of 123: load, not a change). Again on the final build (with the 1080p and
  HD-atlas work): verify 174.8 M checks, 0 differences; lockstep 123 of 123 (108 at the same game frame).
- **Screens seen through Renut** (PC and Ryujinx): EA / PopCap videos, autosave notice, title, main menu, Adventure,
  Peggle Institute, Select a Master, Bjorn's map, level intro, the tutorial, play (shots, bucket, power wheels, "N BALLS
  LEFT!", "LAST BALL!"), "TRY AGAIN" with its rainbow, the retry; later the same day (PC) Extreme Fever, VICTORY and
  the change of level (below). Not reached by the scripted routes: the multiplayer lobby (the main menu takes no
  scripted DOWN or stick press there).
- **Switch build** (`out/nx_dev`, no LTO, no PGO): `check_switch_build.py` all checks passed. A57 census against
  2026-10-19's dev ELF: game code +0.18 % instructions, +0.26 % `bl`, +309 functions (the hook dispatchers: one
  byte load and a branch before the original when the native renderer runs); the rest of the program +17.6 %
  (Renut, plume, uam); NRO 48.6 -> 52.1 MB.
- **1080p and the HD atlases**: PC, simulated docked mode (`peggle2_render_docked`): Renut at 1920x1080 against
  the native renderer at 1920x1080 (both without the pack): mean difference 4.5-5.1 of 255 per channel, the HUD and
  text as sharp; with the pack (pack6) 16 atlases a level shown from it (1024x2048 -> 1536x3072, 4096x4096 ->
  6144x6144, 7-29 ms each to read). Ryujinx docked (the profile's `docked_mode`, set for one run and put back):
  swapchain 3 x 1920x1080, render 1920x1080, 23-30 pictures a second; handheld with `peggle2_hd_atlases = 2` (the
  release's setting): 16 atlases from the pack, 20-29 pictures a second, the frames right.
- **Delivered**: `out/dist/dev_20261020_renut_deko3d/` — `peggle2.nro` (52,166,218 bytes, SHA-256
  `58994fd6...7ab01de8`; Ryujinx with it: intro to a played level with 22 HD atlases, up to 30 pictures a second), `peggle2_renut_deko3d.toml` (copy it to the SD as `peggle2.toml`), `deko3d_shaders.bin` (the
  9 variants: beside the NRO, skips the first compiles), `README.txt`. ELF
  `out/nx/elf/peggle2_renut_deko3d_nolto_20261020.elf`; `check_switch_build.py` all checks passed on it. The test
  runs' logs and shots: `out/renut_tests/{pc,emu}` (raw shots are about 3.6 MB each at 720p: moved off C:, which had
  filled up; delete them when no longer wanted).
- **Test configs wrote no log since 1.0.0** (found here): the NRO's built-in settings have `log_to_file = false` and
  the configs of `tools/make_enhancement_configs.py` set only `log_level = "info"`, so a "logs on" config made no
  `logs/` folder (seen in an emulator run). Their log section now has `log_to_file = true` too (one line added to 23
  configs; the built-in settings and the player's `peggle2.toml` are unchanged).
- **A slow stretch in one emulator run was the machine, not the build**: one Ryujinx run of the delivered config fell
  from 24-29 to 1-4 pictures a second after 180 s of play (ordinary gameplay frames, no memory growth). The same
  route then held 18-29 with the console stack sampler off and 16-29 with it on (the delivered config as is), each
  for 7 minutes; other ports' emulator instances share this PC (one was running at the end of the last run).
- **Taken from banjo-nx at the end (2026-10-20)**: `banjo_renut_device_swizzle` in its final form — a texture
  whose header describes the same data as the device's fetch constant but differs in dword 0, 3 or 5 (which SetTexture
  copies unchanged) is a stale header: the device's copy (what the GPU reads) is used (banjo-nx's whole-frame yellow
  flash: a stack-temporary header rewritten with another swizzle). Peggle's routes logged no `[flash]` line, so the
  picture is unchanged here.
- **HD atlases read on a worker thread** (`peggle2_renut_hd_async`, on): a level start binds a dozen atlases, and
  reading them (LZ4 into up to 36 MB of BC3 each) on the render thread stalled its first frames; now the "HD atlas
  reader" (priority 0x3B, core 2 like Renut's other background threads) reads them and the game's 720 art shows for
  the few frames until each is ready (at most 256 MB of read images wait). false = read at once, as the native
  renderer does. (A first attempt detached the thread: `std::thread::detach()` throws on the Switch's pthreads and
  the NRO aborted in Ryujinx; the thread is now simply never destroyed.)
- **Docked soak with HD atlases** (Ryujinx, 1080p, ~4 minutes of play): the upload rings peaked at 128 MB during atlas
  loads and went back to 64 MB; the host heap leveled at ~1.93 GB (1.47 GB without HD), textures 375 MB. It ended when
  the PC's C: drive filled up (Ryujinx could not write a shot), not in the game.
- **A texture memory cap for deko3d** (`peggle2_renut_texture_cap_mb`, 512): a 10-minute docked HD soak in Ryujinx
  went from 68 to 601 MB of host textures (11 -> 35), and none was ever released: Peggle 2 keeps earlier levels'
  atlases alive, and Renut's cold-texture policy (banjo-nx's `banjo_renut_memory_policy`) compares against the GPU
  memory budget, which deko3d reports as 0 (the Switch's memory is the process's), so it returned before releasing
  anything. Without a budget it now compares the host textures' own total with the cap and releases the ones not
  bound for 120 frames (made again from guest memory, or the pack, if bound again). Soak with a 384 MB cap: the total
  held at ~345 MB (12, then 5 cold textures released), 22-30 pictures a second for the 10 minutes.
- **Docking and undocking while playing** (PC test switches `peggle2_render_test_toggle_at` / `_every`, which only
  the native ring's Swap called; Renut's Swap now calls `peggle2::render::OnPresented` too, a no-op on the Switch):
  13 changes in 200 s (720 <-> 1080 every 450 swaps, menus and a played level), each at the next frame, every frame
  right (`out/renut_tests/pc/toggle1/sheet.png`). On the Switch the applet's mode message goes through the window's
  `HandleDisplayModeChange` to `OnWindowPixelSizeChanged`, Renut's `RequestResize`, and plume's deko3d `resize()`,
  which sizes the swapchain by `peggle2::render::Docked()`.
- **banjo-nx's shader and render-thread work of 2026-10-19/20, taken at 05:50-06:50** (a read-only survey of both
  trees first: `out/renut_port/banjo_optimisations_survey.md`; backups and patches in
  `out/renut_port/backup_20261020_shaderopt/`). Everything banjo-nx documents as a render-thread / plume CPU
  optimisation (memos, pushed constants, ring cleaner, kick submits, barrier merge / lean, depth hand-over, resolve
  skip, proposals 1-13, 16, 18, 20-23) was already here with the same defaults. Taken now:

  | What | Where | Effect on Peggle 2 |
  |---|---|---|
  | `tools/deko3d_glsl_opt.py` and `deko3d_variant_cache.py` as banjo-nx's (27 passes; 9 new: deadcomp, guardsel, lightbranch, nullps, txq, earlyz, gpr32, bonesel, aw4) | tools | deadcomp: 1 change (ps_23FB, an undefined `_67.w` -> 0.0); earlyz: all 4 pixel shaders; the others find nothing in Peggle's 6 shaders |
  | early depth test (constant 909, `banjo_deko3d_early_z`, on) | `render/pipeline.cpp` | every Peggle draw gets 909 = 1, but its RB_DEPTHCONTROL is 0x00700730 (depth test off): no GPU change here; free |
  | texture sizes from a buffer (constant 908, `banjo_deko3d_txq_constants`, on) | plume deko3d, `render/pipeline.cpp` | no `textureSize` in Peggle's shaders; there for safety of the generator |
  | uam: banjo flag 1024 (128-bit row loads), per-compile GPR cap, cache tag bnx4; banjo-nx's split-load fix (06:07: a split global load lost its address) | `app/thirdparty/uam` | none on Peggle's shaders; the DKSH cache was rebuilt (9 variants, 7,556 bytes) |
  | constants 910-912 and in-run A/B targets 15 / 16 (off) | `render/pipeline.cpp`, `render/draw.cpp` | N&B's lists: inert here |
  | no re-present while the game stalls (`banjo_renut_stall_present`, false = new) + `[flash]` warning for a texture sampled with nothing written | `backend/present.cpp`, `render/textures.cpp` | no stale (uncleared deko3d) image shown during loads; no 16 ms re-presents |
  | latency trim (`banjo_deko3d_latency_trim`, OFF as in banjo-nx until a console measures it) | `backend/video.cpp` | Ryujinx with it on: no hang, 0-1 of ~150 frames held (render-thread-bound there) |

  Not taken: store cards (they need the guest window's first MB, which Peggle uses; the back-off covers the hot
  spot), fur-shell skipping (N&B, not exact), the exact gamma lookup (Peggle keeps its power fit), the report counters
  for header / device differences and the aw4 alignment check (diagnostics for passes that do nothing here).
- **The late controller read was never on** (found here): the SDK had `rex::latency::BeforeInputRead` /
  `NoteInputRead` but `InputSystem::GetState` did not call them, so `banjo_late_input` (on) delayed nothing and the
  `[latency]` line said "no frame presented". Both calls added (`sdk/src/input/input_system.cpp`, as banjo-nx's). PC:
  input -> present 10.5-15 ms at 30 fps, late input 0 ms (the frame cap is the limit there); Ryujinx: input ->
  present 19-24 ms, input -> GPU done 52-61 ms, late input 0 ms (the game thread is the limit there). On Windows an
  SDK change alone does not reach `out/pc`: `rexruntimerd.dll` is copied there only when the exe relinks.
- **The DKSH cache lost its last records at every close** (found here, shared code): the append `fflush`ed but did not
  `fsync`; Ryujinx kept the record in its host file buffer and a killed process dropped it, so every start compiled
  the same variant again ("8 hits, 1 added", the file's mtime unchanged). `fsync` after the record (libnx:
  `fsFileFlush`): the next run 9 hits, 0 compiles. A console closed with HOME is killed the same way. Told banjo-nx.
- **Barriers on deko3d are not a problem** (checked with `peggle2_renut_diag_transitions`, a diagnostic key, off): a
  level frame has two passes (the board at 896x736, resolved, then the screen, resolved to the front buffer) and 8
  layout changes, which plume's barrier merge turns into ~6.5 fragment barriers a frame. (The `[renut] deko3d:` line's
  draw, barrier and upload counts are totals since the start, not per 10 s.)
- **The pipeline list lost records the same way** (banjo-nx's finding after the DKSH report, taken at 07:35): its
  `std::ofstream` flushed but never synced. banjo-nx's `render/pipeline.cpp` now writes with `FILE*`, syncs (`fsync`;
  `_commit` on Windows) at most once a second, and `SyncPipelineListLocked()` (called after the present in the Swap's
  render-thread work, under the state mutex) syncs a burst's last records a second later; records written by older
  configurations are normalised to the current keys when loaded (no pipelines built ahead for no draw), and every
  run-time compile logs "uam: run-time variant (... ): defines". Ryujinx: the SD's `pipeline_list.bin` went from
  8,521 bytes (01:15, unchanged through every later run) to 9,466 (07:41); DKSH 9 hits, 0 compiles, 0 run-time
  variants.
- **Extreme Fever, the VICTORY screen and the level change, through Renut** (PC, Windy's first level won with the
  test switch `peggle2_dlc_test=firstorange`, the PS4 content's data root `out/ps4_dlc/game_root_mod_mp`, Windy's
  route of the DLC tests): the zoom-in, "EXTREME FEVER", the fever buckets, fireworks, "EXTRA BALL POINTS", "TOTAL
  POINTS", the white flash, VICTORY with its rainbow and objectives, the iris to the next level and its fly-in. The
  same game frames recorded with both renderers (`peggle2_video_from_frame = 4420`, 180 frames; hashes in
  `out/pc/captures/video_f4420_{renut,native}.hash`, sheets in `out/renut_tests/pc/windy_f4420b_*`) show the same
  sequence; a dark board seen in a 4 s sample was the iris's first frames, which the native sample had passed by
  a few frames. (`boot_test.ps1` clears `REX_PEGGLE2_VIDEO_FROM_FRAME` unless given `-VideoFromFrame`.)
- **From futari-nx (2026-10-20), not acted on yet**: Renut's own keys read their own cvars, so the CPU / non-CPU
  masters set to false leave most of them on (futari gave 52 of them rows in its enhancement table); Renut's kick
  submits and so the latency trim only act on a list of 200 draws (`kKickSubmitDraws`), and Peggle draws 20-35 a
  frame, so both do nothing here (as the emulator run showed: 0-1 frames held); futari's guest vblank thread now
  has banjo-nx's display-thread placement (priority 0x2C, core 2) — Peggle's vblanks come from the SDK's "GPU VSync"
  host thread, unchanged and the same as the native renderer's on the console.
- **Delivered at 07:43**: `out/dist/dev_20261020_renut_deko3d/peggle2.nro` (52,207,178 bytes, SHA-256
  `3dde60ab...fe28072`; `check_switch_build.py` all checks passed; ELF
  `out/nx/elf/peggle2_renut_deko3d_nolto_20261020_0743.elf`; the 06:52 build without the pipeline-list fix was
  `fbcb47bf...fd9750`) and its `deko3d_shaders.bin` (bnx4, 9 variants, `f750bc80...bd84ca`); Ryujinx with it:
  menus, Select a Master, play, power wheels, "LAST BALL!", 27-30 pictures a second when the machine was quiet.
- **The native renderer after this round**: verify mode 110.0 M checks, 0 differences (06:55). Lockstep against
  the 2026-10-13 baselines (`ls_fresh_off1/2`) fell to 46 of 123 with 0 at the same game frame — but a CPU-OFF run
  made now (`ls_off_0800`) scores exactly the same against them, so it is not the CPU work, and an A/B of the new
  input-probe calls (a temporary switch, removed) changed nothing either. The old baselines stopped reproducing on
  this PC between 04:56 (95 of 123, partly 2 frames off) and 06:58, both modes alike; the only event found in
  between is an NVIDIA driver reset at 04:45 (the game renders on the AMD iGPU, the same device in every log), so the
  cause is not known. What is new: the game is now frame-deterministic in the lockstep setup — two fresh CPU-off runs
  (`ls_off_0800`, `ls_off_0830`) agree on all 398 frames. Against them the CPU-on runs: **398 of 398 at the same game
  frame** (`ls_renut_opt_quiet`, 07:47) and 387 of 398 (`ls_on_0835`: the last 11 frames, 1388-1399, the loading
  spinner at the window's end). Use `-Base1 ls_off_0800 -Base2 ls_off_0830` from now on.
- **Renut's keys under the masters** (futari-nx's finding, its classification; 08:24): Renut's optimisations read
  their own keys, so `peggle2_cpu_enhancements = false` / `peggle2_noncpu_enhancements = false` left them on. 52 rows
  in `kEnhancements` now (`renut_*`: 16 CPU, among them Peggle's texture back-off and HD reader; 36 non-CPU, among them
  the texture cap and the late input), each with its code default as the on value, so nothing changes by default;
  `tools/make_enhancement_configs.py` regenerated the built-in defaults and the configs (only the new rows changed;
  the previous files in `out/renut_port/configs_before_masters/`). Not rows: Renut's fixes and settings, banjo-nx's
  N&B lists, the latency trim. PC with both masters off: every `renut_*` row off ("group master off" in the
  `[enhancements]` line), the frames right, render thread 6.0-6.7 ms a frame against 0.5-0.9 with them on — what the
  whole stack saves on the PC. Ryujinx with the defaults: 26-30 pictures a second, DKSH 9 hits. Delivered:
  `peggle2.nro` 52,223,562 bytes, SHA-256 `fb651f82...7110a0`, ELF `..._20261020_0824.elf`, and the regenerated
  `peggle2_renut_deko3d.toml`.
- **Renut and the web installer** (the user's question, 2026-10-20; the survey is
  `out/renut_port/web_installer_deko3d.md`): the dev NRO embeds game-derived shader data — `deko3d_glsl.bin` (the six
  translated shaders as GLSL text; for Peggle its zstd dictionary is that text) and `shader_cache_switch.cpp` (their
  metadata) — so it is a personal test build, not for distribution (its README says so); `deko3d_shaders.bin` is
  compiled from them, so it is game-derived too. Only three of the six shaders are stored in `default.xex`
  (0x820528D0, 0x82052AD8, 0x82052C38); the Bink movie trio (vs_3cae, ps_23fb, ps_b79e) is compiled at boot by
  D3DXCompileShader, so no installer can make those from the game's files: the port has to write them (as for the
  native library). Options: (0) write all six as port shaders (`tools/deko3d_host_shader.py`; the other three are
  SexyAppFramework sprite shaders): nothing game-derived, the DKSH cache can ship; (1) the installer makes the three
  stored ones (Renut's XenosRecomp front end and DXC with Renut's arguments in WebAssembly, SPIRV-Cross in
  WebAssembly, the GLSL passes in JavaScript or Pyodide; only 8 of the 27 passes change Peggle's shaders) and writes
  `deko3d_glsl.bin` beside the NRO, which then reads it from the SD; the console compiles the 9 variants at its first
  boot (~0.4 s each by banjo-nx's console measurement) and keeps them; (2) as (1) plus uam in WebAssembly for the DKSH
  (feasible, ~4 s saved once: not worth it). Either way a release build option must leave the embedded files out.
  Corrections from the survey: three of the six shaders are in the XEX (above said six); 0.6 s per variant was the
  emulator's figure, the console's is ~0.4 s.
- **Port-written Bink movie shaders for Renut** (2026-10-20, 08:40): `shaders/port/renut/peggle2_renut_movie_{vs,ps}.hlsl`
  (no game code; the native renderer's port movie shaders' math, in Renut's interface) and `tools/renut_port_shaders.py`,
  which puts the translator's `shader_common.h` and its prologue (rebuilt from ShaderRecompiler's pattern: checked equal
  to the translator's own text) in front and writes `vs_3caebedd6edf56e6.hlsl`, `ps_23fb686cb21303f8.hlsl`,
  `ps_b79e9877de15d6db.hlsl` into the translator's override folder after the early alpha pass (`gpu.cmake`, option
  `PEGGLE2_RENUT_PORT_SHADERS`, on). Checks: the GLSL after SPIRV-Cross, conversion and every pass is the translation's
  but for the names of temporaries, the order of commutative multiplies and adds (exact), `vec4(0.0).z` written `0.0`, and
  the alpha variant's texture bindings numbered in fetch order (its `texb=` table maps each to the same game texture);
  every pass fires on them as on the translations; the three untranslated shaders byte for byte the same. PC intro videos
  (frames 60-659): a port-shader run shares 336 images with a translation run, and two port-shader runs 328 with each other
  (the videos follow host time): no difference the shaders add. Ryujinx: the EA and PopCap videos play.
- **Release shader layout: no translated game shader in the NRO** (09:11): `tools/renut_release_shaders.py` writes
  `out/renut_switch_release/`: the built-in package with the three port shaders only, `shader_cache_switch.cpp` without
  the SPIR-V (the deko3d back end never reads it; numbers only remain: hashes, fetch masks, vertex input formats, no
  literals in Peggle's), and `sd/deko3d_glsl.bin` with the three stored shaders (a test copy: game data). cmake
  `PEGGLE2_RENUT_RELEASE_SHADERS=ON` builds against it (`app/cmake/peggle2_renut_gpu.cmake`; `build_switch.ps1 -Define`
  cannot pass paths with spaces). `banjo_renut_deko3d.cpp` now reads `deko3d_glsl.bin` beside the NRO first, then the
  built-in package, and takes a package without a dictionary. Checked: none of the stored shaders' distinctive GLSL
  lines are in the 09:11 NRO (all of them were in the 08:24 ELF); Ryujinx with `deko3d_glsl.bin` beside it: menus,
  Select a Master, play, the movies (port shaders); a clean DKSH cache of 9 variants compiled once and kept.
  Without `deko3d_glsl.bin` beside such an NRO (09:52 build): Renut's choice (`banjo_renut_select.cpp`) counts the
  shader table's shaders that have no GLSL in any package (`deko3d::MissingGameShaders`), logs "3 of the game's
  shaders are in no GLSL package ... The native renderer runs instead" and sets `masseffect_renderer_native = true`;
  Ryujinx: the native renderer ran with the release library (`peggle2_shaders.mesp`, 8 shaders), 918 draws in the
  window, none without its shaders (the Switch shots are Renut's, so they stay black there).
  Limitation: without the SPIR-V, `renut_gpu_backend = "vulkan"` (NVK) on the Switch has no game shaders in such a
  build (deko3d is the default and the back end tested); the installer would have to make SPIR-V too for that.
- **The web installer makes `deko3d_glsl.bin`** (the user's choice, option 2; peggle2-nx-installer):
  `wasm/renut_hlsl.*` (`tools/renut_hlsl_frontend.cpp` + Renut's ShaderRecompiler, `tools/renut_hlsl_pch.h`; its HLSL
  for the three stored containers is the native translator's byte for byte), the page's existing `wasm/dxc_web` (DXC
  v2025.1, the native renderer's fixed arguments: shader model 6.6, Vulkan 1.2; Renut's `-DBANJO_DEKO3D -DRENUT_RECOMP`
  as `#define` lines in front, and the y flip it forces on vertex shaders taken out of the GLSL, one fixed pattern),
  `wasm/spirv_cross.*` (SPIRV-Cross 22b22f56, the native tool's source; `wasm/src/deko3d/build_wasm_deko3d.sh`), the
  GLSL conversion and passes in JavaScript (`lib/deko3d_glsl.js`), and `lib/deko3d_package.js` (the package with zstd
  frames of raw blocks: the zstd tool decodes them; no dictionary). Result for the three: after conversion and the
  passes the GLSL is the native chain's byte for byte (so the console's DKSH keys are the same), and Ryujinx with the
  page's package compiled nothing and drew every screen. The JavaScript conversion and passes against the Python
  tools: byte for byte on Peggle's 6 and on banjo-nx's 4,881 shaders, also with banjo-nx's shader lists (so all 27
  passes changed something and were compared), and on 41 made-up edge cases (CRLF, BOM, Unicode, convert's errors: the
  same text or the same failure) (`test/deko3d_glsl_node.mjs`; banjo-nx's shaders read in place, nothing copied).
  Known differences, in no corpus: Unicode 17 vs Python's 16 word characters, the order of an error naming one of
  several misused sampler tables. About 20 ms a shader; Peggle's 6 in 0.04 s. The page's code from the Xbox 360 package (Node, `test/deko3d_node.mjs`, 1.0 s) and in
  Chromium (1.7 s): `deko3d_glsl.bin` 13,752 bytes, SHA-256 `79c467dd...4e06`, the one Ryujinx ran; the installer's
  `createPackage` (update zip, `test/package_node.mjs --deko3d <entry>`) writes it, zip CRCs good. The release
  manifest gets the entry with `test/release_node.mjs ... --deko3d <tested deko3d_glsl.bin>` when an NRO with Renut is
  published (the 1.0.0 NRO has no Renut: no entry now). Values for this build: containers
  `b546220e...b761`, `5222bd1e...550d`, `f7123e3e...9461`, package `79c467dd...4e06`. `lib/installer.js` `makeDeko3dPackage` (after the shader library; checks the
  three containers and the result against the build's manifest entry `"deko3d": { containers_sha256, package_sha256 }`;
  none = skipped), `deko3d_glsl.bin` in both zips, the worker loads the two modules only for such a build, the service
  worker caches them, `renut_shader_common.h` beside `shader_common.h`. Node: `test/deko3d_hlsl_node.mjs`,
  `test/deko3d_chain_node.mjs`, `test/deko3d_node.mjs` (prints the manifest entry).
- **The delivered files in Ryujinx** (the 09:11 NRO, its `deko3d_glsl.bin` and `deko3d_shaders.bin`, the delivered
  config, handheld, HD atlases from the pack): DKSH 9 hits, 0 compiles; 16 atlases from the pack; frames right. One run
  fell to 0.2 pictures a second after 3 minutes while a 14-process corpus test and another port's emulator ran; the
  rerun (one other emulator) held 14-28 with one stretch of 8-12 in which the renderer itself did nothing unusual (no
  atlas read, no compile, render thread 5-6 ms a frame). Docked soak of the same files (9.5 minutes, 1080p, HD):
  12-23 pictures a second while two other emulators ran, then 26-30; 28 atlases from the pack; host heap 1.87 GB
  (as the 07:43 build's soak); frames right (`out/renut_tests/emu/final_docked_soak/sheet.png`). The installer's full
  (not update) package was not run with the deko3d step (no PS4 package found on this PC); its zip entry is the same
  code in both modes.
- **60 pictures a second on deko3d** (Ryujinx, the delivered files, `peggle2_fps = 60`): 43-55 pictures and 50-60 logic
  updates a second through menus and a level (speed right). A first run stopped for 52 s after a level load: every
  thread of the process was silent then (the 10 s report threads too), so the emulator process itself stalled; the
  rerun had no stop.
- **banjo-nx after 07:35, not taken**: its `skinonce` pass and the uam 128-bit stores to storage buffer 13 (N&B's fur
  skin cache, an A/B; Peggle has no skinned shaders, and the installer's JavaScript passes would need it ported), and
  the run-time variant log's shader fingerprint (its own cache tooling).
- **banjo-nx's console display note (2026-10-20, after 11:23)**: on the console a second `viOpenDefaultDisplay` fails
  while libnx's default window holds the display (Ryujinx accepts it). Peggle opens it only in `display_sync.cpp`'s
  thread, which starts only with `banjo_display_sync = true` (at 11:23 off by default and in every Peggle config; on since 13:32 with banjo-nx's fix taken, see "banjo-nx's input-latency set"; on a failure
  it logs and the limiter keeps its timer); Renut's swap pacing (`PaceRenutSwap`) is a timer. So no change here; if the
  key is ever turned on, take banjo-nx's fix (its own `__nx_win_init` / `__nx_win_exit` / `nwindowGetDefault` and that
  display; its docs/deko3d-port-faq.md). The shared Ryujinx now signals the vsync event on a 60 Hz grid (from 11:23):
  nothing in Peggle's default setup waits on it.
- **Releasing an NRO with Renut through the web installer: the steps** (none done yet: publishing is the user's call)
  1. PC build (the translator, the port shaders, `out/pc/renut_gpu/shader_cache.cpp`); the deko3d SPIR-V with
     `BANJO_HLSL_OVERRIDE=out/pc/renut_gpu/early_alpha_hlsl BANJO_DEKO3D_SPIRV_DIR=out/deko3d_shaders/spirv` on
     `XenosRecomp.exe out/renut_shader_dump <cpp> shader_common.h`; `tools/deko3d_shaders.py`, `tools/deko3d_glsl_opt.py
     --check`, `tools/deko3d_shader_package.py`, `tools/renut_switch_shader_cache.py`, `tools/renut_release_shaders.py`.
  2. Switch release build with `-Define "PEGGLE2_RENUT_RELEASE_SHADERS=ON"` (and the release's LTO / PGO settings);
     `check_switch_build.py`; check that no stored shader's GLSL is in the NRO (the 09:11 check: distinctive lines).
     Delivered now: `out/dist/dev_20261020_renut_deko3d/peggle2.nro` of 14:36 without the Vulkan driver, with the
     latency set, sa2-nx's CPU work and futari-nx's deferred chain with its device guard, the local multiplayer fix, without late input's GPU-queue
     part (SHA-256 `776e8e9c...389ab716`, 37,887,562 bytes; 20:06), ELF
     `out/nx/elf/peggle2_renut_novulkan_mpfix_nolto_20261020_2011.elf`; the
     09:52 one with the driver: ELF `out/nx/elf/peggle2_renut_deko3d_split_nolto_20261020_0952.elf`.
  3. Test on the console with the page's `deko3d_glsl.bin` (`test/deko3d_node.mjs --out`) beside the NRO.
  4. Installer: the NRO as the release asset; `node test/release_node.mjs <x360> <ps4> <peggle2.nro>
     <peggle2_shaders.mesp> <recipe> --version <v> --deko3d <tested deko3d_glsl.bin> --write` (it makes the page's
     package again and refuses a different one); `test/package_node.mjs` end to end. An NRO without the Vulkan
     driver never reads `peggle2_shaders.mesp`; the page still writes it (harmless; leaving it out of such a release
     would be an installer change, not made).
- **Docked HD run of the 08:24 build** (Ryujinx, `peggle2_hd_pack` at the emulator SD's `hdpack/`): the pack's 362
  atlases found, 1536x3072 and 6144x6144 atlases read in 39-84 ms on the reader thread, render 1920x1080, 24-28
  pictures a second, the frames right (`out/renut_tests/emu/hd_docked_0830/sheet.png`).
- **Docked soak of the 07:43 build** (Ryujinx, the delivered config, the profile's `docked_mode` on for the run and
  put back; 9 minutes of the soak route): swapchain 3 x 1920x1080, render 1920x1080, 23-29 pictures a second after
  loading, render thread 1.1-3.3 ms a frame, DKSH 9 hits / 0 compiles, no `[flash]` line; the frames right
  (`out/renut_tests/emu/soak_docked_0850/sheet.png`). The HD pack was not found at the delivered config's path in this
  emulator SD (it sits under `hdpack/` there), so this soak drew the game's own atlases.
- **No Vulkan driver in the Switch build** (11:51, the user's call: "remove the vulkan driver from the switch builds
  just stop it linking"). `sdk/cmake/rexglue_switch.cmake` has `REXGLUE_SWITCH_LINK_NVK` (default ON: other titles
  unchanged); `app/CMakeLists.txt` sets it OFF for Peggle 2. OFF: no NVK SDK needed, `rex::nvk` is only zstd z nx
  stdc++ m, plume's `PLUME_SWITCH_LINK_NVK` (set from it in `app/cmake/peggle2_renut_gpu.cmake`) leaves Mesa's link
  group out, and `sdk/src/ui/switch_no_vulkan.c` joins `rexglue_switch_startup`. The SDK's and plume's Vulkan code
  reach the driver through function pointers, so only five symbols were left undefined: `vkGetInstanceProcAddr`,
  `vkDestroyInstance`, `vk_icdGetInstanceProcAddr`, `vk_icdNegotiateLoaderICDInterfaceVersion` (stubbed: no Vulkan,
  `VK_ERROR_INCOMPATIBLE_DRIVER`) and `posix_memalign` (FFmpeg's allocator; newlib has none and Mesa used to supply
  it: on `memalign` now). plume's Switch `VulkanInterface` would then call `vkEnumerateInstanceExtensionProperties`
  through a null pointer if anything made one: banjo-nx's guard (it did the same build today and found this) returns
  with no instance when volk found no `vkCreateInstance` / `vkEnumerateInstanceExtensionProperties`, so
  `CreateVulkanInterface` gives nullptr (`plume_vulkan.cpp`; NRO of 12:05). The native renderer is Vulkan, so
  `banjo_renut_gpu` gets `PEGGLE2_NO_VULKAN`: Renut's
  renderer is chosen whatever `peggle2_renderer` / `masseffect_renderer_native` say (logged once), the back end is
  deko3d whatever `renut_gpu_backend` says (`backend.cpp` `ResolveBackend`), and a missing `deko3d_glsl.bin` is an
  error line instead of the switch to the native renderer. `tools/check_switch_build.py` reads the option from the
  build's CMakeCache and then checks that no Mesa is in the NRO ("Vulkan driver: none"). NRO 52,211,274 ->
  37,781,066 bytes; the link no longer waits on Mesa. Ryujinx (handheld, an old config: `peggle2_renderer = "native"`,
  `masseffect_renderer_native = true`): Renut on deko3d, DKSH 9 hits, 18-27 pictures a second through menus and play,
  frames right (`out/renut_tests/emu/novk_1155`); the 12:05 NRO with `renut_gpu_backend = "vulkan"` too: deko3d, menus,
  Select a Master and play drawn, 22-29 pictures a second (`novk_guard_1207/sheet.png`, shot hashes beside it); docked
  with the delivered config and the HD pack (`novk_docked_1212`): swapchain 3 x 1920x1080, 3 HD atlases from the pack,
  DKSH 9 hits / 0 compiles, 22 pictures a second in play, the frames right (its first 30 s were a slow boot with
  another port's emulator running). Without `deko3d_glsl.bin` (`novk_noglsl_1200`): the boot error line,
  16 capped "not in the GLSL package" lines, 0 draws, the game running at 26 pictures a second on a black screen; no
  crash, the DKSH cache and pipeline list untouched. Still linked and dead (the code's own size in the ELF): the
  recompiled game 33 MB, Renut + plume 2.05 MB, the SDK's Xenos graphics 1.99 MB, the native renderer 1.51 MB, other
  Vulkan code 0.33 MB; dropping the native renderer too is `PEGGLE2_NATIVE_RENDERER=OFF` and a full rebuild (not done).
- **banjo-nx's input-latency set, on by default** (13:32; asked through banjo-nx's session for the user):
  `backend/render_thread.cpp`, `render_thread.h`, `backend/video.cpp` and `display_sync.cpp` are banjo-nx's of
  2026-10-20 (Peggle's copies plus only its additions: patches and the old files in `out/to_delete/latency_before`);
  `video.h` `kKickLists` 6; `banjo_renut_tuning` gets `banjo_late_input_gpu`, `banjo_late_input_gpu_margin_us` (6000)
  and `banjo_deko3d_eager_kicks` with the in-run A/B targets 18 / 19 (17, banjo-nx's skin once, is not here).
  Enhancement rows (non-CPU, on): `renut_late_input_gpu`, `renut_eager_kicks`, `renut_display_sync`
  (`banjo_display_sync`; phase auto and its 12 ms cap were already the defaults). `banjo_deko3d_latency_trim` stays off
  (it starved the GPU on banjo-nx's console with eager kicks). Display sync: banjo-nx calls it from its own frame
  limiter, which Peggle 2 does not have, so `PaceRenutSwap` (`peggle2_frame_hooks.cpp`) now asks
  `display_sync::NextRelease` first and keeps its timer when the refresh is not known; `display_sync.cpp` also brings
  banjo-nx's own `__nx_win_init` / `__nx_win_exit` / `nwindowGetDefault` (the console refuses a second display open
  while libnx's default window holds it). Ryujinx (`latency_1334`, the delivered config, the shared build with the
  60 Hz vsync grid): "[display] the frame limiter follows the display's refresh", refresh 16.68 ms; steady play 30.0
  pictures a second, 300 of 600 refreshes with a new image, 0 never shown, input -> display 16.8 ms; frames right.
  Late input added nothing there (the emulator's game thread is the limit: no slack). Peggle draws 20-35 times a
  frame, so eager kicks rarely fill a list.
- **sa2-nx's Renut CPU work, on by default** (14:06; the user: "wire up all of SA2's new CPU improvements"; its
  write-up `D:\sonic adventure 2\sa2-nx\docs\renut-cpu-work-for-banjo.md`). How: a 3-way merge per file (`git
  merge-file`) with banjo-nx's `c319b29` as the base (read with `git show`, nothing written there), Peggle's file as
  ours and sa2-nx's as theirs (`out/sa2_merge`: base / ours / theirs / merged and the scripts; the files before in
  `out/to_delete/sa2_cpu_before`). Taken whole: `backend/queued.cpp/.h`, `backend/render_thread.cpp/.h`,
  `render/draw.cpp/.h`, `render/pipeline.cpp/.h`, `render/surfaces.cpp/.h`, `render/vertex_format.cpp/.h`,
  `guest/callbacks.cpp/.h`, `guest/command_buffers.cpp` and the new `gpu/sa2_line_compare.h` (one conflict: the
  memexport store cards, Peggle's guarded line kept). Left out as sa2-nx's own: its controller read hook (Peggle's
  SDK calls the late input itself), its guest vertex declarations, a depth-size diagnostic, its gamma ramp, double
  buffer, pacing and game hooks. By hand: `d3d/hooks/device.cpp` (`SnapshotDeltasNewFrame` at the Swap; the core
  layout, key `sa2_renut_core_layout`), the 10 s "[renut] device snapshots" line from `banjo_renut_tuning.cpp`, and
  the SDK's `RexSetGuestThreadCoreLayout` (`system/xthread.cpp`: every XThread registers itself with the pthread
  layer; threads made later take the layout) with `RexSwitchSetCurrentThreadMask` / `RexSwitchSetThreadMaskOf`
  (`core/threading_switch.cpp/.h`). 19 rows (CPU group) keep sa2-nx's key names: `sa2_renut_snapshot_deltas`,
  `_constants`, `_narrow`, `neon_compare`, `capture_batches`, `callbacks_fast`, `wake_batch` (16 / 1),
  `prefetch_next`, `done_notify_waiters`, `batch_clock`, `chain_reuse`, `inline_line_compare`, `stream_mask`,
  `texture_defaults_copy`, `fetch_slots_skip`, `fetch_data_unread`, `shared_partial_clear`, `lean_diagnostics`,
  `core_layout`; the two `_verify` keys stay off (test configs only). Peggle's device layout is sa2-nx's (the same
  private offsets, snapshot 13,088 bytes), so its chain layout and narrow ranges apply unchanged.
  What it does for Peggle: its draws are one plain draw a frame plus `Begin*Vertices` sprite draws, which sa2-nx keeps
  as whole copies outside the delta chain (they are queued later than made), so the deltas and the chain's reuse did
  not fire here until futari-nx's deferred chain (next entry) ("[renut] device snapshots": every draw a whole copy; PC verify run: 0 of ~300 draws per 10 s
  differ). The rest applies to every draw or hook: the hooks' lock-free first calls, the render thread woken per 16
  draws, the done signal only with a waiter, the prefetch, one clock read a batch, the shared constants' NEON and
  skipped compares, the stream mask, the lean diagnostics, the core layout. Checks: PC build and runs; Renut on the
  PC is not frame-deterministic (two runs with the keys off share 47 of 396 frames), and the keys-on run has all 47
  of them (36 at the same frame); Switch `check_switch_build.py` all passed; Ryujinx (`sa2cpu_1407`, the delivered
  config, HD pack): "sa2_renut_core_layout: ... 17 threads set", menus, play and the power wheel right, 26-28
  pictures a second in play, render thread 1.1-2.6 ms a frame, no audio warnings. sa2-nx's store-card items (17, 18)
  do not apply: Peggle has no store cards.
- **Release 1.0.1** (2026-10-20/21; the user: "give me a build to instrument pgo with please turn off all logging
  also please enable 60fps by default now for all builds including the final one as the new renderer improves
  performance enough. make sure the generated .toml file only has the same options as 1.0.0 for the end user", then
  "can you just use the pgo stream you got from ryujinx to build the final should be good enough", "please bump it to
  1.0.1 you can just hex edit it over" and "upload to github please update the readme and update the webinstaller").
  - **60 by default**: `peggle2_fps` defaults to 60 in the code (`peggle2_frame_hooks.cpp`), in every generated
    config (`make_enhancement_configs.py`: the header's default and the play config) and in the release files;
    `tools/win/verify_lockstep.ps1` sets `REX_PEGGLE2_FPS=30` (its baselines were recorded at 30).
  - **Built-in settings** (`app/peggle2_switch_defaults.toml`, compiled into the NRO): Renut's renderer on deko3d
    (`peggle2_renderer = "renut"`, `renut_gpu_backend = "deko3d"`, `masseffect_renderer_native = false`) and
    `peggle2_fps = 60`; logging off as in 1.0.0 (`log_level = "off"`, `log_to_file = false`, no profiler report).
    `deko3d.log` (`banjo_renut_deko3d.cpp`) is now written only with `log_to_file` (a Ryujinx run of the instrumented
    build had written it with logging off). The player's file the game writes (`app/peggle2_switch.toml`) has 1.0.0's
    four keys exactly (`git show HEAD:app/peggle2_switch.toml`: only the fps value and its comment differ).
  - **PGO**: instrumented build `build_switch.ps1 -BuildDir out/nx -Pgo generate -Define
    "PEGGLE2_RENUT_RELEASE_SHADERS=ON;PEGGLE2_SWITCH_LTO=ON;REXGLUE_SWITCH_LINK_NVK=OFF"` (LTO, 52,366,922 bytes;
    ELF `out/nx/elf/peggle2_pgo_generate_lto_20261020_2343.elf`; checks passed). Profile: its Ryujinx run with the
    player's config and the HD pack (`out/renut_tests/emu/pgo_gen_2345`: boot, title, menus, a level, the power wheel;
    the 2-minute dump, `out/pgo_streams/ryujinx_20261020/peggle2_pgo_1.stream`, 2,938,109 bytes), the user's choice
    instead of a console session. Merged by `tools/win/pgo_merge.ps1` into 165 `.gcda` (123 recompiled game, 42 app;
    Renut's objects get no PGO flags); 1.0.0's 164 kept in `out/pgo_gcda_1.0.0/`. Final: `-Pgo use` (LTO, 170 compile
    rules with the profile; `banjo_renut_deko3d.cpp` changed after the session: `-Wno-coverage-mismatch` drops its
    profile), 41,483,850 bytes, `check_switch_build.py` all passed; ELF
    `out/nx/elf/peggle2_1.0.1_lto_pgouse_20261021_0003.elf`.
  - **Version 1.0.1**: the NACP's display version patched in the NRO (one byte at 0x278EEAA, "1.0.0" -> "1.0.1"; the
    three other "1.0.0" strings are toml++'s messages) and `PEGGLE2_VERSION` set to 1.0.1 in `app/CMakeLists.txt` for
    later builds. NRO SHA-256 `577508993bd8d5753e23613ca0ceedfcf75c9b65eecdcb54192ebe37b50b79ed`
    (`out/dist/final_lto_pgo_20261020/`). Ryujinx with the player's config (`final_101_0005`): title, menus, play and
    the power wheels right; no file written but the shots (no log, no `deko3d.log`).
  - **Installer**: `test/release_node.mjs --version 1.0.1 --deko3d` from the two original games (the Xbox 360 package
    in `D:\peggle 2\58411436\000D0000`, the PS4 base package read in place) with 1.0.0's library, index and recipe:
    the 1.0.1 entry with the `deko3d` block (the page remade `deko3d_glsl.bin` byte for byte, `79c467dd...`); the
    deployment workflow now publishes `renut_shader_common.h`; README and NOTICES (Renut's translator, SPIRV-Cross)
    updated.
  - **Repository**: README (what's new, the renderer, 60 by default, the update note for 1.0.0's `peggle2_fps = 30`),
    THIRD_PARTY_NOTICES (reNut, plume, uam, Renut's XenosRecomp, Dear ImGui, SPIRV-Cross, deko3d), the vendored
    `app/thirdparty` committed as banjo-nx's repository has it (without Microsoft's prebuilt DXC binaries and
    D3D12MA's sample executable).
- **Local multiplayer: player 2's controller did nothing in a duel** (20:06; the user's console report: "both
  controllers worked when selecting a character but as soon as the game begins in duel (taking turns) player 1 works
  fine and player 2 the controller does nothing"). Reproduced on the PC (`tools/win/mod_test.ps1` on
  `game_root_mod_mp`, player 2 as `REX_PEGGLE2_INPUT_SCRIPT_P2` in the environment: `-Keys` splits on commas, so a
  script there keeps one event). The 2026-10-14 duel never tested player 2: its second script pressed only the join,
  and the 15 s shot clock fired player 2's shots. New diagnostics, off by default: `input_diag_users` (SDK
  `xam_input.cpp`: XamInputGetState's calls per user every 10 s, with how many had input; XamUserGetDeviceContext's
  calls) and `peggle2_diag_controllers` (`peggle2_debug_hooks.cpp`, a hook on the input manager's update
  `sub_8245F778`: each of its four controller objects' user index, the profile it is bound to at +448, flags).
  Findings: the game polls all four users about 60 times a second, and player 2's presses reached user 1 during the
  duel, yet player 2's turns waited out the shot clock; with only player 1 pressing, player 2's turns fired at once:
  player 2 played on player 1's controller. Peggle 2 ties a controller to a game profile (the poll looks the profile
  up by the user's name, XamUserGetName, through the manager at +1868, and stores it at +448), and a duel player takes
  its turns on its profile's controller; the lobby reads the controllers directly, which is why it worked. Controller
  1 never got a profile (+448 stayed 0): the sign-in handler (`sub_8237B948`) reads every signed-in user's profile
  settings, and the SDK's `XamUserReadProfileSettingsEx` answered only user 0 (`X_ERROR_NO_SUCH_USER` for the guest
  of the 2026-10-14 guest sign-in), so the game made no profile for player 2 and fell back to player 1's. Fix
  (`sdk/src/kernel/xam/xam_user.cpp`): a guest reads the profile's settings, as a guest on the console gets settings.
  After it: controller 1 bound to a profile of its own (`[controllers]`), the banner names player 2 "USER(1)" (before:
  the profile's name for both), and each player's turns follow only their own controller (`out/ps4_dlc/shots/
  mp_fix_p2_sheet.jpg`: player 2 pressing, its shot 1 s into its turn; `mp_fix_p1_sheet.jpg`: player 1 pressing,
  player 2's turn waits out the shot clock, player 1's fires at once; before: `mp_p2test2/3`). Ruled out on the way:
  XamUserGetDeviceContext (never called in a duel), and controllers 2-4 as separate profiles instead of guests (an
  experiment: then no controller drove player 2 at all, the same missing profile). Not checked in Ryujinx: the
  scripted pads land behind the emulator's own controllers (users 2-3) there. Console run pending. Relevant to any
  title that takes more than one local player with the guest sign-in.
- **`banjo_late_input_gpu` removed completely** (20:06; the user's follow-up through banjo-nx's session, banjo-nx's
  package 36s did the same): the key and its margin key, `LateInputGpuOn` / `LateInputGpuMarginUs`, the GPU-slack and
  applied-delay rings and `NoteGpuQueueWait` (`backend/render_thread.cpp/.h`), the first-work stamps, queue-wait
  notes and `ReportQueueWaitLocked` at the kicks and presents (`backend/video.cpp`), the A/B target 19 (now reserved;
  range 0-18) and the report's GPU-queue part. Plain late input (the render-thread slack) stays; the "input -> GPU
  done" note at every kick stays (the probe's measurement, not this feature).
- **`banjo_late_input_gpu` off again** (19:06; the user's request through banjo-nx's session): its row
  `renut_late_input_gpu` is gone, so the key keeps its code default (off; it stays for tests, with its margin key).
  banjo-nx's console (package 36q): in a GPU-bound scene (33.4 ms of GPU work a frame) 24.3 pictures a second with it
  against 29.6-29.9 without; the controller read was held back 29 ms a frame instead of 8-11 (the GPU queue's part
  feeds late input's own slack and the two ratchet up), so the game thread started frames too late and the GPU sat
  idle about 20 % of the time; nothing measurable at the 30 fps cap. Plain late input, display sync and eager kicks
  stay on. Ryujinx (`nogpuq_1907`): no "GPU queue" part in the [latency] line, frames right.
- **futari-nx's deferred chain: Peggle's sprite draws join the delta chain** (14:36; asked through futari-nx's
  session for the user; the user then had sa2-nx take it too, and it did). `backend/queued.cpp`, key
  `futari_renut_deferred_chain` (row `renut_deferred_chain`, CPU, on): `QueueDeferredDraw` makes a Begin*Vertices
  draw that will be queued (scratch) a `CaptureDelta` under `g_delta.mutex` (a stale slot of the device, none
  expected, is dropped and the chain rebases), and `FlushDeferredVertices` queues it under the chain's lock taken
  before the deferred map's. Safe because every hook, the Swap too, flushes its device's deferred draw before it
  captures anything else, so queue order stays capture order. PC verify run (both sa2-nx verify keys): "0
  Begin*Vertices draws as whole copies", 11,050 draws a 10 s window at 540 bytes a draw (13,088 before), 0 of about
  11,000 a window differ. Ryujinx (`defchain_1437`, `defchain_true_1442`): every draw a delta, 546-688 bytes a
  draw in play, frames right. Ryujinx timings of an A/B with the key off (`defchain_false_1440`) were all host load
  (7.9-20.7 pictures a second off, 10.5-26.6 on): no ranking from them.
  Caveat (16:56, futari-nx's message, found by banjo-nx, whose copy hung in 4 of 8 emulator runs before it turned it
  off): the delta chain is one for every Direct3D device, and a deferred draw is made at Begin but queued at the
  device's next hook, so a draw on another device in between moves the chain's base under it. Peggle 2 has one device
  ("Guest D3D device (type 1) created" once a run, PC and Switch), so it was never exposed; futari-nx's guard is taken
  anyway (`queued.cpp`: `DeltaChain::made` / `multi_device`, `DrawCapture::chain_mark`; at a second device's draw
  Begin*Vertices go back to whole copies, and a pending delta whose base moved is taken whole at the flush, counted in
  the "[renut] device snapshots" line). PC verify after it: 0 of about 11,000 a window differ, no fallback; Ryujinx
  (`defguard_1658`): one device, every draw a delta, frames right. Not passed to sa2-nx: its runs have one device
  too (futari-nx asked for it only if sa2-nx had more).
- **Not done**: a console run (the user's); store cards (the back-off instead); the automatic front buffer (Peggle
  swaps its own textures; only a scaler helper reads the automatic one, with a fallback); an offline DKSH build (the
  NRO's own cache from the emulator ships instead: the same uam, in the NRO).

Credit: reNut by masterspike52; banjo-nx (its Renut port, deko3d back end, shader generator, Ryujinx fork and
FAQ); the Futari session's notes (early alpha names, vblank pacing).
