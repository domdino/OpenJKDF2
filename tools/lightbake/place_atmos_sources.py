#!/usr/bin/env python3
"""Place smoke, steam and gas where the level motivates it (atmosphere, step 5 follow-up).

    python3 tools/lightbake/place_atmos_sources.py [LEVEL ...]

Only real sources, found in the level itself:
  * roof vents: things whose template is roofvent* (Nar Shaddaa, Escape '88) -> steam rising;
  * scripted air vents: 08_vents2.cog on Escape '88 blows along a direction through a set of
    vent surfaces -> steam jets from each of those surfaces, drifting that way;
  * toxic gas: 09_gasdamage.cog hurts the player inside its sector (the fuel station) -> a soft,
    sickly gas fog filling the sector.
Writes them into atmos/<level>.json as "auto: ..." entries, replacing earlier auto entries and
leaving hand-placed ones alone (tools/blender/atmos_volumes.py shows and edits both).
"""
import json
import os
import re
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import jkl_export as jx  # noqa: E402

STEAM_VENT = dict(kind=0, rate=7, life=3.5, size=0.04, grow=5.0, rise=0.10, spread=0.02,
                  tint=[1, 1, 1], opacity=0.55, drift=[0.0, 0.0])
GAS = dict(shape=0, density=1.2, tint=[0.75, 0.95, 0.35], soft=0.6, rot_z=0, glow=0.08)


def things(text):
    m = re.search(r"World things\s+(\d+)(.*?)\nend", text, re.S | re.I)
    out = []
    for line in (m.group(2).split("\n") if m else []):
        s = line.split()
        if len(s) > 9 and s[0].rstrip(":").isdigit():
            out.append({"template": s[1].lower(), "pos": [float(x) for x in s[3:6]], "sector": int(s[9])})
    return out


def cogs(text):
    m = re.search(r"World cogs\s+(\d+)(.*?)\nend", text, re.S | re.I)
    out = []
    for line in (m.group(2).split("\n") if m else []):
        s = line.split()
        if len(s) > 1 and s[0].rstrip(":").isdigit():
            out.append((s[1].lower(), s[2:]))
    return out


def vec(tok):
    return [float(x) for x in tok.strip("()").split("/")]


def surface_frame(jkl, si):
    V = np.array(jkl["verts"])
    P = V[jkl["surfaces"][si]["v"]]
    n = np.zeros(3)
    for a, b in zip(P, np.roll(P, -1, 0)):
        n += [(a[1] - b[1]) * (a[2] + b[2]), (a[2] - b[2]) * (a[0] + b[0]), (a[0] - b[0]) * (a[1] + b[1])]
    return P.mean(0), n / (np.linalg.norm(n) + 1e-12)


def sources(level):
    idx = jx.gob_index()
    text = jx.read(idx, "jkl/%s.jkl" % level).decode("latin1")
    jkl = jx.parse_jkl(text)
    V = np.array(jkl["verts"])
    emitters, volumes = [], []
    for t in things(text):
        if t["template"].startswith("roofvent"):
            p = t["pos"]
            emitters.append(dict(STEAM_VENT, name="auto: %s steam" % t["template"], center=[p[0], p[1], p[2] + 0.06]))
    for name, args in cogs(text):
        if name == "08_vents2.cog" and len(args) > 9:
            d = vec(args[1])
            for si in args[4:10]:
                c, n = surface_frame(jkl, int(si))
                c = c + n * 0.02
                emitters.append(dict(STEAM_VENT, name="auto: air vent %s" % si, center=[float(x) for x in c],
                                     rate=10, life=2.5, size=0.03, grow=6.0, rise=0.05, spread=0.03, opacity=0.45,
                                     drift=[d[0] * 0.25, d[1] * 0.25]))
        if name == "09_gasdamage.cog" and args:
            sec = jkl["sectors"][int(args[0])]
            if not sec["verts"]:
                continue
            P = V[sec["verts"]]
            lo, hi = P.min(0), P.max(0)
            # Thick in small drains, thinner in big sectors (a pit 400 m across at full density
            # would be a solid wall): about three units' worth of gas across the sector.
            dens = min(GAS["density"], 3.0 / max(float((hi - lo).max()), 0.1))
            volumes.append(dict(GAS, name="auto: toxic gas sector %s" % args[0], density=round(dens, 3),
                                center=[float(x) for x in (lo + hi) / 2], size=[float(x) for x in (hi - lo)]))
    return emitters, volumes


def main():
    levels = [a.replace(".jkl", "") for a in sys.argv[1:]] or ["01narshadda", "02narshadda", "08escape88", "09fuelstation"]
    root = os.path.dirname(os.path.dirname(HERE))
    for level in levels:
        emitters, volumes = sources(level)
        path = os.path.join(root, "atmos", level + ".json")
        d = json.load(open(path)) if os.path.exists(path) else {"level": level, "volumes": [], "emitters": []}
        d["volumes"] = [v for v in d.get("volumes", []) if not v.get("name", "").startswith("auto:")] + volumes
        d["emitters"] = [e for e in d.get("emitters", []) if not e.get("name", "").startswith("auto:")] + emitters
        if d["volumes"] or d["emitters"]:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            json.dump(d, open(path, "w"), indent=2)
        print("%s: %d emitters, %d gas volumes" % (level, len(emitters), len(volumes)))


if __name__ == "__main__":
    main()
