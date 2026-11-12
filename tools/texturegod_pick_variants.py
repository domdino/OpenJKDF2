#!/usr/bin/env python3
"""Pick the level textures no Texture God pack covers yet, one per level palette.

    python3 tools/texturegod_pick_variants.py N [--out hd/tg_in/levels_palettes]

Needs hd/lightbake/<level>/ for each level (tools/lightbake/jkl_export.py, any density),
which holds every texture in that level's own palette. Each texture on a level surface is
hashed the way the engine does it under that palette; hashes already in a Texture God pack
are skipped. The N largest by surface area (the seven test levels first) are copied as
<mat>@<level>.cel0.png, ready for Texture God; tools/texturegod_import.py maps them back.
"""
import collections, json, os, sys, importlib.util, numpy as np
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
spec = importlib.util.spec_from_file_location("sig", os.path.join(ROOT, "tools/jkgm_signatures.py"))
sig = importlib.util.module_from_spec(spec); spec.loader.exec_module(sig)
mx = sig.mx
game = os.path.join(ROOT, "gamedata/jk1")
mats, cmps = {}, {}
for gob in ("Resource/Res2.gob", "Resource/Res1hi.gob", "Episode/JK1.GOB"):
    p = os.path.join(game, gob)
    for name, off, ln in mx.gob_entries(p):
        b = os.path.basename(name)
        if name.endswith(".mat"): mats[b] = (p, off, ln)
        elif name.endswith(".cmp"): cmps[b] = (p, off, ln)
have = set()
for pack in sorted(p for p in os.listdir(os.path.join(ROOT, "hd/jkgm/materials")) if p.startswith("texturegod")):
    for e in json.load(open(os.path.join(ROOT, "hd/jkgm/materials", pack, "metadata.json")))["materials"]:
        have.update(e.get("replaces_signatures", []))
KEY = ["12escape", "17asarris", "20aboc", "07yun", "10cargo", "14tower", "01narshadda"]
levels = KEY + sorted(l for l in os.listdir(os.path.join(ROOT, "hd/lightbake")) if l not in KEY)
area = collections.Counter(); first = {}
for l in levels:
    d = json.load(open(os.path.join(ROOT, f"hd/lightbake/{l}/level.json")))
    pal = mx.gob_read(*cmps[d["colormap"].lower()])[64:64+768]
    V = np.array(d["verts"])
    for s in d["surfaces"]:
        if s["sky"]: continue
        m = s["mat"] + ".mat"
        if m not in mats: continue
        P = V[s["v"]]; n = np.zeros(3)
        for k in range(1, len(P)-1): n += np.cross(P[k]-P[0], P[k+1]-P[0])
        pm = mx.parse_mat(mx.gob_read(*mats[m]))
        if pm["is16bit"] or not pm["cels"]: continue
        w, h, alpha, raw = pm["cels"][0]
        hsh = sig.engine_hash(w, h, raw, pal, alpha & 1)
        if hsh in have: continue
        area[hsh] += np.linalg.norm(n)/2 * (1000 if l in KEY else 1)
        first.setdefault(hsh, (s["mat"], l))
out = os.path.join(ROOT, sys.argv[sys.argv.index("--out") + 1]) if "--out" in sys.argv else os.path.join(ROOT, "hd/tg_in/levels_palettes")
os.makedirs(out, exist_ok=True)
n = int(sys.argv[1])
pick = [h for h, _ in area.most_common()][:n]
tot = sum(area.values()); cov = sum(area[h] for h in pick)
import shutil
for h in pick:
    m, l = first[h]
    shutil.copy2(os.path.join(ROOT, f"hd/lightbake/{l}/tex/{m}.png"), os.path.join(out, f"{m}@{l}.cel0.png"))
print(len(area), "uncovered texture/palette variants;", len(pick), "picked, %.0f%% of their area" % (100*cov/tot))
print(collections.Counter(first[h][1] for h in pick))
