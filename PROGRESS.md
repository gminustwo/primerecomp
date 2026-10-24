# Progress

## 2026-10-21: project start

Forked from LAS Recomp `b846e72` (history kept, remote `lasrecomp`).

### The game files

| | |
|---|---|
| Title ID | `010012101468C000`, v0 (1.0.0) |
| SDK | 12.3.1.0 |
| exefs | `main` (16.9 MB text, ~75 MB in memory with data/bss), `sdk`, `subsdk0` (nnMedia: `movie::`, Android stagefright, mkvparser), `rtld` |

`main` keeps its dynamic symbol table: **102,587 exported symbols with C++ names** (`CPlayerMP1`,
`CMorphBallMP1`, `CStateManagerGameLogicMP1`, `NScriptLoader`, `rstl`...). Nearly every function in the game has
a name, which makes reverse engineering far cheaper than it was for Link's Awakening.

### The generator works on it unchanged

`tools/gen.py` (LAS Recomp's lifter) on all three modules, Windows, `--simd c`:

| module | functions | C | time | unsupported instructions |
|---|---|---|---|---|
| main | 90,768 | 274 MB, 236 files | 16 s | **0** |
| sdk | 44,792 | 97 MB, 104 files | 6 s | **0** |
| subsdk0 | 17,615 | 57 MB, 96 files | 3 s | **0** |

Spot check: `CMorphBallMP1::PreRenderBallShadow` (`0x7000ced70c`) lifts to clean C.

### NVN coverage

MPR's `main` resolves 525 NVN functions (`pfnc_nvn*`). The inherited renderer implements 495 of them. The 32
missing ones:

- compute: `DispatchCompute`, `DispatchComputeIndirect`, `Barrier`
- indirect draws: `DrawArraysIndirect`, `DrawElementsIndirect`
- sync/events: `SignalEvent`, `WaitEvent`, `WaitSync`
- clears/copies: `ClearBuffer`, `ClearDepthStencil`, `CopyBufferToBuffer`
- fixed-function state: `SetAlphaRef`, `SetDepthBounds`, `SetLineWidth`, `SetPointSize`, `SetPolygonOffsetClamp`,
  `SetPrimitiveRestart`, `SetRenderEnable`, `SetSampleMask`, `ReportCounter`
- debug/no-op: checkpoints, `OverrideSubtileSize`, `ProgramGetSubtileSize`, `TextureBuilderGetViewOffset`,
  `DeviceGetProcAddress` (entry exists through the bootstrap loader HLE)

Compute is the big one: LA never used it, so the Vulkan backend needs compute pipelines and Maxwell compute
shader translation.

### Changes so far

- `tools/patches.py`: LA's per-instruction patches removed (they target LA addresses, which land on unrelated MPR
  code).
- Memory layout: `SDK_BASE` moved from `0x7004000000` to `0x7006000000` because MPR's `main` is larger than LA's
  64 MB slot.
- Modules are a list (`GAME_MODULES` in `runtime/rt.h`), not hardcoded main + sdk: loader, dispatcher, module init,
  the on-device game build (`runtime/gamebuild.c`) and the game library ABI (`GAMELIB_ABI` 2, `tools/gamelib_entry.c`).
- `subsdk0` is **not loaded**. As in LAS Recomp, `movie::` is host code on FFmpeg (`runtime/hle/movie.c`), and
  host implementations win over guest exports (`runtime/loader.c` `resolve`).

## 2026-10-21: LA game code and the randomizer removed

Everything that read or wrote Link's Awakening's memory, and the randomizer, is gone:

- `runtime/hle/`: chamber, digs, doorjump, escape, flags_ext, framepace, hookshot, keysanity, miniboss, mirror, pets,
  pot_glow, qol, rescue, shopsanity, souls, warp, widescreen. Kept, SDK level: bindings, heaps, input, movie,
  nvn_boot, pl, swkbd, vi. framepace read LA's frame timer (`f_700000c910`); MPR will need its own.
