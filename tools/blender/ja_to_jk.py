"""Convert a Jedi Academy / Jedi Outcast character (.glm on the _humanoid skeleton) into a
skinned Jedi Knight model driven by JK's own skeleton and KEY animations.

Run inside Blender (5.x) with the Jedi Academy add-on (mrwonko) enabled:

    Blender -b --python tools/blender/ja_to_jk.py -- \
        GLM BASEPATH SKIN JK_TEMPLATE.3do OUT_DIR NAME [--render]

  GLM          .../base/models/players/kyle/model.glm
  BASEPATH     the JA "base" folder (for textures)
  SKIN         skin name, e.g. default
  JK_TEMPLATE  the original JK model whose skeleton/animations to use (3do/ky.3do)
  OUT_DIR      gets NAME.3do, NAME.skn, textures.json (material -> source image), previews
  NAME         output model name, e.g. ky

Steps: turn the character to face JK's +Y, scale it to the JK model's height, pose every
limb so its joints sit exactly on the JK skeleton's joints (the KEY files set joint
positions, so the mesh must fit JK's proportions), bake that pose, map the JA bone
weights onto JK nodes, and write one skinned mesh on the root node.
"""
import json
import math
import os
import sys

import bpy
import addon_utils
from mathutils import Matrix, Vector

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import jk3do  # noqa: E402

# JA bone -> JK skeleton role. A role is matched to the JK model's node whose name ends with it
# (k_lthigh, stlthigh, ...). Fingers, face bones and twist bones fold into their limb.
BONE_MAP = {
    "model_root": "hip", "pelvis": "hip", "Motion": "hip", "lower_lumbar": "hip",
    "upper_lumbar": "torso", "thoracic": "torso", "lclavical": "torso", "rclavical": "torso",
    "cervical": "neck",
    "cranium": "head", "face": "head", "jaw": "head", "ceyebrow": "head", "leye": "head",
    "reye": "head", "lblip2": "head", "rblip2": "head", "ltlip2": "head", "rtlip2": "head",
    "lfemurYZ": "lthigh", "lfemurX": "lthigh", "ltibia": "lcalf", "ltalus": "lfoot", "ltail": "lthigh",
    "rfemurYZ": "rthigh", "rfemurX": "rthigh", "rtibia": "rcalf", "rtalus": "rfoot", "rtail": "rthigh",
    "lhumerus": "lshouldr", "lhumerusX": "lshouldr", "lradius": "lforearm", "lradiusX": "lforearm",
    "lhand": "lhand", "lhang_tag_bone": "lhand",
    "rhumerus": "rshouldr", "rhumerusX": "rshouldr", "rradius": "rforearm", "rradiusX": "rforearm",
    "rhand": "rhand", "rhang_tag_bone": "rhand",
}
for side in "lr":
    for d in ("d1", "d2", "d3", "d4", "d5"):
        for j in ("j1", "j2", "j3"):
            BONE_MAP["%s_%s_%s" % (side, d, j)] = "%shand" % side

# Bones snapped onto JK joints, parents first: bone -> (role, child bone aimed at the next joint)
SNAP = [
    ("lfemurYZ", "lthigh", "ltibia"), ("ltibia", "lcalf", "ltalus"), ("ltalus", "lfoot", None),
    ("rfemurYZ", "rthigh", "rtibia"), ("rtibia", "rcalf", "rtalus"), ("rtalus", "rfoot", None),
    ("lhumerus", "lshouldr", "lradius"), ("lradius", "lforearm", "lhand"), ("lhand", "lhand", None),
    ("rhumerus", "rshouldr", "rradius"), ("rradius", "rforearm", "rhand"), ("rhand", "rhand", None),
]
# The neck and head aren't pinned to JK's joints (that floats the head off the collar on models
# whose rest pose leans, like the stormtrooper); they ride on the fitted upper body.


