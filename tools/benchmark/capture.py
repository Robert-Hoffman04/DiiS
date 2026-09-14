#!/usr/bin/env python3
"""
capture.py - single-shot network capture client for the desmumewii benchmark
system, speaking the same unified harness wire protocol as
tools/harness-control/wii_control.py (see that tool for interactive/live
session control; this one is non-interactive and built for scripted runs).

Listens for one device connection, records LOG/PROFILE lines and (on
request) one video frame, and exits on its own once a stop condition is
reached instead of a caller having to guess a sleep duration:

  --until-frame N       stop once a perf-zones row reports frame >= N
  --settle-frame N + --capture-frame LABEL
                         wait for frame >= N, request one PKT_CTRL
                         capture_frame, save the PKT_FRAME that comes back,
                         then stop
  --timeout SECONDS     hard cap regardless of the above (always active)

Also stops immediately on a PKT_CRASH. Exit code is non-zero on any ASSERT
failure or CRASH, so a caller gets real pass/fail without reading logs.

Writes into --out:
  harness.log     every LOG line
  profile.log     every PROFILE line (perf-zones CSV rows, frametime rows,
                   and any other harness_profile_emit() output)
  frame.png       the requested frame, if --capture-frame was given and a
                   frame came back (raw .bin if Pillow isn't installed)
  crash_N.txt     one per PKT_CRASH, symbolicated if --map is given
  summary.json    {frame, asserts_pass, asserts_fail, crashes, timed_out}
"""
import argparse
import json
import os
import re
import selectors
import socket
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                 "..", "harness-control"))
import wire  # noqa: E402
from symbolicate import Symbolizer  # noqa: E402

FRAME_ROW_RE = re.compile(r"^(\d+),\d+")
FRAMETIME_RE = re.compile(r"frame=(\d+)")