- `runtime/rando.[ch]`, `third_party/LAS-Randomizer`, `tools/rando/` (and `gen.py`'s randomizer instruction
  variants: they had been compiled into MPR's `main` at 9 LA offsets; regenerated, 0 left), `tools/warp_table.py`,
  `tools/perf_sweep.py`, the LA walk / door test scripts.
- UI: Save Editor, Randomizer, Warp, Cheats and Gameplay tabs, the tracker / inventory / chaos overlays, spoiler
  view. The menu keeps Controls, Video, Audio, Cosmetics and Quit.
- The renderers (Vulkan and Metal) no longer call into LA hooks: pot glow, Escape Bottle tint, pets cursor, mirror
  mode, per-room widescreen framing and effect stretching. `runtime/hle/game.c` answers what they still ask
  (current area for the logs: unknown; may the window go wider than 16:9: no).
- `fs.c`: saves no longer go per randomizer seed. The RomFS overlay (replacement / added files served from a folder)
  is kept, with no folder set (`overlay_dir`).
- Build: `make gen-helper` uses its own venv (`build/gen-venv`); `make rando-helper` is gone; `make dist` and
  `tools/setup_windows.ps1` no longer need the randomizer.

Checked: every Windows runtime source compiles to an object with MSYS2 clang64 (except movie.c, launcher.mm,
shaders.c and smaa_data.c: no FFmpeg / shaderc / SMAA table headers on this machine, same as before the change);
the symbols used across those objects resolve among them, as before. The Metal files (`metal.m`, `renderer.m`)
were edited the same way but not compiled (needs macOS). Not linked or run yet.

### Next

1. **LA presentation features.** These touch no guest memory but are LA's: `runtime/nvn/texpatch.c` (button
   prompts, Hero Mode icon, tunic and heart colours: LA's UI textures by content), the Cosmetics tab, the Video
   tab's "Remove blur" / "Hide Hero Mode icon", Controls' "D-pad moves Link" / "360-degree movement", their
   settings (`runtime/settings.c`), the launcher's seed picker (`rando/` folders). Release packaging for Linux
   (`tools/linux/`: AppImage, `rando_helper.sh`, smoke test) and macOS (`tools/bundle_macos.sh`) still expects the
   seed generator, and `docs/` still describes it.
2. **movie:: entry points.** MPR's `main` imports 41 `movie::` functions and the current HLE covers 15. LA reached
   Decoder/Extractor through vtable slots; MPR calls 26 of those methods directly
   (`movie::Decoder::Configure`, `movie::Extractor::SetDataSource(ClientStreamReader*, const char*)`, `SeekTo`,
   `SetCoreMask`...). Each needs an `HLE_FUNC` wrapper onto the same FFmpeg state.
3. **Identity.** Title ID in `runtime/kernel.c` (`0x01006BB00C6F0000` → `0x010012101468C000`), save/ini/library
   names (`lasrecomp`, `lasgame.dll`, `LARECOMP_*`), app title and icon.
4. **First boot.** Build on Windows (MSYS2 UCRT64 needs clang: `tools/setup_windows.ps1`), boot to `nnMain`, then
   go through the stubbed services and imports one by one from the log.
5. **Graphics.** The 32 missing NVN functions, compute first. MPR's shaders and texture formats are untested
   against the Maxwell translator.

## 2026-10-21 (later): boots, menus, intro at 60 fps

Working: boot to title (intro movies via movie:: over the game's ClientStreamReader), title / main menu, new game
starts, frame pacing locked at 60 (or 120 / uncapped: Video tab "Frame rate", `LARECOMP_FPS`), 3D scene renders.

## 2026-10-21 (evening): in-game, playable start

Working: the intro flyby renders with correct lighting and color grading; the landing on the Orpheon hands over
control; first-person HUD, walking, firing and the in-game tutorial prompts work. 60 fps locked; 120 fps locked with
the game running at the same speed (4 s of walking covers the same ground at 60 and at 120).

What fixed the black screen: the exposure data comes from a vertex shader that stores to an image (SUST), and
the color grading LUTs are 3D textures whose tiling the runtime guessed wrong (see tegra.c).

Open, in order:
1. **Graphics glitches:** the user's open reports (HANDOFF.md). (The dark arm cannon and the black streaks at the
   landing site were the sparse lighting volumes, fixed 2026-10-21.)
2. **Not verified yet:** bosses, every upgrade, the ending (the user plays through; audio is checked by WAV capture,
   saving and loading by the user's own saves).
3. Shader instructions still unhandled: `SUST.BA` (raw image stores), `TLD4.PTP`, gather on cube textures - none
   seen so far.
4. LA leftovers: texpatch.c (its options are hidden from the menu since 2026-10-23), names (`lasrecomp.exe`,
   `LARECOMP_*`), launcher text.

Debug recipe: `LARECOMP_DEBUG_FRAME=<n>` (pass images in build/build/, draw log), `LARECOMP_GPUPROF_AT`,
`LARECOMP_VK_PROF=1` (waits by cause), `LARECOMP_HANG_DUMP`, `LARECOMP_FNTRACE_THREAD` (with `LARECOMP_FNPROF=0:999999999`).

## 2026-10-21 (night): lighting, saves

- Arm cannon / black shadows: the lighting volumes are sparse textures (virtual memory pools); mapped and uploaded now.
- Saving and loading verified end to end: `LARECOMP_AUTOSAVE_AT=<frame>` makes the game run its own autosave
  (CStateManager::StartAutoSave, hooked from CStateManager::BeginFrame) - save.bin grows from 4219 to 6900 bytes
  (a slot added), the slot list shows it after a restart and loading it starts the frigate again. Test runs use
  `LARECOMP_SAVE_DIR` (a scratch copy). The slot's area name reads "<ERROR>" for that save, maybe because it
  wasn't made in a save room - to check against a save station save.
- Menu: Esc no longer opens it while a controller is connected (Steam desktop mode sends Esc for B).

## 2026-10-22: space backdrop, door shields

- Stars and Tallon IV in space: the volumetric fog writer's last slice was never stored (the translator ignored
  condition-code tests on EXIT / BRA and ISET's .CC), and the sky samples that slice.
- Blue door shields: the refraction mip chain is rendered through one texture object per level and sampled
  through another; those objects now share one image (GPU-written, uncompressed textures only: sharing CPU
  textures broke the next room).
- Test runs from PowerShell: `tools/run_game.ps1` (Git Bash launches lost the `LARECOMP_*` variables).

## 2026-10-23: sound effects, rumble

- The user: no cannon sounds after loading a save. Most sound effects in play go through audio splitters (each sound
  a splitter of 5 destinations: the panned dry send and a reverb send), which were ignored - the cannon was silent in
  a new game too. Splitters implemented, and the game's effects ported from Ryujinx: I3DL2 reverb (the reverb send
  bus, wet only), delay, biquad filter, aux (the final mix's software limiter: fed, output passed through) and depop.
- Rumble: the game's SendVibrationValue(s) reach the controller (they were dropped); Audio tab: Rumble.
- The user's save is in Magmoor Caverns (Save Station Magmoor A): test runs load it to look at the area they play.
- Menu: Link's Awakening leftovers hidden (Cosmetics, blur, Hero Mode icon, movement and prompt options).
- `tools/run_game.ps1` no longer stops the user's playtest game.

## 2026-10-23 (evening): 4K, Xbox face layout, Gameplay tab

- The user confirmed the cannon sounds. 4K: MPR presents 1600x900 and the render scale took Link's Awakening's 1080 as
  its base, so Match display drew 5/6 of the window (3200x1800 on a 4K screen); the scale follows the presented
  image now, and Internal resolution offers 900p (Switch) / 1080p / 1440p / 4K / 8K.
- Controls: Face buttons, Nintendo (by position) or Xbox (by label) layout - bindings and menus.
- Gameplay tab: Field of view (the game's default first-person FOV and the live camera's, set each frame) and Skip
  any cutscene. Aspect ratio and Movie audio out of the menu; Quit -> Restart Game (reboots to the game's title);
  the window says MPR Recomp.
