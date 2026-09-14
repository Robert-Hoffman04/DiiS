#!/usr/bin/env python3
"""
Analyse desmumewii renderer-benchmark captures and diff against a previous run.

A "capture" is one dump of the emulator's sd:/bench.log (produced by a
-DDESMUME_BENCH build - see source/src/main.cpp).  benchmark.sh drops one per
scene/renderer into  <RUNDIR>/raw/<scene>_<mode>.log .

Usage:
  analyze.py <RUNDIR> [--scenes PATH] [--baseline DIR] [--no-compare]
        parse <RUNDIR>/raw/*, write <RUNDIR>/results.json + report.md,
        print a summary and (unless --no-compare) a diff against the most
        recent previous run under the same results/ directory.

  analyze.py --compare <DIR_A> <DIR_B>
        just diff two finished runs (each needs a results.json).

bench.log row:  frame,wall_us,block_us,exec_us,draw_us
  frame     NDS frame number at end of the block
  wall_us   cumulative Wii-timebase us since the first benched frame
  block_us  wall us for this block of `frame - prev_frame` frames
  exec_us   us in NDS_exec() over the block
  draw_us   us in Draw()     over the block
"""
import sys
import os
import re
import json
import glob
import argparse
import statistics

TARGET_HZ = 59.8261
# GXMerge/GX2DBG compositing is mandatory whenever the GX core runs, so
# there is no separate "merge" build to isolate anymore (removed, not kept
# as an alias - see tools/benchmark/README.md).
MODE_LABEL = {"sw": "software rasterizer", "gx": "GX hardware 3D + GXMerge",
              "jitoff": "ARM7 interpreter", "jiton": "ARM7 JIT",
              "jit9off": "ARM9 interpreter", "jit9on": "ARM9 JIT",
              "jitfull": "full JIT (GXMerge)"}
MODE_ORDER = ["sw", "gx", "jitoff", "jiton", "jit9off", "jit9on", "jitfull"]
REGRESSION_REL = 0.03   # >3% relative eff_fps drop vs baseline -> flag


