"""Bake light probes for characters and props (light bake step 4).

    Blender -b --factory-startup --python tools/blender/probe_bake.py -- hd/lightbake/<level> \
        [--samples N] [--glow S] [--power P] [--bounces N] [--sky R,G,B] [--spacing S]

Level faces get their light from the light map; models (Kyle, enemies, props) can't, because
they move. This bakes the same light at points where models can be: a grid over every floor
(--spacing, default 0.15 JK units, about 1.5 m) in two layers: just above the floor (0.03 units,
so feet and shins get the light of floor strips and pools) and chest height (0.09 units; both lower
under a low ceiling). The game blends the two by each vertex's height. Each probe is an "ambient cube": the light arriving from +X, -X, +Y, -Y, +Z and -Z,
in colour. The game blends the nearest probes and lights each model vertex from the side its
normal faces, so a model standing by a red strip turns red on that side.

The scene, lights and calibration are the light map bake's (tools/blender/lightbake.py, same
options), so probe light and light map light match. Each probe is a 4 cm cube of six outward
quads baked into a small atlas (3x3 texels per quad, averaged).

Writes hd/jkgm/lightmaps/<level>.jklp (little endian):
    char[4] "JKLP", u32 version (2), u32 world vertex count (checked against the level),
    u32 probe count N, f32 spacing,
    N x { f32 x, y, z; i32 sector; i32 layer (0 floor, 1 chest); f32 rgb[6][3] (+X, -X, +Y, -Y, +Z, -Z) }
"""
import json
import os
import struct
import sys
import time

import bpy
import numpy as np
from mathutils import Vector

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

# lightbake.py's scene building, without running its main().
_src = open(os.path.join(HERE, "lightbake.py")).read()
_cut = _src.rfind("\ntry:\n    main()")
_lb = {"__name__": "lightbake", "__file__": os.path.join(HERE, "lightbake.py")}
exec(compile(_src[:_cut] if _cut > 0 else _src, "lightbake.py", "exec"), _lb)
arg, build, add_lights, add_glow_cards, set_glow, add_bake_target, sample = (
    _lb[k] for k in ("arg", "build", "add_lights", "add_glow_cards", "set_glow", "add_bake_target", "sample"))

LAYERS = (0.03, 0.09)  # probe heights above the floor: shins, chest (Kyle is 0.18 tall)
CHEST = LAYERS[1]
CUBE = 0.004          # probe cube size
CELL = 3              # texels per quad side
# Faces: (normal, t1, t2) with t1 x t2 = normal, so the quad faces outwards.
FACES = [((1, 0, 0), (0, 1, 0), (0, 0, 1)), ((-1, 0, 0), (0, 0, 1), (0, 1, 0)),
         ((0, 1, 0), (0, 0, 1), (1, 0, 0)), ((0, -1, 0), (1, 0, 0), (0, 0, 1)),
         ((0, 0, 1), (1, 0, 0), (0, 1, 0)), ((0, 0, -1), (0, 1, 0), (1, 0, 0))]


def log(*a):
    print("PROBES:", *a, flush=True)


