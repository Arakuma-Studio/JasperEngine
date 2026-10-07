#!/usr/bin/env python3
"""Raw benchmark CSVs -> summary CSVs (committed in tools/htmlui-bench/results) and the
markdown tables used by docs/html-ui/svg-benchmark.md (results/tables.md).

    report.py --raw build/htmlui-bench/raw --results tools/htmlui-bench/results

Timing summaries are over files: each file contributes its own median over repetitions
(svgbench), and the tables give the median, quartiles and p95 of those per-file medians.
Ratios are computed per file (ThorVG time / LunaSVG time, > 1 means LunaSVG is faster) and
summarised by their median and geometric mean.
"""
import argparse
import collections
import csv
import glob
import math
import os
import re

import numpy as np

LIB_ORDER = ["luna", "thorvg", "thorvg-noavx", "thorvg-mt4", "thorvg-cached", "nanosvg", "thorvg-patched", "resvg"]
SET_ORDER = ["icons", "multicolor", "illustrations", "huge", "conformance"]
DECISIVE = ["structure", "shapes", "painting", "paint-servers", "masking"]


def lib_key(l):
    return LIB_ORDER.index(l) if l in LIB_ORDER else 99


def read(pattern):
    rows = []
    for path in sorted(glob.glob(pattern)):
        with open(path) as f:
            rows += list(csv.DictReader(f))
    return rows


def q(v, p):
    return float(np.percentile(v, p)) if len(v) else float("nan")


def fmt(x, digits=1):
    if x is None or (isinstance(x, float) and math.isnan(x)):
        return "-"
    if abs(x) >= 1000:
        return f"{x:,.0f}"
    if abs(x) >= 100:
        return f"{x:.0f}"
    return f"{x:.{digits}f}"


def write_csv(path, header, rows):
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(header)
        w.writerows(rows)


def md_table(header, rows):
    out = ["| " + " | ".join(header) + " |", "|" + "|".join("---" for _ in header) + "|"]
    out += ["| " + " | ".join(str(c) for c in r) + " |" for r in rows]
    return "\n".join(out) + "\n"


def timing(raw):
    """platform -> rows of the time CSVs (native: time/noavx/threads/cached, wasm: wasm-time,
    native-patched: LunaSVG and the filter-patched ThorVG interleaved in one process)."""
    data = {"native": [], "wasm": [], "native-patched": read(os.path.join(raw, "patched-*.csv"))}
    for prefix in ["time", "noavx", "threads", "cached"]:
        data["native"] += read(os.path.join(raw, f"{prefix}-*.csv"))
    data["wasm"] = read(os.path.join(raw, "wasm-time-*.csv"))
    return data


def per_file(rows):
    """(set, lib, metric, size) -> {file: (median, min)} for ok rows; plus failures."""
    d = collections.defaultdict(dict)
    fails = collections.Counter()
    for r in rows:
        key = (r["set"], r["lib"], r["metric"], int(r["size"]))
        if r["status"] != "ok":
            fails[key] += 1
            continue
        d[key][r["file"]] = (float(r["median"]), float(r["min"]), float(r["p95"]))
    return d, fails


