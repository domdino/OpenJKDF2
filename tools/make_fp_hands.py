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

--jke [name ...]: build on the JK Enhanced community weapons instead of the 1997 ones (10-35x
the detail, same JK format, same arm and hand nodes first so JK's own weapon animations drive
them; extract with tools/jke_extract.py). Their hands (HAND_TEX.MAT, split over hand, thumb and
finger nodes) are fitted to and replaced the same way; the hand frame comes from the 1997
model, carried into JKE's hand joint. Output: hd/models_ja/fp_jke/ (+ JKE's gun textures in mat/).
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
JKE = "--jke" in sys.argv
JKE_SRC = os.path.join(ROOT, "hd", "models_src", "jke", "3do")
JKE_MODELS = ["bryv", "strv", "rptv", "conv", "bowv", "rldv"]
if JKE:
    OLD = OLD + ("hand_tex",)
    _named = [a for a in sys.argv[sys.argv.index("--jke") + 1:] if not a.startswith("--")]
    MODELS = _named or JKE_MODELS
HAND_SCALE = 0.0357
# Bare hands from the free "PSX First Person Arms" pack (tools/blender/psx_fp_hands.py writes
# psx_hands.json); --ja-hands keeps Academy Kyle's gloved hands instead.
PSX_SOURCE = os.path.expanduser("~/Downloads/psx-first-person-arms-free-game-assets/arms_01.png")
PSX_TEXTURE = os.path.join(OUT, "psx_skin.png")
SKIN_GAIN = (0.8, 0.77, 0.78)  # the pack's skin is lighter and pinker than Kyle's face texture
SLIM = 0.45                     # forearm narrows by this much towards the sleeve end
PSX_SIZE = 0.9                  # a touch smaller than Academy Kyle's gloved fist


def make_skin():
    from PIL import Image
    im = np.asarray(Image.open(PSX_SOURCE).convert("RGB")).astype(float)
    Image.fromarray(np.clip(im * np.array(SKIN_GAIN), 0, 255).astype(np.uint8)).save(PSX_TEXTURE)


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


def rotation_to(a, b):
    a = a / np.linalg.norm(a)
    b = b / np.linalg.norm(b)
    v = np.cross(a, b)
    c = float(np.dot(a, b))
    if np.linalg.norm(v) < 1e-9:
        return np.eye(3)
    vx = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
    return np.eye(3) + vx + vx @ vx * (1 / (1 + c))


