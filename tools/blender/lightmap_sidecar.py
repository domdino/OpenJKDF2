"""Turn a light bake into the sidecar the game loads (light bake step 2).

    Blender -b --factory-startup --python tools/blender/lightmap_sidecar.py -- hd/lightbake/<level> [--fill F]

Reads level.json and lightmap.exr from tools/lightbake/jkl_export.py and tools/blender/lightbake.py
and writes hd/jkgm/lightmaps/<level>.jklm (gitignored; the game sees it through its jkgm link).
Runs in Blender only because Blender reads EXR.

Format (little endian), version 1:
    char[4]  "JKLM"
    u32      version (1)
    u32      number of world vertices (the engine checks it matches the loaded level)
    u32      number of surface records N (highest baked world surface index + 1)
    u32, u32 atlas width W, height H
    f32      fill: the game shows max(baked light, fill x 1997 vertex light), so places the
             bake missed (designer lights it didn't recover) don't go black (default 0.35)
    f32      gain: overall multiplier (1)
    N x 12 f32, one record per world surface, in world surface order:
             a.xyz, a.w   atlas u = dot(world position, a.xyz) + a.w
             b.xyz, b.w   atlas v = dot(world position, b.xyz) + b.w
             rect         u0, v0, u1, v1: clamp range (texel centres inside the surface's
                          rectangle); u1 <= 0 means the surface has no light map
    W x H x 3 f16 (RGB), rows bottom first (OpenGL's order, and Blender's)
"""
import json
import os
import struct
import sys

import bpy
import numpy as np


def main():
    argv = sys.argv[sys.argv.index("--") + 1:]
    folder = os.path.abspath(argv[0])
    level = os.path.basename(folder.rstrip("/"))
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    d = json.load(open(os.path.join(folder, "level.json")))
    fill = float(argv[argv.index("--fill") + 1]) if "--fill" in argv else 0.35

    W, H = d["atlas"]
    img = bpy.data.images.load(os.path.join(folder, "lightmap.exr"))
    if tuple(img.size) != (W, H):
        raise RuntimeError("lightmap.exr is %dx%d, level.json says %dx%d" % (img.size[0], img.size[1], W, H))
    px = np.empty(W * H * 4, dtype=np.float32)
    img.pixels.foreach_get(px)
    rgb = np.clip(px.reshape(H, W, 4)[:, :, :3], 0.0, 60000.0).astype(np.float16)

    n = max(s["index"] for s in d["surfaces"]) + 1
    rec = np.zeros((n, 12), dtype=np.float32)
    baked = 0
    for s in d["surfaces"]:
        lm = s.get("lm")
        if not lm:
            continue
        o, u, v = np.array(lm["origin"]), np.array(lm["u"]), np.array(lm["v"])
        dens = lm.get("d", d["density"])   # per surface (huge faces are coarser)
        rec[s["index"], 0:3] = u * dens / W
        rec[s["index"], 3] = (lm["x"] - o @ u * dens) / W
        rec[s["index"], 4:7] = v * dens / H
        rec[s["index"], 7] = (lm["y"] - o @ v * dens) / H
        rec[s["index"], 8:12] = ((lm["x"] + 0.5) / W, (lm["y"] + 0.5) / H,
                                 (lm["x"] + lm["w"] - 1.5) / W, (lm["y"] + lm["h"] - 1.5) / H)
        baked += 1

    out_dir = os.path.join(root, "hd", "jkgm", "lightmaps")
    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, level + ".jklm")
    with open(out, "wb") as f:
        f.write(b"JKLM")
        f.write(struct.pack("<5I2f", 1, len(d["verts"]), n, W, H, fill, 1.0))
        f.write(rec.tobytes())
        f.write(rgb.tobytes())
    print("SIDECAR: %s, %d of %d surfaces baked, atlas %dx%d, fill %.2f, light max %.2f mean %.3f, %.0f MB" % (
        os.path.relpath(out, root), baked, n, W, H, fill, float(rgb.max()), float(rgb.astype(np.float32).mean()),
        os.path.getsize(out) / 1e6))


try:
    main()
except Exception:
    import traceback
    traceback.print_exc()
    print("SIDECAR: FAILED")