# Other names JK models use for a role (Kyle: k_lshouldr; Trandoshan/Gran/Weequay: larm, l4arm, lleg).
ROLE_ALIASES = {
    "hip": ["hip", "hips"], "torso": ["torso", "chest"],
    "lshouldr": ["lshouldr", "larm"], "rshouldr": ["rshouldr", "rarm"],
    "lforearm": ["lforearm", "l4arm"], "rforearm": ["rforearm", "r4arm"],
    "lthigh": ["lthigh", "lleg"], "rthigh": ["rthigh", "rleg"],
}
# A role the JK model lacks falls back (stormtrooper: no neck; robed Weequay: one "legs" node).
ROLE_FALLBACK = {"neck": ["torso"], "lthigh": ["legs", "hip"], "rthigh": ["legs", "hip"],
                 "lcalf": ["legs", "lthigh"], "rcalf": ["legs", "rthigh"]}
# Rigid JK parts kept next to the skin unless --keep says otherwise: the weapons.
AUTO_KEEP = ("gun", "weapon")


def resolve_roles(jk):
    """role -> node index for this JK model."""
    names = [nd.name.lower() for nd in jk.nodes]
    out = {}
    for role in set(BONE_MAP.values()):
        for alias in ROLE_ALIASES.get(role, [role]):
            exact = [i for i, n in enumerate(names) if n == alias]
            cands = exact or [i for i, n in enumerate(names) if n.endswith(alias) and len(n) - len(alias) <= 3]
            if cands:
                out[role] = min(cands, key=lambda i: len(names[i]))
                break
    # Trust the hierarchy over names: a foot is the child of that side's calf and a hand the child
    # of that side's forearm (JK's Rodian has lfoot/rfoot swapped: lfoot hangs off rcalf).
    for side in "lr":
        for end, limb in (("foot", "calf"), ("hand", "forearm")):
            pr = out.get(side + limb)
            if pr is None:
                continue
            kids = [i for i, nd in enumerate(jk.nodes) if nd.parent == pr]
            want = [i for i in kids if names[i].endswith(end)]
            if want and out.get(side + end) not in want:
                out[side + end] = want[0]
    for _ in range(2):
        for role, fbs in ROLE_FALLBACK.items():
            if role in out:
                continue
            for fb in fbs:
                if fb in out:
                    out[role] = out[fb]
                    break
                exact = [i for i, n in enumerate(names) if n == fb]
                if exact:
                    out[role] = exact[0]
                    break
    return out


def frame3(left, right, base):
    """Orthonormal frame from a left/right joint pair and a base point below their midpoint."""
    x = (left - right).normalized()
    up = (left + right) / 2 - base
    up = (up - x * up.dot(x)).normalized()
    return Matrix((x, x.cross(up) * -1, up)).transposed()  # columns: x, forward, up


def align_block(arm, bone, ja_l, ja_r, ja_base, jk_l, jk_r, jk_base, move=True):
    """Rotate a pose bone (and everything below it) about its head so the JA joint pair and base
    line up with JK's directions, then shift it so the pair's midpoint lands on JK's."""
    pb = arm.pose.bones[bone]
    A = arm.matrix_world
    Ainv = A.inverted()
    head_w = A @ pb.head
    R = frame3(jk_l, jk_r, jk_base) @ frame3(ja_l, ja_r, ja_base).transposed()
    Rw = Matrix.Translation(head_w) @ R.to_4x4() @ Matrix.Translation(-head_w)
    pb.matrix = Ainv @ Rw @ A @ pb.matrix
    bpy.context.view_layer.update()
    if move:
        mid_ja = (Rw @ ja_l + Rw @ ja_r) / 2
        delta = (jk_l + jk_r) / 2 - mid_ja
        pb.matrix = Ainv @ Matrix.Translation(delta) @ A @ pb.matrix
        bpy.context.view_layer.update()


def log(*a):
    print("JA2JK:", *a, flush=True)


def visible_lod0():
    root0 = bpy.data.objects.get("model_root_0")
    return [o for o in root0.children_recursive
            if o.type == "MESH" and "_cap_" not in o.name.lower() and "_off" not in o.name.lower()
            and not o.name.startswith("*")  # Ghoul2 tags (bolt points), not geometry
            and o.name.split(".")[0].lower() not in SKIN_OFF and o.name.lower().rsplit("_", 1)[0] not in SKIN_OFF
            and not o.hide_render and not o.hide_get()]


