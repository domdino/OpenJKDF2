#!/bin/sh
# Scripted before/after capture: loads a level, freezes the AI, waits for it to
# settle, times frames, saves a PNG and quits. Fully silent: no visible window,
# no Dock icon, no focus stealing (renders offscreen) and no sound.
#
# Usage: scripts/capture.sh NAME [setting=value ...]
#   NAME            output name, e.g. bloom-on (keep it short)
#   setting=value   engine cvars, e.g. r_enableBloom=1 r_enableSSAO=1
#                   r_ssaaMultiple=2 r_enableTextureFilter=1 r_fov=74,
#                   r_enableGrade=1 r_gradeLut=grade/identity.cube r_gradeStrength=0.5,
#                   plus hidpi=0/1 and fullscreen=0/1
#
# Baseline for every run: HiDPI on, windowed, vsync off, no fps cap, all effects
# off. Vsync is off so frame times show real cost, not the display refresh.
#
# Env: CAPTURE_MAP (default 01narshadda.jkl), CAPTURE_DELAY (s, default 8),
#      CAPTURE_FRAMES (default 600), CAPTURE_HD (1 = use hd/jkgm textures, 0 = originals), CAPTURE_WARP ("x y z pitch yaw roll", camera
#      position; each run prints the warp it ended at, so a good view can be reused),
#      CAPTURE_VIDEO (e.g. 'video\\01-02a.smk': shoot that cutscene CAPTURE_DELAY s in),
#      CAPTURE_HDVIDEO (folder of HD cutscene replacements, default hd/video),
#      CAPTURE_CAMERA (1 = third-person camera, to see Kyle),
#      CAPTURE_KEY (e.g. kyrun0.key: play that animation on the player as timing starts)
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
[ -e "$CAPROOT/grade" ] || ln -s "$ROOT/grade" "$CAPROOT/grade"
# HD cutscenes: CAPTURE_HDVIDEO picks the folder (default hd/video).
rm -f "$CAPROOT/hdvideo"
HDVIDEO="${CAPTURE_HDVIDEO:-$ROOT/hd/video}"
[ -d "$HDVIDEO" ] && ln -s "$HDVIDEO" "$CAPROOT/hdvideo"
# HD textures: on unless CAPTURE_HD=0 (for before/after comparisons).
rm -f "$CAPROOT/jkgm"
if [ "${CAPTURE_HD:-1}" = "1" ] && [ -d "$ROOT/hd/jkgm" ]; then
    ln -s "$ROOT/hd/jkgm" "$CAPROOT/jkgm"
fi

# Default camera for level 1: the spawn room, facing the green sign (same room as
# the north star frame). Other maps start wherever the level puts the player.
WARP="${CAPTURE_WARP:-}"
if [ -z "$WARP" ] && [ "${CAPTURE_MAP:-01narshadda.jkl}" = "01narshadda.jkl" ]; then
    WARP="-10.4434 -5.4551 -1.1299 0 -59.98 0"
fi

BASELINE="hidpi=1 fullscreen=0 r_enableVsync=0 r_fpslimit=0 r_enableBloom=0 r_enableSSAO=0 r_ssaaMultiple=1 r_enableTextureFilter=0 r_enableGrade=0 r_perPixelLighting=0 r_glowLights=0 r_reflections=0 r_wetness=0.6 r_specular=1 r_reflectionStrength=1 r_haze=0 r_hazeDensity=0.08 r_hazeStrength=1 r_lightShafts=0 r_shaftStrength=1 r_anisotropy=8 r_reflectionRoughness=0.5"

# Muted: OpenAL Soft (sound effects) and SDL audio (music, cutscenes) both get
# silent drivers. SDL_MAC_BACKGROUND_APP keeps it out of the Dock and from taking
# focus; the engine hides its window in capture mode. "script" gives the engine a
# tty so its log isn't buffered.
ALSOFT_DRIVERS=null \
SDL_AUDIO_DRIVER=dummy \
SDL_MAC_BACKGROUND_APP=1 \
OPENJKDF2_ROOT="$CAPROOT" \
OPENJKDF2_CAPTURE="$OUT_DIR/$NAME.png" \
OPENJKDF2_CAPTURE_CVARS="$BASELINE $*" \
OPENJKDF2_CAPTURE_WARP="$WARP" \
OPENJKDF2_CAPTURE_DELAY="${CAPTURE_DELAY:-8}" \
OPENJKDF2_CAPTURE_FRAMES="${CAPTURE_FRAMES:-600}" \
OPENJKDF2_CAPTURE_VIDEO="${CAPTURE_VIDEO:-}" \
OPENJKDF2_CAPTURE_CAMERA="${CAPTURE_CAMERA:-0}" \
OPENJKDF2_CAPTURE_KEY="${CAPTURE_KEY:-}" \
    script -q /dev/null "$BIN" -autostart -sp -episode JK1 -map "${CAPTURE_MAP:-01narshadda.jkl}" \
    < /dev/null > "$OUT_DIR/$NAME.log" 2>&1 || true

tr -d '\r' < "$OUT_DIR/$NAME.log" | grep "ignored)" || true
if ! tr -d '\r' < "$OUT_DIR/$NAME.log" | grep "^CAPTURE: frames"; then
    echo "Capture failed, see $OUT_DIR/$NAME.log"
    exit 1
fi
