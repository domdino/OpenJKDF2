#!/usr/bin/env python3
"""Rebuild a JKGFXMOD/jkgm HD material pack whose PNGs lost their file names.

The Jedi Knight Remastered 3.3 installer stores its HD textures as unnamed compressed
chunks; only the pack's metadata.json files keep the names. This matches each unnamed PNG
to the metadata entry it belongs to, by comparing it with the original texture that entry
replaces (from tools/mat_extract.py output):

  albedo   - best colour match against the original, same aspect ratio, each PNG used once
  emissive - among the rest, mostly-dark images whose bright parts line up with the original

  tools/jkgm_rebuild.py SRC_DIR [SRC_DIR ...] --extracted hd/extracted --out hd/jkgm/materials

SRC_DIRs hold the unpacked chunks (PNG files with any names, plus the metadata JSON files).
Each metadata file becomes hd/jkgm/materials/<pack>/ with metadata.json and named PNGs,
which is the layout OpenJKDF2 loads from <data root>/jkgm/materials/. A report of weak
matches is printed so they can be checked by eye. hd/ is gitignored.
"""
import argparse
import json
import os
import re
import shutil
import sys

import numpy as np
from PIL import Image

THUMB = 16


def thumb(path):
    im = Image.open(path).convert("RGBA")
    w, h = im.size
    rgba = np.asarray(im.resize((THUMB, THUMB), Image.BOX), dtype=np.float32) / 255.0
    rgb = rgba[..., :3] * rgba[..., 3:4]          # premultiply so transparent areas read as black
    return w, h, rgb


def luma(rgb):
    return rgb[..., 0] * 0.2126 + rgb[..., 1] * 0.7152 + rgb[..., 2] * 0.0722


