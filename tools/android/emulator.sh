#!/bin/bash
# tools/android/emulator.sh: starts the "PrimeRecomp" virtual device (tools/android/setup_sdk.sh --emulator) with the
# host's GPU and waits until it has booted; then tools/android/deploy.sh --log installs and runs the game in it.
# On an Apple Silicon Mac the arm64 system image runs natively (the APK as on a phone, Vulkan through the host's
# MoltenVK); elsewhere it is emulated and slow. See docs/android.md.
set -euo pipefail
cd "$(dirname "$0")/../.."
A=$PWD/build/android
export ANDROID_HOME="$A/sdk" ANDROID_SDK_ROOT="$A/sdk" ANDROID_AVD_HOME="$A/avd"
EMU=$A/sdk/emulator/emulator; [ -e "$EMU.exe" ] && EMU=$EMU.exe
ADB=$A/sdk/platform-tools/adb; [ -e "$ADB.exe" ] && ADB=$ADB.exe
[ -d "$A/avd/PrimeRecomp.avd" ] || { echo "no virtual device: tools/android/setup_sdk.sh --emulator"; exit 1; }
"$EMU" -avd PrimeRecomp -gpu host -no-snapshot-save -no-boot-anim > "$A/emulator.log" 2>&1 &
echo "emulator starting (its log: build/android/emulator.log)"
"$ADB" wait-for-device
until [ "$("$ADB" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ]; do sleep 2; done
echo "booted: $("$ADB" shell getprop ro.product.model | tr -d '\r'), Android $("$ADB" shell getprop ro.build.version.release | tr -d '\r')"
"$ADB" shell "dumpsys SurfaceFlinger 2>/dev/null | grep -i 'vulkan\|GLES' | head -3" || true
