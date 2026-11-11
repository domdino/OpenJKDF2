#!/usr/bin/env python3
"""Lay out labelled original-vs-remaster comparison images for sharing.

    python3 tools/make_side_by_side.py OUT.png TITLE LEFT.png RIGHT.png [LEFT2.png RIGHT2.png ...]
        [--labels "Original (1997)" "Remaster"]

Pairs go in rows; each row is left | right with a small label over each, and the title
across the top. Dark background, 16:9-ish friendly widths.
"""
import sys

from PIL import Image, ImageDraw, ImageFont


def font(size):
    for f in ("/System/Library/Fonts/SFNS.ttf", "/System/Library/Fonts/Helvetica.ttc",
              "/System/Library/Fonts/Supplemental/Arial.ttf"):
        try:
            return ImageFont.truetype(f, size)
        except OSError:
            continue
    return ImageFont.load_default()


def main():
    args = sys.argv[1:]
    labels = ["Original (1997)", "Remaster"]
    if "--labels" in args:
        i = args.index("--labels")
        labels = args[i + 1:i + 3]
        del args[i:i + 3]
    out, title, imgs = args[0], args[1], args[2:]
    pairs = [(Image.open(imgs[i]).convert("RGB"), Image.open(imgs[i + 1]).convert("RGB")) for i in range(0, len(imgs), 2)]
    cell_h = 900
    pairs = [tuple(im.resize((int(im.width * cell_h / im.height), cell_h), Image.LANCZOS) for im in p) for p in pairs]
    cols = max(sum(im.width for im in p) for p in pairs) // 1 if len(pairs) == 1 else None
    gap, pad, label_h, title_h = 16, 40, 56, 96
    if len(pairs) == 1 or True:
        # pairs side by side across the width
        row_w = sum(p[0].width + p[1].width + gap for p in pairs) + gap * (len(pairs) - 1)
    W = row_w + pad * 2
    H = title_h + label_h + cell_h + pad
    canvas = Image.new("RGB", (W, H), (14, 15, 18))
    d = ImageDraw.Draw(canvas)
    d.text((pad, 28), title, fill=(235, 235, 235), font=font(44))
    x = pad
    for left, right in pairs:
        for im, lab, col in ((left, labels[0], (150, 150, 150)), (right, labels[1], (255, 196, 92))):
            d.text((x + 6, title_h + 8), lab, fill=col, font=font(32))
            canvas.paste(im, (x, title_h + label_h))
            x += im.width + gap
        x += gap
    canvas.save(out, quality=90, optimize=True) if out.lower().endswith(".jpg") else canvas.save(out)
    print("wrote", out, canvas.size)


if __name__ == "__main__":
    main()