# ---------------------------------------------------------------- parsing ---
def parse_capture(path):
    core = gxmerge = None
    rows = []
    # a SIGKILL mid-write can leave a stray non-UTF8 byte in the tail line
    with open(path, errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            if line.startswith("#"):
                m = re.search(r"core=(\d+)\s+gxmerge=(\d+)", line)
                if m:
                    core, gxmerge = int(m.group(1)), int(m.group(2))
                continue
            if line.startswith("frame,"):
                continue
            parts = line.split(",")
            if len(parts) != 5:
                continue                       # truncated tail line from SIGKILL
            try:
                rows.append(tuple(int(x) for x in parts))
            except ValueError:
                continue
    return core, gxmerge, rows


def resolve_window(rows, window):
    """window is 'LO-HI' from scenes.conf, or '' => auto (25%..90% of max frame)."""
    block = rows[0][0]
    maxf = rows[-1][0]
    if window and "-" in window:
        lo, hi = (int(x) for x in window.split("-", 1))
    else:
        lo = max(block, (int(maxf * 0.25) // block) * block)
        hi = (int(maxf * 0.90) // block) * block
    return lo, hi


def summarize(path, window):
    core, gxmerge, rows = parse_capture(path)
    if len(rows) < 3:
        return {"error": "only %d data rows in %s" % (len(rows), os.path.basename(path))}

    block = rows[0][0]
    lo, hi = resolve_window(rows, window)
    kept = [r for r in rows if lo <= r[0] <= hi]
    if len(kept) < 2:
        kept = rows[1:]                        # fallback: just drop the boot block
        lo, hi = kept[0][0], kept[-1][0]

    fps = [block / (r[2] / 1e6) for r in kept]
    mean_fps = statistics.mean(fps)
    exec_sh = statistics.mean(r[3] / r[2] for r in kept) * 100
    draw_sh = statistics.mean(r[4] / r[2] for r in kept) * 100
    return {
        "core": core,
        "gxmerge": gxmerge,
        "eff_fps": round(mean_fps, 3),
        "pct_realtime": round(100 * mean_fps / TARGET_HZ, 2),
        "slowdown": round(TARGET_HZ / mean_fps, 3),
        "exec_pct": round(exec_sh, 1),
        "draw_pct": round(draw_sh, 1),
        "fps_min": round(min(fps), 2),
        "fps_max": round(max(fps), 2),
        "fps_cv_pct": round(100 * statistics.pstdev(fps) / mean_fps, 1) if mean_fps else None,
        "blocks_kept": len(kept),
        "blocks_total": len(rows),
        "frame_lo": lo,
        "frame_hi": hi,
        "block_frames": block,
    }


def load_scenes(path):
    scenes = {}
    if not path or not os.path.exists(path):
        return scenes
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            fld = [x.strip() for x in line.split("|")]
            sid = fld[0]
            scenes[sid] = {
                "rom": fld[1] if len(fld) > 1 else "",
                "window": fld[2] if len(fld) > 2 else "",
                "label": fld[3] if len(fld) > 3 and fld[3] else sid,
            }
    return scenes


# ------------------------------------------------------------- assembling ---
def build_results(rundir, scenes_path):
    scenes_cfg = load_scenes(scenes_path)
    meta = {}
    mp = os.path.join(rundir, "meta.json")
    if os.path.exists(mp):
        meta = json.load(open(mp))

    raw = os.path.join(rundir, "raw")
    caps = {}
    for fn in sorted(os.listdir(raw)):
        if not fn.endswith(".log") or fn.startswith("dolphin_"):
            continue
        if fn.endswith(".perfzones.log"):
            continue                       # handled by perfzones.py, not here
        base = fn[:-4]
        if "_" not in base:
            continue
        scene, mode = base.rsplit("_", 1)
        caps.setdefault(scene, {})[mode] = os.path.join(raw, fn)

    # scenes.conf order first, then any capture not mentioned there
    order = [s for s in scenes_cfg if s in caps] + [s for s in sorted(caps) if s not in scenes_cfg]

    out = {"meta": meta, "target_hz": TARGET_HZ, "scenes": {}}
    for scene in order:
        cfg = scenes_cfg.get(scene, {"label": scene, "window": ""})
        entry = {"label": cfg.get("label", scene), "window": cfg.get("window", ""), "modes": {}}
        ordered = [m for m in MODE_ORDER if m in caps[scene]] + \
                  [m for m in caps[scene] if m not in MODE_ORDER]
        for mode in ordered:
            entry["modes"][mode] = summarize(caps[scene][mode], cfg.get("window", ""))
        out["scenes"][scene] = entry
    return out


def find_previous(rundir, baseline):
    if baseline:
        p = baseline
        if os.path.isdir(p):
            p = os.path.join(p, "results.json")
        return p if os.path.exists(p) else None
    root = os.path.dirname(os.path.abspath(rundir))
    here = os.path.abspath(rundir)
    cands = sorted(glob.glob(os.path.join(root, "*", "results.json")))
    cands = [c for c in cands if os.path.abspath(os.path.dirname(c)) != here]
    return cands[-1] if cands else None


# --------------------------------------------------------------- printing ---
def _mode_name(mode):
    return MODE_LABEL.get(mode, mode)


def print_summary(res):
    print()
    print("  DeSmuME Wii renderer benchmark  -  emulation speed vs real DS (%.4f Hz)" % TARGET_HZ)
    m = res.get("meta", {})
    if m:
        print("  %s  |  git %s  |  %s" % (m.get("timestamp", "?"), m.get("git", "?"), m.get("host", "?")))
        if m.get("dolphin"):
            print("  %s" % m["dolphin"])
    print()
    hdr = "  %-13s %-20s %9s %11s %10s %7s %7s %6s" % (
        "scene", "renderer", "eff.fps", "% realtime", "slowdown", "exec%", "draw%", "cv%")
    print(hdr)
    print("  " + "-" * (len(hdr) - 2))
    for scene, sc in res["scenes"].items():
        print("  %s  (frames %s)" % (sc["label"], sc["window"] or "auto"))
        for mode, d in sc["modes"].items():
            if "error" in d:
                print("  %-13s %-20s  !! %s" % ("", _mode_name(mode), d["error"]))
                continue
            print("  %-13s %-20s %9.2f %10.1f%% %9.2fx %6.1f %6.1f %5s" % (
                "", _mode_name(mode), d["eff_fps"], d["pct_realtime"], d["slowdown"],
                d["exec_pct"], d["draw_pct"],
                "-" if d["fps_cv_pct"] is None else ("%.1f" % d["fps_cv_pct"])))
        print()


def print_comparison(cur, prev_path):
    prev = json.load(open(prev_path))
    pm = prev.get("meta", {})
    print("  ---- vs previous run: %s  (git %s) ----" %
          (pm.get("timestamp", os.path.basename(os.path.dirname(prev_path))), pm.get("git", "?")))
    print()
    any_row = False
    for scene, sc in cur["scenes"].items():
        psc = prev.get("scenes", {}).get(scene)
        if not psc:
            continue
        for mode, d in sc["modes"].items():
            pd = psc.get("modes", {}).get(mode)
            if not pd or "error" in d or "error" in pd:
                continue
            any_row = True
            dfps = d["eff_fps"] - pd["eff_fps"]
            rel = dfps / pd["eff_fps"] if pd["eff_fps"] else 0.0
            dpp = d["pct_realtime"] - pd["pct_realtime"]
            tag = ""
            if rel <= -REGRESSION_REL:
                tag = "  <<< REGRESSION"
            elif rel >= REGRESSION_REL:
                tag = "  >>> improved"
            print("  %-11s %-18s %6.1f%% -> %6.1f%%  (%+.1fpp, %+.1f%%)   %5.2f -> %5.2f fps%s" % (
                scene, _mode_name(mode), pd["pct_realtime"], d["pct_realtime"],
                dpp, rel * 100, pd["eff_fps"], d["eff_fps"], tag))
    if not any_row:
        print("  (no overlapping scene/renderer pairs to compare)")
    print()


# ------------------------------------------------------------------ report ---
def write_report(res, path, prev_path):
    L = []
    m = res.get("meta", {})
    L.append("# DeSmuME Wii renderer benchmark\n")
    L.append("Emulation speed as a fraction of real DS hardware (%.4f Hz). "
             "Below 100%% = slow motion.\n" % TARGET_HZ)
    L.append("| field | value |")
    L.append("|---|---|")
    for k in ("timestamp", "git", "host", "kernel", "dolphin", "duration_default_s"):
        if m.get(k) not in (None, ""):
            L.append("| %s | %s |" % (k, m[k]))
    L.append("")
    for scene, sc in res["scenes"].items():
        L.append("## %s" % sc["label"])
        L.append("_frames %s_\n" % (sc["window"] or "auto"))
        L.append("| renderer | eff. fps | % real-time | slowdown | NDS_exec | Draw | cv |")
        L.append("|---|--:|--:|--:|--:|--:|--:|")
        for mode, d in sc["modes"].items():
            if "error" in d:
                L.append("| %s | !! %s | | | | | |" % (_mode_name(mode), d["error"]))
                continue
            L.append("| %s | %.1f | %.1f%% | %.1fx | %.0f%% | %.0f%% | %s |" % (
                _mode_name(mode), d["eff_fps"], d["pct_realtime"], d["slowdown"],
                d["exec_pct"], d["draw_pct"],
                "-" if d["fps_cv_pct"] is None else ("%.1f%%" % d["fps_cv_pct"])))
        L.append("")
    if prev_path and os.path.exists(prev_path):
        prev = json.load(open(prev_path))
        pm = prev.get("meta", {})
        L.append("## vs previous run (%s, git %s)\n" % (pm.get("timestamp", "?"), pm.get("git", "?")))
        L.append("| scene | renderer | prev %% | now %% | Δpp | Δ%% | note |")
        L.append("|---|---|--:|--:|--:|--:|---|")
        for scene, sc in res["scenes"].items():
            psc = prev.get("scenes", {}).get(scene)
            if not psc:
                continue
            for mode, d in sc["modes"].items():
                pd = psc.get("modes", {}).get(mode)
                if not pd or "error" in d or "error" in pd:
                    continue
                rel = (d["eff_fps"] - pd["eff_fps"]) / pd["eff_fps"] if pd["eff_fps"] else 0.0
                note = "regression" if rel <= -REGRESSION_REL else ("improved" if rel >= REGRESSION_REL else "")
                L.append("| %s | %s | %.1f%% | %.1f%% | %+.1f | %+.1f%% | %s |" % (
                    scene, _mode_name(mode), pd["pct_realtime"], d["pct_realtime"],
                    d["pct_realtime"] - pd["pct_realtime"], rel * 100, note))
        L.append("")
    L.append("---\n*Measured under Dolphin. The metric uses the emulated Broadway timebase, so it is "
             "independent of host speed, but Dolphin's PPC JIT is not cycle-accurate and runs GX on the "
             "host GPU - real hardware is slower, especially for the GX/merge paths. Trust the "
             "renderer-to-renderer deltas over the absolute percentages.*\n")
    open(path, "w").write("\n".join(L))


# -------------------------------------------------------------------- main ---
def main():
    ap = argparse.ArgumentParser(description="Analyse desmumewii renderer benchmarks.")
    ap.add_argument("rundir", nargs="?", help="run directory containing raw/")
    ap.add_argument("--scenes", help="path to scenes.conf (default: alongside this script)")
    ap.add_argument("--baseline", help="explicit previous run dir/results.json to diff against")
    ap.add_argument("--no-compare", action="store_true")
    ap.add_argument("--compare", nargs=2, metavar=("DIR_A", "DIR_B"),
                    help="just diff two finished runs and exit")
    args = ap.parse_args()

    here = os.path.dirname(os.path.abspath(__file__))
    scenes_path = args.scenes or os.path.join(here, "scenes.conf")

    if args.compare:
        a = args.compare[0]
        b = args.compare[1]
        a = a if a.endswith(".json") else os.path.join(a, "results.json")
        res = json.load(open(b if b.endswith(".json") else os.path.join(b, "results.json")))
        print_summary(res)
        print_comparison(res, a)
        return

    if not args.rundir:
        ap.error("need a RUNDIR (or --compare A B)")
    rundir = args.rundir
    if not os.path.isdir(os.path.join(rundir, "raw")):
        ap.error("%s has no raw/ subdirectory" % rundir)

    res = build_results(rundir, scenes_path)
    out_json = os.path.join(rundir, "results.json")
    json.dump(res, open(out_json, "w"), indent=2)

    prev_path = None if args.no_compare else find_previous(rundir, args.baseline)
    write_report(res, os.path.join(rundir, "report.md"), prev_path)

    print_summary(res)
    if prev_path:
        print_comparison(res, prev_path)
    else:
        print("  (no previous run found to compare against)\n")
    print("  wrote %s" % out_json)
    print("  wrote %s" % os.path.join(rundir, "report.md"))


if __name__ == "__main__":
    main()