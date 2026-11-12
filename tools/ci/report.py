#!/usr/bin/env python3
"""CI failure reports: collect a lane's results, render the PR comment, post it (design §11).

    python3 tools/ci/report.py collect --lane unit [--results build/test-results] [--out <file>]
    python3 tools/ci/report.py render --reports <dir> --lanes lanes.json --run-url <url> --sha <sha>
                                      [--advisory "unit mcp"] [--needs needs.json] [--started-at <iso>]
                                      [--prefix "Removed from the merge queue:"] --out comment.md
    python3 tools/ci/report.py post (--pr <N> | --pr-from-ref <ref> | --head-sha <sha> --head-label <owner:branch>)
                                    --body comment.md [--repo <owner/name>]
    python3 tools/ci/report.py issue --label ci-red|nightly --title <t> --body comment.md [--commit <sha>]

collect runs as each lane's last step (.github/actions/report). It reads what the lane left in
--results: googletest XML, run_tests.py's mcp.json, and every *.log (build logs of GCC, Clang, ld,
MSVC, Emscripten, CMake, Gradle and Xcode; tools/smoke-test.sh transcripts; gate messages of
gtest-gate.py, mcp_gate.py and browser_gate.py). A log whose first line is "$ <command>" was made
by that command, which then is its failures' repro. Environment: JASPER_CI_REPRO (the lane's
command, one per line), JASPER_CI_STATUS (success|failure|cancelled), JASPER_CI_STARTED (epoch
seconds the job started), GITHUB_SHA, GITHUB_WORKSPACE, GITHUB_STEP_SUMMARY (Markdown appended).
It writes <results>/ci-report/<lane>.json and exits 0 whatever it found.

render, post and issue treat every report and lanes.json as untrusted: files over MAX_JSON_BYTES or
of the wrong shape are dropped, all their text is escaped or fenced, and @-mentions are broken.
post and issue call `gh` (GH_TOKEN); the comment is the one carrying COMMENT_MARKER written by
JASPER_CI_REPORT_AUTHOR (default github-actions[bot]). Standard library only.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.parse
import xml.etree.ElementTree as ET
from pathlib import Path

COMMENT_MARKER = "<!-- jasper-ci-report -->"
COMMENT_LIMIT = 60000          # a GitHub comment holds 65,536 characters
FAILURE_LINES = 40             # one failure's block, title to Reproduce
EXCERPT_LINES = 30
LINE_CHARS = 240
FAILURES_PER_LANE = 10
DIAGNOSTICS = 20               # build diagnostics kept per log
CONTEXT_LINES = 3
MAX_JSON_BYTES = 4_000_000
MAX_REPORT_FAILURES = 200
MAX_LOG_BYTES = 256 * 1024 * 1024
MAX_FILES = 400
LANE_RE = re.compile(r"^[a-z0-9][a-z0-9_-]{0,40}$")
SHA_RE = re.compile(r"^[0-9a-f]{7,40}$")
STATUSES = ("success", "failure", "cancelled", "skipped")
KINDS = ("build", "test", "smoke", "gate", "lint", "infra")
TOP_DIRS = ("engine", "editor", "tools", "libs", "docs", "docker", "CMakeLists.txt", "build", ".github")

# what a lane is, for the comment's headings (design §11.2)
LANE_TITLES = {
    "changes": "change detection",
    "lint": "lint",
    "linux": "Linux editor and engine build (GCC)",
    "unit": "unit tests",
    "asan": "unit tests under ASan and UBSan",
    "htmlui": "HTML UI tests",
    "mcp": "MCP tests",
    "smoke": "game export and launch",
    "web": "Web (Emscripten) build and browser gate",
    "android": "Android (Gradle, NDK) build",
    "windows": "Windows MSVC build",
    "macos": "macOS build (Apple Clang)",
    "ios": "iOS Xcode build",
}


# where a lane's repro runs, when that is not the Linux images
REPRO_WHERE = {"windows": " (Windows, x64 Native Tools shell)", "macos": " (macOS, Xcode)", "ios": " (macOS, Xcode)"}


# ---------------------------------------------------------------------------
# Paths


RUNNER_PREFIXES = [
    re.compile(r"^/src/"),                                                     # tools/build.sh's mount
    re.compile(r"^/(?:home/runner|Users/runner)/work/[^/]+/[^/]+/"),          # Linux and macOS runners
    re.compile(r"^[A-Za-z]:/a/[^/]+/[^/]+/"),                                  # Windows runners (D:\a\<repo>\<repo>\)
]
TEXT_PREFIXES = re.compile(r"(?<![\w./\\-])(?:/src/|/(?:home/runner|Users/runner)/work/[^/\s]+/[^/\s]+/|"
                           r"[A-Za-z]:\\a\\[^\\\s]+\\[^\\\s]+\\|[A-Za-z]:/a/[^/\s]+/[^/\s]+/)")


def usable_roots(roots: list[str]) -> list[str]:
    """Absolute checkout roots, at least two levels deep: a root of / (report.py copied to /tmp/x/)
    must not strip every slash."""
    out = []
    for root in roots:
        root = root.replace("\\", "/").rstrip("/")
        if len([part for part in root.split("/") if part and not part.endswith(":")]) >= 2:
            out.append(root)
    return out


def workspace_roots() -> list[str]:
    return usable_roots([os.environ.get("GITHUB_WORKSPACE", ""), str(Path(__file__).resolve().parents[2])])


def relative_path(raw: str, roots: list[str] | None = None) -> str:
    """A path as the repository names it: no Docker mount, runner workspace or ../ from a build dir."""
    path = raw.strip().replace("\\", "/")
    for root in (usable_roots(roots) if roots is not None else workspace_roots()):
        if path.startswith(root + "/"):
            return path[len(root) + 1:]
    for prefix in RUNNER_PREFIXES:
        if prefix.match(path):
            return prefix.sub("", path, count=1)
    stripped = path
    while stripped.startswith("../"):
        stripped = stripped[3:]
    if stripped != path and stripped.split("/", 1)[0] in TOP_DIRS:
        return stripped
    return path


def relative_text(text: str, roots: list[str] | None = None) -> str:
    out = TEXT_PREFIXES.sub("", text)
    for root in (usable_roots(roots) if roots is not None else workspace_roots()):
        out = out.replace(root + "/", "")
    return out


# ---------------------------------------------------------------------------
# Repro commands


ASSIGN_RE = re.compile(r"""\s*([A-Za-z_][A-Za-z0-9_]*)=('[^']*'|"[^"]*"|[^\s'"]*)(?=\s)""")


def split_env(command: str) -> tuple[list[list[str]], str]:
    """The leading NAME=value words of a command, unquoted, and the rest verbatim."""
    env, rest = [], command.strip()
    while True:
        m = ASSIGN_RE.match(rest)
        if not m:
            break
        try:
            value = shlex.split(m.group(2))[0] if m.group(2) else ""
        except (ValueError, IndexError):
            break
        env.append([m.group(1), value])
        rest = rest[m.end():].lstrip()
    return env, rest


def join_env(env: list[list[str]], rest: str) -> str:
    return " ".join([f"{k}={shlex.quote(v)}" for k, v in env] + [rest])


def set_env(env: list[list[str]], name: str, value: str) -> None:
    for pair in env:
        if pair[0] == name:
            pair[1] = value
            return
    env.append([name, value])


