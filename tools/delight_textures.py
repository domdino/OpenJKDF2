#!/usr/bin/env python3
"""Take the painted-in highlights out of character textures ("de-lighting").

    python3 tools/delight_textures.py PREFIX... [--strength S] [--revert]

The Jedi Academy textures the converted cast uses have shine painted into the colour (bright
streaks on leather, cloth folds lit from above). With the remaster's own lighting on top, the
characters read glossy whatever their roughness. This pulls every pixel brighter than its
surroundings back towards the local average (luminance only, colour kept), so the engine's
lighting does the shading.

For each character material whose texture starts with one of the PREFIXes (e.g. ky_ st_ io_),
writes <texture>_dl.png next to it in hd/jkgm/materials/characters and points the pack entry at
it (metadata.json; the original entry is kept in metadata.before_delight.json once). --revert
puts the original textures back.
"""
import json
import os
import shutil
import sys

import numpy as np
from PIL import Image, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
PACK = os.path.join(os.path.dirname(HERE), "hd", "jkgm", "materials", "characters")


def delight(img, strength):
    a = np.asarray(img.convert("RGBA")).astype(np.float32) / 255.0
    rgb, alpha = a[..., :3], a[..., 3:]
    lum = rgb @ np.array([0.2126, 0.7152, 0.0722], np.float32)
    radius = max(2, int(min(img.size) * 0.04))
    blur = np.asarray(Image.fromarray((lum * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(radius))).astype(np.float32) / 255.0
    excess = np.maximum(lum - blur * 1.08, 0.0)
    new = lum - strength * excess
    # Gently lift the darkest painted shadows too, so form comes from the engine's light.
    deficit = np.maximum(blur * 0.8 - new, 0.0)
    new = new + strength * 0.4 * deficit
    scale = np.where(lum > 1e-4, new / np.maximum(lum, 1e-4), 1.0)[..., None]
    out = np.clip(rgb * scale, 0, 1)
    return Image.fromarray((np.concatenate([out, alpha], -1) * 255 + 0.5).astype(np.uint8), "RGBA")


def main():
    prefixes = [a for a in sys.argv[1:] if not a.startswith("--")]
    strength = float(sys.argv[sys.argv.index("--strength") + 1]) if "--strength" in sys.argv else 0.8
    meta_path = os.path.join(PACK, "metadata.json")
    backup = os.path.join(PACK, "metadata.before_delight.json")
    meta = json.load(open(meta_path))
    if not os.path.exists(backup):
        shutil.copy(meta_path, backup)
    orig = {e["name"]: e for e in json.load(open(backup))["materials"]}
    n = 0
    for e in meta["materials"]:
        src = orig.get(e["name"], e).get("albedo_map", "")
        if not src or not any(src.startswith(p) for p in prefixes):
            continue
        if "--revert" in sys.argv:
            e["albedo_map"] = src
        else:
            stem, ext = os.path.splitext(src)
            dst = stem + "_dl.png"
            delight(Image.open(os.path.join(PACK, src)), strength).save(os.path.join(PACK, dst))
            e["albedo_map"] = dst
        n += 1
    json.dump(meta, open(meta_path, "w"), indent=2)
    print("%s %d textures" % ("reverted" if "--revert" in sys.argv else "de-lit", n))


if __name__ == "__main__":
    main()
