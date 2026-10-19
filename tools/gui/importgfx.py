#!/usr/bin/env python3
"""Imports the menu's textures from res/gfx (grit sources, not tracked) into data/gui/
(tracked; the Makefile links every file there in with bin2o, gui_gfx.cpp decodes them).
Each grit -gT colour key becomes real alpha, so data/gui holds plain RGBA PNGs and the
runtime needs no per-image key. Only the files listed here are copied; res/gfx itself is
left alone. Run from the repo root:  python3 tools/gui/importgfx.py"""
import os
import re
from PIL import Image

SRC = "res/gfx"
DST = "data/gui"
FILES = [
    "boxart_placeholder.png", "cornericons.png", "cursor.png", "dialogbox.bmp",
    "folder_icon.png", "icon_dscard.png", "icon_empty.bmp", "icon_gba.png",
    "iconPhat_gba.png", "icon_gbamode.png", "icon_settings.png", "icon_settings_away.png",
    "icon_unk.png", "iconbox.png", "iconbox_pressed.png", "launch_dot.png",
    "scroll_window.png", "start_border.png", "wirelessicons.png",
]

# The DS menu recolours some sprites at runtime with the user's favourite
# colour (quickmenu: cursorPals / start_borderPals, indexed by the firmware
# colour). The PNGs carry the gray palette; bake in orange: the dialog
# box's (251, 105, 0), the menu's accent, as the fill; cursorPals row 4
# (orange) for the cursor's outline. BGR555 source slot -> colour, per file.
def bgr555(v):
    return tuple(((v >> s) & 31) * 255 // 31 for s in (0, 5, 10))


RECOLOR = {
    "cursor.png": {bgr555(0x3D88): (251, 105, 0), bgr555(0x7FFF): bgr555(0x73DF)},
    "start_border.png": {bgr555(0x3D88): (251, 105, 0)},
}


def near(a, b):   # the PNGs hold 5-bit colours expanded by an unknown rule
    return all(abs(x - y) <= 8 for x, y in zip(a, b))


def key_of(grit):
    # Last uncommented -gT wins (grit's own rule); "-gT!" / none: opaque.
    key = None
    if os.path.exists(grit):
        for line in open(grit):
            m = re.match(r"\s*-gT\s*([0-9A-Fa-f]{6})", line)
            if m:
                key = tuple(int(m.group(1)[i:i + 2], 16) for i in (0, 2, 4))
    return key


os.makedirs(DST, exist_ok=True)
for f in FILES:
    base = os.path.splitext(f)[0]
    im = Image.open(os.path.join(SRC, f)).convert("RGBA")
    key = key_of(os.path.join(SRC, base + ".grit"))
    if key:
        im.putdata([(0, 0, 0, 0) if p[:3] == key else p for p in im.getdata()])
    for src, dst in RECOLOR.get(f, {}).items():
        im.putdata([dst + (p[3],) if p[3] and near(p[:3], src) else p for p in im.getdata()])
    assert im.width % 4 == 0 and im.height % 4 == 0, f
    out = os.path.join(DST, base.lower() + ".png")
    im.save(out, optimize=True)
    print(out, im.size, "key", key)
