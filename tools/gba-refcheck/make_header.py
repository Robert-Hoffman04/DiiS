#!/usr/bin/env python3
"""Generate a valid 192-byte GBA cartridge header and prepend it to a raw
ARM binary, producing a bootable .gba file.

The 156-byte Nintendo logo bitmap is copied byte-for-byte from this
codebase's own source/NDSSystem.cpp (GBA_LOGO[]) -- already committed,
upstream-derived data used purely for hardware/emulator boot-logo
detection, not redistributed Nintendo software. See docs/PLAN.md §4.3
item 8.
"""
import sys

# Copied verbatim from source/NDSSystem.cpp's GBA_LOGO[156].
GBA_LOGO = bytes([
    0x24,0xFF,0xAE,0x51,0x69,0x9A,0xA2,0x21,0x3D,0x84,0x82,0x0A,0x84,0xE4,0x09,0xAD,
    0x11,0x24,0x8B,0x98,0xC0,0x81,0x7F,0x21,0xA3,0x52,0xBE,0x19,0x93,0x09,0xCE,0x20,
    0x10,0x46,0x4A,0x4A,0xF8,0x27,0x31,0xEC,0x58,0xC7,0xE8,0x33,0x82,0xE3,0xCE,0xBF,
    0x85,0xF4,0xDF,0x94,0xCE,0x4B,0x09,0xC1,0x94,0x56,0x8A,0xC0,0x13,0x72,0xA7,0xFC,
    0x9F,0x84,0x4D,0x73,0xA3,0xCA,0x9A,0x61,0x58,0x97,0xA3,0x27,0xFC,0x03,0x98,0x76,
    0x23,0x1D,0xC7,0x61,0x03,0x04,0xAE,0x56,0xBF,0x38,0x84,0x00,0x40,0xA7,0x0E,0xFD,
    0xFF,0x52,0xFE,0x03,0x6F,0x95,0x30,0xF1,0x97,0xFB,0xC0,0x85,0x60,0xD6,0x80,0x25,
    0xA9,0x63,0xBE,0x03,0x01,0x4E,0x38,0xE2,0xF9,0xA2,0x34,0xFF,0xBB,0x3E,0x03,0x44,
    0x78,0x00,0x90,0xCB,0x88,0x11,0x3A,0x94,0x65,0xC0,0x7C,0x63,0x87,0xF0,0x3C,0xAF,
    0xD6,0x25,0xE4,0x8B,0x38,0x0A,0xAC,0x72,0x21,0xD4,0xF8,0x07,
])
assert len(GBA_LOGO) == 156


def build_header(title: bytes, game_code: bytes) -> bytes:
    assert len(title) <= 12
    assert len(game_code) == 4
    h = bytearray(192)
    # 0x00-0x03: entry point, ARM B to 0x080000C0 (right after the header),
    # PC-relative: ((0xC0 - (0x00 + 8)) >> 2) = 0x2E
    h[0:4] = (0xEA000000 | 0x2E).to_bytes(4, "little")
    h[0x04:0x04 + 156] = GBA_LOGO
    # Fixed 12-byte slice, not 0xA0:0xA0+len(title) -- that old form let
    # Python's slice-assignment silently RESIZE the bytearray whenever
    # title was shorter than 12 bytes (assigning a 12-byte value into an
    # N<12-byte slice grows the buffer by 12-N), shifting every byte after
    # it -- including the appended code body -- by that many bytes, while
    # the entry-point branch at offset 0 stays hardcoded for a 192-byte
    # header (target 0x080000C0). Found via a real, reproducible bug hunt
    # (docs/PLAN.md §4.3 item 6): every hand-titled test ROM with a title
    # shorter than 12 chars booted to a PC runaway (CPU executing
    # misaligned garbage from the shifted code) -- "GRADIENT"/"CHECKER"
    # (both exactly 8 chars) happened to shift by a whole 4-byte word,
    # which decodes as one skippable ANDEQ no-op and silently self-heals,
    # masking the bug; 5-char titles ("TEST4" etc, shift=7) are not
    # word-aligned and corrupt every subsequent instruction.
    h[0xA0:0xA0 + 12] = title.ljust(12, b"\x00")
    h[0xAC:0xAC + 4] = game_code
    h[0xB0:0xB2] = b"\x00\x00"     # maker code
    h[0xB2] = 0x96                  # fixed value
    h[0xB3] = 0x00                  # main unit code
    h[0xB4] = 0x00                  # device type
    # 0xB5-0xBB reserved, already zero
    h[0xBC] = 0x00                  # software version
    chk = 0
    for b in h[0xA0:0xBD]:
        chk = (chk - b) & 0xFF
    chk = (chk - 0x19) & 0xFF
    h[0xBD] = chk
    # 0xBE-0xBF reserved, already zero
    return bytes(h)


def main():
    if len(sys.argv) != 5:
        print(f"usage: {sys.argv[0]} <raw.bin> <out.gba> <title> <gamecode4>", file=sys.stderr)
        sys.exit(1)
    raw_path, out_path, title, code = sys.argv[1:5]
    with open(raw_path, "rb") as f:
        body = f.read()
    header = build_header(title.encode("ascii"), code.encode("ascii"))
    out = header + body
    # NDSSystem.cpp's NDS_LoadROM() rejects anything under 352 bytes
    # *before* it even peeks the GBA-logo bytes to route to the GBA load
    # path (that check assumes an NDS-shaped header, not a GBA one, but it
    # runs unconditionally first) -- pad well past that floor so our tiny
    # hand-assembled ROMs don't get silently bounced at the front gate.
    MIN_SIZE = 4096
    if len(out) < MIN_SIZE:
        out = out + b"\x00" * (MIN_SIZE - len(out))
    with open(out_path, "wb") as f:
        f.write(out)
    print(f"wrote {out_path}: {len(out)} bytes")


if __name__ == "__main__":
    main()
