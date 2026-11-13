#!/usr/bin/env python3
"""Turn a Texture God output folder into an HD material pack the game loads.

  tools/texturegod_import.py "/path/to/TEXTURE GOD/out/<run>" [--pack texturegod] [--all] [--source DIR] [--mats DIR]

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
    ap.add_argument("--source", help="folder of the extracted originals the run was made from; palettes are "
                    "then matched against these instead of the HD albedo (needed for looks that shift colour)")
    ap.add_argument("--name-match", action="store_true",
                    help="match by texture name, no hashes (only for names nothing else uses, e.g. a mod's own "
                    "weapon textures loaded as loose files, like the character pack)")
    ap.add_argument("--any-palette", action="store_true", help="passed to jkgm_signatures.py (weapons: every level palette)")
    ap.add_argument("--mats", action="append", default=[],
                    help="folder of loose .mat files the textures came from (mods), passed to jkgm_signatures.py")
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
        # "<mat>@<level>" names a variant extracted in that level's palette.
        mat, cel = m.group(1).split("@")[0], int(m.group(2))
        entry = {"name": "texturegod.%s.cel%d" % (m.group(1), cel),
                 "replaces": [{"name": "mat/%s.mat" % mat, "cel": cel}]}
        if args.name_match:
            entry["replaces"] = [{"name": pre + mat + ".mat", "cel": cel} for pre in ("3do\\mat\\", "3do/mat/", "mat\\", "mat/")]
        for key, field in (("albedo", "albedo_map"), ("orm", "orm_map"), ("normal", "normal_map"), ("emissive", "emissive_map")):
            fn = files.get(key)
            if fn and os.path.isfile(os.path.join(args.run, fn)):
                shutil.copyfile(os.path.join(args.run, fn), os.path.join(pdir, fn))
                entry[field] = fn
        if args.source and os.path.isfile(os.path.join(args.source, src_name)):
            entry["palette_ref"] = os.path.abspath(os.path.join(args.source, src_name))
        if "emissive_map" in entry:
            entry["emissive_factor"] = [1.0, 1.0, 1.0]
        materials.append(entry)

    meta = {"name": args.pack, "description": "Imported from Texture God run %s" % os.path.basename(os.path.normpath(args.run)),
            "comment": "", "materials": materials}
    with open(os.path.join(pdir, "metadata.json"), "w") as f:
        json.dump(meta, f, indent=1)
    print("%d textures imported into %s (%d skipped)" % (len(materials), pdir, skipped))

    if args.name_match:
        return
    # Palette-correct engine hashes (drops name matching). Only this pack is processed.
    tmp = os.path.join(args.out, ".import_tmp")
    if os.path.isdir(tmp):
        shutil.rmtree(tmp)
    os.makedirs(tmp)
    os.symlink(pdir, os.path.join(tmp, args.pack))
    try:
        extra = [a for d in args.mats for a in ("--mats", os.path.abspath(d))] + (["--any-palette"] if args.any_palette else [])
        subprocess.run([sys.executable, "-I", os.path.join(HERE, "jkgm_signatures.py"), tmp] + extra, check=True)
    finally:
        shutil.rmtree(tmp)


if __name__ == "__main__":
    main()
