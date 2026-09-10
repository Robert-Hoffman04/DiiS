#!/usr/bin/env python3
"""
symbolicate.py - turn raw PPC addresses from a PKT_CRASH into function+offset
using the build's linker .map file (plan §3.6 / §3.7).

The Makefile already emits `-Wl,-Map,$(notdir $@).map`, so the device only ever
needs to send addresses - all name resolution happens here.

CRASH payload is best-effort UTF-8 text from a dying machine: register lines and
a backtrace, one item per line, e.g.

    cause DSI
    pc  0x80123456
    lr  0x80100abc
    r1  0x81700f00
    bt  0x80123400
    bt  0x801008f0

render() rewrites every 0x8xxxxxxx token in that text as
`0x80123456 <NDS_exec+0x36>`. Standalone use:

    symbolicate.py desmumewii.elf.map < crash_1.txt
    symbolicate.py desmumewii.elf.map 0x80123456 0x80100abc
"""

import bisect
import re
import sys

_ADDR_RE = re.compile(r"\b0x8[0-9a-fA-F]{6,7}\b")
# devkitPPC / GNU ld map line:  "                0x0000000080123456   symbol_name"
_MAP_SYM_RE = re.compile(r"^\s+0x0*([0-9a-fA-F]{8,16})\s+(\S+)\s*$")


class Symbolizer:
    def __init__(self, map_path):
        self.addrs = []
        self.names = []
        self._load(map_path)

    def _load(self, path):
        table = {}
        try:
            with open(path, errors="replace") as fp:
                for line in fp:
                    m = _MAP_SYM_RE.match(line)
                    if not m:
                        continue
                    addr = int(m.group(1), 16) & 0xFFFFFFFF
                    name = m.group(2)
                    # keep the first name seen for an address; skip ld's
                    # "0x.. = ." style assignments and fill symbols
                    if name.startswith((".", "*", "PROVIDE", "0x", "ASSERT")):
                        continue
                    table.setdefault(addr, name)
        except OSError as e:
            print(f"symbolicate: cannot read {path}: {e}", file=sys.stderr)
        for a in sorted(table):
            self.addrs.append(a)
            self.names.append(table[a])

    def resolve(self, addr):
        addr &= 0xFFFFFFFF
        if not self.addrs:
            return None
        i = bisect.bisect_right(self.addrs, addr) - 1
        if i < 0:
            return None
        base = self.addrs[i]
        return f"{self.names[i]}+0x{addr - base:x}"

    def render(self, payload):
        if isinstance(payload, (bytes, bytearray)):
            payload = payload.decode("utf-8", "replace")

        def repl(m):
            sym = self.resolve(int(m.group(0), 16))
            return f"{m.group(0)} <{sym}>" if sym else m.group(0)

        return _ADDR_RE.sub(repl, payload)


def main(argv):
    if len(argv) < 2:
        print("usage: symbolicate.py <map-file> [addr ...]   (else reads stdin)",
              file=sys.stderr)
        return 2
    sym = Symbolizer(argv[1])
    if not sym.addrs:
        print("symbolicate: no symbols loaded", file=sys.stderr)
        return 1
    if len(argv) > 2:
        for a in argv[2:]:
            r = sym.resolve(int(a, 16))
            print(f"{a}  {r or '<no symbol>'}")
    else:
        sys.stdout.write(sym.render(sys.stdin.read()))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
