#!/usr/bin/env python3
"""Bake light maps for many levels in one go (light bake, step 5).

    python3 tools/lightbake/bake_all.py [LEVEL ...] [--skip-done] [--rebake LEVEL,...] [--probes-only] [--volume-only]

For each level (default: every level in JK1.GOB): export it (tools/lightbake/jkl_export.py,
--density auto), recover the designer lights (fit_lights.py --occlusion), bake in Blender
(tools/blender/lightbake.py, the settings tuned on the six test levels) and write the
sidecar the game loads (tools/blender/lightmap_sidecar.py), then the light probes for models
(tools/blender/probe_bake.py) and the light volume for the fog (tools/lightbake/volume_points.py,
tools/blender/volume_bake.py). --probes-only / --volume-only bake just those for levels already baked. --skip-done leaves levels that
already have hd/jkgm/lightmaps/<level>.jklm alone; --rebake bakes and re-exports the sidecar
only, keeping the level's export and lights (for a new sky, say).

The sky surfaces light the bake in the colour of the level's volumetric sky preset (matched
by the level's sky material, as src/Engine/sithSky.c does): bright for day levels, dim for
storms and night. Progress and timings go to logs/lightbake_all.log.
"""
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
BLENDER = "/Applications/Blender.app/Contents/MacOS/Blender"
BAKE_ARGS = ["--samples", "512", "--glow", "14", "--power", "1.5", "--bounces", "1"]

# Sky light per sithSky preset, matched by sky material (lower case, no .mat).
SKIES = [
    (("13cnsky2",), "0.12,0.13,0.16"),                        # storm
    (("00starry",), "0.05,0.04,0.035"),                       # Nar Shaddaa night smog
    (("03sky09k", "03sky01k"), "0.35,0.37,0.40"),             # Sulon overcast
    (("04sky01", "10sky2", "14sky"), "0.55,0.65,0.85"),       # day, broken cloud
    (("06sky01", "06sky02", "09skyb"), "0.60,0.72,1.00"),     # day, clear
    (("12sky3",), "0.35,0.25,0.25"),                          # sunset
]
DEFAULT_SKY = "0.02,0.025,0.035"
# Sun (or moon) per preset: azimuth, elevation and colour from src/Engine/sithSky.c, so the baked
# shadows agree with the sky; strength is the bake's own (sky light is the "--sky" colour above).
SUNS = {
    "13cnsky2": "215,24,0.35,0.78,0.84,1.0",                           # storm: the moon breaking through
    "04sky01": "60,40,SUN,1.0,0.96,0.88", "10sky2": "60,40,SUN,1.0,0.96,0.88", "14sky": "60,40,SUN,1.0,0.96,0.88",
    "06sky01": "150,55,SUN,1.0,0.97,0.90", "06sky02": "150,55,SUN,1.0,0.97,0.90", "09skyb": "150,55,SUN,1.0,0.97,0.90",
}
SUN_STRENGTH = float(os.environ.get("SUN_STRENGTH", "8.0"))   # cargo: 3 barely showed, 8 reads as daylight


def log(msg):
    line = "%s %s" % (time.strftime("%H:%M:%S"), msg)
    print(line, flush=True)
    with open(os.path.join(ROOT, "logs", "lightbake_all.log"), "a") as f:
        f.write(line + "\n")


def sky_for(folder):
    d = json.load(open(os.path.join(folder, "level.json")))
    used = {s["mat"].lower() for s in d["surfaces"] if s["sky"]}
    for mats, rgb in SKIES:
        if used & set(mats):
            return rgb, sorted(used & set(mats))[0]
    return DEFAULT_SKY, "none"


def run(cmd, tag):
    t0 = time.time()
    p = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    out = (p.stdout or "") + (p.stderr or "")
    keep = [l for l in out.splitlines() if any(k in l for k in
            ("light map:", "terrain skin", "fit:", "VIS: done", "calibration", "final bake", "sun from", "SIDECAR", "RUBBLE", "PROBES", "VOLUME", "volume points", "Error", "FAILED", "Traceback"))]
    for l in keep:
        log("  " + l.strip())
    if p.returncode != 0 or "FAILED" in out:
        log("  %s failed (exit %d)" % (tag, p.returncode))
        return False
    log("  %s %.0f s" % (tag, time.time() - t0))
    return True


