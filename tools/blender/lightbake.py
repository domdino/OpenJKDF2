"""Bake a Jedi Knight level's lighting with Cycles (step 1 of docs/stage2-lightbake-plan.md).

    Blender -b --python tools/blender/lightbake.py -- hd/lightbake/<level> [options]
      --samples N       bake samples (default 128)
      --glow S          emission strength of glowing texels (default 6)
      --sky R,G,B       light from sky surfaces (default 0.02,0.025,0.035)
      --fill F          how much of the 1997 light is kept in the previews (default 0.3)
      --camera "x y z pitch yaw"   JK camera for the previews
      --no-bake         reuse lightmap.exr, only render previews
      --bounces N       diffuse bounces (default 2; fewer = more contrast)
      --soft R          light size for shadow softness, JK units (default 0.01)
      --floor F         share of the fit's constant floor light kept in previews (default 0)
      --power P         extra multiplier on the recovered lights after calibration (default 1)
      --light-exposure E   brightness of the light-only preview (default 1)
      --tag NAME        suffix for the preview files
      --glow-offset D   glowing texels bake as cards D units off their surface, so strips light
                        the wall around them (default 0.015; 0 = flat in the surface)
The raw bake is denoised with Intel Open Image Denoise (lightmap_raw.exr -> lightmap.exr).

Builds the level from tools/lightbake/jkl_export.py's output: every drawn surface, textured,
see-through where its texture is (grates), glowing texels as emitters, sky surfaces emitting
the sky colour. Bakes the light arriving at every surface (direct + bounced, no albedo) into the
light map atlas (UV map "lm"), writes lightmap.exr, and renders three previews from the camera:
  preview_1997.png    textures x the original corner light (the game today)
  preview_baked.png   textures x (baked light + fill x original light), glow on top
  preview_light.png   the baked light alone
"""
import json
import math
import os
import sys

import bpy
from mathutils import Vector


def log(*a):
    print("LIGHTBAKE:", *a, flush=True)


def arg(name, default=None, cast=str):
    argv = sys.argv[sys.argv.index("--") + 1:]
    return cast(argv[argv.index(name) + 1]) if name in argv else default


def image(path, colorspace="sRGB"):
    img = bpy.data.images.load(path, check_existing=True)
    img.colorspace_settings.name = colorspace
    return img


