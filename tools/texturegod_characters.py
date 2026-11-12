#!/usr/bin/env python3
"""Apply a Texture God run to the converted characters' HD pack.

    python3 tools/texturegod_characters.py "<Texture God run folder>" [--albedo]

The run's inputs are the character pack's own PNGs (e.g. ky_kyle_torso.png, copied from
hd/jkgm/materials/characters), so each manifest key names its material. For every texture that
passed QC (or warned), the ORM (AO, roughness, metal) and normal maps are added to that
material's entry in hd/jkgm/materials/characters/metadata.json; with --albedo the repainted
colour replaces the albedo too (otherwise the original Academy art stays, only the material
response changes). Re-run any time; entries are updated in place.
"""
import json
import os
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PACK = os.path.join(ROOT, "hd", "jkgm", "materials", "characters")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    use_albedo = "--albedo" in sys.argv
    run = args[0]
    man = json.load(open(os.path.join(run, "manifest.json")))
    meta_path = os.path.join(PACK, "metadata.json")
    meta = json.load(open(meta_path))
    by_name = {e["name"]: e for e in meta["materials"]}
    done = 0
    for key, e in man.items():
        stem = os.path.splitext(os.path.basename(key))[0]
        entry = by_name.get("characters.%s.mat" % stem)
        if not entry:
            print("  %-24s no character material of that name" % stem)
            continue
        if e.get("status") not in ("PASS", "WARN"):
            print("  %-24s skipped (%s)" % (stem, e.get("status")))
            continue
        folder = os.path.join(run, os.path.dirname(key))
        files = e["files"]
        for kind, field in (("orm", "orm_map"), ("normal", "normal_map")) + ((("albedo", "albedo_map"),) if use_albedo else ()):
            if kind not in files:
                continue
            dst = "%s_tg_%s.png" % (stem, kind)
            shutil.copy2(os.path.join(folder, files[kind]), os.path.join(PACK, dst))
            entry[field] = dst
        done += 1
        print("  %-24s %s%s" % (stem, e["status"], " (+albedo)" if use_albedo else ""))
    json.dump(meta, open(meta_path, "w"), indent=1)
    print("%d materials updated in %s" % (done, os.path.relpath(PACK, ROOT)))


if __name__ == "__main__":
    main()
