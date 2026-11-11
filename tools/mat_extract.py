#!/usr/bin/env python3
"""Extract every Jedi Knight .MAT texture to PNG, for upscaling.

  tools/mat_extract.py [--game gamedata/jk1] [--out hd/extracted] [--cmp dflt.cmp]

Reads the game's GOB archives directly (never modifies them): Resource/Res2.gob,
Resource/Res1hi.gob and Episode/JK1.GOB, in that order, so later ones override earlier
ones like the engine does. Writes:

  hd/extracted/<mat name>.cel<N>.png   one PNG per cel (animated MATs have several),
                                       at full resolution (mip 0), RGBA
  hd/extracted/manifest.json           per MAT: source GOB, size, cels, 8/16-bit, alpha

8-bit textures are coloured with a colormap's palette (default misc/cmp/dflt.cmp; levels
can use others, see --cmp). Palette index 0 is transparent on alpha textures. Solid-colour
MATs (no texture data) are skipped. hd/ is gitignored: these are game assets.
"""
import argparse
import json
import os
import struct
import sys
import zlib


# ---------------------------------------------------------------- GOB archives

def gob_entries(path):
    """Yield (name, offset, length) for every file in a GOB 2.0 archive."""
    with open(path, "rb") as f:
        if f.read(4) != b"GOB ":
            raise ValueError("%s is not a GOB archive" % path)
        _version, index_offset = struct.unpack("<II", f.read(8))
        f.seek(index_offset)
        (count,) = struct.unpack("<I", f.read(4))
        for _ in range(count):
            offset, length = struct.unpack("<II", f.read(8))
            name = f.read(128).split(b"\0")[0].decode("latin1").replace("\\", "/").lower()
            yield name, offset, length


def gob_read(path, offset, length):
    with open(path, "rb") as f:
        f.seek(offset)
        return f.read(length)


# ---------------------------------------------------------------- palette

def cmp_palette(data):
    """256 RGB triples from a .CMP colormap (64-byte header, then the palette)."""
    if data[:4] != b"CMP ":
        raise ValueError("not a CMP file")
    pal = data[64:64 + 768]
    return [tuple(pal[i * 3:i * 3 + 3]) for i in range(256)]


# ---------------------------------------------------------------- MAT parsing

HEADER = struct.Struct("<4sIII I" + "I" * 14)        # magic, revision, type, texinfos, textures, format
TEXINFO = struct.Struct("<IIIIII")
TEXINFO_EXT = struct.Struct("<IIII")
TEXHEADER = struct.Struct("<IIIIII")                  # width, height, alpha_en, unk, unk, mips


def parse_mat(data):
    """Return dict with format info and a list of cels: (width, height, alpha, raw mip0 bytes)."""
    h = HEADER.unpack_from(data, 0)
    magic, _rev, mat_type, num_texinfo, num_textures = h[:5]
    fmt = h[5:]
    if magic != b"MAT ":
        raise ValueError("not a MAT")
    is16 = fmt[0] != 0
    bpp = fmt[1]
    rbits, gbits, bbits, rshift, gshift, bshift = fmt[2:8]
    pos = HEADER.size
    for _ in range(num_texinfo):
        tex_type = TEXINFO.unpack_from(data, pos)[0]
        pos += TEXINFO.size
        if tex_type & 8:
            pos += TEXINFO_EXT.size
    cels = []
    if mat_type != 2:
        return {"is16bit": is16, "bpp": bpp, "cels": cels}
    bytes_pp = 2 if is16 else 1
    for _ in range(num_textures):
        w, hgt, alpha, _u1, _u2, mips = TEXHEADER.unpack_from(data, pos)
        pos += TEXHEADER.size
        mip0 = data[pos:pos + w * hgt * bytes_pp]
        cels.append((w, hgt, alpha, mip0))
        for m in range(mips):
            pos += (w >> m) * (hgt >> m) * bytes_pp
    return {"is16bit": is16, "bpp": bpp, "cels": cels,
            "bits": (rbits, gbits, bbits, rshift, gshift, bshift)}


