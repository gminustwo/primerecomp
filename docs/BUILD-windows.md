# Building PrimeRecomp on Windows (x86-64)

Status: part of main. Builds and runs the game on Vulkan (with your own game files); `make dist` packages the
release zip.

## One-time setup
Run in PowerShell from the repo root:

    powershell -ExecutionPolicy Bypass -File tools\setup_windows.ps1

This installs MSYS2 (UCRT64: clang, lld, make, SDL3, FFmpeg, Vulkan loader + headers, shaderc, Vulkan Memory
Allocator, validation layers), Python 3.12 for the tools (`tools\venv`: unicorn, capstone, lz4) and switches the
clone to LF line endings.

## Build
From an MSYS2 UCRT64 shell, or Git Bash with MSYS2 first on PATH:

    export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
    make gen        # once: recompile the game from extracted/exefs (your own game files; see the start screen)
    make -j         # -> build/primerecomp.exe and the game code library build/primegame.dll

Without `generated/` only the app is built: the start screen builds the game code itself when you set up the game
files (as for players), given the generator and the compiler (`make gen-helper`, `tools/fetch_zig.sh`).
`make runtime-objs` compiles the runtime only (quick check).

Run from a shell that has `C:\msys64\ucrt64\bin` on PATH (SDL3, FFmpeg, Vulkan, shaderc DLLs).

## Package
    make gen-helper     # the game code generator without Python (build/helper/primegen)
    tools/fetch_zig.sh  # the compiler the start screen builds the game code with (build/zig)
    make dist           # -> build/dist/PrimeRecomp-<version>-windows.zip (no game code inside)

The zip's `PrimeRecomp/` folder holds `primerecomp.exe` (icon from `docs/icon/icon.ico`, via windres),
`primegen/`, `zig/` and the DLLs. No installer: game files, saves and settings are kept next to the exe, so
the folder must be writable (the app says so and quits if it isn't, e.g. under Program Files). A source checkout
with `extracted/` in the starting folder keeps using that folder instead.

## Differences from macOS
- Guest window 0x70_0000_0000..0x73_2000_0000 is reserved with VirtualAlloc and committed per region
  (kernel regions, modules, HLE heap); stray first touches are committed by a vectored exception handler.
- AArch64 FP/SIMD instructions are lowered to C (`gen.py --simd=c`, the default on non-arm64 hosts) instead of
  inline-asm passthrough.
- Renderer: the same on both, Vulkan (runtime/nvn/vulkan; macOS through MoltenVK, docs/macos.md); shaders are
  translated to GLSL and compiled with shaderc.
- "Quit to Start Screen" reboots the game in place on both (runtime/main.c); the only re-exec left is the startup
  retry when the guest window is taken, which on Windows starts a new process and exits.

## Tests
- `tools/test_translate.sh`: shader translator fuzz (GLSL compiles and validates; its MSL comparison takes a
  reference translator from LAS Recomp's history, REF=<commit>, which this repository doesn't have).
- `tools/test_vk_pipelines.sh`: fuzzed shaders through the backend's compile/reflection into real pipelines.
- `LARECOMP_VK_VALIDATION=1` with `VK_ADD_LAYER_PATH` pointing at `C:\msys64\ucrt64\bin`: Khronos validation layer.
- Run test windows directly, not under MSYS `timeout` (it stalls the window).
