# Decisions log

Short running log of what was tried, what worked, what didn't and why. Read this first.

## 2026-11-11 - Stage 1: native build

**Repo setup**
- Source is upstream OpenJKDF2 master at ae6ed8a8 (2026-11-01). Remotes: `origin` = github.com/domdino/OpenJKDF2 (fork), `upstream` = shinyquagsire23/OpenJKDF2.
- Upstream already gitignores *.gob, JK.EXE, episode/, MOVIE etc. We added /gamedata/, /hd/, /logs/ and the loose asset types (.mat, .3do, .key, .wav, .ogg, .smk, .san, .cnd, .jkl). No tracked upstream files match those.
- Upstream also gitignores CLAUDE.md and AGENTS.md, so our CLAUDE.md stays local unless force-added.

**Build**
- Builds on Apple Silicon (macOS 26, Xcode 27, Apple clang) with `scripts/build.sh`. Output: `OpenJKDF2.app` at the repo root, arm64 only, Release, no ASAN.
- SDL3, SDL_mixer, OpenAL Soft, zlib, libpng, GLEW, physfs, protobuf and GameNetworkingSockets are git submodules built by CMake. Homebrew extras used: libmodplug, protobuf, innoextract (plus cmake, imagemagick, openal-soft, glew, libpng, openssl@3, already installed).
- **Fix: paths with spaces.** Our folder is `.../JEDI KNIGHT/JEDI KNIGHT`. `cmake_modules/precompile_globals.cmake` wrapped paths in literal quotes, which turned into `JEDI\ KNIGHT` and broke the cog code generator. Removed the quotes and added `VERBATIM` so CMake quotes each argument itself. Small, generic, worth offering upstream.
- The macOS bundle step calls `generate-iconset` (a pip package). build.sh keeps it, and cogapp, in a private venv at `build_openjkdf2_venv/` (gitignored upstream) so the system/conda Python is untouched.
- Harmless warning in the log: install_name_tool "no LC_RPATH" on png.framework. Build continues fine.