SKIN_MAP = {}
SKIN_OFF = set()


def load_skin(base, glm, skin):
    """surface name -> image path from model_<skin>.skin (.tga entries fall back to .jpg/.png)."""
    path = os.path.join(os.path.dirname(glm), "model_%s.skin" % skin)
    if not os.path.exists(path):
        return
    for line in open(path, encoding="latin1"):
        if "," not in line:
            continue
        surf, tex = [x.strip() for x in line.split(",", 1)]
        if tex.lower().endswith("off"):  # "*off": surface hidden in this skin
            SKIN_OFF.add(surf.lower())
            continue
        stem = os.path.splitext(os.path.join(base, tex))[0]
        for ext in (".jpg", ".png", ".tga", ".JPG", ".PNG", ".TGA"):
            if os.path.exists(stem + ext):
                SKIN_MAP[surf.lower()] = stem + ext
                break


def surface_name(o):
    for key in ("g2_prop_name", "name"):
        try:
            v = o.g2_prop_name if key == "g2_prop_name" else None
        except AttributeError:
            v = None
        if v:
            return v.lower()
    return o.name.split(".")[0].lower()


def object_image(o):
    sn = surface_name(o)
    if sn in SKIN_MAP:
        return SKIN_MAP[sn]
    for slot in o.material_slots:
        mat = slot.material
        if mat and mat.use_nodes:
            for n in mat.node_tree.nodes:
                if n.type == "TEX_IMAGE" and n.image:
                    return bpy.path.abspath(n.image.filepath)
    return None


def render(path, objs):
    sc = bpy.context.scene
    for o in bpy.data.objects:  # show exactly what was exported
        if o.type == "MESH":
            o.hide_render = o not in objs
    pts = [o.matrix_world @ Vector(c) for o in objs for c in o.bound_box]
    lo = Vector([min(p[i] for p in pts) for i in range(3)])
    hi = Vector([max(p[i] for p in pts) for i in range(3)])
    ctr, size = (lo + hi) / 2, max(hi - lo)
    cam = bpy.data.objects.get("jkcam") or bpy.data.objects.new("jkcam", bpy.data.cameras.new("jkcam"))
    if cam.name not in sc.collection.objects:
        sc.collection.objects.link(cam)
    cam.data.clip_start, cam.data.clip_end = size * 0.01, size * 100
    sc.camera = cam
    sc.render.engine = "BLENDER_WORKBENCH"
    sc.display.shading.color_type = "TEXTURE"
    sc.render.resolution_x, sc.render.resolution_y = 500, 700
    for i, ang in enumerate((180, 90)):  # front (JK faces +Y), side
        a = math.radians(ang)
        cam.location = ctr + Vector((math.sin(a), -math.cos(a), 0.05)) * size * 1.9
        cam.rotation_euler = (ctr - cam.location).to_track_quat("-Z", "Y").to_euler()
        sc.render.filepath = path.replace(".png", "_%d.png" % i)
        bpy.ops.render.render(write_still=True)


