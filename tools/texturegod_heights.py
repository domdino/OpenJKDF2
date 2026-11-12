#!/usr/bin/env python3
"""Give the Texture God packs parallax: add each texture's height map as its displacement map.

    python3 tools/texturegod_heights.py [--terrain F] [--other F] [--revert]

Texture God writes a 16-bit height map (<stem>_height.png, white = high) for every texture it
remasters; the importer (tools/texturegod_import.py) only took colour, ORM and normals. This finds
each pack's run folder (from the pack description), converts the heights to 8-bit (the engine's
PNG loader), stretched to the full range per texture, and adds `displacement_map` and
`displacement_factor` (depth in texture widths) to every entry. Terrain textures (cliffs, rock,
ground, stone, sand) get --terrain (default 0.05), everything else --other (0.015): panels and
pipes keep a hint of depth without swimming. The shader does the parallax (default_f.glsl).
"""
import glob
import json
import os
import re
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TG_OUT = "/Users/therock/Documents/Work/TEXTURE GOD/out"
TERRAIN = re.compile(r"cliff|clif|rock|rck|grs|grass|stn|ston|wst|dtw|wex|waq|dirt|sand|mud|grnd|car3|cave", re.I)


def main():
    terrain = float(sys.argv[sys.argv.index("--terrain") + 1]) if "--terrain" in sys.argv else 0.05
    other = float(sys.argv[sys.argv.index("--other") + 1]) if "--other" in sys.argv else 0.015
    for meta_path in sorted(glob.glob(os.path.join(ROOT, "hd", "jkgm", "materials", "texturegod*", "metadata.json"))):
        pack = os.path.dirname(meta_path)
        meta = json.load(open(meta_path))
        m = re.search(r"run (\S+)$", meta.get("description", ""))
        run = os.path.join(TG_OUT, m.group(1)) if m else None
        n_t = n_o = missing = 0
        for e in meta["materials"]:
            if "--revert" in sys.argv:
                e.pop("displacement_map", None)
                e.pop("displacement_factor", None)
                continue
            stem = e.get("albedo_map", "").replace("_albedo.png", "")
            src = os.path.join(run, stem + "_height.png") if run else None
            if not src or not os.path.exists(src):
                missing += 1
                continue
            dst_name = stem + "_height8.png"
            dst = os.path.join(pack, dst_name)
            if not os.path.exists(dst):
                h = np.asarray(Image.open(src)).astype(np.float32)
                lo, hi = np.percentile(h, 1), np.percentile(h, 99)
                h = np.clip((h - lo) / max(hi - lo, 1.0), 0.0, 1.0)
                Image.fromarray((h * 255 + 0.5).astype(np.uint8)).convert("RGB").save(dst)
            mat = e["name"].split(".")[1] if "." in e["name"] else e["name"]
            is_terrain = bool(TERRAIN.search(mat))
            e["displacement_map"] = dst_name
            e["displacement_factor"] = terrain if is_terrain else other
            n_t += is_terrain
            n_o += not is_terrain
        json.dump(meta, open(meta_path, "w"), indent=1)
        print("%-20s %s: %d terrain, %d other, %d without a height map" % (
            os.path.basename(pack), "reverted" if "--revert" in sys.argv else "parallax", n_t, n_o, missing))


if __name__ == "__main__":
    main()
