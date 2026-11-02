#!/bin/bash
# tools/android/deploy.sh [game folder] [--no-game] [--launch] [--log]: puts the APK, the game code library and the
# game files on the Android device (or emulator) adb sees, in the app's folder on the shared storage
# (/sdcard/Android/data/io.github.primerecomp/files: the data root, runtime/platform/linux.c), as the start screen's
# setup does on a PC:
#   extracted/exefs, extracted/romfs   from the game folder: a folder holding exefs and romfs - by default this
#                                      checkout's private/game, or the PC / Mac / Linux version's own extracted/
#   extracted/primegame.so(.id)        the game code library: next to this script (a test kit), else
#                                      make PLATFORM=android build/android/build/primegame.so
# The APK: next to this script, else the newest build/android/PrimeRecomp-*-android.apk (tools/android/apk.sh).
# --no-game: the APK and library only (adb push skips unchanged files anyway). --launch: start the app; --log: then
# follow the runtime's log (primerecomp.log) and the app's logcat, and save both next to this script.
# bash 3.2 (macOS) compatible.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
PKG=io.github.primerecomp
ROOT=/sdcard/Android/data/$PKG/files
GAME= NOGAME= LAUNCH= LOG=
for a in "$@"; do case $a in --no-game) NOGAME=1 ;; --launch) LAUNCH=1 ;; --log) LAUNCH=1 LOG=1 ;; *) GAME=$a ;; esac; done

REPO=
[ -f "$HERE/../../Makefile" ] && REPO=$(cd "$HERE/../.." && pwd)
first() { for p in "$@"; do [ -n "$p" ] && [ -e "$p" ] && { echo "$p"; return; }; done; }
ADB=${ADB:-$(first ${REPO:+"$REPO/build/android/sdk/platform-tools/adb"} ${REPO:+"$REPO/build/android/sdk/platform-tools/adb.exe"} \
    ${ANDROID_HOME:+"$ANDROID_HOME/platform-tools/adb"} "$HOME/Library/Android/sdk/platform-tools/adb" \
    "$HOME/Android/Sdk/platform-tools/adb" "$(command -v adb 2>/dev/null || true)")}
[ -n "$ADB" ] || { echo "adb not found: install Android Studio (or the SDK platform-tools), or set ADB=<path>"; exit 1; }
"$ADB" get-state >/dev/null 2>&1 || { echo "no Android device or emulator running ($ADB devices)"; exit 1; }

APK=$(first "$(ls -t "$HERE"/PrimeRecomp-*-android.apk 2>/dev/null | head -1)" \
    ${REPO:+"$(ls -t "$REPO"/build/android/PrimeRecomp-*-android.apk 2>/dev/null | head -1)"})
[ -n "$APK" ] || { echo "no PrimeRecomp-*-android.apk (tools/android/apk.sh)"; exit 1; }
LIB=$(first "$HERE/primegame.so")
if [ -z "$LIB" ] && [ -n "$REPO" ]; then
    (cd "$REPO" && make PLATFORM=android -j"$(getconf _NPROCESSORS_ONLN)" build/android/build/primegame.so)
    LIB=$REPO/build/android/build/primegame.so
fi
[ -n "$LIB" ] && [ -f "$LIB.id" ] || { echo "no game code library (primegame.so + primegame.so.id)"; exit 1; }
if [ -z "$GAME" ]; then
    GAME=$(first ${REPO:+"$REPO/private/game"} "$HOME/Library/Application Support/io.github.primerecomp/extracted" \
        "${XDG_DATA_HOME:-$HOME/.local/share}/primerecomp/extracted")
fi

echo "installing $(basename "$APK")"
"$ADB" install -r "$APK" >/dev/null
"$ADB" shell mkdir -p "$ROOT/extracted"
echo "game code library: $LIB"
"$ADB" push "$LIB" "$LIB.id" "$ROOT/extracted/" | tail -1
if [ -z "$NOGAME" ]; then
    [ -n "$GAME" ] && [ -f "$GAME/exefs/main" ] && [ -d "$GAME/romfs" ] ||
        { echo "no game files (a folder holding exefs and romfs): give its path"; exit 1; }
    echo "copying the game files from $GAME (the first time: ~7 GB, several minutes)"
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
    echo "following the log for 60 s (saved as primerecomp.log and logcat.txt next to this script)"
    sleep 60
    "$ADB" shell cat "$ROOT/primerecomp.log" > "$HERE/primerecomp.log" 2>/dev/null || true
    "$ADB" logcat -d > "$HERE/logcat.txt" 2>/dev/null || true
    tail -30 "$HERE/primerecomp.log"
    echo "--- logcat (errors) ---"
    grep -E " [EF] |FATAL|SIGSEGV|libc  |DEBUG   " "$HERE/logcat.txt" | tail -30 || true
fi
