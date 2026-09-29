#!/usr/bin/env python3
"""Perf-zones table (Task pzones).

Reads the profile.log of one or more capture runs (capture.py output, a
DESMUME_PERFZONES build) and prints every zone sorted by cost, as
"% of frame (ms/frame)", one column per run:

  tools/benchmark/pztable.py software=RUN_DIR accurate=RUN_DIR fast=RUN_DIR
  options: --blocks N   average the last N 60-frame blocks (default 2)
           --classes    also print the per-frame-class split (pzcls lines:
                        GxFast record / replay, CPU raster, GxAccurate pass)
"""
import os, re, sys

CLS_NAMES = {0: "no 3D pass", 1: "GxFast record", 2: "GxFast replay", 4: "CPU raster",
             8: "GxAcc pass", 12: "GxAcc+raster", 5: "record+raster", 6: "replay+raster"}


def load(run, blocks):
    path = os.path.join(run, "profile.log") if os.path.isdir(run) else run
    hdr, rows, cls = None, [], {}
    for l in open(path, errors="replace").read().splitlines():
        if l.startswith("frame,wall_us"):
            hdr = [h[:-3] for h in l.split(",")[2:] if h.endswith("_us")]
        elif hdr and re.match(r"^\d+,\d+(,\d+)+$", l):
            v = list(map(int, l.split(",")))
            rows.append((v[0], v[1], dict(zip(hdr, v[2:2 + len(hdr)]))))
        elif l.startswith("pzcls "):
            kv = dict(x.split("=", 1) for x in l.split()[1:])
            cls.setdefault(int(kv["frame"]), []).append(kv)
    if not rows:
        sys.exit(f"{path}: no perf-zones rows")
    last = rows[-blocks:]
    nfr = sum(60 for _ in last)
    mean = {k: sum(r[2][k] for r in last) / nfr / 1000.0 for k in hdr}
    wall = sum(r[1] for r in last) / nfr / 1000.0
    C = {}
    for f, _, _ in last:
        for kv in cls.get(f, []):
            a = C.setdefault(int(kv["cls"]), {})
            for k, v in kv.items():
                if k not in ("frame", "cls"):
                    a[k] = a.get(k, 0) + int(v)
    return hdr, mean, wall, C


def table(cols, title):
    zones = cols[0][1][0]
    order = sorted(zones, key=lambda z: -max(c[1][1][z] for c in cols))
    print(f"### {title}")
    print("| zone | " + " | ".join(c[0] for c in cols) + " |")
    print("|---|" + "---|" * len(cols))
    for z in order:
        if max(c[1][1][z] for c in cols) < 0.005:
            continue
        print(f"| {z} | " + " | ".join(
            f"{100 * c[1][1][z] / c[1][2]:.1f}% ({c[1][1][z]:.2f}ms)" for c in cols) + " |")
    print("| **frame (sum)** | " + " | ".join(f"**{c[1][2]:.2f}ms**" for c in cols) + " |")
    print()


def main():
    args = sys.argv[1:]
    blocks, classes, runs = 2, False, []
    while args:
        a = args.pop(0)
        if a == "--blocks": blocks = int(args.pop(0))
        elif a == "--classes": classes = True
        else:
            lab, _, d = a.partition("=")
            runs.append((lab, d) if d else (os.path.basename(a.rstrip("/")), a))
    data = [(lab, load(d, blocks)) for lab, d in runs]
    table(data, f"zones, ms per frame (mean of last {blocks} x 60 frames)")
    if not classes:
        return
    for lab, (hdr, _, _, C) in data:
        if not C:
            continue
        cols = []
        for c in sorted(C):
            n = C[c]["n"]
            cols.append((f"{lab}: {CLS_NAMES.get(c, c)} (n={n})",
                         (hdr, {z: C[c].get(z, 0) / n / 1000.0 for z in hdr}, C[c]["wall"] / n / 1000.0)))
        table(cols, f"{lab} by frame class")


if __name__ == "__main__":
    main()
