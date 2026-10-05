#!/usr/bin/env python3
"""Perf-zones table (Task pzones).

Reads the profile.log of one or more capture runs (capture.py output, a
DESMUME_PERFZONES build) and prints every zone sorted by cost, as
"% of frame (ms/frame)", one column per run:

  tools/benchmark/pztable.py software=RUN_DIR accurate=RUN_DIR fast=RUN_DIR
  options: --blocks N   average the last N 60-frame blocks (default 2)
           --classes    also print the per-frame-class split (pzcls lines:
                        GxFast record / replay, CPU raster, GxAccurate pass)
           --gxstats F  (Task hw-measure) also print the GX gate/bail stat lines of
                        F (sd:/gxstats.log, or a harness profile.log) as deltas over
                        the last --blocks x 60 frames (the counters are cumulative)

A RUN may also be a bare perfzones.log / profile.log file (e.g. sd:/perfzones.log
from a real Wii). If it has -DDESMUME_PERFZONES_PMC columns, a second table per run
shows the Broadway performance counters per zone (Task hw-measure): CPI, L1 I/D
misses per 1000 instructions (set 0), L2 I/D misses and branch mispredicts per 1000
instructions (set 1), and the share of the zone's cycles spent in I-fetch miss /
L1 load-miss / DTLB-search stalls (set 2; cycles = zone time x 729 MHz). The event
set rotates every block, so these average the last max(--blocks, 6) blocks.
"""
import os, re, sys

CLS_NAMES = {0: "no 3D pass", 1: "GxFast record", 2: "GxFast replay", 4: "CPU raster",
             8: "GxAcc pass", 12: "GxAcc+raster", 5: "record+raster", 6: "replay+raster"}


PMC_SETS = {0: ("instr", "l1imiss", "l1dmiss", "cycles"), 1: ("instr", "l2imiss", "l2dmiss", "bmispred"),
            2: ("instr", "imisscyc", "ldmisscyc", "dtlbcyc")}
CORE_MHZ = 729.0   # Broadway core clock (= timebase x 12)


def load(run, blocks):
    path = os.path.join(run, "profile.log") if os.path.isdir(run) else run
    hdr, rows, cls, pmcidx, pmc = None, [], {}, None, []
    for l in open(path, errors="replace").read().splitlines():
        if l.startswith("frame,wall_us"):
            cols = l.split(",")
            hdr = [h[:-3] for h in cols[2:] if h.endswith("_us")]
            pmcidx = cols.index("pmcset") if "pmcset" in cols else None
        elif hdr and re.match(r"^\d+,\d+(,\d+)+$", l):
            v = list(map(int, l.split(",")))
            rows.append((v[0], v[1], dict(zip(hdr, v[2:2 + len(hdr)]))))
            if pmcidx is not None and len(v) >= pmcidx + 1 + 4 * len(hdr):
                p = v[pmcidx + 1:]
                pmc.append((v[pmcidx], rows[-1][2], {z: p[4 * i:4 * i + 4] for i, z in enumerate(hdr)}))
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
    return hdr, mean, wall, C, pmc[-max(blocks, 6):]


