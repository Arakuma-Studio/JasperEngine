#!/usr/bin/env python3
"""Build-time analysis of docs/specs/S33-build-speed.md, over what tools/perf/build-bench.sh keeps.

  build_stats.py log <ninja_log>... [--top N]      where a build's time goes: own code against vendored
                                                   libraries, per target, the slowest units, the links
  build_stats.py fanout <build-dir> [--top N]      header fan-out (ninja -t deps): the units each header
                                                   rebuilds and the compile seconds that costs
  build_stats.py headers <build-dir> [--top N]     what parsing the most included headers alone costs
                                                   (-fsyntax-only), times their inclusion count
  build_stats.py time-report <build-dir> [--top N] GCC -ftime-report phases of the slowest units
  build_stats.py results <results.jsonl>...        the phases' wall times as a Markdown table

fanout, headers and time-report run Ninja or the compiler, so run them inside the build image
(tools/build.sh exec linux python3 tools/perf/build_stats.py ...). Standard library only.
"""

import argparse
import collections
import json
import os
import re
import shlex
import subprocess
import sys
import tempfile
import time

SOURCE_ROOT = "/src/"


def read_ninja_log(path):
    """The edges of a .ninja_log (v5/v6): the last entry of every output, outputs of one edge merged."""
    last = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("#"):
                continue
            parts = line.rstrip("\n").split("\t")
            if len(parts) < 5:
                continue
            start, end, _, output, cmdhash = parts[:5]
            last[output] = (int(start), int(end), cmdhash)
    edges = {}
    for output, (start, end, cmdhash) in last.items():
        edges.setdefault((start, end, cmdhash), []).append(output)
    return [(start, end, outputs) for (start, end, _), outputs in edges.items()]


OBJECT_RE = re.compile(r"(?:^|/)CMakeFiles/([^/]+)\.dir/(.+?)\.(?:o|obj)$")


def classify(outputs):
    """(kind, target, source) of an edge: kind is compile, link, archive or other."""
    for output in outputs:
        m = OBJECT_RE.search(output)
        if m:
            return "compile", m.group(1), m.group(2)
    name = os.path.basename(outputs[0])
    if "CMakeFiles/" in outputs[0]:
        return "other", name, ""  # a custom target's stamp
    if name.endswith(".a") or name.endswith(".lib"):
        return "archive", name, ""
    if re.search(r"\.so(\.\d+)*$", name) or name.endswith((".dll", ".dylib", ".html", ".js", ".wasm")) or "." not in name:
        return "link", name, ""
    return "other", name, ""


def is_vendored(target, source, output=""):
    # engine/libs/* and libs/* (vendored trees, and ImGui/tinyexpr compiled into the editor target)
    return "libs/" in source or "libs/" in output.split("CMakeFiles/")[0] or source.startswith("_deps/")


def cmd_log(args):
    for path in args.logs:
        edges = read_ninja_log(path)
        if not edges:
            print(f"{path}: empty")
            continue
        wall = (max(e[1] for e in edges) - min(e[0] for e in edges)) / 1000.0
        sums = collections.Counter()
        per_target = collections.Counter()
        units = []
        links = []
        for start, end, outputs in edges:
            seconds = (end - start) / 1000.0
            kind, target, source = classify(outputs)
            if kind == "compile":
                group = "vendored" if is_vendored(target, source, outputs[0]) else "own"
                sums[group] += seconds
                sums[group + "_units"] += 1
                per_target[target] += seconds
                units.append((seconds, target, source))
            elif kind in ("link", "archive"):
                sums[kind] += seconds
                links.append((seconds, kind, outputs[0]))
            else:
                sums["other"] += seconds
        total = sums["own"] + sums["vendored"] + sums["link"] + sums["archive"] + sums["other"]
        print(f"## {path}")
        print(f"wall {wall:.0f} s, {len(edges)} steps, {total:.0f} CPU-s ({total / wall if wall else 0:.1f} in parallel)")
        print(f"  own code   {sums['own']:8.0f} CPU-s in {int(sums['own_units'])} units")
        print(f"  vendored   {sums['vendored']:8.0f} CPU-s in {int(sums['vendored_units'])} units")
        print(f"  links      {sums['link']:8.1f} s, archives {sums['archive']:.1f} s, other {sums['other']:.1f} s")
        print("  targets by compile CPU-s:")
        for target, seconds in per_target.most_common(args.top):
            print(f"    {seconds:8.0f}  {target}")
        print(f"  slowest {args.top} units:")
        for seconds, target, source in sorted(units, reverse=True)[: args.top]:
            print(f"    {seconds:7.1f}  {target}: {source}")
        print("  links and archives:")
        for seconds, kind, output in sorted(links, reverse=True)[: args.top]:
            print(f"    {seconds:7.1f}  {kind} {output}")
        print()


