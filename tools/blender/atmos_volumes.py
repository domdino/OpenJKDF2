"""JK Atmos: place fog volumes on a Jedi Knight level in Blender (atmosphere step 4).

Install: Edit > Preferences > Add-ons > Install from Disk... > this file (or run it from the
Text editor). Then in the 3D view press N and open the "JK Atmos" tab:

  1. Level: type the level name (e.g. 12escape) and press Load Level. It builds the level from
     its light bake export (hd/lightbake/<level>/level.json, textured, in JK units: 1 unit is
     about 10 m) and loads the level's existing fog volumes from atmos/<level>.json.
  2. Add Fog Box / Add Fog Ellipsoid put a volume at the 3D cursor. Move, rotate (about Z only)
     and scale it like any object. Each volume has its settings in the panel:
       density  how thick (the base haze is 0.1 per unit; 1-3 is a dense bank)
       tint     colour the fog scatters (white = takes the light's colour; darker = smoky)
       soft     how much of the edge fades out (0 = hard box, 1 = fades from the centre)
  3. Add Steam / Add Smoke put an emitter at the 3D cursor (a small arrow pointing up). Its
     settings: rate (puffs a second), life (seconds each), size (start radius, units), grow
     (end size x), rise (units a second), spread, drift (X/Y wind, units a second), tint,
     opacity. Smoke is dense and takes its tint; steam is white and wispy.
  4. Export writes atmos/<level>.json. The game reads it at level load (r_fogVolumes 1,
     needs r_volumetricFog and a baked light volume). Capture or play to see it.

Headless test: Blender -b --python tools/blender/atmos_volumes.py -- --test 12escape
"""
bl_info = {
    "name": "JK Atmos (fog volumes)",
    "author": "JK remaster",
    "version": (1, 0),
    "blender": (4, 0, 0),
    "location": "3D View > Sidebar > JK Atmos",
    "description": "Place fog volumes on Jedi Knight levels and export them for the game",
    "category": "Import-Export",
}

import json
import math
import os
import sys

import bpy

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))


def repo_root():
    """The repo, from this file's location, or the add-on preference if installed elsewhere."""
    if os.path.isdir(os.path.join(ROOT, "hd", "lightbake")):
        return ROOT
    prefs = bpy.context.preferences.addons.get(__name__)
    if prefs and prefs.preferences.repo:
        return bpy.path.abspath(prefs.preferences.repo)
    return ROOT


def lightbake_helpers(root):
    src = open(os.path.join(root, "tools", "blender", "lightbake.py")).read()
    cut = src.rfind("\ntry:\n    main()")
    ns = {"__name__": "lightbake", "__file__": os.path.join(root, "tools", "blender", "lightbake.py")}
    exec(compile(src[:cut] if cut > 0 else src, "lightbake.py", "exec"), ns)
    return ns


def fog_collection():
    c = bpy.data.collections.get("JK Fog")
    if not c:
        c = bpy.data.collections.new("JK Fog")
        bpy.context.scene.collection.children.link(c)
    return c


def make_volume(name, shape, center, size, rot_z, density, tint, soft):
    e = bpy.data.objects.new(name, None)
    e.empty_display_type = "SPHERE" if shape == 1 else "CUBE"
    e.empty_display_size = 1.0
    e.location = center
    e.scale = (max(size[0], 0.01) / 2, max(size[1], 0.01) / 2, max(size[2], 0.01) / 2)
    e.rotation_euler = (0.0, 0.0, math.radians(rot_z))
    e.jk_fog = True
    e.jk_fog_density = density
    e.jk_fog_tint = tint
    e.jk_fog_soft = soft
    e.color = (*tint, 1.0)
    fog_collection().objects.link(e)
    return e


EMITTER_DEFAULTS = {0: dict(rate=8.0, life=3.5, size=0.05, grow=5.0, rise=0.12, spread=0.04, opacity=0.6, tint=(1, 1, 1)),
                    1: dict(rate=5.0, life=6.0, size=0.12, grow=3.5, rise=0.12, spread=0.08, opacity=0.85, tint=(0.45, 0.42, 0.4))}


