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
| Haze and light shafts | Depth-based haze lit by the scene, screen-space light shafts | Done (playtesting) |
| Filmic colour | Colour grading pass with live-reloading .cube LUTs from Resolve | Done |
| Glowing lights and sabers that roll off softly | Bloom (built in) into a half-float buffer with highlight roll-off | Done |
| Crisp, detailed, grimy surfaces | HD textures with roughness and normal maps | Stage 3 |
| Detailed characters | New models through a Blender pipeline | Stage 4 |

## Progress

| Stage | What | Status |
|---|---|---|
| 1 | Native build on Apple Silicon, one-command build and run | Done |
| 2 | Rendering: Retina, filtering, colour grading, per-pixel lighting, atmosphere | In progress |
| 3 | HD textures with material maps, true-colour replacement loader | In progress |
| 4 | Models: Blender import/export for .3DO and .KEY, new Kyle and stormtroopers | Not started |
| 5 | Cutscenes: upscaled and graded, played from modern video files | Not started |
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
- [x] One-switch preset: `remaster 1` in the console

### Stage 3 checklist

- [x] Research: OpenJKDF2 already loads HD texture packs (colour, glow, displacement maps)
- [x] Tool to extract every original texture to PNG (`tools/mat_extract.py`)
- [x] Rebuild the community Neural Upscale pack from its installer (555 textures matched so far)
- [x] Rebuild from all three Remastered installers, palette-correct (no wrong colours in other levels)
- [x] Engine fix: HD texture hashes use the level's real palette (coverage went from almost none to dozens per view)
- [ ] Performance: HD textures plus the full remaster look are too slow in some rooms (optimise passes)
- [x] Material maps in the engine: roughness, metalness, AO and normal maps per texture (rock defaults to matte)
- [x] Importer for Texture God output (my own AI upscaling and PBR pipeline)
- [ ] First full Texture God batch in game
- [ ] Roughness and normal maps for the wet, grimy look (loader support plus generation)
- [ ] Upscale and hand-finish key textures (hangar, Nar Shaddaa, Kyle)

## Latest

Stage 3 has started. Top: the remastered look on the original textures. Bottom: the same with HD textures from the community Neural Upscale pack (the Sarris building and Boc's rock wall gain real detail). The game's own loader already reads HD texture packs; new tools extract every original texture for upscaling and rebuild packs whose files lost their names.

![Original vs HD textures](../docs/screenshots/stage3-hd-pack-off-on.png)

The whole Stage 2 look behind one switch: open the console (backtick) and type `remaster 1` (`remaster 0` for the original). Top: original game. Bottom: remaster preset (per-pixel light with wet highlights, coloured saber light, reflections, haze, light shafts, grade, bloom, ambient occlusion).

![Original vs remaster preset](../docs/screenshots/stage2-remaster-preset-off-on.png)

Haze and light shafts, first pass (left off, right on): the hangar gets a hazy glow around the shuttle and light spilling from the wall strips; the rooftop's sky and far building sit back in hazy air.

![Haze and light shafts off and on](../docs/screenshots/stage2-haze-shafts-off-on.png)

Screen-space reflections, first pass: wet floors now mirror what's around them (top off, bottom on). Hangar floor reflecting the shuttle and light strips, the wet rooftop reflecting the figures, and Boc's saber throwing a pink pool.

![Reflections off and on](../docs/screenshots/stage2-reflections-off-on.png)

Per-pixel lighting, phase 1. The same light near the far wall: the original per-vertex lighting (left) barely shows it; per-pixel (right) gives a proper pool of light and a wet glint on the ceiling ring.

![Per-pixel lighting before and after](../docs/screenshots/stage2-perpixel-light-before-after.png)

Built-in effects, all running natively. Top: original, bloom. Bottom: ambient occlusion, everything on with a test grade.

![Effects comparison](../docs/screenshots/stage2-effects-base-bloom-ssao-all.png)

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
