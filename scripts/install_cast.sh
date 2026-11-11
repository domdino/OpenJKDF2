#!/bin/sh
# Install (or remove) the converted characters as loose files in a game data folder.
# Loose files in <data>/3do override the GOB archives; nothing original is changed.
#
#   scripts/install_cast.sh                 install every model in hd/models_ja into gamedata/jk1
#   scripts/install_cast.sh ky st io        install just these
#   scripts/install_cast.sh --remove        remove all installed models (back to the 1997 cast)
#   DATA=gamedata/capture scripts/install_cast.sh ...   another data folder (captures)
set -e
cd "$(dirname "$0")/.."
DATA="${DATA:-gamedata/jk1}"

if [ "$1" = "--remove" ]; then
    for d in hd/models_ja/*/; do
        n=$(basename "$d")
        rm -f "$DATA/3do/$n.3do" "$DATA/3do/$n.skn"
        for m in "$d"mat/*.mat; do [ -f "$m" ] && rm -f "$DATA/3do/mat/$(basename "$m")"; done
    done
    echo "removed converted models from $DATA/3do"
    exit 0
fi

mkdir -p "$DATA/3do/mat"
# HD textures for the characters live in the HD pack folder; make sure the data folder sees it.
[ -e "$DATA/jkgm" ] || ln -s "$PWD/hd/jkgm" "$DATA/jkgm"
names="$*"
[ -n "$names" ] || names=$(ls hd/models_ja)
count=0
for n in $names; do
    d="hd/models_ja/$n"
    if [ ! -f "$d/$n.3do" ] || [ ! -f "$d/$n.skn" ]; then
        echo "skip $n (not converted)"
        continue
    fi
    cp "$d/$n.3do" "$d/$n.skn" "$DATA/3do/"
    cp "$d"/mat/*.mat "$DATA/3do/mat/"
    count=$((count + 1))
done
echo "installed $count model(s) into $DATA/3do"
