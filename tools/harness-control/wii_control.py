#!/usr/bin/env python3
"""
wii_control.py - desktop side of the unified harness transport (plan §2 / §3.7).

Listens on one TCP port for the device's HARNESS_TRANSPORT_NET connection - the
*same* server whether the device is Dolphin (network passthrough, per §0) or a
real Wii on the LAN. Receives LOG / FRAME / PROFILE / CRASH / ASSERT, sends
CTRL / INPUT.

Usage:
    wii_control.py [--host 0.0.0.0] [--port 4300] [--out DIR] [--map FILE]
                   [--watchdog SECONDS] [--frames-per-rom N]

While connected, stdin accepts line commands:
    <a|b|x|y|s|u|d|l|r|z|L|R ...>   send those as an INPUT packet
    :next_rom                       send CTRL next_rom
    :capture_frame [label]          send CTRL capture_frame
    :ctrl <text>                    send an arbitrary CTRL command
    :quit                           close and exit

Exit code is non-zero if any ASSERT failure or CRASH was seen (so an
unattended multi-ROM run produces a real pass/fail without a human reading logs).
"""

import argparse
import os
import selectors
import socket
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wire  # noqa: E402
from symbolicate import Symbolizer  # noqa: E402

INPUT_ALPHABET = set("abxysudlrzLR")


class Session:
    def __init__(self, conn, outdir, symbolizer, watchdog):
        self.conn = conn
        self.outdir = outdir
        self.sym = symbolizer
        self.watchdog = watchdog          # seconds, or 0/None to disable
        self.dec = wire.Decoder()
        self.log_fp = open(os.path.join(outdir, "harness.log"), "w", buffering=1)
        self.profile_fp = open(os.path.join(outdir, "profile.log"), "w", buffering=1)
        self.assert_failures = 0
        self.assert_passes = 0
        self.crashes = 0
        self.frames = 0
        self.last_heartbeat = time.monotonic()
        self.cur_rom = "?"
        self._lock = threading.Lock()

    # --- outbound ---------------------------------------------------------
    def send(self, ptype, payload=b""):
        with self._lock:
            try:
                self.conn.sendall(wire.encode(ptype, payload))
            except OSError as e:
                print(f"[send failed: {e}]", file=sys.stderr)

    def send_input(self, bytestr):
        self.send(wire.PKT_INPUT, bytestr.encode("ascii", "ignore"))

    def send_ctrl(self, text):
        self.send(wire.PKT_CTRL, text)
        print(f"[-> CTRL {text}]")

    # --- inbound ---------------------------------------------------------
    def handle(self, data):
        for ptype, payload in self.dec.feed(data):
            name = wire.TYPE_NAME.get(ptype, f"0x{ptype:02x}")
            fn = getattr(self, f"_on_{name.lower()}", None)
            if fn:
                fn(payload)
            else:
                print(f"[{name}] {len(payload)} bytes (unhandled)")

    def _on_log(self, payload):
        line = payload.decode("utf-8", "replace").rstrip("\n")
        print(line)
        self.log_fp.write(line + "\n")
        self.last_heartbeat = time.monotonic()

    def _on_profile(self, payload):
        text = payload.decode("utf-8", "replace")
        self.profile_fp.write(text if text.endswith("\n") else text + "\n")
        self.last_heartbeat = time.monotonic()

    def _on_ctrl(self, payload):
        cmd = payload.decode("utf-8", "replace").strip()
        self.last_heartbeat = time.monotonic()
        if cmd.startswith("heartbeat") or cmd == "ping":
            # device -> host liveness; optionally carries "heartbeat rom=<name>"
            for tok in cmd.split():
                if tok.startswith("rom="):
                    self.cur_rom = tok[4:]
            self.log_fp.write(f"[heartbeat] {cmd}\n")
            print(f"[<- {cmd}]")
            return
        if cmd == "pong":
            return
        print(f"[<- CTRL {cmd}]")
        if cmd.startswith("rom="):
            self.cur_rom = cmd[4:]

    def _on_frame(self, payload):
        f = wire.decode_frame(payload)
        self.frames += 1
        self.last_heartbeat = time.monotonic()
        base = f"frame_{f['seq']:06d}"
        if f["label"]:
            safe = "".join(c if c.isalnum() or c in "-_." else "_" for c in f["label"])
            base += f"_{safe}"
        path = self._write_image(f, base)
        print(f"[FRAME] seq={f['seq']} {f['w']}x{f['h']} fmt={f['fmt']} "
              f"tick={f['tick']} -> {os.path.basename(path)}")

    def _write_image(self, f, base):
        try:
            from PIL import Image
        except ImportError:
            path = os.path.join(self.outdir, base + f".{f['w']}x{f['h']}.bin")
            with open(path, "wb") as fp:
                fp.write(f["image"])
            return path
        w, h, fmt, img = f["w"], f["h"], f["fmt"], f["image"]
        if fmt == wire.FRAME_FMT_RGB565:
            im = Image.frombytes("RGB", (w, h),
                                 _rgb565_to_rgb888(img, w * h), "raw", "RGB")
        elif fmt == wire.FRAME_FMT_RGBA8888:
            im = Image.frombytes("RGBA", (w, h), img)
        elif fmt == wire.FRAME_FMT_BGRA8888:
            im = Image.frombytes("RGBA", (w, h), img)
            b, g, r, a = im.split()
            im = Image.merge("RGBA", (r, g, b, a))
        else:
            path = os.path.join(self.outdir, base + ".bin")
            with open(path, "wb") as fp:
                fp.write(img)
            return path
        path = os.path.join(self.outdir, base + ".png")
        im.save(path)
        return path

    def _on_assert(self, payload):
        # payload: 1 byte pass/fail (1=pass) + UTF-8 message
        ok = bool(payload[:1] and payload[0])
        msg = payload[1:].decode("utf-8", "replace")
        if ok:
            self.assert_passes += 1
        else:
            self.assert_failures += 1
            line = f"[ASSERT FAIL] rom={self.cur_rom} {msg}"
            print(line, file=sys.stderr)
            self.log_fp.write(line + "\n")
        self.last_heartbeat = time.monotonic()

    def _on_crash(self, payload):
        self.crashes += 1
        text = self.sym.render(payload) if self.sym else payload.decode(
            "utf-8", "replace")
        banner = "=" * 60
        print(f"\n{banner}\n[CRASH] rom={self.cur_rom}\n{text}\n{banner}\n",
              file=sys.stderr)
        with open(os.path.join(self.outdir, f"crash_{self.crashes}.txt"), "w") as fp:
            fp.write(text + "\n")

    # --- watchdog -------------------------------------------------------
    def watchdog_stale(self):
        if not self.watchdog:
            return False
        return (time.monotonic() - self.last_heartbeat) > self.watchdog


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