def make_emitter(name, kind, center, **kw):
    e = bpy.data.objects.new(name, None)
    e.empty_display_type = "SINGLE_ARROW"
    e.empty_display_size = 0.15
    e.location = center
    e.jk_emit = True
    e.jk_emit_kind = kind
    p = dict(EMITTER_DEFAULTS[kind])
    p.update({k: v for k, v in kw.items() if v is not None})
    e.jk_emit_rate, e.jk_emit_life, e.jk_emit_size = p["rate"], p["life"], p["size"]
    e.jk_emit_grow, e.jk_emit_rise, e.jk_emit_spread = p["grow"], p["rise"], p["spread"]
    e.jk_emit_opacity = p["opacity"]
    e.jk_fog_tint = p["tint"]
    e.jk_emit_drift = p.get("drift", (0.0, 0.0))
    e.color = (*p["tint"], 1.0)
    fog_collection().objects.link(e)
    return e


def load_level(level):
    root = repo_root()
    folder = os.path.join(root, "hd", "lightbake", level)
    lj = os.path.join(folder, "level.json")
    if not os.path.exists(lj):
        raise RuntimeError("no light bake export for %s (run tools/lightbake/jkl_export.py %s)" % (level, level))
    for o in list(bpy.data.objects):
        if o.name.startswith("level") or getattr(o, "jk_fog", False) or getattr(o, "jk_emit", False):
            bpy.data.objects.remove(o, do_unlink=True)
    lb = lightbake_helpers(root)
    d = json.load(open(lj))
    obj = lb["build"](d, folder, (0.3, 0.3, 0.35), 1.0)
    obj.name = "level " + level
    # Viewport: show the textures (the bake materials are diffuse + emission).
    for area in getattr(bpy.context.screen, "areas", []) if bpy.context.screen else []:
        if area.type == "VIEW_3D":
            for sp in area.spaces:
                if sp.type == "VIEW_3D":
                    sp.shading.type = "MATERIAL"
                    sp.clip_end = 2000.0
                    sp.clip_start = 0.01
    n = 0
    path = os.path.join(root, "atmos", level + ".json")
    if os.path.exists(path):
        for i, v in enumerate(json.load(open(path)).get("volumes", [])):
            make_volume(v.get("name", "fog %d" % i), int(v.get("shape", 0)), v.get("center", [0, 0, 0]),
                        v.get("size", [1, 1, 1]), v.get("rot_z", 0.0), v.get("density", 0.5),
                        v.get("tint", [1, 1, 1]), v.get("soft", 0.3))
            n += 1
        for i, v in enumerate(json.load(open(path)).get("emitters", [])):
            make_emitter(v.get("name", "emitter %d" % i), int(v.get("kind", 0)), v.get("center", [0, 0, 0]),
                         rate=v.get("rate"), life=v.get("life"), size=v.get("size"), grow=v.get("grow"),
                         rise=v.get("rise"), spread=v.get("spread"), opacity=v.get("opacity"),
                         tint=tuple(v["tint"]) if "tint" in v else None,
                         drift=tuple(v["drift"]) if "drift" in v else None)
            n += 1
    bpy.context.scene.jk_level = level
    return n