def summarize_timing(platform, rows, results, md):
    d, fails = per_file(rows)
    out = []
    for (s, lib, metric, size), files in d.items():
        med = [v[0] for v in files.values()]
        mins = [v[1] for v in files.values()]
        p95s = [v[2] for v in files.values()]
        out.append([platform, s, lib, metric, size, len(files), fails[(s, lib, metric, size)], fmt(q(med, 50), 3),
                    fmt(q(med, 25), 3), fmt(q(med, 75), 3), fmt(q(med, 95), 3), fmt(float(np.mean(med)), 3),
                    fmt(q(mins, 50), 3), fmt(float(np.median(np.array(p95s) / np.maximum(med, 1e-9))), 3)])
    out.sort(key=lambda r: (SET_ORDER.index(r[1]) if r[1] in SET_ORDER else 9, r[3], r[4], lib_key(r[2])))
    write_csv(os.path.join(results, f"timing_{platform}.csv"),
              ["platform", "set", "lib", "metric", "size", "files", "failed", "median_us", "p25_us", "p75_us", "p95_us",
               "mean_us", "median_min_us", "median_rep_p95_over_median"], out)

    # Markdown: one table per set and metric (raster per size; parse; cold)
    for s in SET_ORDER:
        libs = sorted({k[1] for k in d if k[0] == s}, key=lib_key)
        if not libs:
            continue
        sizes = sorted({k[3] for k in d if k[0] == s and k[2] == "raster"})
        md.append(f"\n**{platform}, {s}: rasterize (clear + full draw), median of per-file medians in µs "
                  f"[p25-p75] (files)**\n\n")
        rows_md = []
        for lib in libs:
            row = [lib]
            for size in sizes:
                f = d.get((s, lib, "raster", size))
                if not f:
                    row.append("-")
                    continue
                med = [v[0] for v in f.values()]
                row.append(f"{fmt(q(med, 50))} [{fmt(q(med, 25))}-{fmt(q(med, 75))}] ({len(med)})")
            rows_md.append(row)
        md.append(md_table(["library"] + [f"{z} px" for z in sizes], rows_md))
        cold_sizes = sorted({k[3] for k in d if k[0] == s and k[2] == "cold"})
        md.append(f"\n**{platform}, {s}: parse, readback and load + first render (cold), median µs [p25-p75]**\n\n")
        rows_md = []
        for lib in libs:
            row = [lib]
            for metric, size in [("parse", 0)] + [("cold", z) for z in cold_sizes] + [("readback", 48)]:
                f = d.get((s, lib, metric, size))
                if not f:
                    row.append("-")
                    continue
                med = [v[0] for v in f.values()]
                row.append(f"{fmt(q(med, 50))} [{fmt(q(med, 25))}-{fmt(q(med, 75))}]")
            rows_md.append(row)
        md.append(md_table(["library", "parse"] + [f"cold {z} px" for z in cold_sizes] + ["readback 48 px"], rows_md))
    return d


def ratios(platform, d, results, md, base="luna", others=("thorvg", "thorvg-noavx", "nanosvg")):
    """Per-file time ratios other/base."""
    out = []
    for s in SET_ORDER:
        for other in others:
            keys = sorted({(k[2], k[3]) for k in d if k[0] == s and k[1] == other})
            for metric, size in keys:
                a, b = d.get((s, base, metric, size), {}), d.get((s, other, metric, size), {})
                common = sorted(set(a) & set(b))
                if not common:
                    continue
                r = np.array([b[f][0] / a[f][0] for f in common])
                rmin = np.array([b[f][1] / a[f][1] for f in common])
                med_a, med_b = q([a[f][0] for f in common], 50), q([b[f][0] for f in common], 50)
                out.append([platform, s, other, base, metric, size, len(common), f"{np.median(r):.3f}",
                            f"{math.exp(np.mean(np.log(r))):.3f}", f"{q(r, 25):.3f}", f"{q(r, 75):.3f}",
                            f"{med_b / med_a:.3f}", f"{np.median(rmin):.3f}", f"{np.mean(r > 1):.3f}"])
            # parse included: per file parse + raster
            for size in sorted({k[3] for k in d if k[0] == s and k[1] == other and k[2] == "raster"}):
                pa, pb = d.get((s, base, "parse", 0), {}), d.get((s, other, "parse", 0), {})
                ra, rb = d.get((s, base, "raster", size), {}), d.get((s, other, "raster", size), {})
                common = sorted(set(pa) & set(pb) & set(ra) & set(rb))
                if not common:
                    continue
                r = np.array([(pb[f][0] + rb[f][0]) / (pa[f][0] + ra[f][0]) for f in common])
                rmin = np.array([(pb[f][1] + rb[f][1]) / (pa[f][1] + ra[f][1]) for f in common])
                med_a = q([pa[f][0] + ra[f][0] for f in common], 50)
                med_b = q([pb[f][0] + rb[f][0] for f in common], 50)
                out.append([platform, s, other, base, "parse+raster", size, len(common), f"{np.median(r):.3f}",
                            f"{math.exp(np.mean(np.log(r))):.3f}", f"{q(r, 25):.3f}", f"{q(r, 75):.3f}",
                            f"{med_b / med_a:.3f}", f"{np.median(rmin):.3f}", f"{np.mean(r > 1):.3f}"])
    write_csv(os.path.join(results, f"ratios_{platform}.csv"),
              ["platform", "set", "lib", "base", "metric", "size", "files", "median_ratio", "geomean_ratio", "p25_ratio",
               "p75_ratio", "ratio_of_medians", "median_ratio_of_mins", "share_base_faster"], out)
    md.append(f"\n**{platform}: time ratio library / LunaSVG per file (> 1: LunaSVG faster); median ratio "
              f"[p25-p75], geometric mean, share of files where LunaSVG is faster**\n\n")
    rows_md = []
    for r in out:
        if r[4] in ("raster", "parse", "cold", "parse+raster") and r[1] in ("icons", "multicolor", "illustrations", "huge"):
            rows_md.append([r[1], r[2], r[4], r[5] or "-", f"{r[7]} [{r[9]}-{r[10]}]", r[8], f"{float(r[13]) * 100:.0f} %"])
    md.append(md_table(["set", "library", "metric", "size", "ratio", "geomean", "LunaSVG faster"], rows_md))
    return out


