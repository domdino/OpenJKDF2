#!/usr/bin/env python3
"""Swap the held device in a first-person model for the JK Enhanced one, keeping our arms.

    python3 tools/swap_fp_device.py detv

For weapons where building the arms onto the JKE model goes wrong (the thermal detonator: JKE's
shoulder joint sits elsewhere, so the sleeve and upper arm land badly), take our finished model
(hd/models_ja/fp/<name>.3do, Kyle's arms on the 1997 weapon), drop the 1997 device faces from
the hand node, and put JKE's device mesh on that node instead, moved from JKE's hand joint into
ours. Output: hd/models_ja/fp_jke/<name>.3do/.skn (+ the device textures in mat/).
"""
import os
import shutil
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import jk3do  # noqa: E402

DEVICES = {"detv": ("k_rhand", "det.mat")}   # model: (hand node, 1997 device material)


def node(m, name):
    return next(i for i, nd in enumerate(m.nodes) if nd.name.lower() == name)


def main():
    name = sys.argv[1]
    hand, old_mat = DEVICES[name]
    ours = jk3do.load(os.path.join(ROOT, "hd", "models_ja", "fp", name + ".3do"))
    jke = jk3do.load(os.path.join(ROOT, "hd", "models_src", "jke", "3do", name + ".3do"))
    io, ij = node(ours, hand), node(jke, hand)
    M = np.linalg.inv(jk3do.rest_matrices(ours)[io]) @ jk3do.rest_matrices(jke)[ij]
    dst = ours.geosets[0][ours.nodes[io].mesh]
    src = jke.geosets[0][jke.nodes[ij].mesh]
    keep = [(f, n) for f, n in zip(dst.faces, dst.face_normals) if ours.materials[f["mat"]].lower() != old_mat]
    dst.faces, dst.face_normals = [f for f, _ in keep], [n for _, n in keep]
    v0, t0 = len(dst.verts), len(dst.uvs)
    R = M[:3, :3]
    dst.verts += [tuple((M @ [v[0], v[1], v[2], 1.0])[:3]) + (v[3],) for v in src.verts]
    dst.normals += [tuple(R @ np.array(n)) for n in src.normals]
    dst.uvs += list(src.uvs)
    for f, n in zip(src.faces, src.face_normals):
        mat = jke.materials[f["mat"]]
        if mat.lower() not in [x.lower() for x in ours.materials]:
            ours.materials.append(mat)
        g = dict(f)
        g["mat"] = [x.lower() for x in ours.materials].index(mat.lower())
        g["idx"] = [(v + v0, t + t0) for v, t in f["idx"]]
        dst.faces.append(g)
        dst.face_normals.append(tuple(R @ np.array(n)))
    dst.radius = max(np.linalg.norm(np.array(v[:3])) for v in dst.verts)
    out = os.path.join(ROOT, "hd", "models_ja", "fp_jke")
    os.makedirs(os.path.join(out, "mat"), exist_ok=True)
    jk3do.save(ours, os.path.join(out, name + ".3do"), "%s: our arms, JK Enhanced device (tools/swap_fp_device.py)" % name)
    shutil.copy(os.path.join(ROOT, "hd", "models_ja", "fp", name + ".skn"), os.path.join(out, name + ".skn"))
    for mat in jke.materials:
        p = os.path.join(ROOT, "hd", "models_src", "jke", "mat", mat.lower())
        if os.path.exists(p):
            shutil.copy(p, os.path.join(out, "mat", mat.lower()))
    print("%s: %d device faces from JKE on %s" % (name, len(src.faces), hand))


if __name__ == "__main__":
    main()
