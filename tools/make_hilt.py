#!/usr/bin/env python3
"""Put a Jedi Academy lightsaber hilt into JK's saber models.

    python3 tools/make_hilt.py [HILT]      HILT = folder under models/weapons2 (default: saber,
                                           Kyle's hilt from Jedi Outcast)

Replaces the 1997 hilt faces (KYSABER*.MAT) in:
  sabg.3do  third-person hilt in Kyle's hand (also handless, see tools/strip_weapon_hands.py)
  sabv.3do  first-person view (hands kept)
  sab0.3do, sabp.3do  the pickups lying in levels
The new hilt takes the old one's place and length exactly: in sabg/sabv the emitter sits at
the hand mesh's origin pointing +Y (where the engine starts the blade). Output goes to
hd/models_ja/saber/ (3do + mat/ + textures.json); its textures go into the HD pack via
tools/make_character_textures.py. Install with scripts/install_cast.sh saber.
"""
import json
import os
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import jk3do  # noqa: E402

JA = os.path.join(os.path.dirname(ROOT), "JEDI ACADEMY", "gamedata", "base")
BLENDER = "/Applications/Blender.app/Contents/MacOS/Blender"
SRC = os.path.join(ROOT, "hd", "models_src", "3do")
OUT = os.path.join(ROOT, "hd", "models_ja", "saber")
TARGETS = ["sabg", "sabv", "sab0", "sabp"]


def hilt_frame(me, faces):
    """Origin (emitter), unit axis (pommel -> emitter) and length of the old hilt faces."""
    vs = sorted({v for f in faces for v, _ in f["idx"]})
    p = np.array([me.verts[v][:3] for v in vs])
    c = p.mean(0)
    _, _, vt = np.linalg.svd(p - c, full_matrices=False)
    axis = vt[0] / np.linalg.norm(vt[0])
    t = (p - c) @ axis
    if axis[1] < 0 or (abs(axis[1]) < 1e-6 and axis[2] < 0):
        axis, t = -axis, -t
    length = float(t.max() - t.min())
    emitter = c + axis * t.max()
    return emitter, axis, length


def rotation_to(a, b):
    a = a / np.linalg.norm(a); b = b / np.linalg.norm(b)
    v = np.cross(a, b); c = float(np.dot(a, b))
    if np.linalg.norm(v) < 1e-9:
        return np.eye(3) if c > 0 else np.diag([1, -1, -1.0])
    vx = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
    return np.eye(3) + vx + vx @ vx * (1 / (1 + c))


def main():
    hilt = sys.argv[1] if len(sys.argv) > 1 else "saber"
    folder = os.path.join(JA, "models", "weapons2", hilt)
    glm = [f for f in os.listdir(folder) if f.endswith(".glm")][0]
    os.makedirs(OUT, exist_ok=True)
    js = os.path.join(OUT, "hilt.json")
    subprocess.run([BLENDER, "-b", "--factory-startup", "-noaudio", "--python",
                    os.path.join(HERE, "blender", "ja_hilt_export.py"), "--",
                    os.path.join(folder, glm), JA, js], check=True, capture_output=True)
    h = json.load(open(js))
    hv = np.array(h["verts"])
    hc = hv.mean(0)
    _, _, vt = np.linalg.svd(hv - hc, full_matrices=False)
    hax = vt[0]
    ht = (hv - hc) @ hax
    emit = np.array(h["emitter"]) if h["emitter"] else None
    if emit is not None and (emit - hc) @ hax < 0:
        hax, ht = -hax, -ht
    h_len = float(ht.max() - ht.min())
    h_emit = hc + hax * ht.max()

    textures = {}
    for name in TARGETS:
        m = jk3do.load(os.path.join(SRC, name + ".3do"))
        for geo in m.geosets:
            for me in geo:
                old = [f for f in me.faces if m.materials[f["mat"]].lower().startswith("kysaber")]
                if not old:
                    continue
                emitter, axis, length = hilt_frame(me, old)
                others = [f for f in me.faces if f not in old]
                if others and name not in ("sabg", "sabv"):  # a pickup with a blade stub: emitter faces it
                    bc = np.mean([me.verts[v][:3] for f in others for v, _ in f["idx"]], axis=0)
                    hc_old = np.mean([me.verts[v][:3] for f in old for v, _ in f["idx"]], axis=0)
                    if (bc - hc_old) @ axis < 0:
                        emitter = emitter - axis * length
                        axis = -axis
                if name in ("sabg", "sabv"):  # the engine starts the blade at the mesh origin, +Y
                    emitter, axis = np.zeros(3), np.array([0, 1.0, 0])
                s = length / h_len
                R = rotation_to(hax, axis)
                keep = [(f, n) for f, n in zip(me.faces, me.face_normals) if f not in old]
                if name == "sabg":  # third person: the new Kyle supplies the hand
                    keep = [(f, n) for f, n in keep if not m.materials[f["mat"]].lower().startswith("kyhand")]
                me.faces = [f for f, _ in keep]
                me.face_normals = [n for _, n in keep]
                v0 = len(me.verts)
                for p in hv:
                    q = emitter + R @ ((p - h_emit) * s)
                    me.verts.append((q[0], q[1], q[2], 0.0))
                # vertex normals: outward from the hilt axis
                for p in hv:
                    d = (p - hc) - hax * ((p - hc) @ hax)
                    d = R @ (d / (np.linalg.norm(d) or 1))
                    me.normals.append(tuple(d))
                for f, uv, img in zip(h["faces"], h["uvs"], h["images"]):
                    mname = ("sab_%s" % os.path.splitext(os.path.basename(img))[0].lower() if img else "sab_none")[:28] + ".mat"
                    if mname not in m.materials:
                        m.materials.append(mname)
                    textures[mname] = img
                    t0 = len(me.uvs)
                    me.uvs += [tuple(x) for x in uv]
                    idx = [(v + v0, t0 + k) for k, v in enumerate(f)]
                    a, b, c = (np.array(me.verts[i][:3]) for i, _ in idx[:3])
                    n = np.cross(b - a, c - a)
                    n = n / (np.linalg.norm(n) or 1)
                    me.faces.append({"mat": m.materials.index(mname), "type": "0x0000", "geo": 4, "light": 3,
                                     "tex": 0, "extralight": 0.0, "idx": idx})
                    me.face_normals.append(tuple(n))
                me.radius = max(me.radius, max(np.linalg.norm(np.array(v[:3])) for v in me.verts))
                print("%-5s %-10s old hilt %d faces (len %.4f) -> %d faces, scale %.4f" % (name, me.name, len(old), length, len(h["faces"]), s))
        jk3do.save(m, os.path.join(OUT, name + ".3do"), "%s with the %s hilt from Jedi Academy (tools/make_hilt.py)" % (name, hilt))
    json.dump({"textures": textures, "uv_fraction": True, "all_meshes": True}, open(os.path.join(OUT, "textures.json"), "w"), indent=1)
    print("wrote", ", ".join(TARGETS), "to", OUT)


if __name__ == "__main__":
    main()
