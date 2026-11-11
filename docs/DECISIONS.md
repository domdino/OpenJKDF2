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