def build(d, folder, sky_rgb, glow_strength):
    verts, faces, uv_tex, uv_lm, orig, mat_index = [], [], [], [], [], []
    W, H = d["atlas"]
    mats = {}
    # Terrain skin (tools/lightbake/terrain_skin.py): skinned faces are baked as their skin, so
    # the light map holds the light and shadow of every lump. --no-skin bakes the flat faces.
    skin, skin_vid, smooth = {}, {}, []
    sp = os.path.join(folder, "skin.npz")
    if os.path.exists(sp) and "--no-skin" not in sys.argv:
        import numpy as np
        z = np.load(sp)
        sP, sT, sUV, sS = z["pos"], z["tris"], z["uv"], z["surf"]
        order = np.argsort(sS, kind="stable")
        bounds = np.searchsorted(sS[order], np.unique(sS), side="left").tolist() + [len(order)]
        for a, b in zip(bounds[:-1], bounds[1:]):
            skin[int(sS[order[a]])] = order[a:b]
        log("terrain skin: %d surfaces, %d triangles" % (len(skin), len(sT)))
    for s in d["surfaces"]:
        key = ("sky" if s["sky"] else s["mat"])
        if key not in mats:
            mats[key] = len(mats)
        if s["index"] in skin and "lm" in s:
            lm = s["lm"]
            o, ua, va = Vector(lm["origin"]), Vector(lm["u"]), Vector(lm["v"])
            dens = lm.get("d", d["density"])
            c = min(float(np.mean(s["intensities"])), 2.0) if s["intensities"] else 1.0
            for t in skin[s["index"]]:
                corners = []
                for k in range(3):
                    vi = int(sT[t, k])
                    if vi not in skin_vid:          # shared, so the skin bakes smooth, not faceted
                        skin_vid[vi] = len(verts)
                        verts.append(tuple(float(x) for x in sP[vi]))
                    corners.append(skin_vid[vi])
                    p = sP[vi]
                    q = Vector(p) - o
                    # Inside the face's own rectangle (the lumps can reach a little past it).
                    x = min(max(lm["x"] + q.dot(ua) * dens, lm["x"] + 0.5), lm["x"] + lm["w"] - 0.5)
                    y = min(max(lm["y"] + q.dot(va) * dens, lm["y"] + 0.5), lm["y"] + lm["h"] - 0.5)
                    uv_lm.append((x / W, y / H))
                    uv_tex.append((float(sUV[t, k, 0]), 1.0 - float(sUV[t, k, 1])))
                    orig.append(c)
                faces.append(corners)
                smooth.append(len(faces) - 1)
                mat_index.append(mats[key])
            continue
        base = len(verts)
        P = [d["verts"][v] for v in s["v"]]
        verts += P
        faces.append(list(range(base, base + len(P))))
        uv_tex += [(u, 1.0 - v) for u, v in s["uv"]]
        if "lm" in s:
            lm = s["lm"]
            o, ua, va = Vector(lm["origin"]), Vector(lm["u"]), Vector(lm["v"])
            dens = lm.get("d", d["density"])   # per surface (huge faces are coarser)
            for p in P:
                q = Vector(p) - o
                uv_lm.append(((lm["x"] + q.dot(ua) * dens) / W, (lm["y"] + q.dot(va) * dens) / H))
        else:
            uv_lm += [(0.0, 0.0)] * len(P)
        orig += [min(i, 2.0) for i in s["intensities"]] if s["intensities"] else [1.0] * len(P)
        mat_index.append(mats[key])
    me = bpy.data.meshes.new("level")
    me.from_pydata(verts, [], faces)
    me.update()
    lt = me.uv_layers.new(name="tex")
    ll = me.uv_layers.new(name="lm")
    for i, loop in enumerate(me.loops):
        lt.data[i].uv = uv_tex[loop.index]
        ll.data[i].uv = uv_lm[loop.index]
    col = me.color_attributes.new("orig", "FLOAT_COLOR", "CORNER")
    for i, loop in enumerate(me.loops):
        c = orig[loop.index] / 2.0
        col.data[i].color = (c, c, c, 1.0)
    for p, mi in zip(me.polygons, mat_index):
        p.material_index = mi
    for i in smooth:
        me.polygons[i].use_smooth = True
    me.uv_layers.active = ll
    obj = bpy.data.objects.new("level", me)
    bpy.context.scene.collection.objects.link(obj)

    # Materials: textured diffuse, see-through by alpha, glowing texels emit.
    names = sorted(mats, key=lambda k: mats[k])
    for key in names:
        m = bpy.data.materials.new(key)
        m.use_nodes = True
        nt = m.node_tree
        nt.nodes.clear()
        out = nt.nodes.new("ShaderNodeOutputMaterial")
        if key == "sky":
            em = nt.nodes.new("ShaderNodeEmission")
            em.inputs["Color"].default_value = (*sky_rgb, 1.0)
            em.inputs["Strength"].default_value = 1.0
            # The sky faces close the level in like a lid; let shadow rays through them so a sun
            # (--sun) outside can shine in. Everything else still sees glowing sky.
            lp = nt.nodes.new("ShaderNodeLightPath")
            tr = nt.nodes.new("ShaderNodeBsdfTransparent")
            mix = nt.nodes.new("ShaderNodeMixShader")
            nt.links.new(lp.outputs["Is Shadow Ray"], mix.inputs[0])
            nt.links.new(em.outputs[0], mix.inputs[1])
            nt.links.new(tr.outputs[0], mix.inputs[2])
            nt.links.new(mix.outputs[0], out.inputs[0])
        else:
            uvn = nt.nodes.new("ShaderNodeUVMap")
            uvn.uv_map = "tex"
            tex = nt.nodes.new("ShaderNodeTexImage")
            tex.name = "albedo"
            tex.image = image(os.path.join(folder, "tex", key + ".png"))
            tex.interpolation = "Closest"
            nt.links.new(uvn.outputs[0], tex.inputs[0])
            dif = nt.nodes.new("ShaderNodeBsdfDiffuse")
            nt.links.new(tex.outputs["Color"], dif.inputs["Color"])
            shader = dif.outputs[0]
            gp = os.path.join(folder, "glow", key + ".png")
            if os.path.exists(gp):
                gt = nt.nodes.new("ShaderNodeTexImage")
                gt.name = "glow"
                gt.image = image(gp)
                gt.interpolation = "Closest"
                nt.links.new(uvn.outputs[0], gt.inputs[0])
                em = nt.nodes.new("ShaderNodeEmission")
                em.inputs["Strength"].default_value = glow_strength
                nt.links.new(gt.outputs["Color"], em.inputs["Color"])
                add = nt.nodes.new("ShaderNodeAddShader")
                nt.links.new(shader, add.inputs[0])
                nt.links.new(em.outputs[0], add.inputs[1])
                shader = add.outputs[0]
            tr = nt.nodes.new("ShaderNodeBsdfTransparent")
            mix = nt.nodes.new("ShaderNodeMixShader")
            nt.links.new(tex.outputs["Alpha"], mix.inputs[0])
            nt.links.new(tr.outputs[0], mix.inputs[1])
            nt.links.new(shader, mix.inputs[2])
            nt.links.new(mix.outputs[0], out.inputs[0])
        obj.data.materials.append(m)
    return obj


