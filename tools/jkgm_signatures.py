#!/usr/bin/env python3
"""Make an HD material pack palette-correct: match each image by engine hash only.

  tools/jkgm_signatures.py hd/jkgm/materials [--game gamedata/jk1]

Jedi Knight colours each level's textures through that level's own palette (colormap), so
one MAT can look like lit city windows in one level and dark wall panels in another. A pack
entry that replaces a MAT by name gets applied in every level, whatever its palette, which
shows the wrong colours. OpenJKDF2 can also match by a hash of the texture as it's drawn
(palette applied), which is exactly per level.

For every entry this renders the replaced original under each of the game's level palettes,
keeps the palettes whose render matches the HD image, and writes the engine's own hash for
each into "replaces_signatures" (computed the way jkgm_get_tex_hash does: MD5 over width,
height and every pixel converted to 16-bit through the palette). Name matching ("replaces")
is removed. Entries no palette reproduces are dropped, which also weeds out bad matches.
Run it on a pack built with tools/jkgm_rebuild.py --name-match.
"""
import argparse
import hashlib
import importlib.util
import json
import os
import struct

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("mat_extract", os.path.join(HERE, "mat_extract.py"))
mx = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(mx)

THUMB = 16


def thumb_rgba(rgba_img):
    a = np.asarray(rgba_img.resize((THUMB, THUMB), Image.BOX), dtype=np.float32) / 255.0
    return a[..., :3] * a[..., 3:4]


def luma(t):
    return (t[..., 0] * 0.2126 + t[..., 1] * 0.7152 + t[..., 2] * 0.0722).ravel()


def score(a, b):
    err = float(np.abs(a - b).mean())
    la, lb = luma(a), luma(b)
    corr = float(np.corrcoef(la, lb)[0, 1]) if la.std() > 0.01 and lb.std() > 0.01 else 1.0
    return err + 0.1 * (1.0 - corr)


def engine_hash(w, h, indices, palette_bytes, alpha):
    """OpenJKDF2 jkgm_get_tex_hash for an 8-bit texture (palette = 768 bytes as in the CMP)."""
    pal = np.frombuffer(palette_bytes, dtype=np.uint8).reshape(256, 3).astype(np.uint32)
    idx = np.frombuffer(indices, dtype=np.uint8)
    c0, c1, c2 = pal[idx, 0], pal[idx, 1], pal[idx, 2]
    if alpha:
        px = ((c0 >> 3) & 0x1F) | (((c1 >> 2) & 0x3F) << 5) | (((c2 >> 3) & 0x1F) << 11)
    else:
        px = ((c0 >> 3) & 0x1F) | (((c1 >> 3) & 0x1F) << 5) | (((c2 >> 3) & 0x1F) << 10) | np.where(idx == 0, 0, 0x8000)
    m = hashlib.md5()
    m.update(struct.pack("<II", w, h))
    m.update(px.astype("<u2").tobytes())
    return m.hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    root = os.path.dirname(HERE)
    ap.add_argument("packs")
    ap.add_argument("--game", default=os.path.join(root, "gamedata", "jk1"))
    ap.add_argument("--max-score", type=float, default=0.05)
    ap.add_argument("--any-palette", action="store_true",
                    help="sign every entry for every level palette (weapons and other things seen in every level: "
                    "one HD look everywhere instead of only where the palette matches)")
    ap.add_argument("--mats", action="append", default=[],
                    help="also a folder of loose .mat files (mods, e.g. hd/models_src/jke/mat); these win over the GOBs")
    args = ap.parse_args()

    mats, cmps = {}, {}
    for gob in ("Resource/Res2.gob", "Resource/Res1hi.gob", "Episode/JK1.GOB"):
        path = os.path.join(args.game, gob)
        for name, off, ln in mx.gob_entries(path):
            base = os.path.basename(name)
            if name.endswith(".mat"):
                mats[base] = (path, off, ln)
            elif name.endswith(".cmp") and base != "uicolormap.cmp":
                cmps[base] = (path, off, ln)
    palettes = {name: mx.gob_read(*loc)[64:64 + 768] for name, loc in cmps.items()}
    for d in args.mats:
        for f in os.listdir(d):
            if f.lower().endswith(".mat"):
                mats[f.lower()] = ("file", os.path.join(d, f))

    def read_mat(loc):
        return open(loc[1], "rb").read() if loc[0] == "file" else mx.gob_read(*loc)

    for pack in sorted(os.listdir(args.packs)):
        mpath = os.path.join(args.packs, pack, "metadata.json")
        if not os.path.exists(mpath):
            continue
        meta = json.load(open(mpath))
        kept, dropped, n_hashes = [], 0, 0
        for e in meta["materials"]:
            rep = (e.get("replaces") or [{}])[0]
            mat = os.path.basename(rep.get("name", "")).lower()
            cel = rep.get("cel", 0)
            hd_path = os.path.join(args.packs, pack, e.get("albedo_map", ""))
            if mat not in mats or not os.path.isfile(hd_path):
                dropped += 1
                continue
            parsed = mx.parse_mat(read_mat(mats[mat]))
            if cel >= len(parsed["cels"]):
                dropped += 1
                continue
            w, h, alpha, raw = parsed["cels"][cel]
            if parsed["is16bit"]:
                # Palette doesn't apply; the engine hashes the raw 16-bit pixels.
                sigs = {hashlib.md5(struct.pack("<II", w, h) + raw).hexdigest()}
            else:
                # An importer can point at the untouched extracted original ("palette_ref"), so
                # stylised looks that shift colour still find their palette.
                hd = thumb_rgba(Image.open(e.get("palette_ref") or hd_path).convert("RGBA"))
                idx = np.frombuffer(raw, dtype=np.uint8)
                scored = []
                for pname, pal in palettes.items():
                    p = np.frombuffer(pal, dtype=np.uint8).reshape(256, 3)
                    rgba = np.zeros((h, w, 4), dtype=np.uint8)
                    rgba[..., :3] = p[idx].reshape(h, w, 3)
                    rgba[..., 3] = np.where((idx == 0) & bool(alpha), 0, 255).reshape(h, w)
                    scored.append((score(thumb_rgba(Image.fromarray(rgba, "RGBA")), hd), pname))
                scored.sort()
                best = scored[0][0]
                good = [pn for s, pn in scored if s <= args.max_score and s <= best + 0.01]
                if args.any_palette:
                    good = [pn for _, pn in scored]
                if not good:
                    dropped += 1
                    continue
                # Several palettes can give identical pixels; each distinct hash once.
                sigs = {engine_hash(w, h, raw, palettes[pn], alpha & 1) for pn in good}
            e = dict(e)
            e.pop("replaces", None)
            e.pop("palette_ref", None)
            e["replaces_signatures"] = sorted(sigs)
            n_hashes += len(sigs)
            kept.append(e)
        meta["materials"] = kept
        meta["comment"] = "Hash-only (per-palette) signatures by tools/jkgm_signatures.py. " + str(meta.get("comment", ""))
        with open(mpath, "w") as f:
            json.dump(meta, f, indent=1)
        print("%-28s %4d entries kept, %4d dropped (no palette matches), %d signatures"
              % (pack, len(kept), dropped, n_hashes))


if __name__ == "__main__":
    main()