def sun_args(preset):
    s = SUNS.get(preset)
    return ["--sun", s.replace("SUN", str(SUN_STRENGTH))] if s else []


def blender(script, *args):
    return [BLENDER, "-b", "--factory-startup", "-noaudio", "--python", os.path.join(ROOT, script), "--", *args]


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    rebake = set(sys.argv[sys.argv.index("--rebake") + 1].split(",")) if "--rebake" in sys.argv else set()
    rb_arg = sys.argv[sys.argv.index("--rebake") + 1] if "--rebake" in sys.argv else None
    args = [a for a in args if a not in rebake and a != rb_arg]
    # No levels named: all of them (with --rebake alone, only the rebakes).
    if not args and (not rebake or "--skip-done" in sys.argv or "--probes-only" in sys.argv or "--volume-only" in sys.argv):
        sys.path.insert(0, os.path.dirname(HERE))
        import mat_extract as mx
        args = sorted(os.path.basename(n)[:-4] for n, _, _ in mx.gob_entries(
            os.path.join(ROOT, "gamedata", "jk1", "Episode", "JK1.GOB")) if n.endswith(".jkl"))
    for level in list(rebake) + args:
        folder = os.path.join("hd", "lightbake", level)
        if "--volume-only" in sys.argv:
            if not os.path.exists(os.path.join(ROOT, folder, "probes.json")):
                log("%s: no probes yet (they hold the calibration), volume skipped" % level)
                continue
            t0 = time.time()
            sky, preset = sky_for(os.path.join(ROOT, folder))
            if run(["python3", "-I", "tools/lightbake/volume_points.py", level], "volume points") and \
                    run(blender("tools/blender/volume_bake.py", folder, *BAKE_ARGS, "--sky", sky, *sun_args(preset)), "volume"):
                log("%s: volume done in %.1f min" % (level, (time.time() - t0) / 60))
            continue
        if "--probes-only" in sys.argv:
            if not os.path.exists(os.path.join(ROOT, folder, "lightmap.exr")):
                log("%s: no light bake yet, probes skipped" % level)
                continue
            t0 = time.time()
            sky, preset = sky_for(os.path.join(ROOT, folder))
            if run(blender("tools/blender/probe_bake.py", folder, *BAKE_ARGS, "--sky", sky, *sun_args(preset)), "probes"):
                log("%s: probes done in %.1f min" % (level, (time.time() - t0) / 60))
            continue
        done = os.path.exists(os.path.join(ROOT, "hd", "jkgm", "lightmaps", level + ".jklm"))
        if "--skip-done" in sys.argv and done and level not in rebake:
            log("%s: already baked, skipped" % level)
            continue
        t0 = time.time()
        log("%s: start" % level)
        if level not in rebake:
            if not run(["python3", "-I", "tools/lightbake/jkl_export.py", level, "--density", "auto"], "export"):
                continue
            if not run(["python3", "-I", "tools/lightbake/fit_lights.py", folder, "--occlusion"], "fit"):
                continue
        # Terrain skin (stage 3): lumpy rock over the flat terrain faces, baked with the rest.
        if not run(["python3", "-I", "tools/lightbake/terrain_skin.py", level], "skin"):
            continue
        sky, preset = sky_for(os.path.join(ROOT, folder))
        log("  sky %s (%s)" % (sky, preset))
        if not run(blender("tools/blender/lightbake.py", folder, *BAKE_ARGS, "--sky", sky, *sun_args(preset)), "bake"):
            continue
        if not run(blender("tools/blender/lightmap_sidecar.py", folder), "sidecar"):
            continue
        if not run(blender("tools/blender/probe_bake.py", folder, *BAKE_ARGS, "--sky", sky, *sun_args(preset)), "probes"):
            continue
        if not run(blender("tools/blender/rubble_bake.py", folder, *BAKE_ARGS, "--sky", sky, *sun_args(preset)), "rubble"):
            continue
        if not run(["python3", "-I", "tools/lightbake/volume_points.py", level], "volume points"):
            continue
        if not run(blender("tools/blender/volume_bake.py", folder, *BAKE_ARGS, "--sky", sky, *sun_args(preset)), "volume"):
            continue
        log("%s: done in %.1f min" % (level, (time.time() - t0) / 60))


if __name__ == "__main__":
    main()