def export_level(level):
    root = repo_root()
    vols = []
    for o in bpy.data.objects:
        if not getattr(o, "jk_fog", False):
            continue
        s = o.scale * o.empty_display_size
        vols.append({
            "name": o.name,
            "shape": 1 if o.empty_display_type == "SPHERE" else 0,
            "center": [round(x, 4) for x in o.location],
            "size": [round(2 * abs(x), 4) for x in s],
            "rot_z": round(math.degrees(o.rotation_euler.z), 2),
            "density": round(o.jk_fog_density, 4),
            "tint": [round(x, 4) for x in o.jk_fog_tint],
            "soft": round(o.jk_fog_soft, 3),
        })
    emits = []
    for o in bpy.data.objects:
        if not getattr(o, "jk_emit", False):
            continue
        emits.append({
            "name": o.name, "kind": o.jk_emit_kind,
            "center": [round(x, 4) for x in o.location],
            "rate": round(o.jk_emit_rate, 3), "life": round(o.jk_emit_life, 3), "size": round(o.jk_emit_size, 4),
            "grow": round(o.jk_emit_grow, 3), "rise": round(o.jk_emit_rise, 4), "spread": round(o.jk_emit_spread, 4),
            "tint": [round(x, 4) for x in o.jk_fog_tint], "opacity": round(o.jk_emit_opacity, 3),
            "drift": [round(x, 4) for x in o.jk_emit_drift],
        })
    os.makedirs(os.path.join(root, "atmos"), exist_ok=True)
    path = os.path.join(root, "atmos", level + ".json")
    with open(path, "w") as f:
        json.dump({"level": level, "volumes": vols, "emitters": emits}, f, indent=2)
    return path, len(vols) + len(emits)


class JKATMOS_OT_load(bpy.types.Operator):
    bl_idname = "jkatmos.load_level"
    bl_label = "Load Level"
    bl_description = "Build the level from its light bake export and load its fog volumes"

    def execute(self, context):
        try:
            n = load_level(context.scene.jk_level.strip().lower().replace(".jkl", ""))
        except Exception as e:
            self.report({"ERROR"}, str(e))
            return {"CANCELLED"}
        self.report({"INFO"}, "Loaded %s with %d fog volumes" % (context.scene.jk_level, n))
        return {"FINISHED"}


class JKATMOS_OT_add(bpy.types.Operator):
    bl_idname = "jkatmos.add_fog"
    bl_label = "Add Fog"
    bl_description = "Add a fog volume at the 3D cursor"
    shape: bpy.props.IntProperty(default=0)

    def execute(self, context):
        c = context.scene.cursor.location
        e = make_volume("fog box" if self.shape == 0 else "fog ellipsoid", self.shape, tuple(c),
                        (2.0, 2.0, 0.4), 0.0, 1.0, (1.0, 1.0, 1.0), 0.5)
        for o in context.selected_objects:
            o.select_set(False)
        e.select_set(True)
        context.view_layer.objects.active = e
        return {"FINISHED"}


class JKATMOS_OT_add_emitter(bpy.types.Operator):
    bl_idname = "jkatmos.add_emitter"
    bl_label = "Add Emitter"
    bl_description = "Add a smoke or steam emitter at the 3D cursor"
    kind: bpy.props.IntProperty(default=0)

    def execute(self, context):
        e = make_emitter("steam" if self.kind == 0 else "smoke", self.kind, tuple(context.scene.cursor.location))
        for o in context.selected_objects:
            o.select_set(False)
        e.select_set(True)
        context.view_layer.objects.active = e
        return {"FINISHED"}


class JKATMOS_OT_export(bpy.types.Operator):
    bl_idname = "jkatmos.export"
    bl_label = "Export"
    bl_description = "Write atmos/<level>.json for the game"

    def execute(self, context):
        level = context.scene.jk_level.strip().lower().replace(".jkl", "")
        if not level:
            self.report({"ERROR"}, "Set the level name first")
            return {"CANCELLED"}
        path, n = export_level(level)
        self.report({"INFO"}, "Wrote %d fog volumes to %s" % (n, path))
        return {"FINISHED"}