def stdin_loop(sess):
    for raw in sys.stdin:
        line = raw.strip()
        if not line:
            continue
        if line in (":quit", ":q"):
            os._exit(_exit_code(sess))
        if line.startswith(":ctrl "):
            sess.send_ctrl(line[6:].strip())
        elif line == ":next_rom":
            sess.send_ctrl("next_rom")
        elif line.startswith(":capture_frame"):
            rest = line[len(":capture_frame"):].strip()
            sess.send_ctrl("capture_frame" + (f" {rest}" if rest else ""))
        elif line.startswith(":movie"):
            # :movie record <dev-path> | :movie play <dev-path> | :movie stop
            sess.send_ctrl("movie " + line[len(":movie"):].strip())
        elif line.startswith(":"):
            print(f"[unknown command: {line}]", file=sys.stderr)
        else:
            seq = "".join(tok for tok in line.split() if tok)
            bad = set(seq) - INPUT_ALPHABET
            if bad:
                print(f"[bad input chars: {''.join(sorted(bad))}]", file=sys.stderr)
            else:
                sess.send_input(seq)


def _exit_code(sess):
    return 1 if (sess.assert_failures or sess.crashes) else 0


def serve(args):
    # line-buffer stdout so `wii_control.py > file &` shows progress live
    try:
        sys.stdout.reconfigure(line_buffering=True)
    except AttributeError:
        pass
    os.makedirs(args.out, exist_ok=True)
    sym = Symbolizer(args.map) if args.map else None

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.host, args.port))
    srv.listen(1)
    print(f"wii_control: listening on {args.host}:{args.port}  (out={args.out})")

    conn, addr = srv.accept()
    print(f"wii_control: connected from {addr[0]}:{addr[1]}")
    conn.setblocking(False)

    sess = Session(conn, args.out, sym, args.watchdog)
    threading.Thread(target=stdin_loop, args=(sess,), daemon=True).start()

    sel = selectors.DefaultSelector()
    sel.register(conn, selectors.EVENT_READ)
    try:
        while True:
            for key, _ in sel.select(timeout=1.0):
                data = key.fileobj.recv(65536)
                if not data:
                    print("wii_control: device closed the connection")
                    raise KeyboardInterrupt
                sess.handle(data)
            if sess.watchdog_stale():
                print(f"[WATCHDOG] no device traffic for {args.watchdog}s "
                      f"(rom={sess.cur_rom}) -> CTRL next_rom", file=sys.stderr)
                sess.log_fp.write(f"[WATCHDOG] hung rom={sess.cur_rom}\n")
                sess.send_ctrl("next_rom")
                sess.last_heartbeat = time.monotonic()
    except (KeyboardInterrupt, ConnectionError):
        pass
    finally:
        conn.close()
        srv.close()

    print(f"\nsummary: asserts {sess.assert_passes} pass / "
          f"{sess.assert_failures} fail, {sess.crashes} crash, "
          f"{sess.frames} frames captured")
    return _exit_code(sess)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="0.0.0.0",
                    help="bind address (default 0.0.0.0; use 127.0.0.1 for Dolphin-only)")
    ap.add_argument("--port", type=int, default=4300,
                    help="listen port (default 4300; wiiload's own 4299 is separate)")
    ap.add_argument("--out", default="harness-out",
                    help="directory for logs / captured frames / crash dumps")
    ap.add_argument("--map", default=None,
                    help="linker .map file for CRASH symbolication "
                         "(e.g. desmumewii.elf.map)")
    ap.add_argument("--watchdog", type=float, default=0,
                    help="seconds of device silence before sending CTRL next_rom "
                         "(0 = disabled)")
    ap.add_argument("--frames-per-rom", type=int, default=0,
                    help="reserved; the device drives rom advance today")
    args = ap.parse_args()
    sys.exit(serve(args))


if __name__ == "__main__":
    main()
