"""Export Jedi Academy Kyle's forearms and hands, fingers curled into a grip, for JK's
first-person weapon models (tools/make_fp_hands.py).

    Blender -b --python tools/blender/ja_fp_arms.py -- GLM BASEPATH SKIN OUT.json [--render PNG] [--subdiv N]

--subdiv N rounds the sleeves (arm and cuff surfaces) with N levels of subdivision.

For each side (r, l): vertices (JA world space, after posing), normals, faces with per-corner
UVs (0-1, v down) and image, a role per vertex weight (shoulder / forearm / hand), and the key
points used to fit the hand onto a JK view model: wrist, fingertip centroid, thumb tip, elbow.
"""
import json
import math
import os
import sys

import bpy
import addon_utils
from mathutils import Matrix, Vector

ROLE = {"humerus": "shoulder", "humerusX": "shoulder", "clavical": "shoulder",
        "radius": "forearm", "radiusX": "forearm", "hand": "hand", "hang_tag_bone": "hand"}
SURFACES = ("arm", "hand", "hand_wrist")
CURL = {"d1": (0.85, 0.8), "d2": (1.55, 1.6), "d3": (1.55, 1.6), "d4": (1.6, 1.65), "d5": (1.6, 1.65)}  # radians per joint: a closed fist


def log(*a):
    print("FPARMS:", *a, flush=True)


def role_of(group, side):
    g = group[1:] if group.startswith(side) and not group.startswith(side + "_") else group
    if group.startswith(side + "_d"):
        return "hand"
    return ROLE.get(g)


