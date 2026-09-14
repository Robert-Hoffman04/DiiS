#!/usr/bin/env python3
"""
Analyse a desmumewii benchmark run (tools/benchmark/run.sh) and diff it
against the previous one.

A run directory has raw/perf_<scene>_<mode>/ (fps + perf-zones breakdown,
captured live via capture.py's profile.log) and raw/wrestler_<id>/
(correctness-probe screenshot, captured the same way). This script reads
both, writes results.json + report.md, and prints a summary plus a diff
against the most recent previous run under the same results/ directory.

Usage:
  analyze.py <RUNDIR> [--scenes PATH] [--wrestlers PATH] [--baseline DIR] [--no-compare]
  analyze.py --compare <DIR_A> <DIR_B>
"""
import argparse
import glob
import hashlib
import json
import os
import re
import statistics
import sys

TARGET_HZ = 59.8261
MODE_LABEL = {"sw": "software rasterizer", "gx": "GX hardware 3D + GXMerge",
              "jitoff": "ARM7 interpreter", "jiton": "ARM7 JIT",
              "jit9off": "ARM9 interpreter", "jit9on": "ARM9 JIT",
              "jitfull": "full JIT (GXMerge)"}
MODE_ORDER = ["sw", "gx", "jitoff", "jiton", "jit9off", "jit9on", "jitfull"]
REGRESSION_REL = 0.03   # >3% relative eff_fps drop vs baseline -> flag

CSV_ROW_RE = re.compile(r"^\d+,\d+")
FRAMETIME_RE = re.compile(
    r"^frametime frame=(\d+) n=(\d+) p50_us=(\d+) p95_us=(\d+) p99_us=(\d+) worst_us=(\d+)")
# §3.3 JIT cache-pressure stat block (jit_cache.cpp profEmitReport) - a
# running-total snapshot per core, emitted roughly every 8192 JIT dispatches.
# The last one per tag in a capture is that run's final cache-health state.
JIT_CACHE_RE = re.compile(
    r"^jit cache=(\S+) lookups=(\d+) hit=(\d+)% coldmiss=(\d+) collmiss=(\d+) "
    r"reg=(\d+) evict=(\d+) evictlife_avg=(\d+) flush=(\d+) "
    r"arena=(\d+)/(\d+)\((\d+)%\) arenapeak=(\d+)\((\d+)%\)")
JIT_CACHE_NOTE_RE = re.compile(r"^jit cache=(\S+) (WARNING|note): (.+)$")


