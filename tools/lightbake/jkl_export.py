#!/usr/bin/env python3
"""Export a Jedi Knight level for light baking (step 1 of docs/stage2-lightbake-plan.md).

    python3 tools/lightbake/jkl_export.py 12escape [--density 32]

Reads the level straight from the game's GOBs (never modifies them) and writes
hd/lightbake/<level>/ (gitignored, game data):
  level.json        vertices; every drawn surface with its material, texture UVs (0-1),
                    flags, original corner intensities, and its light map rectangle
                    (plane axes, origin, atlas position); the atlas size
  tex/<mat>.png     the material's first cel in the level's palette
  glow/<mat>.png    its glowing texels (the engine's test: colours the colormap keeps bright
                    at its darkest light level), black elsewhere; only for materials that glow

Light map layout: each JK surface is flat, so its light map coordinates are a projection
onto its own plane (u along its first edge, v across), scaled to `density` texels per JK
unit (Kyle is 0.18 units tall), packed into one atlas with padding.
"""
import json
import os
import struct
import sys
import zlib

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.dirname(HERE))
import mat_extract as mx  # noqa: E402

GAME = os.path.join(ROOT, "gamedata", "jk1")
GOBS = [os.path.join(GAME, "Resource", "Res2.gob"), os.path.join(GAME, "Resource", "Res1hi.gob"),
        os.path.join(GAME, "Episode", "JK1.GOB")]
SURF_HORIZON_SKY, SURF_CEILING_SKY = 0x200, 0x400
PAD = 2
ATLAS_W = 4096


def gob_index():
    idx = {}
    for g in GOBS:
        if os.path.exists(g):
            for name, off, ln in mx.gob_entries(g):
                idx[name] = (g, off, ln)
    return idx


def read(idx, *names):
    for n in names:
        e = idx.get(n.lower())
        if e:
            return mx.gob_read(*e)
    return None


def parse_jkl(text):
    lines = [l.split("#")[0].rstrip() for l in text.split("\n")]
    out = {"materials": [], "colormap": None}

    def section(prefix):
        for i, l in enumerate(lines):
            if l.lower().startswith(prefix.lower()):
                return i
        raise KeyError(prefix)

    def rows(start, count):
        got, i = [], start + 1
        while len(got) < count and i < len(lines):
            s = lines[i].split()
            i += 1
            if s:
                got.append(s)
        return got

    i = section("World Colormaps")
    out["colormap"] = rows(i, 1)[0][1]
    i = section("World materials")
    for s in rows(i, int(lines[i].split()[2])):
        if s[0].lower() == "end":
            break
        out["materials"].append(s[1])
    i = section("World vertices")
    out["verts"] = [[float(x) for x in s[1:4]] for s in rows(i, int(lines[i].split()[2]))]
    i = section("World texture vertices")
    out["tverts"] = [[float(x) for x in s[1:3]] for s in rows(i, int(lines[i].split()[3]))]
    i = section("World surfaces")
    surfs = []
    for s in rows(i, int(lines[i].split()[2])):
        n = int(s[9])
        vs = [tuple(int(x) for x in t.split(",")) for t in s[10:10 + n]]
        surfs.append({"mat": int(s[1]), "flags": int(s[2], 16), "faceflags": int(s[3], 16),
                      "geo": int(s[4]), "light": int(s[5]), "tex": int(s[6]), "adjoin": int(s[7]),
                      "extralight": float(s[8]), "v": [v for v, _ in vs], "tv": [t for _, t in vs],
                      "intensities": [float(x) for x in s[10 + n:10 + 2 * n]]})
    out["surfaces"] = surfs
    # Sectors: centre, corner vertices, surface range, ambient light.
    sectors, cur = [], None
    i = section("World sectors")
    j = i + 1
    while j < len(lines):
        s = lines[j].split()
        j += 1
        if not s:
            continue
        key = s[0].upper()
        if key == "SECTOR":
            cur = {"verts": [], "ambient": 0.0, "extralight": 0.0}
            sectors.append(cur)
        elif cur is None:
            continue
        elif key == "AMBIENT":
            cur["ambient"] = float(s[2])
        elif key == "EXTRA":
            cur["extralight"] = float(s[2])
        elif key == "CENTER":
            cur["center"] = [float(x) for x in s[1:4]]
        elif key == "SURFACES":
            cur["surfaces"] = [int(s[1]), int(s[2])]
        elif key.endswith(":") and key[:-1].isdigit() and len(s) == 2:
            cur["verts"].append(int(s[1]))
        elif key.startswith("SECTION"):
            break
    out["sectors"] = sectors
    return out


def glow_indices(cmp):
    """Palette indices that stay bright at the colormap's darkest light level."""
    pal = np.frombuffer(cmp[64:64 + 768], dtype=np.uint8).reshape(256, 3).astype(float) / 255
    light = np.frombuffer(cmp[64 + 768:64 + 768 + 0x4000], dtype=np.uint8).reshape(64, 256)
    luma = pal @ np.array([0.299, 0.587, 0.114])
    def glowing(level):
        lit = luma[light[level]]
        g = (luma > 0.15) & (lit >= 0.6 * luma)
        g[0] = False
        return g
    a, b = glowing(0), glowing(63)
    return a if a.sum() <= b.sum() else b


