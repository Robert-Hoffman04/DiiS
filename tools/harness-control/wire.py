#!/usr/bin/env python3
"""
wire.py - the DeSmuMEWii unified harness wire protocol (plan §2).

One TCP connection, one length-framed protocol, every packet type the harness
needs. Header is fixed 10 bytes, big-endian:

    magic (4 bytes) | version (1) | packet_type (1) | payload_length (u32) | payload

`magic` and the big-endian layout are chosen so a raw hexdump of the stream is
greppable and the framing survives the fact that payloads range from UTF-8 log
lines to raw framebuffer bytes - never assume one send() == one recv().

This module is the single source of truth for the constants; the device side
(source/src/harness/) must be kept byte-compatible with it.
"""

import struct

MAGIC = b"DSMH"          # DeSmuMEWii Harness
VERSION = 1
HEADER = struct.Struct(">4sBBI")   # magic, version, type, payload_length
HEADER_LEN = HEADER.size           # 10

# Packet types. device->host unless noted. Keep in sync with harness_transport.h.
PKT_LOG     = 0x01   # device->host  UTF-8 text line
PKT_FRAME   = 0x02   # device->host  seq, label, tick, dims, format, image bytes
PKT_INPUT   = 0x03   # host->device  raw bytes, geckoinput alphabet "abxysudlrzLR"
PKT_CTRL    = 0x04   # both          short text command (next_rom, capture_frame, ping)
PKT_PROFILE = 0x05   # device->host  structured zone / cache-stat rows (UTF-8, CSV-ish)
PKT_CRASH   = 0x06   # device->host  PC/LR/GPRs + short backtrace, best-effort one-shot
PKT_ASSERT  = 0x07   # device->host  pass/fail bit + message (failures only)

TYPE_NAME = {
    PKT_LOG: "LOG", PKT_FRAME: "FRAME", PKT_INPUT: "INPUT", PKT_CTRL: "CTRL",
    PKT_PROFILE: "PROFILE", PKT_CRASH: "CRASH", PKT_ASSERT: "ASSERT",
}
NAME_TYPE = {v: k for k, v in TYPE_NAME.items()}


def encode(packet_type, payload=b""):
    """Frame one packet. `payload` may be bytes or str (encoded as UTF-8)."""
    if isinstance(payload, str):
        payload = payload.encode("utf-8")
    return HEADER.pack(MAGIC, VERSION, packet_type, len(payload)) + payload


class ProtocolError(Exception):
    pass


class Decoder:
    """Incremental decoder. feed() bytes, iterate the (type, payload) it yields."""

    MAX_PAYLOAD = 8 * 1024 * 1024   # a DS framebuffer is ~256KB; 8MB is generous

    def __init__(self):
        self._buf = bytearray()

    def feed(self, data):
        self._buf.extend(data)
        while True:
            if len(self._buf) < HEADER_LEN:
                return
            magic, version, ptype, plen = HEADER.unpack_from(self._buf, 0)
            if magic != MAGIC:
                # resync: drop one byte and retry rather than wedging the stream
                del self._buf[0]
                continue
            if version != VERSION:
                raise ProtocolError(f"unsupported wire version {version}")
            if plen > self.MAX_PAYLOAD:
                raise ProtocolError(f"payload_length {plen} exceeds cap")
            if len(self._buf) < HEADER_LEN + plen:
                return
            payload = bytes(self._buf[HEADER_LEN:HEADER_LEN + plen])
            del self._buf[:HEADER_LEN + plen]
            yield ptype, payload


# --- payload helpers -------------------------------------------------------
# FRAME payload sub-header, big-endian: seq(u32) tick(u64) w(u16) h(u16)
# fmt(u8) label_len(u8) | label bytes | image bytes
_FRAME_SUBHDR = struct.Struct(">IQHHBB")

FRAME_FMT_RGB565 = 0
FRAME_FMT_RGBA8888 = 1
FRAME_FMT_BGRA8888 = 2


def decode_frame(payload):
    seq, tick, w, h, fmt, label_len = _FRAME_SUBHDR.unpack_from(payload, 0)
    off = _FRAME_SUBHDR.size
    label = payload[off:off + label_len].decode("utf-8", "replace")
    image = payload[off + label_len:]
    return dict(seq=seq, tick=tick, w=w, h=h, fmt=fmt, label=label, image=image)


def encode_frame(seq, tick, w, h, fmt, image, label=""):
    lb = label.encode("utf-8")[:255]
    return encode(PKT_FRAME,
                  _FRAME_SUBHDR.pack(seq, tick, w, h, fmt, len(lb)) + lb + image)
