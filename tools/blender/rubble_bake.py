"""Bake the light on the crease rubble into its corners (terrain skin, rubble step).

    Blender -b --factory-startup --python tools/blender/rubble_bake.py -- hd/lightbake/<level> \
        [--samples N] [--glow S] [--power P] [--bounces N] [--sky R,G,B] [--sun ...]

Reads hd/lightbake/<level>/rubble.npz (tools/lightbake/terrain_skin.py) and bakes the light
arriving at every rock corner in the light map's scene (same lights, sun, glow, sky and the
calibration tools/blender/probe_bake.py saved in probes.json, so run that first), so each rock
gets its own shading, shadows and contact darkness.

Writes hd/jkgm/lightmaps/<level>.jklr (little endian):
    char[4] "JKLR", u32 version (1), u32 world vertex count (checked against the level),
    u32 surface count S, then S x { u32 surface (the wall the rocks belong to), u32 triangles T,
                                    T x 3 x f32 x, y, z, u, v, r, g, b (light, light map units),
                                                nx, ny, nz (smooth normal) }  (version 2)
"""
import json
import os
import struct
import sys
import time

import bpy
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
_src = open(os.path.join(HERE, "lightbake.py")).read()
_cut = _src.rfind("\ntry:\n    main()")
_lb = {"__name__": "lightbake", "__file__": os.path.join(HERE, "lightbake.py")}
exec(compile(_src[:_cut] if _cut > 0 else _src, "lightbake.py", "exec"), _lb)
arg, build, add_lights, add_glow_cards, set_glow, add_sun = (
    _lb[k] for k in ("arg", "build", "add_lights", "add_glow_cards", "set_glow", "add_sun"))


def log(*a):
    print("RUBBLE:", *a, flush=True)


def main():
    folder = os.path.abspath(sys.argv[sys.argv.index("--") + 1])
    level = os.path.basename(folder.rstrip("/"))
    out = os.path.join(ROOT, "hd", "jkgm", "lightmaps", level + ".jklr")
    rp = os.path.join(folder, "rubble.npz")
    if not os.path.exists(rp):
        if os.path.exists(out):
            os.remove(out)
        log("no rubble for %s" % level)
        return
    d = json.load(open(os.path.join(folder, "level.json")))
    scale = json.load(open(os.path.join(folder, "probes.json")))["scale"]
    z = np.load(rp)
    T, UV, S = z["tris"], z["uv"], z["surf"]
    glow = arg("--glow", 14.0, float)
    sky = tuple(float(x) for x in arg("--sky", "0.02,0.025,0.035").split(","))
    for o in list(bpy.data.objects):
        bpy.data.objects.remove(o, do_unlink=True)
    sc = bpy.context.scene
    sc.render.engine = "CYCLES"
    prefs = bpy.context.preferences.addons["cycles"].preferences
    prefs.compute_device_type = "METAL"
    prefs.get_devices()
    for dv in prefs.devices:
        dv.use = dv.type == "METAL"
    sc.cycles.device = "GPU"
    sc.world = bpy.data.worlds.new("w")
    sc.world.use_nodes = True
    sc.world.node_tree.nodes["Background"].inputs[0].default_value = (0, 0, 0, 1)
    sc.cycles.max_bounces = 4
    sc.cycles.diffuse_bounces = arg("--bounces", 1, int)
    sc.cycles.transparent_max_bounces = 8
    sc.cycles.samples = arg("--samples", 512, int)
    sc.render.bake.use_pass_direct = True
    sc.render.bake.use_pass_indirect = True
    sc.render.bake.use_pass_color = False
    sc.render.bake.target = "VERTEX_COLORS"

    obj = build(d, folder, sky, glow)
    rocks = bpy.data.objects.get("rubble")
    lj = os.path.join(folder, "lights.json")
    fitted = json.load(open(lj)) if os.path.exists(lj) else None
    lamps = add_lights(fitted["lights"]) if fitted else []
    add_sun()
    for o, e in lamps:
        o.data.energy = e * scale * arg("--power", 1.0, float)
    off = arg("--glow-offset", 0.015, float)
    cards = add_glow_cards(d, folder, glow, off) if off > 0 else None
    set_glow(obj, glow, cards)

    me = rocks.data
    col = me.color_attributes.new("light", "FLOAT_COLOR", "POINT")
    me.color_attributes.active_color = col
    me.color_attributes.render_color_index = me.color_attributes.active_color_index
    for o in bpy.data.objects:
        o.select_set(o == rocks)
    bpy.context.view_layer.objects.active = rocks
    bpy.context.view_layer.update()
    t0 = time.time()
    bpy.ops.object.bake(type="DIFFUSE", pass_filter={"DIRECT", "INDIRECT"})
    c = np.empty(len(me.vertices) * 4, dtype=np.float32)
    col.data.foreach_get("color", c)
    light = c.reshape(-1, 4)[:, :3]
    log("%d corners baked in %.0f s; mean %.3f, max %.2f" % (len(light), time.time() - t0, float(light.mean()), float(light.max())))

    P = z["pos"].astype(np.float32)
    N = z["nrm"].astype(np.float32)
    order = np.argsort(S, kind="stable")
    ids = np.unique(S)
    with open(out, "wb") as f:
        f.write(b"JKLR")
        f.write(struct.pack("<3I", 2, len(d["verts"]), len(ids)))
        for si in ids:
            sel = order[S[order] == si]
            rec = np.concatenate([P[T[sel]], UV[sel], light[T[sel]], N[T[sel]]], 2).astype("<f4")   # T x 3 x 11
            f.write(struct.pack("<2I", int(si), len(sel)))
            f.write(rec.tobytes())
    log("SIDECAR %s, %.1f MB" % (os.path.relpath(out, ROOT), os.path.getsize(out) / 1e6))


try:
    main()
except Exception:
    import traceback
    traceback.print_exc()
    print("RUBBLE: FAILED")