def same_aspect(a, b):
    return abs(a[0] / a[1] - b[0] / b[1]) < 0.01


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src", nargs="+")
    ap.add_argument("--extracted", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--max-score", type=float, default=0.03,
                    help="reject albedo matches worse than this (mean colour error 0..1)")
    args = ap.parse_args()

    # Candidates and metadata files
    cands, metas = [], []
    for d in args.src:
        for name in os.listdir(d):
            p = os.path.join(d, name)
            if not os.path.isfile(p):
                continue
            head = open(p, "rb").read(8)
            if head.startswith(b"\x89PNG"):
                try:
                    w, h, t = thumb(p)
                    cands.append({"path": p, "size": (w, h), "thumb": t, "luma": float(luma(t).mean())})
                except Exception:
                    pass
            elif head.lstrip().startswith(b"{"):
                try:
                    j = json.load(open(p))
                    if isinstance(j, dict) and "materials" in j:
                        metas.append((p, j))
                except Exception:
                    pass
    print("%d candidate PNGs, %d metadata files" % (len(cands), len(metas)))

    # Entries needing an albedo / emissive image (by target file name, deduplicated)
    targets = {}   # (pack_idx, filename) -> {"kind", "orig": (w,h,thumb)}
    for mi, (_p, meta) in enumerate(metas):
        for e in meta["materials"]:
            rep = (e.get("replaces") or [{}])[0]
            mat = os.path.basename(rep.get("name", "")).lower()
            if not mat.endswith(".mat"):
                continue
            orig_png = os.path.join(args.extracted, "%s.cel%d.png" % (mat[:-4], rep.get("cel", 0)))
            if not os.path.exists(orig_png):
                continue
            orig = thumb(orig_png)
            for kind in ("albedo_map", "emissive_map", "displacement_map"):
                fn = e.get(kind)
                if fn:
                    targets.setdefault((mi, fn), {"kind": kind, "orig": orig, "albedo": (mi, e.get("albedo_map"))})
    print("%d images to place" % len(targets))

    used, placed, weak = set(), {}, []

    def assign(kind, scorer):
        pairs = []
        for key, t in targets.items():
            if t["kind"] != kind or key in placed:
                continue
            # Glow and displacement maps only go with an entry whose colour map matched.
            if kind != "albedo_map" and t["albedo"] not in placed:
                continue
            for ci, c in enumerate(cands):
                if ci in used or not same_aspect(c["size"], t["orig"][:2]):
                    continue
                pairs.append((scorer(c, t), key, ci))
        pairs.sort(key=lambda x: x[0])
        for score, key, ci in pairs:
            if key in placed or ci in used:
                continue
            if kind == "albedo_map" and score > args.max_score:
                continue   # no confident match: leave it out rather than guess
            placed[key] = (ci, score)
            used.add(ci)

    # Albedo: plain colour distance to the original.
    assign("albedo_map", lambda c, t: float(np.abs(c["thumb"] - t["orig"][2]).mean()))
    # Emissive: mostly dark, bright where the original is bright (glowing texels).
    def emissive_score(c, t):
        ol = luma(t["orig"][2]); cl = luma(c["thumb"])
        corr = np.corrcoef(ol.ravel(), cl.ravel())[0, 1] if cl.std() > 1e-4 and ol.std() > 1e-4 else 0.0
        return (1.0 - corr) + c["luma"]
    assign("emissive_map", emissive_score)
    # Displacement (rare): greyscale structure correlated with the original.
    def disp_score(c, t):
        ol = luma(t["orig"][2]); cl = luma(c["thumb"])
        corr = np.corrcoef(ol.ravel(), cl.ravel())[0, 1] if cl.std() > 1e-4 and ol.std() > 1e-4 else 0.0
        return 1.0 - abs(corr)
    assign("displacement_map", disp_score)

    # Write packs
    os.makedirs(args.out, exist_ok=True)
    for mi, (p, meta) in enumerate(metas):
        pack = re.sub(r"[^a-z0-9_-]+", "_", (meta.get("name") or "pack%d" % mi).lower()).strip("_") or "pack%d" % mi
        pdir = os.path.join(args.out, pack)
        os.makedirs(pdir, exist_ok=True)
        n = 0
        for (pmi, fn), (ci, score) in placed.items():
            if pmi != mi:
                continue
            shutil.copyfile(cands[ci]["path"], os.path.join(pdir, fn))
            n += 1
            if (targets[(pmi, fn)]["kind"] == "albedo_map" and score > 0.02):
                weak.append((score, pack, fn))
        # Only list entries whose colour map was placed, and drop references to glow or
        # displacement maps that weren't, so the engine never tries to load a missing file
        # (unlisted textures simply keep their originals).
        kept = []
        for e in meta["materials"]:
            if not e.get("albedo_map") or not os.path.exists(os.path.join(pdir, e["albedo_map"])):
                continue
            e = dict(e)
            for k, fk in (("emissive_map", "emissive_factor"), ("displacement_map", "displacement_factor")):
                if e.get(k) and not os.path.exists(os.path.join(pdir, e[k])):
                    e.pop(k, None)
                    e.pop(fk, None)
            kept.append(e)
        out_meta = dict(meta)
        out_meta["materials"] = kept
        out_meta["comment"] = ("Rebuilt by tools/jkgm_rebuild.py from unnamed installer chunks: %d of %d "
                               "entries matched confidently. " % (len(kept), len(meta["materials"]))) + str(meta.get("comment", ""))
        with open(os.path.join(pdir, "metadata.json"), "w") as f:
            json.dump(out_meta, f, indent=1)
        if not kept:
            shutil.rmtree(pdir)   # nothing usable in this pack
        print("%-28s %3d images placed, %d entries" % (pack, n, len(meta["materials"])))

    import collections
    hist = collections.Counter()
    for key, (ci, score) in placed.items():
        if targets[key]["kind"] == "albedo_map":
            hist[min(int(score / 0.02), 10)] += 1
    print("albedo match error histogram (0.02 buckets):", dict(sorted(hist.items())))
    missing = [k for k in targets if k not in placed]
    print("unplaced: %d   unused candidates: %d" % (len(missing), len(cands) - len(used)))
    for score, pack, fn in sorted(weak, reverse=True)[:15]:
        print("  weak albedo match %.3f  %s/%s" % (score, pack, fn))


if __name__ == "__main__":
    main()
