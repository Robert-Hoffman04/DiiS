#!/usr/bin/env python3
"""DS system font (NFTR) -> source/gui/gui_font_data.h: the GUI's two bitmap fonts.

The source is the classic DS menu's font, ds.nftr (10x11 cells, 2 bpp, no kerning), as
TWiLight Menu's quickmenu ships it in nitrofiles/graphics/font. It's Nintendo's font, so
it isn't kept in this repo: pass the folder that holds it. Body is the font at 1x (the
canvas is DS-sized, shown at 2x), Title at 2x; glyphs are scaled with nearest neighbour
so they stay pixel-exact. Both share one I4 atlas (4-bit coverage, GX 8x8 tiles), drawn one texel per
320x240 canvas pixel. Coverage: the CPS below that the font maps. Glyph 0 of each font is
a box drawn for every code point the font doesn't have.

Run from the repo root:  python3 tools/gui/mkfont.py <dir with ds.nftr> [--png]"""
import struct
import sys
from PIL import Image, ImageDraw

ARGS = [a for a in sys.argv[1:] if not a.startswith("--")]
FONTDIR = ARGS[0] if ARGS else "../../DS/TWiLightMenu/quickmenu/nitrofiles/graphics/font"
FONTS = [("BODY", "ds.nftr", 1), ("TITLE", "ds.nftr", 2)]
ATLAS_W = 512
PAD = 1

CPS = list(range(0x20, 0x7F)) + list(range(0xA0, 0x180)) + [
    0x2013, 0x2014, 0x2018, 0x2019, 0x201A, 0x201C, 0x201D, 0x201E, 0x2022, 0x2026,
    0x2030, 0x2039, 0x203A, 0x20AC, 0x2122]


def q4(v):
    return (v * 15 + 127) // 255


class Nftr:
    """Glyph bitmaps (2D lists of 0..(1<<bpp)-1), widths (left, glyph w, advance) and the
    code point -> glyph map of a Nitro font."""

    def __init__(self, path):
        d = self.d = open(path, "rb").read()
        assert d[0:4] == b"RTFN" and d[0x10:0x14] == b"FNIF", path
        self.lineH = d[0x19]
        cglp, cwdh, cmap = struct.unpack_from("<III", d, 0x20)   # each points past its tag + size
        self.cw, self.ch = d[cglp], d[cglp + 1]
        self.ts = struct.unpack_from("<H", d, cglp + 2)[0]
        self.bpp = d[cglp + 6]
        self.bits = cglp + 8
        n = (struct.unpack_from("<I", d, cglp - 4)[0] - 0x10) // self.ts
        self.widths = {}
        o = cwdh
        while 0 < o < len(d):
            first, last, nxt = struct.unpack_from("<HHI", d, o)
            for i in range(first, min(last, n - 1) + 1):
                e = o + 8 + (i - first) * 3
                self.widths[i] = (struct.unpack_from("<b", d, e)[0], d[e + 1], d[e + 2])
            o = nxt
        self.map = {}
        o = cmap
        while 0 < o < len(d):
            fc, lc, typ, _, nxt = struct.unpack_from("<HHHHI", d, o)
            b = o + 12
            if typ == 0:
                fi = struct.unpack_from("<H", d, b)[0]
                for c in range(fc, lc + 1):
                    self.map[c] = fi + c - fc
            elif typ == 1:
                for c in range(fc, lc + 1):
                    i = struct.unpack_from("<H", d, b + 2 * (c - fc))[0]
                    if i != 0xFFFF:
                        self.map[c] = i
            else:
                for k in range(struct.unpack_from("<H", d, b)[0]):
                    c, i = struct.unpack_from("<HH", d, b + 2 + 4 * k)
                    self.map[c] = i
            o = nxt

    def glyph(self, i):
        raw = self.d[self.bits + i * self.ts:self.bits + (i + 1) * self.ts]
        v, tot, m = int.from_bytes(raw, "big"), self.ts * 8, (1 << self.bpp) - 1
        return [[(v >> (tot - self.bpp * (y * self.cw + x + 1))) & m for x in range(self.cw)]
                for y in range(self.ch)]

    def ink_rows(self, cp):
        g = self.glyph(self.map[cp])
        return [y for y, row in enumerate(g) if any(row)]