def mutation(raw, results, md):
    rows = read(os.path.join(raw, "mutate-*.csv"))
    agg = collections.defaultdict(list)
    status = collections.defaultdict(collections.Counter)
    for r in rows:
        status[(r["set"], r["lib"])][r["status"]] += 1
        if r["status"] in ("in_place", "reparse"):
            agg[(r["set"], r["lib"])].append(float(r["median"]))
    out = []
    for (s, lib), v in sorted(agg.items(), key=lambda kv: (SET_ORDER.index(kv[0][0]), lib_key(kv[0][1]))):
        st = status[(s, lib)]
        mode = "in place" if st["in_place"] else "re-parse"
        out.append([s, lib, mode, len(v), sum(st.values()) - len(v), fmt(q(v, 50)), fmt(q(v, 25)), fmt(q(v, 75))])
    write_csv(os.path.join(results, "mutation.csv"),
              ["set", "lib", "mode", "files", "not_done", "median_us", "p25_us", "p75_us"], out)
    md.append("\n**Mutation: set the fill of one element by id and re-render at 48 px, median µs per file**\n\n")
    md.append(md_table(["set", "library", "how", "files", "not found/failed", "median µs", "p25", "p75"], out))


def memory(raw, results, md):
    rows = read(os.path.join(raw, "memory-*.csv"))
    agg = collections.defaultdict(list)
    rss = {}
    for r in rows:
        key = (r["set"], r["lib"])
        if r["metric"] in ("heap_parse_bytes", "heap_render_bytes") and r["status"] == "ok":
            agg[key + (r["metric"],)].append(float(r["median"]))
        elif r["metric"] in ("rss_start_kb", "rss_peak_kb"):
            rss[key + (r["metric"],)] = float(r["median"])
    out = []
    for s in SET_ORDER:
        for lib in sorted({k[1] for k in agg if k[0] == s}, key=lib_key):
            p = agg.get((s, lib, "heap_parse_bytes"), [])
            rr = agg.get((s, lib, "heap_render_bytes"), [])
            out.append([s, lib, len(p), fmt(q(p, 50) / 1024, 1), fmt(max(p) / 1024 if p else float("nan"), 0),
                        fmt(q(rr, 50) / 1024, 1), fmt(max(rr) / 1024 if rr else float("nan"), 0),
                        fmt(rss.get((s, lib, "rss_peak_kb"), float("nan")) / 1024, 1),
                        fmt((rss.get((s, lib, "rss_peak_kb"), float("nan")) - rss.get((s, lib, "rss_start_kb"), float("nan"))) / 1024, 1)])
    write_csv(os.path.join(results, "memory.csv"),
              ["set", "lib", "files", "heap_after_parse_median_kib", "heap_after_parse_max_kib",
               "heap_after_render_median_kib", "heap_after_render_max_kib", "peak_rss_mib", "peak_rss_growth_mib"], out)
    md.append("\n**Memory: retained heap per document after parse and after the first 512 px render (KiB), "
              "peak RSS of the process (MiB)**\n\n")
    md.append(md_table(["set", "library", "files", "after parse median", "after parse max", "after render median",
                        "after render max", "peak RSS", "RSS growth"], out))


