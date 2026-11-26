#!/usr/bin/env python3
"""Tests of tools/crash/symbolicate.py against a real binary.

    python3 -I tools/crash/test_symbolicate.py BINARY

BINARY is an ELF or Mach-O built with symbols and a build id (the native jasperengine-crash-test).
A report naming `main` by module+offset must come back with main named; a Windows report prints
the cdb steps; a report with no frames named exits 1.

--group (shipped-game-diagnostics AC17-AC19): the shared fixtures (tools/crash/fixtures/group/,
which the editor's CrashGroups passes too) give expected.json's groups and short names; a symbols
tree in S30's layout names BINARY's frames only from the copy with the report's build id, never
from a decoy of another build under the same name; 1000 report folders group within 10 s
(printed; enforced when JASPER_TEST_PERF_SLACK is set, as the unit benches are).
"""

from __future__ import annotations

import calendar
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
FIXTURES = HERE / "fixtures" / "group"
GROUP_KEYS = ("signature", "kind", "count", "versions", "platforms", "first_seen", "last_seen", "report_ids", "unresolved")


def symbol_offset(binary: Path, name: str) -> int:
    apple = sys.platform == "darwin"
    out = subprocess.run(["nm", "-U" if apple else "--defined-only", str(binary)], capture_output=True, text=True, check=True).stdout
    base = 0
    if apple:
        commands = subprocess.run(["otool", "-l", str(binary)], capture_output=True, text=True, check=True).stdout
        # Mach-O symbol values include the preferred __TEXT address (usually 4 GiB
        # for an executable); engine reports store offsets from the loaded image.
        match = re.search(r"segname __TEXT\s+vmaddr (0x[0-9a-fA-F]+)", commands)
        if not match:
            raise SystemExit(f"{binary} has no Mach-O __TEXT segment")
        base = int(match[1], 16)
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == ("_" + name if apple else name):
            return int(parts[0], 16) - base
    raise SystemExit(f"{binary} defines no {name}")


def symbolicate(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, "-I", str(HERE / "symbolicate.py"), *args], capture_output=True, text=True)