def add_glow_cards(d, folder, strength, offset):
    """Glowing texels as thin emitting cards standing `offset` off their surface (see-through
    except where they glow), in their own object so they light the bake but aren't baked into.
    Flat strips painted into a wall can't light that wall; raised a little, like real light
    bars, they spill onto the panels beside them. The surfaces' own emission is turned off."""
    verts, faces, uvs, mats = [], [], [], {}
    for s in d["surfaces"]:
        if s["sky"] or not os.path.exists(os.path.join(folder, "glow", s["mat"] + ".png")):
            continue
        n = Vector(s["normal"])
        base = len(verts)
        verts += [tuple(Vector(d["verts"][v]) + n * offset) for v in s["v"]]
        faces.append(list(range(base, base + len(s["v"]))))
        uvs += [(u, 1.0 - v) for u, v in s["uv"]]
        mats.setdefault(s["mat"], len(mats))
    if not faces:
        return None
    me = bpy.data.meshes.new("glowcards")
    me.from_pydata(verts, [], faces)
    me.update()
    lt = me.uv_layers.new(name="tex")
    for i, loop in enumerate(me.loops):
        lt.data[i].uv = uvs[loop.index]
    order = sorted(mats, key=lambda k: mats[k])
    face_mats = [mats[s["mat"]] for s in d["surfaces"]
                 if not s["sky"] and os.path.exists(os.path.join(folder, "glow", s["mat"] + ".png"))]
    for p, mi in zip(me.polygons, face_mats):
        p.material_index = mi
    obj = bpy.data.objects.new("glowcards", me)
    bpy.context.scene.collection.objects.link(obj)
    for key in order:
        m = bpy.data.materials.new("card_" + key)
        m.use_nodes = True
        nt = m.node_tree
        nt.nodes.clear()
        out = nt.nodes.new("ShaderNodeOutputMaterial")
        uvn = nt.nodes.new("ShaderNodeUVMap")
        uvn.uv_map = "tex"
        gt = nt.nodes.new("ShaderNodeTexImage")
        gt.image = image(os.path.join(folder, "glow", key + ".png"))
        gt.interpolation = "Closest"
        nt.links.new(uvn.outputs[0], gt.inputs[0])
        em = nt.nodes.new("ShaderNodeEmission")
        em.name = "card_emission"
        em.inputs["Strength"].default_value = strength
        nt.links.new(gt.outputs["Color"], em.inputs["Color"])
        # opaque only where it glows
        bw = nt.nodes.new("ShaderNodeRGBToBW")
        nt.links.new(gt.outputs["Color"], bw.inputs[0])
        ramp = nt.nodes.new("ShaderNodeMath")
        ramp.operation = "GREATER_THAN"
        ramp.inputs[1].default_value = 0.02
        nt.links.new(bw.outputs[0], ramp.inputs[0])
        tr = nt.nodes.new("ShaderNodeBsdfTransparent")
        mix = nt.nodes.new("ShaderNodeMixShader")
        nt.links.new(ramp.outputs[0], mix.inputs[0])
        nt.links.new(tr.outputs[0], mix.inputs[1])
        nt.links.new(em.outputs[0], mix.inputs[2])
        nt.links.new(mix.outputs[0], out.inputs[0])
        obj.data.materials.append(m)
    return obj


