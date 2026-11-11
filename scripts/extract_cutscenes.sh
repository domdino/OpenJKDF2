#!/bin/sh
# Extract every Jedi Knight cutscene (Smacker .SMK) to ProRes 422 HQ .mov for
# upscaling and grading. Native size (640x300), 15 fps, original 22 kHz audio.
# Colours are converted to Rec.709 and tagged so editing apps read them correctly.
#
#   scripts/extract_cutscenes.sh [game root] [output folder]
#
# Defaults: gamedata/jk1 -> hd/video_src. Finished HD versions go in hd/video
# with the same base name (e.g. 01-02A.mov); see docs/DECISIONS.md.
set -e
cd "$(dirname "$0")/.."
SRC="${1:-gamedata/jk1}/Resource/VIDEO"
OUT="${2:-hd/video_src}"
mkdir -p "$OUT"
for f in "$SRC"/*.SMK "$SRC"/*.smk; do
    [ -f "$f" ] || continue
    base=$(basename "$f")
    base="${base%.*}"
    dst="$OUT/$base.mov"
    if [ -f "$dst" ]; then echo "skip $base"; continue; fi
    echo "extract $base"
    ffmpeg -nostdin -hide_banner -loglevel error -i "$f" \
        -map 0:v:0 -map 0:a:0 \
        -vf "scale=out_color_matrix=bt709:out_range=tv,format=yuv422p10le" \
        -c:v prores_ks -profile:v 3 -vendor apl0 \
        -colorspace bt709 -color_primaries bt709 -color_trc bt709 \
        -c:a pcm_s16le \
        "$dst.tmp.mov"
    mv "$dst.tmp.mov" "$dst"
done
echo "done: $(ls "$OUT"/*.mov | wc -l | tr -d ' ') files in $OUT"
