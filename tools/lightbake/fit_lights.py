#!/usr/bin/env python3
"""Recover the level designers' lights from the 1997 lighting (light bake, approach A).

    python3 tools/lightbake/fit_lights.py hd/lightbake/<level> [--max N] [--occlusion]

The shipped levels keep only the result of the designers' JED lights: a brightness per surface
corner. JED lit a corner with each light's intensity fading linearly to zero at its range, so
those corner values are a sum of a few such lights. This finds lights that reproduce them:
  * candidates inside every sector (its centre, and points part way from the centre towards
    each of its corners), each at several ranges;
  * a greedy non-negative fit: add the candidate that best explains what's still unexplained,
    refit all chosen lights' intensities (non-negative least squares), repeat until it stops
    paying off.
Writes lights.json beside level.json: [{pos, intensity, range}], plus a constant floor and the
fit quality. tools/blender/lightbake.py then bakes these as real lights (shadows, bounce).
"""
import json
import os
import subprocess
import sys

import numpy as np
from scipy.optimize import nnls
from scipy.sparse import csc_matrix
from scipy.spatial import cKDTree

RADII = (0.3, 0.6, 1.2, 2.4, 4.8)
BLENDER = "/Applications/Blender.app/Contents/MacOS/Blender"
FRACTIONS = (0.35, 0.7)


def main():
    folder = sys.argv[1]
    max_lights = int(sys.argv[sys.argv.index("--max") + 1]) if "--max" in sys.argv else 800
    d = json.load(open(os.path.join(folder, "level.json")))
    V = np.array(d["verts"])

    # Samples: every corner of every lit surface, nudged off the surface (lights in front of a
    # surface light it; JED measured at the corner itself, the nudge only avoids ties).
    pts, target = [], []
    for s in d["surfaces"]:
        if s["sky"] or not s["intensities"]:
            continue
        n = np.array(s["normal"])
        for v, i in zip(s["v"], s["intensities"]):
            pts.append(V[v] + n * 0.005)
            target.append(min(i, 2.0))
    pts, target = np.array(pts), np.array(target)

    # Candidates inside sectors.
    cands = []
    for sec in d["sectors"]:
        if "center" not in sec or not sec["verts"]:
            continue
        c = np.array(sec["center"])
        cands.append(c)
        for v in sec["verts"]:
            for f in FRACTIONS:
                cands.append(c + (V[v] - c) * f)
    cands = np.unique(np.round(np.array(cands), 3), axis=0)

    # Walls: with --occlusion a candidate only lights the corners it can see (ray cast in
    # Blender, cached in visibility.npz), so lights are recovered inside the rooms they light.
    visible = None
    if "--occlusion" in sys.argv:
        vis_path = os.path.join(folder, "visibility.npz")
        cand_path = os.path.join(folder, "candidates.npz")
        fresh = os.path.exists(vis_path) and os.path.exists(cand_path) and \
            np.array_equal(np.load(cand_path)["cands"], cands)
        if not fresh:
            np.savez(cand_path, cands=cands, pts=pts, rmax=max(RADII))
            here = os.path.dirname(os.path.abspath(__file__))
            subprocess.run([BLENDER, "-b", "--factory-startup", "-noaudio", "--python",
                            os.path.join(os.path.dirname(here), "blender", "light_visibility.py"),
                            "--", folder], check=True)
        v = np.load(vis_path)
        visible = [set(v["indices"][v["indptr"][i]:v["indptr"][i + 1]].tolist()) for i in range(len(cands))]

    # Sparse basis: column (candidate, range) = max(0, 1 - distance / range) at each sample.
    tree = cKDTree(pts)
    rows, cols, vals, meta = [], [], [], []
    for ci, c in enumerate(cands):
        for r in RADII:
            idx = tree.query_ball_point(c, r)
            if visible is not None:
                idx = [i for i in idx if i in visible[ci]]
            if len(idx) < 3:
                continue
            dist = np.linalg.norm(pts[idx] - c, axis=1)
            col = len(meta)
            rows += idx
            cols += [col] * len(idx)
            vals += list(1.0 - dist / r)
            meta.append((ci, r))
    A = csc_matrix((vals, (rows, cols)), shape=(len(pts), len(meta)))
    norms = np.sqrt(np.asarray(A.multiply(A).sum(axis=0)).ravel()) + 1e-9
    print("%s: %d corner samples, %d candidate positions, %d basis columns" % (
        os.path.basename(folder.rstrip("/")), len(pts), len(cands), len(meta)))

    # Greedy non-negative pursuit, with a constant floor column always in.
    sel = []
    Xsel = np.ones((len(pts), 1))
    w, _ = nnls(Xsel, target)
    resid = target - Xsel @ w
    base_rmse = float(np.sqrt(np.mean((target - target.mean()) ** 2)))
    last = float(np.sqrt(np.mean(resid ** 2)))
    while len(sel) < max_lights:
        corr = (A.T @ resid) / norms
        corr[sel] = -np.inf
        k = int(np.argmax(corr))
        if corr[k] <= 0:
            break
        sel.append(k)
        Xsel = np.hstack([Xsel, A[:, k].toarray()])
        w, _ = nnls(Xsel, target)
        resid = target - Xsel @ w
        rmse = float(np.sqrt(np.mean(resid ** 2)))
        if len(sel) % 25 == 0:
            print("  %3d lights: rmse %.4f" % (len(sel), rmse))
        if len(sel) > 20 and last - rmse < base_rmse * 0.0004:
            break
        last = rmse
    # Drop lights the final fit zeroed.
    lights = []
    for k, wk in zip(sel, w[1:]):
        if wk > 1e-3:
            ci, r = meta[k]
            lights.append({"pos": cands[ci].tolist(), "intensity": float(wk), "range": r})
    r2 = 1.0 - np.mean(resid ** 2) / np.mean((target - target.mean()) ** 2)
    print("fit: %d lights, floor %.3f, rmse %.4f (target spread %.4f), R^2 %.3f" % (
        len(lights), w[0], last, base_rmse, r2))
    json.dump({"lights": lights, "floor": float(w[0]), "rmse": last, "r2": float(r2)},
              open(os.path.join(folder, "lights.json"), "w"), indent=1)


if __name__ == "__main__":
    main()
