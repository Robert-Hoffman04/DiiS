#!/usr/bin/env python3
"""
Break a -DDESMUME_PERFZONES capture (sd:/perfzones.log) into where the emulated
frame's wall time actually goes: CPU interpreter vs CPU JIT-execute vs JIT-build
vs geometry engine vs 3D render vs 2D compositor vs SPU vs GX present vs other.

A capture is one dump of sd:/perfzones.log, produced by the benchmark's
`profile` mode (tools/benchmark/benchmark.sh --modes profile) and dropped at
  <RUNDIR>/raw/<scene>_<mode>.perfzones.log

Row:  frame,wall_us,<zone>_us... ,<zone>_hits...
with the zone column order fixed by source/src/perf_zones.{h,cpp}:
  other arm9_interp arm9_jit arm9_build arm7_interp arm7_jit arm7_build
  gpu_ge gpu_render gpu_2d spu draw

Usage:
  perfzones.py <RUNDIR>                 analyse every raw/*.perfzones.log
  perfzones.py path/to/one.perfzones.log
  perfzones.py <RUNDIR> --compare <OLD_RUNDIR>
  perfzones.py <file> --window 300-1200
"""
import sys
import os
import re
import glob
import json
import argparse
import statistics

HERE = os.path.dirname(os.path.abspath(__file__))
TARGET_HZ = 59.8261

# grouping of the raw zones into report rows (kept in source order within a group)
GROUPS = [
    ("ARM9  interpreter",  ["arm9_interp"]),
    ("ARM9  JIT execute",  ["arm9_jit"]),
    ("ARM9  JIT build",    ["arm9_build"]),
    ("ARM7  interpreter",  ["arm7_interp"]),
    ("ARM7  JIT execute",  ["arm7_jit"]),
    ("ARM7  JIT build",    ["arm7_build"]),
    ("GPU   geometry eng", ["gpu_ge"]),
    ("GPU   3D render",    ["gpu_render"]),
    ("GPU   2D compositor",["gpu_2d"]),
    ("GPU   GX2DBG bake",  ["gx2dbg_bake"]),
    ("SPU",                ["spu"]),
    ("GX    screen convert",["draw_convert"]),
    ("GX    band present", ["draw_present"]),
    ("GX    present resid",["draw"]),
    ("DMA   channels",     ["dma"]),
    ("other / glue",       ["other"]),
]
SUPERGROUPS = [
    ("CPU  ARM9",  ["arm9_interp", "arm9_jit", "arm9_build"]),
    ("CPU  ARM7",  ["arm7_interp", "arm7_jit", "arm7_build"]),
    ("GPU  (GE+3D+2D)", ["gpu_ge", "gpu_render", "gpu_2d", "gx2dbg_bake"]),
    ("SPU",        ["spu"]),
    ("GX present",  ["draw", "draw_convert", "draw_present"]),
    ("DMA",        ["dma"]),
    ("other",      ["other"]),
]


