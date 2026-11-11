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
- Not yet checked by hand: menu save/load, lightsaber, Force powers, music, cutscenes. Needs a play session.

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
