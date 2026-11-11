#!/bin/sh
# Scripted before/after capture: loads a level, freezes the AI, waits for it to
# settle, times frames, saves a PNG and quits. The game window pops up while it
# runs (muted). Don't touch the mouse or the view will move.
#
# Usage: scripts/capture.sh NAME [setting=value ...]
#   NAME            output name, e.g. bloom-on (keep it short)
#   setting=value   engine cvars, e.g. r_enableBloom=1 r_enableSSAO=1
#                   r_ssaaMultiple=2 r_enableTextureFilter=1 r_fov=74,
#                   plus hidpi=0/1 and fullscreen=0/1
#
# Baseline for every run: HiDPI on, windowed, vsync off, no fps cap, all effects
# off. Vsync is off so frame times show real cost, not the display refresh.
#
# Env: CAPTURE_MAP (default 01narshadda.jkl), CAPTURE_DELAY (s, default 8),
#      CAPTURE_FRAMES (default 600), CAPTURE_WARP ("x y z pitch yaw roll", camera
#      position; each run prints the warp it ended at, so a good view can be reused)
# Output: logs/captures/NAME.png and NAME.log; the stats line is printed.
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NAME="$1"
if [ -z "$NAME" ]; then
    sed -n 2,18p "$0"
    exit 1
fi
shift

BIN="$ROOT/OpenJKDF2.app/Contents/MacOS/openjkdf2-64"
GAME="$ROOT/gamedata/jk1"
CAPROOT="$ROOT/gamedata/capture"
OUT_DIR="$ROOT/logs/captures"
mkdir -p "$OUT_DIR" "$CAPROOT/player"

# Captures run from their own data root so they never touch your real player
# profiles, saves or settings: links to the game data, plus its own player folder.
for item in JK.EXE Episode Resource MUSIC; do
    [ -e "$CAPROOT/$item" ] || ln -s "$GAME/$item" "$CAPROOT/$item"
done

BASELINE="hidpi=1 fullscreen=0 r_enableVsync=0 r_fpslimit=0 r_enableBloom=0 r_enableSSAO=0 r_ssaaMultiple=1 r_enableTextureFilter=0"

# Muted: OpenAL Soft (sound effects) and SDL audio (music, cutscenes) both get
# silent drivers. "script" gives the engine a tty so its log isn't buffered.
ALSOFT_DRIVERS=null \
SDL_AUDIO_DRIVER=dummy \
OPENJKDF2_ROOT="$CAPROOT" \
OPENJKDF2_CAPTURE="$OUT_DIR/$NAME.png" \
OPENJKDF2_CAPTURE_CVARS="$BASELINE $*" \
OPENJKDF2_CAPTURE_WARP="${CAPTURE_WARP:-}" \
OPENJKDF2_CAPTURE_DELAY="${CAPTURE_DELAY:-8}" \
OPENJKDF2_CAPTURE_FRAMES="${CAPTURE_FRAMES:-600}" \
    script -q /dev/null "$BIN" -autostart -sp -episode JK1 -map "${CAPTURE_MAP:-01narshadda.jkl}" \
    < /dev/null > "$OUT_DIR/$NAME.log" 2>&1 || true

tr -d '\r' < "$OUT_DIR/$NAME.log" | grep "ignored)" || true
if ! tr -d '\r' < "$OUT_DIR/$NAME.log" | grep "^CAPTURE: frames"; then
    echo "Capture failed, see $OUT_DIR/$NAME.log"
    exit 1
fi