# ---------------------------------------------------------------- parsing ---
def parse_capture(path):
    zones = None
    block = None
    rows = []
    with open(path, errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            if line.startswith("#"):
                m = re.search(r"block=(\d+)", line)
                if m:
                    block = int(m.group(1))
                continue
            if line.startswith("frame,"):
                cols = line.split(",")
                zones = [c[:-3] for c in cols if c.endswith("_us") and c != "wall_us"]
                continue
            parts = line.split(",")
            if zones is None or len(parts) != 2 + 2 * len(zones):
                continue
            try:
                vals = [int(x) for x in parts]
            except ValueError:
                continue
            frame = vals[0]
            us = dict(zip(zones, vals[2:2 + len(zones)]))
            hits = dict(zip(zones, vals[2 + len(zones):]))
            rows.append((frame, vals[1], us, hits))
    return block, zones, rows


def resolve_window(rows, window):
    lo_f, hi_f = rows[0][0], rows[-1][0]
    if window and "-" in window:
        lo, hi = (int(x) for x in window.split("-", 1))
        return lo, hi
    span = hi_f - lo_f
    return lo_f + int(span * 0.15), lo_f + int(span * 0.9)


def load_scene_windows():
    p = os.path.join(HERE, "scenes.conf")
    out = {}
    if not os.path.exists(p):
        return out
    for line in open(p):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        fld = [x.strip() for x in line.split("|")]
        if len(fld) >= 3:
            out[fld[0]] = fld[2]
    return out


def summarize(path, window):
    block, zones, rows = parse_capture(path)
    if not rows or len(rows) < 3:
        return {"error": "only %d rows" % len(rows)}
    lo, hi = resolve_window(rows, window)
    kept = [r for r in rows if lo <= r[0] <= hi]
    if len(kept) < 2:
        kept = rows[1:]
        lo, hi = kept[0][0], kept[-1][0]

    agg = {z: 0 for z in zones}
    hits = {z: 0 for z in zones}
    wall = 0
    for _, w, us, h in kept:
        wall += w
        for z in zones:
            agg[z] += us[z]
            hits[z] += h[z]
    total_us = sum(agg.values()) or 1

    # per-block fps for context, and its stability
    fps = [block / (r[1] / 1e6) for r in kept if r[1] > 0]
    mean_fps = statistics.mean(fps) if fps else 0.0

    return {
        "zones": zones,
        "us": agg,
        "hits": hits,
        "wall_us": wall,
        "total_us": total_us,
        "blocks": len(kept),
        "block_frames": block,
        "frame_lo": lo,
        "frame_hi": hi,
        "eff_fps": round(mean_fps, 2),
        "pct_realtime": round(100 * mean_fps / TARGET_HZ, 1) if mean_fps else None,
        "fps_cv_pct": round(100 * statistics.pstdev(fps) / mean_fps, 1) if len(fps) > 1 and mean_fps else None,
    }


def _pick(d, keys):
    return sum(d.get(k, 0) for k in keys)


# --------------------------------------------------------------- printing ---
def bar(frac, width=42):
    n = int(round(frac * width))
    return "#" * n + "." * (width - n)


def print_one(title, s, prev=None):
    print()
    print("  %s" % title)
    if "error" in s:
        print("    !! %s" % s["error"])
        return
    tot = s["total_us"]
    per_frame_ms = tot / 1000.0 / (s["blocks"] * s["block_frames"])
    print("    window frames %d-%d   %d blocks x %d frames   eff %s fps (%s%% real-time%s)"
          % (s["frame_lo"], s["frame_hi"], s["blocks"], s["block_frames"],
             s["eff_fps"], s["pct_realtime"],
             "" if s["fps_cv_pct"] is None else ", cv %s%%" % s["fps_cv_pct"]))
    ideal_ms = 1000.0 / TARGET_HZ
    print("    instrumented wall: %.2f ms/frame  (vs %.2f ms ideal at %.4f Hz -> %.2fx slower; "
          "unzoned frame-loop glue is the small remainder)"
          % (per_frame_ms, ideal_ms, TARGET_HZ, per_frame_ms / ideal_ms))
    print()
    print("    %-20s %10s %8s   %s" % ("zone", "ms/frame", "share", "share of instrumented wall"))
    print("    " + "-" * 78)
    nframes = s["blocks"] * s["block_frames"]
    for label, keys in GROUPS:
        us = _pick(s["us"], keys)
        if us == 0:
            continue
        frac = us / tot
        msf = us / 1000.0 / nframes
        delta = ""
        if prev and "error" not in prev:
            p_frac = _pick(prev["us"], keys) / prev["total_us"]
            dp = (frac - p_frac) * 100
            if abs(dp) >= 0.3:
                delta = "  %+.1fpp" % dp
        print("    %-20s %10.3f %7.1f%%   %s%s" % (label, msf, 100 * frac, bar(frac), delta))
    print("    " + "-" * 78)
    print("    %-20s %10.3f %7s" % ("TOTAL", per_frame_ms, "100.0%"))
    print()
    print("    rollup:")
    for label, keys in SUPERGROUPS:
        us = _pick(s["us"], keys)
        if us == 0:
            continue
        frac = us / tot
        print("      %-16s %7.1f%%   %s" % (label, 100 * frac, bar(frac, 32)))
    # a couple of derived call-rate stats
    h = s["hits"]
    nf = nframes
    print()
    print("    build episodes/frame:  arm9 %.1f   arm7 %.1f       "
          "(a 'hit' = a transition into that zone)"
          % (h.get("arm9_build", 0) / nf, h.get("arm7_build", 0) / nf))
    a9 = _pick(s["us"], ["arm9_interp", "arm9_jit", "arm9_build"]) or 1
    print("    ARM9 CPU split:  interp %.0f%%  jit-exec %.0f%%  jit-build %.0f%%"
          % (100 * s["us"].get("arm9_interp", 0) / a9,
             100 * s["us"].get("arm9_jit", 0) / a9,
             100 * s["us"].get("arm9_build", 0) / a9))


# ------------------------------------------------------------------ report ---
def write_report(results, path):
    L = ["# Full-JIT frame-time breakdown (perf_zones)\n"]
    L.append("Where the emulated frame's wall time goes, measured on the Wii "
             "timebase with the `perf_zones` accountant (`-DDESMUME_PERFZONES`). "
             "Real frame budget at %.4f Hz is %.2f ms.\n" % (TARGET_HZ, 1000.0 / TARGET_HZ))
    for name, s in results.items():
        L.append("## %s\n" % name)
        if "error" in s:
            L.append("_%s_\n" % s["error"])
            continue
        nframes = s["blocks"] * s["block_frames"]
        tot = s["total_us"]
        L.append("_frames %d-%d, eff %s fps (%s%% real-time)_\n"
                 % (s["frame_lo"], s["frame_hi"], s["eff_fps"], s["pct_realtime"]))
        L.append("| zone | ms/frame | share |")
        L.append("|---|--:|--:|")
        for label, keys in GROUPS:
            us = _pick(s["us"], keys)
            if us == 0:
                continue
            L.append("| %s | %.3f | %.1f%% |"
                     % (label.strip(), us / 1000.0 / nframes, 100 * us / tot))
        L.append("| **TOTAL** | **%.3f** | **100%%** |" % (tot / 1000.0 / nframes))
        L.append("")
        L.append("| rollup | share |")
        L.append("|---|--:|")
        for label, keys in SUPERGROUPS:
            us = _pick(s["us"], keys)
            if us:
                L.append("| %s | %.1f%% |" % (label.strip(), 100 * us / tot))
        L.append("")
    open(path, "w").write("\n".join(L))


# -------------------------------------------------------------------- main ---
def collect(target, window_override):
    """target: a run dir (has raw/) or a single .perfzones.log file."""
    windows = load_scene_windows()
    out = {}
    if os.path.isdir(target):
        files = sorted(glob.glob(os.path.join(target, "raw", "*.perfzones.log")))
        for fn in files:
            base = os.path.basename(fn)[:-len(".perfzones.log")]
            scene = base.rsplit("_", 1)[0] if "_" in base else base
            win = window_override or windows.get(scene, "")
            out[base] = summarize(fn, win)
    else:
        base = os.path.basename(target)
        for suf in (".perfzones.log", ".log"):
            if base.endswith(suf):
                base = base[:-len(suf)]
                break
        scene = base.rsplit("_", 1)[0] if "_" in base else base
        win = window_override or windows.get(scene, "")
        out[base] = summarize(target, win)
    return out


def main():
    ap = argparse.ArgumentParser(description="perf_zones frame-time breakdown")
    ap.add_argument("target", help="run dir (with raw/) or a single .perfzones.log")
    ap.add_argument("--compare", help="previous run dir / file to diff against")
    ap.add_argument("--window", help="override frame window LO-HI")
    ap.add_argument("--json", action="store_true", help="dump results.perfzones.json too")
    args = ap.parse_args()

    cur = collect(args.target, args.window)
    prev = collect(args.compare, args.window) if args.compare else {}

    print()
    print("  DeSmuME Wii - full-JIT frame-time breakdown (perf_zones)")
    for name, s in cur.items():
        pkey = name if name in prev else next((k for k in prev), None)
        print_one(name, s, prev.get(pkey) if prev else None)

    if os.path.isdir(args.target):
        rep = os.path.join(args.target, "report_perfzones.md")
        write_report(cur, rep)
        print()
        print("  wrote %s" % rep)
        if args.json:
            jp = os.path.join(args.target, "results.perfzones.json")
            json.dump(cur, open(jp, "w"), indent=2)
            print("  wrote %s" % jp)
    print()


if __name__ == "__main__":
    main()
