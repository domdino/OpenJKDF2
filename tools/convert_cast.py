#!/usr/bin/env python3
"""Convert JK's cast to Jedi Academy characters and render share-ready side-by-sides.

    python3 tools/convert_cast.py [name ...]        (default: everyone in CAST)

For each entry: tools/blender/ja_to_jk.py (headless Blender), tools/make_character_textures.py,
tools/blender/compare_render.py, tools/make_side_by_side.py. Results:
  hd/models_ja/<name>/          <name>.3do, .skn, mat/*.mat (install into the game's 3do/)
  hd/jkgm/materials/characters  HD textures (name-matched)
  docs/screenshots/cast/<name>.jpg   labelled original vs remaster
Needs gamedata for JK (hd/models_src, from tools/gob_extract.py) and Jedi Academy
(../JEDI ACADEMY/gamedata/base), plus Blender 5 with blender-sith and the Jedi Academy add-on
in the Blender profile given by $BLENDER_USER_RESOURCES.
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
JA = os.path.join(os.path.dirname(ROOT), "JEDI ACADEMY", "gamedata", "base")
BLENDER = "/Applications/Blender.app/Contents/MacOS/Blender"
SRC = os.path.join(ROOT, "hd", "models_src")

# JK model -> (Jedi Academy character, skin, title for the side-by-side)
CAST = {
    "ky":    ("kyle", "default", "Kyle Katarn"),
    "st":    ("stormtrooper", "default", "Stormtrooper"),
    "s2":    ("stormtrooper", "default", "Stormtrooper (variant 2)"),
    "s3":    ("stormtrooper", "default", "Stormtrooper (variant 3)"),
    "s4":    ("stormtrooper", "default", "Stormtrooper (variant 4)"),
    "io":    ("imperial", "default", "Imperial officer"),
    "ic":    ("imperial", "commander", "Imperial commando"),
    "icrpt": ("imperial", "commander", "Imperial commando (repeater)"),
    # JK's "ry"/"rh" are Grans (three eyes) and "gr" is a Rodian (green, antennae), despite the names.
    "ry":    ("gran", "default", "Gran"),
    "rystr": ("gran", "default", "Gran (rifle)"),
    "rh":    ("gran", "sp", "Gran (variant 2)"),
    "rhstr": ("gran", "sp", "Gran (variant 2, rifle)"),
    "gr":    ("rodian", "default", "Rodian"),
    # Not converted: "wm"/"w2" are human women civilians (Nar Shaddaa); Jedi Academy has no match.
    "th":    ("trandoshan", "default", "Trandoshan"),
    "thstr": ("trandoshan", "default", "Trandoshan (rifle)"),
    "tk":    ("trandoshan", "sp", "Trandoshan (variant 2)"),
    "tkstr": ("trandoshan", "sp", "Trandoshan (variant 2, rifle)"),
    "ugalt": ("ugnaught", "default", "Ugnaught"),
    # The passed-out drunk on the Nar Shaddaa bar table ("tuskensoused", model thout: a Trandoshan
    # lying down). A 4th field fits onto another JK model than the entry's own name.
    "thout":  ("trandoshan", "default", "Passed-out Trandoshan"),
    "thoutw": ("weequay", "default", "Passed-out Weequay", "thout"),
}


_LEVELS = None


def level_cmp(name):
    """Colormap of the first level the JK model appears in, so the original renders in its in-game
    colours (the default palette makes e.g. olive officer uniforms bright yellow)."""
    global _LEVELS
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    import re
    import mat_extract as m
    gob = os.path.join(ROOT, "gamedata", "jk1", "Episode", "JK1.GOB")
    if _LEVELS is None:
        _LEVELS = []
        for n, o, l in m.gob_entries(gob):
            if n.lower().endswith(".jkl"):
                _LEVELS.append((n.lower(), m.gob_read(gob, o, l).decode("latin1")))
        _LEVELS.sort()
    for lvl, t in _LEVELS:
        if re.search(r"(?im)model3d=%s\.3do" % re.escape(name), t):
            sec = re.search(r"(?is)world colormaps\s+\d+(.*?)end", t)
            cm = re.search(r"\d+:\s*(\S+\.cmp)", sec.group(1)) if sec else None
            if cm:
                cmp = cm.group(1).lower()
                dst = os.path.join(SRC, "misc", "cmp", cmp)
                if not os.path.exists(dst):
                    subprocess.run([sys.executable, "-I", os.path.join(ROOT, "tools", "gob_extract.py"), SRC, cmp],
                                   capture_output=True)
                if os.path.exists(dst):
                    return dst, lvl.split("\\")[-1]
    return os.path.join(SRC, "misc", "cmp", "dflt.cmp"), "default palette"


def run(cmd, tag):
    p = subprocess.run(cmd, capture_output=True, text=True)
    lines = [l for l in (p.stdout + p.stderr).splitlines() if l.startswith(("JA2JK:", "CMP:", "wrote", "Traceback")) or "Error" in l]
    for l in lines:
        print("   ", tag, l[:200])
    return p.returncode == 0 and not any("FAILED" in l or "Traceback" in l for l in lines)


def convert(name):
    ja, skin, title = CAST[name][:3]
    target = CAST[name][3] if len(CAST[name]) > 3 else name
    out = os.path.join(ROOT, "hd", "models_ja", name)
    tpl = os.path.join(SRC, "3do", target + ".3do")
    glm = os.path.join(JA, "models", "players", ja, "model.glm")
    print("==", name, "->", ja, skin)
    if not run([BLENDER, "-b", "--factory-startup", "-noaudio", "--python",
                os.path.join(ROOT, "tools", "blender", "ja_to_jk.py"), "--",
                glm, JA, skin, tpl, out, name], "convert"):
        return False
    if not run([sys.executable, "-I", os.path.join(ROOT, "tools", "make_character_textures.py"), out, name], "textures"):
        return False
    cmp_prefix = os.path.join(ROOT, "hd", "compare", name)
    pal, lvl = level_cmp(target)
    print("    palette", os.path.basename(pal), "from", lvl)
    if not run([BLENDER, "-b", "--factory-startup", "-noaudio", "--python",
                os.path.join(ROOT, "tools", "blender", "compare_render.py"), "--",
                tpl, os.path.join(SRC, "3do", "mat"), os.path.join(out, name + ".3do"), os.path.join(out, "mat"),
                os.path.join(ROOT, "hd", "jkgm", "materials", "characters"),
                pal, cmp_prefix], "render"):
        return False
    os.makedirs(os.path.join(ROOT, "docs", "screenshots", "cast"), exist_ok=True)
    return run([sys.executable, "-I", os.path.join(ROOT, "tools", "make_side_by_side.py"),
                os.path.join(ROOT, "docs", "screenshots", "cast", name + ".jpg"), title,
                cmp_prefix + "_orig_front.png", cmp_prefix + "_new_front.png",
                cmp_prefix + "_orig_threequarter.png", cmp_prefix + "_new_threequarter.png"], "sbs")


def main():
    names = sys.argv[1:] or list(CAST)
    ok = [n for n in names if convert(n)]
    print("converted %d/%d: %s" % (len(ok), len(names), " ".join(ok)))
    bad = [n for n in names if n not in ok]
    if bad:
        print("failed:", " ".join(bad))


if __name__ == "__main__":
    main()