def surface_frame(P):
    n = np.zeros(3)
    for k in range(1, len(P) - 1):
        n += np.cross(P[k] - P[0], P[k + 1] - P[0])
    area = np.linalg.norm(n) / 2
    n /= (np.linalg.norm(n) or 1)
    u = P[1] - P[0]
    u -= n * (u @ n)
    u /= (np.linalg.norm(u) or 1)
    v = np.cross(n, u)
    return n, u, v, area


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    level = args[0].replace(".jkl", "")
    density = float(sys.argv[sys.argv.index("--density") + 1]) if "--density" in sys.argv else 32.0
    out = os.path.join(ROOT, "hd", "lightbake", level)
    os.makedirs(os.path.join(out, "tex"), exist_ok=True)
    os.makedirs(os.path.join(out, "glow"), exist_ok=True)
    idx = gob_index()
    jkl = parse_jkl(read(idx, "jkl/%s.jkl" % level).decode("latin1"))
    cmp = read(idx, "misc/cmp/" + jkl["colormap"])
    pal = mx.cmp_palette(cmp)
    glow = glow_indices(cmp)
    print("%s: %d verts, %d surfaces, %d materials, colormap %s, %d glowing colours" % (
        level, len(jkl["verts"]), len(jkl["surfaces"]), len(jkl["materials"]), jkl["colormap"], glow.sum()))

    # Textures: albedo and glow mask per material (first cel).
    mats = {}
    for mi, name in enumerate(jkl["materials"]):
        data = read(idx, "mat/" + name, "3do/mat/" + name)
        if not data:
            continue
        try:
            m = mx.parse_mat(data)
        except Exception:
            continue
        if not m["cels"]:
            continue
        w, h, alpha, raw = m["cels"][0]
        _, _, rgba = mx.to_rgba(m, m["cels"][0], pal)
        stem = os.path.splitext(name.lower())[0]
        mx.write_png(os.path.join(out, "tex", stem + ".png"), w, h, rgba)
        has_holes = bool(alpha) and not m["is16bit"] and (b"\x00" in raw)
        info = {"file": stem, "w": w, "h": h, "glow": False, "alpha": has_holes}
        if not m["is16bit"]:
            ids = np.frombuffer(raw, dtype=np.uint8)
            mask = glow[ids]
            if mask.mean() > 0.002:
                px = np.frombuffer(rgba, dtype=np.uint8).reshape(-1, 4).copy()
                px[~mask, :3] = 0
                px[:, 3] = 255
                mx.write_png(os.path.join(out, "glow", stem + ".png"), w, h, px.tobytes())
                info["glow"] = True
                info["glowFraction"] = float(mask.mean())
        mats[mi] = info

    # Surfaces that are drawn (geo > 0), with a light map rectangle each.
    V = np.array(jkl["verts"])
    surfaces, rects = [], []
    for si, s in enumerate(jkl["surfaces"]):
        if s["geo"] == 0 or s["mat"] < 0 or s["mat"] not in mats:
            continue
        P = V[s["v"]]
        n, u, v, area = surface_frame(P)
        info = mats[s["mat"]]
        uv = [[jkl["tverts"][t][0] / info["w"], jkl["tverts"][t][1] / info["h"]] if t >= 0 else [0, 0] for t in s["tv"]]
        sky = bool(s["flags"] & (SURF_HORIZON_SKY | SURF_CEILING_SKY))
        rec = {"index": si, "mat": info["file"], "flags": s["flags"], "faceflags": s["faceflags"],
               "alpha": info["alpha"], "geo": s["geo"], "light": s["light"],
               "extralight": s["extralight"], "v": s["v"], "uv": uv, "intensities": s["intensities"],
               "normal": n.tolist(), "sky": sky}
        if not sky:
            pu, pv = (P - P[0]) @ u, (P - P[0]) @ v
            w = max(2, int(np.ceil((pu.max() - pu.min()) * density)) + 1)
            h = max(2, int(np.ceil((pv.max() - pv.min()) * density)) + 1)
            rec["lm"] = {"u": u.tolist(), "v": v.tolist(), "origin": (P[0] + u * pu.min() + v * pv.min()).tolist(),
                         "w": w, "h": h}
            rects.append(rec)
        surfaces.append(rec)

    # Shelf packing, tallest first.
    rects.sort(key=lambda r: -r["lm"]["h"])
    x = y = shelf = 0
    for r in rects:
        w, h = r["lm"]["w"] + PAD * 2, r["lm"]["h"] + PAD * 2
        if w > ATLAS_W:
            raise RuntimeError("surface %d too large for the atlas at this density" % r["index"])
        if x + w > ATLAS_W:
            x, y, shelf = 0, y + shelf, 0
        r["lm"]["x"], r["lm"]["y"] = x + PAD, y + PAD
        x += w
        shelf = max(shelf, h)
    atlas_h = y + shelf
    atlas_h = 1 << int(np.ceil(np.log2(max(atlas_h, 1))))
    texels = sum(r["lm"]["w"] * r["lm"]["h"] for r in rects)
    print("light map: %d surfaces, %.1f M texels, atlas %dx%d (%.0f%% used) at %g texels/unit" % (
        len(rects), texels / 1e6, ATLAS_W, atlas_h, 100.0 * texels / (ATLAS_W * atlas_h), density))
    json.dump({"level": level, "colormap": jkl["colormap"], "density": density, "atlas": [ATLAS_W, atlas_h],
               "verts": jkl["verts"], "materials": {v["file"]: v for v in mats.values()}, "surfaces": surfaces,
               "sectors": jkl["sectors"]},
              open(os.path.join(out, "level.json"), "w"))
    print("wrote", os.path.relpath(out, ROOT))


if __name__ == "__main__":
    main()