def binary_size(raw, results, md):
    out = []
    for platform in ["native", "wasm"]:
        path = os.path.join(raw, f"binary-size-{platform}.csv")
        if not os.path.exists(path):
            continue
        rows = {r["program"]: int(r["bytes"]) for r in csv.DictReader(open(path))}
        base = rows["size_baseline"]
        names = {"size_lunasvg": "LunaSVG 3.5 (+plutovg, stb_image, stb_truetype)", "size_nanosvg": "nanosvg",
                 "size_thorvg_svg_probe": "ThorVG: SVG", "size_thorvg_svgfull_probe": "ThorVG: SVG + PNG + JPG + TTF",
                 "size_thorvg_svglottie_probe": "ThorVG: SVG + PNG + JPG + TTF + Lottie"}
        for p, label in names.items():
            out.append([platform, label, rows[p], rows[p] - base])
    write_csv(os.path.join(results, "binary_size.csv"), ["platform", "build", "bytes", "delta_bytes"], out)
    md.append("\n**Binary size: stripped -Os probe (load + render + write), delta against the same program "
              "without an SVG library (KiB)**\n\n")
    labels = list(dict.fromkeys(r[1] for r in out))
    rows_md = []
    for label in labels:
        row = [label]
        for platform in ["native", "wasm"]:
            m = [r for r in out if r[0] == platform and r[1] == label]
            row.append(fmt(m[0][3] / 1024, 1) if m else "-")
        rows_md.append(row)
    md.append(md_table(["library", "native x86-64 delta", "wasm delta"], rows_md))


def correctness(raw, results, md):
    rows = read(os.path.join(raw, "compare.csv"))
    agg = collections.defaultdict(list)
    for r in rows:
        agg[(r["set"], r["size"], r["ref"], r["lib"], r["category"])].append(r)
    out = []
    for key in sorted(agg, key=lambda k: (SET_ORDER.index(k[0]), int(k[1]), k[2], k[4], lib_key(k[3]))):
        v = agg[key]
        n = len(v)
        passed = sum(int(x["pass"]) for x in v)
        wrong = sum(float(x["frac64"]) >= 0.02 for x in v)
        mad = [float(x["mad"]) for x in v]
        aa = [float(x["aa_mad"]) for x in v if int(x["aa_n"]) > 0]
        out.append(list(key) + [n, passed, f"{passed / n * 100:.1f}", wrong, f"{np.mean(mad):.2f}",
                                f"{np.median(mad):.2f}", f"{np.mean(aa):.2f}" if aa else "-"])
    write_csv(os.path.join(results, "correctness.csv"),
              ["set", "size", "ref", "lib", "category", "files", "pass", "pass_pct", "visibly_wrong", "mad_mean",
               "mad_median", "aa_mad_mean"], out)

    md.append("\n**Correctness against resvg (and Chrome at 48 px): pass = < 1 % of pixels with a channel "
              "difference > 16; visibly wrong = >= 2 % of pixels off by > 64; MAD = mean absolute difference "
              "(0-255); AA = MAD over the reference's partially covered pixels**\n\n")
    rows_md = [[r[0], r[1], r[2], r[3], r[5], f"{r[7]} %", r[8], r[9], r[11]] for r in out if r[0] != "conformance"]
    md.append(md_table(["set", "px", "reference", "library", "files", "pass", "visibly wrong", "MAD", "AA MAD"], rows_md))

    # resvg suite per category, plus the decisive categories combined
    conf = [r for r in rows if r["set"] == "conformance"]
    libs = sorted({r["lib"] for r in conf}, key=lib_key)
    cats = [c for c in ["structure", "shapes", "painting", "paint-servers", "masking", "text", "filters"]]
    table, out2 = [], []
    for lib in libs:
        row = [lib]
        for c in cats:
            v = [r for r in conf if r["lib"] == lib and r["category"] == c]
            p = sum(int(x["pass"]) for x in v)
            row.append(f"{p / len(v) * 100:.1f} % ({p}/{len(v)})" if v else "-")
            out2.append([lib, c, len(v), p])
        v = [r for r in conf if r["lib"] == lib and r["category"] in DECISIVE]
        p = sum(int(x["pass"]) for x in v)
        row.append(f"**{p / len(v) * 100:.1f} %** ({p}/{len(v)})" if v else "-")
        out2.append([lib, "decisive", len(v), p])
        v = [r for r in conf if r["lib"] == lib]
        p = sum(int(x["pass"]) for x in v)
        row.append(f"{p / len(v) * 100:.1f} % ({p}/{len(v)})")
        out2.append([lib, "all", len(v), p])
        table.append(row)
    write_csv(os.path.join(results, "conformance.csv"), ["lib", "category", "tests", "pass"], out2)
    md.append("\n**resvg test suite pass rate per category (300 px wide, vs the suite's reference PNGs)**\n\n")
    md.append(md_table(["library"] + cats + ["decisive 5 combined", "all"], table))

    # finer: pass rate per sub-directory (feature) for the feature matrix
    sub = collections.defaultdict(lambda: [0, 0])
    for r in conf:
        parts = r["file"].split("/")
        k = (r["lib"], "/".join(parts[:2]))
        sub[k][0] += 1
        sub[k][1] += int(r["pass"])
    write_csv(os.path.join(results, "conformance_features.csv"), ["lib", "feature", "tests", "pass"],
              [[k[0], k[1], v[0], v[1]] for k, v in sorted(sub.items())])
    return rows


