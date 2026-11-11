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

# JA bone -> JK node (by name). Fingers, face bones and twist bones fold into their limb.
BONE_MAP = {
    "model_root": "k_hip", "pelvis": "k_hip", "Motion": "k_hip", "lower_lumbar": "k_hip",
    "upper_lumbar": "k_torso", "thoracic": "k_torso", "lclavical": "k_torso", "rclavical": "k_torso",
    "cervical": "k_neck",
    "cranium": "k_head", "face": "k_head", "jaw": "k_head", "ceyebrow": "k_head", "leye": "k_head",
    "reye": "k_head", "lblip2": "k_head", "rblip2": "k_head", "ltlip2": "k_head", "rtlip2": "k_head",
    "lfemurYZ": "k_lthigh", "lfemurX": "k_lthigh", "ltibia": "k_lcalf", "ltalus": "k_lfoot", "ltail": "k_lthigh",
    "rfemurYZ": "k_rthigh", "rfemurX": "k_rthigh", "rtibia": "k_rcalf", "rtalus": "k_rfoot", "rtail": "k_rthigh",
    "lhumerus": "k_lshouldr", "lhumerusX": "k_lshouldr", "lradius": "k_lforearm", "lradiusX": "k_lforearm",
    "lhand": "k_lhand", "lhang_tag_bone": "k_lhand",
    "rhumerus": "k_rshouldr", "rhumerusX": "k_rshouldr", "rradius": "k_rforearm", "rradiusX": "k_rforearm",
    "rhand": "k_rhand", "rhang_tag_bone": "k_rhand",
}
for side in "lr":
    for d in ("d1", "d2", "d3", "d4", "d5"):
        for j in ("j1", "j2", "j3"):
            BONE_MAP["%s_%s_%s" % (side, d, j)] = "k_%shand" % side

# Bones snapped onto JK joints, parents first: bone -> (JK node, child bone aimed at the next joint)
SNAP = [
    ("lfemurYZ", "k_lthigh", "ltibia"), ("ltibia", "k_lcalf", "ltalus"), ("ltalus", "k_lfoot", None),
    ("rfemurYZ", "k_rthigh", "rtibia"), ("rtibia", "k_rcalf", "rtalus"), ("rtalus", "k_rfoot", None),
    ("lhumerus", "k_lshouldr", "lradius"), ("lradius", "k_lforearm", "lhand"), ("lhand", "k_lhand", None),
    ("rhumerus", "k_rshouldr", "rradius"), ("rradius", "k_rforearm", "rhand"), ("rhand", "k_rhand", None),
    ("cervical", "k_neck", "cranium"), ("cranium", "k_head", None),
]


def log(*a):
    print("JA2JK:", *a, flush=True)


def visible_lod0():
    root0 = bpy.data.objects.get("model_root_0")
    return [o for o in root0.children_recursive
            if o.type == "MESH" and "_cap_" not in o.name.lower() and "_off" not in o.name.lower()
            and not o.name.startswith("*")  # Ghoul2 tags (bolt points), not geometry
            and not o.hide_render and not o.hide_get()]


SKIN_MAP = {}


def load_skin(base, glm, skin):
    """surface name -> image path from model_<skin>.skin (.tga entries fall back to .jpg/.png)."""
    path = os.path.join(os.path.dirname(glm), "model_%s.skin" % skin)
    if not os.path.exists(path):
        return
    for line in open(path, encoding="latin1"):
        if "," not in line:
            continue
        surf, tex = [x.strip() for x in line.split(",", 1)]
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
    os.makedirs(out_dir, exist_ok=True)
    addon_utils.enable("jediacademy", default_set=True)

    # JK skeleton: joint (rotation centre) of every node at rest, in model space
    jk = jk3do.load(template)
    rest = jk3do.rest_matrices(jk)
    node_idx = {nd.name.lower(): i for i, nd in enumerate(jk.nodes)}
    joint = {nd.name.lower(): Vector((rest[i] @ [-nd.pivot[0], -nd.pivot[1], -nd.pivot[2], 1])[:3])
             for i, nd in enumerate(jk.nodes)}
    zs = []
    for k, me in enumerate(jk.geosets[0]):
        ns = [i for i, nd in enumerate(jk.nodes) if nd.mesh == k]
        if ns:
            zs += [(rest[ns[0]] @ [v[0], v[1], v[2], 1])[2] for v in me.verts]
    jk_lo, jk_hi = min(zs), max(zs)

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
    s = (jk_hi - jk_lo) / (ja_hi - ja_lo)
    pelvis = arm.matrix_world @ arm.data.bones["pelvis"].head_local
    rot = Matrix.Rotation(math.pi, 4, "Z")
    p2 = rot @ pelvis
    hip = joint["k_hip"]
    G = Matrix.Translation((hip.x - p2.x * s, hip.y - p2.y * s, jk_lo - ja_lo * s)) @ Matrix.Scale(s, 4) @ rot
    tops = [o for o in bpy.data.objects if o.parent is None]
    for o in tops:
        o.matrix_world = G @ o.matrix_world
    bpy.context.view_layer.update()
    log("scale %.5f, JA height %.3f -> JK %.4f" % (s, ja_hi - ja_lo, jk_hi - jk_lo))

    # Snap limbs onto JK joints (armature space), parents first. Connected bones are pinned to
    # their parent's tip and can't be moved, so disconnect them first (rest pose unchanged).
    bpy.context.view_layer.objects.active = arm
    bpy.ops.object.mode_set(mode="EDIT")
    for eb in arm.data.edit_bones:
        eb.use_connect = False
    bpy.ops.object.mode_set(mode="POSE")
    Ainv = arm.matrix_world.inverted()
    for bone, node, child in SNAP:
        pb = arm.pose.bones.get(bone)
        if not pb:
            continue
        h = Ainv @ joint[node]
        M = pb.matrix.copy()
        R = M.to_3x3()
        if child:
            cur = arm.pose.bones[child].head - pb.head
            nxt = [c for b, c2, _ in SNAP for c in [c2] if b == child][0]
            tgt = (Ainv @ joint[nxt]) - h
            R = cur.rotation_difference(tgt).to_matrix() @ R
        pb.matrix = Matrix.Translation(h) @ R.to_4x4()
        bpy.context.view_layer.update()
    errs = {b: ((arm.matrix_world @ arm.pose.bones[b].head) - joint[n]).length for b, n, _ in SNAP if b in arm.pose.bones}
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
                    acc[node_idx[jkn.lower()]] = acc.get(node_idx[jkn.lower()], 0) + g.weight
            ws = sorted(acc.items(), key=lambda kv: -kv[1])[:4] or [(node_idx["k_hip"], 1.0)]
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
    out.radius = max(Vector(v[:3]).length for v in out.verts)

    jk.materials = mats
    jk.geosets = [[out]]
    for nd in jk.nodes:
        nd.mesh = -1
    jk.nodes[0].mesh = 0
    json.dump({"textures": textures, "uv_fraction": True}, open(os.path.join(out_dir, "textures.json"), "w"), indent=1)
    jk3do.save(jk, os.path.join(out_dir, name + ".3do"), "%s converted from %s (%s skin) by tools/blender/ja_to_jk.py" % (name, glm, skin))
    jk3do.write_skin(os.path.join(out_dir, name + ".skn"), [(0, 0, weights)])
    log("wrote %s: %d vertices, %d faces, %d materials" % (name, len(out.verts), len(out.faces), len(mats)))
    if do_render:
        render(os.path.join(out_dir, name + "_preview.png"), meshes)


try:
    main()
except Exception:
    import traceback
    traceback.print_exc()
    log("FAILED")
