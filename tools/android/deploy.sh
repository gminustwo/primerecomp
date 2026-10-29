#!/bin/bash
# tools/android/deploy.sh [game folder] [--no-game] [--launch] [--log]: puts the APK, the game code library and the
# game files on the Android device adb sees, in the app's folder on the shared storage
# (/sdcard/Android/data/io.github.primerecomp/files: the data root, runtime/platform/linux.c), as the start screen's
# setup does on a PC:
#   extracted/exefs, extracted/romfs   from the game folder (default private/game: a folder holding exefs and romfs)
#   extracted/primegame.so(.id)        make PLATFORM=android build/android/build/primegame.so
# --no-game: the APK and library only (the game files are already there; adb push skips unchanged files anyway).
# --launch: start the app; --log: then follow the runtime's log (primerecomp.log) and the app's logcat.
set -euo pipefail
cd "$(dirname "$0")/../.."
A=$PWD/build/android
ADB=${ADB:-$A/sdk/platform-tools/adb}
PKG=io.github.primerecomp
ROOT=/sdcard/Android/data/$PKG/files
GAME=private/game NOGAME= LAUNCH= LOG=
for a in "$@"; do case $a in --no-game) NOGAME=1 ;; --launch) LAUNCH=1 ;; --log) LAUNCH=1 LOG=1 ;; *) GAME=$a ;; esac; done

"$ADB" get-state >/dev/null 2>&1 || { echo "no Android device: connect it with USB debugging on ($ADB devices)"; exit 1; }
APK=$(ls -t "$A"/PrimeRecomp-*-android.apk | head -1)
echo "installing $(basename "$APK")"
"$ADB" install -r "$APK" >/dev/null
make PLATFORM=android -j"$(nproc)" build/android/build/primegame.so
"$ADB" shell mkdir -p "$ROOT/extracted"
"$ADB" push "$A/build/primegame.so" "$A/build/primegame.so.id" "$ROOT/extracted/" | tail -1
if [ -z "$NOGAME" ]; then
    [ -f "$GAME/exefs/main" ] && [ -d "$GAME/romfs" ] || { echo "$GAME: no exefs/main and romfs here"; exit 1; }
    echo "copying the game files (the first time: ~7 GB, several minutes)"
    "$ADB" push --sync "$GAME/exefs" "$ROOT/extracted/" | tail -1
    "$ADB" push --sync "$GAME/romfs" "$ROOT/extracted/" | tail -1
fi
if [ -n "$LAUNCH" ]; then
    "$ADB" shell am force-stop $PKG
    "$ADB" logcat -c
    "$ADB" shell am start -n $PKG/.PrimeActivity >/dev/null
    echo "started"
fi
if [ -n "$LOG" ]; then
    sleep 3
    "$ADB" shell "tail -f $ROOT/primerecomp.log" &
    "$ADB" logcat -s SDL SDL/APP DEBUG AndroidRuntime libc '*:F'
fi