def with_filter(args: str, test: str) -> str:
    if "--gtest_filter=" in args:
        return re.sub(r"--gtest_filter=\S*", lambda _: f"--gtest_filter={test}", args)
    return f"{args} --gtest_filter={test}".strip()


def narrow_gtest(base: str, result_stem: str, test: str) -> str:
    """The lane's command run for one googletest test (result_stem: the XML's name, core-asan, editor...)."""
    env, rest = split_env(base)
    if "htmlui-tests" in rest or "htmlui-editor-tests" in rest:
        current = dict(env).get("HTMLUI_TEST_ARGS", "")
        set_env(env, "HTMLUI_TEST_ARGS", with_filter(current, test))
    elif "unit-tests" in rest:
        suite = "editor" if result_stem.startswith("editor") else "core" if result_stem.startswith("core") else ""
        if suite:
            set_env(env, "UNIT_SUITE", suite)
        current = dict(env).get("UNIT_TEST_ARGS", "")
        set_env(env, "UNIT_TEST_ARGS", with_filter(current, test))
    elif "--gtest_filter=" in rest:
        rest = with_filter(rest, test)
    else:
        env.insert(0, ["GTEST_FILTER", test])   # googletest reads it when no --gtest_filter is given
    return join_env(env, rest)


def narrow_mcp(test_file: str, test: str | None) -> str:
    path = f"tools/mcp/tests/{Path(test_file).name}"
    if test:
        return f"MCP_TEST_ARGS={shlex.quote('-k ' + test)} tools/build.sh mcp-tests {path}"
    return f"tools/build.sh mcp-tests {path}"


# ---------------------------------------------------------------------------
# Parsers


ANSI_RE = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]|\x1b\][^\x07]*\x07")
PREFIX_RE = re.compile(r"^(?:C/C\+\+: |\s*\[CXX\d+\] )")  # Gradle's native build lines


def clean_line(line: str) -> str:
    return PREFIX_RE.sub("", ANSI_RE.sub("", line.rstrip("\r\n")).rstrip("\r"))


def make_failure(kind: str, title: str, location: str = "", excerpt: str = "", repro: str = "") -> dict:
    lines = [clip(l) for l in excerpt.rstrip("\n").splitlines()]
    if len(lines) > EXCERPT_LINES + 10:
        lines = lines[:EXCERPT_LINES + 10] + [f"… {len(lines) - EXCERPT_LINES - 10} more lines"]
    return {"kind": kind, "title": clip(title.strip()), "location": clip(location), "excerpt": "\n".join(lines), "repro": repro}


def read_lines(path: Path) -> list[str]:
    with open(path, "rb") as f:
        data = f.read(MAX_LOG_BYTES)
    return [clean_line(line) for line in data.decode("utf-8", "replace").splitlines()]


def tail(lines: list[str], n: int) -> str:
    return "\n".join(lines[-n:])


def parse_gtest_xml(path: Path, repro_base: str) -> tuple[dict, list[dict]]:
    """Counts as tools/ci/gtest-gate.py counts them, and one failure per failed test."""
    tests = {"ran": 0, "failed": 0, "skipped": 0, "disabled": 0}
    failures: list[dict] = []
    try:
        root = ET.parse(path).getroot()
    except (ET.ParseError, OSError):
        return tests, failures
    if root.tag != "testsuites":
        return tests, failures
    for suite in root.iter("testsuite"):
        for case in suite.iter("testcase"):
            name = f"{suite.get('name', '?')}.{case.get('name', '?')}"
            if case.get("status") == "notrun" or case.get("result") == "suppressed":
                tests["disabled"] += 1
                continue
            if case.get("result") == "skipped" or case.find("skipped") is not None:
                tests["skipped"] += 1
                continue
            tests["ran"] += 1
            problems = case.findall("failure") + case.findall("error")
            if not problems:
                continue
            tests["failed"] += 1
            texts = [(p.text or p.get("message") or "").strip() for p in problems]
            location = ""
            first = texts[0].splitlines()[0] if texts and texts[0] else ""
            m = re.match(r"^(.+?):(\d+)$", first) or re.match(r"^(.+?)\((\d+)\)$", first)
            if m:
                location = f"{relative_path(m.group(1))}:{m.group(2)}"
            elif case.get("file"):
                location = f"{relative_path(case.get('file', ''))}:{case.get('line', '0')}"
            body = []
            for text in texts:
                lines = text.splitlines()
                if lines and re.match(r"^.+?(?::\d+|\(\d+\))$", lines[0]):
                    lines = lines[1:] if len(texts) == 1 else [relative_text(lines[0])] + lines[1:]
                body += lines
            failures.append(make_failure("test", name, location, relative_text("\n".join(body)),
                                         narrow_gtest(repro_base, path.stem, name)))
    return tests, failures


def parse_mcp_json(path: Path, repro_base: str, logs: list[Path] = ()) -> tuple[dict, list[dict], list[dict]]:
    """run_tests.py --json: failed and crashed tests, the counts, and the files --suite ci left out."""
    tests = {"ran": 0, "failed": 0, "skipped": 0, "disabled": 0}
    data = load_json(path, max_bytes=64 * 1024 * 1024)
    if not isinstance(data, dict) or not isinstance(data.get("results"), list):
        return tests, [], []
    failures = []
    for r in data["results"]:
        if not isinstance(r, dict):
            continue
        status, name = r.get("status"), str(r.get("name", "?"))
        if status == "skip":
            tests["skipped"] += 1
            continue
        if status not in ("pass", "fail"):
            continue
        tests["ran"] += 1
        if status == "fail":
            tests["failed"] += 1
            file, _, test = name.partition("::")
            detail = str(r.get("detail", ""))
            failures.append(make_failure("test", name, f"tools/mcp/tests/{Path(file).name}",
                                         relative_text(tail(detail.splitlines(), EXCERPT_LINES)), narrow_mcp(file, test)))
    log_lines = []
    for log in logs:
        log_lines += read_lines(log)
    for label in data.get("crashed") or []:
        label = str(label)
        m = re.search(r"(test_[\w-]+\.py)", label)
        file = m.group(1) if m else ""
        chunk = label.rsplit(" (worker exit", 1)[0]
        block = []
        if f"== {chunk}" in log_lines:
            start = log_lines.index(f"== {chunk}") + 1
            end = next((i for i in range(start, len(log_lines)) if log_lines[i].startswith("== ")), len(log_lines))
            block = log_lines[start:end]
        tests["failed"] += 1
        failures.append(make_failure("test", f"{label} crashed", f"tools/mcp/tests/{file}" if file else "",
                                     relative_text(tail(block, EXCERPT_LINES)) or label,
                                     narrow_mcp(file, None) if file else repro_base))
    left_out = [{"file": str(x.get("file", "")), "reason": str(x.get("reason", ""))}
                for x in data.get("left_out") or [] if isinstance(x, dict)]
    return tests, failures, left_out


