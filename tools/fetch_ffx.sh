#!/bin/sh
# AMD FSR 3 / FSR 4 (runtime/nvn/fsr.h): fetches AMD's FSR SDK 2.4.0 and takes its two signed Vulkan DLLs into
# build/ffx/, with AMD's license (Kits/FidelityFX/docs/license.md), for next to primerecomp.exe:
#   amd_fidelityfx_loader_vk.dll, amd_fidelityfx_upscaler_vk.dll
# `make dist` puts them in the Windows release. AMD's license allows shipping them in binary form, with its notice;
# they need the Microsoft Visual C++ runtime (x64), which nearly every Windows PC has.
# FFX_ZIP=<path to FidelityFX-SDK-2.4.0's source zip> skips the download (about 250 MB).
set -e
cd "$(dirname "$0")/.."
TAG=v2.4.0
URL=https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/archive/refs/tags/$TAG.zip
OUT=build/ffx
mkdir -p "$OUT"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
ZIP=${FFX_ZIP:-}
if [ -z "$ZIP" ]; then
    ZIP="$TMP/ffx.zip"
    echo "downloading AMD's FSR SDK $TAG (about 250 MB)"
    curl -fL --retry 3 -o "$ZIP" "$URL"
fi
K=FidelityFX-SDK-${TAG#v}/Kits/FidelityFX
unzip -q -o -j "$ZIP" "$K/signedbin/amd_fidelityfx_loader_vk.dll" -d "$OUT"
unzip -q -o -j "$ZIP" "$K/signedbin/amd_fidelityfx_upscaler_vk.dll" -d "$OUT"
unzip -q -o -j "$ZIP" "$K/docs/license.md" -d "$TMP"
cp "$TMP/license.md" "$OUT/AMD-FSR-LICENSE.md"
# (the files AMD signed for 2.4.0)
cd "$OUT"
sha256sum -c <<'EOF'
38a56a01fb3565c0757d9f5c1e4e69de4b65e5f2e5769b323b200dc1f142bab9  amd_fidelityfx_loader_vk.dll
4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7  amd_fidelityfx_upscaler_vk.dll
EOF
echo "build/ffx: copy amd_fidelityfx_loader_vk.dll and amd_fidelityfx_upscaler_vk.dll next to primerecomp.exe"