def add_bake_target(obj, img):
    for m in obj.data.materials:
        nt = m.node_tree
        n = nt.nodes.new("ShaderNodeTexImage")
        n.name = "bake_target"
        n.image = img
        for x in nt.nodes:
            x.select = False
        n.select = True
        nt.nodes.active = n


def preview_materials(obj, lm_img, fill, mode, floor=None):
    """Swap every material for a flat emission of what the game would show."""
    for m in obj.data.materials:
        nt = m.node_tree
        alb = nt.nodes.get("albedo")
        glow = nt.nodes.get("glow")
        out = [n for n in nt.nodes if n.type == "OUTPUT_MATERIAL"][0]
        if alb is None:   # sky: keep the original sky look simple
            continue
        uvl = nt.nodes.new("ShaderNodeUVMap")
        uvl.uv_map = "lm"
        lm = nt.nodes.new("ShaderNodeTexImage")
        lm.image = lm_img
        lm.interpolation = "Linear"
        nt.links.new(uvl.outputs[0], lm.inputs[0])
        attr = nt.nodes.new("ShaderNodeAttribute")
        attr.attribute_name = "orig"
        orig2 = nt.nodes.new("ShaderNodeMath")
        orig2.operation = "MULTIPLY"
        orig2.inputs[1].default_value = 2.0 * (fill if mode == "baked" else 1.0)
        nt.links.new(attr.outputs["Fac"], orig2.inputs[0])
        light = nt.nodes.new("ShaderNodeMix")
        light.data_type = "RGBA"
        light.blend_type = "ADD"
        light.inputs["Factor"].default_value = 1.0
        if mode == "baked" and floor is not None:   # recovered lights: bake + constant floor
            nt.links.new(lm.outputs["Color"], light.inputs[6])
            light.inputs[7].default_value = (floor, floor, floor, 1)
        elif mode == "baked":
            nt.links.new(lm.outputs["Color"], light.inputs[6])
            nt.links.new(orig2.outputs[0], light.inputs[7])
        elif mode == "light":
            nt.links.new(lm.outputs["Color"], light.inputs[6])
            light.inputs[7].default_value = (0, 0, 0, 1)
        else:  # 1997
            light.inputs[6].default_value = (0, 0, 0, 1)
            nt.links.new(orig2.outputs[0], light.inputs[7])
        shade = nt.nodes.new("ShaderNodeMix")
        shade.data_type = "RGBA"
        shade.blend_type = "MULTIPLY"
        shade.inputs["Factor"].default_value = 1.0
        if mode == "light":
            e = arg("--light-exposure", 1.0, float)
            shade.inputs[6].default_value = (e, e, e, 1)
        else:
            nt.links.new(alb.outputs["Color"], shade.inputs[6])
        nt.links.new(light.outputs[2], shade.inputs[7])
        col = shade.outputs[2]
        if glow is not None and mode != "light":
            add = nt.nodes.new("ShaderNodeMix")
            add.data_type = "RGBA"
            add.blend_type = "ADD"
            add.inputs["Factor"].default_value = 1.0
            nt.links.new(col, add.inputs[6])
            nt.links.new(glow.outputs["Color"], add.inputs[7])
            col = add.outputs[2]
        em = nt.nodes.new("ShaderNodeEmission")
        nt.links.new(col, em.inputs["Color"])
        tr = nt.nodes.new("ShaderNodeBsdfTransparent")
        mix = nt.nodes.new("ShaderNodeMixShader")
        nt.links.new(alb.outputs["Alpha"], mix.inputs[0])
        nt.links.new(tr.outputs[0], mix.inputs[1])
        nt.links.new(em.outputs[0], mix.inputs[2])
        nt.links.new(mix.outputs[0], out.inputs[0])