def ninja_deps(build_dir):
    """{object: [header...]} from ninja -t deps, paths relative to the source tree."""
    text = subprocess.run(["ninja", "-C", build_dir, "-t", "deps"], check=True, capture_output=True, text=True).stdout
    deps = {}
    current = None
    for line in text.splitlines():
        if not line.strip():
            current = None
            continue
        if not line.startswith(" "):
            current = line.split(":", 1)[0]
            deps[current] = []
        elif current is not None:
            dep = os.path.normpath(os.path.join(build_dir, line.strip()) if not line.strip().startswith("/") else line.strip())
            deps[current].append(dep[len(SOURCE_ROOT):] if dep.startswith(SOURCE_ROOT) else dep)
    return deps


def own_header(path):
    return (path.startswith("engine/") or path.startswith("editor/")) and "/libs/" not in path \
        and not path.startswith("engine/libs/") and path.endswith((".h", ".hpp", ".inl", ".inc"))


def compile_seconds(build_dir):
    log = os.path.join(build_dir, ".ninja_log")
    seconds = {}
    if os.path.exists(log):
        for start, end, outputs in read_ninja_log(log):
            for output in outputs:
                seconds[output] = (end - start) / 1000.0
    return seconds


def cmd_fanout(args):
    deps = ninja_deps(args.build_dir)
    seconds = compile_seconds(args.build_dir)
    objects = [o for o in deps if OBJECT_RE.search(o)]
    total_cpu = sum(seconds.get(o, 0.0) for o in objects)
    users = collections.defaultdict(list)
    for obj in objects:
        for header in set(deps[obj]):
            if not header.endswith((".c", ".cc", ".cpp", ".cxx", ".mm")):
                users[header].append(obj)
    print(f"## {args.build_dir}: {len(objects)} units, {total_cpu:.0f} compile CPU-s, {len(users)} headers")

    def row(header, objs):
        cpu = sum(seconds.get(o, 0.0) for o in objs)
        return f"  {len(objs):5d} units  {cpu:7.0f} CPU-s ({100.0 * cpu / total_cpu if total_cpu else 0:4.1f}%)  {header}"

    own = sorted(((h, o) for h, o in users.items() if own_header(h)), key=lambda x: -len(x[1]))
    print(f"### the {args.top} most included own headers (engine/, editor/): what touching one rebuilds")
    for header, objs in own[: args.top]:
        print(row(header, objs))
    print(f"### the {args.top} own headers whose touch costs the most compile time")
    for header, objs in sorted(own, key=lambda x: -sum(seconds.get(o, 0.0) for o in x[1]))[: args.top]:
        print(row(header, objs))
    print(f"### the {args.top} most included headers of any origin (precompiled-header candidates)")
    for header, objs in sorted(users.items(), key=lambda x: -len(x[1]))[: args.top]:
        print(row(header, objs))
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump({h: len(o) for h, o in users.items()}, f, indent=0, sort_keys=True)


def load_compile_commands(build_dir):
    with open(os.path.join(build_dir, "compile_commands.json"), encoding="utf-8") as f:
        entries = json.load(f)
    by_output = {}
    for entry in entries:
        argv = entry["arguments"] if "arguments" in entry else shlex.split(entry["command"])
        if argv and os.path.basename(argv[0]) == "ccache":
            argv = argv[1:]
        out = argv[argv.index("-o") + 1] if "-o" in argv else ""
        by_output[os.path.normpath(os.path.join(entry["directory"], out))] = (entry["directory"], argv, entry["file"])
    return by_output


def strip_output(argv):
    """The command without -o <file>, -c <source> and the dependency-file options."""
    result = []
    skip = False
    for i, arg in enumerate(argv):
        if skip:
            skip = False
            continue
        if arg in ("-o", "-MF", "-MT", "-MQ", "-c"):
            skip = True
            continue
        if arg in ("-MD", "-MMD"):
            continue
        result.append(arg)
    return result


def run_timed(argv, cwd, repeat=1):
    best = None
    for _ in range(repeat):
        t0 = time.monotonic()
        proc = subprocess.run(argv, cwd=cwd, capture_output=True, text=True)
        elapsed = time.monotonic() - t0
        if proc.returncode != 0:
            return None, proc.stderr
        best = elapsed if best is None else min(best, elapsed)
    return best, ""


