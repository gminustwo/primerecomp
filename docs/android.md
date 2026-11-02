# PrimeRecomp on Android (experimental)

**Status:** the Android build (arm64-v8a, Android 11+) compiles and packages, but hasn't run anywhere yet. The first
target is an emulator on an Apple Silicon Mac, then Snapdragon (Adreno) devices. Expect it not to boot the first time:
the log says why.

What's there: the runtime as `libmain.so` in an APK (SDL3's Java activity around it), with SDL3, FFmpeg and shaderc
built for Android. What isn't: the start screen's setup (it builds the game code with zig, which a phone can't run) -
the game code library is built on a computer for now and copied over with the game files - and touch controls (a
controller or, in an emulator, the keyboard).

## Building (Windows: MSYS2 UCRT64 shell; macOS / Linux: a terminal)

Everything goes under `build/android` (nothing system-wide, about 5 GB):

    tools/android/setup_sdk.sh           # JDK 21, Android SDK + NDK 30, Gradle  (--emulator: also the emulator, below)
    tools/android/build_deps.sh          # SDL3, shaderc, FFmpeg for arm64-v8a  (build/android/deps)
    tools/android/apk.sh                 # -> build/android/PrimeRecomp-<version>-android.apk  (17 MB, no game code)

The game code, from your own game files' `exefs` (the AArch64 generator mode, as on Linux aarch64 and macOS):

    cd tools
    ./venv/bin/python gen.py sdk  <exefs>/sdk  0x7006000000 ../generated-aarch64/sdk --hle ../runtime/hle/hle_symbols.txt --simd asm
    ./venv/bin/python gen.py main <exefs>/main 0x7000000000 ../generated-aarch64/main --simd asm
    cd .. && make PLATFORM=android build/android/build/primegame.so   # (+ .id)

(Windows: `./venv/Scripts/python`. On a Mac set up for the macOS version, `<exefs>` is
`~/Library/Application Support/io.github.primerecomp/extracted/exefs`.)

## Running

On a device with USB debugging on, or in the emulator:

    tools/android/deploy.sh [game folder] --log

installs the APK, copies the game code library and the game files (`exefs`, `romfs`: about 7 GB the first time) into
the app's folder on the shared storage (`/sdcard/Android/data/io.github.primerecomp/files`, the data root: saves,
settings and `primerecomp.log` live there too), starts the game, and after a minute saves `primerecomp.log` and
`logcat.txt` next to the script. The game folder defaults to this checkout's `private/game`, else the macOS / Linux
version's own `extracted/`.

## The emulator on an Apple Silicon Mac

The arm64 system image runs natively there, so this is the real APK as on a phone - except the GPU: Vulkan goes
through the emulator to the Mac's MoltenVK, with whatever features that passes on (the renderer wants Vulkan 1.3).

    tools/android/setup_sdk.sh --emulator   # + emulator, Android 15 arm64 image, virtual device "PrimeRecomp"
                                            #   (8 GB RAM, 32 GB storage, host GPU)
    tools/android/emulator.sh               # starts it, waits for the boot
    tools/android/deploy.sh --log           # (the first copy of the game files takes a few minutes)

Or with Android Studio: Device Manager -> a Pixel 8, Android 15 (API 35) **arm64-v8a** image, Show Advanced Settings:
RAM 8192 MB, internal storage 32 GB, graphics Hardware; start it, then `deploy.sh --log` (it finds Android Studio's
adb in `~/Library/Android/sdk`).

The keyboard works as on the PC (the emulator passes it through); a controller usually doesn't reach the emulator.

## What the first runs will tell

- **The guest address range**: the game's memory is a fixed 12.5 GB window at `0x7000000000`. On a PC the program
  re-executes itself until it gets it; an Android app can't, so if something already sits there the log says
  `FATAL: guest address range ... is not free` with what holds it (`[guest] taken:` lines).
- **Vulkan**: `[vk]` lines name the device and what it lacks (the emulator's, then each phone's driver).
- **Crashes**: `logcat.txt` has the native backtrace; `build/android/build/libmain.so` and `primegame.so` keep their
  symbols for `ndk-stack` / `llvm-addr2line`.
