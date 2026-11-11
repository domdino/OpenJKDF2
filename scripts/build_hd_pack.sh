#!/bin/sh
# Rebuild the HD texture pack from the Jedi Knight Remastered 3.3 installer, without
# running it (it's a Windows installer). Everything lands in hd/, which is gitignored.
#
# Usage: scripts/build_hd_pack.sh INSTALLER.exe [INSTALLER2.exe ...]
#   (Remastered 3.3 and 2.0.1 together give the most textures; 1.0 adds nothing over 2.0.1)
#
# Steps: 7-Zip splits the installer into its raw chunks; bzip2 and zlib chunks are
# decompressed; tools/mat_extract.py exports the game's original textures; then
# tools/jkgm_rebuild.py matches the (unnamed) HD PNGs to the pack metadata by comparing
# them with those originals, keeping only confident matches.
# Needs: brew install sevenzip; python3 with Pillow and NumPy.
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
EXE="$1"
if [ -z "$EXE" ] || [ ! -f "$EXE" ]; then
    sed -n 2,13p "$0"
    exit 1
fi
command -v 7zz >/dev/null || { echo "7zz not found: brew install sevenzip"; exit 1; }

RAW="$ROOT/hd/_installer_raw"
rm -rf "$RAW"
mkdir -p "$RAW/z"

echo "Splitting installer..."
7zz x -t# -y -o"$RAW" "$EXE" > /dev/null
for f in "$RAW"/*.bz2; do bunzip2 -q "$f" 2>/dev/null || true; done

echo "Decompressing zlib chunks..."
python3 -I - "$RAW" <<'EOF'
import os, sys, zlib
d = sys.argv[1]
for name in os.listdir(d):
    p = os.path.join(d, name)
    if not os.path.isfile(p):
        continue
    with open(p, "rb") as f:
        data = f.read()
    if data[:3] == b"\x01\x78\xda":
        try:
            open(os.path.join(d, "z", name), "wb").write(zlib.decompress(data[1:]))
        except zlib.error:
            pass
EOF

echo "Extracting original textures..."
python3 -I "$ROOT/tools/mat_extract.py"

echo "Matching HD textures..."
rm -rf "$ROOT/hd/jkgm/materials"
python3 -I "$ROOT/tools/jkgm_rebuild.py" "$RAW" "$RAW/z" \
    --extracted "$ROOT/hd/extracted" --out "$ROOT/hd/jkgm/materials" --name-match

echo "Making matches palette-correct (engine hashes per level palette)..."
python3 -I "$ROOT/tools/jkgm_signatures.py" "$ROOT/hd/jkgm/materials"

rm -rf "$RAW"
echo "Done: hd/jkgm/materials (scripts/run.sh links it into the game data)."