def place(d, spacing):
    """Grid points over every floor (surface facing up), at each layer's height."""
    V = np.array(d["verts"])
    sector_of = {}
    for si, sec in enumerate(d["sectors"]):
        first, count = sec["surfaces"]   # JKL: first surface, number of surfaces
        for s in range(first, first + count):
            sector_of[s] = si
    dg = bpy.context.evaluated_depsgraph_get()
    sc = bpy.context.scene
    seen, out = set(), []
    for s in d["surfaces"]:
        n = np.array(s["normal"])
        if s["sky"] or "lm" not in s or n[2] < 0.65:
            continue
        P = V[s["v"]]
        lo, hi = P[:, :2].min(0), P[:, :2].max(0)
        xs = (np.arange(np.floor(lo[0] / spacing), np.ceil(hi[0] / spacing) + 1) + 0.5) * spacing
        ys = (np.arange(np.floor(lo[1] / spacing), np.ceil(hi[1] / spacing) + 1) + 0.5) * spacing
        gx, gy = np.meshgrid(xs, ys)
        pts = np.stack([gx.ravel(), gy.ravel()], 1)
        # Inside the polygon (convex, either winding) in the XY projection.
        e0, e1 = P[:, :2], np.roll(P[:, :2], -1, 0)
        cross = (e1[:, 0] - e0[:, 0])[None] * (pts[:, 1:2] - e0[:, 1][None]) - \
                (e1[:, 1] - e0[:, 1])[None] * (pts[:, 0:1] - e0[:, 0][None])
        inside = np.all(cross >= -1e-9, 1) | np.all(cross <= 1e-9, 1)
        for x, y in pts[inside]:
            z = P[0, 2] - (n[0] * (x - P[0, 0]) + n[1] * (y - P[0, 1])) / n[2]
            key = (int(round(x / spacing * 2)), int(round(y / spacing * 2)), int(round(z * 50)))
            if key in seen:
                continue
            seen.add(key)
            o = Vector((x, y, z + 0.002))
            hit, loc, *_ = sc.ray_cast(dg, o, Vector((0, 0, 1)), distance=2 * CHEST)
            ceil = (loc.z - o.z) if hit else 1e9
            if ceil < 0.03:
                continue
            for layer, lh in enumerate(LAYERS):
                h = min(lh, ceil * (0.25 if layer == 0 else 0.5))
                out.append(((x, y, z + h), sector_of.get(s["index"], -1), layer))
    return out


