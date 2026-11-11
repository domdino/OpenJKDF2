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
    for w in hd/models_ja/weapons/*.3do hd/models_ja/saber/*.3do; do [ -f "$w" ] && rm -f "$DATA/3do/$(basename "$w")"; done
    echo "removed converted models from $DATA/3do"
    exit 0
fi

mkdir -p "$DATA/3do/mat"
# HD textures for the characters live in the HD pack folder; make sure the data folder sees it.
[ -e "$DATA/jkgm" ] || ln -s "$PWD/hd/jkgm" "$DATA/jkgm"
names="$*"
[ -n "$names" ] || names=$(ls hd/models_ja | grep -v '^weapons$')
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
# The new Kyle has his own hands: use the third-person weapon models without the 1997 hand
# (tools/strip_weapon_hands.py).
case " $names " in *" ky "*)
    if [ -d hd/models_ja/weapons ]; then cp hd/models_ja/weapons/*.3do "$DATA/3do/"; echo "installed handless weapon models"; fi;;
esac
# The lightsaber (tools/make_hilt.py): third-person, first-person and pickup models.
case " $names " in *" saber "*|*" ky "*)
    if [ -f hd/models_ja/saber/sabg.3do ]; then cp hd/models_ja/saber/*.3do "$DATA/3do/"; cp hd/models_ja/saber/mat/*.mat "$DATA/3do/mat/"; echo "installed the new lightsaber"; fi;;
esac
echo "installed $count model(s) into $DATA/3do"
