#!/bin/bash
# tools/android/setup_sdk.sh [--emulator]: what the Android build needs, under build/android (nothing system-wide): a
# JDK 21 (Temurin), the SDK command-line tools, platform-tools (adb), platform 35, build-tools 36.1, NDK 30, Gradle.
# --emulator also: the emulator, an Android 15 arm64 system image (native on Apple Silicon Macs) and a virtual device
# "PrimeRecomp" (8 GB RAM, 32 GB storage, host GPU); start it with tools/android/emulator.sh.
# Windows (MSYS2 shell), macOS, Linux. bash 3.2 compatible. See docs/android.md.
set -euo pipefail
cd "$(dirname "$0")/../.."
A=$PWD/build/android
mkdir -p "$A/dl"
case "$(uname -s)" in MINGW*|MSYS*) OS=windows; CLT=win ;; Darwin) OS=mac; CLT=mac ;; *) OS=linux; CLT=linux ;; esac
case "$(uname -m)" in arm64|aarch64) JARCH=aarch64 ;; *) JARCH=x64 ;; esac
W() { cygpath -m "$1" 2>/dev/null || echo "$1"; }  # (native Windows programs want Windows paths)
get() { [ -s "$A/dl/$2" ] || { curl -fL --retry 3 -o "$A/dl/$2.part" "$1" && mv "$A/dl/$2.part" "$A/dl/$2"; }; }

EXT=tar.gz; [ $OS = windows ] && EXT=zip
get "https://api.adoptium.net/v3/binary/latest/21/ga/$OS/$JARCH/jdk/hotspot/normal/eclipse" "jdk21-$OS-$JARCH.$EXT"
get "https://dl.google.com/android/repository/commandlinetools-$CLT-16111833_latest.zip" "cmdline-tools-$CLT.zip"
get "https://services.gradle.org/distributions/gradle-8.14.3-bin.zip" gradle.zip
if [ ! -d "$A/jdk" ]; then
    rm -rf "$A/jdk.tmp" && mkdir "$A/jdk.tmp"
    if [ $EXT = zip ]; then unzip -q "$A/dl/jdk21-$OS-$JARCH.zip" -d "$A/jdk.tmp"
    else tar -xzf "$A/dl/jdk21-$OS-$JARCH.tar.gz" -C "$A/jdk.tmp"; fi
    J=$(ls -d "$A"/jdk.tmp/*)
    [ -d "$J/Contents/Home" ] && J=$J/Contents/Home  # (macOS: a .jdk bundle)
    mv "$J" "$A/jdk" && rm -rf "$A/jdk.tmp"
fi
if [ ! -d "$A/sdk/cmdline-tools/latest" ]; then
    mkdir -p "$A/sdk/cmdline-tools" && unzip -q "$A/dl/cmdline-tools-$CLT.zip" -d "$A/sdk/cmdline-tools"
    mv "$A/sdk/cmdline-tools/cmdline-tools" "$A/sdk/cmdline-tools/latest"
fi
if [ ! -d "$A/gradle" ]; then
    rm -rf "$A/gradle.tmp" && unzip -q "$A/dl/gradle.zip" -d "$A/gradle.tmp" && mv "$A"/gradle.tmp/* "$A/gradle" && rmdir "$A/gradle.tmp"
fi
export JAVA_HOME="$A/jdk" PATH="$A/jdk/bin:$PATH"

# Packages: the command-line tools' own "android sdk" (slash-separated names; their sdkmanager forwards to it and
# splits the old semicolon names), else an older sdkmanager
EMU=; [ "${1:-}" = --emulator ] && EMU=1
IMAGE=android-35/google_apis/arm64-v8a
PKGS="platform-tools platforms/android-35 build-tools/36.1.0 ndk/30.0.16248370"
[ -n "$EMU" ] && PKGS="$PKGS emulator system-images/$IMAGE"
B=$A/sdk/cmdline-tools/latest/bin
if [ -e "$B/android" ] || [ -e "$B/android.exe" ]; then
    CLI=$B/android; [ -e "$CLI.exe" ] && CLI=$CLI.exe
    "$CLI" --sdk="$(W "$A/sdk")" sdk install $PKGS
else
    SDKM=$B/sdkmanager; [ -e "$SDKM.bat" ] && SDKM=$SDKM.bat
    yes | "$SDKM" --sdk_root="$(W "$A/sdk")" --licenses >/dev/null || true
    "$SDKM" --sdk_root="$(W "$A/sdk")" $(echo "$PKGS" | tr '/' ';')
fi

if [ -n "$EMU" ]; then
    AVDM=$B/avdmanager; [ -e "$AVDM.bat" ] && AVDM=$AVDM.bat
    export ANDROID_HOME="$A/sdk" ANDROID_SDK_ROOT="$A/sdk" ANDROID_AVD_HOME="$A/avd"
    mkdir -p "$A/avd"
    if [ ! -d "$A/avd/PrimeRecomp.avd" ]; then
        echo no | "$AVDM" create avd -n PrimeRecomp -k "system-images;$(echo $IMAGE | tr '/' ';')" -d pixel_8 --force
        # the game needs room: 8 GB of RAM (the guest heap alone is 3.25 GB), 32 GB of storage (7 GB of game files),
        # the host's GPU for Vulkan, a keyboard (the PC controls)
        C=$A/avd/PrimeRecomp.avd/config.ini
        grep -v '^hw.ramSize\|^disk.dataPartition.size\|^hw.gpu\.\|^hw.keyboard' "$C" > "$C.new" || true
        printf 'hw.ramSize=8192\ndisk.dataPartition.size=32G\nhw.gpu.enabled=yes\nhw.gpu.mode=host\nhw.keyboard=yes\n' >> "$C.new"
        mv "$C.new" "$C"
    fi
    echo "virtual device PrimeRecomp ready: tools/android/emulator.sh starts it"
fi
echo "Android SDK ready in build/android"