def probe_object(probes, img):
    W = img.size[0] // CELL
    verts, faces, uvs = [], [], []
    for pi, (p, _, _) in enumerate(probes):
        c = Vector(p)
        for fi, (n, t1, t2) in enumerate(FACES):
            n, t1, t2 = Vector(n), Vector(t1), Vector(t2)
            k = pi * 6 + fi
            u0, v0 = (k % W) * CELL, (k // W) * CELL
            base = len(verts)
            for a, b in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
                verts.append(c + (n + t1 * a + t2 * b) * (CUBE / 2))
                uvs.append(((u0 + (a + 1) / 2 * CELL) / img.size[0], (v0 + (b + 1) / 2 * CELL) / img.size[1]))
            faces.append([base, base + 1, base + 2, base + 3])
    me = bpy.data.meshes.new("probes")
    me.from_pydata([tuple(v) for v in verts], [], faces)
    me.update()
    uvl = me.uv_layers.new(name="uv")
    for i, loop in enumerate(me.loops):
        uvl.data[i].uv = uvs[loop.vertex_index]
    m = bpy.data.materials.new("probe")
    m.use_nodes = True
    nt = m.node_tree
    nt.nodes.clear()
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    dif = nt.nodes.new("ShaderNodeBsdfDiffuse")
    dif.inputs["Color"].default_value = (1, 1, 1, 1)
    nt.links.new(dif.outputs[0], out.inputs[0])
    t = nt.nodes.new("ShaderNodeTexImage")
    t.image = img
    nt.nodes.active = t
    me.materials.append(m)
    obj = bpy.data.objects.new("probes", me)
    bpy.context.scene.collection.objects.link(obj)
    return obj


def main():
    folder = os.path.abspath(sys.argv[sys.argv.index("--") + 1])
    level = os.path.basename(folder.rstrip("/"))
    d = json.load(open(os.path.join(folder, "level.json")))
    samples = arg("--samples", 512, int)
    glow = arg("--glow", 14.0, float)
    sky = tuple(float(x) for x in arg("--sky", "0.02,0.025,0.035").split(","))
    spacing = arg("--spacing", 0.15, float)
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
    sc.render.bake.use_pass_direct = True
    sc.render.bake.use_pass_indirect = True
    sc.render.bake.use_pass_color = False

    obj = build(d, folder, sky, glow)
    lj = os.path.join(folder, "lights.json")
    fitted = json.load(open(lj)) if os.path.exists(lj) else None
    lamps = add_lights(fitted["lights"]) if fitted else []
    # Place probes before the glow cards exist: the ceiling check would hit the card floating
    # just above a lit floor strip and skip exactly the floors that matter most.
    bpy.context.view_layer.update()
    t0 = time.time()
    probes = place(d, spacing)
    log("%d probes at %.2f spacing (%.0f s)" % (len(probes), spacing, time.time() - t0))
    if not probes:
        raise RuntimeError("no floors found")
    off = arg("--glow-offset", 0.015, float)
    cards = add_glow_cards(d, folder, glow, off) if off > 0 else None
    bpy.context.view_layer.update()

    # Calibration, as lightbake.py does it (fewer samples: only the average matters).
    scale = 1.0
    if lamps:
        W, H = d["atlas"]
        cal = bpy.data.images.new("calibration", W, H, float_buffer=True, alpha=False)
        add_bake_target(obj, cal)
        set_glow(obj, 0.0, cards)
        for o in bpy.data.objects:
            o.select_set(o == obj)
        bpy.context.view_layer.objects.active = obj
        sc.cycles.samples = arg("--calib-samples", 64, int)
        t0 = time.time()
        bpy.ops.object.bake(type="DIFFUSE", pass_filter={"DIRECT", "INDIRECT"}, margin=0)
        pix, target = [], []
        for srf in d["surfaces"]:
            if srf["sky"] or not srf["intensities"] or "lm" not in srf:
                continue
            lm = srf["lm"]
            pix.append((int(lm["x"] + lm["w"] / 2), int(lm["y"] + lm["h"] / 2)))
            target.append(min(float(np.mean(srf["intensities"])), 2.0))
        baked, model = sample(cal, pix), np.array(target)
        lit = model > 0.05
        scale = float(model[lit].mean() / max(baked[lit].mean(), 1e-6)) if lit.sum() > 10 else 1.0
        scale = min(max(scale, 0.05), 20.0)
        log("calibration: light power x%.3f (%.0f s)" % (scale, time.time() - t0))
        for o, e in lamps:
            o.data.energy = e * scale * arg("--power", 1.0, float)
            o.data.update_tag()
    set_glow(obj, glow, cards)

    # Probe bake.
    per_row = 512
    rows = (len(probes) * 6 + per_row - 1) // per_row
    img = bpy.data.images.new("probes", per_row * CELL, rows * CELL, float_buffer=True, alpha=False)
    pobj = probe_object(probes, img)
    for o in bpy.data.objects:
        o.select_set(o == pobj)
    bpy.context.view_layer.objects.active = pobj
    bpy.context.view_layer.update()
    sc.cycles.samples = arg("--samples", 512, int)
    t0 = time.time()
    bpy.ops.object.bake(type="DIFFUSE", pass_filter={"DIRECT", "INDIRECT"}, margin=0)
    px = np.empty(img.size[0] * img.size[1] * 4, dtype=np.float32)
    img.pixels.foreach_get(px)
    px = px.reshape(img.size[1], img.size[0], 4)[:, :, :3]
    cells = px.reshape(rows, CELL, per_row, CELL, 3).mean(axis=(1, 3)).reshape(-1, 3)[:len(probes) * 6]
    cube = cells.reshape(len(probes), 6, 3)
    log("baked in %.0f s; mean %.3f, max %.2f" % (time.time() - t0, float(cube.mean()), float(cube.max())))

    out_dir = os.path.join(ROOT, "hd", "jkgm", "lightmaps")
    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, level + ".jklp")
    with open(out, "wb") as f:
        f.write(b"JKLP")
        f.write(struct.pack("<3If", 2, len(d["verts"]), len(probes), spacing))
        for (p, sec, layer), c in zip(probes, cube):
            f.write(struct.pack("<3f2i", *p, sec, layer))
            f.write(np.clip(c, 0, 60000).astype("<f4").tobytes())
    json.dump({"scale": scale, "probes": len(probes), "spacing": spacing}, open(os.path.join(folder, "probes.json"), "w"))
    log("SIDECAR %s, %d probes, %.1f MB" % (os.path.relpath(out, ROOT), len(probes), os.path.getsize(out) / 1e6))


try:
    main()
except Exception:
    import traceback
    traceback.print_exc()
    print("PROBES: FAILED")
