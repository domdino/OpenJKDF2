#!/usr/bin/env python3
"""Remove Kyle's 1997 hand from the third-person weapon models.

In third person JK attaches a weapon by replacing Kyle's right-hand mesh with the weapon's
model (bryg.3do, strg.3do, sabg.3do, ...), and those models include a low-poly hand
(KYHAND*.MAT). The converted, skinned Kyle has his own hand, so the old one would show twice.
This writes copies without the hand faces to hd/models_ja/weapons/ (installed with the cast
by scripts/install_cast.sh).

    python3 tools/strip_weapon_hands.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import jk3do  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "hd", "models_src", "3do")
OUT = os.path.join(ROOT, "hd", "models_ja", "weapons")
WEAPONS = ["bryg", "cong", "bowg", "fistg", "rldg", "rptg", "sabg", "seqg", "strg", "detg"]
HAND_MATS = ("kyhand",)


def strip(name):
    path = os.path.join(SRC, name + ".3do")
    if not os.path.exists(path):
        return None
    m = jk3do.load(path)
    removed = kept = 0
    for geo in m.geosets:
        for me in geo:
            faces, normals = [], []
            for f, fn in zip(me.faces, me.face_normals):
                if m.materials[f["mat"]].lower().startswith(HAND_MATS):
                    removed += 1
                else:
                    faces.append(f)
                    normals.append(fn)
                    kept += 1
            me.faces, me.face_normals = faces, normals
    os.makedirs(OUT, exist_ok=True)
    jk3do.save(m, os.path.join(OUT, name + ".3do"), "%s without the 1997 hand (tools/strip_weapon_hands.py)" % name)
    return removed, kept


if __name__ == "__main__":
    import subprocess
    missing = [w for w in WEAPONS if not os.path.exists(os.path.join(SRC, w + ".3do"))]
    if missing:
        subprocess.run([sys.executable, "-I", os.path.join(ROOT, "tools", "gob_extract.py"),
                        os.path.join(ROOT, "hd", "models_src")] + [w + ".3do" for w in missing], check=True)
    for w in WEAPONS:
        r = strip(w)
        print("%-6s %s" % (w, "missing" if r is None else "removed %d hand faces, kept %d" % r))
