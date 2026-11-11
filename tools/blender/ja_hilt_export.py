"""Export a Jedi Academy lightsaber hilt (.glm) as JSON for tools/make_hilt.py.

    Blender -b --python tools/blender/ja_hilt_export.py -- HILT.glm BASEPATH OUT.json

JSON: verts (hilt-local, Blender units), faces [[v, ...]], uvs per face corner (0-1, v down),
face image paths, and the emitter point (the *blade1 tag) and long axis.
"""
import json
import os
import sys

import bpy
import addon_utils
from mathutils import Vector


def main():
    argv = sys.argv[sys.argv.index("--") + 1:]
    glm, base, out = argv[:3]
    addon_utils.enable("jediacademy", default_set=True)
    for o in list(bpy.data.objects):
        bpy.data.objects.remove(o, do_unlink=True)
    bpy.ops.import_scene.glm(filepath=glm, skin="", basepath=base, guessTextures=True)
    root0 = bpy.data.objects.get("model_root_0")
    objs = [o for o in root0.children_recursive if o.type == "MESH" and not o.name.startswith("*")
            and "_off" not in o.name.lower() and not o.hide_render]
    tag = [o for o in root0.children_recursive if o.type == "MESH" and o.name.lower().startswith("*blade1")]
    emitter = None
    if tag:
        t = tag[0]
        emitter = sum((t.matrix_world @ v.co for v in t.data.vertices), Vector()) / len(t.data.vertices)
    verts, faces, uvs, images = [], [], [], []
    for o in objs:
        img = None
        for slot in o.material_slots:
            if slot.material and slot.material.use_nodes:
                for n in slot.material.node_tree.nodes:
                    if n.type == "TEX_IMAGE" and n.image:
                        img = bpy.path.abspath(n.image.filepath)
        if img:
            stem = os.path.splitext(img)[0]
            for ext in (".jpg", ".png", ".tga"):
                if os.path.exists(stem + ext):
                    img = stem + ext
                    break
        me = o.data
        uvl = me.uv_layers.active
        v0 = len(verts)
        verts += [list(o.matrix_world @ v.co) for v in me.vertices]
        for p in me.polygons:
            faces.append([me.loops[li].vertex_index + v0 for li in p.loop_indices])
            uvs.append([[uvl.data[li].uv[0], 1.0 - uvl.data[li].uv[1]] if uvl else [0, 0] for li in p.loop_indices])
            images.append(img)
    json.dump({"verts": verts, "faces": faces, "uvs": uvs, "images": images,
               "emitter": list(emitter) if emitter else None}, open(out, "w"))
    print("HILT: %d verts, %d faces, emitter %s" % (len(verts), len(faces), emitter))


try:
    main()
except Exception:
    import traceback
    traceback.print_exc()
    print("HILT: FAILED")