def _rgb565_to_rgb888(data, npix):
    out = bytearray(npix * 3)
    for i in range(min(npix, len(data) // 2)):
        v = data[2 * i] | (data[2 * i + 1] << 8)
        r = (v >> 11) & 0x1F
        g = (v >> 5) & 0x3F
        b = v & 0x1F
        out[3 * i] = (r << 3) | (r >> 2)
        out[3 * i + 1] = (g << 2) | (g >> 4)
        out[3 * i + 2] = (b << 3) | (b >> 2)
    return bytes(out)


def _save_frame(f, outdir, basename):
    try:
        from PIL import Image
    except ImportError:
        path = os.path.join(outdir, basename + f".{f['w']}x{f['h']}.bin")
        open(path, "wb").write(f["image"])
        return path
    w, h, fmt, img = f["w"], f["h"], f["fmt"], f["image"]
    if fmt == wire.FRAME_FMT_RGB565:
        im = Image.frombytes("RGB", (w, h), _rgb565_to_rgb888(img, w * h), "raw", "RGB")
    elif fmt == wire.FRAME_FMT_RGBA8888:
        im = Image.frombytes("RGBA", (w, h), img)
    elif fmt == wire.FRAME_FMT_BGRA8888:
        im = Image.frombytes("RGBA", (w, h), img)
        b, g, r, a = im.split()
        im = Image.merge("RGBA", (r, g, b, a))
    else:
        path = os.path.join(outdir, basename + ".bin")
        open(path, "wb").write(img)
        return path
    path = os.path.join(outdir, basename + ".png")
    im.save(path)
    return path


class Capture:
    def __init__(self, args, sym):
        self.args = args
        self.sym = sym
        self.frame = 0
        self.asserts_pass = 0
        self.asserts_fail = 0
        self.crashes = 0
        self.timed_out = False
        self.got_frame = False
        self.requested_capture = False
        self.log_fp = open(os.path.join(args.out, "harness.log"), "w", buffering=1)
        self.profile_fp = open(os.path.join(args.out, "profile.log"), "w", buffering=1)

    def _note_frame_number(self, text):
        for line in text.splitlines():
            m = FRAME_ROW_RE.match(line) or FRAMETIME_RE.search(line)
            if m:
                try:
                    self.frame = max(self.frame, int(m.group(1)))
                except ValueError:
                    pass

    def handle(self, ptype, payload, send):
        if ptype == wire.PKT_LOG:
            line = payload.decode("utf-8", "replace").rstrip("\n")
            self.log_fp.write(line + "\n")
        elif ptype == wire.PKT_PROFILE:
            text = payload.decode("utf-8", "replace")
            self.profile_fp.write(text if text.endswith("\n") else text + "\n")
            self._note_frame_number(text)
        elif ptype == wire.PKT_ASSERT:
            ok = bool(payload[:1] and payload[0])
            msg = payload[1:].decode("utf-8", "replace")
            if ok:
                self.asserts_pass += 1
            else:
                self.asserts_fail += 1
                line = f"[ASSERT FAIL] {msg}"
                print(line, file=sys.stderr)
                self.log_fp.write(line + "\n")
        elif ptype == wire.PKT_CRASH:
            self.crashes += 1
            text = self.sym.render(payload) if self.sym else payload.decode("utf-8", "replace")
            open(os.path.join(self.args.out, f"crash_{self.crashes}.txt"), "w").write(text + "\n")
            print(f"[CRASH]\n{text}", file=sys.stderr)
        elif ptype == wire.PKT_FRAME:
            f = wire.decode_frame(payload)
            path = _save_frame(f, self.args.out, "frame")
            print(f"[FRAME] {f['w']}x{f['h']} -> {os.path.basename(path)}")
            self.got_frame = True

    def maybe_request_capture(self, send):
        a = self.args
        if not a.capture_frame or self.requested_capture:
            return
        if self.frame >= a.settle_frame:
            send(wire.PKT_CTRL, f"capture_frame {a.capture_frame}")
            self.requested_capture = True

    def done(self):
        a = self.args
        if self.crashes:
            return True
        if a.capture_frame:
            return self.got_frame
        if a.until_frame and self.frame >= a.until_frame:
            return True
        return False

    def write_summary(self):
        out = {
            "frame": self.frame,
            "asserts_pass": self.asserts_pass,
            "asserts_fail": self.asserts_fail,
            "crashes": self.crashes,
            "timed_out": self.timed_out,
            "got_frame": self.got_frame,
        }
        json.dump(out, open(os.path.join(self.args.out, "summary.json"), "w"), indent=2)
        return out


def run(args):
    os.makedirs(args.out, exist_ok=True)
    sym = Symbolizer(args.map) if args.map else None
    cap = Capture(args, sym)

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.host, args.port))
    srv.listen(1)
    srv.settimeout(args.connect_timeout)
    try:
        conn, addr = srv.accept()
    except socket.timeout:
        print(f"capture: no device connected within {args.connect_timeout}s", file=sys.stderr)
        cap.timed_out = True
        cap.write_summary()
        return 2
    print(f"capture: connected from {addr[0]}:{addr[1]}")
    conn.setblocking(False)

    def send(ptype, payload=b""):
        try:
            conn.sendall(wire.encode(ptype, payload))
        except OSError:
            pass

    dec = wire.Decoder()
    sel = selectors.DefaultSelector()
    sel.register(conn, selectors.EVENT_READ)
    t0 = time.monotonic()
    try:
        while True:
            if time.monotonic() - t0 > args.timeout:
                print(f"capture: timeout after {args.timeout}s (last frame={cap.frame})",
                      file=sys.stderr)
                cap.timed_out = True
                break
            for key, _ in sel.select(timeout=0.25):
                data = key.fileobj.recv(65536)
                if not data:
                    print("capture: device closed the connection")
                    cap.timed_out = cap.timed_out or not cap.done()
                    raise StopIteration
                for ptype, payload in dec.feed(data):
                    cap.handle(ptype, payload, send)
            cap.maybe_request_capture(send)
            if cap.done():
                break
    except StopIteration:
        pass
    finally:
        conn.close()
        srv.close()

    summary = cap.write_summary()
    print(f"capture: frame={summary['frame']} asserts={summary['asserts_pass']}"
          f"/{summary['asserts_fail']} crashes={summary['crashes']}")
    if summary["asserts_fail"] or summary["crashes"]:
        return 1
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=4300)
    ap.add_argument("--out", required=True, help="directory for logs/frame/summary")
    ap.add_argument("--until-frame", type=int, default=0,
                     help="stop once a perf-zones row reports this frame number")
    ap.add_argument("--settle-frame", type=int, default=0,
                     help="frame to wait for before --capture-frame is sent")
    ap.add_argument("--capture-frame", metavar="LABEL",
                     help="request one PKT_FRAME (labelled LABEL) once --settle-frame "
                          "is reached, save it, then stop")
    ap.add_argument("--timeout", type=float, default=180,
                     help="hard cap in seconds regardless of frame progress (default 180)")
    ap.add_argument("--connect-timeout", type=float, default=30,
                     help="seconds to wait for the device to connect (default 30)")
    ap.add_argument("--map", help="linker .map file for CRASH symbolication")
    args = ap.parse_args()
    sys.exit(run(args))


if __name__ == "__main__":
    main()
