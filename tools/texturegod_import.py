#!/usr/bin/env python3
"""Turn a Texture God output folder into an HD material pack the game loads.

  tools/texturegod_import.py "/path/to/TEXTURE GOD/out/<run>" [--pack texturegod] [--all]

Reads <run>/manifest.json (keyed by the extracted file name, "<mat>.cel<N>.png") and writes
hd/jkgm/materials/<pack>/ with metadata.json and, per texture, the albedo, ORM
(R = AO, G = roughness, B = metalness), normal and emissive maps. Only textures that passed
Texture God's QC are imported unless --all. Then runs tools/jkgm_signatures.py on the
pack, so every texture is matched by the engine hash for each level palette it suits
(no name matching, which would show the wrong colours in other levels).

Packs load in alphabetical order and later ones win, so "texturegod" overrides the
community Neural Upscale pack wherever both cover a texture.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run")
    ap.add_argument("--pack", default="texturegod")
    ap.add_argument("--out", default=os.path.join(ROOT, "hd", "jkgm", "materials"))
    ap.add_argument("--all", action="store_true", help="import textures that failed QC too")
    args = ap.parse_args()

    manifest = json.load(open(os.path.join(args.run, "manifest.json")))
    pdir = os.path.join(args.out, args.pack)
    if os.path.isdir(pdir):
        shutil.rmtree(pdir)
    os.makedirs(pdir)

    materials, skipped = [], 0
    for src_name, info in sorted(manifest.items()):
        m = re.match(r"(.+)\.cel(\d+)\.png$", src_name)
        files = info.get("files") or {}
        if not m or "albedo" not in files:
            skipped += 1
            continue
        if not args.all and str(info.get("status", "")).upper() not in ("PASS", "WARN"):
            skipped += 1
            continue
        mat, cel = m.group(1), int(m.group(2))
        entry = {"name": "texturegod.%s.cel%d" % (mat, cel),
                 "replaces": [{"name": "mat/%s.mat" % mat, "cel": cel}]}
        for key, field in (("albedo", "albedo_map"), ("orm", "orm_map"), ("normal", "normal_map"), ("emissive", "emissive_map")):
            fn = files.get(key)
            if fn and os.path.isfile(os.path.join(args.run, fn)):
                shutil.copyfile(os.path.join(args.run, fn), os.path.join(pdir, fn))
                entry[field] = fn
        if "emissive_map" in entry:
            entry["emissive_factor"] = [1.0, 1.0, 1.0]
        materials.append(entry)

    meta = {"name": args.pack, "description": "Imported from Texture God run %s" % os.path.basename(os.path.normpath(args.run)),
            "comment": "", "materials": materials}
    with open(os.path.join(pdir, "metadata.json"), "w") as f:
        json.dump(meta, f, indent=1)
    print("%d textures imported into %s (%d skipped)" % (len(materials), pdir, skipped))

    # Palette-correct engine hashes (drops name matching). Only this pack is processed.
    tmp = os.path.join(args.out, ".import_tmp")
    if os.path.isdir(tmp):
        shutil.rmtree(tmp)
    os.makedirs(tmp)
    os.symlink(pdir, os.path.join(tmp, args.pack))
    try:
        subprocess.run([sys.executable, "-I", os.path.join(HERE, "jkgm_signatures.py"), tmp], check=True)
    finally:
        shutil.rmtree(tmp)


if __name__ == "__main__":
    main()