def tool_module():
    spec = importlib.util.spec_from_file_location("symbolicate", HERE / "symbolicate.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def fixture_copy(into: Path) -> dict:
    """the fixture folders under `into`, each file dated its folder's session time + the offset"""
    expected = json.loads((FIXTURES / "expected.json").read_text())
    shutil.copytree(FIXTURES, into, ignore=shutil.ignore_patterns("expected.json"))
    for folder in into.iterdir():
        match = re.match(r"(\d{4})(\d{2})(\d{2})T(\d{2})(\d{2})(\d{2})Z", folder.name)
        when = calendar.timegm(tuple(int(g) for g in match.groups()) + (0, 0, 0)) + expected["mtime_offset_seconds"]
        for file in folder.iterdir():
            os.utime(file, (when, when))
    return expected


def group_json(*args: str) -> tuple[int, dict | None, str]:
    result = symbolicate("--json", *args)
    try:
        return result.returncode, json.loads(result.stdout), result.stderr
    except ValueError:
        return result.returncode, None, result.stdout + result.stderr


def check_group_fixtures(root: Path, failures: list) -> None:
    """AC17: the shared fixtures give expected.json's groups and short names"""
    reports = root / "group"
    expected = fixture_copy(reports)
    code, out, error = group_json("--group", str(reports))
    if code != 0 or out is None:
        failures.append(f"--group over the fixtures: exit {code}, {error}")
        return
    got = [{k: g[k] for k in GROUP_KEYS} for g in out["groups"]]
    if got != expected["groups"]:
        failures.append("--group differs from expected.json:\n" + json.dumps(got, indent=1))
    if out["reports"] != 8:
        failures.append(f"8 fixture reports, read {out['reports']}")
    print(f"--group: {len(got)} groups of {out['reports']} fixture reports")
    module = tool_module()
    for symbol, name in expected["short_names"]:
        if module.short_name(symbol) != name:
            failures.append(f"short_name({symbol!r}) = {module.short_name(symbol)!r}, expected {name!r}")
    # the filters and the split
    code, out, _ = group_json("--group", str(reports), "--kinds", "user,hang")
    if code != 0 or sorted(g["kind"] for g in out["groups"]) != ["hang", "user"]:
        failures.append(f"--kinds user,hang: {out}")
    code, out, _ = group_json("--group", str(reports), "--by", "version", "--kinds", "crash")
    split = [g for g in out["groups"] if "addItem" in g["signature"]] if out else []
    if code != 0 or sorted(g["version"] for g in split) != ["1.3", "1.4"]:
        failures.append(f"--by version splits the 1.3 and 1.4 crashes of one bug: {split}")
    code, out, _ = group_json("--group", str(reports), "--since", "2026-11-05")
    if code != 0 or out["reports"] != 4:
        failures.append(f"--since 2026-11-05 keeps 4 reports: {out and out['reports']}")
    code, out, _ = group_json("--group", str(reports), "--top", "1")
    if code != 0 or len(out["groups"]) != 1:
        failures.append(f"--top 1: {out}")
    empty = root / "empty"
    empty.mkdir()
    if symbolicate("--group", str(empty)).returncode != 1:
        failures.append("--group over a folder without reports exits 1")
    if symbolicate("--group", str(root / "missing")).returncode != 2:
        failures.append("--group over a missing folder exits 2")
    if symbolicate("--group", str(reports), "--kinds", "crashes").returncode != 2:
        failures.append("an unknown kind exits 2")


def check_build_id_lookup(binary: Path, root: Path, failures: list) -> None:
    """AC18: a symbols tree in S30's layout names the frames only from the file with the report's
    build id; a decoy of another build under the same name is never used"""
    module = tool_module()
    locations = module.build_id_locations(binary)
    if not locations:
        failures.append(f"{binary} has no build id (ELF note or Mach-O LC_UUID): link it with one")
        return
    offset, size = locations[0]
    data = bytearray(binary.read_bytes())
    build_id = bytes(data[offset:offset + size]).hex()
    if binary.read_bytes()[:4] != b"\x7fELF":   # a report writes a Mach-O UUID as dwarfdump prints it
        build_id = build_id.upper()
        build_id = "-".join((build_id[:8], build_id[8:12], build_id[12:16], build_id[16:20], build_id[20:]))
    for start, length in locations:
        for i in range(start, start + length):
            data[i] ^= 0xFF
    main_offset = symbol_offset(binary, "main")
    reports = root / "by-id" / "reports" / "20261110T100000Z-9001"
    reports.mkdir(parents=True)
    (reports / "crash.txt").write_text(
        "JasperEngine crash report v1\nproduct: test\nversion: 1.0\n"
        f"build_id: {build_id}\nplatform: linux-x86_64\nreason: SIGSEGV (invalid memory access)\nsignal: 11\nbacktrace:\n"
        f"#0 {binary.name}+0x{main_offset + 4:x}\n#1 libnothing.so+0x10\nmodules:\n"
        f"55d000000000-55d000100000 r-xp 00000000 08:01 42 /nonexistent/{binary.name}\n")
    symbols = root / "by-id" / "release" / "symbols"
    # the decoy sorts first and has the same symbols: only its build id tells it apart
    decoy = symbols / "a-player" / binary.name
    good = symbols / "game" / binary.name
    for path, content in ((decoy, bytes(data)), (good, binary.read_bytes())):
        path.parent.mkdir(parents=True)
        path.write_bytes(content)
    if module.build_ids(decoy) == module.build_ids(good):
        failures.append("the decoy has the binary's build id")
    code, out, error = group_json("--group", str(reports.parent), "--symbols", str(symbols))
    group = out["groups"][0] if out and out["groups"] else {}
    named = [f["name"] for f in group.get("frames", [])]
    print(f"--symbols with the matching build: {group.get('signature')} {named}")
    if code != 0 or group.get("unresolved") or not named or not named[0].startswith("main") or "| main" not in group.get("signature", ""):
        failures.append(f"the matching build id's file names main: exit {code}, {group}, {error}")
    good.unlink()
    code, out, error = group_json("--group", str(reports.parent), "--symbols", str(symbols))
    group = out["groups"][0] if out and out["groups"] else {}
    print(f"--symbols with only the decoy: {group.get('signature')}")
    if code != 0 or not group.get("unresolved") or "+0x" not in group.get("signature", "") or any(f["name"] for f in group.get("frames", [])):
        failures.append(f"a file of another build id is never used: {group}, {error}")


def check_group_scale(root: Path, failures: list) -> None:
    """AC19: 1000 report folders (125 copies of the fixtures) group with the expected counts"""
    base = root / "scale-base"
    expected = fixture_copy(base)
    many = root / "scale"
    for copy in range(125):
        shutil.copytree(base, many / f"device-{copy:03d}")
    started = time.monotonic()
    code, out, error = group_json("--group", str(many))
    elapsed = time.monotonic() - started
    slack = float(os.environ.get("JASPER_TEST_PERF_SLACK", "0") or 0)
    print(f"[bench] symbolicate.py --group over 1000 report folders: {elapsed:.2f} s (budget 10 s"
          + (f" x {slack:g}, enforced)" if slack > 0 else ", printed: JASPER_TEST_PERF_SLACK unset)"))
    if code != 0 or out is None:
        failures.append(f"--group over 1000 folders: exit {code}, {error}")
        return
    counts = [(g["signature"], g["count"]) for g in out["groups"]]
    wanted = [(g["signature"], g["count"] * 125) for g in expected["groups"]]
    if out["reports"] != 1000 or sorted(counts) != sorted(wanted):
        failures.append(f"1000 folders: {out['reports']} reports, counts {counts}, expected {wanted}")
    if slack > 0 and elapsed > 10.0 * slack:
        failures.append(f"--group over 1000 folders took {elapsed:.2f} s, budget {10.0 * slack:g} s")


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    binary = Path(sys.argv[1]).resolve()
    offset = symbol_offset(binary, "main")
    failures = []
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        report = root / "crash.txt"
        report.write_text("JasperEngine crash report v1\nproduct: test\nplatform: linux-x86_64\nbuild_id: none\n"
                          "reason: SIGSEGV (invalid memory access)\nframes:\n"
                          f"#0 {binary.name}+0x{offset + 4:x}\n#1 libnothing.so+0x10\n")
        result = symbolicate(str(report), "--binary-dir", str(binary.parent))
        print(result.stdout)
        if result.returncode != 0 or "=>  main" not in result.stdout:
            failures.append(f"main was not named (exit {result.returncode}): {result.stdout} {result.stderr}")
        if "1 of 2 frames named" not in result.stdout:
            failures.append("an unknown module must stay unnamed")

        # a library loaded after install: an absolute address, placed through the maps copy
        late = root / "late.txt"
        start = 0x55d000000000
        late.write_text("JasperEngine crash report v1\nplatform: linux-x86_64\nbuild_id: none\nbacktrace:\n"
                        f"#0 module?+0x{start + offset + 4:x}\nmodules:\n"
                        f"{start:x}-{start + 0x10000000:x} r-xp 00000000 08:01 42 {binary}\n")
        result = symbolicate(str(late))
        print(result.stdout)
        if result.returncode != 0 or "=>  main" not in result.stdout:
            failures.append(f"a late library's frame was not placed through the maps: {result.stdout} {result.stderr}")

        windows = root / "win" / "crash.txt"
        windows.parent.mkdir()
        windows.write_text("JasperEngine crash report v1\nplatform: windows-x86_64\nbuild_id: ABC1\nframes:\n#0 game.exe+0x10\n")
        result = symbolicate(str(windows.parent))
        if result.returncode != 0 or "cdb -z" not in result.stdout:
            failures.append(f"a Windows report gives the cdb steps: {result.stdout}")

        empty = root / "none.txt"
        empty.write_text("JasperEngine crash report v1\nplatform: linux-x86_64\nframes:\n#0 missing.so+0x10\n")
        if symbolicate(str(empty)).returncode != 1:
            failures.append("a report with nothing named must exit 1")

        check_group_fixtures(root, failures)
        check_build_id_lookup(binary, root, failures)
        check_group_scale(root, failures)

    for failure in failures:
        print("FAIL:", failure)
    print("PASS" if not failures else f"{len(failures)} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
