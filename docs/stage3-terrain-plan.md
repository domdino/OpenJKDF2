# Terrain plan: more polygons for rock, cliffs and canyons

Status: Dom chose parallax, then the skin (2026-11-12). Parallax is in (with a loader fix that finally shows the Texture God textures). The skin works end to end on the tower (generator, bake, engine; see DECISIONS) but the gain is subtle so far; rubble is next.

## What the terrain is

There are no terrain models in JK: cliffs, canyon walls and rock floors are ordinary level faces, big and flat. Where it matters most:

| Level | Terrain | Faces |
|---|---|---|
| The tower (14) and the ascent (18) | `14nuclif1a` cliff rock is 80-87 % of everything you see | 1,000-1,400 faces, each about 4-6 m across |
| The Maw (15) | `15cliff2` cliffs, 83 % of the level | 81 faces, the median one is 100 square units (about 10,000 m²): huge flat slabs |
| Escape house (04) | `00cliff3` cliffs, `04wex4a` rock walls | cliffs 2.8 square units per face |
| Barons Hed (06), fuel station (09), Katarn's farm (03) | canyon walls, rock, grass | mixed |

The same faces are also the collision: the player walks on them. Anything we add on top must stay close to them, or the player will float or sink.

## Options

### 1. Parallax on the rock textures (quick win, no new geometry)
The engine already has parallax occlusion mapping (the HD pack's `displacement_map`, used by no pack yet), and Texture God has written a 16-bit height map for every texture it processed (1,152 of them). Importing those heights for rock, cliff and ground textures gives the surfaces real-looking depth: cracks, ledges and bumps that shift with the view and shadow each other.
- Cost: an importer change and tuning the depth per material. A day or so.
- Limit: silhouettes stay flat. A cliff edge against the sky is still a straight line, and the huge Maw slabs still read as slabs.

### 2. Modify the existing geometry: a high-poly "skin" over the terrain (my recommendation for the bulk)
For each level, a tool selects the terrain faces (by material: cliff, rock, ground), tessellates them in Blender and displaces the result with the texture's own height map plus larger rock-scale noise (bigger ledges, outcrops, overhangs on the Maw). Edges where terrain meets architecture stay pinned, so it still joins up. The result is drawn in place of the flat faces; collision stays on the originals, so displacement is kept within a few tens of cm on walkable floors and can be large on walls and cliffs nobody touches.
- It goes through the light bake like everything else, so it gets real shadows from its own bumps, and the sun now in the bake rakes across it.
- Engine work: a per-level mesh sidecar like the light maps (surface number -> replacement patch), drawn when that surface would be drawn, so portal culling still works. That's a new file format and about 3-4 engine files: a "big change" by our rules.
- Automatic for every level, tuned per material in a small table.

### 3. Buy assets and place them by hand (for hero spots)
Photoscanned cliff and rock kits (Quixel Megascans on Fab, Sketchfab, KitBash3D and similar; check each licence allows use outside Unreal for a personal project). Placed in Blender over the level, the same way as the fog volumes (the JK Atmos add-on would get an "Add Prop" button), exported per level, drawn by the engine as static props.
- Best possible look where it matters: the Maw, the tower's base, the Barons Hed canyon.
- Hand work per spot, and the props need lighting: they'd be lit by the light probes, or baked into the light bake if we include them in the Blender scene.
- Engine work: static props from a per-level file (we already draw 3do models; high-poly ones need the larger mesh limits we raised for the cast).

### Rubble along creases (Dom's idea)
No hard straight edges where a rock wall meets the floor: a tool finds every concave crease between two organic surfaces (rock, cliff, dirt, sand, grass; never metal, panels or architecture) and scatters rubble along it, mostly small, a few bigger, half-buried and tucked against the wall, textured with that wall's rock and baked with everything else. It shares the skin's extra-geometry sidecar. Visual only (no collision), so kept low and tight to the wall.

## Recommendation

Do 1 first (days, every level gets depth on its rock), then 2 for the bulk of the terrain, then 3 only for a handful of hero spots where you want a specific look. 2 and 3 share most of their engine work (extra geometry per level from a sidecar), so building 2 makes 3 cheap.

## Questions for Dom

- Happy with 1 then 2, with bought assets later for hero spots?
- Any budget or preferred source for bought assets?
