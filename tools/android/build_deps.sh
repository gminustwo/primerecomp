#!/bin/bash
# Android (arm64-v8a) libraries the runtime links against, into build/android/deps: SDL3 (ndk-build, its own
# Android.mk), shaderc (the NDK's copy, libshaderc_combined) and an LGPL FFmpeg with just what the game's movies need
# (the configuration of tools/build_deps_windows.sh). Run from an MSYS2 shell on Windows; the SDK / NDK are the ones
# tools/android/... installs under build/android (nothing system-wide). Skips what's already built.
set -euo pipefail
cd "$(dirname "$0")/../.."
A=$PWD/build/android
NDK=${NDK:-$(ls -d "$A"/sdk/ndk/* | sort -V | tail -1)}
API=${API:-29}
P=$A/deps
S=$A/src
mkdir -p "$P/lib" "$P/include" "$S"
JOBS=$(getconf _NPROCESSORS_ONLN)
case "$(uname -s)" in MINGW*|MSYS*) HOST=windows-x86_64; NDKBUILD() { "$(cygpath -u "${SYSTEMROOT:-C:\Windows}")/System32/cmd.exe" //c "$(cygpath -w "$NDK/ndk-build.cmd")" "$@" || exit 1; } ;;
    Darwin) HOST=darwin-x86_64; NDKBUILD() { "$NDK/ndk-build" "$@"; } ;; *) HOST=linux-x86_64; NDKBUILD() { "$NDK/ndk-build" "$@"; } ;; esac
TC=$NDK/toolchains/llvm/prebuilt/$HOST/bin

fetch() {  # url sha256 file
    [ -f "$S/$3" ] || curl -fsSL -o "$S/$3.part" "$1" && { [ -f "$S/$3" ] || mv "$S/$3.part" "$S/$3"; }
    echo "$2  $S/$3" | { command -v sha256sum >/dev/null && sha256sum -c --quiet || shasum -a 256 -c --quiet; }
}

# ---- SDL3 (3.4.18, as the Windows build's MSYS2 package): libSDL3.so, and its Java glue for the APK
SDL=SDL3-3.4.18
fetch https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/$SDL.tar.gz \
      9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3 $SDL.tar.gz
[ -d "$S/$SDL" ] || tar -xzf "$S/$SDL.tar.gz" -C "$S"
if [ ! -f "$P/lib/libSDL3.so" ]; then
    NDKBUILD NDK_PROJECT_PATH=null APP_BUILD_SCRIPT="$(cygpath -w "$S/$SDL/Android.mk" 2>/dev/null || echo "$S/$SDL/Android.mk")" \
        APP_PLATFORM=android-$API APP_ABI=arm64-v8a APP_STL=c++_shared APP_OPTIM=release \
        NDK_OUT="$(cygpath -w "$A/obj/sdl" 2>/dev/null || echo "$A/obj/sdl")" \
        NDK_LIBS_OUT="$(cygpath -w "$A/obj/sdl-libs" 2>/dev/null || echo "$A/obj/sdl-libs")" -j"$JOBS" SDL3
    cp "$A/obj/sdl/local/arm64-v8a/libSDL3.so" "$P/lib/"  # (unstripped: the APK packaging strips it)
    rm -rf "$P/include/SDL3" && cp -r "$S/$SDL/include/SDL3" "$P/include/"
fi

# ---- shaderc (the NDK's sources): libshaderc_combined.a (glslang + SPIRV-Tools inside)
if [ ! -f "$P/lib/libshaderc_combined.a" ]; then
    SC=$NDK/sources/third_party/shaderc
    O=$A/obj/shaderc/obj/local/arm64-v8a
    # (its own libshaderc_combined target merges with an MRI script echoed through the shell, which cmd.exe's quoting
    # breaks on Windows: the parts are built, then merged here)
    NDKBUILD NDK_PROJECT_PATH="$(cygpath -w "$A/obj/shaderc" 2>/dev/null || echo "$A/obj/shaderc")" \
        APP_BUILD_SCRIPT="$(cygpath -w "$SC/Android.mk" 2>/dev/null || echo "$SC/Android.mk")" \
        APP_PLATFORM=android-$API APP_ABI=arm64-v8a APP_STL=c++_shared APP_OPTIM=release -j"$JOBS" shaderc
    W() { cygpath -m "$1" 2>/dev/null || echo "$1"; }  # (llvm-ar is a native program: Windows paths there)
    { echo "create $(W "$P/lib/libshaderc_combined.a")"
      for l in glslang OGLCompiler OSDependent shaderc shaderc_util SPIRV HLSL SPIRV-Tools SPIRV-Tools-opt; do
          [ -f "$O/lib$l.a" ] && echo "addlib $(W "$O/lib$l.a")"; done
      echo save; echo end; } > "$A/obj/shaderc/combine.mri"
    "$TC/llvm-ar" -M < "$A/obj/shaderc/combine.mri"
    rm -rf "$P/include/shaderc" && cp -r "$SC/libshaderc/include/shaderc" "$P/include/"
fi

# ---- FFmpeg (9.0.2, LGPL, the Windows build's configuration)
fetch https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz \
      8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e ffmpeg-9.0.2.tar.xz
if [ ! -f "$P/lib/libavcodec.so" ]; then
    rm -rf "$S/ffmpeg" && mkdir -p "$S/ffmpeg" && tar -xf "$S/ffmpeg-9.0.2.tar.xz" -C "$S/ffmpeg" --strip-components 1
    mkdir -p "$A/tmp"  # (the NDK clang is a native program: configure's test files at a path it can open too)
    (cd "$S/ffmpeg" && TMPDIR="$(cygpath -m "$A/tmp" 2>/dev/null || echo "$A/tmp")" ./configure --prefix="$P" --target-os=android --arch=aarch64 --enable-cross-compile \
        --cc="$TC/clang" --cxx="$TC/clang++" --ld="$TC/clang" --nm="$TC/llvm-nm" --ar="$TC/llvm-ar" \
        --ranlib="$TC/llvm-ranlib" --strip="$TC/llvm-strip" \
        --host-cc="${HOSTCC:-clang}" --host-ld="${HOSTCC:-clang}" \
        --extra-cflags="--target=aarch64-linux-android$API" --extra-ldflags="--target=aarch64-linux-android$API" \
        --enable-shared --disable-static --enable-pic \
        --disable-autodetect --disable-programs --disable-doc --disable-network --disable-avdevice --disable-avfilter \
        --disable-swresample --disable-everything \
        --enable-protocol=file --enable-demuxer=matroska,mov \
        --enable-decoder=vp9,vp8,vorbis,opus,h264,aac --enable-parser=vp9,vp8,vorbis,opus,h264,aac \
        && make -j"$JOBS" && make install)
fi
grep -q 'GPL version 3\|--enable-gpl' "$P/lib/pkgconfig/libavcodec.pc" && { echo "android deps: FFmpeg is not LGPL"; exit 1; }
echo "android deps: $(ls "$P/lib" | tr '\n' ' ')"
