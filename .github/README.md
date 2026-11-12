# Jedi Knight: Dark Forces II, remastered for Apple Silicon

**A personal project to rebuild the look of Star Wars Jedi Knight (1997) on a modern Mac, while keeping it unmistakably Jedi Knight.**

![Today vs target: level 1 as it runs now (left) and the art direction it's heading towards (right)](../docs/screenshots/hero-today-vs-target.webp)

*Left: the game today, running natively on Apple Silicon. Right: the art direction (AI concept art of the same room, used as a north star, not a promise).*

---

## What this is

Jedi Knight is one of my all-time favourite games, and this fork is where I'm remastering it: native on Apple Silicon first, then modern lighting, HD materials, new models and upscaled cutscenes. The goal is the game as you remember it, rather than how it actually looked in 1997.

The engine is [OpenJKDF2](https://github.com/shinyquagsire23/OpenJKDF2), the community's function-by-function reimplementation of the original engine. All the hard reverse-engineering credit goes to that project. Everything here is built on top of it and kept in sync with it.

**What it isn't:** a new game, a mod pack or a commercial product. **No game assets are included or ever will be.** You need your own copy of the game (the GOG version is recommended).

## Art direction

The target is dark, wet, grimy and atmospheric: lived-in industrial rather than clean sci-fi. Light pools on wet metal, haze in the air, light shafts through skylights, dense blacks and a desaturated, warm-brown grade.

That breaks down into concrete engine work:

| The look needs | How | Status |
|---|---|---|
| Lights that pool on surfaces and glint off wet metal | Per-pixel lighting with specular highlights | Done (playtesting) |
| Wet floors reflecting light strips, sky and objects | Screen-space reflections | Done (playtesting) |
| Haze and light shafts | Volumetric fog lit by the baked light, screen-space light shafts | Fog on all 25 levels (playtesting); height fog, smoke and steam [next](../docs/stage2-atmosphere-plan.md) |
| Filmic colour | Colour grading pass with live-reloading .cube LUTs from Resolve | Done |
| Glowing lights and sabers that roll off softly | Bloom (built in) into a half-float buffer with highlight roll-off | Done |
| Every shot lit like a film: side key light, rim light on every character, nothing flat | Light bake with recovered designer lights (Blender Cycles); light probes so characters take the baked light's colour and direction, plus key, rim and floor light | All 25 levels baked: light maps and light probes in game (playtesting) |
| Crisp, detailed, grimy surfaces | HD textures with roughness and normal maps | Stage 3 |
| Detailed characters | New models through a Blender pipeline | Stage 4 |

## Progress

| Stage | What | Status |
|---|---|---|
| 1 | Native build on Apple Silicon, one-command build and run | Done |
| 2 | Rendering: Retina, filtering, colour grading, per-pixel lighting, atmosphere | In progress |
| 3 | HD textures with material maps, true-colour replacement loader | In progress |
| 4 | Models: Blender import/export for .3DO and .KEY, new Kyle and stormtroopers | In progress |
| 5 | Cutscenes: upscaled and graded, played from modern video files | In progress |
| 6 | Mysteries of the Sith: the same treatment for the expansion | Not started |

### Stage 2 checklist

- [x] Audit what the renderer already offers (bloom, SSAO, SSAA, filtering, FOV and frame cap are all built in)
- [x] Silent, repeatable screenshot and frame-timing captures for before/after comparisons
- [x] Retina (HiDPI) by default on macOS
- [x] Mipmaps and anisotropic filtering for true-colour textures
- [x] Colour grading: exposure, highlight roll-off, live-reloading .cube LUTs
- [x] Measure the cost of bloom, SSAO and SSAA (130+ fps with everything on)
- [x] Art direction chosen
- [x] Per-pixel lighting with wet highlights for dynamic lights ([plan](../docs/stage2-lighting-plan.md)), being playtested
- [x] Highlights from glowing surfaces: built, but reads as blobs rather than wet; off by default
- [x] Screen-space reflections for wet, reflective floors (being playtested)
- [x] Coloured light from sabers, including enemy sabers
- [x] Haze and light shafts (being playtested)
- [x] Volumetric cloud sky with moon shafts on the storm levels (Maw, Sarris, the ascent); other skies to follow
- [x] Cinematic key and rim light on characters and props (first pass)
- [x] Light bake in game: baked light maps replace the 1997 corner light on six test levels (`r_lightmaps`, in the remaster preset) ([plan](../docs/stage2-lightbake-plan.md))
- [x] Light probes: characters and props lit by the bake around them (colour, direction, floor light)
- [x] Light bake on all 25 levels: light maps, light probes and light volumes
- [ ] Light bake tuning (sun for day levels)
- [x] Volumetric fog lit by the baked light (`r_volumetricFog`, all 25 levels)
- [x] Height fog on outdoor levels, distance fog
- [x] Fog volumes placed by hand in Blender (`tools/blender/atmos_volumes.py`)
- [x] Smoke and steam emitters, placed in Blender, lit by the bake
- [ ] Atmosphere, continued: dust in the light, shaft occlusion ([plan](../docs/stage2-atmosphere-plan.md))
- [x] One-switch preset: `remaster 1` in the console

### Stage 3 checklist

- [x] Research: OpenJKDF2 already loads HD texture packs (colour, glow, displacement maps)
- [x] Tool to extract every original texture to PNG (`tools/mat_extract.py`)
- [x] Rebuild the community Neural Upscale pack from its installer (555 textures matched so far)
- [x] Rebuild from all three Remastered installers, palette-correct (no wrong colours in other levels)
- [x] Engine fix: HD texture hashes use the level's real palette (coverage went from almost none to dozens per view)
- [x] Performance: full remaster + HD runs at 156 fps on the MacBook's native screen; a 7680x3240 ultrawide gets automatic render scaling (52 -> 77 fps). The earlier slowdowns were Topaz and Texture God sharing the GPU
- [x] Material maps in the engine: roughness, metalness, AO and normal maps per texture (rock defaults to matte)
- [x] Importer for Texture God output (my own AI upscaling and PBR pipeline)
- [x] First full Texture God batch in game (217 level textures, gritty look, own pack)
- [x] Texture God on every level, including per-level palette variants (507 textures in game)
- [ ] Roughness and normal maps for the wet, grimy look (loader support plus generation)
- [ ] Upscale and hand-finish key textures (hangar, Nar Shaddaa, Kyle)

### Stage 4 checklist

- [x] Blender tools: the community [blender-sith](https://github.com/smlu/blender-sith) add-on, tested on Kyle and the stormtrooper (model and animation in and out of Blender 5.3, identical in game)
- [x] Tool to pull any model, animation or texture out of the game archives (`tools/gob_extract.py`)
- [x] Engine limits mapped: 512 vertices and 512 faces per body part, 64 joints
- [x] Raise the per-part limits for detailed models (8192 vertices per mesh)
- [x] Skinning: one continuous mesh bends with the original skeleton, so every original animation drives modern characters (tested pixel-identical)
- [x] Fit Jedi Academy's Kyle onto JK's skeleton (tools/blender/ja_to_jk.py): in game, driven by the original animations
- [x] Stormtrooper from Jedi Academy (keeps JK's own rifle as a rigid part)
- [x] The cast from Jedi Academy: officers, commandos, Grans, Rodians, Trandoshans, Ugnaughts (18 JK models)
- [x] Animation check: crouch, fire, jump, punch and saber swing drive the new Kyle cleanly
- [ ] Named characters (Jerec and the Dark Jedi, Jan) and civilians
- [ ] Material pass on the converted characters (Texture God) - Kyle done (roughness/AO/normal maps)
- [ ] New Kyle, built in the original's 16 rigid parts so every existing animation still drives him
- [ ] New stormtrooper
- [x] New lightsaber: Kyle's hilt from Jedi Outcast in third person, first person and the pickups
- [x] Third-person weapons no longer carry the 1997 hand
- [x] Glowing plasma blades: white-hot core, coloured sheath and halo, animated, lighting the room (`r_saberGlow`)
- [x] Lightsaber swing trails (only on real swings)
- [x] First-person hands: Kyle's gloves and sleeves on every weapon view
- [x] First-person hands v2: bare hands (free PSX arms pack), rounded sleeves, Texture God jacket

### Stage 5 checklist

- [x] Extract all 25 cutscenes (640x300, 15 fps, 33 minutes) to ProRes for upscaling and grading (`scripts/extract_cutscenes.sh`)
- [x] Engine plays HD replacements (H.264, HEVC or ProRes, any resolution or frame rate) in place of the originals, with the originals as fallback and subtitles kept
- [x] Upscale all 25 (Topaz, 3x to 1920x900, original 15 fps; interpolation looked janky)
- [ ] Grade in After Effects
- [ ] Playtest every cutscene in game

## Latest

Every comparison below is the real 1997 game (original models, textures and lighting) on the left and the remaster on the right. Work in progress lives on feature branches until it's been playtested; the newest items are on [`stage2-atmosphere`](https://github.com/domdino/OpenJKDF2/tree/stage2-atmosphere).

**Baked lighting, light probes and volumetric fog.** Every level's lighting has been re-baked in Blender's Cycles path tracer: the level designers' lights (recovered from the 1997 lighting), every glowing strip and lamp as a real light, and sky light through openings, with shadows and bounce light. Levels use the result as light maps; characters and props take their light from baked light probes (colour, direction, a rim from the light behind them and the glow of the floor under their feet); and the air is filled with volumetric fog lit by the same bake, so it glows red over the hangar strips and stays dark between lights. All 25 levels are baked.

![Hangar: the 1997 game vs the remaster](../docs/screenshots/stage2-baked-light-hangar.jpg)

Six levels, 1997 vs remaster:

![Baked lighting on six levels](../docs/screenshots/stage2-lightmaps-in-game.jpg)

**Everything together:** HD textures, Jedi Academy's cast on the original skeletons, per-pixel lighting, wet reflections, bloom, ambient occlusion, a filmic grade and the new skies (open the console with backtick and type `remaster 1`; `remaster 0` for the original):

![The 1997 game vs the remaster](../docs/screenshots/stage2-1997-vs-remaster.jpg)

**Bloom and lens flares:** bright light fixtures (skylights, lamps, light panels) bloom, and everything bright throws a film-style anamorphic streak and lens ghosts:

![Bloom and lens flare, 1997 vs remaster](../docs/screenshots/stage2-bloom-flares.jpg)

**Fog placed by hand:** a Blender add-on loads any level, lets me drop boxes and ellipsoids of fog over it and exports them for the game. First use: low mist rolling across the hangar floor, lit red by the strips:

![Hand-placed floor mist in the hangar](../docs/screenshots/stage2-fog-volumes-hangar.jpg)

**Smoke and steam:** emitters placed in the same Blender add-on, drawn as soft puffs that billow and rise, fade into floors and walls, and take the baked light around them:

![Smoke and steam in the hangar](../docs/screenshots/stage2-smoke-hangar.jpg)

**Skies:** a volumetric cloud sky on the storm levels and presets for the rest (Nar Shaddaa smog, Sulon overcast, day and sunset skies):

![Skies, 1997 vs remaster](../docs/screenshots/stage2-volumetric-sky.jpg)

**Lightsaber:** a glowing plasma blade instead of a flat textured strip, a white-hot core, a deep coloured sheath and a halo that lights the room, with swing trails. ([video](../docs/screenshots/stage4-saber-blade.mp4))

![Lightsaber, 1997 vs remaster](../docs/screenshots/stage4-saber-blade.jpg)

**The cast:** most of Jedi Knight's characters now use Jedi Academy's models, fitted onto the original skeletons so every 1997 animation still drives them (same lighting and framing for each pair):

![The cast, 1997 vs remaster](../docs/screenshots/stage4-cast-sheet.jpg)

![In game: Nar Shaddaa](../docs/screenshots/cast/ingame-narshadda.jpg)

**Texture God on every level:** my own AI upscaling and PBR pipeline covers every level texture, including per-level palette variants (507 textures in game). Original texture (left of each pair) vs Texture God:

![Texture God palette variants](../docs/screenshots/stage3-texturegod-palette-variants.jpg)

**Cutscenes:** all 25 play upscaled in game (Topaz, 1920x900), original (left) vs upscale:

![Cutscenes, original vs upscale](../docs/screenshots/stage5-cutscenes-hd.jpg)

Next up: [atmosphere](../docs/stage2-atmosphere-plan.md), continued: dust in the light, light shafts that stop at walls.

Decisions, findings and dead ends are logged as I go in [docs/DECISIONS.md](../docs/DECISIONS.md).

## Building it yourself (macOS, Apple Silicon)

```sh
git clone --recursive https://github.com/domdino/OpenJKDF2.git
cd OpenJKDF2
brew install cmake imagemagick openal-soft glew libpng openssl@3 protobuf libmodplug innoextract
scripts/build.sh
```

Extract your own copy of the game into `gamedata/jk1/`. For the GOG installer: `innoextract --language english -I app <installer.exe>`, then move the contents of `app/` there. Then:

```sh
scripts/run.sh
```

To see the remastered look, open the console in game (backtick) and type `remaster 1`. Colour grades live in `grade/`; the game reloads `grade/default.cube` live whenever it changes.

## Credits and legal

All engine credit goes to the [OpenJKDF2](https://github.com/shinyquagsire23/OpenJKDF2) project and its contributors. Star Wars, Jedi Knight and related marks are the property of Lucasfilm and Disney. This project is not affiliated with or endorsed by them, includes no original game assets, and is for personal, non-commercial use only. Concept images are AI-generated references for art direction.