def graft_hand(a, h, texture):
    """Kyle's arm (a) with his gloved hand faces swapped for another hand (h, same JSON layout):
    h's wrist/fingertips/thumb frame is mapped onto Kyle's (rotation, uniform scale, translation)."""
    def fr(d):
        w, t, th = (np.array(d[k]) for k in ("wrist", "tips", "thumb"))
        return w, frame(w, t, th), np.linalg.norm(t - w)
    wa, Fa, la = fr(a)
    wh, Fh, lh = fr(h)
    R = Fa @ Fh.T
    s = la / lh * PSX_SIZE
    keep_f = [i for i, img in enumerate(a["images"]) if not (img and "basic_hand" in img)]
    used = sorted({v for i in keep_f for v in a["faces"][i]})
    remap = {v: k for k, v in enumerate(used)}
    out = {k: a[k] for k in ("wrist", "tips", "thumb", "elbow")}
    out["verts"] = [a["verts"][v] for v in used]
    out["normals"] = [a["normals"][v] for v in used]
    out["roles"] = [a["roles"][v] for v in used]
    out["faces"] = [[remap[v] for v in a["faces"][i]] for i in keep_f]
    out["uvs"] = [a["uvs"][i] for i in keep_f]
    out["images"] = [a["images"][i] for i in keep_f]
    # Kyle's glove was a gauntlet reaching up into the sleeve: keep as much of the new forearm
    # (plus a margin) so the bare wrist fills that gap instead of leaving a hole.
    up = np.array(a["elbow"]) - wa
    up /= np.linalg.norm(up)
    glove = [np.array(a["verts"][v]) for i, img in enumerate(a["images"]) if img and "basic_hand" in img for v in a["faces"][i]]
    reach = max(float((p - wa) @ up) for p in glove) * 1.1
    hv = [wa + R @ ((np.array(p) - wh) * s) for p in h["verts"]]
    hn = [R @ np.array(n) for n in h["normals"]]
    # The rigs bend at the wrist differently: turn the new forearm onto Kyle's (blended by the
    # forearm weight, like a wrist joint), then slim it to sit inside the sleeve.
    fa = R @ (np.array(h["elbow"]) - wh)
    Q = rotation_to(fa, up)
    for i, r in enumerate(h["roles"]):
        wf = 1.0 - r.get("hand", 0.0)
        if wf > 0:
            q = Q @ (hv[i] - wa)
            hv[i] = hv[i] + wf * (wa + q - hv[i])
            n = hn[i] + wf * (Q @ hn[i] - hn[i])
            hn[i] = n / (np.linalg.norm(n) or 1)
    for i, p in enumerate(hv):
        d = float((p - wa) @ up)
        if d > 0.0:
            k = 1.0 - SLIM * min(d / reach, 1.0)
            hv[i] = wa + up * d + (p - wa - up * d) * k
    hf = [i for i, f in enumerate(h["faces"]) if min(float((hv[v] - wa) @ up) for v in f) <= reach]
    v0 = len(out["verts"])
    out["verts"] += [list(p) for p in hv]
    out["normals"] += [list(n) for n in hn]
    out["roles"] += [{k: w for k, w in r.items() if k != "shoulder"} or {"forearm": 1.0} for r in h["roles"]]
    out["faces"] += [[v + v0 for v in h["faces"][i]] for i in hf]
    out["uvs"] += [h["uvs"][i] for i in hf]
    out["images"] += [texture] * len(hf)
    out["fit"] = a["fit"] if "fit" in a else [p for p, r in zip(a["verts"], a["roles"]) if max(r, key=r.get) == "hand"]
    return out, s


def subtree(m, i):
    out, todo = [], [i]
    while todo:
        k = todo.pop()
        out.append(k)
        todo += [j for j, nd in enumerate(m.nodes) if nd.parent == k]
    return out


def hand_surface(m, rest, ih, prefix, n=1500, seed=1):
    """Points over every face with this material on the hand node and the nodes under it."""
    pts = [surface_points(m, m.geosets[0][m.nodes[k].mesh], rest[k], prefix, n, seed)
           for k in subtree(m, ih) if m.nodes[k].mesh >= 0]
    pts = [p for p in pts if len(p)]
    if not pts:
        return np.zeros((0, 3))
    allp = np.concatenate(pts)
    return allp[np.random.default_rng(seed).choice(len(allp), size=min(n, len(allp)), replace=False)]


def node_named(m, name):
    for i, nd in enumerate(m.nodes):
        if nd.name.lower() == name.lower():
            return i
    return None


