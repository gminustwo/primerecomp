# PrimeRecomp on Linux (x86-64, aarch64)

Status: **x86-64 builds and runs; not yet tried on real Linux hardware.** The x86-64 AppImage (`make appimage`) was
tested in WSL2 (Ubuntu 24.04) on Mesa's software renderer (llvmpipe): the start screen, a full setup from the game
files (game code generated and compiled in 106 s on 32 threads), the title screen and a loaded save in Chozo Ruins.
Real GPUs (the Steam Deck's RADV, Intel ANV, NVIDIA) haven't been tried yet. aarch64 isn't built for PrimeRecomp yet.

## Steam Deck
The Steam Deck runs the x86-64 AppImage (current SteamOS has a newer glibc than the 2.35 it needs, RADV, and an AVX2
CPU). Set it up once in Desktop Mode; play it from Game Mode afterwards:

1. In Desktop Mode, copy `PrimeRecomp-<version>-linux-x86_64.AppImage` somewhere in your home folder (e.g.
   `~/Games/PrimeRecomp/`). In Dolphin: right-click it -> Properties -> Permissions -> "Is executable" (or
   `chmod +x` in Konsole).
2. Copy your own game files (the folder holding `exefs` and `romfs`) onto the Deck or its SD card.
3. Start the AppImage. On the start screen pick that folder under Game files folder, then **Set up game files**. It
   copies the game files to `~/.local/share/primerecomp` (about 15 GB on the internal drive) and recompiles the
   game for the Deck; that takes several minutes. Your copied game files can be deleted afterwards.
4. Add it to Steam: Steam (desktop) -> Games -> Add a Non-Steam Game to My Library -> Browse, set the file type to
   "All Files", pick the AppImage. It then shows up in Game Mode.
5. In Game Mode the Deck's controls arrive as Steam's Xbox controller. Open the PrimeRecomp menu with L3+R3 (press
   both sticks) -> Controls -> Face buttons -> Xbox, so the button marked A is A.

To keep those 15 GB on the SD card instead, move `~/.local/share/primerecomp` there before the setup and leave a
link in its place (`ln -s /run/media/deck/<card name>/primerecomp ~/.local/share/primerecomp`). The folder picker is a
desktop file dialog, which is why the setup is done in Desktop Mode. The log is
`~/.local/share/primerecomp/primerecomp.log`: attach it to bug reports.

## Running the AppImage
    chmod +x PrimeRecomp-<version>-linux-x86_64.AppImage
    ./PrimeRecomp-<version>-linux-x86_64.AppImage

Needs: a distribution with glibc 2.35 or newer (Ubuntu 22.04, Debian 12, Fedora 36, Arch, ...), a Vulkan 1.3 driver
(Mesa RADV / ANV / NVK / Turnip / Asahi Honeykrisp, or NVIDIA's), and on x86-64 a CPU with AVX2 (2013 or newer).
The start screen asks for the folder with your own game files and sets them up.

Everything the app writes lives in `~/.local/share/primerecomp` (`$XDG_DATA_HOME/primerecomp`): game files (`extracted/`),
saves, settings (`primerecomp.ini`), shader caches, imported fonts. Started without a terminal the log is
`~/.local/share/primerecomp/primerecomp.log` (from a terminal it prints there instead; the run before is
`primerecomp.previous.log`). `LARECOMP_DATA_ROOT=<dir>` uses another folder.

Controllers work as on the other systems (SDL3's gamepad support, bundled): Xbox, PlayStation (DualShock 4,
DualSense), Switch Pro / Joy-Con, 8BitDo and other pads through the kernel's drivers, plugged in or paired while the
game runs. For SDL's own drivers (rumble, LEDs and gyro on PlayStation / Switch pads) the user needs access to
`/dev/hidraw*`, which most distributions give with Steam's udev rules (`steam-devices`, or Steam installed); without
them the controller still works through the kernel driver. Started from Steam with Steam Input on, the game sees
Steam's virtual Xbox controller (F1 / L3+R3 -> Controls -> Face buttons: the Xbox layout puts A on the bottom button).

If the AppImage won't start with a FUSE error, install your distribution's `fuse3` package (the AppImage runtime
needs `fusermount3`), or run it as `./PrimeRecomp-...-linux-x86_64.AppImage --appimage-extract-and-run`.

## Building from source
Ubuntu / Debian packages (other distributions: the same libraries under their names; SDL3 3.2 or newer):

    sudo apt install clang lld make pkg-config python3-venv libsdl3-dev libavformat-dev libavcodec-dev \
        libavutil-dev libswscale-dev libvulkan-dev libshaderc-dev libvulkan-memory-allocator-dev glslc vulkan-tools

Then from the repo root:

    python3 -m venv tools/venv && tools/venv/bin/pip install lz4 capstone unicorn
    make -j         # the start screen only (no game code yet): set up the game files there, or put extracted/ here
    make gen        # once: recompile the game from your own game files
    make -j         # -> build/primerecomp and the game code library build/primegame.so

Run `./build/primerecomp` from the repo root: with `extracted/` in the starting folder it uses that folder for
everything (dev mode) instead of `~/.local/share/primerecomp`.

`ARCH` picks the target, separately from the OS: the generated code differs per arch (aarch64 runs the guest's
FP/SIMD as inline asm, like Apple Silicon; x86-64 lowers it to C with `-march=x86-64-v3`, like Windows). `make gen`
passes the matching `--simd`. A cross build (`ARCH=aarch64` on an x86-64 machine, `SYSROOT=<arm64 root filesystem>`)
keeps its own `generated-aarch64/` and `build/aarch64/`. Distributions that name shaderc's library
`libshaderc_shared.so` need `SHADERC=-lshaderc_shared`.

## AppImages
Needs podman, and for aarch64 QEMU user emulation with binfmt (`sudo apt install podman qemu-user qemu-user-binfmt`).

    make appimage                # this machine's arch -> build/dist/PrimeRecomp-<version>-linux-<arch>.AppImage
    make appimage ARCH=aarch64   # cross-compiled from x86-64
    make appimages               # both

Everything is built in Ubuntu 22.04 containers (`tools/linux/Containerfile`), so the result runs on glibc 2.35+:
SDL3, an LGPL-only FFmpeg (only the movie codecs, as on Windows / macOS), and shaderc from pinned sources
(`tools/linux/build_deps.sh`), then the game against them (`tools/linux/package.sh`). aarch64 is cross-compiled in
the amd64 container against the arm64 image's root filesystem (`build/linux/aarch64/sysroot`). The game code generator
helper (`tools/linux/gen_helper.sh`, PyInstaller) is frozen in a container of its own arch (on arm64 under QEMU). No
game code goes in: the start screen's setup generates and compiles it on the player's machine (`primegen`, `zig cc`).
From Windows, the same works inside WSL2 (Ubuntu with `podman` installed; build from a copy of the checkout on the
WSL file system, not under `/mnt`).

The AppImage holds `primerecomp`, `primegen/`, `zig/`, the start screen font and the bundled libraries (SDL3,
FFmpeg, shaderc). Not bundled: glibc, libstdc++, the Vulkan loader and the GPU drivers (always the system's). No game
files or fonts.

## Differences from macOS / Windows
- Platform layer `runtime/platform/linux.c`: like macOS, the guest window (0x70_0000_0000..0x73_2000_0000) is one
  read/write mapping (`MAP_NORESERVE`), crashes are caught with `sigaction` on an alternate stack.
- The HLE / service / reset tables are ELF sections found through the linker's `__start_` / `__stop_` symbols.
- Renderer: the Windows Vulkan backend unchanged. No page write tracking yet (Windows: `MEM_WRITE_WATCH`): the vertex
  / index cache reads guest memory in place (runtime/nvn/vulkan/exec.c), which costs little on an APU such as the
  Steam Deck's.
- Without the user's Switch fonts the game text uses DejaVu Sans (TrueType; the game's font engine can't use CFF
  fonts such as Noto Sans CJK).
- Pages: 4K and 16K kernels (Asahi) work; 64K-page kernels (some ARM servers) are not supported.

## Testing on ARM (aarch64): what to report
The aarch64 AppImage is experimental: built on x86-64, only its start screen has run (emulated): reports from real hardware
(Asahi Linux, Snapdragon laptops, Raspberry Pi 5 and other boards) are what tells whether it works. Please include:

1. **Distribution and kernel**: `cat /etc/os-release | head -3; uname -r; getconf PAGESIZE`
2. **GPU and Vulkan driver**: `vulkaninfo --summary` (package `vulkan-tools`); at least the devices section with
   `deviceName`, `driverName` and `driverInfo`.
3. **What happened**: start screen? extraction? in game? where it stopped, any visual problems (screenshots help).
   With a controller: its model, and whether it works (the log names it: `[input] gamepad: ...`).
4. **The log**: `~/.local/share/primerecomp/primerecomp.log` (or the terminal output when started from a terminal).
   A crash writes `*** signal ...` and a backtrace there; attach the whole file.
5. **A crash backtrace with gdb** when the log's isn't enough:

       ./PrimeRecomp-<version>-linux-aarch64.AppImage --appimage-extract    # -> squashfs-root/
       cd ~/.local/share/primerecomp
       gdb -batch -ex 'handle SIGSEGV nostop noprint pass' -ex run -ex 'thread apply all bt 20' \
           --args ~/squashfs-root/usr/bin/primerecomp > gdb.txt 2>&1

   (Adjust the `squashfs-root` path to where you extracted it. The game itself uses SIGSEGV for memory watches, hence
   the `handle` line; a real crash still ends in the backtrace. Attach `gdb.txt`.)
