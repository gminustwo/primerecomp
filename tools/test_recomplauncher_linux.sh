#!/bin/bash
# The existing release container, synthetic runtime tests, then the finished image's actual manifest.
set -euo pipefail
cd "$(dirname "$0")/.."
bash tools/linux/gen_helper.sh x86_64
bash tools/linux/package.sh x86_64
clang -Wall -Wextra -Iruntime tools/test_launcher_cli.c runtime/launcher_cli.c -o build/test_launcher_cli
export APPIMAGE_EXTRACT_AND_RUN=1
image=$(find build/dist -maxdepth 1 -name '*.AppImage' -print -quit)
[ -n "$image" ]
python3 tools/test_recomplauncher.py --exe "$image" --parser build/test_launcher_cli -v
# Extract into build/, never the checkout root. This verifies delivery: the image carries the repo's file as is.
cp recomplauncher.json build/recomplauncher.expected.json
cd build
image=$(find dist -maxdepth 1 -name '*.AppImage' -print -quit)
"$image" --appimage-extract recomplauncher.json >/dev/null
python3 - <<'PY'
import json
from pathlib import Path
p = Path("squashfs-root/recomplauncher.json")
assert p.is_file() and not p.is_symlink()
m = json.loads(p.read_text())
assert m == json.loads(Path("recomplauncher.expected.json").read_text())
assert m["format"] == 1 and m["repo"] == "gminustwo/primerecomp"
assert any(name.endswith(".AppImage") for name in m["start"]["program"])
PY
