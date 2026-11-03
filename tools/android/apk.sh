#!/bin/bash
# tools/android/apk.sh: build/android/PrimeRecomp-<version>-android.apk - the runtime (make PLATFORM=android) with its
# libraries (tools/android/build_deps.sh) and SDL's Java activity, packaged by Gradle (android/, which Android Studio
# opens too). No game code inside: like the PC builds, the game code library is built from the player's own game files
# (make PLATFORM=android build/android/build/primegame.so, for now on a PC) and copied to the device with the game files.
# Uses Android Studio's SDK / NDK / JDK, or setup_sdk.sh's under build/android (tools/android/env.sh). Run from an
# MSYS2 shell on Windows.
set -euo pipefail
cd "$(dirname "$0")/../.."
. tools/android/env.sh
VERSION=$(sed -n 's/^#define LARECOMP_VERSION "\(.*\)"/\1/p' runtime/ui/launcher.mm)  # (as android/app/build.gradle)

tools/android/build_deps.sh  # (quick when built: it skips each part whose output exists)
make PLATFORM=android NDK="$NDK" -j"${JOBS:-$(getconf _NPROCESSORS_ONLN)}" build/android/build/libmain.so  # (JOBS=<n>)

# native libraries (stripped: libmain.so carries 100+ MB of debug info - the unstripped one stays in build/android/build
# for ndk-stack / addr2line on a crash's offsets)
J=$A/jniLibs/arm64-v8a
rm -rf "$A/jniLibs" && mkdir -p "$J"
for l in "$A"/deps/lib/libSDL3.so "$A"/deps/lib/libavutil.so "$A"/deps/lib/libavcodec.so "$A"/deps/lib/libavformat.so \
         "$A"/deps/lib/libswscale.so "$A"/deps/lib/libshaderc_shared.so "$A"/build/libmain.so \
         "$NDK/toolchains/llvm/prebuilt/$HOST/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so"; do
    "$TC/llvm-strip" --strip-unneeded -o "$J/$(basename "$l")" "$l"
done

# assets: copied into the data root at the first start (runtime/paths.c)
rm -rf "$A/assets" && mkdir -p "$A/assets"
cp third_party/fonts/Baloo2/Baloo2-SemiBold.ttf third_party/fonts/Baloo2/OFL.txt "$A/assets/"
cp docs/icon/icon_1024.png "$A/assets/icon.png"

# Gradle (its wrapper, android/gradlew: what Android Studio's Build > Build APK runs too)
[ -n "$JDK" ] && export JAVA_HOME="$JDK"
W() { cygpath -m "$1" 2>/dev/null || echo "$1"; }
echo "sdk.dir=$(W "$SDK")" > android/local.properties
GRADLE=./gradlew; [ $HOST = windows-x86_64 ] && GRADLE=./gradlew.bat
(cd android && "$GRADLE" --no-daemon -q assembleRelease)
cp android/app/build/outputs/apk/release/app-release.apk "$A/PrimeRecomp-$VERSION-android.apk"
echo "built $A/PrimeRecomp-$VERSION-android.apk ($(du -h "$A/PrimeRecomp-$VERSION-android.apk" | cut -f1))"