def set_glow(obj, strength, cards=None):
    for m in obj.data.materials:
        for n in m.node_tree.nodes:
            if n.type == "EMISSION" and m.name != "sky":
                n.inputs["Strength"].default_value = 0.0 if cards else strength
    if cards:
        for m in cards.data.materials:
            m.node_tree.nodes["card_emission"].inputs["Strength"].default_value = strength


def add_sun():
    """--sun AZ,EL,STRENGTH[,R,G,B]: a sun (or moon) for outdoor levels, in the direction of the
    level's sky preset (src/Engine/sithSky.c: azimuth from world +X counter-clockwise, elevation
    above the horizon), so baked shadows agree with the sky. Not part of the calibration (that
    matches the recovered designer lights to the 1997 light). Returns (object, strength) or None."""
    spec = arg("--sun")
    if not spec:
        return None
    v = [float(x) for x in spec.split(",")]
    az, el, strength = math.radians(v[0]), math.radians(v[1]), v[2]
    col = v[3:6] if len(v) >= 6 else (1.0, 0.96, 0.88)
    d = Vector((math.cos(el) * math.cos(az), math.cos(el) * math.sin(az), math.sin(el)))
    ld = bpy.data.lights.new("sun", "SUN")
    ld.energy = strength
    ld.color = col
    ld.angle = math.radians(1.5)   # a soft-edged sun disc
    o = bpy.data.objects.new("sun", ld)
    o.rotation_euler = d.to_track_quat("Z", "Y").to_euler()   # a sun shines down its -Z
    bpy.context.scene.collection.objects.link(o)
    log("sun from azimuth %.0f, elevation %.0f, strength %.2f" % (v[0], v[1], strength))
    return o, strength


def add_lights(lights):
    """The fitted designer lights (tools/lightbake/fit_lights.py) as point lights. Relative power
    a * r^2 (a JED light of intensity a and range r gives about a/2 at r/2); calibrated later."""
    objs = []
    for i, L in enumerate(lights):
        ld = bpy.data.lights.new("jed%d" % i, "POINT")
        ld.energy = L["intensity"] * L["range"] ** 2
        ld.shadow_soft_size = arg("--soft", 0.01, float)
        o = bpy.data.objects.new("jed%d" % i, ld)
        o.location = L["pos"]
        bpy.context.scene.collection.objects.link(o)
        objs.append((o, ld.energy))
    return objs


def corner_samples(d):
    """(surface corner points nudged off the surface, light map pixel to sample) for every lit
    corner, in the order fit_lights.py uses."""
    import numpy as np
    V = np.array(d["verts"])
    pts, pix = [], []
    for s in d["surfaces"]:
        if s["sky"] or not s["intensities"] or "lm" not in s:
            continue
        lm = s["lm"]
        n, u, v, o = (np.array(lm[k]) if k in lm else np.array(s["normal"]) for k in ("normal", "u", "v", "origin"))
        cx, cy = lm["x"] + lm["w"] / 2.0, lm["y"] + lm["h"] / 2.0
        for vi in s["v"]:
            q = V[vi] - o
            x = lm["x"] + q @ u * lm.get("d", d["density"])
            y = lm["y"] + q @ v * lm.get("d", d["density"])
            # one texel in from the corner, towards the rectangle's centre
            x += 1.0 if x < cx else -1.0
            y += 1.0 if y < cy else -1.0
            pts.append(V[vi] + n * 0.005)
            pix.append((int(x), int(y)))
    return np.array(pts), pix


def sample(img, pix):
    import numpy as np
    W, H = img.size
    px = np.empty(W * H * 4, dtype=np.float32)
    img.pixels.foreach_get(px)
    px = px.reshape(H, W, 4)
    return np.array([px[min(max(y, 0), H - 1), min(max(x, 0), W - 1), :3].mean() for x, y in pix])


