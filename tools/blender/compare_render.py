"""Render a share-ready side-by-side of an original JK model and its remastered version.

Run inside Blender 5.x with the blender-sith add-on (bl_ext.user_default.sith) installed:

    Blender -b --python tools/blender/compare_render.py -- \
        ORIG.3do ORIG_MAT_DIR NEW.3do NEW_MAT_DIR HD_PACK_DIR CMP OUT_PREFIX

Writes OUT_PREFIX_orig_<view>.png and OUT_PREFIX_new_<view>.png for a front and a
three-quarter view, same camera, lights and framing. tools/make_side_by_side.py lays
them out with labels. Original textures use nearest filtering (as the 1997 game);
remastered materials are swapped for their full-resolution HD pack images.
"""
import math
import os
import sys

import bpy
import addon_utils
from mathutils import Vector


def log(*a):
    print("CMP:", *a, flush=True)


def clear():
    for o in list(bpy.data.objects):
        bpy.data.objects.remove(o, do_unlink=True)


def import_model(path, mat_dirs, cmp):
    clear()
    bpy.ops.import_scene.sith_3do(filepath=path, mat_dir=mat_dirs[0], cmp_file=cmp, import_radius_objects=False)
    return [o for o in bpy.data.objects if o.type == "MESH"]


def set_images(objs, hd_dir, nearest):
    for o in objs:
        for slot in o.material_slots:
            mat = slot.material
            if not mat or not mat.use_nodes:
                continue
            for n in mat.node_tree.nodes:
                if n.type != "TEX_IMAGE":
                    continue
                base = mat.name.split(".mat")[0].lower()
                png = os.path.join(hd_dir, base + ".png") if hd_dir else None
                if png and os.path.exists(png):
                    n.image = bpy.data.images.load(png, check_existing=True)
                    n.interpolation = "Linear"
                elif nearest:
                    n.interpolation = "Closest"


def setup_scene(objs):
    sc = bpy.context.scene
    for eng in ("BLENDER_EEVEE_NEXT", "BLENDER_EEVEE"):
        try:
            sc.render.engine = eng
            break
        except TypeError:
            continue
    sc.render.resolution_x, sc.render.resolution_y = 900, 1200
    sc.render.film_transparent = False
    sc.view_settings.view_transform = "AgX" if "AgX" in [v.identifier for v in sc.view_settings.bl_rna.properties["view_transform"].enum_items] else "Standard"
    world = bpy.data.worlds.new("w") if not sc.world else sc.world
    sc.world = world
    world.use_nodes = True
    bg = world.node_tree.nodes.get("Background")
    bg.inputs[0].default_value = (0.004, 0.0045, 0.006, 1)
    bg.inputs[1].default_value = 1.0

    pts = [o.matrix_world @ Vector(c) for o in objs for c in o.bound_box]
    lo = Vector([min(p[i] for p in pts) for i in range(3)])
    hi = Vector([max(p[i] for p in pts) for i in range(3)])
    ctr, size = (lo + hi) / 2, (hi - lo)
    h = size.z
    for name, d, energy, color in (("key", (0.8, 1.2, 0.9), 105, (1.0, 0.95, 0.88)),
                                   ("fill", (-1.3, 0.8, 0.3), 32, (0.75, 0.82, 1.0)),
                                   ("rim", (0.2, -1.5, 1.2), 100, (0.9, 0.95, 1.0))):
        ld = bpy.data.lights.new(name, "AREA")
        ld.energy = energy * h * h * 4
        ld.size = h * 1.5
        ld.color = color
        lo_ = bpy.data.objects.new(name, ld)
        sc.collection.objects.link(lo_)
        lo_.location = ctr + Vector(d) * h * 2.0
        lo_.rotation_euler = (ctr - lo_.location).to_track_quat("-Z", "Y").to_euler()
    cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
    cam.data.lens = 85
    cam.data.clip_start, cam.data.clip_end = h * 0.01, h * 100
    sc.collection.objects.link(cam)
    sc.camera = cam
    return ctr, h


def shoot(prefix, ctr, h, frame_ctr, frame_h):
    sc = bpy.context.scene
    cam = sc.camera
    for tag, ang in (("front", 0), ("threequarter", 35)):
        a = math.radians(ang)
        dist = frame_h * 2.7
        # JK models face +Y: the camera sits in front (+Y) looking back at them
        cam.location = frame_ctr + Vector((math.sin(a) * dist, math.cos(a) * dist, frame_h * 0.08))
        cam.rotation_euler = (frame_ctr - cam.location).to_track_quat("-Z", "Y").to_euler()
        sc.render.filepath = "%s_%s.png" % (prefix, tag)
        bpy.ops.render.render(write_still=True)


def main():
    argv = sys.argv[sys.argv.index("--") + 1:]
    orig, orig_mats, new, new_mats, hd_dir, cmp, out = argv[:7]
    addon_utils.enable("bl_ext.user_default.sith", default_set=True)
    os.makedirs(os.path.dirname(out), exist_ok=True)

    # Frame both on the original's box so sizes compare honestly.
    objs = import_model(orig, [orig_mats], cmp)
    set_images(objs, None, nearest=True)
    ctr, h = setup_scene(objs)
    shoot(out + "_orig", ctr, h, ctr, h)
    frame = (ctr.copy(), h)

    bpy.ops.wm.read_factory_settings(use_empty=True)
    addon_utils.enable("bl_ext.user_default.sith", default_set=True)
    objs = import_model(new, [new_mats, orig_mats], cmp)
    # kept original parts (weapons) find their MATs in the original folder
    set_images(objs, hd_dir, nearest=True)
    setup_scene(objs)
    shoot(out + "_new", frame[0], frame[1], frame[0], frame[1])
    log("done", out)


try:
    main()
except Exception:
    import traceback
    traceback.print_exc()
    print("CMP: FAILED", flush=True)
