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
