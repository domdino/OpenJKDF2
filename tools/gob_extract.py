#!/usr/bin/env python3
"""Extract files from the game's GOB archives by name pattern.

    python3 tools/gob_extract.py OUT_DIR PATTERN [PATTERN ...] [--game gamedata/jk1]

Patterns are shell-style and match the file name only, case-insensitive,
e.g. 'ky.3do' 'ky*.key' '*.cmp'. Searches every GOB under Episode/ and
Resource/. Files keep their GOB folder (3do/, 3do/key/, mat/, misc/...) so
tools that look for textures next to a model find them.

    --with-mats   also extract every .mat a matched .3do lists, plus dflt.cmp,
                  so the model opens in Blender with its textures
"""
import fnmatch
import glob
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mat_extract as m  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def gobs(game):
    out = []
    for sub in ("Resource", "Episode"):
        out += sorted(glob.glob(os.path.join(game, sub, "*.gob")) + glob.glob(os.path.join(game, sub, "*.GOB")))
    return out


def main():
    args = sys.argv[1:]
    game = os.path.join(ROOT, "gamedata", "jk1")
    if "--game" in args:
        i = args.index("--game")
        game = args[i + 1]
        del args[i:i + 2]
    with_mats = "--with-mats" in args
    args = [a for a in args if a != "--with-mats"]
    if len(args) < 2:
        print(__doc__)
        sys.exit(1)
    out_dir, patterns = args[0], [p.lower() for p in args[1:]]

    index = []  # (gob, path_in_gob, offset, length); first GOB wins, Res2 before Res1hi before episode
    seen = set()
    for g in gobs(game):
        for name, off, ln in m.gob_entries(g):
            key = name.lower().replace("\\", "/")
            if key in seen:
                continue
            seen.add(key)
            index.append((g, key, off, ln))

    def write(entry):
        g, key, off, ln = entry
        dst = os.path.join(out_dir, *key.split("/"))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        data = m.gob_read(g, off, ln)
        with open(dst, "wb") as f:
            f.write(data)
        return data

    by_name = {}
    for e in index:
        by_name.setdefault(e[1].split("/")[-1], e)

    count = 0
    wanted_mats = set()
    for e in index:
        base = e[1].split("/")[-1]
        if any(fnmatch.fnmatch(base, p) for p in patterns):
            data = write(e)
            count += 1
            if with_mats and base.endswith(".3do"):
                text = data.decode("latin1")
                sec = re.search(r"(?is)^\s*MATERIALS\s+\d+(.*?)^\s*SECTION", text, re.M)
                if sec:
                    wanted_mats.update(x.lower() for x in re.findall(r"(?im)^\s*\d+:\s*(\S+\.mat)", sec.group(1)))
    if with_mats:
        for mat in sorted(wanted_mats) + ["dflt.cmp"]:
            if mat in by_name:
                write(by_name[mat])
                count += 1
            else:
                print("  not found:", mat)
    print("%d files written to %s" % (count, out_dir))


if __name__ == "__main__":
    main()