# Build diagnostics. Each: (regex, how to make location and title from the match).
GCC_RE = re.compile(r"^(?P<file>(?:[A-Za-z]:)?[^:\s][^:]*?):(?P<line>\d+):(?:(?P<col>\d+):)?\s*(?P<sev>(?:fatal )?error): (?P<msg>.+)$")
LD_REF_RE = re.compile(r"^(?P<file>(?:[A-Za-z]:)?[^:\s][^:]*?):(?:(?P<line>\d+)|\([^)]*\)): (?P<msg>undefined reference to .+)$")
MSVC_RE = re.compile(r"^\s*(?P<file>.+?)\((?P<line>\d+)(?:,\d+)?\)\s*:\s*(?P<sev>(?:fatal )?error) (?P<code>[A-Z]+\d+)\s*:\s*(?P<msg>.+)$")
MSVC_LINK_RE = re.compile(r"^\s*(?P<file>[^:]+?)\s+:\s+(?P<sev>(?:fatal )?error) (?P<code>[A-Z]+\d+)\s*:\s*(?P<msg>.+)$")
CMAKE_RE = re.compile(r"^CMake Error(?: at (?P<file>.+?):(?P<line>\d+) \((?P<what>[^)]*)\))?:\s*(?P<msg>.*)$")
TOOL_RE = re.compile(r"^(?:(?P<tool>[\w.+-]+(?:\.exe)?): )?(?P<sev>(?:fatal )?error): (?P<msg>.+)$")
APPLE_UNDEF_RE = re.compile(r"^Undefined symbols for architecture (?P<arch>\S+):$")
GRADLE_RE = re.compile(r"^\* What went wrong:$")
MSBUILD_SUFFIX_RE = re.compile(r"\s+\[[^\]]+\.(?:vcxproj|csproj|proj|targets|sln)\]$")
# lines that only repeat that something above failed
NOISE_RE = re.compile(r"ld returned \d+ exit status|linker command failed with exit code|failed \(returned \d+\)|"
                      r"\d+ errors? generated|build stopped|^\d+ unresolved externals$")
BOUNDARY_RE = re.compile(r"^(?:\[\d+/\d+\] |FAILED: |ninja: |> Task |\*\* BUILD |== )")
GATE_RE = re.compile(r"^(?P<gate>[A-Za-z0-9_.-]*gate[A-Za-z0-9_.-]*): FAIL: (?P<msg>.+)$")
GATE_REPEATS_RE = re.compile(r"^\d+ (?:tests? )?(?:failed|crashed)\b")
RUN_RE = re.compile(r"^\[ RUN      \] (\S+)$")
END_RE = re.compile(r"^\[ +(?:OK|FAILED|SKIPPED) +\] (\S+)")
SMOKE_STEP_RE = re.compile(r"^== (.+)$")
SANITIZER_RE = re.compile(r"^==\d+==ERROR: (?P<san>\w+Sanitizer): (?P<msg>.+)$|^WARNING: (?P<tsan>ThreadSanitizer): (?P<tmsg>.+?)(?: \(pid=\d+\))?$")
UBSAN_RE = re.compile(r"^(?P<file>[^\s:][^:]*?):(?P<line>\d+):(?:\d+:)? runtime error: (?P<msg>.+)$")
FRAME_RE = re.compile(r"^\s+#\d+ 0x[0-9a-f]+ in (?P<fn>.+?) (?P<file>[^\s()]+):(?P<line>\d+)(?::\d+)?$")
TESTBODY_RE = re.compile(r"\b([A-Za-z0-9]+)_([A-Za-z0-9]+)_Test::TestBody\(\)")


def repo_frame(lines: list[str]) -> tuple[str, str]:
    """The first stack frame in this repository's code (location), and the googletest test it is in."""
    location, test = "", ""
    for line in lines:
        m = FRAME_RE.match(line)
        if not m:
            continue
        path = relative_path(m.group("file"))
        if not location and not path.startswith(("/", "../", "libs/")):
            location = f"{path}:{m.group('line')}"
        t = TESTBODY_RE.search(m.group("fn"))
        if t and not test:
            test = f"{t.group(1)}.{t.group(2)}"
        if location and test:
            break
    return location, test


def sanitizer_block(lines: list[str], i: int) -> list[str]:
    block = []
    for line in lines[i:i + 80]:
        block.append(line)
        if line.startswith("SUMMARY: ") or re.match(r"^==\d+==ABORTING", line):
            break
    return block


def next_result_stem(lines: list[str], i: int) -> str:
    for later in lines[i:]:
        m = re.match(r"^gtest-gate: (\S+?)\.xml", later)
        if m:
            return Path(m.group(1)).name
    return ""


def diagnostic_at(lines: list[str], i: int) -> tuple[str, str, bool] | None:
    """(location, title, primary) when lines[i] starts a build diagnostic."""
    line = lines[i]
    m = MSVC_RE.match(line)
    if m:
        msg = MSBUILD_SUFFIX_RE.sub("", m.group("msg"))
        return f"{relative_path(m.group('file'))}:{m.group('line')}", f"{m.group('sev')} {m.group('code')}: {msg}", True
    m = MSVC_LINK_RE.match(line)
    if m and (m.group("code").startswith("LNK") or m.group("file") == "LINK"):
        msg = MSBUILD_SUFFIX_RE.sub("", m.group("msg"))
        return relative_path(m.group("file")), f"{m.group('sev')} {m.group('code')}: {msg}", True
    m = GCC_RE.match(line)
    if m:
        msg = m.group("msg")
        return f"{relative_path(m.group('file'))}:{m.group('line')}", f"{m.group('sev')}: {msg}", not NOISE_RE.search(msg)
    m = LD_REF_RE.match(line)
    if m:
        loc = relative_path(m.group("file")) + (f":{m.group('line')}" if m.group("line") else "")
        return loc, m.group("msg"), True
    m = CMAKE_RE.match(line)
    if m:
        if m.group("file"):
            return f"{relative_path(m.group('file'))}:{m.group('line')}", \
                f"CMake Error at {relative_path(m.group('file'))}:{m.group('line')} ({m.group('what')})", True
        return "", f"CMake Error: {m.group('msg')}".rstrip(": "), True
    m = APPLE_UNDEF_RE.match(line)
    if m:
        symbol = lines[i + 1].strip().split(", referenced from", 1)[0] if i + 1 < len(lines) else ""
        return "", f"Undefined symbols for architecture {m.group('arch')}: {symbol}".rstrip(": "), True
    m = TOOL_RE.match(line)
    if m:
        tool = m.group("tool")
        title = f"{tool + ': ' if tool else ''}{m.group('sev')}: {relative_text(m.group('msg'))}"
        return "", title, not NOISE_RE.search(m.group("msg"))
    return None


def context_after(lines: list[str], i: int, block: bool = False) -> list[str]:
    """The lines after a diagnostic: GCC's source line and caret, MSVC's notes; a CMake block whole."""
    out = []
    j = i + 1
    limit = 14 if block else CONTEXT_LINES
    while j < len(lines) and len(out) < limit:
        line = lines[j]
        if BOUNDARY_RE.match(line) or diagnostic_at(lines, j) is not None and not line.lstrip().startswith(">>>"):
            break
        if block and line and not line.startswith(" "):
            break
        out.append(line)
        j += 1
    while out and not out[-1].strip():
        out.pop()
    return out


def parse_smoke(lines: list[str], repro: str) -> list[dict]:
    step, failures = "", []
    for i, line in enumerate(lines):
        m = SMOKE_STEP_RE.match(line)
        if m:
            step = m.group(1)
        if line.startswith("FAIL: "):
            following = []
            for later in lines[i + 1:]:
                if SMOKE_STEP_RE.match(later) or later.startswith("FAIL: ") or later == "PASS":
                    break
                following.append(later)
            following = following[-EXCERPT_LINES:]
            location = ""
            for j in range(len(following)):
                found = diagnostic_at(following, j)
                if found and found[0] and found[2]:
                    location = found[0]
                    break
            failures.append(make_failure("smoke", f"{step}: {line}" if step else line, location,
                                         relative_text("\n".join([line] + following)), repro))
    return failures


