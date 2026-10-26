#!/bin/bash
# make deps (macOS): the libraries and tools the shipped .app contains, built from pinned sources for the app's
# minimum macOS (MACOS_MIN, default 15.0) into build/deps. Homebrew's bottles target the macOS they were built on, so
# they can't ship. Plain `make` (dev builds) keeps using Homebrew.
#   SDL3; FFmpeg (LGPL only, no GPL parts: just what the game's movies need - WebM: VP9 video + Vorbis audio, plus
#   VP8 / Opus / H.264 / AAC in WebM or MP4 - and swscale); the Vulkan renderer's: the Vulkan headers, Vulkan Memory
#   Allocator (header), the Vulkan loader, MoltenVK (Vulkan on Metal: Khronos' release build, for macOS 12+) and shaderc
#   (GLSL -> SPIR-V, one shared library as on Windows / Linux); a relocatable CPython 3.12 (python-build-standalone,
#   built for macOS 11+) that `make gen-helper` freezes.
# Needs: Xcode command line tools, cmake, ninja, git (shaderc's sources). Each step is skipped when its output exists
# (delete build/deps/<x> to redo).
set -euo pipefail
cd "$(dirname "$0")/.."
MIN=${MACOS_MIN:-15.0}
P=$PWD/build/deps
S=$P/src
mkdir -p "$S" "$P/bin" "$P/lib"
export MACOSX_DEPLOYMENT_TARGET=$MIN
JOBS=$(sysctl -n hw.ncpu)

fetch() {  # url sha256 file
    [ -f "$S/$3" ] || curl -fsSL -o "$S/$3.part" "$1" && { [ -f "$S/$3" ] || mv "$S/$3.part" "$S/$3"; }
    echo "$2  $S/$3" | shasum -a 256 -c --quiet
}
unpack() { rm -rf "$S/$2" && mkdir -p "$S/$2" && tar -xf "$S/$1" -C "$S/$2" --strip-components 1; }

fetch https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz \
      9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3 SDL3-3.4.18.tar.gz
fetch https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz \
      8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e ffmpeg-9.0.2.tar.xz
fetch https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.4.350.1.tar.gz \
      6d1bb65e49520344cc0a48af3dc02e993781efff14c7ebdcb8ae9fa23ddf7e83 Vulkan-Headers-1.4.350.1.tar.gz
fetch https://github.com/KhronosGroup/Vulkan-Loader/archive/refs/tags/vulkan-sdk-1.4.350.1.tar.gz \
      602984a71000981e25e4feb419e6cdd70b18ffe2b8004f60f591706027bca468 Vulkan-Loader-1.4.350.1.tar.gz
fetch https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/archive/refs/tags/v3.3.0.tar.gz \
      c4f6bbe6b5a45c2eb610ca9d231158e313086d5b1a40c9922cb42b597419b14e VulkanMemoryAllocator-3.3.0.tar.gz
fetch https://github.com/KhronosGroup/MoltenVK/releases/download/v1.4.2/MoltenVK-macos.tar \
      f95765a6229cb7b915990a2890ce12ebe36a730b021545d3d52ae69ce4c4024e MoltenVK-1.4.2-macos.tar
fetch https://github.com/google/shaderc/archive/refs/tags/v2026.4.tar.gz \
      f06ce5bcca94e5df7f34e115743597d0ad2e13c5fe9213c67dc8c76031241947 shaderc-2026.4.tar.gz
fetch "https://github.com/astral-sh/python-build-standalone/releases/download/20260924/cpython-3.12.14%2B20260924-aarch64-apple-darwin-install_only.tar.gz" \
      9763f43db2481a6af36af82ec40302aab7a73632f880129d07a6e81aec846277 cpython-3.12.14-20260924.tar.gz

if [ ! -f "$P/lib/libSDL3.0.dylib" ]; then
    unpack SDL3-3.4.18.tar.gz sdl
    cmake -S "$S/sdl" -B "$S/sdl/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$P" \
          -DCMAKE_OSX_DEPLOYMENT_TARGET="$MIN" -DCMAKE_OSX_ARCHITECTURES=arm64 -DSDL_SHARED=ON -DSDL_STATIC=OFF \
          -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_INSTALL_DOCS=OFF
    cmake --build "$S/sdl/build" -j"$JOBS" && cmake --install "$S/sdl/build"
fi

if [ ! -f "$P/lib/libavcodec.dylib" ]; then
    unpack ffmpeg-9.0.2.tar.xz ffmpeg
    (cd "$S/ffmpeg" && ./configure --prefix="$P" --cc=clang --arch=arm64 --enable-shared --disable-static \
        --extra-cflags="-mmacosx-version-min=$MIN" --extra-ldflags="-mmacosx-version-min=$MIN" \
        --disable-autodetect --disable-programs --disable-doc --disable-network --disable-avdevice --disable-avfilter \
        --disable-swresample --disable-everything \
        --enable-protocol=file --enable-demuxer=matroska,mov \
        --enable-decoder=vp9,vp8,vorbis,opus,h264,aac --enable-parser=vp9,vp8,vorbis,opus,h264,aac \
        --install-name-dir='@rpath' && make -j"$JOBS" && make install)