def denoise(src, dst, W, H):
    """Intel Open Image Denoise over the baked atlas, in a throwaway scene (compositor only)."""
    sc = bpy.data.scenes.new("denoise")
    sc.render.engine = "BLENDER_WORKBENCH"
    sc.render.resolution_x, sc.render.resolution_y, sc.render.resolution_percentage = W, H, 100
    sc.render.image_settings.file_format = "OPEN_EXR"
    sc.render.image_settings.color_depth = "16"
    sc.view_settings.view_transform = "Standard"
    sc.render.use_compositing = True
    cam = bpy.data.objects.new("dcam", bpy.data.cameras.new("dcam"))
    sc.collection.objects.link(cam)
    sc.camera = cam
    tree = bpy.data.node_groups.new("denoise", "CompositorNodeTree")
    tree.interface.new_socket("Image", in_out="OUTPUT", socket_type="NodeSocketColor")
    sc.compositing_node_group = tree
    img = bpy.data.images.load(src)
    n_img = tree.nodes.new("CompositorNodeImage")
    n_img.image = img
    n_dn = tree.nodes.new("CompositorNodeDenoise")
    n_out = tree.nodes.new("NodeGroupOutput")
    tree.links.new(n_img.outputs["Image"], n_dn.inputs["Image"])
    tree.links.new(n_dn.outputs["Image"], n_out.inputs[0])
    sc.render.filepath = dst
    bpy.ops.render.render(scene=sc.name, write_still=True)
    bpy.data.scenes.remove(sc)