def parse_log(path: Path, repro_base: str, transcripts: list[Path] = ()) -> list[dict]:
    """The failures one log shows: smoke steps, gate verdicts, crashed gtests, then build diagnostics."""
    lines = read_lines(path)
    repro = repro_base
    if lines and lines[0].startswith("$ "):
        repro = lines[0][2:].strip()
    if any(SMOKE_STEP_RE.match(l) for l in lines) and any(l.startswith("FAIL: ") or l == "PASS" for l in lines):
        return parse_smoke(lines, repro)

    failures: list[dict] = []
    # googletest console: a test that started and never ended crashed the binary
    running, crashed_at = None, []
    running_at: list[str] = []
    for i, line in enumerate(lines):
        running_at.append(running[0] if running else "")
        m = RUN_RE.match(line)
        if m:
            if running:
                crashed_at.append(running)
            running = (m.group(1), i)
            continue
        if running and (END_RE.match(line) or line.startswith("[==========]")):
            running = None
        elif running and GATE_RE.match(line) or running and line.startswith("gtest-gate: "):
            crashed_at.append(running)
            running = None
    if running:
        crashed_at.append(running)
    consumed: set[int] = set()
    for name, i in crashed_at:
        end = next((j for j in range(i + 1, len(lines)) if RUN_RE.match(lines[j]) or lines[j].startswith(("[==========]", "gtest-gate: "))),
                   len(lines))
        after = lines[i + 1:end]
        consumed.update(range(i + 1, end))
        title, location = f"{name} crashed", ""
        start = next((k for k, l in enumerate(after) if SANITIZER_RE.match(l)), None)
        if start is not None:
            m = SANITIZER_RE.match(after[start])
            what = re.sub(r" on (?:address|unknown address) 0x[0-9a-f]+.*$", "", m.group("msg") or m.group("tmsg"))
            title = f"{name} crashed: {m.group('san') or m.group('tsan')}: {what}"
            after = after[start:]
            location = repo_frame(after)[0]
        failures.append(make_failure("test", title, location, relative_text("\n".join(after[:EXCERPT_LINES])) or "(no output)",
                                     narrow_gtest(repro, next_result_stem(lines, end), name)))

    # sanitizer reports outside a crashed test: UBSan's runtime errors, a leak at exit, a data race
    for i, line in enumerate(lines):
        if i in consumed:
            continue
        m = UBSAN_RE.match(line)
        if m:
            frames = [l for l in lines[i + 1:i + 6] if FRAME_RE.match(l)]
            test = running_at[i] or repo_frame(lines[i + 1:i + 30])[1]
            failures.append(make_failure("test", f"runtime error: {m.group('msg')}", f"{relative_path(m.group('file'))}:{m.group('line')}",
                                         relative_text("\n".join([line] + frames[:4])),
                                         narrow_gtest(repro, next_result_stem(lines, i), test) if test else repro))
            continue
        m = SANITIZER_RE.match(line)
        if m:
            block = sanitizer_block(lines, i)
            consumed.update(range(i, i + len(block)))
            location, test = repo_frame(block)
            test = test or running_at[i]
            what = re.sub(r" on (?:address|unknown address) 0x[0-9a-f]+.*$", "", m.group("msg") or m.group("tmsg"))
            excerpt = block[:EXCERPT_LINES - 1] + ([block[-1]] if len(block) >= EXCERPT_LINES else [])
            failures.append(make_failure("test", f"{m.group('san') or m.group('tsan')}: {what}", location,
                                         relative_text("\n".join(excerpt)),
                                         narrow_gtest(repro, next_result_stem(lines, i), test) if test else repro))

    # gate verdicts
    for i, line in enumerate(lines):
        m = GATE_RE.match(line)
        if not m:
            continue
        msg = m.group("msg")
        if GATE_REPEATS_RE.match(msg) or crashed_at and "crashed, or never ran" in msg:
            continue
        gate = m.group("gate")
        start = i
        while start > 0 and (lines[start - 1].startswith(gate + ": ") or lines[start - 1].startswith("  ")) \
                and i - start < 15:
            start -= 1
        block = [l for l in lines[start:i + 1] if not GATE_RE.match(l) or l == line]
        if gate == "browser_gate":
            errors = [l for l in lines if re.match(r"^\[[\s\d.-]+\] (?:error|exception|assert): ", l)]
            for transcript in transcripts:
                errors += [l for l in read_lines(transcript) if re.match(r"^\[[\s\d.-]+\] (?:error|exception|assert): ", l)
                           and l not in errors]
            block = errors[:EXCERPT_LINES] + [line]
        failures.append(make_failure("gate", f"{gate}: {msg}", "", relative_text("\n".join(block)), repro))

    # build diagnostics
    primary, secondary, seen = [], [], set()
    i = 0
    while i < len(lines) and len(primary) < DIAGNOSTICS:
        line = lines[i]
        if GRADLE_RE.match(line):
            body = []
            for later in lines[i + 1:]:
                if later.startswith("* Try:") or later.startswith("* Exception is:"):
                    break
                body.append(later)
            body = [b for b in body if b.strip()]
            if body:
                secondary.append(make_failure("build", f"Gradle: {body[0]}", "", relative_text("\n".join(body[:EXCERPT_LINES])), repro))
            i += 1
            continue
        found = diagnostic_at(lines, i)
        if found is None:
            if line.startswith("FAILED: ") and not secondary:
                secondary.append(make_failure("build", line, "", relative_text("\n".join(lines[i:i + 12])), repro))
            i += 1
            continue
        location, title, is_primary = found
        key = (location, title)
        if key not in seen:
            seen.add(key)
            ctx = context_after(lines, i, block=line.startswith("CMake Error"))
            failure = make_failure("build", relative_text(title), location, relative_text("\n".join([line] + ctx)), repro)
            (primary if is_primary else secondary).append(failure)
        i += 1
    failures += primary if primary else secondary[:1]
    return failures


# ---------------------------------------------------------------------------
# collect


def disk_line(path: Path) -> str:
    df = shutil.which("df")
    if df:
        try:
            out = subprocess.run([df, "-h", str(path)], capture_output=True, text=True, timeout=20).stdout.strip()
            if out:
                return out.splitlines()[-1]
        except (OSError, subprocess.SubprocessError):
            pass
    try:
        usage = shutil.disk_usage(path)
        return f"{usage.free / 2**30:.1f}G free of {usage.total / 2**30:.1f}G"
    except OSError:
        return ""