**Game data**
- Extracted from the GOG offline installer (build 17966, GOG id 1422286819) with `innoextract --language english -I app` into `gamedata/jk1/`. Layout matches what OpenJKDF2 expects: JK.EXE, Episode/JK1*.GOB, Resource/{JK_.CD, Res1hi.gob, Res2.gob, VIDEO/*.SMK x25}, MUSIC/Track12-32.ogg.
- `scripts/run.sh` points the engine at it with `OPENJKDF2_ROOT`. That env var beats every other data path, so nothing goes in ~/Library/Application Support. Saves, player profiles and settings are written inside gamedata/jk1/player/.
- macOS filesystem is case-insensitive, so GOG's `Resource/` and `Episode/` capitalisation is fine.

**First run**
- `scripts/run.sh -- -autostart -sp -episode JK1 -map 01narshadda.jkl` launches straight into level 1. Confirmed the level loads (engine wrote its level-start autosave `_JKAUTO_01narshadda.jks`).
- Hand playtest by Dom (player "Dom", HiDPI on, windowed): cutscenes play in sync, music plays, menu save and load work, Force powers work (via cheat). Looks like the original. Baseline screenshot: docs/screenshots/stage1-baseline-01narshadda.webp.
- Lightsaber not tested yet: Kyle doesn't get it until a later level and no cheat gives it early. Check when we reach it.
- Stage 1 done 2026-11-11.

**Settings worth knowing (per player, Setup > Display)**
- `windowishidpi` is off by default. Turn on HiDPI for native Retina resolution.
- Already built in: bloom, SSAO, SSAA multiplier, texture filtering, FOV, fps limit, vsync. Audit these properly in Stage 2.
- `bEnableJkgm` is on by default. OpenJKDF2 can load JKGFXMOD-style HD material packs (jkgm/). Upstream PR #487 (Oct 2026) also mentions "HD PNG uploads". Big head start for Stage 3: check this before writing our own texture loader.

**Mysteries of the Sith**
- Supported via the `-motsCompat` flag with data in `OPENJKMOTS_ROOT`. `scripts/run.sh mots` is wired for `gamedata/mots/`.
- Actively maintained: 18 MotS-related upstream commits in 2026, latest 2026-11-01.
- GOG MotS installer is in USEFUL STUFF. Full gap check deferred until JK1 is done, per the brief.

**Community packs on hand (USEFUL STUFF), for Stage 3 research**
- Jedi Knight Remastered 3.3 (General Tantor, 2024): bundles OpenJKDF2 v0.9.1 + Neural Upscale texture pack + Vurt's Extreme Definition pack + HD Kyle + model fixes. Windows installer, but it targets OpenJKDF2, so the content should drop into our build.
- Jedi Knight Ultimate 1.1: needs JKR, adds HD skies, weapon/Kyle skins and a gameplay mod (Saber Battle X) in EnhancementMod.gob.
- JK Enhanced (JKE.gob, 2007) and a saber colour mod for JKR.
- MotS Remastered 3.0 (OpenJKDF2-based) and 5.11 (JKGFXMOD-based). Keep both until the MotS stage.

**Rejected: Smith (smith.bah.wtf)**
- A separate Sith-engine emulator: closed source, C#/.NET Framework, Windows only, and its own site says gameplay is mostly broken. OpenJKDF2 is a faithful reimplementation of the real engine, so Smith adds nothing as a base. Installer binned.
- Ideas only: it generates bumpmaps from the original textures, which is one route for Stage 2 lighting. Its physics notes (Research.pdf) aren't needed, as OpenJKDF2 has the real constants.

## 2026-11-11 - Stage 2: rendering audit (nothing changed yet)

What OpenJKDF2's OpenGL renderer already does, from reading std3D.c, Window.c, jkPlayer.c and the shaders. Settings are per player (Setup > Display, plus Advanced) and are also cvars (r_*).

**Already there**
- Resolution: HiDPI toggle (off by default), fullscreen, window resizes freely. GL 3.3 core context, well under macOS's 4.1 cap.
- FOV: 40-170, "vertical (Hor+)" mode, default 90 vertical. That's wider than the 1997 original (90 horizontal at 4:3 = about 74 vertical).
- Frame rate: fps cap 0-360 or uncapped, vsync. Physics and cog scripts run on fixed ticks, so high fps is safe.
- Texture filtering: optional bilinear. For the original 8-bit textures it's done inside the shader (4 palette lookups blended), so the original light-level colour tables still apply. True-colour textures use normal GL filtering.
- Anti-aliasing: SSAA only (render at a multiple of the resolution and shrink). No MSAA.
- Bloom: built in. Fullbright palette colours (lights, glowing grates, saber, blaster bolts) write to a separate emissive buffer, which is blurred over 4 downsampled levels and added back.
- SSAO: built in. The main pass writes position and normal buffers for it.
- Gamma and HUD scale.
- HD materials: JKGFXMOD (jkgm) packs supported, including emissive and displacement maps. Custom saber RGB colours supported in the shader.
- F12 saves a PNG screenshot (SHOT####.PNG). A cog verb can trigger it too, so before/after captures can be scripted.

**Missing or weak**
- No mipmaps on any texture (MAX_LEVEL 0) and no anisotropic filtering. Distant surfaces shimmer and alias, worse at Retina resolution. Easy for true-colour textures. Hard for the original 8-bit ones, because palette indices can't be averaged. Stage 3's true-colour path fixes that naturally.
- Lighting is the original model: one light value per vertex, run through the game's 8-bit light tables. JK levels have big, low-poly faces, so lighting is blotchy and dynamic lights (saber, blaster bolts, explosions) light whole vertices rather than pools on surfaces. No per-pixel lighting.
- No MSAA. SSAA covers it at a higher cost.
- HiDPI is off by default for new players.

## 2026-11-11 - North star (art direction)

docs/screenshots/northstar-01narshadda.webp is an AI reimagining of the Stage 1 baseline shot (same room, same layout). It's the target look, not a literal spec. What it's made of, and where each part lands:

| Ingredient in the image | Needs | Stage |
|---|---|---|
| Crisp brushed metal, panel seams, rivets | HD albedo plus normal and roughness/specular maps per texture | 3 |
| Highlights on the copper pillar, sheen on the floor | Per-pixel lighting with specular, plus some reflection (cubemaps or screen-space) | 2 |
| Pools of light from the floor grates, glow | Per-pixel dynamic lights, bloom (exists, needs tuning) | 2 |
| Dark corners, contact shading | SSAO (exists, needs tuning) | 2 |
| Filmic contrast, cool teal grade | HDR tone mapping plus a colour-grade LUT (.cube) exported from Resolve/AE | 2 |
| Round handrails, bevelled panels, real ceiling depth | Mostly faked with normal/parallax maps. True silhouettes need level geometry edits (.jkl), which is a big job | 3+ |
| Detailed body and pistol | New models | 4 |

Priority change: per-pixel lighting must support normal and specular maps, and Stage 3 must produce material maps, not just upscaled colour. JKGM currently handles albedo, emissive and displacement only.

## 2026-11-11 - Stage 2: first batch (capture tool, HiDPI, filtering, grading)

**Capture tool (scripts/capture.sh)**
- Autostarts a level, applies display settings, freezes the AI and makes the player invulnerable, waits, times N frames, writes a PNG to logs/captures/ and quits. Muted (silent OpenAL Soft and SDL audio drivers). Runs from its own data root (gamedata/capture, links to the game data), so real profiles and saves are never touched.
- Gotchas found on the way: autostart never loads a player's saved settings (it builds a fresh default profile), so capture settings are applied in-engine from OPENJKDF2_CAPTURE_CVARS. The "press a key" mission text screen blocks autostart unless skipped. HiDPI is a global (registry) setting, not just per player.
- Not perfectly frozen: idle animations and animated textures (blinking signs, floor lights) still move between runs. Fine for before/after shots, worth knowing when diffing.

**HiDPI default**: on for macOS for new installs and new players (WINDOW_DEFAULT_HIDPI). Existing profiles keep their setting.

**Mipmaps + anisotropic**: true-colour textures (16-bit MATs, HD jkgm materials) get a full mip chain and up to 16x anisotropic when texture filtering is on. Filtering off keeps the original crisp look. JK1's own textures are all 8-bit, so there's nothing to see until HD textures load in Stage 3. Couldn't test with the JKR 3.3 pack yet: its installer isn't Inno Setup, so innoextract can't open it.

**Colour grading**
- Off by default (r_enableGrade). When on, the scene composites into a half-float buffer, then one pass applies exposure, a soft highlight roll-off above 0.8 (so bloom eases into white instead of clipping) and a 3D LUT, before the HUD is drawn. The HUD stays ungraded.
- LUTs are standard .cube files (Resolve, After Effects), 3D only. Default path grade/default.cube, relative to the data folder; run.sh links the repo's grade/ folder in. The file is re-checked twice a second and reloaded on change.
- Verified: identity LUT matches the ungraded image (differences only where things animate). Teal test LUT loads and grades as expected. Before/after: logs/captures/compare-teal.png.
- Workflow for real grades: grab an ungraded frame (F12 in game, or a capture with r_enableGrade=0), grade it in Resolve on a Rec.709 timeline, export a 33-point .cube to grade/default.cube.

## 2026-11-11 - North star 2 (art direction)

A second AI reimagining of the same spawn room, same enemy pose as our capture view. File: docs/screenshots/northstar2-01narshadda.webp. Darker and grimier than north star 1: lived-in industrial rather than clean sci-fi. New ingredients beyond north star 1:

| Ingredient | Needs | Stage |
|---|---|---|
| Wet, glossy grime on every surface, sharp highlights | Roughness maps with low roughness plus per-pixel specular. Wetness is mostly a material choice | 2 + 3 |
| Light shafts from the skylight, haze in the air | Screen-space god rays (radial blur from bright sources) plus light distance haze. Fairly cheap post effects | 2 |
| Lens flare streak across the skylight | Anamorphic streak from the bloom buffer. Cheap, but easy to overdo | 2 |
| Darker, desaturated, warm-brown grade with dense blacks | A LUT, plus a lower exposure. Already possible with the grading pass | 2 (done) |
| Detailed corpse and enemy models | New models | 4 |

Open question for Dom: north star 1 (clean, teal, readable) or north star 2 (grimy, wet, dark, atmospheric), or a blend. Affects the default grade, how strong the specular is, and whether haze and god rays are worth building.

**Effect costs (1280x960 Retina window, Apple Silicon, vsync off, 900 frames each)**

| Setting | avg fps | avg ms | p99 ms |
|---|---|---|---|
| Baseline (all off) | 208 / 200 (two runs) | 4.8 / 5.0 | 8.9 |
| Texture filtering | 207 | 4.8 | 8.8 |
| Bloom | 182 | 5.5 | 9.1 |
| SSAO | 177 | 5.6 | 8.8 |
| SSAA 1.5x | 145 | 6.9 | 12.8 |
| SSAA 2x | 208 | 4.8 | 9.1 |
| Grade (LUT + roll-off) | 224 | 4.5 | 8.3 |
| All on (bloom, SSAO, grade, filtering, SSAA 1.5x) | 131 | 7.6 | 9.4 |

- Honest read: everything is cheap at this size. Even all on stays above 130 fps, and p99 stays under 10 ms. Individual costs are mostly inside run-to-run noise: frame times jump between about 4.2 and 8.2 ms as the GPU changes clock states, and SSAA 2x coming out "free" while 1.5x looks slow is that noise, not a real result.
- Good enough to decide: no need for MSAA, and nothing here is too expensive to leave on. Proper per-effect numbers need GPU timer queries and a fullscreen native-resolution run, which is about 5x the pixels. Worth doing once per-pixel lighting adds real load.
- Contact sheet: docs/screenshots/stage2-effects-base-bloom-ssao-all.png (top: base, bloom; bottom: SSAO, all on with the teal test grade).

**Capture mode, final form**: borderless window parked at -30000,-30000, background app (no Dock icon, no focus), never grabs or warps the mouse (logged as mousegrab=0), mouse input ignored, camera re-warped with the head levelled before timing. Hiding the window outright doesn't work on macOS: a hidden window has no drawable and everything renders black.

## 2026-11-11 - Art direction chosen: north star 2 (grimy)

Dom picked north star 2: dark, wet, grimy, atmospheric. What that sets:
- Default grade: darker, desaturated, warm-brown midtones, dense blacks (grade/grimy-test.cube is the starter until Dom grades one in Resolve).
- Materials (Stage 3): low-roughness, wet-looking grime, so roughness/specular maps matter as much as colour.
- Per-pixel lighting must support strong, sharp specular highlights.
- Haze and light shafts from bright sources (skylights, windows) are worth building. Lens streaks: maybe, subtly.
- Grimy grade v2: the first version crushed the low end on top of an already dark game (average brightness 0.055 vs 0.158 ungraded) and Dom couldn't see anything. v2 keeps only 30% of the toe crush and desaturates to 0.7 instead of 0.6: average 0.135, moody but readable. Lesson: the darkness in north star 2 comes from lighting contrast (pools of light against dark), not from crushing the whole image. docs/screenshots/stage2-grimy-grade-test.png now shows original | v1 | v2.

## 2026-11-11 - Stage 2: per-pixel lighting, phase 1

Done, off by default (`r_perPixelLighting 1` in the in-game console to try it; backtick opens the console). Plan: docs/stage2-lighting-plan.md.

**How it works**
- Depth: the render cache already stores 1/(32 x view depth) in each GPU vertex (`D3DVERTEX.nx`). It's passed to the shader as `v_invz` with `noperspective` interpolation (1/depth is linear in screen space), which gives exact camera-space depth per pixel. No change to the CPU vertex pipeline was needed. (The vertex `z` turned out not to be usable: with `rdCache_dword_865258 == 16` it's 1/(depth x farPlane), not a 0-1 depth.)
- Position: inverts `rdCamera_PerspProject` (x = halfW + X*f/Y, y = halfH - Z*f/Y) using `gl_FragCoord`, canvas size and focal length. Normal: flat face normal from screen-space derivatives of that position.
- Lights: every world draw uploads the camera's light list (up to 32), transformed to camera space with `rdCamera.orient`. Same linear falloff as the original (intensity - distance x attenuationMax, cut at maxRadius). Diffuse is slightly wrapped, because the original applied dynamic light to level geometry with no angle term at all.
- Highlights: Blinn-Phong, shininess 8 to 128 from `r_wetness` (default 0.6), strength `r_specular` (default 1.0). None on see-through, additive or fullbright faces. Highlights also feed the bloom buffer, so wet glints glow.
- No double lighting: with per-pixel on, `sithRender_BuildDynamicLights` leaves the per-vertex dynamic term at zero and `rdModel3` skips gathering camera lights. Baked static light is untouched.
- Important correction to the plan: on the GL path, lighting isn't applied through the 8-bit light tables; it arrives as the per-vertex colour (`f_color`). The light tables only feed the glow buffer. So per-pixel light adds to that colour.

**Verification (silent captures, spawn room)**
- Test light: capture-only `OPENJKDF2_CAPTURE_LIGHT="x y z intensity radius"` injects a fixed dynamic light into the normal light list, so both paths light the same thing.
- With the test light: the per-vertex original barely shows it (spread thin across big faces). Per-pixel gives a clear pool on the far wall and a wet glint on the ceiling ring. docs/screenshots/stage2-perpixel-light-before-after.png.
- Brightness match: whole-frame average 0.174 per-vertex vs 0.181 per-pixel (+4%). With no test light, models in the room (the body, the gun) come out 3-5% brighter per-pixel. The level has a dynamic light here, now applied with a different shape. Acceptable.
- Frame rate: no measurable cost at 1280x960; run-to-run noise (135-173 fps) is bigger than any difference.

**Known limits / next**
- Flat derivative normals: correct on JK's flat level faces, faceted on models. Fine for now; Stage 4 models can bring real normals.
- Light colour is white for JK1. MotS coloured lights plug into the colour uniform later.
- Needs a play session from Dom (blaster fights, sabers, explosions) before it's switched on by default.
- Phase 2 (highlights from skylights and glowing surfaces) is what makes the static scene look wet. Today's highlights only come from dynamic lights.

## 2026-11-11 - Capture views moved to later levels

Dom asked for screenshots from later in the game. Standard set from now on (`scripts/capture_set.sh NAME [settings]`, silent, one contact sheet per run), each at level start:
- 12escape: dark hangar, shuttle, glowing red floor strips and white wall strips
- 17asarris: outdoors under a stormy sky
- 20aboc: Boc with his saber lit (a live dynamic light). Boc still moves between runs (his cog drives him even with AI frozen), so this one isn't pixel-identical.

Dom also supplied AI reimaginings of these exact views (plus 07yun, 10cargo, 14tower). Common to all of them: wet, reflective floors that mirror light strips, sky and objects; coloured light from sources (Boc's saber throws a pink pool); haze; moonlit sky light.

## 2026-11-11 - Stage 2: glow lights (phase 2), experimental, off by default

`src/Engine/sithGlowLights.c`: at level load, finds glowing level surfaces (texels that stay bright at the darkest light level of the world's light table; the dark end is detected from the data: level 0 in JK1) and places one light per glowing surface, coloured by those texels. Each frame the nearest 24 in visible sectors go to the shader as highlight-mostly lights. `r_glowLights` (needs `r_perPixelLighting`). `OPENJKDF2_GLOW_DEBUG=1` prints what's gathered.
- 12escape: 26 glowing palette colours, 319 glow lights from 4335 surfaces.
- First pass did nothing visible (0.4% change): strength was tied to how much of the texture glows, so thin strips on big floor textures clamped to the minimum, and lights sat flush on their own surface so the floor around a strip got no light. Fixed by fixed strength (0.5 to 1.0) and a 0.12 offset off the surface.
- Second pass: visible, but as soft pinkish blobs on the walls next to the light strips (docs/screenshots/stage2-glowlights-hangar-test.png). Doesn't read as wet, and still no glints on the floor.
- Conclusion: the wet look in every goal image is reflections, not point-light highlights. Floors mirror the strips, the shuttle and the sky. The technique for that is screen-space reflections. Glow lights stay in the engine, off by default; their positions are still useful as sources for haze and light shafts.

**Next candidates**
- Screen-space reflections on glossy surfaces (the wet floors). The shader already rebuilds camera-space position and normal per pixel, so these can go into the existing G-buffer targets for a post pass.
- Coloured light for sabers (and other coloured dynamic lights): currently white.
- Haze and light shafts (phase 3).

## 2026-11-11 - Coloured saber light, update check off

**Saber light colour (needs r_perPixelLighting)**
- The original only lights the player's saber (things flagged EMITLIGHT). Boc's sabers emit nothing in the game data, so the pink pool in the goal images needed an addition: with per-pixel lighting on, every AI actor with a lit saber (JKFLAG_SABERON, a saber polyline) casts a light in its blade's colour. Off = original behaviour.
- Position: AI wielders don't keep `saberBladePos` up to date, so the light sits at the wielder's position. The pool lands on the floor around them, which is what the goal images show.
- Colour: average of the blade material's clearly saturated texels (a blade is a white core with coloured edges; a plain average comes out white). The player's custom saberrgb colour wins when enabled. Colours live in a side table beside sithRender_aThingLights, not in rdLight.color, which MotS lighting uses.
- Result: Boc throws a pink pool on the floor (docs/screenshots/stage2-saber-light-colour.png). The player's saber is now properly green.
- Debug: OPENJKDF2_PPL_DEBUG=1 logs the light list every 120 frames, actors seen by the gather, and saber colour lookup failures.

**Update check off by default**: the engine checked GitHub for upstream releases on every launch (Little Snitch flagged it each time, as every rebuild changes the checksum). This fork shouldn't offer upstream builds anyway. `net_disableUpdates` now defaults to 1 and is set in the local saved settings.

## 2026-11-11 - Screen-space reflections

`r_reflections 1` (needs `r_perPixelLighting 1`; strength `r_reflectionStrength`, default 1; amount follows `r_wetness`). Off by default until playtested.

**How it works**
- The world shader writes a fifth render target (`tex4`, RGBA16F): octahedral camera-space normal, depth (negative = surface doesn't reflect), alpha 1 on solid surfaces. See-through faces write all zeros with alpha 0, so the blend modes leave the buffer untouched.
- A post pass (`ssr_f.glsl`) runs after the scene is composited and before bloom. Per reflective pixel it rebuilds the position, marches the reflection ray in camera space (48 growing steps, then a 6-step binary refine against the stored depth), and adds the scene colour where it hits. Floors (normal close to world up) reflect most, walls 20%, with Fresnel boost at grazing angles and a fade at screen edges.
- Debug view: a negative `r_reflectionStrength` colours pixels instead: green = hit, red = reflective but missed, blue = not reflective, cyan = amount ~0.

**Bugs found on the way (worth remembering)**
- Shaders are compiled into the binary at build time (embedded resources). Copying an edited .glsl into the app bundle does nothing; rebuild.
- Solid surfaces draw with blend mode `D3DBLEND_ONE`; only flags 0x600 (`SRCALPHA`/`INVSRCALPHA`) are see-through. Treating `ONE` as additive silently disabled reflection data, and the wet highlights, on every solid surface.
- "Vertex light at full brightness" is not a test for light sources: brightly lit floors are at full light too. Removed from both highlights and reflections.
- Capture runs now set every remaster lighting setting explicitly; a leftover `r_glowLights` from an earlier run had leaked into a "reflections off" capture.

**Result** (docs/screenshots/stage2-reflections-off-on.png, top off, bottom on): the hangar floor faintly mirrors the shuttle and wall strips; the Sarris rooftop reflects the figures in the wet stone. Subtle at default wetness 0.6, which suits a first pass. No measurable cost at 1280x960.

**Limits**: only what's on screen can reflect (screen-space). Reflections of the sky and off-screen lights need environment probes later. The north star images go much further (glossy material maps, haze, stronger contrast), which is Stage 3 materials plus phase 3 atmosphere.

Reference images added: northstar-later-levels-a/b (the later-level views reimagined), northstar-dream-saber-room and -2 (the saber room as a long-term mood target: saber-coloured pool, haze, red rim light).

## 2026-11-11 - Stage 2 phase 3: haze and light shafts

Both off by default, both need `r_perPixelLighting 1` (they read the depth target the world pass writes; it's now written whenever reflections, haze or shafts are on).

**Haze** (`r_haze`, `r_hazeDensity` default 0.08 per world unit, `r_hazeStrength`): fog = 1 - exp(-density x depth), capped at 0.75, alpha-blended over the scene after reflections and before bloom. Its colour comes from the scene blurred twice (1/4 then 1/16 resolution) plus a small dark base, so haze glows with nearby light (pink near a saber, red near red strips) instead of flat grey. Where nothing was drawn it counts as 40 units away.

**Light shafts** (`r_lightShafts`, `r_shaftStrength`): screen-space light scattering towards up to 4 glow lights (phase 2 positions): in front of the camera, near the screen, ranked by intensity over distance. Each pixel samples 32 steps towards each light with decay 0.95, accumulating the glow buffer plus a bright-pass of the scene (luminance above 0.7, so white strips and sky count), fading with screen distance from the light. A per-pixel jittered start removes 32-step banding.
- First version was nearly invisible (2% change at 4x strength) and banded: the glow buffer alone is mostly dark. The bright-pass and jitter fixed both.

**Result** (docs/screenshots/stage2-haze-shafts-off-on.png, left off, right on): hangar gets a hazy glow around the shuttle and soft light spilling from the wall strips; the Sarris rooftop's sky and far building sit back in hazy air. Subtle, which suits defaults. No measurable frame cost at 1280x960. Note: the two Sarris shots aren't framed identically (that level's start view shifted between runs).

**Not yet**: shafts don't check occlusion (depth target is bound but unused), and there's no sky-sourced shaft (the Sarris storm or a skylight) since the sky isn't a glow light. Both are natural next steps if the look calls for stronger rays.

## 2026-11-11 - One-switch remaster preset

Console command `remaster 1` turns on the whole Stage 2 look with tuned values; `remaster 0` returns to the original game; `remaster` on its own reports what's on. It sets the individual cvars (so anything can still be fine-tuned afterwards) and saves the player profile. Capture runs accept `remaster=1`.

Preset values (in `jkPlayer_aRemasterPreset`, jkPlayer.c): per-pixel lighting on (specular 1, wetness 0.6), reflections on (strength 1), haze on (density 0.08, strength 1), light shafts on (strength 1), grade on with grade/default.cube (the grimy starter), bloom on, SSAO on. Glow lights stay off (experimental). `remaster 0` switches those features off; strengths and the LUT path are left at their defaults.

Result: docs/screenshots/stage2-remaster-preset-off-on.png (top original, bottom preset). Frame rate the same either way at 1280x960 (~147 fps in that run). The 1997 textures are now the most obvious gap; Boc's saber pools may be a touch hot and are the first thing to judge in a playtest.

## 2026-11-11 - Stage 3 start: HD textures

**OpenJKDF2 already has an HD texture loader (jkgm, from JKGFXMOD).** Packs live in `<game data>/jkgm/materials/<pack>/metadata.json` plus PNGs. Each entry has `albedo_map` (colour), optional `emissive_map` (glow), optional `displacement_map` (parallax), tint factors, and says what it replaces either by name (`"replaces": [{"name": "mat/x.mat", "cel": 0}]`) or by `replaces_signatures` (MD5 of the original texture's pixels converted to 16-bit, plus width and height). No normal or roughness maps; those would be our addition later. Loaded textures now get mipmaps and anisotropic filtering (Stage 2 work).

**Extraction tool: `tools/mat_extract.py`.** Reads Res2.gob, Res1hi.gob and JK1.GOB directly (later ones override, like the engine) and writes every cel of every MAT to `hd/extracted/<mat>.cel<N>.png` at full resolution, with `manifest.json`. 1934 MATs, 2155 PNGs, 0 errors, 8 seconds. 8-bit textures use a colormap palette (default `dflt.cmp`; `--cmp` for others; magenta texels are palette entries only meaningful in other levels' colormaps). Pure Python, no dependencies. MAT layout: 76-byte header (magic, revision, type, texinfo count, texture count, 14-word format), 24-byte texinfo records (+16 if bit 8 set), then per texture a 24-byte header (width, height, alpha, 2 unknown, mips) and all mip levels.

**Jedi Knight Remastered 3.3 pack.** Its installer isn't Inno Setup; 7-Zip (`brew install sevenzip`, `7zz x -t#`) splits it into 1391 raw chunks: bzip2 streams, plus zlib streams behind a 0x01 marker byte. Inside: 1055 PNGs, four jkgm metadata files (the main "Jedi Knight Neural Upscale Pack" with 2010 entries, plus jkgfxext1, saber19, projectile19), and Windows OpenJKDF2 binaries. The PNGs have no names (the file table is encoded somewhere not yet found), and the installer carries fewer images than the metadata lists (1055 vs 2414).
- `tools/jkgm_rebuild.py` matches unnamed PNGs to metadata entries by comparing 16x16 thumbnails with the original texture each entry replaces (same aspect ratio, each PNG used once). Glow maps only go with an entry whose colour map matched. Entries without a confident match are dropped from the written metadata, so those textures keep their originals.
- Calibration by eye: error < 0.02 is reliably right; around 0.045 about half are wrong. Cut-off 0.03. Result: 555 entries (607 images, 230 MB) in `hd/jkgm/materials/jk_neural_upscale_pack`, 2 in jkgfxext1.
- `scripts/build_hd_pack.sh <installer.exe>` redoes the whole thing. `scripts/run.sh` links `hd/jkgm` into the game data; captures use it unless `CAPTURE_HD=0`.

**Result** (docs/screenshots/stage3-hd-pack-off-on.png; top originals, bottom HD; both with `remaster=1` and texture filtering on): the Sarris building panels and Boc's rock wall get real HD detail. The hangar barely changes (few of its textures matched). No load errors, no frame cost.

**Open questions**
- Where the other ~1360 images are. Possibly in JKR 1.0/2.0.1 (in the Trash, recoverable) or a file table I haven't decoded. Worth checking before upscaling those textures ourselves.
- The upscale pack is ~2020-era neural upscaling: good detail, but colour only. The grimy wet look needs roughness (and ideally normal) maps, which neither the pack nor the loader has yet.

## 2026-11-11 - Stage 3: more of the pack, palettes, and the CD

**Sources.** Remastered 2.0.1 adds 1462 PNGs (JKR 1.0 is byte-identical to 2.0.1). With 3.3 that's 1838 unique images for ~2400 pack slots, so ~600 textures aren't in any installer. Same-name packs from different installers are merged.

**Matching, second round**
- A palette-independent structure pass (brightness-pattern correlation plus a brightness-difference penalty, cut-off 0.12) placed ~190 more, and the colour pass now also requires the brightness pattern to correlate (plain colour distance let any two dark textures "match").
- But it exposed the real problem: **level palettes.** Each level colours textures through its own colormap (21 in JK1), so one MAT can be lit city windows in one level and dark wall panels in another. The engine checks MAT names before hashes, and a name match applies one image in every level: the hangar's red floor strips turned yellow, its walls became city windows, and Boc's wall blew out orange.
- Hash-only with the pack's own signatures fixed that but dropped almost everything (the engine's hashes rarely equal the pack's recorded ones).
- **`tools/jkgm_signatures.py`**: for each entry, renders the original under every level palette, keeps the palettes that reproduce the HD image, and writes the engine's own hash for each (MD5 over width, height and pixels converted to 16-bit through the palette, 565 for alpha textures, 1555 otherwise, as jkgm_get_tex_hash does). Names are dropped. 950 entries, 2197 signatures.
- Result: palette-correct and safe (hangar untouched, nothing wrong anywhere in the capture set), but few textures apply in these three views; the Sarris building's HD panels that showed with name matching don't appear. **Next step:** confirm my hashes equal the engine's (add a debug print of jkgm_get_tex_hash for textures in view) before trusting coverage numbers.
- `scripts/build_hd_pack.sh` now takes several installers and runs rebuild then signatures.

**Original CD-ROM (Dom's boxed copy).** Two disc rips (data + CD audio tracks, cue sheets) and box/manual scans. Disc 1's data track (MODE1/2352) converts to ISO by keeping bytes 16..2064 of each sector. Res2.gob and Res1hi.gob are byte-identical to GOG's; JK1.GOB and JK.EXE differ (GOG ships the 1.01 patch). So the extracted textures are the genuine originals. The CD audio is the original soundtrack, lossless; OpenJKDF2 reads CD numbering (MUSIC/1/TrackN, MUSIC/2/TrackN), so a higher-quality soundtrack encode is possible later.

## 2026-11-11 - Known issue: everything is equally shiny (Dom's note)

Rock gets the same gloss as metal: `r_wetness` is one global value, because the engine has no idea what a surface is made of. Rocks aren't shiny like this even when wet. Proper fix: per-texture roughness maps. Texture God (Dom's pipeline) outputs roughness/ORM per texture, so the plan is jkgm loader + shader support for a roughness map (and later normal maps), with a sensible default (fairly rough) for textures without one. Until then, a lower global wetness is the stopgap.

## 2026-11-11 - Engine fix: HD texture hashes now use the level's palette

**Root cause of the low HD coverage.** OpenJKDF2 hashes each texture (for HD pack signatures) while a level loads, using `worldpal_data`. That buffer is only refreshed when a frame is drawn, and the loading bar keeps overwriting it with UI palettes, so hashes were computed with a stale palette and almost never matched. Shared textures first loaded by the menu world (static.jkl, which has no colormap) were hashed against the buffer's initial contents: all white (0xFF). Proven with a debug log (`OPENJKDF2_JKGM_DEBUG=1`: name, cel, alpha, size, hash, match type per texture) compared against `tools/jkgm_signatures.py`.

**Fix** (jkgm.cpp/.h, sithWorld.c): the HD loader gets its own hash palette (`jkgm_set_hash_palette`). Before a level's text is parsed, `sithWorld_SetHashPaletteFromLevel` reads the level file, finds the first `world colormaps` entry (the georesource section comes after materials, so it has to be found up front), loads that colormap and hands it over. Levels without one (the menu world) use dflt.cmp. Falls back to the old behaviour if nothing is set.

**Result.** Sarris view: my tool reproduces the engine hash for 292/292 textures (was 103), and 61 textures load HD replacements (was 0). Boc's room: 84 HD textures. Hangar and Sarris still show no wrong textures. docs/screenshots/stage3-hd-hashfix-off-on.png (the Boc pair isn't comparable: the boss script advanced to a different framing with HD on).

**Performance problem found.** Boc's room with HD textures plus the full remaster look: ~33-40 fps at 1280x960 (vs 112 with HD but no remaster, ~120 with remaster but no HD). Bisect, one effect off at a time: bloom off 61, per-pixel lighting off 53, haze off 47, reflections off 46, SSAO off 42, shafts off 40. Cumulative, not one culprit: heavier main pass with HD (more texture bandwidth, 16x anisotropic) plus every full-screen pass. Next: half-res bloom first level and reflections, lower anisotropy, GPU profile in Xcode, then re-measure at full-screen Retina (about 5x these pixels).

## 2026-11-11 - Performance pass 1 (HD textures + full remaster look)

Changes, none of which change the look (checked in the hangar; Sarris differs only by its unstable start camera):
- Bloom's first blur at half resolution (was full: the blur shader takes 49 samples per pixel). Radius halved to match.
- Scene colour texture capped at 2 mip levels. The haze pass rebuilds its mips every frame, and the default meant a full chain of the whole scene each time.
- Haze blurs use 8 directions x 2 rings (17 samples, was 49): haze colour is very soft anyway.
- Light shafts: 16 steps (was 32) with stronger per-step decay to cover the same distance, and lights too far from a pixel to contribute are skipped. This was the heaviest pass: up to 4 lights x 32 steps x 2 texture reads per pixel.
- Reflections: 32 marching steps with faster growth (was 48) and a 4-step refine (was 6).
- `r_anisotropy` (default 8, range 1-16) caps anisotropic filtering, which was 16x.

**Not measured yet.** During this work a Texture God FLUX job was running on the GPU (another session), so timings were meaningless (the hangar read 27-31 fps against 175 earlier with the same settings). Re-measure the Boc room, hangar and Sarris with the GPU idle before deciding on pass 2 (half-res reflections, GPU profile in Xcode, full-screen Retina numbers). Rule for later: don't benchmark while Texture God is running.

## 2026-11-11 - Reflection fixes from Dom's playtest screenshots

Dom: reflections in the hangar smeared diagonally off the wall light strips ("surely they should go straight down"), and rock reflected like a mirror.
- **Streaks:** an SSR artifact. A ray skimming past a thin light strip counted as a hit from behind, and every floor pixel along that line took the same strip end. Fixed: tighter depth tolerance (max(0.01, step) instead of max(0.02, 2x step)), only surfaces facing the ray count as hits (dot(hit normal, ray) < -0.05), and long rays fade out.
- **Mirror rock:** reflections were perfectly sharp on everything. Now glossy: the shader mixes in a blurred copy of the scene (the same 1/4-res blur the haze uses, now built once when reflections or haze are on) by `r_reflectionRoughness` (default 0.5; walls and slopes +0.3; longer rays blurrier), and rough surfaces reflect a bit less. Reads as a wet sheen, not a mirror.
- Preset wetness lowered to 0.4 (was 0.6), since one gloss value still applies to every surface. Per-texture roughness (from Texture God) is the real fix.
- Before/after: docs/screenshots/stage2-reflection-streak-fix.png (hangar), stage2-reflection-glossy-rock.png (Sarris; framing differs between runs).

## 2026-11-11 - Material maps: roughness, metalness, AO, normals

**Pack format extension** (optional, backwards compatible): `orm_map` (glTF convention, R = ambient occlusion, G = roughness, B = metalness) and `normal_map` (tangent-space, OpenGL +Y). Texture God outputs exactly these.

**Engine**
- jkgm loader: parses both, decodes and uploads them with mips and anisotropic filtering (always smooth-filtered) via `jkgm_load_extra_map`. New surface fields `orm_texture_id`, `normal_texture_id`; reset, freed and refreshed wherever the displacement map is.
- Packs now load in alphabetical order and later ones win where they overlap, so `texturegod` overrides the community `jk_neural_upscale_pack`.
- Renderer binds ORM on texture unit 5 and normal on unit 6 per surface, with `u_hasOrm` / `u_hasNormal`. A sixth render target (`tex5`, RGBA8: roughness, metalness) carries material to the reflection pass.
- Shader: roughness sets the highlight exponent (2/r^4 - 2) and strength ((1 - r)^2); metal tints highlights with the surface colour and cuts diffuse; AO darkens lighting; the normal map bends the face normal through a tangent frame built from screen derivatives of position and UV (no tangents in JK's vertex data). Surfaces without maps default to roughness 0.75, non-metal, no AO, so rock and plaster stop reading as polished. `r_wetness` now smooths the whole world on top of the material (roughness x (1 - 0.6 x wetness)) instead of being the only gloss control.
- Reflections take each pixel's roughness from the material target: smooth surfaces reflect, rough ones barely, floors still favoured.

**Importer: `tools/texturegod_import.py <Texture God run>`**. Reads the run's manifest (keyed `<mat>.cel<N>.png`), imports albedo/ORM/normal/emissive for every texture that passed QC into `hd/jkgm/materials/texturegod`, then runs `jkgm_signatures.py` for palette-correct engine hashes.

**Test** with Dom's fast run (`out/jk_fast`, PBRify only): 30 imported, 29 matched a palette (66 signatures). Level 1 spawn view: 10 Texture God textures confirmed loading via `OPENJKDF2_JKGM_DEBUG`. The visible difference is small (2.8%), as expected from the colour-locked fast pass (docs/screenshots/stage3-texturegod-fast-zoom.png). Defaults on everything else: hangar floor reflects a little less, Sarris rock highlight broader and dimmer (docs/screenshots/stage3-material-defaults.png). Frame rate unaffected (hangar 152, Sarris 164 fps, GPU idle).

**Unverified**: normal-map handedness (JK's V direction vs the PNG's) can't be judged on the fast pass; check on the first full Texture God run, where bumps should light from the correct side. If inverted, flip the green channel in the shader.

## 2026-11-11 - Stage 5: HD cutscenes

**Originals.** 25 Smacker files in `Resource/VIDEO` (GOG copy), all 640x300, 15.0015 fps (66,660 us per frame), 22 kHz stereo, 32.8 minutes in total. Each SMK also carries a data track with subtitle indices per frame, which the engine draws in the bar under the picture.

**Extraction: `scripts/extract_cutscenes.sh`** writes `hd/video_src/<NAME>.mov`: ProRes 422 HQ, 10-bit, native size and rate, PCM audio, converted to Rec.709 and tagged so grading apps read it right. 3.5 GB, about 20 s for all. Checked against a direct decode: identical apart from 4:2:2 chroma on hard edges.

**Playback design.** The SMK still plays as before and stays the master clock, the audio and the subtitle source. If `hdvideo/<NAME>.mov|mp4|m4v` exists in the game folder (run.sh links it to `hd/video`), its picture replaces the SMK's. The HD frame is chosen by time: SMK frame index x 66.66 ms, plus wall-clock time since that frame (capped at one frame), so higher frame rates play smoothly. So:
- resolution, frame rate and codec are free (H.264, HEVC, ProRes; anything AVFoundation reads);
- the HD file must keep the original's length. Interpolate to 23.976 fps, don't conform/retime;
- aspect: always drawn into the original picture area. If the HD file's shape differs (Topaz fits 640x300 into 1920x1080 by adding 90 px bars top and bottom), the centre is cropped back to 2.13:1, so the bars vanish. Verified with a letterboxed 1080p test file: picture lands exactly on the original area;
- audio still comes from the SMK. An HD audio track is ignored for now (could be added if the audio gets remastered);
- `r_hdCutscenes 0` turns it off for A/B.

Decoding is AVFoundation (`src/Platform/macOS/jkCutsceneHD_avf.m`): hardware HEVC/H.264/ProRes, no new dependencies, BGRA frames uploaded to one GL texture (with mips) and drawn with the UI shader in place of the SMK picture in `std3D_DrawMenu`. macOS only; other platforms compile it out and play the originals.

Rejected: FFmpeg libraries (big dependency, licensing, nothing gained on a Mac); replacing the SMK entirely (would lose subtitles and the existing audio sync).

**Test**: the capture tool can now shoot cutscenes (`CAPTURE_VIDEO='./Resource/VIDEO/03-04A.SMK'`, plus `CAPTURE_HDVIDEO=<folder>`); the engine's capture timer now also runs during cutscene frames. A 1920x900 HEVC 23.976 fps test file (black and white with a red box, `hd/video_test`) replaced 03-04A in place, with frame rate unchanged (63 fps menu-capped either way). docs/screenshots/stage5-hd-cutscene-test.png.

Not yet checked: playing a full cutscene with sound end to end in a real session, and pausing (the HD frame holds; resumes with the SMK).

**Importing renders: `python3 tools/import_cutscenes.py [folder]`** (default hd/video_src). Finds renders named `<NAME>_<anything>.mp4|mov|m4v` (Topaz writes e.g. `01-02A_chf3_prob4.mp4` next to its source), skips ones still rendering (Topaz temp names `NAME_<digits>.mp4`, or modified in the last 30 s), checks each opens and matches the original's length within 0.5 s, then hard-links the newest into `hd/video/<NAME>.<ext>`. Re-run it any time; it replaces older imports.
