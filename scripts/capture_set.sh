#!/bin/sh
# Capture the standard set of later-game views with the same settings and build a
# contact sheet. Silent like capture.sh (no window, no sound, no mouse grab).
#
# Usage: scripts/capture_set.sh NAME [setting=value ...]
#   e.g. scripts/capture_set.sh ppl-on r_perPixelLighting=1
# Output: logs/captures/NAME-<map>.png per view, and logs/captures/NAME-set.png
#
# The set (start of each level, so every run frames the same view):
#   12escape   dark hangar, shuttle, glowing red floor strips (glow highlights)
#   17asarris  outdoors under a stormy sky (sky light, haze)
#   20aboc     Boc with his saber lit (a live dynamic light)
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NAME="$1"
if [ -z "$NAME" ]; then
    sed -n 2,13p "$0"
    exit 1
fi
shift

MAPS="12escape 17asarris 20aboc"
OUT="$ROOT/logs/captures"
SHOTS=""
for map in $MAPS; do
    # Unquoted "$@" passes each setting as its own argument.
    CAPTURE_MAP="$map.jkl" CAPTURE_DELAY="${CAPTURE_DELAY:-6}" CAPTURE_FRAMES="${CAPTURE_FRAMES:-300}" \
        "$ROOT/scripts/capture.sh" "$NAME-$map" "$@" | sed -E 's/^CAPTURE: //; s/ warp=.*//; s/^/'"$map"': /'
    SHOTS="$SHOTS $NAME-$map.png"
done

# Relative names from inside the folder: the repo path contains spaces.
cd "$OUT"
# shellcheck disable=SC2086
magick $SHOTS +append -resize 50% "$NAME-set.png"
echo "Sheet: $OUT/$NAME-set.png"