def collect(lane: str, results: Path, out: Path, env: dict | None = None) -> dict:
    env = os.environ if env is None else env
    status = env.get("JASPER_CI_STATUS", "failure") or "failure"
    if status not in STATUSES:
        status = "failure"
    commands = [c.strip() for c in env.get("JASPER_CI_REPRO", "").splitlines() if c.strip()]
    repro = commands[0] if commands else ""
    tests = {"ran": 0, "failed": 0, "skipped": 0, "disabled": 0}
    failures: list[dict] = []
    left_out: list[dict] = []
    report_dir = out.parent.resolve()

    files = []
    if results.is_dir():
        for path in sorted(results.rglob("*")):
            if len(files) >= MAX_FILES:
                break
            if path.is_file() and report_dir not in path.resolve().parents:
                files.append(path)
    logs = [p for p in files if p.suffix == ".log" and "web" not in p.relative_to(results).parts[:-1]]
    transcripts = [p for p in files if p.suffix == ".log" and p not in logs]

    def add(counts: dict) -> None:
        for k in tests:
            tests[k] += counts.get(k, 0)

    for path in files:
        try:
            if path.suffix == ".xml":
                counts, found = parse_gtest_xml(path, repro)
                add(counts)
                failures += found
            elif path.suffix == ".json" and path.name.startswith("mcp") and "seconds" not in path.name:
                counts, found, left = parse_mcp_json(path, repro, logs)
                add(counts)
                failures += found
                left_out += left
        except Exception as error:  # noqa: BLE001  (a parser bug must not lose the rest of the report)
            failures.append(make_failure("infra", f"report.py could not read {path.name}", "", repr(error), repro))

    if status != "success":
        for path in logs:
            try:
                failures += parse_log(path, repro, transcripts)
            except Exception as error:  # noqa: BLE001
                failures.append(make_failure("infra", f"report.py could not read {path.name}", "", repr(error), repro))
        if status == "failure" and not failures:
            kind = "lint" if lane == "lint" else "smoke" if lane == "smoke" else "build" if logs else "infra"
            if logs:
                newest = max(logs, key=lambda p: p.stat().st_mtime)
                failures.append(make_failure(kind, f"{lane} failed: no diagnostic recognised, the tail of {newest.name}",
                                             "", relative_text(tail(read_lines(newest), EXCERPT_LINES)), repro))
            else:
                failures.append(make_failure(kind, f"{lane} failed before it wrote any result: see the job's log",
                                             "", "", repro))
    else:
        failures = []
    if len(failures) > MAX_REPORT_FAILURES:
        dropped = len(failures) - MAX_REPORT_FAILURES
        failures = failures[:MAX_REPORT_FAILURES] + [make_failure("infra", f"{dropped} more failures not kept", "", "", repro)]

    started = env.get("JASPER_CI_STARTED", "")
    seconds = int(time.time() - int(started)) if started.isdigit() else 0
    data = {
        "lane": lane,
        "status": status,
        "sha": env.get("JASPER_CI_SHA") or env.get("GITHUB_SHA", ""),
        "commands": commands,
        "tests": tests,
        "failures": failures,
        "artifacts": [p.relative_to(results).as_posix() for p in files][:50],
        "timings": {"seconds": seconds},
        "disk": disk_line(results if results.is_dir() else Path(".")),
    }
    if left_out:
        data["left_out"] = left_out
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_suffix(".json.tmp")
    tmp.write_text(json.dumps(data, indent=1) + "\n", encoding="utf-8")
    os.replace(tmp, out)

    summary = env.get("GITHUB_STEP_SUMMARY")
    if summary:
        report_obj = sanitise_report(data)
        text = "\n".join(render_lane(report_obj, lane, "")) if status != "success" else \
            f"{lane}: passed{count_note(report_obj)}"
        with open(summary, "a", encoding="utf-8") as f:
            f.write(text + "\n\n")
    return data


# ---------------------------------------------------------------------------
# Untrusted input


def load_json(path: Path, max_bytes: int = MAX_JSON_BYTES):
    try:
        if path.stat().st_size > max_bytes:
            return None
        return json.loads(path.read_text(encoding="utf-8", errors="replace"))
    except (OSError, ValueError, RecursionError):
        return None


def text_field(value, limit: int = 20000) -> str:
    return value[:limit] if isinstance(value, str) else ""


def int_field(value) -> int:
    return value if isinstance(value, int) and not isinstance(value, bool) and 0 <= value < 10**9 else 0


def sanitise_report(data) -> dict | None:
    if not isinstance(data, dict) or not isinstance(data.get("lane"), str) or not LANE_RE.match(data["lane"]):
        return None
    status = data.get("status") if data.get("status") in STATUSES else "failure"
    tests = data.get("tests") if isinstance(data.get("tests"), dict) else {}
    failures = []
    for f in data.get("failures") if isinstance(data.get("failures"), list) else []:
        if isinstance(f, dict) and len(failures) < 500:
            failures.append({"kind": f.get("kind") if f.get("kind") in KINDS else "infra",
                             "title": text_field(f.get("title"), 2000), "location": text_field(f.get("location"), 500),
                             "excerpt": text_field(f.get("excerpt"), 20000), "repro": text_field(f.get("repro"), 2000)})
    timings = data.get("timings") if isinstance(data.get("timings"), dict) else {}
    return {
        "lane": data["lane"], "status": status, "sha": text_field(data.get("sha"), 64),
        "commands": [text_field(c, 2000) for c in (data.get("commands") or [])[:10] if isinstance(c, str)]
        if isinstance(data.get("commands"), list) else [],
        "tests": {k: int_field(tests.get(k)) for k in ("ran", "failed", "skipped", "disabled")},
        "failures": failures,
        "artifacts": [text_field(a, 300) for a in (data.get("artifacts") or [])[:50] if isinstance(a, str)]
        if isinstance(data.get("artifacts"), list) else [],
        "timings": {"seconds": int_field(timings.get("seconds"))},
        "disk": text_field(data.get("disk"), 300),
        "left_out": [{"file": text_field(x.get("file"), 300), "reason": text_field(x.get("reason"), 500)}
                     for x in (data.get("left_out") or [])[:200] if isinstance(x, dict)]
        if isinstance(data.get("left_out"), list) else [],
    }


def load_reports(directory: Path) -> dict[str, dict]:
    """Every <lane>.json under directory (flat, or one ci-report-<lane>/ directory each), by lane."""
    reports: dict[str, dict] = {}
    if not directory.is_dir():
        return reports
    for path in sorted(directory.rglob("*.json"))[:MAX_FILES]:
        if path.is_symlink() or not path.is_file():
            continue
        report = sanitise_report(load_json(path))
        if report and report["lane"] not in reports:
            reports[report["lane"]] = report
    return reports


def load_lanes(path: Path | None) -> dict:
    """lanes.json (tools/ci/plan_lanes.py), reduced to what render and check_gate read."""
    data = load_json(path) if path else None
    out = {"ok": False, "event": "", "full": False, "lanes": {}, "shards": [], "not_in_ci": []}
    if not isinstance(data, dict) or not isinstance(data.get("lanes"), dict):
        return out
    out["ok"] = True
    out["event"] = data.get("event") if isinstance(data.get("event"), str) else ""
    out["full"] = data.get("full") is True
    for name, lane in list(data["lanes"].items())[:60]:
        if isinstance(name, str) and LANE_RE.match(name) and isinstance(lane, dict):
            out["lanes"][name] = {"run": lane.get("run") is True, "why": text_field(lane.get("why"), 300)}
    mcp = data.get("mcp") if isinstance(data.get("mcp"), dict) else {}
    out["shards"] = [s for s in (mcp.get("shards") or [])[:100] if isinstance(s, str) and re.match(r"^[0-9A-Za-z_-]{1,20}$", s)] \
        if isinstance(mcp.get("shards"), list) else []
    out["not_in_ci"] = [{"file": text_field(x.get("file"), 300), "reason": text_field(x.get("reason"), 300)}
                        for x in (mcp.get("not_in_ci") or [])[:200] if isinstance(x, dict)] \
        if isinstance(mcp.get("not_in_ci"), list) else []
    return out


