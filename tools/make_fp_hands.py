#!/usr/bin/env python3
"""Replace the 1997 first-person hands with Jedi Academy Kyle's (gloves and jacket sleeves).

    python3 tools/make_fp_hands.py           (after tools/blender/ja_fp_arms.py made arms.json)

For every first-person weapon model (bryv.3do, strv.3do, sabv.3do, ...):
  * the 1997 hand, cuff and arm faces (HD*.MAT, HND-WRT.MAT, KYRARM.MAT) are removed, the
    weapon kept;
  * Kyle's posed forearm and hand are fitted onto each hand that was there: wrist, fingertip
    centroid and thumb of the old hand define a frame, Kyle's are mapped onto it (rotation,
    uniform scale, translation);
  * the arm is one skinned mesh (<name>.skn): hand vertices follow K_?hand, forearm K_?forearm,
    upper arm K_?shouldr where the model shows one, so the wrist bends with the weapon
    animations instead of tearing.
Output: hd/models_ja/fp/<name>.3do/.skn + textures.json (then tools/make_character_textures.py
hd/models_ja/fp name,name,... and scripts/install_cast.sh fp).
"""
import json
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import jk3do  # noqa: E402

SRC = os.path.join(ROOT, "hd", "models_src", "3do")
OUT = os.path.join(ROOT, "hd", "models_ja", "fp")
MODELS = ["fistv", "bryv", "strv", "rptv", "conv", "bowv", "rldv", "seqv", "detv", "sabv"]
OLD = ("hd", "hnd-wrt", "kyrarm")
HAND_SCALE = 0.0357


def frame(w, t, th):
    x = t - w
    x /= np.linalg.norm(x)
    y = (th - w) - x * np.dot(th - w, x)
    y /= np.linalg.norm(y)
    return np.stack([x, y, np.cross(x, y)], axis=1)


def surface_points(m, me, node_mat, prefix, n=1500, seed=1):
    """Points sampled evenly over the faces whose material starts with prefix (model space)."""
    rng = np.random.default_rng(seed)
    tris = []
    for f in me.faces:
        if not m.materials[f["mat"]].lower().startswith(prefix):
            continue
        p = [(node_mat @ [*me.verts[v][:3], 1.0])[:3] for v, _ in f["idx"]]
        for k in range(1, len(p) - 1):
            tris.append((p[0], p[k], p[k + 1]))
    if not tris:
        return np.zeros((0, 3))
    T = np.array(tris)
    areas = np.linalg.norm(np.cross(T[:, 1] - T[:, 0], T[:, 2] - T[:, 0]), axis=1) / 2
    pick = rng.choice(len(T), size=n, p=areas / areas.sum())
    u, v = rng.random(n), rng.random(n)
    flip = u + v > 1
    u[flip], v[flip] = 1 - u[flip], 1 - v[flip]
    t = T[pick]
    return t[:, 0] + (t[:, 1] - t[:, 0]) * u[:, None] + (t[:, 2] - t[:, 0]) * v[:, None]


def icp(src, dst, R, t, iters=40):
    """Refine a rigid transform (R, t) mapping src points onto the dst surface (nearest points)."""
    for _ in range(iters):
        cur = src @ R.T + t
        d2 = ((cur[:, None, :] - dst[None, :, :]) ** 2).sum(-1)
        nn = dst[d2.argmin(1)]
        # robust: ignore the worst 20 % of pairs
        err = d2.min(1)
        keep = err <= np.quantile(err, 0.8)
        a, b = cur[keep], nn[keep]
        ca, cb = a.mean(0), b.mean(0)
        U, _, Vt = np.linalg.svd((a - ca).T @ (b - cb))
        D = np.diag([1, 1, np.sign(np.linalg.det(Vt.T @ U.T))])
        dR = Vt.T @ D @ U.T
        R = dR @ R
        t = dR @ (t - ca) + cb
    return R, t, float(np.sqrt(err.mean()))


def node_named(m, name):
    for i, nd in enumerate(m.nodes):
        if nd.name.lower() == name.lower():
            return i
    return None


