#!/usr/bin/env python3
"""Where to bake the light volume (atmosphere step 1): points filling the air of a level.

    python3 tools/lightbake/volume_points.py LEVEL [--fine 0.25] [--coarse 1.0]

The fog needs to know how much light there is everywhere in the air, not just near floors.
This fills every sector with points on two aligned grids:
  * fine (--fine, default 0.25 units, about 2.5 m) inside normal-sized sectors;
  * coarse (--coarse, default 1.0, doubled until it fits) inside every sector, so the huge open
    sectors some levels have (space, sky, canyons hundreds of units across) still get light.
A point is inside a sector when it's on the inner side of every one of its faces, portals
included (read from the JKL itself: the light bake export leaves portals out), and at least
EDGE units from all of them. JK sectors are convex.

Writes hd/lightbake/<level>/volume_points.npz: integer grid coordinates and the spacing of each
grid (world position = index x spacing). tools/blender/volume_bake.py bakes them.
"""
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import jkl_export as jx  # noqa: E402

EDGE = 0.01              # keep points this far inside every face
SECTOR_CAP = 40000       # a sector needing more fine points than this only gets coarse ones
FINE_CAP = 1500000       # fine points per level (spacing grows until it fits)
COARSE_CAP = 300000      # coarse points per level


def sector_planes(jkl):
    """Per sector: (normals, offsets) with n.p + o >= 0 inside, every face including portals."""
    V = np.array(jkl["verts"])
    out = []
    for sec in jkl["sectors"]:
        first, count = sec.get("surfaces", (0, 0))
        c = np.array(sec.get("center", V[sec["verts"]].mean(0) if sec["verts"] else [0, 0, 0]))
        ns, os_ = [], []
        for i in range(first, first + count):
            P = V[jkl["surfaces"][i]["v"]]
            # Newell normal, turned to face the sector's centre (sectors are convex).
            n = np.zeros(3)
            for a, b in zip(P, np.roll(P, -1, 0)):
                n += [(a[1] - b[1]) * (a[2] + b[2]), (a[2] - b[2]) * (a[0] + b[0]), (a[0] - b[0]) * (a[1] + b[1])]
            ln = np.linalg.norm(n)
            if ln < 1e-12:
                continue
            n /= ln
            o = -n @ P.mean(0)
            if n @ c + o < 0:
                n, o = -n, -o
            ns.append(n)
            os_.append(o)
        out.append((np.array(ns), np.array(os_), V[sec["verts"]] if sec["verts"] else None))
    return out


def fill(planes, spacing, cap=None):
    """Grid indices of points inside each sector; sectors over cap are left out (returned)."""
    pts, big = [], []
    for si, (N, O, P) in enumerate(planes):
        if P is None or not len(N):
            continue
        lo = np.floor(P.min(0) / spacing).astype(int)
        hi = np.ceil(P.max(0) / spacing).astype(int)
        dims = hi - lo + 1
        if cap and np.prod(dims.astype(float)) > cap * 8:
            big.append(si)
            continue
        g = np.stack(np.meshgrid(*[np.arange(lo[k], hi[k] + 1) for k in range(3)], indexing="ij"), -1).reshape(-1, 3)
        w = g * spacing
        ok = np.all(w @ N.T + O >= EDGE, axis=1)
        g = g[ok]
        if cap and len(g) > cap:
            big.append(si)
            continue
        pts.append(g)
    pts = np.unique(np.concatenate(pts), axis=0) if pts else np.zeros((0, 3), int)
    return pts, big


def main():
    level = sys.argv[1].replace(".jkl", "")
    fine = float(sys.argv[sys.argv.index("--fine") + 1]) if "--fine" in sys.argv else 0.25
    coarse = float(sys.argv[sys.argv.index("--coarse") + 1]) if "--coarse" in sys.argv else 1.0
    idx = jx.gob_index()
    jkl = jx.parse_jkl(jx.read(idx, "jkl/%s.jkl" % level).decode("latin1"))
    planes = sector_planes(jkl)
    while True:
        f, big = fill(planes, fine, SECTOR_CAP)
        if len(f) <= FINE_CAP:
            break
        fine *= 1.25
    while True:
        c, _ = fill(planes, coarse)
        if len(c) <= COARSE_CAP:
            break
        coarse *= 2.0
    out = os.path.join(jx.ROOT, "hd", "lightbake", level, "volume_points.npz")
    np.savez(out, fine=f.astype(np.int16), fine_spacing=fine, coarse=c.astype(np.int16), coarse_spacing=coarse)
    print("volume points: %d fine at %.3f (%d big sectors coarse only), %d coarse at %.2f" % (
        len(f), fine, len(big), len(c), coarse))


if __name__ == "__main__":
    main()