def main():
    folder = os.path.abspath(sys.argv[sys.argv.index("--") + 1])
    d = json.load(open(os.path.join(folder, "level.json")))
    samples = arg("--samples", 128, int)
    glow = arg("--glow", 6.0, float)
    sky = tuple(float(x) for x in arg("--sky", "0.02,0.025,0.035").split(","))
    fill = arg("--fill", 0.3, float)
    cam = arg("--camera")
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
    sc.cycles.diffuse_bounces = arg("--bounces", 2, int)
    sc.cycles.transparent_max_bounces = 8

    obj = build(d, folder, sky, glow)
    log("built", len(d["surfaces"]), "surfaces,", len(obj.data.materials), "materials")
    lj = os.path.join(folder, "lights.json")
    fitted = json.load(open(lj)) if os.path.exists(lj) and "--no-lights" not in sys.argv else None
    lamps = add_lights(fitted["lights"]) if fitted else []
    sun = add_sun()
    off = arg("--glow-offset", 0.015, float)
    cards = add_glow_cards(d, folder, glow, off) if off > 0 else None
    set_glow(obj, glow, cards)
    if fitted:
        log("%d recovered lights (fit R^2 %.2f, floor %.3f)" % (len(lamps), fitted["r2"], fitted["floor"]))
    W, H = d["atlas"]
    exr = os.path.join(folder, "lightmap.exr")
    if "--no-bake" not in sys.argv:
        img = bpy.data.images.new("lightmap", W, H, float_buffer=True, alpha=False)
        add_bake_target(obj, img)
        bpy.context.view_layer.objects.active = obj
        obj.select_set(True)
        sc.cycles.samples = samples
        sc.render.bake.use_pass_direct = True
        sc.render.bake.use_pass_indirect = True
        sc.render.bake.use_pass_color = False
        sc.render.bake.margin = 2
        import time
        import numpy as np
        if lamps:
            # Calibrate: bake the recovered lights alone and match the 1997 lighting they were
            # fitted to (least squares over every surface corner).
            set_glow(obj, 0.0, cards)
            if sun:
                sun[0].data.energy = 0.0   # the calibration is for the designer lights alone
            bpy.ops.object.bake(type="DIFFUSE", pass_filter={"DIRECT", "INDIRECT"}, margin=2)
            # Match the 1997 brightness at surface centres (mean of the corner values there):
            # corners sit in creases, which a real bake darkens and JED's linear light didn't.
            pix, target = [], []
            for srf in d["surfaces"]:
                if srf["sky"] or not srf["intensities"] or "lm" not in srf:
                    continue
                lm = srf["lm"]
                pix.append((int(lm["x"] + lm["w"] / 2), int(lm["y"] + lm["h"] / 2)))
                target.append(min(float(np.mean(srf["intensities"])), 2.0))
            baked = sample(img, pix)
            model = np.array(target)
            log("calibration bake mean %.4f, 1997 mean %.4f" % (float(baked.mean()), float(model.mean())))
            lit = model > 0.05
            # Match the average, not the median: a real bake concentrates light in pools with a
            # dark majority between them, and matching medians would brighten that majority flat.
            scale = float(model[lit].mean() / max(baked[lit].mean(), 1e-6)) if lit.sum() > 10 else 1.0
            scale = min(max(scale, 0.05), 20.0)
            corr = float(np.corrcoef(model, baked * scale)[0, 1])
            log("calibration: light power x%.3f, correlation with the 1997 light %.2f" % (scale, corr))
            for o, e in lamps:
                o.data.energy = e * scale * arg("--power", 1.0, float)
            set_glow(obj, glow, cards)
            if sun:
                sun[0].data.energy = sun[1]
                sun[0].data.update_tag()
            # Final bake into a fresh image: reading the calibration pixels back pins the
            # first image's buffer, and a second bake into it never reached the saved file.
            img = bpy.data.images.new("lightmap_final", W, H, float_buffer=True, alpha=False)
            for m in obj.data.materials:
                n = m.node_tree.nodes.get("bake_target")
                if n:
                    n.image = img
                m.node_tree.update_tag()
            for o, _ in lamps:
                o.data.update_tag()
            if cards:
                for m in cards.data.materials:
                    m.node_tree.update_tag()
            bpy.context.view_layer.update()
        t0 = time.time()
        res = bpy.ops.object.bake(type="DIFFUSE", pass_filter={"DIRECT", "INDIRECT"}, margin=2)
        log("baked %dx%d at %d samples in %.0f s (%s)" % (W, H, samples, time.time() - t0, res))
        _px = np.empty(W * H * 4, dtype=np.float32)
        img.pixels.foreach_get(_px)
        log("final bake image '%s' mean %.4f; lamp energies %s; glow nodes %s" % (
            img.name, float(_px.reshape(-1, 4)[:, :3].mean()),
            [round(o.data.energy, 3) for o, _ in lamps[:3]],
            sorted({round(n.inputs["Strength"].default_value, 2) for m in obj.data.materials for n in m.node_tree.nodes if n.type == "EMISSION"})))
        raw = os.path.join(folder, "lightmap_raw.exr")
        img.filepath_raw = raw
        img.file_format = "OPEN_EXR"
        img.save()
        t0 = time.time()
        denoise(raw, exr, W, H)
        log("denoised in %.0f s" % (time.time() - t0))
    lm_img = bpy.data.images.load(exr)
    lm_img.colorspace_settings.name = "Non-Color"

    if cam:
        x, y, z, pitch, yaw = (float(v) for v in cam.split())
        c = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
        sc.collection.objects.link(c)
        sc.camera = c
        c.location = (x, y, z + 0.037)
        c.rotation_euler = (math.radians(90 + pitch), 0, math.radians(yaw))
        c.data.sensor_fit = "HORIZONTAL"
        c.data.angle = math.radians(90)
        c.data.clip_start = 0.005
        sc.render.resolution_x, sc.render.resolution_y, sc.render.resolution_percentage = 1280, 960, 100
        sc.cycles.samples = 16
        sc.view_settings.view_transform = "Standard"
        if cards:
            cards.hide_render = True
        # Previews: emission only, so they show the light maps, not a fresh render.
        sc.cycles.max_bounces = 0
        base_mats = [m.copy() for m in obj.data.materials]
        for mode in ("1997", "baked", "light"):
            for i, m in enumerate(base_mats):
                obj.data.materials[i] = m.copy()
            preview_materials(obj, lm_img, fill, mode, fitted["floor"] * arg("--floor", 0.0, float) if fitted else None)
            sc.render.filepath = os.path.join(folder, "preview_%s%s.png" % (mode, arg("--tag", "")))
            bpy.ops.render.render(write_still=True)
            log("rendered", mode)


try:
    main()
except Exception:
    import traceback
    traceback.print_exc()
    log("FAILED")
