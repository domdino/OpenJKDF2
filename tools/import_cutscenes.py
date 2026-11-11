#!/usr/bin/env python3
"""Put finished HD cutscenes where the game plays them, and check them.

    python3 tools/import_cutscenes.py [source folder] [--dry-run]

Looks in the source folder (default hd/video_src) for renders named after an
original cutscene plus any suffix, e.g. 01-02A_chf3_prob4.mp4 from Topaz, and
links the newest finished one for each cutscene into hd/video as 01-02A.mp4
(a hard link, so no extra disk space). Files still being written (Topaz's
temporary NAME_123456789.mp4, or anything modified in the last 30 s) are skipped.

Checks every file: it must open, and its length must match the original within
half a second (the engine keeps the original's timing, sound and subtitles, so
a retimed clip would drift). Mismatches are reported and not imported.
"""
import json
import os
import re
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ORIG = os.path.join(ROOT, "gamedata", "jk1", "Resource", "VIDEO")
DEST = os.path.join(ROOT, "hd", "video")
EXTS = (".mp4", ".mov", ".m4v")
TOLERANCE = 0.5


def probe(path):
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0",
         "-show_entries", "stream=width,height,r_frame_rate,codec_name:format=duration",
         "-of", "json", path],
        capture_output=True, text=True)
    if out.returncode != 0:
        return None
    j = json.loads(out.stdout)
    if not j.get("streams"):
        return None
    s = j["streams"][0]
    num, den = s.get("r_frame_rate", "0/1").split("/")
    return {
        "w": s.get("width", 0), "h": s.get("height", 0),
        "fps": float(num) / float(den) if float(den) else 0.0,
        "codec": s.get("codec_name", "?"),
        "dur": float(j.get("format", {}).get("duration", 0) or 0),
    }


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    dry = "--dry-run" in sys.argv
    src = os.path.abspath(args[0]) if args else os.path.join(ROOT, "hd", "video_src")
    os.makedirs(DEST, exist_ok=True)

    originals = sorted(f for f in os.listdir(ORIG) if f.lower().endswith(".smk"))
    files = os.listdir(src)
    now = time.time()
    done = waiting = bad = 0

    for smk in originals:
        name = os.path.splitext(smk)[0]
        pat = re.compile(re.escape(name) + r"_(.+)$", re.I)
        cands = []
        for f in files:
            stem, ext = os.path.splitext(f)
            m = pat.match(stem)
            if not m or ext.lower() not in EXTS:
                continue
            p = os.path.join(src, f)
            if re.fullmatch(r"\d{6,}", m.group(1)) or now - os.path.getmtime(p) < 30:
                waiting += 1
                print("  %-10s still rendering: %s" % (name, f))
                continue
            cands.append(p)
        if not cands:
            continue
        hd = max(cands, key=os.path.getmtime)
        o = probe(os.path.join(ORIG, smk))
        h = probe(hd)
        if not h:
            bad += 1
            print("  %-10s CAN'T READ %s" % (name, os.path.basename(hd)))
            continue
        diff = h["dur"] - o["dur"]
        info = "%dx%d %s %.3f fps, %.1f s (%+.2f s)" % (h["w"], h["h"], h["codec"], h["fps"], h["dur"], diff)
        if abs(diff) > TOLERANCE:
            bad += 1
            print("  %-10s LENGTH MISMATCH %s: %s (original %.1f s)" % (name, os.path.basename(hd), info, o["dur"]))
            continue
        ext = os.path.splitext(hd)[1].lower()
        dst = os.path.join(DEST, name + ext)
        if not dry:
            for e in EXTS:
                old = os.path.join(DEST, name + e)
                if os.path.lexists(old):
                    os.remove(old)
            try:
                os.link(hd, dst)
            except OSError:
                shutil.copy2(hd, dst)
        done += 1
        print("  %-10s ok  %s <- %s" % (name, info, os.path.basename(hd)))

    print("%s%d imported to %s, %d still rendering, %d problems, %d of %d cutscenes have HD"
          % ("(dry run) " if dry else "", done, os.path.relpath(DEST, ROOT), waiting, bad,
             len([f for f in os.listdir(DEST) if os.path.splitext(f)[1].lower() in EXTS]), len(originals)))


if __name__ == "__main__":
    main()
