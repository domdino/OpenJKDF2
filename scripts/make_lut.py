#!/usr/bin/env python3
"""Generate .cube 3D LUTs for the in-game grading pass.

  scripts/make_lut.py identity grade/identity.cube
  scripts/make_lut.py teal grade/teal-test.cube
  scripts/make_lut.py grimy grade/grimy-test.cube

identity: neutral, changes nothing. Use it to check the pipeline, or load it into
          Resolve as a starting node.
teal:     a rough starter look in the direction of the north star frame: cooler
          teal shadows, slightly warm highlights, a gentle S-curve and a touch less
          saturation. A placeholder until a real grade is made in Resolve.
grimy:    starter look for north star 2 (the chosen direction): crushed blacks,
          heavy desaturation, warm-brown mids, slightly cool highlights.

The intended workflow for real grades: take an ungraded in-game frame (F12, or
scripts/capture.sh), grade it in Resolve on a Rec.709 timeline, export a 33-point
.cube, save it as grade/default.cube. The game reloads it live.
"""
import sys

SIZE = 33


def identity(r, g, b):
    return r, g, b


def smoothstep(e0, e1, x):
    t = min(max((x - e0) / (e1 - e0), 0.0), 1.0)
    return t * t * (3.0 - 2.0 * t)


def teal(r, g, b):
    luma = 0.2126 * r + 0.7152 * g + 0.0722 * b

    # Gentle S-curve around mid grey for contrast.
    def curve(x):
        return x + 0.18 * (x - 0.5) * (1.0 - abs(2.0 * x - 1.0)) * 1.0

    r, g, b = curve(r), curve(g), curve(b)

    # Split tone: teal into the shadows, a little warmth into the highlights.
    shadow = 1.0 - smoothstep(0.0, 0.55, luma)
    high = smoothstep(0.55, 1.0, luma)
    r += -0.035 * shadow + 0.025 * high
    g += 0.010 * shadow + 0.008 * high
    b += 0.030 * shadow - 0.020 * high

    # Pull saturation down slightly for a more filmic feel.
    l2 = 0.2126 * r + 0.7152 * g + 0.0722 * b
    sat = 0.88
    r = l2 + (r - l2) * sat
    g = l2 + (g - l2) * sat
    b = l2 + (b - l2) * sat

    clamp = lambda x: min(max(x, 0.0), 1.0)
    return clamp(r), clamp(g), clamp(b)


def grimy(r, g, b):
    """Starter look for north star 2: dark, desaturated, warm-brown, dense blacks."""
    clamp = lambda x: min(max(x, 0.0), 1.0)

    # Firm blacks and gentle contrast, without crushing: JK is already a dark game, so
    # only the very bottom gets denser and the mids stay readable. (The first version
    # pulled the whole low end down and lost most of the detail.)
    def curve(x):
        x = clamp(x)
        toe = x * x * (1.6 - 0.6 * x)
        shaped = x + (toe - x) * 0.3             # 30% of the toe crush
        return clamp(shaped + 0.08 * (x - 0.5) * (1.0 - abs(2.0 * x - 1.0)))

    r, g, b = curve(r), curve(g), curve(b)

    # Desaturation, lighter than before so materials still read.
    luma = 0.2126 * r + 0.7152 * g + 0.0722 * b
    sat = 0.7
    r = luma + (r - luma) * sat
    g = luma + (g - luma) * sat
    b = luma + (b - luma) * sat

    # Warm-brown midtones, near-neutral cool highlights (skylight), neutral blacks.
    mid = 4.0 * luma * (1.0 - luma)
    high = smoothstep(0.7, 1.0, luma)
    r += 0.045 * mid - 0.010 * high
    g += 0.015 * mid + 0.000 * high
    b += -0.040 * mid + 0.015 * high

    return clamp(r), clamp(g), clamp(b)


LOOKS = {"identity": identity, "teal": teal, "grimy": grimy}


def main():
    if len(sys.argv) != 3 or sys.argv[1] not in LOOKS:
        sys.exit("usage: make_lut.py {%s} OUT.cube" % "|".join(LOOKS))
    look, out = LOOKS[sys.argv[1]], sys.argv[2]
    n = SIZE - 1
    with open(out, "w") as f:
        f.write('TITLE "%s"\n' % sys.argv[1])
        f.write("LUT_3D_SIZE %d\n" % SIZE)
        f.write("DOMAIN_MIN 0.0 0.0 0.0\nDOMAIN_MAX 1.0 1.0 1.0\n")
        # .cube order: red changes fastest, then green, then blue.
        for bi in range(SIZE):
            for gi in range(SIZE):
                for ri in range(SIZE):
                    r, g, b = look(ri / n, gi / n, bi / n)
                    f.write("%.6f %.6f %.6f\n" % (r, g, b))
    print("wrote", out)


if __name__ == "__main__":
    main()