def main():
    argv = sys.argv[sys.argv.index("--") + 1:]
    glm, base, skin, template, out_dir, name = argv[:6]
    do_render = "--render" in argv
    # --keep stgun,...: keep the JK model's own rigid meshes on these nodes (e.g. the trooper's rifle)
    keep = argv[argv.index("--keep") + 1].lower().split(",") if "--keep" in argv else None
    os.makedirs(out_dir, exist_ok=True)
    addon_utils.enable("jediacademy", default_set=True)

    # JK skeleton: joint (rotation centre) of every node at rest, in model space
    jk = jk3do.load(template)
    rest = jk3do.rest_matrices(jk)
    roles = resolve_roles(jk)
    native = {r for r, i in roles.items() if any(jk.nodes[i].name.lower().endswith(a) for a in ROLE_ALIASES.get(r, [r]))}
    if keep is None:
        keep = [nd.name.lower() for nd in jk.nodes if any(k in nd.name.lower() for k in AUTO_KEEP)]
    missing = {"hip", "torso", "head", "lhand", "rhand", "lfoot", "rfoot"} - set(roles)
    if missing:
        raise RuntimeError("JK model lacks nodes for %s" % sorted(missing))
    log("JK nodes for roles:", {r: jk.nodes[i].name for r, i in sorted(roles.items())})
    joint_of = [Vector((rest[i] @ [-nd.pivot[0], -nd.pivot[1], -nd.pivot[2], 1])[:3]) for i, nd in enumerate(jk.nodes)]
    joint = {r: joint_of[i] for r, i in roles.items()}
    snap = [(b, r, c) for b, r, c in SNAP if r in native]
    zs = []
    for k, me in enumerate(jk.geosets[0]):
        ns = [i for i, nd in enumerate(jk.nodes) if nd.mesh == k]
        if ns:
            zs += [(rest[ns[0]] @ [v[0], v[1], v[2], 1])[2] for v in me.verts]
    jk_lo, jk_hi = min(zs), max(zs)
    # --scale-ref <standing .3do>: size from a standing model of the same character. A model
    # baked lying down (the Nar Shaddaa bar drunk) is only as tall as it is thick, which made the
    # character half size and then stretched thin to reach the joints.
    jk_height = jk_hi - jk_lo
    if "--scale-ref" in argv:
        ref = jk3do.load(argv[argv.index("--scale-ref") + 1])
        rrest = jk3do.rest_matrices(ref)
        rz = []
        for k, me in enumerate(ref.geosets[0]):
            ns = [i for i, nd in enumerate(ref.nodes) if nd.mesh == k]
            if ns:
                rz += [(rrest[ns[0]] @ [v[0], v[1], v[2], 1])[2] for v in me.verts]
        jk_height = max(rz) - min(rz)
        log("scale from %s: height %.4f (the target is %.4f tall)" % (os.path.basename(argv[argv.index("--scale-ref") + 1]), jk_height, jk_hi - jk_lo))

    for o in list(bpy.data.objects):  # factory scene: cube, camera, light
        bpy.data.objects.remove(o, do_unlink=True)
    load_skin(base, glm, skin)
    bpy.ops.import_scene.glm(filepath=glm, skin=skin, basepath=base, guessTextures=False)
    arm = [o for o in bpy.data.objects if o.type == "ARMATURE"][0]
    meshes = visible_lod0()
    log("imported", len(meshes), "visible surfaces")

    # Global fit: face +Y, scale to JK height, stand on JK's floor, centred on JK's hip
    pts = [o.matrix_world @ v.co for o in meshes for v in o.data.vertices]
    ja_lo, ja_hi = min(p.z for p in pts), max(p.z for p in pts)
    s = jk_height / (ja_hi - ja_lo)
    pelvis = arm.matrix_world @ arm.data.bones["pelvis"].head_local
    rot = Matrix.Rotation(math.pi, 4, "Z")
    p2 = rot @ pelvis
    hip = joint["hip"]
    G = Matrix.Translation((hip.x - p2.x * s, hip.y - p2.y * s, jk_lo - ja_lo * s)) @ Matrix.Scale(s, 4) @ rot
    tops = [o for o in bpy.data.objects if o.parent is None]
    for o in tops:
        o.matrix_world = G @ o.matrix_world
    bpy.context.view_layer.update()
    log("scale %.5f, JA height %.3f -> JK %.4f" % (s, ja_hi - ja_lo, jk_height))

    # Snap limbs onto JK joints (armature space), parents first. Connected bones are pinned to
    # their parent's tip and can't be moved, so disconnect them first (rest pose unchanged).
    bpy.context.view_layer.objects.active = arm
    bpy.ops.object.mode_set(mode="EDIT")
    for eb in arm.data.edit_bones:
        eb.use_connect = False
    bpy.ops.object.mode_set(mode="POSE")
    Ainv = arm.matrix_world.inverted()
    pre = {b: ((arm.matrix_world @ arm.pose.bones[b].head) - joint[n]) for b, n, _ in SNAP
           if b in arm.pose.bones and n in roles}
    log("pre-snap offsets (x,y,z):", {b: tuple(round(c, 4) for c in v) for b, v in pre.items()})
    # Posture first: hips, then the whole upper body (shoulder line, lean, twist), as blocks.
    W = lambda b: arm.matrix_world @ arm.pose.bones[b].head
    if {"lthigh", "rthigh", "torso"} <= native:
        # base point straight below the hip midpoint: only the hip line's turn is matched
        drop = Vector((0, 0, 0.05))
        align_block(arm, "pelvis", W("lfemurYZ"), W("rfemurYZ"), (W("lfemurYZ") + W("rfemurYZ")) / 2 - drop,
                    joint["lthigh"], joint["rthigh"], (joint["lthigh"] + joint["rthigh"]) / 2 - drop, move=False)
    if {"lshouldr", "rshouldr", "torso"} <= native:
        align_block(arm, "upper_lumbar", W("lhumerus"), W("rhumerus"), W("upper_lumbar"),
                    joint["lshouldr"], joint["rshouldr"], joint["torso"])
    post = {b: round(((arm.matrix_world @ arm.pose.bones[b].head) - joint[n]).length, 4)
            for b, n, _ in SNAP if b in arm.pose.bones and n in roles}
    log("offsets after posture fit:", post)
    for bone, node, child in snap:
        pb = arm.pose.bones.get(bone)
        if not pb:
            continue
        h = Ainv @ joint[node]
        M = pb.matrix.copy()
        R = M.to_3x3()
        if child:
            cur = arm.pose.bones[child].head - pb.head
            nxt = [c2 for b, c2, _ in snap if b == child]
            if not nxt:
                continue
            nxt = nxt[0]
            tgt = (Ainv @ joint[nxt]) - h
            R = cur.rotation_difference(tgt).to_matrix() @ R
        pb.matrix = Matrix.Translation(h) @ R.to_4x4()
        bpy.context.view_layer.update()
    # Keep the head facing straight ahead and upright (as the JK originals do at rest), undoing the
    # upper body's lean/twist for the head only. (JK head nodes' own rest rotations are compensated
    # by their meshes, so they can't be used as a target.)
    if "head" in native and "cranium" in arm.pose.bones:
        pb = arm.pose.bones["cranium"]
        before = pb.matrix.to_3x3()
        rest_rot = arm.data.bones["cranium"].matrix_local.to_3x3()
        pb.matrix = Matrix.Translation(pb.head) @ rest_rot.to_4x4()
        bpy.context.view_layer.update()
        log("head turned %.1f degrees back to face forward" % math.degrees((before.transposed() @ rest_rot).to_quaternion().angle))
    errs = {b: ((arm.matrix_world @ arm.pose.bones[b].head) - joint[n]).length for b, n, _ in snap if b in arm.pose.bones}
    log("max joint error after snapping: %.6f" % max(errs.values()), {b: round(e, 5) for b, e in errs.items() if e > 1e-4})

    bpy.ops.object.mode_set(mode="OBJECT")

    # Bake: evaluated (posed) meshes, in JK model space
    dg = bpy.context.evaluated_depsgraph_get()
    out = jk3do.Mesh()
    out.name = "skin"
    weights, mats, textures = [], [], {}
    for o in meshes:
        img = object_image(o)
        if not img:
            log("no texture for surface", o.name)
        key = os.path.splitext(os.path.basename(img))[0].lower() if img else "none"
        mat_name = ("%s_%s" % (name, key))[:28] + ".mat"
        if mat_name not in mats:
            mats.append(mat_name)
            textures[mat_name] = img
        mi = mats.index(mat_name)
        oe = o.evaluated_get(dg)
        me = oe.to_mesh()
        mw = o.matrix_world
        nrm_m = mw.to_3x3().inverted().transposed()
        v0, t0 = len(out.verts), len(out.uvs)
        groups = {g.index: g.name for g in o.vertex_groups}
        for v, vo in zip(me.vertices, o.data.vertices):
            p = mw @ v.co
            out.verts.append((p.x, p.y, p.z, 0.0))
            n = (nrm_m @ v.normal).normalized()
            out.normals.append((n.x, n.y, n.z))
            acc = {}
            for g in vo.groups:
                jkn = BONE_MAP.get(groups.get(g.group, ""))
                if jkn and g.weight > 0:
                    acc[roles[jkn]] = acc.get(roles[jkn], 0) + g.weight
            ws = sorted(acc.items(), key=lambda kv: -kv[1])[:4] or [(roles["hip"], 1.0)]
            tot = sum(w for _, w in ws)
            weights.append([(n_, w / tot) for n_, w in ws])
        uvl = me.uv_layers.active
        me.calc_loop_triangles()
        for poly in me.polygons:
            idx = []
            for li in poly.loop_indices:
                uv = uvl.data[li].uv if uvl else (0, 0)
                out.uvs.append((uv[0], 1.0 - uv[1]))  # fraction for now; scaled to texels below
                idx.append((me.loops[li].vertex_index + v0, len(out.uvs) - 1))
            pn = (nrm_m @ poly.normal).normalized()
            out.faces.append({"mat": mi, "type": "0x0000", "geo": 4, "light": 3, "tex": 0,
                              "extralight": 0.0, "idx": idx})
            out.face_normals.append((pn.x, pn.y, pn.z))
        oe.to_mesh_clear()
    # The skin hangs from the root node: express it in that node's rest space.
    import numpy as np
    inv0 = np.linalg.inv(rest[0])
    R0 = inv0[:3, :3]
    out.verts = [tuple((inv0 @ [v[0], v[1], v[2], 1])[:3]) + (v[3],) for v in out.verts]
    out.normals = [tuple(R0 @ n) for n in out.normals]
    out.face_normals = [tuple(R0 @ n) for n in out.face_normals]
    out.radius = max(Vector(v[:3]).length for v in out.verts)

    # Kept rigid parts (e.g. the rifle) come first, with their original materials.
    geo = []
    old_geo = jk.geosets[0]
    old_mats = jk.materials
    new_mats = []
    kept_nodes = [i for i, nd in enumerate(jk.nodes) if nd.name.lower() in keep and nd.mesh >= 0]
    for i in kept_nodes:
        me = old_geo[jk.nodes[i].mesh]
        for f in me.faces:
            mname = old_mats[f["mat"]]
            if mname not in new_mats:
                new_mats.append(mname)
            f["mat"] = new_mats.index(mname)
        jk.nodes[i].mesh = len(geo)
        geo.append(me)
    offset = len(new_mats)
    for f in out.faces:
        f["mat"] += offset
    jk.materials = new_mats + mats
    skin_idx = len(geo)
    geo.append(out)
    jk.geosets = [geo]
    for i, nd in enumerate(jk.nodes):
        if i not in kept_nodes:
            nd.mesh = -1
    jk.nodes[0].mesh = skin_idx
    if kept_nodes:
        log("kept rigid:", [jk.nodes[i].name for i in kept_nodes])
    json.dump({"textures": textures, "uv_fraction": True, "skin_mesh": skin_idx},
              open(os.path.join(out_dir, "textures.json"), "w"), indent=1)
    jk3do.save(jk, os.path.join(out_dir, name + ".3do"), "%s converted from %s (%s skin) by tools/blender/ja_to_jk.py" % (name, glm, skin))
    jk3do.write_skin(os.path.join(out_dir, name + ".skn"), [(0, skin_idx, weights)])
    log("wrote %s: %d vertices, %d faces, %d materials" % (name, len(out.verts), len(out.faces), len(mats)))
    if do_render:
        render(os.path.join(out_dir, name + "_preview.png"), meshes)


try:
    main()
except Exception:
    import traceback
    traceback.print_exc()
    log("FAILED")