fi

if [ ! -f "$P/include/vulkan/vulkan.h" ]; then
    unpack Vulkan-Headers-1.4.350.1.tar.gz vulkan-headers
    mkdir -p "$P/include" && cp -R "$S/vulkan-headers/include/vulkan" "$S/vulkan-headers/include/vk_video" "$P/include/"
fi
if [ ! -f "$P/include/vma/vk_mem_alloc.h" ]; then
    unpack VulkanMemoryAllocator-3.3.0.tar.gz vma
    mkdir -p "$P/include/vma" && cp "$S/vma/include/vk_mem_alloc.h" "$P/include/vma/"
fi

# the loader finds MoltenVK through its ICD file (the .app: Contents/Resources/vulkan/icd.d, tools/bundle_macos.sh)
if [ ! -f "$P/lib/libvulkan.1.dylib" ]; then
    unpack Vulkan-Loader-1.4.350.1.tar.gz vulkan-loader
    cmake -S "$S/vulkan-loader" -B "$S/vulkan-loader/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$P" \
          -DCMAKE_OSX_DEPLOYMENT_TARGET="$MIN" -DCMAKE_OSX_ARCHITECTURES=arm64 -DVULKAN_HEADERS_INSTALL_DIR="$P" \
          -DUPDATE_DEPS=OFF -DBUILD_TESTS=OFF -DCMAKE_INSTALL_NAME_DIR='@rpath'
    cmake --build "$S/vulkan-loader/build" -j"$JOBS" && cmake --install "$S/vulkan-loader/build"
fi
if [ ! -f "$P/lib/libMoltenVK.dylib" ]; then
    unpack MoltenVK-1.4.2-macos.tar moltenvk
    lipo "$S/moltenvk/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib" -thin arm64 -output "$P/lib/libMoltenVK.dylib"
    mkdir -p "$P/share/vulkan/icd.d"
    cp "$S/moltenvk/MoltenVK/dynamic/dylib/macOS/MoltenVK_icd.json" "$P/share/vulkan/icd.d/"
    sed -i '' 's|"./libMoltenVK.dylib"|"../../../lib/libMoltenVK.dylib"|' "$P/share/vulkan/icd.d/MoltenVK_icd.json"
    cp "$S/moltenvk/LICENSE" "$P/share/vulkan/MoltenVK-LICENSE"
fi

# shaderc as one shared library (glslang, SPIRV-Tools inside); its DEPS file pins the glslang / SPIRV-Tools /
# SPIRV-Headers commits (utils/git-sync-deps clones them)
if [ ! -f "$P/lib/libshaderc_shared.dylib" ]; then
    unpack shaderc-2026.4.tar.gz shaderc
    (cd "$S/shaderc" && python3 utils/git-sync-deps)
    cmake -S "$S/shaderc" -B "$S/shaderc/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$P" \
          -DCMAKE_OSX_DEPLOYMENT_TARGET="$MIN" -DCMAKE_OSX_ARCHITECTURES=arm64 -DSHADERC_SKIP_TESTS=ON \
          -DSHADERC_SKIP_EXAMPLES=ON -DSHADERC_SKIP_COPYRIGHT_CHECK=ON -DSPIRV_SKIP_TESTS=ON -DSPIRV_SKIP_EXECUTABLES=ON \
          -DENABLE_GLSLANG_BINARIES=OFF -DSPIRV_TOOLS_BUILD_STATIC=ON -DBUILD_SHARED_LIBS=OFF -DCMAKE_INSTALL_NAME_DIR='@rpath' \
          -DCMAKE_BUILD_WITH_INSTALL_NAME_DIR=ON
    cmake --build "$S/shaderc/build" --target shaderc_shared -j"$JOBS"
    mkdir -p "$P/include/shaderc"
    cp "$S/shaderc/libshaderc/include/shaderc/"*.h "$P/include/shaderc/"
    cp -P "$S/shaderc/build/libshaderc/libshaderc_shared"*.dylib "$P/lib/"
fi

if [ ! -x "$P/python/bin/python3.12" ]; then
    rm -rf "$P/python" && mkdir -p "$P/python"
    tar -xf "$S/cpython-3.12.14-20260924.tar.gz" -C "$P/python" --strip-components 1
fi

echo "build/deps ready (macOS $MIN):"
for f in "$P"/lib/*.dylib; do
    [ -L "$f" ] && continue
    printf '  %-28s minos %s\n' "$(basename "$f")" "$(otool -l "$f" | awk '/LC_BUILD_VERSION/ {b=1} b && /minos/ {print $2; exit}')"
done
