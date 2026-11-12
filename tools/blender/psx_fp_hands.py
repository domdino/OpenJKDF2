"""Export the hands of the free "PSX First Person Arms" rig, fingers curled into a grip, for
tools/make_fp_hands.py (which grafts them onto Kyle's Jedi Academy sleeves).

    Blender -b --python tools/blender/psx_fp_hands.py -- arms_rig.blend OUT.json [--render PNG]

For each side (r, l): vertices (world space, posed), normals, faces with per-corner UVs (0-1,
v down), a role per vertex weight (hand / forearm), and key points: wrist (hand bone head),
fingertip centroid (middle of the last finger segments) and thumb tip (middle of thumb.03).
Hand and forearm are exported (not the upper arm); make_fp_hands.py trims the forearm to the
length that tucks into Kyle's sleeve.
"""
import json
import sys

import bpy
from mathutils import Vector

FINGER_CURL = {"01": 1.2, "02": 1.1, "03": 1.1}  # radians about each bone's local X: a closed fist
THUMB_CURL = {"01": 0.5, "02": 0.5, "03": 0.5}
HAND_PREFIX = ("hand.", "palm.", "f_", "thumb.")


def log(*a):
    print("PSXHANDS:", *a, flush=True)


def main():
    argv = sys.argv[sys.argv.index("--") + 1:]
    blend, out = argv[:2]
    render = argv[argv.index("--render") + 1] if "--render" in argv else None
    bpy.ops.wm.open_mainfile(filepath=blend)
    rig = bpy.data.objects["ArmsRig"]
    mesh = bpy.data.objects["ArmsMesh"]
    for pb in rig.pose.bones:
        n = pb.name
        if n.startswith(("f_", "thumb.")):
            seg = n.split(".")[1]
            ang = (THUMB_CURL if n.startswith("thumb") else FINGER_CURL).get(seg, 0.0)
            pb.rotation_mode = "XYZ"
            pb.rotation_euler = (ang, 0, 0)
    bpy.context.view_layer.update()
    R = rig.matrix_world
    dg = bpy.context.evaluated_depsgraph_get()
    me = mesh.evaluated_get(dg).to_mesh()
    mw = mesh.matrix_world
    nm = mw.to_3x3().inverted().transposed()
    groups = {g.index: g.name for g in mesh.vertex_groups}
    uvl = me.uv_layers.active
    result = {}
    for side, S in (("r", ".R"), ("l", ".L")):
        def mid(name):
            pb = rig.pose.bones[name + S]
            return (R @ pb.head + R @ pb.tail) / 2
        wrist = R @ rig.pose.bones["hand" + S].head
        tips = sum((mid("f_%s.03" % f) for f in ("index", "middle", "ring", "pinky")), Vector()) / 4
        thumb = mid("thumb.03")
        roles, handw = {}, {}
        for v, vo in zip(me.vertices, mesh.data.vertices):
            acc = {}
            for g in vo.groups:
                name = groups.get(g.group, "")
                if not name.endswith(S) or g.weight <= 0:
                    continue
                role = "hand" if name.startswith(HAND_PREFIX) else "forearm" if name.startswith("forearm") else "shoulder"
                acc[role] = acc.get(role, 0) + g.weight
            tot = sum(acc.values())
            if tot:
                roles[v.index] = {k: w / tot for k, w in acc.items()}
                handw[v.index] = acc.get("hand", 0) / tot
        # hand and forearm (make_fp_hands.py trims the forearm to fit inside the sleeve)
        keep = [p for p in me.polygons if all(v in roles and roles[v].get("shoulder", 0) < 0.5 for v in p.vertices)]
        remap, verts, normals, vroles = {}, [], [], []
        faces, uvs = [], []
        for p in keep:
            for v in p.vertices:
                if v not in remap:
                    remap[v] = len(verts)
                    verts.append(list(mw @ me.vertices[v].co))
                    normals.append(list((nm @ me.vertices[v].normal).normalized()))
                    vroles.append(roles[v])
            faces.append([remap[me.loops[li].vertex_index] for li in p.loop_indices])
            uvs.append([[uvl.data[li].uv[0], 1.0 - uvl.data[li].uv[1]] for li in p.loop_indices])
        result[side] = {"verts": verts, "normals": normals, "roles": vroles, "faces": faces, "uvs": uvs,
                        "wrist": list(wrist), "tips": list(tips), "thumb": list(thumb),
                        "elbow": list(R @ rig.pose.bones["forearm" + S].head)}
        log(side, "verts", len(verts), "faces", len(faces))
    mesh.evaluated_get(dg).to_mesh_clear()
    json.dump(result, open(out, "w"))
    if render:
        sc = bpy.context.scene
        for o in bpy.data.objects:
            o.hide_render = o != mesh
        cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
        sc.collection.objects.link(cam)
        sc.camera = cam
        c = (Vector(result["r"]["wrist"]) + Vector(result["l"]["wrist"])) / 2
        cam.location = c + Vector((0, -2.2, 0.6))
        cam.rotation_euler = (c - cam.location).to_track_quat("-Z", "Y").to_euler()
        sc.render.engine = "BLENDER_WORKBENCH"
        sc.display.shading.color_type = "TEXTURE"
        sc.render.resolution_x, sc.render.resolution_y, sc.render.resolution_percentage = 800, 500, 100
        sc.render.filepath = render
        bpy.ops.render.render(write_still=True)


try:
    main()
except Exception:
    import traceback
    traceback.print_exc()
    log("FAILED")
