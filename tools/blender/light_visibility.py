"""Which surface corners each candidate light can see (for tools/lightbake/fit_lights.py).

    Blender -b --python tools/blender/light_visibility.py -- hd/lightbake/<level>

Reads candidates.npz (cands, pts, rmax) written by fit_lights.py, builds a ray-cast tree of the
level's opaque surfaces (see-through ones like grates don't block), and writes visibility.npz:
for every candidate, the corners within rmax that have a clear line of sight (CSR layout:
indptr, indices).
"""
import json
import os
import sys
import time

import numpy as np
from mathutils import Vector
from mathutils.bvhtree import BVHTree
from mathutils.kdtree import KDTree


def main():
    folder = os.path.abspath(sys.argv[sys.argv.index("--") + 1])
    d = json.load(open(os.path.join(folder, "level.json")))
    c = np.load(os.path.join(folder, "candidates.npz"))
    cands, pts, rmax = c["cands"], c["pts"], float(c["rmax"])
    verts = [Vector(v) for v in d["verts"]]
    polys = [s["v"] for s in d["surfaces"]
             if not s["sky"] and not s.get("alpha") and not (s.get("faceflags", 0) & 2)]
    bvh = BVHTree.FromPolygons(verts, polys, all_triangles=False, epsilon=0.0)
    kd = KDTree(len(pts))
    for i, p in enumerate(pts):
        kd.insert(Vector(p), i)
    kd.balance()
    t0 = time.time()
    indptr, indices = [0], []
    rays = 0
    for ci, cp in enumerate(cands):
        o = Vector(cp)
        vis = []
        for _co, i, dist in kd.find_range(o, rmax):
            rays += 1
            if dist < 1e-6:
                vis.append(i)
                continue
            dvec = (Vector(pts[i]) - o) / dist
            hit = bvh.ray_cast(o, dvec, dist - 2e-3)
            if hit[0] is None:
                vis.append(i)
        vis.sort()
        indices += vis
        indptr.append(len(indices))
        if ci % 2000 == 0:
            print("VIS: %d/%d candidates, %d rays, %.0f s" % (ci, len(cands), rays, time.time() - t0), flush=True)
    np.savez_compressed(os.path.join(folder, "visibility.npz"),
                        indptr=np.array(indptr, dtype=np.int64), indices=np.array(indices, dtype=np.int32))
    print("VIS: done, %d candidates, %d rays, %.1f%% visible, %.0f s" % (
        len(cands), rays, 100.0 * len(indices) / max(rays, 1), time.time() - t0), flush=True)


try:
    main()
except Exception:
    import traceback
    traceback.print_exc()
    print("VIS: FAILED")
