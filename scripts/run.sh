#!/bin/sh
# Launch Jedi Knight (OpenJKDF2) with the game data in gamedata/jk1.
#
# Usage: scripts/run.sh              play Jedi Knight
#        scripts/run.sh mots         play Mysteries of the Sith (data in gamedata/mots)
#        scripts/run.sh -- <args>    pass extra engine arguments, e.g.
#          scripts/run.sh -- -autostart -sp -episode JK1 -map 01narshadda.jkl
#
# Engine log goes to the terminal and to logs/last_run.log.
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/OpenJKDF2.app/Contents/MacOS/openjkdf2-64"

if [ ! -x "$BIN" ]; then
    echo "No build found at OpenJKDF2.app. Run scripts/build.sh first."
    exit 1
fi

GAME="jk1"
if [ "$1" = "mots" ]; then
    GAME="mots"
    shift
fi
[ "$1" = "--" ] && shift

mkdir -p "$ROOT/logs"

if [ "$GAME" = "mots" ]; then
    if [ ! -d "$ROOT/gamedata/mots" ]; then
        echo "No Mysteries of the Sith data in gamedata/mots yet."
        exit 1
    fi
    export OPENJKMOTS_ROOT="$ROOT/gamedata/mots"
    set -- -motsCompat "$@"
else
    if [ ! -f "$ROOT/gamedata/jk1/Resource/JK_.CD" ] && [ ! -f "$ROOT/gamedata/jk1/resource/jk_.cd" ]; then
        echo "No Jedi Knight data in gamedata/jk1 (expected Resource/JK_.CD)."
        exit 1
    fi
    export OPENJKDF2_ROOT="$ROOT/gamedata/jk1"
fi

"$BIN" "$@" 2>&1 | tee "$ROOT/logs/last_run.log"