def decision(d_native, ratio_rows, comp_rows, md):
    lines = ["\n**Decision inputs (rule of phase-0-foundation.md 3.9.4)**\n"]
    for lib in ["thorvg", "thorvg-noavx"]:
        for metric in ["raster", "parse+raster", "cold"]:
            for size in [24, 48]:
                m = [r for r in ratio_rows if r[1] == "icons" and r[2] == lib and r[4] == metric and r[5] == size]
                if m:
                    r = m[0]
                    lines.append(f"- Lucide {metric} {size} px, {lib} / LunaSVG: median per-file ratio {r[7]}, "
                                 f"ratio of medians {r[11]}, ratio of per-file minimums {r[12]} "
                                 f"(LunaSVG >= 1.5x faster would need >= 1.5)")
    conf = [r for r in comp_rows if r["set"] == "conformance" and r["category"] in DECISIVE]
    rates = {}
    for lib in ["luna", "thorvg", "thorvg-patched", "nanosvg"]:
        v = [r for r in conf if r["lib"] == lib]
        if v:
            rates[lib] = sum(int(x["pass"]) for x in v) / len(v) * 100
    lines.append("- resvg suite, structure+shapes+painting+paint-servers+masking: " +
                 ", ".join(f"{k} {v:.1f} %" for k, v in rates.items()) +
                 (f" (LunaSVG - ThorVG = {rates['luna'] - rates['thorvg']:+.1f} points; >= +10 needed)" if "luna" in rates and "thorvg" in rates else ""))
    for s, size in [("icons", "48"), ("icons", "24"), ("multicolor", "48"), ("multicolor", "128")]:
        v = collections.defaultdict(dict)
        for r in comp_rows:
            if r["set"] == s and r["size"] == size and r["ref"] == "resvg":
                v[r["lib"]][r["file"]] = float(r["frac64"]) >= 0.02
        if "luna" in v and "thorvg" in v:
            tl = sorted(f for f in v["thorvg"] if v["thorvg"][f] and not v["luna"].get(f, True))
            lt = sorted(f for f in v["luna"] if v["luna"][f] and not v["thorvg"].get(f, True))
            extra = ""
            if "thorvg-patched" in v:
                pl = sorted(f for f in v["thorvg-patched"] if v["thorvg-patched"][f] and not v["luna"].get(f, True))
                extra = f"; with the filter patch: {len(pl)}"
            lines.append(f"- {s} {size} px: visibly wrong in ThorVG but not in LunaSVG: {len(tl)}; the reverse: "
                         f"{len(lt)}{extra}")
    md.append("\n".join(lines) + "\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--raw", required=True)
    ap.add_argument("--results", required=True)
    args = ap.parse_args()
    os.makedirs(args.results, exist_ok=True)
    md = ["<!-- generated by tools/htmlui-bench/scripts/report.py; do not edit -->\n"]
    data = timing(args.raw)
    d_native = summarize_timing("native", data["native"], args.results, md) if data["native"] else {}
    ratio_rows = ratios("native", d_native, args.results, md) if d_native else []
    if data["wasm"]:
        d_wasm = summarize_timing("wasm", data["wasm"], args.results, md)
        ratios("wasm", d_wasm, args.results, md, others=("thorvg", "nanosvg"))
    if data["native-patched"]:
        d_p = summarize_timing("native-patched", data["native-patched"], args.results, md)
        ratios("native-patched", d_p, args.results, md, others=("thorvg-patched",))
    mutation(args.raw, args.results, md)
    memory(args.raw, args.results, md)
    binary_size(args.raw, args.results, md)
    comp_rows = correctness(args.raw, args.results, md)
    decision(d_native, ratio_rows, comp_rows, md)
    with open(os.path.join(args.results, "tables.md"), "w") as f:
        f.write("".join(md))
    print(f"wrote {args.results}/tables.md and summary CSVs")


if __name__ == "__main__":
    main()