def main():
    argv = sys.argv[sys.argv.index("--") + 1:]
    glm, base, skin, out = argv[:4]
    render = argv[argv.index("--render") + 1] if "--render" in argv else None
    subdiv = int(argv[argv.index("--subdiv") + 1]) if "--subdiv" in argv else 0
    addon_utils.enable("jediacademy", default_set=True)
    for o in list(bpy.data.objects):
        bpy.data.objects.remove(o, do_unlink=True)
    skin_map = {}
    sp = os.path.join(os.path.dirname(glm), "model_%s.skin" % skin)
    for line in open(sp, encoding="latin1"):
        if "," in line:
            s, t = [x.strip() for x in line.split(",", 1)]
            stem = os.path.splitext(os.path.join(base, t))[0]
            for ext in (".jpg", ".png", ".tga"):
                if os.path.exists(stem + ext):
                    skin_map[s.lower()] = stem + ext
                    break
    bpy.ops.import_scene.glm(filepath=glm, skin=skin, basepath=base, guessTextures=False)
    arm = [o for o in bpy.data.objects if o.type == "ARMATURE"][0]
    A = arm.matrix_world
    if subdiv:
        # Round the low-poly sleeves: Catmull-Clark on the rest mesh, applied under the armature
        # so weights and UVs are interpolated onto the new vertices.
        root0 = bpy.data.objects.get("model_root_0")
        for o in root0.children_recursive:
            base_name = o.name.lower().rsplit("_", 1)[0]
            if o.type != "MESH" or base_name not in [s + "_arm" for s in "rl"] + [s + "_hand_wrist" for s in "rl"]:
                continue
            bpy.context.view_layer.objects.active = o
            mod = o.modifiers.new("round", "SUBSURF")
            mod.levels = mod.render_levels = subdiv
            mod.uv_smooth = "PRESERVE_BOUNDARIES"
            bpy.ops.object.modifier_move_to_index(modifier=mod.name, index=0)
            bpy.ops.object.modifier_apply(modifier=mod.name)
            for p in o.data.polygons:
                p.use_smooth = True
            log("rounded", o.name, len(o.data.vertices), "verts")
    bpy.context.view_layer.objects.active = arm
    bpy.ops.object.mode_set(mode="EDIT")
    for eb in arm.data.edit_bones:
        eb.use_connect = False
    bpy.ops.object.mode_set(mode="POSE")

    result = {}
    for side in "rl":
        hand = arm.pose.bones[side + "hand"]
        radius = arm.pose.bones[side + "radius"]
        hand_w = A @ hand.head
        # palm faces the body (x towards 0) in Academy's A-pose; fingers hang down the hand bone
        fingers = [b for b in arm.pose.bones if b.name.startswith(side + "_d") and b.name.endswith("_j1")]
        tip_dir = sum(((A @ b.head) - hand_w for b in fingers), Vector()).normalized()
        palm = Vector((-1.0 if hand_w.x > 0 else 1.0, 0.0, 0.0))
        palm = (palm - tip_dir * palm.dot(tip_dir)).normalized()
        axis = tip_dir.cross(palm).normalized()
        for b in fingers:
            d = b.name.split("_")[1]
            a1, a2 = CURL.get(d, (1.2, 1.3))
            for joint, ang in (("_j1", a1), ("_j2", a2)):
                pb = arm.pose.bones.get(side + "_" + d + joint)
                if not pb:
                    continue
                h = A @ pb.head
                ax = axis if d != "d1" else (axis + tip_dir * 0.6).normalized()  # the thumb folds across
                R = Matrix.Translation(h) @ Matrix.Rotation(ang, 4, ax) @ Matrix.Translation(-h)
                pb.matrix = A.inverted() @ R @ A @ pb.matrix
                bpy.context.view_layer.update()
        bpy.context.view_layer.update()

        dg = bpy.context.evaluated_depsgraph_get()
        root0 = bpy.data.objects.get("model_root_0")
        surfs = [o for o in root0.children_recursive if o.type == "MESH" and not o.name.startswith("*")
                 and o.name.lower().rsplit("_", 1)[0] in [side + "_" + s for s in SURFACES]]
        verts, normals, roles, faces, uvs, images = [], [], [], [], [], []
        tips, thumbs = [], []
        for o in surfs:
            img = skin_map.get(o.name.lower().rsplit("_", 1)[0])
            me = o.evaluated_get(dg).to_mesh()
            mw = o.matrix_world
            nm = mw.to_3x3().inverted().transposed()
            groups = {g.index: g.name for g in o.vertex_groups}
            v0 = len(verts)
            for v, vo in zip(me.vertices, o.data.vertices):
                p = mw @ v.co
                verts.append(list(p))
                normals.append(list((nm @ v.normal).normalized()))
                acc = {}
                for g in vo.groups:
                    r = role_of(groups.get(g.group, ""), side)
                    if r and g.weight > 0:
                        acc[r] = acc.get(r, 0) + g.weight
                tot = sum(acc.values()) or 1.0
                roles.append({k: w / tot for k, w in acc.items()} or {"hand": 1.0})
                gnames = [groups.get(g.group, "") for g in vo.groups if g.weight > 0.5]
                if any(n.startswith(side + "_d") and n.endswith("_j2") and not n.startswith(side + "_d1") for n in gnames):
                    tips.append(p)
                if any(n == side + "_d1_j2" for n in gnames):
                    thumbs.append(p)
            uvl = me.uv_layers.active
            for poly in me.polygons:
                faces.append([me.loops[li].vertex_index + v0 for li in poly.loop_indices])
                uvs.append([[uvl.data[li].uv[0], 1.0 - uvl.data[li].uv[1]] for li in poly.loop_indices])
                images.append(img)
            o.evaluated_get(dg).to_mesh_clear()
        def centroid(ps):
            return list(sum((Vector(p) for p in ps), Vector()) / len(ps)) if ps else None
        result[side] = {"verts": verts, "normals": normals, "roles": roles, "faces": faces, "uvs": uvs,
                        "images": images, "wrist": list(hand_w), "elbow": list(A @ radius.head),
                        "tips": centroid(tips), "thumb": centroid(thumbs)}
        log(side, "surfaces", [o.name for o in surfs], "verts", len(verts), "faces", len(faces),
            "tips", len(tips), "thumb", len(thumbs))
        if render and side == "r":
            sc = bpy.context.scene
            for o in bpy.data.objects:
                if o.type == "MESH":
                    o.hide_render = o not in surfs
            cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
            sc.collection.objects.link(cam)
            sc.camera = cam
            c = hand_w + tip_dir * 0.3
            cam.location = c + palm * -1.6 + Vector((0, -0.6, 0.2))
            cam.rotation_euler = (c - cam.location).to_track_quat("-Z", "Y").to_euler()
            sc.render.engine = "BLENDER_WORKBENCH"
            sc.display.shading.color_type = "TEXTURE"
            sc.render.resolution_x, sc.render.resolution_y = 500, 500
            sc.render.filepath = render
            bpy.ops.render.render(write_still=True)
    json.dump(result, open(out, "w"))


try:
    main()
except Exception:
    import traceback
    traceback.print_exc()
    log("FAILED")
