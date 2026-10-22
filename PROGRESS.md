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

### Next

1. **Strip LA game code.** These read or write LA addresses and must go before the first boot: `runtime/hle/`
   chamber, digs, doorjump, escape, flags_ext, hookshot, keysanity, miniboss, mirror, pets, pot_glow, qol, rescue,
   shopsanity, souls, warp, widescreen, `runtime/rando.c`, the randomizer UI tabs, `third_party/LAS-Randomizer`.
   Keep the SDK-level HLE (heaps, input, movie, nvn_boot, pl, swkbd, vi, framepace).
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