# ---------------------------------------------------------------- parsing ---
def parse_profile_log(path):
    """Returns (zone_names, rows, frametimes, jit_cache, jit_notes).
    rows: list of (frame, wall_us, {zone: us}, {zone: hits}).
    frametimes: list of dicts.
    jit_cache: {tag: {field: value}} - last report seen per core tag.
    jit_notes: [(tag, level, text)] - WARNING/note lines, in order seen."""
    zone_names = None
    rows, frametimes = [], []
    jit_cache = {}
    jit_notes = []
    with open(path, errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if line.startswith("frame,wall_us"):
                cols = line.split(",")[2:]              # drop frame, wall_us
                half = len(cols) // 2
                zone_names = [c[:-3] for c in cols[:half]]   # strip "_us"
                continue
            m = FRAMETIME_RE.match(line)
            if m:
                frametimes.append({
                    "frame": int(m.group(1)), "n": int(m.group(2)),
                    "p50_us": int(m.group(3)), "p95_us": int(m.group(4)),
                    "p99_us": int(m.group(5)), "worst_us": int(m.group(6)),
                })
                continue
            m = JIT_CACHE_RE.match(line)
            if m:
                tag = m.group(1)
                jit_cache[tag] = {
                    "lookups": int(m.group(2)), "hit_pct": int(m.group(3)),
                    "coldmiss": int(m.group(4)), "collmiss": int(m.group(5)),
                    "reg": int(m.group(6)), "evict": int(m.group(7)),
                    "evictlife_avg": int(m.group(8)), "flush": int(m.group(9)),
                    "arena_used": int(m.group(10)), "arena_cap": int(m.group(11)),
                    "arena_pct": int(m.group(12)),
                    "arenapeak": int(m.group(13)), "arenapeak_pct": int(m.group(14)),
                }
                continue
            m = JIT_CACHE_NOTE_RE.match(line)
            if m:
                jit_notes.append((m.group(1), m.group(2), m.group(3)))
                continue
            if not CSV_ROW_RE.match(line):
                continue                                  # foreign profile line (jit probes etc)
            parts = line.split(",")
            if not zone_names or len(parts) != 2 + 2 * len(zone_names):
                continue
            try:
                nums = [int(x) for x in parts]
            except ValueError:
                continue
            frame, wall_us = nums[0], nums[1]
            n = len(zone_names)
            us = dict(zip(zone_names, nums[2:2 + n]))
            hits = dict(zip(zone_names, nums[2 + n:2 + 2 * n]))
            rows.append((frame, wall_us, us, hits))
    return zone_names or [], rows, frametimes, jit_cache, jit_notes


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


def summarize_perf(path, window):
    zone_names, rows, frametimes, jit_cache, jit_notes = parse_profile_log(path)
    if len(rows) < 3:
        return {"error": "only %d usable rows in %s" % (len(rows), os.path.basename(path))}

    block = rows[0][0]
    lo, hi = resolve_window(rows, window)
    kept = [r for r in rows if lo <= r[0] <= hi]
    if len(kept) < 2:
        kept = rows[1:]
        lo, hi = kept[0][0], kept[-1][0]

    fps = [block / (r[1] / 1e6) for r in kept if r[1]]
    if not fps:
        return {"error": "zero wall_us in every kept row"}
    mean_fps = statistics.mean(fps)

    zones_pct, zones_ms = {}, {}
    for z in zone_names:
        zones_pct[z] = round(statistics.mean(
            (r[2].get(z, 0) / r[1]) * 100 for r in kept if r[1]), 1)
        # mean time this zone costs per single emulated frame (not per block)
        zones_ms[z] = round(statistics.mean(r[2].get(z, 0) for r in kept) / block / 1000, 3)

    ft_kept = [t for t in frametimes if lo <= t["frame"] <= hi]
    frametime = None
    if ft_kept:
        frametime = {k: round(statistics.mean(t[k] for t in ft_kept))
                      for k in ("p50_us", "p95_us", "p99_us", "worst_us")}

    return {
        "eff_fps": round(mean_fps, 3),
        "pct_realtime": round(100 * mean_fps / TARGET_HZ, 2),
        "slowdown": round(TARGET_HZ / mean_fps, 3),
        "zones_pct": zones_pct,
        "zones_ms": zones_ms,
        "frametime": frametime,
        "jit_cache": jit_cache,
        "jit_notes": [{"tag": t, "level": lv, "text": txt} for t, lv, txt in jit_notes],
        "fps_min": round(min(fps), 2),
        "fps_max": round(max(fps), 2),
        "fps_cv_pct": round(100 * statistics.pstdev(fps) / mean_fps, 1) if mean_fps else None,
        "blocks_kept": len(kept),
        "blocks_total": len(rows),
        "frame_lo": lo,
        "frame_hi": hi,
        "block_frames": block,
    }


def summarize_wrestler(outdir):
    summary_path = os.path.join(outdir, "summary.json")
    summary = json.load(open(summary_path)) if os.path.exists(summary_path) else {}
    img = None
    for ext in (".png", ".256x192.bin", ".bin"):
        p = os.path.join(outdir, "frame" + ext)
        if os.path.exists(p):
            img = p
            break
    entry = {
        "asserts_pass": summary.get("asserts_pass", 0),
        "asserts_fail": summary.get("asserts_fail", 0),
        "crashes": summary.get("crashes", 0),
        "timed_out": summary.get("timed_out", False),
        "got_frame": summary.get("got_frame", False),
    }
    if img:
        entry["image"] = os.path.relpath(img, start=os.path.dirname(os.path.dirname(outdir)))
        entry["image_sha256"] = hashlib.sha256(open(img, "rb").read()).hexdigest()
    else:
        entry["error"] = "no screenshot captured"
    return entry


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


def load_wrestlers(path):
    ids = {}
    if not path or not os.path.exists(path):
        return ids
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            fld = [x.strip() for x in line.split("|")]
            wid = fld[0]
            ids[wid] = {"label": fld[5] if len(fld) > 5 and fld[5] else wid}
    return ids


# ------------------------------------------------------------- assembling ---
def build_results(rundir, scenes_path, wrestlers_path):
    scenes_cfg = load_scenes(scenes_path)
    wrestlers_cfg = load_wrestlers(wrestlers_path)
    meta = {}
    mp = os.path.join(rundir, "meta.json")
    if os.path.exists(mp):
        meta = json.load(open(mp))

    raw = os.path.join(rundir, "raw")
    perf_caps = {}     # scene -> mode -> dir
    wrestler_caps = {}  # id -> dir
    if os.path.isdir(raw):
        for name in sorted(os.listdir(raw)):
            full = os.path.join(raw, name)
            if not os.path.isdir(full):
                continue
            if name.startswith("perf_"):
                rest = name[len("perf_"):]
                if "_" not in rest:
                    continue
                scene, mode = rest.rsplit("_", 1)
                perf_caps.setdefault(scene, {})[mode] = full
            elif name.startswith("wrestler_"):
                wrestler_caps[name[len("wrestler_"):]] = full

    order = [s for s in scenes_cfg if s in perf_caps] + \
            [s for s in sorted(perf_caps) if s not in scenes_cfg]

    out = {"meta": meta, "target_hz": TARGET_HZ, "scenes": {}, "wrestlers": {}}
    for scene in order:
        cfg = scenes_cfg.get(scene, {"label": scene, "window": ""})
        entry = {"label": cfg.get("label", scene), "window": cfg.get("window", ""), "modes": {}}
        ordered = [m for m in MODE_ORDER if m in perf_caps[scene]] + \
                  [m for m in perf_caps[scene] if m not in MODE_ORDER]
        for mode in ordered:
            profile = os.path.join(perf_caps[scene][mode], "profile.log")
            entry["modes"][mode] = (summarize_perf(profile, cfg.get("window", ""))
                                     if os.path.exists(profile)
                                     else {"error": "no profile.log captured"})
        out["scenes"][scene] = entry

    wids = [w for w in wrestlers_cfg if w in wrestler_caps] + \
           [w for w in sorted(wrestler_caps) if w not in wrestlers_cfg]
    for wid in wids:
        label = wrestlers_cfg.get(wid, {}).get("label", wid)
        out["wrestlers"][wid] = dict(label=label, **summarize_wrestler(wrestler_caps[wid]))
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


def _jit_cache_line(jit_cache):
    """One compact 'arm7 hit=99% arena_peak=1% flush=2 | arm9 ...' line, or ''."""
    if not jit_cache:
        return ""
    parts = []
    for tag in sorted(jit_cache):
        c = jit_cache[tag]
        parts.append("%s hit=%d%% arena_peak=%d%% evict=%d flush=%d" % (
            tag, c["hit_pct"], c["arenapeak_pct"], c["evict"], c["flush"]))
    return " | ".join(parts)


def print_summary(res):
    print()
    print("  DeSmuME Wii benchmark  -  emulation speed vs real DS (%.4f Hz)" % TARGET_HZ)
    m = res.get("meta", {})
    if m:
        print("  %s  |  git %s  |  %s" % (m.get("timestamp", "?"), m.get("git", "?"), m.get("host", "?")))
        if m.get("dolphin"):
            print("  %s" % m["dolphin"])
    print()
    hdr = "  %-13s %-20s %9s %11s %10s %6s %6s" % (
        "scene", "renderer", "eff.fps", "% realtime", "slowdown", "p95ms", "cv%")
    print(hdr)
    print("  " + "-" * (len(hdr) - 2))
    for scene, sc in res["scenes"].items():
        print("  %s  (frames %s)" % (sc["label"], sc["window"] or "auto"))
        for mode, d in sc["modes"].items():
            if "error" in d:
                print("  %-13s %-20s  !! %s" % ("", _mode_name(mode), d["error"]))
                continue
            p95 = "-" if not d["frametime"] else "%.1f" % (d["frametime"]["p95_us"] / 1000)
            print("  %-13s %-20s %9.2f %10.1f%% %9.2fx %6s %5s" % (
                "", _mode_name(mode), d["eff_fps"], d["pct_realtime"], d["slowdown"], p95,
                "-" if d["fps_cv_pct"] is None else ("%.1f" % d["fps_cv_pct"])))
            jcl = _jit_cache_line(d.get("jit_cache"))
            if jcl:
                print("  %-13s   %s" % ("", jcl))
            for note in d.get("jit_notes", []):
                if note["level"] == "WARNING":
                    print("  %-13s   !! jit %s: %s" % ("", note["tag"], note["text"]))
        print()

    if res.get("wrestlers"):
        print("  correctness probes")
        for wid, w in res["wrestlers"].items():
            if "error" in w:
                print("  %-13s !! %s" % (w["label"], w["error"]))
                continue
            tag = "asserts %d/%d" % (w["asserts_pass"], w["asserts_pass"] + w["asserts_fail"]) \
                if (w["asserts_pass"] or w["asserts_fail"]) else "screenshot captured"
            crash = "  !! CRASH" if w["crashes"] else ""
            print("  %-13s %s%s" % (w["label"], tag, crash))
        print()


JIT_CACHE_HIT_REGRESSION_PP = 5   # percentage-point drop in hit rate -> flag


def _jit_cache_delta_note(cur_jc, prev_jc):
    if not cur_jc or not prev_jc:
        return ""
    bits = []
    for tag in sorted(set(cur_jc) & set(prev_jc)):
        d_hit = cur_jc[tag]["hit_pct"] - prev_jc[tag]["hit_pct"]
        if d_hit <= -JIT_CACHE_HIT_REGRESSION_PP:
            bits.append("%s hit %d%%->%d%%" % (tag, prev_jc[tag]["hit_pct"], cur_jc[tag]["hit_pct"]))
    return ("  !! jit cache regression: " + ", ".join(bits)) if bits else ""


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
            rel = (d["eff_fps"] - pd["eff_fps"]) / pd["eff_fps"] if pd["eff_fps"] else 0.0
            tag = ""
            if rel <= -REGRESSION_REL:
                tag = "  <<< REGRESSION"
            elif rel >= REGRESSION_REL:
                tag = "  >>> improved"
            print("  %-11s %-18s %6.1f%% -> %6.1f%%  (%+.1f%%)   %5.2f -> %5.2f fps%s%s" % (
                scene, _mode_name(mode), pd["pct_realtime"], d["pct_realtime"],
                rel * 100, pd["eff_fps"], d["eff_fps"], tag,
                _jit_cache_delta_note(d.get("jit_cache"), pd.get("jit_cache"))))
    if not any_row:
        print("  (no overlapping scene/renderer pairs to compare)")
    print()

    prev_w = prev.get("wrestlers", {})
    if cur.get("wrestlers") and prev_w:
        print("  ---- correctness probes vs previous run ----")
        for wid, w in cur["wrestlers"].items():
            pw = prev_w.get(wid)
            if not pw or "error" in w or "error" in pw:
                continue
            if w["image_sha256"] == pw["image_sha256"]:
                print("  %-13s unchanged" % w["label"])
            else:
                print("  %-13s CHANGED  (screenshot differs from previous run)" % w["label"])
        print()


# ------------------------------------------------------------------ report ---
def write_report(res, path, prev_path):
    L = []
    m = res.get("meta", {})
    L.append("# DeSmuME Wii benchmark\n")
    L.append("Emulation speed as a fraction of real DS hardware (%.4f Hz). "
             "Below 100%% = slow motion.\n" % TARGET_HZ)
    L.append("| field | value |")
    L.append("|---|---|")
    for k in ("timestamp", "git", "host", "kernel", "dolphin", "timeout_default_s"):
        if m.get(k) not in (None, ""):
            L.append("| %s | %s |" % (k, m[k]))
    L.append("")
    for scene, sc in res["scenes"].items():
        L.append("## %s" % sc["label"])
        L.append("_frames %s_\n" % (sc["window"] or "auto"))
        L.append("| renderer | eff. fps | % real-time | slowdown | p95 frame | worst frame | cv |")
        L.append("|---|--:|--:|--:|--:|--:|--:|")
        for mode, d in sc["modes"].items():
            if "error" in d:
                L.append("| %s | !! %s | | | | | |" % (_mode_name(mode), d["error"]))
                continue
            ft = d["frametime"] or {}
            p95 = "%.1f ms" % (ft["p95_us"] / 1000) if ft else "-"
            worst = "%.1f ms" % (ft["worst_us"] / 1000) if ft else "-"
            L.append("| %s | %.1f | %.1f%% | %.1fx | %s | %s | %s |" % (
                _mode_name(mode), d["eff_fps"], d["pct_realtime"], d["slowdown"], p95, worst,
                "-" if d["fps_cv_pct"] is None else ("%.1f%%" % d["fps_cv_pct"])))
        L.append("")
        zone_modes = [(m, d) for m, d in sc["modes"].items() if "error" not in d and d.get("zones_pct")]
        if zone_modes:
            all_zones = set()
            for _, d in zone_modes:
                all_zones.update(d["zones_pct"])
            # order rows by the max share any mode gave this zone, busiest first
            order_zones = sorted(all_zones,
                                  key=lambda z: -max(d["zones_pct"].get(z, 0) for _, d in zone_modes))
            L.append("| zone | " + " | ".join(_mode_name(m) for m, _ in zone_modes) + " |")
            L.append("|---|" + "--:|" * len(zone_modes))
            for z in order_zones:
                pct_vals = [d["zones_pct"].get(z, 0) for _, d in zone_modes]
                ms_vals = [d.get("zones_ms", {}).get(z, 0) for _, d in zone_modes]
                if all(v == 0 for v in pct_vals):
                    continue
                cells = ["%.1f%% (%.2fms)" % (p, ms) for p, ms in zip(pct_vals, ms_vals)]
                L.append("| %s | " % z + " | ".join(cells) + " |")
            L.append("")
            L.append("_% is share of frame time in that zone; ms is average wall time "
                      "per emulated frame._\n")
        jit_modes = [(m, d) for m, d in sc["modes"].items() if "error" not in d and d.get("jit_cache")]
        if jit_modes:
            L.append("| JIT cache | core | hit % | arena peak | evictions | flushes | avg lifetime |")
            L.append("|---|---|--:|--:|--:|--:|--:|")
            for mode, d in jit_modes:
                for tag in sorted(d["jit_cache"]):
                    c = d["jit_cache"][tag]
                    L.append("| %s | %s | %d%% | %d%% | %d | %d | %d |" % (
                        _mode_name(mode), tag, c["hit_pct"], c["arenapeak_pct"],
                        c["evict"], c["flush"], c["evictlife_avg"]))
            L.append("")
            for mode, d in jit_modes:
                for note in d.get("jit_notes", []):
                    if note["level"] == "WARNING":
                        L.append("- **%s %s WARNING:** %s" % (_mode_name(mode), note["tag"], note["text"]))
            L.append("")
    if res.get("wrestlers"):
        L.append("## correctness probes\n")
        L.append("| probe | result |")
        L.append("|---|---|")
        for wid, w in res["wrestlers"].items():
            if "error" in w:
                L.append("| %s | !! %s |" % (w["label"], w["error"]))
                continue
            bits = []
            if w["asserts_pass"] or w["asserts_fail"]:
                bits.append("asserts %d/%d" % (w["asserts_pass"], w["asserts_pass"] + w["asserts_fail"]))
            bits.append("screenshot captured" if w["got_frame"] else "no screenshot")
            if w["crashes"]:
                bits.append("**CRASH**")
            L.append("| %s | %s |" % (w["label"], ", ".join(bits)))
        L.append("")
        for wid, w in res["wrestlers"].items():
            if w.get("image") and w["image"].endswith(".png"):
                L.append("**%s**\n" % w["label"])
                L.append("![%s](%s)\n" % (w["label"], w["image"]))
    if prev_path and os.path.exists(prev_path):
        prev = json.load(open(prev_path))
        pm = prev.get("meta", {})
        L.append("## vs previous run (%s, git %s)\n" % (pm.get("timestamp", "?"), pm.get("git", "?")))
        L.append("| scene | renderer | prev %% | now %% | Δ%% | note |")
        L.append("|---|---|--:|--:|--:|---|")
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
                jcnote = _jit_cache_delta_note(d.get("jit_cache"), pd.get("jit_cache")).strip(" !")
                if jcnote:
                    note = (note + "; " if note else "") + jcnote
                L.append("| %s | %s | %.1f%% | %.1f%% | %+.1f%% | %s |" % (
                    scene, _mode_name(mode), pd["pct_realtime"], d["pct_realtime"], rel * 100, note))
        L.append("")
        prev_w = prev.get("wrestlers", {})
        if res.get("wrestlers") and prev_w:
            L.append("| probe | vs previous |")
            L.append("|---|---|")
            for wid, w in res["wrestlers"].items():
                pw = prev_w.get(wid)
                if not pw or "error" in w or "error" in pw:
                    continue
                note = "unchanged" if w["image_sha256"] == pw["image_sha256"] else "**CHANGED**"
                L.append("| %s | %s |" % (w["label"], note))
            L.append("")
    L.append("---\n*Measured under Dolphin. The metric uses the emulated Broadway timebase, so it is "
             "independent of host speed, but Dolphin's PPC JIT is not cycle-accurate and runs GX on the "
             "host GPU - real hardware is slower, especially for the GX/merge paths. Trust the "
             "renderer-to-renderer deltas over the absolute percentages.*\n")
    open(path, "w").write("\n".join(L))


# -------------------------------------------------------------------- main ---
def main():
    ap = argparse.ArgumentParser(description="Analyse a desmumewii benchmark run.")
    ap.add_argument("rundir", nargs="?", help="run directory containing raw/")
    ap.add_argument("--scenes", help="path to scenes.conf (default: alongside this script)")
    ap.add_argument("--wrestlers", help="path to wrestlers.conf (default: alongside this script)")
    ap.add_argument("--baseline", help="explicit previous run dir/results.json to diff against")
    ap.add_argument("--no-compare", action="store_true")
    ap.add_argument("--compare", nargs=2, metavar=("DIR_A", "DIR_B"),
                     help="just diff two finished runs and exit")
    args = ap.parse_args()

    here = os.path.dirname(os.path.abspath(__file__))
    scenes_path = args.scenes or os.path.join(here, "scenes.conf")
    wrestlers_path = args.wrestlers or os.path.join(here, "wrestlers.conf")

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

    res = build_results(rundir, scenes_path, wrestlers_path)
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
