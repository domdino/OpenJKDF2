# Light bake plan (Stage 2, lighting phase 4)

Status: proposal, waiting for Dom's yes. Touches about 6 engine files and adds a new per-level sidecar file format, so per CLAUDE.md it needs approval first.

## The problem

The level lighting is still the 1997 bake: one brightness value per polygon corner, painted by the level designers' lights in JED. Those lights aren't in the shipped levels; only the result is. JK polygons are big (a hangar wall can be one face), so light can only change across a face as a straight-line blend between its corners. That gives:
- even, flat light everywhere, with no pools and no darkness in between;
- glowing strips, lamps, lava and screens that light nothing around them;
- no shadows, no bounce light and no colour (everything is white light).

Everything we've added since (per-pixel lights, reflections, haze, grade) sits on top of that flat base, so it can't fix it. A grade on top would only hide it.

## The idea

Bake proper lighting offline, once per level, with Blender's Cycles path tracer on the M5 Max GPU, and store it as a **light map**: a texture of light covering every visible surface at about 15 cm per texel (64 texels per JK unit; Kyle is 0.18 units tall). The game then uses the light map in place of the 1997 corner values. Cycles gives everything for free: soft shadows, coloured bounce light, area lights from every glowing texel, and sky light through openings.

Light sources in the bake:
1. **Glowing texels.** Every texel the colormap keeps bright at its darkest light level (the same test `sithGlowLights.c` already uses) becomes emissive, in its own colour. Red floor strips throw red light, white wall strips white, lava orange, consoles their screen colour.
2. **Fullbright and self-lit faces** (light mode 0, extra light) as emitters too.
3. **Sky.** Sky faces emit the sky's colour, so outdoor areas get cool, soft light from above and stay dark under overhangs. Optional per-level moon or sun (direction, colour) for hard shadows outdoors, set by hand per level in a small JSON.
4. **The original 1997 light as fill.** The designers lit a lot of places with invisible lights we can't recover. The original corner values are kept as a dimmed fill (start at about 30 %, per-level adjustable) so nothing the designers meant to be visible goes pitch black. This keeps the game playable while the baked light does the mood.

Dynamic light stays as it is: sabers, blaster bolts, explosions and the headlight still light per pixel on top.

## Size and cost

- 12escape (the hangar): 1,576 square units of visible surface, about 6.5 million light map texels at 64 per unit, which fits one 2,560 to 3,000 px atlas (half-float, about 50 MB in GPU memory; stored as a 16-bit PNG, a few MB on disk).
- Bake time: guess 2 to 10 minutes per level on the M5 Max (to be measured on the first level). The whole game is an overnight batch.
- Frame cost: one extra texture read per pixel. Negligible.

## Parts

**Bake tool (Python + Blender, no engine changes)**
- `tools/lightbake/jkl_geometry.py`: reads a .jkl (vertices, texture vertices, surfaces, sectors, materials, flags, original intensities).
- `tools/lightbake/atlas.py`: gives every drawn surface a rectangle in the atlas. Each JK surface is flat, so its light map coordinates are a plain projection onto its own plane (two axes plus an offset). Padding between rectangles so filtering never bleeds.
- `tools/blender/lightbake.py`: builds the level in Blender (original textures, or the HD pack where present, for bounce colour), the emitters above, then bakes irradiance into the atlas with Cycles (denoised), and writes the result.
- Output per level, in `hd/lightmaps/` (gitignored): `<level>.png` (16-bit atlas) and `<level>.json` (per surface: plane axes, offset, atlas rectangle; the fill amount; version and source level checksum so a changed level is detected).

**Engine (about 6 files)**
- `sithRender.c`: tags each level face sent to the render cache with its surface number (new field on `rdProcEntry`).
- `rdCache.c`: writes the surface number into the GPU vertex's spare `ny` field (unused today, so the clipping code needs no change).
- `std3D.c`: loads the level's light map at level load; uploads the atlas and a small data texture of per-surface projections; passes the inverse camera rotation so the shader can rebuild world position (it already rebuilds camera-space position for per-pixel lighting).
- `default_f.glsl` / vertex shader: for level faces with a light map, world position, then light map coordinates, then sample; that replaces the static part of the vertex light. Characters, items and doors (all 3DO things) keep vertex/sector light for now.
- `jkPlayer.c`: `r_lightmaps` setting (on in the remaster preset, off = original), plus `r_lightmapFill` to tune the 1997 fill live.
- `sithWorld.c`: finds and validates the sidecar on load; silently falls back to the original light if it's missing or doesn't match the level.

## Known limits

- **Light switches.** A few levels change lights from cog scripts (flickering, lights going out). The bake can't follow; plan: scale the baked light by how much the original corner light changed, so switching off still darkens. Check which levels do this first.
- **Characters and props** don't get the baked light at first: they'll look like they do now. Next step after this works: a coarse grid of light probes baked in the same pass, so Kyle walks into a red pool and turns red.
- **Doors and lifts** are models, not level geometry: they aren't in the bake, so light passes through a closed door. Usually invisible; fix case by case if it shows.
- **Mysteries of the Sith** already has coloured corner light: the same pipeline works there, using its RGB values as the fill. Note for later.

## Steps (each ends with screenshots)

1. **Bake one level in Blender only** (12escape hangar). Render the baked hangar in Blender from the capture camera: you judge the look before any engine work. About half a day.
2. **Engine support**, then the hangar in game: original vs light map side-by-side. About half a day.
3. **Tune** on the standard views (hangar, Sarris rooftop, Boc's room, Yun, cargo, tower): emitter strength, fill amount, sky, texel density, seams.
4. **Light probes** for characters and props.
5. **Batch** all levels overnight; spot-check each; add to the remaster preset.