# ---------------------------------------------------------------------------
# Markdown


CONTROL_RE = re.compile(r"[\x00-\x08\x0b-\x1f\x7f\u200e\u200f\u202a-\u202e\u2066-\u2069]")
# GitHub's mention rule: an @ not after a word character (so a@b.com and ?draw@Foo@@QEAA stay as they are)
MENTION_RE = re.compile(r"(?<![A-Za-z0-9_.@-])@(?=[A-Za-z0-9_])")
MD_SPECIAL_RE = re.compile(r"([\\`*_{}\[\]()#+\-.!|~>])")


def clean(text: str) -> str:
    """Untrusted text, minus control and bidi characters, with every @mention broken by a word joiner."""
    return MENTION_RE.sub("@\u2060", CONTROL_RE.sub("", text.replace("\t", "    ")))


def md(text: str) -> str:
    """Untrusted text as inert Markdown: one line, escaped, no HTML."""
    text = " ".join(clean(text).split())
    text = text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
    return MD_SPECIAL_RE.sub(r"\\\1", text)


def code(text: str) -> str:
    """Untrusted text as one code span: a backtick run longer than any inside it."""
    text = " ".join(clean(text).split()) or " "
    longest = max((len(m) for m in re.findall(r"`+", text)), default=0)
    ticks = "`" * (longest + 1)
    if text.startswith("`") or text.endswith("`") or (text.startswith(" ") and text.endswith(" ") and text.strip()):
        text = f" {text} "
    return f"{ticks}{text}{ticks}"


def fence(lines: list[str]) -> list[str]:
    """Untrusted lines in a fenced block no line of them can close."""
    body = [clean(l) for l in lines]
    longest = max((len(m) for l in body for m in re.findall(r"`+", l)), default=0)
    ticks = "`" * max(3, longest + 1)
    return [f"{ticks}text"] + body + [ticks]


def clip(line: str) -> str:
    return line if len(line) <= LINE_CHARS else line[:LINE_CHARS - 1] + "…"


def lane_label(lane: str, shards: list[str]) -> str:
    m = re.match(r"^mcp-(.+)$", lane)
    if m:
        return f"mcp ({m.group(1)}/{len(shards)})" if m.group(1) in shards and shards else f"mcp ({m.group(1)})"
    return lane


def lane_base(lane: str) -> str:
    return "mcp" if lane.startswith("mcp-") else lane


def count_note(report: dict) -> str:
    t = report["tests"]
    if not t["ran"] and not t["skipped"]:
        return ""
    extra = [f"{t[k]} {k}" for k in ("skipped", "disabled") if t[k]]
    return f", {t['ran']} tests" + (f" ({', '.join(extra)})" if extra else "")


def render_failure(f: dict, run_url: str, where: str = "") -> list[str]:
    head = f"**{code(f['title'] or f['kind'])}**" + (f" · {code(f['location'])}" if f["location"] else "")
    out = [head]
    excerpt = [clip(l) for l in f["excerpt"].splitlines()]
    while excerpt and not excerpt[0].strip():
        excerpt.pop(0)
    room = FAILURE_LINES - 5   # title, two fences, Reproduce, the "more lines" note
    if excerpt:
        if len(excerpt) > room:
            cut = len(excerpt) - room
            excerpt = excerpt[:room]
            out += fence(excerpt)
            out.append(f"… {cut} more lines in the [run's logs]({run_url})")
        else:
            out += fence(excerpt)
    out.append(f"Reproduce{where}: {code(f['repro'])}" if f["repro"] else "Reproduce: see the job's log")
    return out


def lane_parts(report: dict, label: str, run_url: str, title: str = "") -> tuple[list[str], list[list[str]], list[str], int]:
    """A failed lane's heading, one unit per failure (FAILURES_PER_LANE at most), its evidence line,
    and how many failures it has."""
    t = report["tests"]
    what = LANE_TITLES.get(lane_base(report["lane"]), "")
    if report["status"] == "cancelled":
        summary = "cancelled"
    elif t["ran"]:
        summary = f"{t['failed']} failed, {max(0, t['ran'] - t['failed'])} passed, {t['skipped']} skipped"
    else:
        summary = what or "failed"
    head = [f"### {title}{label} — {summary}", ""]
    if not report["failures"] and report["status"] != "cancelled":
        head += ["No details were collected: see the job's log.", ""]
    where = REPRO_WHERE.get(lane_base(report["lane"]), "")
    units = [render_failure(f, run_url, where) + [""] for f in report["failures"][:FAILURES_PER_LANE]]
    logs = [a for a in report["artifacts"] if a.endswith((".log", ".png", ".xml", ".json"))][:6]
    evidence = f"Evidence: artifact `test-results-{report['lane']}`"
    if logs:
        evidence += " (" + ", ".join(code(a) for a in logs) + ")"
    return head, units, [evidence, ""], len(report["failures"])


def more_note(report_lane: str, hidden: int, run_url: str) -> list[str]:
    if hidden <= 0:
        return []
    return [f"… and {hidden} more failures in this lane: `ci-report-{report_lane}`"
            + (f", [the run]({run_url})" if run_url else ""), ""]


def render_lane(report: dict, label: str, run_url: str, title: str = "") -> list[str]:
    head, units, evidence, total = lane_parts(report, label, run_url, title)
    return head + [line for unit in units for line in unit] + more_note(report["lane"], total - len(units), run_url) + evidence


def fit(parts: list[tuple[str, tuple]], budget: int, run_url: str) -> list[str]:
    """Every lane's heading and evidence, then failures round-robin across lanes while the budget lasts,
    so one lane's long failures cannot hide another lane. Whole units only: a fence is never cut."""
    def size(lines: list[str]) -> int:
        return sum(len(line) + 1 for line in lines)

    for lane, (head, _, evidence, total) in parts:
        budget -= size(head) + size(evidence) + size(more_note(lane, total, run_url))
    chosen = [0] * len(parts)
    progress = True
    while progress:
        progress = False
        for k, (_, (_, units, _, _)) in enumerate(parts):
            if chosen[k] < len(units) and size(units[chosen[k]]) <= budget:
                budget -= size(units[chosen[k]])
                chosen[k] += 1
                progress = True
    out: list[str] = []
    for k, (lane, (head, units, evidence, total)) in enumerate(parts):
        out += head + [line for unit in units[:chosen[k]] for line in unit] + more_note(lane, total - chosen[k], run_url) + evidence
    return out


def minutes(seconds: int) -> str:
    return "<1 min" if seconds < 60 else f"{round(seconds / 60)} min"


