#!/bin/bash
# tools/android/apk.sh: build/android/PrimeRecomp-<version>-android.apk - the runtime (make PLATFORM=android) with its
# libraries (tools/android/build_deps.sh) and SDL's Java activity, packaged by Gradle (android/). No game code inside:
# like the PC builds, the game code library is built from the player's own game files (make PLATFORM=android
# build/android/build/primegame.so, for now on a PC) and copied to the device with the game files.
# Uses the JDK / SDK / NDK / Gradle under build/android (nothing system-wide). Run from an MSYS2 shell on Windows.
set -euo pipefail
cd "$(dirname "$0")/../.."
A=$PWD/build/android
NDK=${NDK:-$(ls -d "$A"/sdk/ndk/* | sort -V | tail -1)}
case "$(uname -s)" in MINGW*|MSYS*) HOST=windows-x86_64 ;; Darwin) HOST=darwin-x86_64 ;; *) HOST=linux-x86_64 ;; esac
TC=$NDK/toolchains/llvm/prebuilt/$HOST/bin
VERSION=$(grep versionName android/app/build.gradle | cut -d"'" -f2)

[ -f "$A/deps/lib/libSDL3.so" ] || tools/android/build_deps.sh
make PLATFORM=android -j"$(getconf _NPROCESSORS_ONLN)" build/android/build/libmain.so

# native libraries (stripped: libmain.so carries 100+ MB of debug info - the unstripped one stays in build/android/build
# for ndk-stack / addr2line on a crash's offsets)
J=$A/jniLibs/arm64-v8a
rm -rf "$A/jniLibs" && mkdir -p "$J"
for l in "$A"/deps/lib/libSDL3.so "$A"/deps/lib/libavutil.so "$A"/deps/lib/libavcodec.so "$A"/deps/lib/libavformat.so \
         "$A"/deps/lib/libswscale.so "$A"/build/libmain.so \
         "$NDK/toolchains/llvm/prebuilt/$HOST/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so"; do
    "$TC/llvm-strip" --strip-unneeded -o "$J/$(basename "$l")" "$l"
done

# assets: copied into the data root at the first start (runtime/paths.c)
rm -rf "$A/assets" && mkdir -p "$A/assets"
cp third_party/fonts/Baloo2/Baloo2-SemiBold.ttf third_party/fonts/Baloo2/OFL.txt "$A/assets/"
cp docs/icon/icon_1024.png "$A/assets/icon.png"

# Gradle: the JDK and SDK under build/android
export JAVA_HOME="$A/jdk" ANDROID_HOME="$A/sdk"
W() { cygpath -m "$1" 2>/dev/null || echo "$1"; }
echo "sdk.dir=$(W "$A/sdk")" > android/local.properties
GRADLE="$A/gradle/bin/gradle"; [ -f "$GRADLE.bat" ] && GRADLE="$GRADLE.bat"
(cd android && "$GRADLE" --no-daemon -q assembleRelease)
cp android/app/build/outputs/apk/release/app-release.apk "$A/PrimeRecomp-$VERSION-android.apk"
echo "built $A/PrimeRecomp-$VERSION-android.apk ($(du -h "$A/PrimeRecomp-$VERSION-android.apk" | cut -f1))"
