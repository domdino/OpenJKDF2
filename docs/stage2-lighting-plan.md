# Stage 2: per-pixel lighting plan

Status: proposal, waiting for a yes. Target look: north star 2 (dark, wet, grimy, atmospheric).

## How lighting works today

- **Static light is baked.** The level editor baked lighting into a brightness value per level vertex (`surfaceInfo.intensities`). The level files store no actual light sources, so there are no light positions to work from.
- **Dynamic lights are real points.** Each frame the engine collects up to 32 light-emitting things in view: blaster bolts, explosions, sabers, headlights, glowing pickups. Each has a world position and intensity (`sithRender.c`, `rdCamera_AddLight`).
- **But dynamic lights are applied per vertex**, the same as the baked light:
  - Level vertices: `sithRender_BuildDynamicLights` adds `intensity - distance * attenuation` with no surface angle at all.
  - Models: `rdLight_CalcVertexIntensities` adds the same, times the surface angle.
- **The GPU only sees one brightness value per vertex.** Vertices arrive already projected to the screen. The shader gets screen position, depth, UV and that brightness value, then runs it through the game's 8-bit light tables. That's why lights look blotchy on JK's big flat faces and a blaster bolt brightens whole chunks of wall.

## The approach

Keep the baked static light exactly as it is: it's the level designers' lighting and the game's look. Move **dynamic** lights from per-vertex to per-pixel, and add specular highlights.

The key trick is that no vertex pipeline rewrite is needed. The shader can rebuild each pixel's 3D position from what it already has: screen position, depth and the camera's field of view. Surface normals come from screen-space derivatives (the existing parallax code already does this). So the CPU only has to send the light list each frame.

### Phase 1: per-pixel dynamic lights (the core)
1. **Upload the light list.** Each frame, send the camera's light list to the shader as uniforms: view-space position, intensity, radius. Colour and spot cone go in too, for MotS's coloured lights later.
2. **Rebuild position and normal per pixel** in `default_f.glsl`, from screen position, depth and camera intrinsics.
3. **Light each pixel.** Use the original falloff formula and the surface angle, so a blaster bolt is as bright as before, just shaped correctly: round pools on walls, falling off with angle.
4. **Add specular highlights** (Blinn-Phong) from dynamic lights. Default glossiness per surface comes from a "wetness" setting; Stage 3 material maps replace it per texture. Highlights also feed the bloom buffer, so wet glints glow.
5. **Avoid double lighting.** When per-pixel lighting is on, skip the old per-vertex dynamic light in `sithRender_BuildDynamicLights` and the model light path (`rdModel3.c`).
6. **Keep the 8-bit look an option.** For original textures the combined light still goes through the game's light tables, so the palette banding character stays. HD textures get smooth true-colour lighting.
7. **Toggle**: `r_perPixelLighting` (off by default until it's verified), plus `r_specular` and `r_wetness`. Off means exactly the current game.

### Phase 2: static highlights from glowing surfaces
North star 2's wet glints mostly come from the skylight and the floor grates, which aren't dynamic lights. The plan: at level load, scan for sky ceilings and fullbright or emissive surfaces, and place invisible "virtual lights" at them. These only add specular highlights and a little extra light, so the baked lighting is untouched. Lights are culled to the visible area. This is the piece that makes the grimy look read as wet rather than just dark.

### Phase 3: atmosphere
- Distance haze tinted by the grade, using the depth buffer that's already there.
- Light shafts: a screen-space radial blur from bright sky and emissive areas (the skylight in the spawn room is the test case).
- Optional, subtle lens streak from the bloom buffer.

## Files touched

Phase 1: `std3D.c` (uniforms), `default_f.glsl` and `default_v.glsl`, `sithRender.c` (skip per-vertex dynamic light), `rdModel3.c` (same for models), `jkPlayer.c/h` (settings). Phase 2 adds a small new module for virtual lights, hooked into level load. That's more than a handful of engine files, hence this plan.

## Risks and unknowns

- **Depth encoding.** The exact meaning of vertex `z` has to be pinned down to rebuild positions accurately. The vertex shader treats it as `1 - 1/viewZ`-style; to be confirmed against `rdCamera`. If it's lossy, fallback plan: pass view-space position as an extra vertex attribute. That's bigger, because it touches the clip/projection path.
- **Derivative normals** are exact on flat faces (most of JK) but noisy along silhouettes and on curved models. Fine for walls; models may need per-vertex normals passed through later (Stage 4 territory anyway).
- **Brightness matching.** Per-pixel lights must not make the game brighter or darker overall. Check: captures with the lighting toggle on and off, the same frame, plus a blaster fight scene.
- **Mysteries of the Sith** has coloured and spot lights (`JKM_LIGHTING`). The uniform layout carries colour and cone from day one so it plugs in later rather than needing a rewrite.

## How it's verified

- Silent captures, on and off, in the spawn room and in a scene with a blaster bolt or saber near a wall. Thing lights can be spawned in capture mode for repeatable tests.
- Frame time stays above 120 fps at Retina window size with everything on.
- A play session from Dom before it's switched on by default.

## Estimate

- Phase 1: one solid session. Most of the risk is in the depth reconstruction; once that's confirmed, the rest is shader work.
- Phase 2: about one session.
- Phase 3: about half a session.
