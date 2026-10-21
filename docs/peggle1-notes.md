# Peggle 1 port: notes

The record of the port: what each step found, with its evidence. Peggle 2's notes (`peggle2-notes.md`) stay as the
reference for everything this port took from peggle2-nx.

## The game (2026-10-21)

The user's files are in `D:\peggle 2\peggle 1`. They were extracted with `tools/stfs_extract.py` (every block
hash-checked) into `assets/`, which is git-ignored.

| Package | Content type | What | Extracted to |
|---|---|---|---|
| `Peggle/58410889/000D0000/87D0A5D3...` | `0x000D0000` (XBLA game) | 36 files, 101.6 MiB | `assets/game_root` |
| `Peggle Nights/58410889/00000002/6AE57722...` | `0x00000002` (marketplace content) | `content.pak`, `spa.bin`, `this_is_nights` | `assets/dlc_nights` |
| `111272B2/TU_1C42249_...` | `0x000B0000` (title update) | Title Update #3: `default.xexp`, `nightstu.pak` | `assets/tu3` |

- **`default.xex`** (`tools/xex_info.py`):
  - XEX2, original PE name `Thunderball.exe`, base `0x82000000`, entry `0x823D81C0`, image size `0x011D0000`
  - version 0.0.1.2, title 58410889, media `111272B2`
  - imports only `xam.xex` and `xboxkrnl.exe` (2.0.7776)
  - static libraries: XDK **2.0.7776** (D3D9, XAPILIB, XCAM, XONLINE, XMEDIA, LIBCPMT, XBOXKRNL, XGRAPHC, XACT, XAUD, X3DAUD, XNET, QNET, XRNSS, XRNMS, XHV)
- **Title Update #3** (`default.xexp`): a delta patch from version 0.0.1.2 to **0.0.4.2**, image size `0x01390000`. The SDK applies a `default.xexp` lying next to `default.xex` by itself, both in the code generator and in the game (`UserModule::LoadFromFile`: "XEX patch applied successfully"). So `assets/game_root` holds both files, and every address in this port is the patched game's.
- **Data:**
  - PopCap `.pak` archives: `peggle.pak`, `bootstrap.pak`, `aux_sounds.pak`, and the language packs `en`, `de`, `es`, `fr`, `it`, `jp`, `kr`, `pt`, `tc`
  - the PopCap logo video `PopCap_Logo_MedQual_720p.wmv`
  - the achievement images and `ArcadeInfo.xml`