def cmd_headers(args):
    deps = ninja_deps(args.build_dir)
    commands = load_compile_commands(args.build_dir)
    users = collections.defaultdict(list)
    for obj, headers in deps.items():
        if not OBJECT_RE.search(obj):
            continue
        for header in set(headers):
            if header.endswith((".h", ".hpp", ".hh", ".inl")) or ("/c++/" in header and "." not in os.path.basename(header)):
                users[header].append(obj)
    rows = []
    for header, objs in sorted(users.items(), key=lambda x: -len(x[1]))[: args.top]:
        # parsed with the flags of a C++ unit that includes it
        unit = next((o for o in objs if os.path.normpath(os.path.join(args.build_dir, o)) in commands
                     and commands[os.path.normpath(os.path.join(args.build_dir, o))][2].endswith((".cpp", ".cc", ".cxx"))), None)
        if unit is None:
            continue
        directory, argv, _ = commands[os.path.normpath(os.path.join(args.build_dir, unit))]
        path = header if header.startswith("/") else SOURCE_ROOT + header
        with tempfile.NamedTemporaryFile("w", suffix=".cpp", delete=False) as tmp:
            tmp.write(f'#include "{path}"\n')
        try:
            seconds, err = run_timed(strip_output(argv) + ["-fsyntax-only", tmp.name], directory, repeat=2)
        finally:
            os.unlink(tmp.name)
        if seconds is None:
            print(f"  (failed alone: {header}: {err.strip().splitlines()[0] if err.strip() else ''})", file=sys.stderr)
            continue
        rows.append((seconds * len(objs), seconds, len(objs), header))
    print(f"## {args.build_dir}: the {args.top} most included headers, parsed alone (-fsyntax-only)")
    print("  parse-s x units = CPU-s   header")
    for cost, seconds, count, header in sorted(rows, reverse=True):
        print(f"  {seconds:6.2f} x {count:5d} = {cost:7.0f}   {header}")


PHASE_RE = re.compile(r"^\s*(phase [a-z .]+?|template instantiation|TOTAL)\s*:\s*(.*)$")


def cmd_time_report(args):
    commands = load_compile_commands(args.build_dir)
    seconds = compile_seconds(args.build_dir)
    units = sorted(((s, o) for o, s in seconds.items() if OBJECT_RE.search(o)), reverse=True)
    print(f"## {args.build_dir}: -ftime-report of the {args.top} slowest units (wall seconds)")
    for logged, output in units[: args.top]:
        key = os.path.normpath(os.path.join(args.build_dir, output))
        if key not in commands:
            continue
        directory, argv, source = commands[key]
        proc = subprocess.run(strip_output(argv) + ["-ftime-report", "-c", source, "-o", "/dev/null"],
                              cwd=directory, capture_output=True, text=True)
        phases = {}
        for line in proc.stderr.splitlines():
            m = PHASE_RE.match(line)
            if m:
                numbers = re.findall(r"(\d+\.\d+)\s*\(", m.group(2))
                if len(numbers) >= 3:
                    phases[m.group(1).strip()] = float(numbers[2])
        total = phases.get("TOTAL", 0.0)
        parse = phases.get("phase parsing", 0.0) + phases.get("phase lang. deferred", 0.0)
        gen = phases.get("phase opt and generate", 0.0)
        tmpl = phases.get("template instantiation", 0.0)
        print(f"  {logged:6.1f} s logged, {total:6.1f} s: parse+deferred {parse:5.1f}, opt+generate {gen:5.1f}, "
              f"(templates {tmpl:4.1f})  {os.path.relpath(source, SOURCE_ROOT) if source.startswith(SOURCE_ROOT) else source}")


def cmd_results(args):
    print("| results | scenario | phase | wall s | steps | ccache hits/misses | load before/after | file |")
    print("|---|---|---|---|---|---|---|---|")
    for path in args.results:
        with open(path, encoding="utf-8") as f:
            for line in f:
                if not line.strip():
                    continue
                r = json.loads(line)
                cc = "" if r.get("ccache_hits") is None else f"{r['ccache_hits']}/{r['ccache_misses']}"
                print(f"| {os.path.basename(os.path.dirname(path))} | {r['scenario']} | {r['phase']} | {r['seconds']:.0f} | {r['steps']} | {cc} "
                      f"| {r['load_before']}/{r['load_after']} | {r.get('file', '')} |")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("log")
    p.add_argument("logs", nargs="+")
    p.add_argument("--top", type=int, default=25)
    p.set_defaults(func=cmd_log)
    p = sub.add_parser("fanout")
    p.add_argument("build_dir")
    p.add_argument("--top", type=int, default=20)
    p.add_argument("--json")
    p.set_defaults(func=cmd_fanout)
    p = sub.add_parser("headers")
    p.add_argument("build_dir")
    p.add_argument("--top", type=int, default=40)
    p.set_defaults(func=cmd_headers)
    p = sub.add_parser("time-report")
    p.add_argument("build_dir")
    p.add_argument("--top", type=int, default=12)
    p.set_defaults(func=cmd_time_report)
    p = sub.add_parser("results")
    p.add_argument("results", nargs="+")
    p.set_defaults(func=cmd_results)
    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
