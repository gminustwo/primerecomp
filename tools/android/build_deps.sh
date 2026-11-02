#!/bin/bash
# Android (arm64-v8a) libraries the runtime links against, into build/android/deps: SDL3 (ndk-build, its own
# Android.mk), shaderc (CMake, libshaderc_shared as on Linux), an LGPL FFmpeg with just what the game's movies need
# (the configuration of tools/build_deps_windows.sh), and the Vulkan / VMA headers. Run from an MSYS2 shell on
# Windows; the SDK / NDK are Android Studio's or setup_sdk.sh's (tools/android/env.sh). Skips what's already built.
set -euo pipefail
cd "$(dirname "$0")/../.."
. tools/android/env.sh  # (A, NDK, HOST, TC: Android Studio's SDK, or setup_sdk.sh's)
API=${API:-29}
P=$A/deps
S=$A/src
mkdir -p "$P/lib" "$P/include" "$S"
JOBS=$(getconf _NPROCESSORS_ONLN)
if [ $HOST = windows-x86_64 ]; then
    NDKBUILD() { "$(cygpath -u "${SYSTEMROOT:-C:\Windows}")/System32/cmd.exe" //c "$(cygpath -w "$NDK/ndk-build.cmd")" "$@" || exit 1; }
else
    NDKBUILD() { "$NDK/ndk-build" "$@"; }
fi

fetch() {  # url sha256 file
    [ -f "$S/$3" ] || curl -fsSL -o "$S/$3.part" "$1" && { [ -f "$S/$3" ] || mv "$S/$3.part" "$S/$3"; }
    echo "$2  $S/$3" | { command -v shasum >/dev/null && shasum -a 256 -c --quiet || sha256sum -c --quiet; }  # (macOS'
    # own sha256sum has no --quiet)
}

# ---- headers: Vulkan (newer than the NDK's; -Ibuild/android/deps/include comes first) and Vulkan Memory Allocator,
# the versions of the Linux / macOS builds
fetch https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.4.350.1.tar.gz \
      6d1bb65e49520344cc0a48af3dc02e993781efff14c7ebdcb8ae9fa23ddf7e83 Vulkan-Headers-1.4.350.1.tar.gz
fetch https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/archive/refs/tags/v3.3.0.tar.gz \
      c4f6bbe6b5a45c2eb610ca9d231158e313086d5b1a40c9922cb42b597419b14e VulkanMemoryAllocator-3.3.0.tar.gz
if [ ! -f "$P/include/vulkan/vulkan.h" ]; then
    rm -rf "$S/vulkan-headers" && mkdir -p "$S/vulkan-headers"
    tar -xzf "$S/Vulkan-Headers-1.4.350.1.tar.gz" -C "$S/vulkan-headers" --strip-components 1
    cp -R "$S/vulkan-headers/include/vulkan" "$S/vulkan-headers/include/vk_video" "$P/include/"
fi
if [ ! -f "$P/include/vma/vk_mem_alloc.h" ]; then
    rm -rf "$S/vma" && mkdir -p "$S/vma" && tar -xzf "$S/VulkanMemoryAllocator-3.3.0.tar.gz" -C "$S/vma" --strip-components 1
    mkdir -p "$P/include/vma" && cp "$S/vma/include/vk_mem_alloc.h" "$P/include/vma/"
fi

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

# ---- shaderc 2026.3 (the Linux build's): libshaderc_shared.so, glslang and SPIRV-Tools inside. Not the NDK's own copy
# (sources/third_party/shaderc): its glslang is years old and refuses extensions the shader translator uses
# (GL_EXT_texture_shadow_lod: every shader failed, nothing drawn). CMake + Ninja: Android Studio's (SDK Manager > SDK
# Tools > CMake), else the system's; its DEPS file pins the glslang / SPIRV-Tools / SPIRV-Headers commits
# (utils/git-sync-deps clones them: git, python3).
fetch https://github.com/google/shaderc/archive/refs/tags/v2026.3.tar.gz \
      ee493ccf1b3038b4ef2fe024664c5eb2dc4bcc1f6b05b33e3909de0e19c81024 shaderc-2026.3.tar.gz
if [ ! -f "$P/lib/libshaderc_shared.so" ]; then
    CMAKE=${CMAKE:-$( (ls -d "$SDK"/cmake/*/bin/cmake "$SDK"/cmake/*/bin/cmake.exe 2>/dev/null || true) | sort -V | tail -1)}
    CMAKE=${CMAKE:-cmake}
    NINJA=$( (ls -d "$(dirname "$CMAKE")"/ninja "$(dirname "$CMAKE")"/ninja.exe 2>/dev/null || true) | head -1)
    NINJA=${NINJA:-ninja}
    W() { cygpath -m "$1" 2>/dev/null || echo "$1"; }  # (cmake is a native program: Windows paths there)
    rm -rf "$S/shaderc" && mkdir -p "$S/shaderc" && tar -xzf "$S/shaderc-2026.3.tar.gz" -C "$S/shaderc" --strip-components 1
    (cd "$S/shaderc" && "${PYTHON:-python3}" utils/git-sync-deps)
    rm -rf "$A/obj/shaderc"
    "$CMAKE" -S "$(W "$S/shaderc")" -B "$(W "$A/obj/shaderc")" -G Ninja -DCMAKE_MAKE_PROGRAM="$(W "$NINJA")" \
        -DCMAKE_TOOLCHAIN_FILE="$(W "$NDK/build/cmake/android.toolchain.cmake")" -DANDROID_ABI=arm64-v8a \
        -DANDROID_PLATFORM=android-$API -DANDROID_STL=c++_shared -DCMAKE_BUILD_TYPE=Release \
        -DSHADERC_SKIP_TESTS=ON -DSHADERC_SKIP_EXAMPLES=ON -DSHADERC_SKIP_COPYRIGHT_CHECK=ON -DSPIRV_SKIP_TESTS=ON \
        -DSPIRV_SKIP_EXECUTABLES=ON -DENABLE_GLSLANG_BINARIES=OFF -DSPIRV_TOOLS_BUILD_STATIC=ON -DBUILD_SHARED_LIBS=OFF
    "$CMAKE" --build "$(W "$A/obj/shaderc")" --target shaderc_shared -j"$JOBS"
    rm -rf "$P/include/shaderc" && mkdir -p "$P/include/shaderc"
    cp "$S/shaderc/libshaderc/include/shaderc/"*.h "$P/include/shaderc/"
    cp "$A/obj/shaderc/libshaderc/libshaderc_shared.so" "$P/lib/"  # (unstripped: apk.sh strips its copy)
fi
rm -f "$P/lib/libshaderc_combined.a"  # (the NDK's old shaderc, from earlier builds: not linked any more)

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
