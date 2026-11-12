"""Bake the light volume: how much light there is in the air, everywhere (atmosphere step 1).

    Blender -b --factory-startup --python tools/blender/volume_bake.py -- hd/lightbake/<level> \
        [--samples N] [--glow S] [--power P] [--bounces N] [--sky R,G,B]

Reads the points from tools/lightbake/volume_points.py and bakes the light arriving at each from
all directions, in the light map's scene (same lights, glow, sky; the calibration is the one
tools/blender/probe_bake.py saved in probes.json, so run that first). Each point is a tiny
octahedron whose six corners face +-X, +-Y, +-Z; Cycles bakes the light into its vertex colours
and the six are averaged. The volumetric fog takes its colour and brightness from this, so air
glows where light is and stays dark where it isn't.

Writes hd/jkgm/lightmaps/<level>.jklv (little endian):
    char[4] "JKLV", u32 version (1), u32 world vertex count (checked against the level),
    then two grids, fine and coarse, each:
        f32 spacing, u32 count N, N x { i16 x, y, z (grid index; world = index x spacing);
                                        f16 r, g, b (light, the light map's units) }
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

R = 0.003   # octahedron radius


def log(*a):
    print("VOLUME:", *a, flush=True)


def octahedra(P):
    """Mesh of one small octahedron per point, smooth shaded (corner normals = the six axes)."""
    n = len(P)
    ax = np.array([[1, 0, 0], [-1, 0, 0], [0, 1, 0], [0, -1, 0], [0, 0, 1], [0, 0, -1]], float)
    verts = (P[:, None, :] + ax[None] * R).reshape(-1, 3)
    tris = []
    for sx in (0, 1):
        for sy in (2, 3):
            for sz in (4, 5):
                t = (sx, sy, sz)
                neg = (sx == 1) + (sy == 3) + (sz == 5)
                tris.append(t if neg % 2 == 0 else (sx, sz, sy))
    tris = np.array(tris)
    faces = (np.arange(n)[:, None, None] * 6 + tris[None]).reshape(-1, 3)
    me = bpy.data.meshes.new("volume")
    me.vertices.add(len(verts))
    me.vertices.foreach_set("co", verts.astype(np.float32).ravel())
    me.loops.add(len(faces) * 3)
    me.loops.foreach_set("vertex_index", faces.astype(np.int32).ravel())
    me.polygons.add(len(faces))
    me.polygons.foreach_set("loop_start", np.arange(0, len(faces) * 3, 3, dtype=np.int32))
    me.polygons.foreach_set("use_smooth", np.ones(len(faces), dtype=bool))
    me.update()
    me.validate()
    col = me.color_attributes.new("vol", "FLOAT_COLOR", "POINT")
    me.color_attributes.active_color = col
    me.color_attributes.render_color_index = me.color_attributes.active_color_index
    m = bpy.data.materials.new("vol")
    m.use_nodes = True
    m.node_tree.nodes["Principled BSDF"].inputs["Base Color"].default_value = (1, 1, 1, 1)
    me.materials.append(m)
    obj = bpy.data.objects.new("volume", me)
    bpy.context.scene.collection.objects.link(obj)
    return obj


def main():
    folder = os.path.abspath(sys.argv[sys.argv.index("--") + 1])
    level = os.path.basename(folder.rstrip("/"))
    d = json.load(open(os.path.join(folder, "level.json")))
    pj = os.path.join(folder, "probes.json")
    if not os.path.exists(pj):
        raise RuntimeError("no probes.json: run tools/blender/probe_bake.py first (it saves the calibration)")
    scale = json.load(open(pj))["scale"]
    vp = np.load(os.path.join(folder, "volume_points.npz"))
    grids = [("fine", vp["fine"].astype(np.int32), float(vp["fine_spacing"])),
             ("coarse", vp["coarse"].astype(np.int32), float(vp["coarse_spacing"]))]
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
    lj = os.path.join(folder, "lights.json")
    fitted = json.load(open(lj)) if os.path.exists(lj) else None
    lamps = add_lights(fitted["lights"]) if fitted else []
    add_sun()
    for o, e in lamps:
        o.data.energy = e * scale * arg("--power", 1.0, float)
    off = arg("--glow-offset", 0.015, float)
    cards = add_glow_cards(d, folder, glow, off) if off > 0 else None
    set_glow(obj, glow, cards)

    out_dir = os.path.join(ROOT, "hd", "jkgm", "lightmaps")
    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, level + ".jklv")
    blobs = []
    for name, G, sp in grids:
        if not len(G):
            blobs.append((sp, G, np.zeros((0, 3), np.float32)))
            continue
        t0 = time.time()
        vobj = octahedra(G * sp)
        for o in bpy.data.objects:
            o.select_set(o == vobj)
        bpy.context.view_layer.objects.active = vobj
        bpy.context.view_layer.update()
        bpy.ops.object.bake(type="DIFFUSE", pass_filter={"DIRECT", "INDIRECT"})
        c = np.empty(len(G) * 6 * 4, dtype=np.float32)
        vobj.data.color_attributes["vol"].data.foreach_get("color", c)
        light = c.reshape(len(G), 6, 4)[:, :, :3].mean(1)
        log("%s: %d points at %.2f baked in %.0f s; mean %.3f, max %.2f" % (
            name, len(G), sp, time.time() - t0, float(light.mean()), float(light.max())))
        blobs.append((sp, G, light))
        bpy.data.objects.remove(vobj, do_unlink=True)
    with open(out, "wb") as f:
        f.write(b"JKLV")
        f.write(struct.pack("<2I", 1, len(d["verts"])))
        for sp, G, light in blobs:
            f.write(struct.pack("<fI", sp, len(G)))
            rec = np.zeros(len(G), dtype=[("i", "<i2", 3), ("c", "<f2", 3)])
            rec["i"] = G
            rec["c"] = np.clip(light, 0, 60000)
            f.write(rec.tobytes())
    log("SIDECAR %s, %.1f MB" % (os.path.relpath(out, ROOT), os.path.getsize(out) / 1e6))


try:
    main()
except Exception:
    import traceback
    traceback.print_exc()
    print("VOLUME: FAILED")
