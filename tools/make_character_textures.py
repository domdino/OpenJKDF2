#!/usr/bin/env python3
"""Second step after tools/blender/ja_to_jk.py: turn a converted character's source images
into game materials.

    python3 tools/make_character_textures.py hd/models_ja/kyle ky [--size 128] [--pack characters]

For every material in OUT/textures.json:
  * writes OUT/mat/<name>.mat: an 8-bit MAT (SIZE x SIZE, the image quantised to the default
    palette) used as the fallback when HD textures are off;
  * copies the full-resolution image into the HD pack hd/jkgm/materials/<pack>/ as PNG and
    adds a name-matched entry, so the game shows the original true-colour art (and later any
    Texture God material maps);
  * converts the .3do's UVs from 0-1 fractions to texels of that MAT (3DO 2.1 units).
Install by copying OUT/<name>.3do, OUT/<name>.skn and OUT/mat/*.mat into the game's
3do/ and 3do/mat/ folders (loose files override the GOBs).
"""
import json
import os
import struct
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import jk3do  # noqa: E402
import mat_extract  # noqa: E402

TEMPLATE_MAT = os.path.join(ROOT, "hd", "models_src", "3do", "mat", "kyhand.mat")
CMP = os.path.join(ROOT, "hd", "models_src", "misc", "cmp", "dflt.cmp")


def write_mat(path, indices, w, h):
    """8-bit single-cel MAT, header and texinfo copied from an original JK model texture."""
    tpl = open(TEMPLATE_MAT, "rb").read()
    hdr = mat_extract.HEADER.size
    info_len = mat_extract.TEXINFO.size
    if mat_extract.TEXINFO.unpack_from(tpl, hdr)[0] & 8:
        info_len += mat_extract.TEXINFO_EXT.size
    head = tpl[:hdr + info_len]
    tex = mat_extract.TEXHEADER.pack(w, h, 0, 0, 0, 1)
    open(path, "wb").write(head + tex + bytes(indices))


def main():
    args = [a for a in sys.argv[1:]]
    size = 128
    pack = "characters"
    if "--size" in args:
        i = args.index("--size"); size = int(args[i + 1]); del args[i:i + 2]
    if "--pack" in args:
        i = args.index("--pack"); pack = args[i + 1]; del args[i:i + 2]
    out_dir, name = args[0], args[1]
    info = json.load(open(os.path.join(out_dir, "textures.json")))
    pal = np.array(mat_extract.cmp_palette(open(CMP, "rb").read()), dtype=np.float32)
    pal_use = pal.copy()
    pal_use[0] = 1e6  # index 0 is transparent in some modes; never pick it

    names = name.split(",")  # several models can share one folder's materials (e.g. the saber)
    models = {n: jk3do.load(os.path.join(out_dir, n + ".3do")) for n in names}
    all_mats = []
    for mm in models.values():
        all_mats += [x for x in mm.materials if x not in all_mats]
    os.makedirs(os.path.join(out_dir, "mat"), exist_ok=True)
    pack_dir = os.path.join(ROOT, "hd", "jkgm", "materials", pack)
    os.makedirs(pack_dir, exist_ok=True)
    meta_path = os.path.join(pack_dir, "metadata.json")
    meta = json.load(open(meta_path)) if os.path.exists(meta_path) else {
        "name": pack, "description": "Converted characters (tools/make_character_textures.py)", "materials": []}

    dims = {}
    for mat_name in all_mats:
        if mat_name not in info["textures"]:
            continue  # an original JK material on a kept rigid part
        src = info["textures"].get(mat_name)
        if src and os.path.exists(src):
            img = Image.open(src).convert("RGB")
        else:
            img = Image.new("RGB", (size, size), (128, 128, 128))
        small = np.asarray(img.resize((size, size), Image.LANCZOS), dtype=np.float32).reshape(-1, 3)
        d = ((small[:, None, :] - pal_use[None, :, :]) ** 2).sum(-1)
        idx = d.argmin(1).astype(np.uint8)
        write_mat(os.path.join(out_dir, "mat", mat_name), idx, size, size)
        dims[mat_name] = (size, size)
        if src and os.path.exists(src):
            png = mat_name.replace(".mat", ".png")
            img.save(os.path.join(pack_dir, png))
            old = [e for e in meta["materials"] if e.get("name") == "%s.%s" % (pack, mat_name)]
            meta["materials"] = [e for e in meta["materials"] if e.get("name") != "%s.%s" % (pack, mat_name)]
            entry = {
                "name": "%s.%s" % (pack, mat_name),
                "albedo_map": png,
                "replaces": [{"name": n, "cel": 0} for n in
                             ("3do\\mat\\" + mat_name, "3do/mat/" + mat_name, "mat\\" + mat_name, "mat/" + mat_name)],
            }
            # keep maps added later (tools/texturegod_characters.py): ORM, normal, a repainted albedo
            for k, v in (old[0].items() if old else ()):
                if k in ("orm_map", "normal_map") or (k == "albedo_map" and "_tg_" in v):
                    entry[k] = v
            meta["materials"].append(entry)
        print("  %-28s %s" % (mat_name, os.path.basename(src) if src else "(grey)"))

    if info.get("uv_fraction"):
        # Faces with a converted material have 0-1 UVs; make them texels of that MAT.
        for n, m in models.items():
            for geo in m.geosets:
                for me in geo:
                    uvs = list(me.uvs)
                    for f in me.faces:
                        mname = m.materials[f["mat"]]
                        if mname not in dims:
                            continue
                        w, h = dims[mname]
                        for _, t in f["idx"]:
                            u, v = me.uvs[t]
                            uvs[t] = (u * w, v * h)
                    me.uvs = uvs
            jk3do.save(m, os.path.join(out_dir, n + ".3do"), "%s with texel UVs (tools/make_character_textures.py)" % n)
        info["uv_fraction"] = False
        json.dump(info, open(os.path.join(out_dir, "textures.json"), "w"), indent=1)
    json.dump(meta, open(meta_path, "w"), indent=1)
    print("%d materials -> %s/mat, HD pack %s" % (len(dims), out_dir, os.path.relpath(pack_dir, ROOT)))


if __name__ == "__main__":
    main()