def main():
    arms = json.load(open(os.path.join(OUT, "arms.json")))
    textures = {}
    for name in MODELS:
        path = os.path.join(SRC, name + ".3do")
        newer = os.path.join(ROOT, "hd", "models_ja", "saber", name + ".3do")  # sabv with the new hilt
        if os.path.exists(newer):
            path = newer
        if not os.path.exists(path):
            print(name, "missing")
            continue
        m = jk3do.load(path)
        rest = jk3do.rest_matrices(m)
        geo = m.geosets[0]
        skin = jk3do.Mesh()
        skin.name = "fparms"
        weights = []
        fitted = []
        for side in "rl":
            ih = node_named(m, "k_%shand" % side)
            if ih is None or m.nodes[ih].mesh < 0:
                continue
            me = geo[m.nodes[ih].mesh]
            def pts(prefix):
                vs = {v for f in me.faces if m.materials[f["mat"]].lower().startswith(prefix) for v, _ in f["idx"]}
                return [(rest[ih] @ [*me.verts[v][:3], 1.0])[:3] for v in vs]
            tips, thumb = pts("hdftip"), pts("hdsdthum")
            if not tips or not thumb:
                continue
            nd = m.nodes[ih]
            w_jk = (rest[ih] @ [-nd.pivot[0], -nd.pivot[1], -nd.pivot[2], 1.0])[:3]
            t_jk, th_jk = np.mean(tips, axis=0), np.mean(thumb, axis=0)
            a = arms[side]
            w_ja, t_ja, th_ja = (np.array(a[k]) for k in ("wrist", "tips", "thumb"))
            # One hand size for every weapon (the fit's typical value): the 1997 fists curl by
            # different amounts, so wrist-to-fingertip distances would make hands change size.
            s = HAND_SCALE
            Rm = frame(w_jk, t_jk, th_jk) @ frame(w_ja, t_ja, th_ja).T
            nodes = {"hand": ih, "forearm": node_named(m, "k_%sforearm" % side), "shoulder": node_named(m, "k_%sshouldr" % side)}
            ish = nodes["shoulder"]
            show_shoulder = ish is not None and m.nodes[ish].mesh >= 0 and any(
                m.materials[f["mat"]].lower().startswith("kyrarm") for f in geo[m.nodes[ish].mesh].faces)
            keep_v = []
            for r in a["roles"]:
                dom = max(r, key=r.get)
                keep_v.append(dom != "shoulder" or show_shoulder)
            # Rough fit from wrist/fingertips/thumb, then refine onto the old hand's surface (ICP),
            # keeping the size fixed: the new hand takes the old one's place and orientation.
            hand_pts = np.array([np.array(p) for p, r in zip(a["verts"], a["roles"]) if max(r, key=r.get) == "hand"])
            src = (hand_pts - w_ja) * s
            dst = surface_points(m, me, rest[ih], "hd")
            t0 = w_jk.copy()
            Rm, tv, rms = icp(src, dst, Rm, t0)
            remap = {}
            for vi, (p, n) in enumerate(zip(a["verts"], a["normals"])):
                if not keep_v[vi]:
                    continue
                q = tv + Rm @ ((np.array(p) - w_ja) * s)
                remap[vi] = len(skin.verts)
                skin.verts.append((q[0], q[1], q[2], 0.0))
                skin.normals.append(tuple(Rm @ np.array(n)))
                ws = []
                for role, wt in sorted(a["roles"][vi].items(), key=lambda kv: -kv[1]):
                    node = nodes.get(role) if role != "shoulder" or show_shoulder else nodes["forearm"]
                    if node is None:
                        node = ih
                    ws.append((node, wt))
                merged = {}
                for node, wt in ws:
                    merged[node] = merged.get(node, 0) + wt
                tot = sum(merged.values())
                weights.append([(k, v / tot) for k, v in sorted(merged.items(), key=lambda kv: -kv[1])[:4]])
            for f, uv, img in zip(a["faces"], a["uvs"], a["images"]):
                if not all(v in remap for v in f):
                    continue
                mname = ("fp_%s" % os.path.splitext(os.path.basename(img))[0].lower() if img else "fp_none")[:28] + ".mat"
                if mname not in m.materials:
                    m.materials.append(mname)
                textures[mname] = img
                t0 = len(skin.uvs)
                skin.uvs += [tuple(x) for x in uv]
                idx = [(remap[v], t0 + k) for k, v in enumerate(f)]
                pa, pb, pc = (np.array(skin.verts[i][:3]) for i, _ in idx[:3])
                nrm = np.cross(pb - pa, pc - pa)
                nrm = nrm / (np.linalg.norm(nrm) or 1)
                # keep JA's outward winding after a possible mirror (det < 0 never happens here: rotation)
                skin.faces.append({"mat": m.materials.index(mname), "type": "0x0000", "geo": 4, "light": 3,
                                   "tex": 0, "extralight": 0.0, "idx": idx})
                skin.face_normals.append(tuple(nrm))
            fitted.append("%s (scale %.4f, fit error %.4f%s)" % (side, s, rms, ", upper arm" if show_shoulder else ""))
        if not skin.verts:
            print(name, "no hands to replace")
            continue
        # drop the 1997 hand/cuff/arm faces, keep the weapon
        for me in geo:
            keep = [(f, n) for f, n in zip(me.faces, me.face_normals) if not m.materials[f["mat"]].lower().startswith(OLD)]
            me.faces = [f for f, _ in keep]
            me.face_normals = [n for _, n in keep]
        inv0 = np.linalg.inv(rest[0])
        skin.verts = [tuple((inv0 @ [v[0], v[1], v[2], 1.0])[:3]) + (0.0,) for v in skin.verts]
        skin.normals = [tuple(inv0[:3, :3] @ np.array(n)) for n in skin.normals]
        skin.face_normals = [tuple(inv0[:3, :3] @ np.array(n)) for n in skin.face_normals]
        skin.radius = max(np.linalg.norm(np.array(v[:3])) for v in skin.verts)
        idx = len(geo)
        geo.append(skin)
        if m.nodes[0].mesh >= 0:
            raise RuntimeError("%s: root node already has a mesh" % name)
        m.nodes[0].mesh = idx
        jk3do.save(m, os.path.join(OUT, name + ".3do"), "%s with Jedi Academy Kyle's arms (tools/make_fp_hands.py)" % name)
        jk3do.write_skin(os.path.join(OUT, name + ".skn"), [(0, idx, weights)])
        print("%-6s %d vertices, %d faces: %s" % (name, len(skin.verts), len(skin.faces), ", ".join(fitted)))
    json.dump({"textures": textures, "uv_fraction": True}, open(os.path.join(OUT, "textures.json"), "w"), indent=1)


if __name__ == "__main__":
    main()
