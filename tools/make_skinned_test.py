#!/usr/bin/env python3
"""Skinning test model: merges a rigid JK model's parts (top detail level) into ONE mesh
on the root node, with every vertex weighted 100% to the node its part hung from, and
writes <out>.3do + <out>.skn. In game it must look and animate exactly like the original;
any difference is a skinning bug.

    python3 tools/make_skinned_test.py hd/models_src/3do/ky.3do OUT_DIR
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import jk3do  # noqa: E402


def main():
    src, out_dir = sys.argv[1], sys.argv[2]
    m = jk3do.load(src)
    rest = jk3do.rest_matrices(m)
    assert np.allclose(rest[0], np.eye(4)), "root node must sit at the origin with no rotation"
    node_of_mesh = {nd.mesh: i for i, nd in enumerate(m.nodes) if nd.mesh >= 0}

    merged = jk3do.Mesh()
    merged.name = "skin"
    weights = []
    for k, me in enumerate(m.geosets[0]):
        if k not in node_of_mesh or not me.verts:
            continue
        n = node_of_mesh[k]
        B = rest[n]
        R = B[:3, :3]
        v0, t0 = len(merged.verts), len(merged.uvs)
        for (x, y, z, i), nrm in zip(me.verts, me.normals):
            p = B @ np.array([x, y, z, 1.0])
            merged.verts.append((p[0], p[1], p[2], i))
            merged.normals.append(tuple(R @ np.array(nrm)))
            weights.append([(n, 1.0)])
        merged.uvs += me.uvs
        for f, fn in zip(me.faces, me.face_normals):
            g = dict(f)
            g["idx"] = [(v + v0, t + t0) for v, t in f["idx"]]
            merged.faces.append(g)
            merged.face_normals.append(tuple(R @ np.array(fn)))
        merged.geo_mode, merged.light_mode, merged.tex_mode = me.geo_mode, me.light_mode, me.tex_mode
    merged.radius = max(np.linalg.norm(v[:3]) for v in merged.verts)

    m.geosets = [[merged]]
    for nd in m.nodes:
        nd.mesh = -1
    m.nodes[0].mesh = 0
    base = os.path.splitext(os.path.basename(src))[0]
    os.makedirs(out_dir, exist_ok=True)
    jk3do.save(m, os.path.join(out_dir, base + ".3do"), "skinned test merged from %s" % base)
    jk3do.write_skin(os.path.join(out_dir, base + ".skn"), [(0, 0, weights)])
    print("%s: %d vertices, %d faces in one skinned mesh over %d nodes" % (base, len(merged.verts), len(merged.faces), len(m.nodes)))


if __name__ == "__main__":
    main()
