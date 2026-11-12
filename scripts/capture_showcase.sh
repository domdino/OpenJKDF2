#!/bin/sh
# Capture the standard showcase views, for 1997-vs-remaster comparisons (docs/screenshots).
#   scripts/capture_showcase.sh orig     the real 1997 game (CAPTURE_ORIGINAL=1)
#   scripts/capture_showcase.sh remaster the full remaster preset with HD textures
#   scripts/capture_showcase.sh remaster VIEW ...   just these views
# Output: logs/captures/show-<mode>-<view>.png
cd "$(dirname "$0")/.."
MODE="$1"; shift
VIEWS="${*:-nar1p nar3p hangar sarris boc yun cargo tower fuel barons saber}"
for v in $VIEWS; do
    MAP=""; CAM=0; WARP=""; WEAPON=""
    case $v in
        nar1p)  MAP=01narshadda.jkl ;;
        nar3p)  MAP=01narshadda.jkl; CAM=1 ;;
        hangar) MAP=12escape.jkl; CAM=1 ;;
        sarris) MAP=17asarris.jkl; WARP="2.6106 -0.5464 2.0590 0 135 0" ;;
        boc)    MAP=20aboc.jkl ;;
        yun)    MAP=07yun.jkl ;;
        cargo)  MAP=10cargo.jkl; WARP="-3.1100 14.0600 1.6903 25 0 0" ;;
        tower)  MAP=14tower.jkl; WARP="8.5200 15.3900 19.3077 15 0 0"; export CAPTURE_DELAY=3 ;;
        fuel)   MAP=09fuelstation.jkl ;;
        barons) MAP=06abarons.jkl ;;
        saber)  MAP=01narshadda.jkl; WEAPON=10; export CAPTURE_CMDS=red5 ;;
        *) echo "unknown view $v"; continue ;;
    esac
    if [ "$MODE" = "orig" ]; then
        CAPTURE_ORIGINAL=1 CAPTURE_MAP=$MAP CAPTURE_CAMERA=$CAM CAPTURE_WARP="$WARP" CAPTURE_WEAPON="$WEAPON" CAPTURE_FRAMES=30 \
            scripts/capture.sh show-orig-$v | grep -o "avg_fps=[0-9.]*" | sed "s/^/$v /"
    else
        CAPTURE_MAP=$MAP CAPTURE_CAMERA=$CAM CAPTURE_WARP="$WARP" CAPTURE_WEAPON="$WEAPON" CAPTURE_FRAMES=30 \
            scripts/capture.sh show-remaster-$v remaster=1 r_enableTextureFilter=1 | grep -o "avg_fps=[0-9.]*" | sed "s/^/$v /"
    fi
    unset CAPTURE_DELAY CAPTURE_CMDS
done