def render(reports: dict[str, dict], lanes: dict, run_url: str, sha: str, advisory: list[str],
           needs: dict | None = None, started_at: str = "", prefix: str = "") -> str:
    """The PR comment (design §11.2): failures first, one line and <details> when green."""
    run_id = (re.search(r"/runs/(\d+)", run_url) or [None, ""])[1]
    run_url = run_url if re.match(r"^https://[\w.-]+/[\w.-]+/[\w.-]+/actions/runs/\d+(?:/attempts/\d+)?$", run_url) else ""
    run_link = f"[run {run_id}]({run_url})" if run_url and run_id else "run"
    sha_short = sha[:7] if SHA_RE.match(sha or "") else "unknown"
    needs = needs if isinstance(needs, dict) else {}
    shards = lanes["shards"]
    advisory_set = {a for a in advisory if LANE_RE.match(a)}

    # every lane that ran, in the plan's order; a shard per MCP shard
    expected: list[str] = []
    for name, lane in lanes["lanes"].items():
        if name in ("report", "check") or not lane["run"]:
            continue
        expected += [f"mcp-{s}" for s in shards] if name == "mcp" and shards else [name]
    expected += [name for name in reports if name not in expected]
    states: dict[str, str] = {}
    for name in expected:
        if name in reports:
            states[name] = reports[name]["status"]
        else:
            result = needs.get(lane_base(name), {}).get("result") if isinstance(needs.get(lane_base(name)), dict) else None
            states[name] = result if result in STATUSES else "missing"
    red = [n for n in expected if states[n] in ("failure", "cancelled") and lane_base(n) not in advisory_set]
    advisory_red = [n for n in expected if states[n] in ("failure", "cancelled") and lane_base(n) in advisory_set]
    missing = [n for n in expected if states[n] == "missing"]
    jobs = len(expected)

    seconds = max([r["timings"]["seconds"] for r in reports.values()] or [0])
    if started_at:
        try:
            start = dt.datetime.fromisoformat(started_at.replace("Z", "+00:00"))
            seconds = int((dt.datetime.now(dt.timezone.utc) - start).total_seconds())
        except ValueError:
            pass
    lead = f"{md(prefix)} " if prefix else ""

    def lane_section(name: str, title: str = "") -> tuple[str, tuple]:
        report = reports.get(name)
        if report is None:
            report = {"lane": name, "status": states[name], "tests": {"ran": 0, "failed": 0, "skipped": 0, "disabled": 0},
                      "failures": [make_failure("infra", f"{name} {states[name]} without a report", "", "", "")],
                      "artifacts": [], "timings": {"seconds": 0}}
        return name, lane_parts(report, lane_label(name, shards), run_url, title)

    def footer() -> list[str]:
        passed = []
        for name, lane in lanes["lanes"].items():
            if name in ("report", "check"):
                continue
            if not lane["run"]:
                passed.append(f"{name} (skipped: {md(lane['why']) or 'not needed'})")
                continue
            for n in ([f"mcp-{s}" for s in shards] if name == "mcp" and shards else [name]):
                if states.get(n) == "success":
                    tests = reports[n]["tests"]["ran"] if n in reports else 0
                    label = lane_label(n, shards)
                    passed.append((f"{label[:-1]}, {tests} tests)" if label.endswith(")") else f"{label} ({tests} tests)")
                                  if tests else label)
        out = []
        if passed:
            out.append("Passed: " + " · ".join(passed))
        if missing:
            out.append("No report from: " + " · ".join(lane_label(n, shards) for n in missing))
        not_run = {x["file"]: x["reason"] for x in lanes["not_in_ci"]}
        for r in reports.values():
            for x in r.get("left_out", []):
                key = x["file"] if "/" in x["file"] else f"tools/mcp/tests/{x['file']}"
                not_run.setdefault(key, x["reason"])
        if not_run:
            out.append("Not run in CI: " + " · ".join(f"{code(f)} ({md(r) or 'no reason given'})"
                                                     for f, r in list(not_run.items())[:40]))
        if run_id:
            one = ("; per failed lane: " + " ".join(f"`-n ci-report-{n}`" for n in red[:10])) if red else ""
            out.append(f"Machine-readable: `gh run download {run_id} -p 'ci-report-*'`{one}")
        return out

    def assemble(head: list[str], banner: list[str], parts: list[tuple[str, tuple]], tail_lines: list[str]) -> str:
        budget = COMMENT_LIMIT - sum(len(line) + 1 for line in head + banner + tail_lines) - 200
        return "\n".join(head + banner + fit(parts, budget, run_url) + tail_lines).rstrip() + "\n"

    banner: list[str] = []
    parts = [lane_section(name, "advisory (rollout): ") for name in advisory_red]
    if advisory_red:
        banner = ["> **advisory (rollout): failing** — " + " · ".join(lane_label(n, shards) for n in advisory_red)
                  + " (shown, not counted: `ADVISORY_LANES`)", ""]

    if not red:
        verdict = f"✅ CI passed on `{sha_short}`: {jobs} jobs" if not missing else \
            f"⚠️ CI finished on `{sha_short}`: {jobs} jobs, {len(missing)} without a report (the `Check` status decides)"
        head = [COMMENT_MARKER, f"{lead}{verdict}, {minutes(seconds)} · {run_link}", ""]
        details = ["<details><summary>Lanes</summary>", ""]
        for name, lane in lanes["lanes"].items():
            if name in ("report", "check"):
                continue
            if not lane["run"]:
                details.append(f"- {name}: skipped ({md(lane['why']) or 'not needed'})")
                continue
            for n in ([f"mcp-{s}" for s in shards] if name == "mcp" and shards else [name]):
                state = {"success": "passed", "missing": "no report"}.get(states.get(n, "missing"), states.get(n, ""))
                details.append(f"- {lane_label(n, shards)}: {state}{count_note(reports[n]) if n in reports else ''}")
        for name in expected:
            if name not in lanes["lanes"] and lane_base(name) not in lanes["lanes"]:
                details.append(f"- {lane_label(name, shards)}: {states[name]}{count_note(reports[name])}")
        if missing:
            head.insert(2, "")
            head.insert(3, "No report from: " + " · ".join(lane_label(n, shards) for n in missing))
        details += [""] + [line for line in footer() if not line.startswith(("Passed: ", "No report from: "))] + ["", "</details>"]
        return assemble(head, banner, parts, details)

    head = [COMMENT_MARKER,
            f"{lead}❌ CI failed on `{sha_short}` — {len(red)} of {jobs} jobs · {run_link} · {minutes(seconds)}", ""]
    parts += [lane_section(name) for name in red]
    return assemble(head, banner, parts, [""] + footer())


# ---------------------------------------------------------------------------
# gh


def gh(*args: str, check: bool = True) -> str:
    p = subprocess.run(["gh", *args], capture_output=True, text=True)
    if check and p.returncode != 0:
        raise SystemExit(f"report.py: gh {' '.join(args[:3])} failed ({p.returncode}): {p.stderr.strip()[:2000]}")
    return p.stdout


def repo_arg(value: str | None) -> str:
    repo = value or os.environ.get("GITHUB_REPOSITORY", "")
    if not re.match(r"^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$", repo):
        raise SystemExit("report.py: --repo <owner/name> (or GITHUB_REPOSITORY) is needed")
    return repo


def read_body(path: Path) -> str:
    body = path.read_text(encoding="utf-8")
    if COMMENT_MARKER not in body:
        body = f"{COMMENT_MARKER}\n{body}"
    if len(body) > 65536:
        raise SystemExit(f"report.py: {path} is {len(body)} characters, more than a comment holds")
    return body


def api_with_body(method: str, endpoint: str, body: str) -> str:
    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False, encoding="utf-8") as f:
        json.dump({"body": body}, f)
        name = f.name
    try:
        return gh("api", "--method", method, endpoint, "--input", name)
    finally:
        os.unlink(name)