def to_rgba(mat, cel, palette):
    w, h, alpha, raw = cel
    out = bytearray(w * h * 4)
    if not mat["is16bit"]:
        for i, idx in enumerate(raw):
            r, g, b = palette[idx]
            out[i * 4:i * 4 + 4] = bytes((r, g, b, 0 if (alpha and idx == 0) else 255))
    else:
        rbits, gbits, bbits, rshift, gshift, bshift = mat["bits"]
        for i in range(w * h):
            (v,) = struct.unpack_from("<H", raw, i * 2)
            def chan(bits, shift):
                if not bits:
                    return 0
                c = (v >> shift) & ((1 << bits) - 1)
                return (c * 255) // ((1 << bits) - 1)
            a = 255
            if alpha and rbits + gbits + bbits == 15:
                a = 255 if (v & 0x8000) else 0
            out[i * 4:i * 4 + 4] = bytes((chan(rbits, rshift), chan(gbits, gshift), chan(bbits, bshift), a))
    return w, h, bytes(out)


# ---------------------------------------------------------------- PNG writing

def write_png(path, w, h, rgba):
    def chunk(tag, payload):
        body = tag + payload
        return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)
    rows = b"".join(b"\x00" + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(rows, 9))
           + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)


# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap.add_argument("--game", default=os.path.join(root, "gamedata", "jk1"))
    ap.add_argument("--out", default=os.path.join(root, "hd", "extracted"))
    ap.add_argument("--cmp", default="dflt.cmp", help="colormap for 8-bit palettes (misc/cmp/<name>)")
    args = ap.parse_args()

    gobs = [os.path.join(args.game, "Resource", "Res2.gob"),
            os.path.join(args.game, "Resource", "Res1hi.gob"),
            os.path.join(args.game, "Episode", "JK1.GOB")]

    mats, cmps = {}, {}
    for gob in gobs:
        if not os.path.exists(gob):
            print("missing", gob, file=sys.stderr)
            continue
        for name, off, ln in gob_entries(gob):
            if name.endswith(".mat"):
                mats[os.path.basename(name)] = (gob, off, ln)
            elif name.endswith(".cmp"):
                cmps[os.path.basename(name)] = (gob, off, ln)

    if args.cmp not in cmps:
        sys.exit("colormap %s not found; available: %s" % (args.cmp, ", ".join(sorted(cmps))))
    palette = cmp_palette(gob_read(*cmps[args.cmp]))

    os.makedirs(args.out, exist_ok=True)
    manifest, n_png, n_skip, n_err = {}, 0, 0, 0
    for name in sorted(mats):
        gob, off, ln = mats[name]
        try:
            mat = parse_mat(gob_read(gob, off, ln))
        except Exception as e:
            n_err += 1
            print("skip %s: %s" % (name, e), file=sys.stderr)
            continue
        if not mat["cels"]:
            n_skip += 1
            continue
        base = name[:-4]
        for i, cel in enumerate(mat["cels"]):
            w, h, rgba = to_rgba(mat, cel, palette)
            write_png(os.path.join(args.out, "%s.cel%d.png" % (base, i)), w, h, rgba)
            n_png += 1
        w, h, alpha, _ = mat["cels"][0]
        manifest[name] = {"gob": os.path.basename(gob), "width": w, "height": h,
                          "cels": len(mat["cels"]), "is16bit": mat["is16bit"], "alpha": bool(alpha)}

    with open(os.path.join(args.out, "manifest.json"), "w") as f:
        json.dump({"palette": args.cmp, "materials": manifest}, f, indent=1, sort_keys=True)
    print("%d MATs, %d PNGs written, %d solid-colour skipped, %d errors -> %s"
          % (len(manifest), n_png, n_skip, n_err, args.out))


if __name__ == "__main__":
    main()