- **Where the game looks for the extra content** (`tools/image_strings.py`):
  - `update:\nightstu.pak`, then `d:\nightstu.pak`: the title update's pack. It is copied into `assets/game_root`, so the `d:` path finds it.
  - `game:\PopCap_Logo_MedQual_720p.wmv`
  - the Nights DLC (`:\content.pak`, `:\this_is_nights`), read from its content package. It is installed from `dlc/` next to the executable (`src/peggle1_dlc.cpp`, sa2-nx's installer).

## Code generation (2026-10-21)

- **Manifest:** `app/peggle1_manifest.toml`; the loop is `tools/win/codegen_p1.ps1`.
- **Jump stubs.** Peggle's executable holds 13 identical tables of 13,011 linker jump stubs (`lis r11,hi; addi r11,r11,lo; mtctr r11; bctr`, 16 bytes each). One table is in `.xidata` (`0x82140010`); the others are in `.text` and in the nine `.embsec_` code sections. Calls into the stubs:
  - by `bl`: the generator makes functions of these
  - by `b` (tail calls): these were "target not in any function" (189 sites, 149 targets: 146 stubs and 3 small functions). `tools/jump_stubs.py` finds the stubs; `app/overrides.toml` lists every stub that any `b`/`bc` reaches (569).

  `tools/callers.py` lists a function's callers through the stubs as well.
- **Catch funclets ending in a rethrow** (`bl _CxxThrowException` with r3 = r4 = 0, which does not return), followed by two data words. The generator ran on into the next funclet: the build failed with "use of undeclared label". `end` entries in the overrides fix the three cases (`0x8227FC78`, `0x8262C97C`, `0x82274454`). `tools/label_check.py` finds every function of this kind in the generated code in one pass, without a build.
- **Jump tables** (`tools/switch_check.py --code 82140000:82F778A4`: its default range is Peggle 2's `.text`, which would leave out Peggle 1's `.embsec_` sections):
  - `sub_82448190`: the table was cut short (17 cases generated, 64 bounded in the code).
  - `sub_82848E00`: a byte table with no bound in the code. The generator read 99 entries, scrambled the function's body and pulled the next function's code into it. The table has 16 entries: the next table (`0x820DD2E8`) belongs to `sub_8284AAF8`. Entry 4 lands on the next function's start, `0x82848F50`, which becomes a call and return.

  Both are fixed with `[[switch_tables]]` entries. Six other tables have no bound in the code either; their generated cases stay inside their functions (checked by hand), so they were left alone.

  Correction (same day): `sub_82448190`'s "64" is a 6-bit mask (`rlwinm r10,r9,0,26,31`), not the table's size. The table at `0x82070738` is 12 bytes: 9 cases and 3 zero bytes, then floats. The 64-case entry sent 9 cases into the next function ("Jump target ... unresolved"). The entry now lists the 12 real cases. A masked index bounds the values, not the table: check where the table's bytes end.
- **The pointer sweep** (`tools/pointer_fixpoint.py`) took two jump tables for functions. In each case the `lis/addi` that builds a table's address had read as a callback being built:
  - `0x82488564`, `sub_8248852C`'s absolute table right after its `bctr`. Its case addresses decode as `lwz`. Accepting it split the function: "Unresolved conditional branch" warnings.
  - `0x827D9CFC`

  Both are commented out in the overrides. The sweep's other entries after a `bctr` are stubs following another stub (fine).
- **`tools/codegen.ps1`** (Peggle 2's, which the sweep runs after each pass) ran the post-codegen passes with Peggle 2's lists. The passes edited the tree before one of them failed. They are now opt-in (`-PostPasses`), and the tree was regenerated from scratch.
- These findings were relayed to the Mushihimesama session on 2026-10-21, at the user's request, for its new port.

## First boots on the PC (2026-10-21, emulated GPU)

- **Link:** the game imports the Xbox Live Vision camera functions (`XUsbcam*`). The SDK has Xenia's "not connected" versions (`sdk/src/kernel/xboxkrnl/xboxkrnl_usbcam.cpp`), but its build had left the file out. It is in now.
- **Boot 1:** the title update applies, Peggle Nights installs into the content store, then the game stops: "Call to invalid or unregistered function at guest address 0x8214E720". That address is a jump stub reached through data (a vtable or callback).
  - The fix is `src/peggle1_stubs.cpp`. After the image loads, every stub without a generated function gets its target's function in the function table: 169,143 stubs; 5,438 have their own function; 163,559 call their target directly.
  - 146 stubs (12 distinct targets) jumped to code with no function. These are small functions right after another function's last branch, now listed in the overrides.
- **Boot 2:** "Call to invalid or unregistered function at 0x828144C0", a this-adjustor thunk (`addi r3,r3,-16; b ...`, one of a run at `0x828144A0..`) reached through a vtable. This is the class `tools/pointer_fixpoint.py` sweeps.
  - The sweep accepted 87 functions and reached its fixpoint on the second run.
  - Two of its acceptances were jump tables (above). The fix was to declare their owners, `0x8248852C` and `0x827D9CD0`; with those declared, the tables are no longer gaps.
- **Boot 3:** the PopCap logo video plays (the XMV/WMV decoder runs as recompiled code). Then "Call to invalid or unregistered function at 0x8284B058", a vtable entry that tail-calls an import thunk (`0x82182D30-0x821841E0`, 332 thunks). The sweep had rejected it as a "tail branch into a function body". It was the only rejection of that kind whose branch goes to a thunk; the other 67 end in the `__restgprlr` epilogues, so they are pieces of other functions.
- **Boot 4** (scripted: START, then A every 3 s; `out/renut_tests/menus1/sheet.jpg`), no crash in 130 s:
  - PopCap logo, loading screen, title ("PRESS START")
  - main menu (Single Player, Multi Player, Leaderboards, Achievements, Help & Options, Download Content)
  - **Peggle Nights**' menu (Adventure, Challenge, Trophies, Stats): the DLC from `dlc/` and the title update work
  - "Welcome to the Institute", Nights Stage One (Bjorn), the instructions, and a level played: shots, "Total miss!", the free-ball bucket, the score

  About 60 frames a second on the title screen.
- **One giant function**, `0x82C71E80` in the second `.embsec_` section, generates 1.36 MB of C++. It gets a file of its own.

### Addresses found so far

| What | Peggle 1 | How |
|---|---|---|
| `D3DDevice_Swap` | `sub_824430C8` | the only caller of `VdSwap` |
| `_CxxThrowException` | `sub_8279AAB0` | 688 call sites; copies the 32-byte record template at `0x820D4DD0`, calls RaiseException |
| `RaiseException` | `sub_827CB950` | calls `RtlRaiseException` |
| ring buffer set-up | `sub_82440810` | calls `VdInitializeRingBuffer` |

## The app (2026-10-21)

The app is peggle2-nx's, renamed by `tools/port_rename.py`: identifiers and file names only. Comments keep "Peggle 2"
and "peggle2-nx" where they record that port's findings.

Left out of the build: Peggle 2's PS4 content (`src/dlc`), the PM4-ring Vulkan renderer (`src/me`,
`PEGGLE1_NATIVE_RENDERER` OFF), the hot-function natives (`src/cpu/peggle1_hot_guest.cpp`), the texture
diagnostics, and the post-codegen passes (`PEGGLE1_PERF_POST` OFF). Those passes carried Peggle 2's function lists;
they come back once Peggle 1 runs. The frame hooks' 60 fps switch is idle until the presentation-interval word of
XDK 7776's device is measured (`kDeviceIntervalOffset`).

The Switch renderer is Renut's on deko3d only (the user, 2026-10-21: "we can skip making a vulkan renderer on switch I
just want deko3d").

## Renut's renderer at Peggle 1's addresses (2026-10-21)

A helper agent matched Renut's hooks against Peggle 1. Its data, method and evidence are in `out/renut_port/`:
`hook_map.md` / `.json`, `device_offsets.md`, `device_offset_sites_p1.json`, `other_refs.json`. It matched from N&B,
since every hook keeps its N&B address in a `// N&B sub_X` note.

- **Hooks:** 361 in all; 348 with high confidence, 4 medium, 9 absent.
  - The 9 absent are `BeginRingManual`, `SetTexture_Inline` (14 identical copies, none called) and N&B's 7 game hooks (photo capture, widescreen).
  - 278 of the 352 mapped functions are N&B's instruction for instruction.
  - 33 hooks Peggle 2 had to leave out exist in Peggle 1 and are back (InsertCallback, command buffers, tiling, Present, DrawVertices, the lock and desc functions, ...).
  - Only `D3DDevice_Swap` (`sub_824430C8`) is shared with the app's own hooks (the frame hooks); everything else is a plain hook.
  - Applied with `tools/renut_hook_remap.py`, now Peggle 1's: it rewrites from the N&B note and puts back peggle2-nx's `#if 0` hooks that Peggle 1 links.
- **The device:** XDK 7776's `CDevice` is laid out exactly as N&B's 7645. 296 of 297 harvested offsets have delta 0; the global qword the device copies holds the XDK build, 7776 here and 7645 in N&B. Direct3D_CreateDevice (`sub_824444F8`) allocates 24,192 bytes and clears 24,064, as N&B does.
  - `tools/renut_device_offsets_p1.py` puts every Peggle 2 offset back: 152 lines and 5 hand-changed sites.
  - The ends of three ranges were set by hand: the snapshot's private unread words (10,880-11,775) and the `ChainChanged` ranges of the targets (12,440-12,459) and textures (12,544-12,647).
  - `d3d/d3d.h` is now identical to banjo-nx's.
- **Peggle 2-only behaviour removed** (back to banjo-nx's):
  - the 32-byte block at +11028 (Peggle 1's is N&B's +10900, from SetRingBufferParameters)
  - the `+16712` clear is restored
  - SetTexture(NULL)'s fetch-type clear
  - BeginExport's NaN-vertex condition

  `d3d_native_state.cpp` is banjo-nx's file again, with Peggle 1's addresses.
- **Guest data:**
  - state tables `0x82F85848` / `0x82F85D08`
  - format table `0x8206FBB8`
  - caps `0x82070C28`
  - HiZ owner `0x83024404`
  - export switches `0x83024420` / `0x83024424`
  - global qword `0x82F85E58`
  - full viewport `0x8206FEC8`
  - no-large-pages flag `0x83029CD8`
  - Fusion flag `0x83024414`
  - VdGlobalDevice slot `0x830B0CDC`
  - XMemFree `sub_82463228`
  - GetCurrentThreadId `sub_824639B0`
- **Signature differences** that matter if a hook gets called:
  - BlockOnFence's flag is in r7
  - LockResource has a new 64-bit r4, which LockSurface passes through
  - InvalidateConstants and VertexShader_Bind each take one more argument (no caller in Peggle 1)
  - Several helpers N&B inlined are real, hooked calls here (AddRef in GetDevice, XGRegisterPixelShader in CreatePixelShader, SetVertexDeclaration in UnsetAll, ...)
- **Shaders:** the image holds 82 shader containers, 68 vertex and 15 pixel, one of them a duplicate (`tools/renut_shader_dump.py` -> `out/renut_shader_dump`).

### On the PC (Vulkan), 2026-10-21

- **Build:**
  - `PEGGLE1_RENUT_RENDERER=ON`; the Bink port shaders are off (`PEGGLE1_RENUT_PORT_SHADERS`: Peggle 1 has no Bink).
  - The render-mode, HD-pack and PC profiler-stub files of `src/me` are built with Renut.
  - The snapshot-delta chain's read chunks are 39 (N&B's layout, as banjo-nx's), not SA2's and Peggle 2's 40.
- **The automatic front buffer:** Peggle 1 creates its device with one; N&B does not, so Renut had "an automatic front buffer is not supported". Its Present resolves the back buffer into the front buffer before the Swap, and the PopCap logo video is shown that way. The video was blank white until `InitializePresentationParameters` made the texture as Peggle 1's D3D does: `CreateTexture(w, h, 1, 1, 0, front format, 0, 3)` at `+14828` (`native::CreateTexture`).
- **The game's presentation interval is 1:** Peggle 1 presents every vblank (logged at Swap from `+13596`), so no 60 fps patch is needed.
- **Against the emulated GPU** (`tools/win/p1_lockstep.ps1`: the same frame-timed input, 1,200 frames each from game frame 1900; `tools/compare_raw.py --window 250`):
  - The two runs drift apart in game frames, because loading takes wall time. So each frame is compared with its best match.
  - The best matches differ by 2.5 levels on average, at most 8, with no pixel over the tolerance. That is the display ramp: Renut applies it as a power, exponent 1.172, worst difference 6 of 255.
  - The rest is animation timing (a box sliding in, a ball elsewhere).
  - Screens seen through Renut: the logo video, loading, the main menu, the Nights menu, Bjorn's intro, the instructions, a level played (shots, free ball, "Total miss!", the bucket).

### The optimised codegen (2026-10-21)

Only the optimised-codegen NRO is delivered (the user's rule for every port). Peggle 1's:

- **`app/perf_overrides.toml`, stage 1:** Peggle 2's set without `non_argument_as_local` and `elide_lr_stores`. Those two need keep lists made for this game: the audit (`tools/non_argument_audit.py`) finds 8,168 catch funclets that take r12 on entry, the FPR/VMX restore helpers, and four vector save/restore helpers the SDK does not know (`sub_82954048`, `sub_82954380`, `sub_82D34570`, `sub_82D348B8`).
  - `tools/read_before_write.py` on code generated with the options finds 23 functions.
    - One is a split piece: `sub_8246E474`, the `__try { RaiseException }` tail, as Peggle 2's `sub_8231CEE4`. It is marked `share_registers`.
    - Eight vector functions read v64, v65, v69 and v72 that their callers set: Microsoft's WMV decoder. banjo-nx found the same (permute masks in v64/v65).
    - The rest is harmless (CR saves, `xor r30,r30,r30`, the setjmp sites' jump buffer, a data word).
  - **Found by the PC run:** the PopCap logo video was garbage blocks. Keeping `non_volatile_as_local_keep = ["v64-v72"]` in ctx fixed it (shots of the logo as with the plain codegen).
- **`tools/post_codegen.py`**, set up for Peggle 1:
  - Peggle 1's memcpy (`0x8279B350`, byte for byte Peggle 2's for 1,328 bytes) and memset (`0x8279DF80`).
  - Peggle 1's entry point for `leaf_dead_stores.py` (`0x823D81C0`).
  - Peggle 2's three game patches (`mp_masters`, `fps_updates`, `costume_states`) left out.
  - Empty promotion and localized-copy lists until a census and a console profile of Peggle 1 exist.
  - Peggle 2's choice lists (`app/config`) set aside.
  - `tools/codegen.ps1 -PostPasses`; the build runs the passes after its own codegen re-runs (`PEGGLE1_PERF_POST` ON).
  - Some of the passes' counts:

    | Pass | Count |
    |---|---|
    | loop locals | 5,021 loops |
    | leaf locals | 2,056 functions |
    | memcpy / memset calls made inline | 508 / 204 |
    | `stvlx` / `stvrx` as stores | 751 / 261 |
    | loads folded to read-only constants | 1,105 more over the CFG |
    | tiny leaf functions inlined | 18,108 |
- **Checked on the PC, Renut on Vulkan:** the scripted route (logo video, title, menus, the Institute, Nights stage one, a level played). `[verify] generated-code self-checks: 22.5 M loads folded to read-only constants, 3.2 M critical-section owners from the PCR, 0 differences`; flush-mode flow: 68 M proven checks, 0 had to switch.
- **The Guwange session's never-ending loops:** a guest loop with no side effects may be deleted by clang. Its `spin_forever.py --check` finds 9 in Peggle 1 (read only): the seven network-error halts (`b .`) and inline copies. GCC keeps them on the Switch (`check_switch_build.py` sees them), so they are not changed.

### Switch shaders and build (2026-10-21)

- The deko3d shaders are made as in peggle2-nx's notes: Renut's XenosRecomp with `BANJO_HLSL_OVERRIDE=out/pc/renut_gpu/early_alpha_hlsl` and `BANJO_DEKO3D_SPIRV_DIR=out/deko3d_shaders/spirv`, then `deko3d_shaders.py`, `deko3d_glsl_opt.py --check`, `deko3d_shader_package.py --out out/renut_switch` and `renut_switch_shader_cache.py`.
  - All 82 shaders compile with uam; 67 vertex DKSH (median 35 KB, largest 207 KB) and 15 pixel.
  - The GLSL package is 534 KB and the SPIR-V cache 535 KB.
- `tools/win/build_switch.ps1` builds Renut with no Vulkan driver.
- The built-in settings (`app/peggle1_switch_defaults.toml`) are written by hand for now: Renut on deko3d, logs on, licence 1, guest sign-in, 720p in both modes. The player's file (`app/peggle1_switch.toml`) holds language and controllers.
- **Icon:** Peggle 2's was the user's own picture and has been set aside. Development NROs take `out/icon_dev.jpg`, the game's 64x64 `icon.png` scaled up (game data, never committed). A release needs a picture of the user's choosing.
- **Ryujinx:** Peggle's data folder (`D:\New folder\ryu-data\peggle2`) has `sdcard\switch\peggle1-nx`, a junction to `out/emu_sd/peggle1-nx`, which holds:
  - the game pack, from `tools/build_asset_pack.py`: 38 files, 104 MB
  - `dlc/` with the Nights package
  - the NRO and its toml

  The runner is `tools/win/p1_emu_run.sh`, with `tools/win/p1_emu_base.toml`.

## The first Switch NRO (2026-10-21)

- **Plain codegen first** (`out/nx/elf/peggle1_dev_nolto_20261021_1507.elf`; a local test build, not delivered):
  - Ryujinx, 150 s: logo video, title, menus, Nights stage one, a level played; no crash.
  - 60 game frames a second in play, with the render thread at 2.3-2.4 ms a frame (about 200 draws a frame; 500-800 in the menus).
  - `check_switch_build.py` failed until Peggle 1's lists went in: its seven guest halts and its two streaming copies (`sub_82467580`, `sub_82466D24`, 72 vperm each, as Peggle 2's).
  - The NVK keys Renut's tuning reads are defined in the Renut glue when the native renderer is not built.
- **The optimised codegen** (`out/nx/elf/peggle1_opt_dev_nolto_20261021_1604.elf`): `check_switch_build.py` all checks passed ("flavour: optimised codegen", no Vulkan driver, 0 vector FMAs, `-mearly-ra=none` everywhere). NRO 41.3 MB, against 52.5 with the plain codegen.
  - Ryujinx, 160 s: the same route and the logo video right; 60 game frames a second in play, render thread 2.9-3.4 ms a frame.
- **Delivered:** `out/dist/dev_20261021_peggle1/peggle1-nx/` (copy to `sdmc:/switch/`):
  - `peggle1.nro`: SHA-256 `3bca49b6...1916b94`, 41,318,360 bytes; no LTO / PGO; 720p in both modes; logs on
  - the game pack
  - `dlc/` with the Nights package
  - `deko3d_shaders.bin`: 17 variants compiled in the emulator
  - `README.txt`

### Console: the whole Switch froze at the first menu item (2026-10-21)

- **The user's report:** "Clicking any of the menu items in game dead hangs my switch and produces no crash log". Their SD card's logs (`F:\switch\peggle1-nx\logs`, two runs, docked and handheld) show:
  - title and main menu at 32-49 fps, render thread 1-2 ms, GPU 1-2 ms a frame
  - then the game lists its DLC, `XamContentCreate` opens Peggle Nights, and the content folder is mounted
  - then nothing from any thread: the 10 s reports stop too
  - the first log ends in binary garbage
- **The PC log shows the next step:** one `NtReadFile` of all of `content.pak`, 22,907,436 bytes, into a fresh guest allocation. The SD's `HostPathFile` read with `pread` straight into the guest buffer. On Horizon that is guest memory: process code memory, committed page by page on first touch (`core/guest_memory_switch.h`), which the file system refuses as a transfer buffer (`0xD401`).
  - The asset pack had met this and reads in 1 MB pieces, through a host buffer when one fails (`pack_storage_switch.cpp`).
  - Loose SD files had no such guard. Saves are small, and no port had read a large file from the SD before.
- **Fix** (`sdk/src/core/filesystem_posix.cpp`, Switch only): `PosixFileHandle::Read` / `Write` go through a 1 MB host buffer per thread, piece by piece. The results are the same as one `pread`/`pwrite`; it costs one memcpy (about 5 ms for those 23 MB).
- **Checked in Ryujinx:** "LOADING PEGGLE NIGHTS...", the Institute, Nights stage one, a level; `content.pak` read in 30-40 ms, in 2 of 3 runs. One run stalled 2 s into boot, before any SD read of this kind: the whole emulator stalled (its own log went on) while the game's threads were silent, as peggle2-nx saw once.
- **Delivered:** `out/dist/dev_20261021_peggle1/peggle1-nx/peggle1.nro`, ELF `out/nx/elf/peggle1_opt_dev_iofix_nolto_20261021_1625.elf`.
- **Next console run (the I/O fix):** white screen at the PopCap logo, two cores at 100 %. Every pack read finished (the asset pack is not involved). Right after `sa2_renut_core_layout` re-masked the 12 guest threads at the first Swap, every guest thread went silent (the renderer's and XMA's 10 s reports stop) while the host's report thread went on.
  - One Ryujinx run of the same NRO had stalled the same way.
  - banjo-nx turned its copy off for this hang (4 of 12 emulator runs).
  - It is explicit only now (`peggle1_enhancements.cpp`). Two Ryujinx runs went through the logo and the Nights load (`content.pak` in 29-38 ms).
  - ELF `out/nx/elf/peggle1_opt_dev_iofix_nolayout_nolto_20261021_1645.elf`.

## Hot functions, the job pool, saves, multiplayer and the exit (2026-10-21, evening)

- **30 localized copies** (`app/src/cpu/hot/localized.txt`): the 30 hottest guest functions by self time of the PC profile (`out/renut_tests/prof1`), without waits and without `sub_8231D050` (an `lwarx`/`stwcx.` atomic, which `localize_native.py` refuses; `sub_82747A38`, the next one, instead). Of Peggle 2's natives only memcpy (`sub_8279B350`) and memset (`sub_8279DF80`) are in Peggle 1 byte for byte (`tools/p2_function_matches.py`): both are hooked with Peggle 2's code and hashes.
  - PC, verify mode (every call checked): 6.53 M checks, 0 differences, all 32 hooks used.
  - Ryujinx (ARM), verify mode: 7.48 M checks, 0 differences, all 32 used (`out/renut_tests/emu/emu10`).
- **The job pool spun** (`app/src/peggle1_worker_pool.cpp`, key `peggle1_worker_pool_sleep`, CPU group).
  - The pool (`sub_8270D280`) starts five workers (`sub_8270D0F8`) on one manual-reset event. The producer (`sub_8270D330`) queues a job and sets the event under the pool's critical section.
  - Only a worker that loses the race for a job clears the event. On the 360 the five wake together. On a host the first one takes the job before the others look, and the event stays set.
  - Measured with `peggle1_diag_waits`: before, about 9-10 million satisfied waits per worker per 10 s; after, 2-30 per 10 s (PC and Ryujinx).
  - Fix: a satisfied wait from 0x8270D140 that finds no job clears the event, looks at the queue again and waits again. The producer counts before it sets, so no wake-up is lost.
- **Saves**: no save-game package. Progress is the title-specific profile settings: 0x63E83FFF (the profile's DataSync stream, little-endian; parser `sub_821F4C20`), 0x63E83FFE (768 bytes) and 0x63E83FFD (Nights, with a keyed SHA-1). `tools/peggle1_save.py` reads and writes the first.
  - The MultiPlayer menu needs profile +44 >= 1 (`sub_823C3F20`: "complete the first stage of Adventure Mode").
  - A version-19 stream with +44 = 1 opens it. After a duel the game rewrote it as version 28, 506 bytes (the size the format predicts), with +44 still 1.
- **Same Console Duel works** (PC `out/renut_tests/mp1`, Ryujinx `emu13`): menu, level choice, rules, player 2 joins, turns, results screen ("Saving..."), a second duel.
  - Player 2 is a controller without a sign-in. `sub_82645660` shows XUserGetName's buffer without checking the result, so stack bytes appeared as the name.
  - Fix (`sdk/src/kernel/xam/xam_user.cpp`): with `xam_guest_signin`, a connected controller without a profile is named like its guest ("User(n)").
  - Test harness: on the Switch the second-player script adds no idle pad, because the system's own pad is player 1.
- **Exit** ("Return To Game Library", Yes): `XamLoaderLaunchTitle` ends the title, the module thread ends, then `OnDestroy`.
  - The runtime is now reset before the window (`rex_app.cpp`).
  - Ryujinx (`emu14`): "Execution complete", "Switch: exiting process (0)", `ISelfController::Exit`, no exception.
  - On the PC the title stops but its main thread never finishes, so the window stays open (not looked into further).
- **Not port bugs**:
  - `std::thread::detach` throws on the Switch (libnx), which crashed the first emulator run with the PC-only wait counter on; it now keeps its thread.
  - One Ryujinx run died in libnx `mutexLock` on a static mutex ("invalid memory region", an emulator memory-tracking fault); the rerun was clean.
- **Delivered**: `out/dist/dev_20261021_peggle1/peggle1-nx/peggle1.nro`, SHA-256 `9569933a...e7a2bd1f`. ELF `out/nx/elf/peggle1_opt_dev_hot30_pool_exitfix_nolto_20261021_1825.elf`.

## Language, release and installer (2026-10-21, night)

- **Language**: `peggle1_language` in the toml works (Ryujinx with `de`: the German menus, `de.pak` read). Empty now
  follows the system's language (`app/src/peggle1_language.cpp`: the Switch's `setGetSystemLanguage`, Windows' display
  language on the PC; both Chinese scripts give `zh`, the game's traditional Chinese; Dutch and Russian give English).
  Ryujinx set to Japanese with the key empty: the Japanese menus.
- **The release (1.0.0)**: no log (`app/peggle1_switch_defaults.toml`: no log lines, files, stderr file or
  `deko3d.log`; a crash still writes `logs/rex/rex_crash.log`), no LTO and no PGO (the user's choice: Peggle is light;
  `PEGGLE1_SWITCH_LTO` defaults to off), the game's own icon (`app/icon.jpg`, the user's exception), and no game shader
  inside: `-Define "PEGGLE1_RENUT_RELEASE_SHADERS=ON"` builds against `out/renut_switch_release` (the shader table
  without SPIR-V, no built-in package; Peggle 1 has no port shaders), so the NRO reads `deko3d_glsl.bin` beside itself.
  The NRO is 40,536,134 bytes (SHA-256 `c91aec0c...dc6fa`), without a `BDKG` package or a zstd frame. Its icon was
  replaced after the first upload by the user's 512x512 picture: the ELF and the NRO's code part are the same bytes,
  only the asset section changed (the first upload, `7b654f69...`, had the 64x64 icon scaled up).
- **The shaders of the release** are the native chain without early alpha (`out/deko3d_release`; only
  `ps_13D1F9C0BB97B49D` and `ps_30F9B5EF9AB1346A` differ from the development package), because the installer page
  does not port the early alpha pass. All 82 compile with uam.
- **The installer page** (`D:\peggle 2\peggle1-nx-installer`, from peggle2-nx-installer):
  - Title Update #3 changes one of the 82 stored shader containers, so the page applies the update to `default.xex`
    (`lib/xexp.js`: the runtime's `XexModule::ApplyPatch` steps; `wasm/lzxdelta.*`: libmspack's LZX decoder with the
    old bytes as reference data). The patched image is the code generator's `out/default_image.bin` apart from 80
    bytes of the import table (0x830B0A40-0x830B0CA7) the loader fills in.
  - Its 82 shaders come out as the native chain's GLSL: 15 byte for byte, 67 with the same code and other SPIR-V
    temporary numbers (DXC v2025.1 against 1.8.2407). Package `941ee70b...fdf3`, pinned in `release/manifest.json`.
  - The game pack is `tools/build_asset_pack.py`'s byte for byte apart from the random pack id.
  - The zip (180 MB; 37 s in Node, 64 s in Chromium), extracted into a fresh SD folder and started in Ryujinx without a
    `peggle1.toml`: the game wrote its toml, installed Peggle Nights from `dlc/`, compiled 17 shader variants from
    `deko3d_glsl.bin` (none failed) and played the Nights stage; no `logs/` folder.
- **Ryujinx only**: two runs died in libnx `mutexLock` on the SDK's global lock, from the audio worker's
  `XAudioSubmitRenderDriverFrame` ("invalid memory region" in Ryujinx's memory tracking); the console never showed it
  (the user: "I don't get crashes on console").