def find_pr(args, repo: str) -> int | None:
    if args.pr is not None:
        return args.pr
    if args.pr_from_ref:
        m = re.match(r"^(?:refs/heads/)?gh-readonly-queue/[^/]+(?:/[^/]+)*/pr-(\d+)-[0-9a-f]+$", args.pr_from_ref)
        if not m:
            raise SystemExit(f"report.py: {args.pr_from_ref!r} is not a merge-queue ref (gh-readonly-queue/<base>/pr-<N>-<sha>)")
        return int(m.group(1))
    if not (args.head_sha and SHA_RE.match(args.head_sha) and len(args.head_sha) == 40 and args.head_label):
        raise SystemExit("report.py: give --pr, --pr-from-ref, or --head-sha with --head-label")
    head = urllib.parse.quote(args.head_label, safe="")
    numbers = gh("api", f"repos/{repo}/pulls?state=open&head={head}&per_page=100",
                 "--jq", f".[] | select(.head.sha == {json.dumps(args.head_sha)}) | .number").split()
    return int(numbers[0]) if numbers and numbers[0].isdigit() else None


def post(args) -> int:
    repo = repo_arg(args.repo)
    body = read_body(args.body)
    pr = find_pr(args, repo)
    if pr is None:
        print(f"report.py: no open pull request has head {args.head_label} at {args.head_sha}: nothing posted")
        return 0
    author = os.environ.get("JASPER_CI_REPORT_AUTHOR", "github-actions[bot]")
    if not re.match(r"^[A-Za-z0-9\[\]-]+$", author):
        raise SystemExit("report.py: JASPER_CI_REPORT_AUTHOR is not a login")
    ids = gh("api", "--paginate", f"repos/{repo}/issues/{pr}/comments?per_page=100", "--jq",
             f".[] | select(.user.login == {json.dumps(author)} and (.body | contains({json.dumps(COMMENT_MARKER)}))) | .id").split()
    if ids and ids[0].isdigit():
        api_with_body("PATCH", f"repos/{repo}/issues/comments/{ids[0]}", body)
        print(f"report.py: updated comment {ids[0]} on #{pr}")
    else:
        api_with_body("POST", f"repos/{repo}/issues/{pr}/comments", body)
        print(f"report.py: commented on #{pr}")
    return 0


ISSUE_LABELS = {"ci-red": ("B60205", "CI failures on main"), "nightly": ("B60205", "Nightly workflow failures")}


def issue(args) -> int:
    """Open or comment on the one open issue with the label, as the old nightly.yml reporter did."""
    repo = repo_arg(args.repo)
    body = read_body(args.body)
    color, description = ISSUE_LABELS[args.label]
    gh("label", "create", args.label, "--repo", repo, "--color", color, "--description", description, check=False)
    number = gh("issue", "list", "--repo", repo, "--label", args.label, "--state", "open", "--limit", "1",
                "--json", "number", "--jq", ".[0].number // empty").strip()
    with tempfile.NamedTemporaryFile("w", suffix=".md", delete=False, encoding="utf-8") as f:
        f.write(body)
        body_file = f.name
    try:
        if number.isdigit():
            gh("issue", "comment", number, "--repo", repo, "--body-file", body_file)
            print(f"report.py: commented on issue #{number}")
        else:
            url = gh("issue", "create", "--repo", repo, "--label", args.label, "--title", args.title,
                     "--body-file", body_file).strip()
            number = (re.search(r"/issues/(\d+)", url) or [None, ""])[1]
            print(f"report.py: opened issue {url}")
    finally:
        os.unlink(body_file)
    if args.commit and number:
        if not (SHA_RE.match(args.commit) and len(args.commit) == 40):
            raise SystemExit("report.py: --commit takes a full commit sha")
        server = os.environ.get("GITHUB_SERVER_URL", "https://github.com").rstrip("/")
        link = f"{server}/{repo}/issues/{number}"
        for pr in gh("api", f"repos/{repo}/commits/{args.commit}/pulls", "--jq", ".[].number", check=False).split()[:1]:
            if pr.isdigit():
                gh("pr", "comment", pr, "--repo", repo, "--body",
                   f"CI failed on `{args.commit[:7]}` after this pull request landed: {link}", check=False)
    return 0


# ---------------------------------------------------------------------------


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    c = sub.add_parser("collect", help="write one lane's report JSON")
    c.add_argument("--lane", required=True)
    c.add_argument("--results", type=Path, default=Path("build/test-results"))
    c.add_argument("--out", type=Path)
    r = sub.add_parser("render", help="the PR comment from every lane's report")
    r.add_argument("--reports", type=Path, required=True)
    r.add_argument("--lanes", type=Path, required=True)
    r.add_argument("--run-url", required=True)
    r.add_argument("--sha", required=True)
    r.add_argument("--advisory", default="", help="space separated lanes whose failures do not count (rollout)")
    r.add_argument("--advisory-from", type=Path, help="read ADVISORY_LANES from this workflow file instead")
    r.add_argument("--needs", type=Path, help="toJSON(needs): job results for lanes that left no report")
    r.add_argument("--started-at", default="", help="the run's start (ISO 8601), for its duration")
    r.add_argument("--prefix", default="", help='text before the verdict ("Removed from the merge queue:")')
    r.add_argument("--out", type=Path, required=True)
    p = sub.add_parser("post", help="upsert the PR comment")
    target = p.add_mutually_exclusive_group(required=True)
    target.add_argument("--pr", type=int)
    target.add_argument("--pr-from-ref", help="a merge-queue ref, gh-readonly-queue/<base>/pr-<N>-<sha>")
    target.add_argument("--head-sha", help="with --head-label: the open PR whose head is this commit (fork PRs)")
    p.add_argument("--head-label", help="<owner>:<branch> of the PR's head")
    p.add_argument("--body", type=Path, required=True)
    p.add_argument("--repo")
    i = sub.add_parser("issue", help="open or comment the labelled issue")
    i.add_argument("--label", required=True, choices=sorted(ISSUE_LABELS))
    i.add_argument("--title", required=True)
    i.add_argument("--body", type=Path, required=True)
    i.add_argument("--commit", help="also comment on the PR that landed this commit, linking the issue")
    i.add_argument("--repo")
    args = parser.parse_args(argv)

    if args.command == "collect":
        if not LANE_RE.match(args.lane):
            parser.error(f"--lane {args.lane!r}: lowercase letters, digits, - and _ only")
        out = args.out or args.results / "ci-report" / f"{args.lane}.json"
        data = collect(args.lane, args.results, out)
        print(f"report.py: {args.lane} {data['status']}: {len(data['failures'])} failures, {data['tests']['ran']} tests ran -> {out}")
        return 0
    if args.command == "render":
        advisory = args.advisory
        if args.advisory_from:
            m = re.search(r"^\s*ADVISORY_LANES:\s*['\"]?([a-z0-9 _-]*)['\"]?\s*$",
                          args.advisory_from.read_text(encoding="utf-8"), re.M)
            advisory = m.group(1) if m else ""
        needs = load_json(args.needs) if args.needs else None
        body = render(load_reports(args.reports), load_lanes(args.lanes), args.run_url, args.sha, advisory.split(),
                      needs, args.started_at, args.prefix)
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(body, encoding="utf-8")
        print(f"report.py: wrote {args.out} ({len(body)} characters)")
        return 0
    if args.command == "post":
        return post(args)
    return issue(args)


if __name__ == "__main__":
    sys.exit(main())
