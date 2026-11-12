#!/usr/bin/env python3
"""Extract the JK Enhanced community mod's models and textures (3do/, mat/) from JKE.gob.

    python3 tools/jke_extract.py [path/to/JKE.gob]

Default source: ../USEFUL STUFF/JK_Enhanced/JKE/JKE.gob. Writes hd/models_src/jke/{3do,mat}/
(gitignored; third-party mod assets, personal use only). Used by tools/make_fp_hands.py --jke.
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
GOB = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(ROOT), "USEFUL STUFF", "JK_Enhanced", "JKE", "JKE.gob")
OUT = os.path.join(ROOT, "hd", "models_src", "jke")

b = open(GOB, "rb").read()
if b[:4] != b"GOB ":
    sys.exit("not a GOB file: %s" % GOB)
off = struct.unpack("<I", b[8:12])[0]
n = struct.unpack("<I", b[off:off + 4])[0]
k = 0
for i in range(n):
    e = off + 4 + i * 136
    o, s = struct.unpack("<II", b[e:e + 8])
    name = b[e + 8:e + 136].split(b"\0")[0].decode("latin1").lower().replace("\\", "/")
    if name.startswith(("3do/", "mat/")):
        p = os.path.join(OUT, name)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        open(p, "wb").write(b[o:o + s])
        k += 1
print("%d files extracted to %s" % (k, os.path.relpath(OUT, ROOT)))