def render(fn, cp, s, base):
    """Glyph image (L) at scale s cropped to its ink, its offset from the pen / baseline,
    and its advance in px."""
    i = fn.map[cp]
    left, _, adv = fn.widths.get(i, (0, fn.cw, fn.cw))
    m = (1 << fn.bpp) - 1
    im = Image.new("L", (fn.cw, fn.ch), 0)
    for y, row in enumerate(fn.glyph(i)):
        for x, p in enumerate(row):
            im.putpixel((x, y), p * 255 // m)
    box = im.getbbox()
    if not box:
        return None, 0, 0, adv * s
    im = im.crop(box)
    im = im.resize((im.width * s, im.height * s), Image.NEAREST)
    return im, (left + box[0]) * s, (box[1] - base) * s, adv * s


def notdef(capH, s):
    """Hollow box, cap height tall (pen-relative like render())."""
    w = 4 * s
    im = Image.new("L", (w, capH), 0)
    ImageDraw.Draw(im).rectangle((0, 0, w - 1, capH - 1), outline=255)
    return im, s, -capH, w + 2 * s


def main():
    fonts = []
    for name, file, s in FONTS:
        fn = Nftr(f"{FONTDIR}/{file}")
        # Baseline: under 'H' (one past its last ink row); descent: to the cell bottom.
        base = fn.ink_rows(ord("H"))[-1] + 1
        capH = (base - fn.ink_rows(ord("H"))[0]) * s
        asc, desc = base * s, (fn.ch - base) * s
        glyphs = []   # (cp, img, ox, oy, adv16)
        im, ox, oy, adv = notdef(capH, s)
        glyphs.append((0, im, ox, oy, adv * 16))
        for cp in CPS:
            if cp in fn.map:
                im, ox, oy, adv = render(fn, cp, s, base)
                glyphs.append((cp, im, ox, oy, adv * 16))
        fonts.append(dict(name=name, file=file, size=s, asc=asc, desc=desc, capH=capH,
                          glyphs=glyphs, kern=[]))

    # Shelf-pack every glyph of both fonts, tallest first.
    items = [(fi, gi) for fi, f in enumerate(fonts) for gi, g in enumerate(f["glyphs"]) if g[1]]
    items.sort(key=lambda it: -fonts[it[0]]["glyphs"][it[1]][1].size[1])
    place = {}
    x = y = shelfH = 0
    for fi, gi in items:
        w, h = fonts[fi]["glyphs"][gi][1].size
        if x + w + PAD > ATLAS_W:
            x, y, shelfH = 0, y + shelfH + PAD, 0
        place[(fi, gi)] = (x, y)
        x += w + PAD
        shelfH = max(shelfH, h)
    H = (y + shelfH + 7) & ~7
    assert H <= 1024, H
    atlas = Image.new("L", (ATLAS_W, H), 0)
    for (fi, gi), (px, py) in place.items():
        atlas.paste(fonts[fi]["glyphs"][gi][1], (px, py))
    if "--png" in sys.argv:
        atlas.save("gui_font_atlas.png")

    pix = atlas.load()
    data = bytearray()
    for ty in range(0, H, 8):
        for tx in range(0, ATLAS_W, 8):
            for yy in range(8):
                for xx in range(0, 8, 2):
                    data.append((q4(pix[tx + xx, ty + yy]) << 4) | q4(pix[tx + xx + 1, ty + yy]))

    with open("source/gui/gui_font_data.h", "w") as f:
        f.write("// Generated by tools/gui/mkfont.py from the DS system font (ds.nftr). Do not edit.\n")
        f.write("// One I4 atlas (GX 8x8 tiles) shared by both fonts; one texel per 320x240 canvas pixel.\n")
        f.write("// Advances and kerning are in 1/16 px. Glyph 0 of each font is the missing-glyph box.\n")
        f.write("#pragma once\n")
        f.write(f"#define GUI_FONT_ATLAS_W {ATLAS_W}\n#define GUI_FONT_ATLAS_H {H}\n")
        f.write(f"static u8 GuiFontAtlas[GUI_FONT_ATLAS_W * GUI_FONT_ATLAS_H / 2] __attribute__((aligned(32))) = {{\n")
        for i in range(0, len(data), 16):
            f.write("\t" + ",".join(f"0x{v:02X}" for v in data[i:i + 16]) + ",\n")
        f.write("};\n")
        for fi, fo in enumerate(fonts):
            n = fo["name"]
            f.write(f"\n// {fo['file']} at {fo['size']}x\n")
            f.write(f"#define GUI_FONT_{n}_ASCENT {fo['asc']}\n#define GUI_FONT_{n}_DESCENT {fo['desc']}\n")
            f.write(f"#define GUI_FONT_{n}_CAP {fo['capH']}\n")
            f.write(f"static const GuiGlyph GuiFont{n.title()}Glyphs[] = {{\n")
            for gi, (cp, im, ox, oy, adv) in enumerate(fo["glyphs"]):
                px, py = place.get((fi, gi), (0, 0))
                w, h = im.size if im else (0, 0)
                f.write(f"\t{{ 0x{cp:04X}, {px}, {py}, {w}, {h}, {ox}, {oy}, {adv} }},\n")
            f.write("};\n")
            f.write(f"static const GuiKern GuiFont{n.title()}Kern[] = {{\n")
            f.write("\t{ 0xFFFFFFFF, 0 },\n")   # no kerning in the DS font
            f.write("};\n")
    print(f"atlas {ATLAS_W}x{H} ({len(data)} bytes); " +
          ", ".join(f"{fo['name']}: {len(fo['glyphs'])} glyphs, asc {fo['asc']} desc {fo['desc']}" for fo in fonts))


main()
