# PrimeRecomp on macOS (Apple Silicon)

Status: **builds and runs** (2026-10-25, M3 Max, macOS 27). `make app` makes `build/PrimeRecomp.app`; started from
Finder its start screen set up the game files from a folder with `exefs` and `romfs` (copied, data image, game code
generated and compiled: 89 s) and Play boots the game: intro, title, the Frigate Orpheon's landing deck, HUD, pause
menu and map, at 60 fps with the Retina display's full resolution; the frigate played by hand (about 17 minutes).
Not tried yet: controllers, a long session, Intel Macs (not supported: the game code is built for Apple Silicon).

What MoltenVK needed (runtime/nvn/vulkan):
- Geometry shaders: Apple's GPUs have none. The pause map draws its rooms' fill through one (it projects each
  vertex and shades the face); on the Mac the vertex shader runs it instead (exec.c gs_emul_sources), which is exact
  for what the map draws. Geometry shaders that branch or cull (one depth-only pass so far) are still left out, as
  before (`[shader] GS ... drawn without it` in the log).
- Metal refuses pipelines that blend from a colour the fragment shader doesn't write: blending is off for targets a
  shader doesn't write, and dual-source blending takes the shader's second colour from its RT1 output (the gunship's
  glass), which every renderer gets now.
- The translated shaders' NaN-aware `fmin` / `fmax` / `clz` helpers are named `fminnm` / `fmaxnm` / `clz32`: Metal
  has functions of the old names, and the calls were ambiguous.

Known: the first time a new shader is drawn MoltenVK compiles it for Metal, which can show as a brief hitch or a
momentarily black object; later runs use the caches (`cache/` in the data folder, saved when the app quits).

## Running the app
1. Copy `PrimeRecomp.app` to Applications (or anywhere) and open it. A build you made yourself opens directly; one
   downloaded from the internet is ad-hoc signed, not notarized: if macOS refuses to open it, allow it under System
   Settings -> Privacy & Security -> Open Anyway.
2. The start screen asks for your own game files: choose the folder that holds `exefs` and `romfs`, then **Set up**.
   It copies them in and recompiles the game for your Mac (a few minutes, once).
3. Press **Play**.

Needs: a Mac with Apple Silicon (M1 or newer) and macOS 15 or newer, and about 15 GB of free disk space.

Everything the app writes lives in `~/Library/Application Support/io.github.primerecomp`: game files (`extracted/`),
the game code library, saves, settings (`primerecomp.ini`), shader caches. Started from Finder the log is
`primerecomp.log` there (the run before is `primerecomp.previous.log`): attach it to bug reports. "Delete game files"
on the start screen removes the copied game files again. `LARECOMP_DATA_ROOT=<dir>` uses another folder.

The renderer is the same Vulkan renderer as on Windows and Linux, on MoltenVK (Vulkan on Metal, bundled), so the
upscaler option is AMD FSR 1 as on the other systems.

## Building from source
Needs Xcode (or its command line tools), CMake, Ninja and Python 3 (`brew install cmake ninja`). From the repo root:

    make deps           # once: SDL3, LGPL FFmpeg, the Vulkan loader, MoltenVK, shaderc and the Vulkan / VMA headers
                        # from pinned sources into build/deps (built for macOS 15), plus Python 3.12 for the helper
    make -j             # -> build/primerecomp, the start screen (no game code yet)

The app itself holds no game code. Either set up the game files on its start screen (it then needs the generator and
the compiler, below), or recompile the game in the checkout:

    python3 -m venv tools/venv && tools/venv/bin/pip install capstone==5.0.9 unicorn==2.1.4 lz4==4.4.5
    # your own game files as extracted/exefs, extracted/romfs; the start screen's setup also makes extracted/romfs.bin
    make gen            # once: recompile the game from extracted/exefs
    make -j             # -> build/primerecomp and the game code library build/primegame.dylib

Run `./build/primerecomp` from the repo root: with `extracted/` in the starting folder it uses that folder for
everything (dev mode) instead of `~/Library/Application Support/io.github.primerecomp`.

## The app
    make gen-helper     # the game code generator without Python (build/helper/primegen)
    tools/fetch_zig.sh  # the compiler the start screen builds the game code with (build/zig)
    make app            # -> build/PrimeRecomp.app (runs on macOS 15+; no game code or game files inside)
    make dmg            # -> build/dist/PrimeRecomp-<version>-macOS.dmg (the app next to an Applications shortcut)

`make app` builds the runtime again in `build/mac15` against `build/deps` and packages it with
`tools/bundle_macos.sh`: the executable in `Contents/MacOS`, its libraries and MoltenVK in `Contents/Frameworks`,
MoltenVK's ICD file in `Contents/Resources/vulkan/icd.d` (where the Vulkan loader looks inside an app; the runtime also
points `VK_DRIVER_FILES` there), and the generator, zig and the game code's headers in `Contents/Resources`. The app
is ad-hoc signed.

## Debugging
- The `LARECOMP_*` test variables work as on the other systems (see HANDOFF.md, "How to work"), e.g.
  `LARECOMP_SHOT_AT=<frame>,...` with `LARECOMP_SHOT_DIR=<dir>`.
- `LARECOMP_VK_VALIDATION=1`: the Khronos validation layer, if installed (`brew install vulkan-validationlayers`,
  then `VK_ADD_LAYER_PATH=/opt/homebrew/share/vulkan/explicit_layer.d`).
- `LARECOMP_LAUNCHER_EXTRACT=<folder>`: the start screen's setup from that folder, logged, then exit.
