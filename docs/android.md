# PrimeRecomp on Android (experimental)

**Status:** the Android build (arm64-v8a; Vulkan 1.3, so Android 13+ in practice) reaches the title screen in the
Android 15 emulator on an Apple Silicon Mac (v0.0.6: 40+ fps through the emulator's GPU forwarding, the game at full
speed). Not tried yet: real phones (Snapdragon / Adreno first), playing past the title screen. Expect a phone not to
boot the first time: the log says why.

What's there: the runtime as `libmain.so` in an APK (SDL3's Java activity around it), with SDL3, FFmpeg and shaderc
built for Android. What isn't: the start screen's setup (it builds the game code with zig, which a phone can't run) -
the game code library is built on a computer for now and copied over with the game files - and touch controls (a
controller or, in an emulator, the keyboard).

## Building (Windows: MSYS2 UCRT64 shell; macOS / Linux: a terminal)

With **Android Studio** (2026.2 or newer): in its SDK Manager, install Android SDK Platform 35 and, under SDK Tools,
NDK (Side by side) 30. The scripts use its SDK, NDK and JDK (`tools/android/env.sh`; `ANDROID_HOME` / `JAVA_HOME`
pick others):

    tools/android/build_deps.sh          # SDL3, shaderc, FFmpeg for arm64-v8a  (build/android/deps)
    tools/android/apk.sh                 # -> build/android/PrimeRecomp-<version>-android.apk  (no game code)

`apk.sh` builds the runtime (`libmain.so`), stages it with the libraries and assets in `build/android`, and runs
Gradle (`android/gradlew`). After it has run once, `android/` also opens as a project in Android Studio: Build >
Generate App Bundles or APKs > Build APK(s) packages the staged libraries again, and Run installs it on a device or
emulator. Rebuild the native code with `apk.sh` (Android Studio doesn't compile it). The version comes from the PC
builds' (`runtime/ui/launcher.mm`).

Without Android Studio, everything can go under `build/android` instead (nothing system-wide, about 5 GB): run
`tools/android/setup_sdk.sh` first (JDK 21, the SDK command-line tools, platform 35, NDK 30; `--emulator`: also the
emulator, below), then the two commands above.

The game code, from your own game files' `exefs` (the AArch64 generator mode, as on Linux aarch64 and macOS, but for
Android's guest window at `0x10_0000_0000` - below):

    cd tools
    ./venv/bin/python gen_window.py sdk  <exefs>/sdk  0x1006000000 ../generated-android/sdk --hle ../runtime/hle/hle_symbols.txt --simd asm
    ./venv/bin/python gen_window.py main <exefs>/main 0x1000000000 ../generated-android/main --simd asm
    cd .. && make PLATFORM=android build/android/build/primegame.so   # (+ .id)

(Windows: `./venv/Scripts/python`. On a Mac set up for the macOS version, `<exefs>` is
`~/Library/Application Support/io.github.primerecomp/extracted/exefs`. A `primegame.so` made for v0.0.6, at
`0x70_0000_0000` in `generated-aarch64`, isn't loaded any more: the log says so; make it again as above.)

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

- **The guest address range**: the game's memory is a fixed 12.5 GB window, at `0x10_0000_0000` on Android
  (`0x70_0000_0000` on a computer). A phone with a 39-bit address space (512 GB) has its system libraries all through
  `0x70_0000_0000` before the app starts - v0.0.6 stopped there on a POCO F5 (issue #20). On a PC the program
  re-executes itself until it gets its window; an Android app can't, so if something already sits there the log says
  `FATAL: guest address range ... is not free` with what holds it (`[guest] taken:` lines).
- **Vulkan**: `[vk]` lines name the device and what it lacks (the emulator's, then each phone's driver).
- **Crashes**: `logcat.txt` has the native backtrace; `build/android/build/libmain.so` and `primegame.so` keep their
  symbols for `ndk-stack` / `llvm-addr2line`.