class JKATMOS_PT_panel(bpy.types.Panel):
    bl_label = "JK Atmos"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "JK Atmos"

    def draw(self, context):
        lay = self.layout
        lay.prop(context.scene, "jk_level", text="Level")
        lay.operator("jkatmos.load_level")
        row = lay.row(align=True)
        row.operator("jkatmos.add_fog", text="Add Fog Box").shape = 0
        row.operator("jkatmos.add_fog", text="Add Fog Ellipsoid").shape = 1
        row = lay.row(align=True)
        row.operator("jkatmos.add_emitter", text="Add Steam").kind = 0
        row.operator("jkatmos.add_emitter", text="Add Smoke").kind = 1
        o = context.active_object
        if o and getattr(o, "jk_emit", False):
            box = lay.box()
            box.label(text=o.name)
            box.prop(o, "jk_emit_kind", text="Smoke (1) / steam (0)")
            for prop, label in (("jk_emit_rate", "Rate"), ("jk_emit_life", "Life"), ("jk_emit_size", "Size"),
                                ("jk_emit_grow", "Grow"), ("jk_emit_rise", "Rise"), ("jk_emit_spread", "Spread"),
                                ("jk_emit_drift", "Drift"), ("jk_fog_tint", "Tint"), ("jk_emit_opacity", "Opacity")):
                box.prop(o, prop, text=label)
        if o and getattr(o, "jk_fog", False):
            box = lay.box()
            box.label(text=o.name)
            box.prop(o, "jk_fog_density", text="Density")
            box.prop(o, "jk_fog_tint", text="Tint")
            box.prop(o, "jk_fog_soft", text="Soft edge")
        lay.operator("jkatmos.export")


class JKATMOS_Prefs(bpy.types.AddonPreferences):
    bl_idname = __name__
    repo: bpy.props.StringProperty(name="Repo folder", subtype="DIR_PATH",
                                   description="The JEDI KNIGHT repo (only needed if the add-on was installed elsewhere)")

    def draw(self, context):
        self.layout.prop(self, "repo")


def _tint_update(self, context):
    self.color = (*self.jk_fog_tint, 1.0)


CLASSES = (JKATMOS_OT_load, JKATMOS_OT_add, JKATMOS_OT_add_emitter, JKATMOS_OT_export, JKATMOS_PT_panel, JKATMOS_Prefs)


def register():
    bpy.types.Scene.jk_level = bpy.props.StringProperty(name="Level", default="12escape")
    bpy.types.Object.jk_fog = bpy.props.BoolProperty(default=False)
    bpy.types.Object.jk_fog_density = bpy.props.FloatProperty(default=1.0, min=0.0, soft_max=5.0)
    bpy.types.Object.jk_fog_tint = bpy.props.FloatVectorProperty(subtype="COLOR", size=3, default=(1, 1, 1),
                                                                 min=0.0, max=1.0, update=_tint_update)
    bpy.types.Object.jk_fog_soft = bpy.props.FloatProperty(default=0.5, min=0.0, max=1.0)
    bpy.types.Object.jk_emit = bpy.props.BoolProperty(default=False)
    bpy.types.Object.jk_emit_kind = bpy.props.IntProperty(default=0, min=0, max=1)
    bpy.types.Object.jk_emit_rate = bpy.props.FloatProperty(default=8.0, min=0.1, soft_max=20.0)
    bpy.types.Object.jk_emit_life = bpy.props.FloatProperty(default=3.5, min=0.2, soft_max=10.0)
    bpy.types.Object.jk_emit_size = bpy.props.FloatProperty(default=0.05, min=0.005, soft_max=1.0)
    bpy.types.Object.jk_emit_grow = bpy.props.FloatProperty(default=5.0, min=0.1, soft_max=10.0)
    bpy.types.Object.jk_emit_rise = bpy.props.FloatProperty(default=0.12, soft_min=-1.0, soft_max=1.0)
    bpy.types.Object.jk_emit_spread = bpy.props.FloatProperty(default=0.04, min=0.0, soft_max=1.0)
    bpy.types.Object.jk_emit_opacity = bpy.props.FloatProperty(default=0.6, min=0.0, max=1.0)
    bpy.types.Object.jk_emit_drift = bpy.props.FloatVectorProperty(size=2, default=(0.0, 0.0))
    for c in CLASSES:
        bpy.utils.register_class(c)


def unregister():
    for c in reversed(CLASSES):
        bpy.utils.unregister_class(c)


if __name__ == "__main__":
    register()
    if "--test" in sys.argv:
        level = sys.argv[sys.argv.index("--test") + 1]
        n = load_level(level)
        print("JKATMOS: loaded", level, "with", n, "volumes;", len(bpy.data.objects), "objects")
        path, m = export_level(level + "_roundtrip")
        print("JKATMOS: exported", m, "volumes to", path)
