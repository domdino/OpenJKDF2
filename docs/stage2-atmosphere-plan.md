# Atmosphere plan: smoke, fog and volumetric light (Stage 2, lighting phase 5)

Status: proposal, waiting for Dom's go-ahead.

## Why

Two jobs:
1. **Mood.** The north star frames all have air you can see: haze that glows the colour of nearby lights, beams, smoke and steam drifting through them. Characters and light pools read better against it.
2. **Hiding.** Low-poly distant terrain, flat pits, abrupt level edges and bare corners look worst in clear air. Ground fog in a canyon, smoke over a pit or denser air at a level's far edge hide them the way film sets use smoke.

## What we have today

- **Haze** (`r_haze`): screen-space distance fog, coloured by a blur of the frame, so it glows near bright things. It's the same density everywhere, it can't sit low in a pit, and it doesn't know where light actually is: a dark corridor hazes the same as a lit one.
- **Light shafts** (`r_lightShafts`): screen-space god rays towards up to 4 bright sources on screen. They're cheap and look right when the source is visible, but disappear when it's off screen.
- **Volumetric sky** for outdoor levels.
- **New since the light bake:** baked light everywhere near floors (light probes), in colour. That's the missing ingredient for fog that glows red over the hangar strips and goes dark between them.

## The plan

### 1. Light volume (bake)
Extend `tools/blender/probe_bake.py` to also bake a coarse 3D grid of light across each level's air, not just near floors: about 0.3 units (3 m) apart, inside sectors only, with the same ambient-cube format. It's seconds per level, like the probes. Stored as a small 3D texture per level (a few MB). The fog takes its colour and brightness from this, so air glows where the light is and stays dark where it isn't. Characters could later use it too (jumping, flying, lifts).

### 2. Volumetric fog (engine)
Replace the screen-space haze with real volumetric fog, the technique modern engines use (a "froxel" grid):
- A low-res 3D grid fitted to the camera (about 160 x 90 x 64 cells). Each frame, every cell gets a density and a light value, then the shader marches through it per pixel.
- **Density** = level base haze + **height fog** (thick near a level's floor height, thinning upwards: ground fog in pits and canyons) + **fog volumes** (step 3) + slow-moving 3D noise so it drifts and swirls instead of sitting flat.
- **Light** = the baked light volume (step 1) plus dynamic lights (sabers, blasters, the headlight), so a saber swing lights the smoke around it.
- Cost estimate: 1-2 ms at 1440p on the M5 Max. It replaces the haze pass, so the net cost is lower. The screen-space shafts stay for strong beams.

### 3. Fog and smoke volumes, placed by hand in Blender
Per-level JSON (`hd/atmos/<level>.json`, gitignored like the rest of hd/) listing boxes and spheres of fog or smoke: position, size, density, colour tint, drift speed, edge softness. Dom places them in Blender over the level geometry (the light bake export already builds it), and a small add-on exports the JSON. This is the "hide bad terrain" tool: put a fog bank over the ugly bit. Also per-level presets (base haze, height fog level, tint) matched by level like the sky presets, so every level starts with sensible air before any hand work.

### 4. Smoke and steam emitters
Soft particles: camera-facing smoke sprites that fade where they meet geometry (no hard intersection lines), lit by the light volume, animated with flipbooks. Flipbooks can come from Blender (Mantaflow) or EmberGen, which is Dom's department. Emitters are placed in the same JSON (vents, chimneys, burning wreckage, the hangar floor) with rate, size, rise speed and wind. The game's own smoke and explosion sprites could switch to the same soft, lit rendering later.

### 5. Dust and finishing
Dust motes drifting in lit air (tiny particles that only show where the light volume is bright), wind presets for outdoor levels, and occlusion for the screen-space shafts (they currently pass through walls).

## Parts and size

- Bake: `probe_bake.py` (grid placement inside sectors, one more Cycles bake), output `hd/jkgm/lightmaps/<level>.jklv`.
- Engine: a new `src/Engine/sithAtmos.c` (presets, volumes, emitters, loading), shader passes in `std3D.c` and new shaders (froxel fill, integrate, apply; particle draw), settings in `jkPlayer.c` (`r_volumetricFog`, `r_fogDensity`, `r_smoke`, in the remaster preset). That's about 4 engine files plus new shaders and a new file format, so it's a big change by the CLAUDE.md rule: hence this plan.
- Blender add-on: `tools/blender/atmos_volumes.py` (load a level's geometry, place fog boxes and emitters as empties, export JSON).

## Steps (each ends with screenshots and a side-by-side)

1. Light volume bake on the hangar, checked in Blender (slices through the volume).
2. Volumetric fog in the engine, density only (base + height fog + noise), lit by the light volume. Hangar: red-glowing air over the strips, dark between. Compare with today's haze.
3. Height fog presets for every level; check the outdoor levels (canyons, Barons Hed, the fuel station, the tower) for terrain it hides.
4. Fog volumes from JSON plus the Blender add-on; Dom places the first ones.
5. Smoke and steam emitters with soft, lit particles; first flipbooks.
6. Dust, wind, shaft occlusion. Batch all levels.

## Risks

- **Too much fog flattens the image** (the grade lesson: darkness should come from contrast). Defaults stay light; hiding fog is placed by hand where it's needed.
- **Fog over dark areas** needs real darkness in the light volume. The light bake gives that; the 1997 fill floor does not, so the fog uses the bake only.
- **Translucent faces** (water, glass) and the fog need care with draw order. Water is already a known gap in the light bake.
- **Mysteries of the Sith** gets the same system for free; its in-engine cutscenes would gain the most from it.