def main():
    arms = json.load(open(os.path.join(OUT, "arms.json")))
    psx = os.path.join(OUT, "psx_hands.json")
    if os.path.exists(psx) and "--ja-hands" not in sys.argv:
        hands = json.load(open(psx))
        make_skin()
        for side in "rl":
            arms[side], s = graft_hand(arms[side], hands[side], PSX_TEXTURE)
            print("grafted the PSX %s hand (scale %.3f)" % (side, s))
    textures = {}
    out_dir = os.path.join(ROOT, "hd", "models_ja", "fp_jke") if JKE else OUT
    os.makedirs(out_dir, exist_ok=True)
    for name in MODELS:
        path = os.path.join(SRC, name + ".3do")
        newer = os.path.join(ROOT, "hd", "models_ja", "saber", name + ".3do")  # sabv with the new hilt
        if os.path.exists(newer) and not JKE:
            path = newer
        m97 = None
        if JKE:
            m97 = jk3do.load(path)
            path = os.path.join(JKE_SRC, name + ".3do")
        if not os.path.exists(path):
            print(name, "missing")
            continue
        m = jk3do.load(path)
        rest = jk3do.rest_matrices(m)
        rest97 = jk3do.rest_matrices(m97) if m97 else None
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
            # The hand frame comes from the 1997 hand (its fingertip and thumb textures); with
            # --jke it is carried from the 1997 hand joint into JKE's.
            src_m, src_rest, src_ih, carry = m, rest, ih, np.eye(4)
            if m97 is not None:
                src_ih = node_named(m97, "k_%shand" % side)
                if src_ih is None or m97.nodes[src_ih].mesh < 0:
                    continue
                src_m, src_rest = m97, rest97
                carry = rest[ih] @ np.linalg.inv(rest97[src_ih])
            sme = src_m.geosets[0][src_m.nodes[src_ih].mesh]
            def pts(prefix):
                vs = {v for f in sme.faces if src_m.materials[f["mat"]].lower().startswith(prefix) for v, _ in f["idx"]}
                return [(carry @ src_rest[src_ih] @ [*sme.verts[v][:3], 1.0])[:3] for v in vs]
            tips, thumb = pts("hdftip"), pts("hdsdthum")
            nd = src_m.nodes[src_ih]
            w_jk = (carry @ src_rest[src_ih] @ [-nd.pivot[0], -nd.pivot[1], -nd.pivot[2], 1.0])[:3]
            if not tips and thumb and JKE:
                # No fingertip texture (the sequencer's flat hand): the hand points farthest from
                # the wrist stand in for the fingertips; the surface fit below refines it.
                hand = np.array(pts("hdinsid") + pts("hdback"))
                if len(hand):
                    d = np.linalg.norm(hand - w_jk, axis=1)
                    tips = list(hand[d >= np.quantile(d, 0.8)])
            if not tips or not thumb:
                continue
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
            if m97 is not None:   # JKE's upper arm is HAND_TEX: ask the 1997 model whether one shows
                i97 = node_named(m97, "k_%sshouldr" % side)
                show_shoulder = ish is not None and i97 is not None and m97.nodes[i97].mesh >= 0 and any(
                    m97.materials[f["mat"]].lower().startswith("kyrarm") for f in m97.geosets[0][m97.nodes[i97].mesh].faces)
            keep_v = []
            for r in a["roles"]:
                dom = max(r, key=r.get)
                keep_v.append(dom != "shoulder" or show_shoulder)
            # Rough fit from wrist/fingertips/thumb, then refine onto the old hand's surface (ICP),
            # keeping the size fixed: the new hand takes the old one's place and orientation.
            hand_pts = np.array(a["fit"]) if "fit" in a else np.array(
                [np.array(p) for p, r in zip(a["verts"], a["roles"]) if max(r, key=r.get) == "hand"])
            src = (hand_pts - w_ja) * s
            dst = hand_surface(m, rest, ih, "hand_tex") if JKE else surface_points(m, me, rest[ih], "hd")
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
        jk3do.save(m, os.path.join(out_dir, name + ".3do"), "%s with Jedi Academy Kyle's arms (tools/make_fp_hands.py%s)" % (
            name, " --jke, on the JK Enhanced weapon" if JKE else ""))
        jk3do.write_skin(os.path.join(out_dir, name + ".skn"), [(0, idx, weights)])
        print("%-6s %d vertices, %d faces: %s" % (name, len(skin.verts), len(skin.faces), ", ".join(fitted)))
    if JKE:
        # the guns' own textures, plus the arm textures already made for the 1997 set
        import shutil
        os.makedirs(os.path.join(out_dir, "mat"), exist_ok=True)
        for name in MODELS:
            fp = os.path.join(out_dir, name + ".3do")
            if not os.path.exists(fp):
                continue
            for mat in jk3do.load(fp).materials:
                src = os.path.join(ROOT, "hd", "models_src", "jke", "mat", mat.lower())
                if os.path.exists(src):
                    shutil.copy(src, os.path.join(out_dir, "mat", mat.lower()))
        for f in os.listdir(os.path.join(OUT, "mat")):
            shutil.copy(os.path.join(OUT, "mat", f), os.path.join(out_dir, "mat", f))
        return
    json.dump({"textures": textures, "uv_fraction": True}, open(os.path.join(OUT, "textures.json"), "w"), indent=1)


if __name__ == "__main__":
    main()