def pmc_table(lab, hdr, pmc):
    """Per-zone hardware-counter rates, one row per zone (Task hw-measure)."""
    us, ev, nblk = {z: 0 for z in hdr}, {}, {}
    for st, zus, p in pmc:
        nblk[st] = nblk.get(st, 0) + 1
        for z in hdr:
            us[z] += zus[z]
            e = ev.setdefault(z, {})
            e.setdefault(("us", st), 0)
            e[("us", st)] += zus[z]
            for name, val in zip(PMC_SETS.get(st, ("instr", "p2", "p3", "p4")), p[z]):
                e[(name, st)] = e.get((name, st), 0) + val
    nfr = 60 * len(pmc)
    print(f"### {lab}: Broadway counters per zone ({len(pmc)} blocks, sets "
          + ",".join(f"{k}x{v}" for k, v in sorted(nblk.items())) + ")")
    print("| zone | ms | MIPS | CPI | cyc/us | L1I/ki | L1D/ki | L2I/ki | L2D/ki | bmis/ki | imiss cyc% | ldmiss cyc% | dtlb cyc% |")
    print("|---|" + "---|" * 12)

    def rate(e, name, st, per):
        i = e.get(("instr", st), 0)
        return f"{1000.0 * e[(name, st)] / i:.2f}" if i and (name, st) in e and per else "-"

    def share(e, name, st):
        cyc = e.get(("us", st), 0) * CORE_MHZ
        return f"{100.0 * e[(name, st)] / cyc:.1f}" if cyc and (name, st) in e else "-"

    for z in sorted(hdr, key=lambda z: -us[z]):
        if us[z] / nfr < 5:          # < 5 us/frame
            continue
        e = ev[z]
        instr = sum(e.get(("instr", st), 0) for st in nblk)
        iu = sum(e.get(("us", st), 0) for st in nblk)
        mips = f"{instr / iu:.0f}" if iu else "-"
        if e.get(("instr", 0)) and e.get(("cycles", 0)):
            cpi = f"{e[('cycles', 0)] / e[('instr', 0)]:.2f}"
        elif instr:
            cpi = f"{iu * CORE_MHZ / instr:.2f}t"   # time-based (no cycle counter in the window)
        else:
            cpi = "-"
        # set-0 cycle counter vs zone time: ~729 on hardware (a sanity check of the counters)
        cpu = f"{e[('cycles', 0)] / e[('us', 0)]:.0f}" if e.get(("us", 0)) and ("cycles", 0) in e else "-"
        print(f"| {z} | {us[z] / nfr / 1000.0:.2f} | {mips} | {cpi} | {cpu} | {rate(e, 'l1imiss', 0, 1)} | {rate(e, 'l1dmiss', 0, 1)} | "
              f"{rate(e, 'l2imiss', 1, 1)} | {rate(e, 'l2dmiss', 1, 1)} | {rate(e, 'bmispred', 1, 1)} | "
              f"{share(e, 'imisscyc', 2)} | {share(e, 'ldmisscyc', 2)} | {share(e, 'dtlbcyc', 2)} |")
    print()


GAUGES = {"mode", "t1", "t2", "eva", "evb", "en", "under", "tex", "ci", "live",
          "rgb5a3equiv", "seq", "tagmax"}


def gxstats(path, blocks):
    """Delta of every numeric key=value of each stat-line kind over the last blocks x 60
    frames (the -DDSB_STATS / -DDSA_GXGEOM_TEXSTATS counters are cumulative)."""
    marks, lines = [], []
    for l in open(path, errors="replace").read().splitlines():
        m = re.match(r"^# gxstats frame=(\d+)", l)
        if m:
            marks.append(len(lines))
            continue
        if re.match(r"^(ds[ab]\w*|gxds3dstats|rasterskip) ", l):
            lines.append(l)
    if not lines:
        print(f"{path}: no gx stat lines"); return
    # The last block is left out: a log cut by a power-off / kill mid-write ends in a
    # partial block (each block is written after its "# gxstats" marker).
    hi = marks[-1] if len(marks) > 1 else len(lines)
    lo = marks[-blocks - 1] if len(marks) > blocks else 0

    def kv(l):
        return {k: int(v) for k, v in re.findall(r"(\S+?)=(\d+)(?=\s|$)", l)
                if k not in ("dispcnt", "types")}   # hex register snapshots

    first, last = {}, {}
    for i, l in enumerate(lines[:hi]):
        tag = l.split()[0]
        if i < lo:
            first[tag] = l
        last[tag] = l
    print(f"### gx stats: {path} (delta over {blocks} x 60 frames before the last block; n = frames covered)")
    for tag in sorted(last):
        a, b = kv(first.get(tag, "")), kv(last[tag])
        # gauges / register snapshots print as their last value, counters as deltas
        d = " ".join(f"{k}={b[k]}" if k in GAUGES else f"{k}={b[k] - a.get(k, 0)}"
                     for k in b if k in GAUGES or b[k] - a.get(k, 0))
        why = re.search(r" why=(\S*)", last[tag])
        print(f"{tag:12s} {d}" + (f"  (last why={why.group(1)})" if why else ""))
    print()


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
    blocks, classes, runs, gxs = 2, False, [], None
    while args:
        a = args.pop(0)
        if a == "--blocks": blocks = int(args.pop(0))
        elif a == "--classes": classes = True
        elif a == "--gxstats": gxs = args.pop(0)
        else:
            lab, _, d = a.partition("=")
            runs.append((lab, d) if d else (os.path.basename(a.rstrip("/")), a))
    data = [(lab, load(d, blocks)) for lab, d in runs]
    table(data, f"zones, ms per frame (mean of last {blocks} x 60 frames)")
    for lab, (hdr, _, _, _, pmc) in data:
        if pmc:
            pmc_table(lab, hdr, pmc)
    if gxs:
        gxstats(gxs, blocks)
    if not classes:
        return
    for lab, (hdr, _, _, C, _) in data:
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
