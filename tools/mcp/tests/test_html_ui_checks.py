"""HTML/CSS UI checks (phase 4, p4-checks, spec §3.4 and §5.2): `jasperengine-editor check --ui`
(default set, --all, --files, text/JSON, --hook with UI warnings shown once), `ui-support` and the
public support schema and diagnostic codes, and the parity of the CLI findings
with what the editor reports for the same files (external_changes problems, a live document's
diagnostics).

The CLI tests run on scratch copies of the minimal HTML project (no editor). The parity test uses the editor
(and inspect_ui, p4-mcp-tools).
"""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import tempfile
import time
from contextlib import contextmanager
from pathlib import Path

import yaml

import harness
from harness import Editor, TestFailure, REPO

EDITOR = {"project": REPO / "tools" / "fixtures" / "html-ui"}
CI = True

TIMEOUT = 900

# No network: the editor's automatic import of LLM_HTML's remote URLs (the parity test loads it in a
# live document) reads this mirror, a folder that does not exist, and fails, so the editor and the CLI
# see the same page (with the network, the font could arrive in the middle of the comparison). Set
# before the editor starts; a test file that imports this one sets its own mirror afterwards.
os.environ["JASPERENGINE_IMPORT_MIRROR"] = str(Path(tempfile.gettempdir()) / "jasper-uicheck-no-network")

FINDING_KEYS = {"tool", "path", "line", "col", "severity", "message"}
TEXT_LINE = re.compile(r"^(?P<path>[^:]+):(?P<line>\d+):(?P<col>\d+): (?P<severity>error|warning): (?P<message>.*?)"
                       r"(?: \(fix: (?P<fix>.*)\))? \[(?P<code>HTMLUI-[A-Z]+-\d{4})\]$")
SHOWN_ONCE = "UI warnings are shown once"

# ---------------------------------------------------------------------------- shared test data

# A 30-line panel: a title, two buttons and a list, with flex, grid, var(), clamp() and ::before;
# compat-clean (no warning, no error)
GOOD_HTML = """<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <title>Panel</title>
  <link rel="stylesheet" href="panel.css">
</head>
<body>
  <div class="panel">
    <h1 class="title">Settings</h1>
    <div class="actions">
      <button id="ok" class="btn primary">OK</button>
      <button id="cancel" class="btn">Cancel</button>
    </div>
    <ul class="list">
      <li>Audio</li>
      <li>Video</li>
      <li>Controls</li>
    </ul>
  </div>
</body>
</html>
"""

GOOD_CSS = """:root { --accent: #3fa9f5; --panel: #1e2430; }
body { margin: 0; font-family: sans-serif; color: #ffffff; }
.panel {
  position: absolute; left: 40px; top: 40px; width: 420px;
  padding: clamp(12px, 2vw, 24px);
  background-color: var(--panel);
  border-radius: 8px;
  display: flex; flex-direction: column; gap: 12px;
}
.title { margin: 0; font-size: 28px; color: var(--accent); }
.title::before { content: "> "; color: #ffffff; }
.actions { display: grid; grid-template-columns: 1fr 1fr; gap: 8px; }
.btn { padding: 8px 12px; background-color: #39414f; border: 1px solid #556070; color: #ffffff; text-align: center; }
.btn.primary { background-color: var(--accent); }
.list { margin: 0; padding-left: 20px; }
"""

# A model's first attempt at a shop screen (spec §5.7): the definitional mistakes in the HTML,
# one table-driven construct per CSS line
LLM_HTML = """<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <title>Shop</title>
  <link href="https://fonts.googleapis.com/css2?family=Karla:wght@400;700&display=swap" rel="stylesheet">
  <link rel="stylesheet" href="shop_llm.css">
  <script src="app.js"></script>
</head>
<body>
  <div class="shop">
    <h1 class="title">Item Shop</h1>
    <div class="grid">
      <div class="card"><span class="sale">-20%</span>
        <img src="https://example.com/sword.png" alt="Sword">
        <h2 class="name">Sword</h2>
        <button class="buy" onclick="buy('sword')">Buy</button>
      </div>
      <div class="card">
        <button class="buy-legendary">Purchase Legendary Sword of Infinite Destiny</button>
      </div>
    </div>
    <canvas id="fx"></canvas>
  </div>
</body>
</html>
"""

LLM_CSS = """:root { --accent: #ffb400; }
body { margin: 0; font-family: 'Karla', sans-serif; }
.shop { position: sticky; top: 0; }
.shop { padding: clamp(8px, 2vw, 24px); }
.shop { background: rgba(0,0,0,.5); }
.title { color: var(--accent) !important; }
.grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(160px, 1fr)); }
.grid { gap: 16px; }
.card { border: 1px solid #333; }
.card { border-radius: 50%; }
.card { aspect-ratio: 1; }
.card { transition: all .2s ease-in-out; }
.card { mix-blend-mode: screen; }
.card:has(.sale) { outline: 2px solid gold; }
.card::before { content: "*"; }
.name { text-overflow: ellipsis; overflow: hidden; white-space: nowrap; }
.buy-legendary { width: 120px; white-space: nowrap; }
@container (min-width: 400px) { .card { padding: 4px; } }
@media (prefers-color-scheme: dark) { .shop { background: #111; } }
"""

# ---------------------------------------------------------------------------- helpers

def fail(message: str) -> None:
    raise TestFailure(message)


def check(cond, message: str) -> None:
    if not cond:
        fail(message)


def check_eq(actual, expected, message: str = "") -> None:
    if actual != expected:
        fail(f"{message + ': ' if message else ''}expected {expected!r}, got {actual!r}")


def _ignore_copy(directory: str, names: list[str]) -> set[str]:
    ignored = {".git"} & set(names)
    if Path(directory).name == ".jasperengine" and "build" in names:
        ignored.add("build")
    return ignored


@contextmanager
def scratch_project(source: Path | None = None):
    """A temporary copy of the minimal HTML project (or of `source`), without .git and .jasperengine/build."""
    tmp = Path(tempfile.mkdtemp(prefix="jasper-uicheck-"))
    try:
        project = tmp / "project"
        shutil.copytree(source or EDITOR["project"], project, ignore=_ignore_copy, symlinks=True)
        yield project
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def editor_cli(*args: str, cwd: Path | None = None, stdin: str | None = None, timeout: float = 120):
    return subprocess.run(harness.editor_command() + list(args), cwd=str(cwd) if cwd else None,
                          input=stdin if stdin is not None else "", capture_output=True, text=True, timeout=timeout)


def run_check(project: Path, *args: str, cwd: Path | None = None, stdin: str | None = None,
              timeout: float = 120) -> subprocess.CompletedProcess:
    return editor_cli("check", "--project", str(project), *args, cwd=cwd or project, stdin=stdin, timeout=timeout)


def run_json(project: Path, *args: str, **kw) -> tuple[int, dict]:
    r = run_check(project, *args, "--format", "json", **kw)
    try:
        data = json.loads(r.stdout)
    except json.JSONDecodeError:
        fail(f"--format json printed something that is not JSON (exit {r.returncode}):\n"
             f"stdout: {r.stdout[-2000:]}\nstderr: {r.stderr[-2000:]}")
    return r.returncode, data


def write(project: Path, rel: str, text: str) -> Path:
    path = project / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    return path


def line_of(text: str, needle: str) -> int:
    """1-based line of the first line containing `needle`."""
    for i, line in enumerate(text.split("\n")):
        if needle in line:
            return i + 1
    fail(f"test data has no line with {needle!r}")
    return 0


def col_of(text: str, needle: str) -> int:
    line = text.split("\n")[line_of(text, needle) - 1]
    return line.index(needle) + 1


def ui_support() -> dict:
    r = editor_cli("ui-support")
    check_eq(r.returncode, 0, f"ui-support: {r.stderr}")
    return json.loads(r.stdout)


def support_row(data: dict, kind: str, name: str, prop: str | None = None) -> dict | None:
    for f in data["features"]:
        if f["kind"] == kind and f["name"] == name and (prop is None or f.get("property") == prop):
            return f
    return None


def hook_input(project: Path, rel: str | None = None, tool: str = "Write", **tool_input) -> str:
    data = {"hook_event_name": "PostToolUse", "tool_name": tool, "cwd": str(project), "tool_input": dict(tool_input)}
    if rel is not None:
        data["tool_input"]["file_path"] = str(project / rel)
        data["tool_input"].setdefault("content", "...")
    return json.dumps(data)


def touch_write(path: Path, text: str) -> None:
    """Write so the mtime surely differs from the previous write (coarse-mtime filesystems)."""
    time.sleep(0.02)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def normalized(findings) -> set[tuple]:
    return {(f["path"], f["line"], f["col"], f["severity"], f["code"]) for f in findings}


def tiny_png() -> bytes:
    """A valid 1x1 RGBA PNG."""
    import struct
    import zlib

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    raw = b"\x00\xff\x00\x00\xff"
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))



# ---------------------------------------------------------------------------- 1. usage

async def test_cli_usage_ui():
    r = editor_cli("check", "--help")
    check_eq(r.returncode, 0, "check --help")
    check("--ui" in r.stdout, "check --help advertises the UI option")
    r = editor_cli("ui-support", "--help")
    check_eq(r.returncode, 0, "ui-support --help")
    r = editor_cli("ui-support", "--nope")
    check_eq(r.returncode, 2, "ui-support rejects an unknown option as a usage error")


# ---------------------------------------------------------------------------- 2. default set

async def test_cli_default_includes_ui():
    with scratch_project() as project:
        write(project, "assets/ui/panel.html", GOOD_HTML)
        write(project, "assets/ui/panel.css", GOOD_CSS)
        code, data = run_json(project)
        check_eq(code, 0, f"the clean panel passes the default check: {data}")
        check_eq((data["errors"], data["warnings"]), (0, 0), f"GOOD_HTML/GOOD_CSS are compat-clean: {data['findings']}")
        check_eq(data["checked"].get("ui"), 1, f"one document; its linked css is not a standalone stylesheet: {data['checked']}")
        check("lua" in data["checked"] and "shaders" in data["checked"], f"the default keeps lua and shaders: {data['checked']}")

        write(project, "assets/ui/orphan.css", ".x { color: red; }\n")
        code, data = run_json(project)
        check_eq(data["checked"].get("ui"), 2, f"an orphan css is checked on its own: {data['checked']}")
        code, data = run_json(project, "--all", timeout=900)
        check_eq(data["checked"].get("ui"), 2, f"--all includes ui: {data['checked']}")
        code, data = run_json(project, "--lua")
        check("ui" not in data["checked"], f"--lua alone has no ui key: {data['checked']}")
        code, data = run_json(project, "--ui")
        check_eq(data["checked"], {"ui": 2}, "--ui alone")

        r = run_check(project)
        check_eq(r.returncode, 0, r.stderr)
        check(re.search(r"\(lua \d+ files?, (navigation \d+ surfaces?, )?shaders \d+ forks?, (texture_imports \d+ textures?, )?ui 2 files(, textures \d+ recipes?)?(, anims \d+ clips?)?(, vat \d+ files?)?(, normals \d+ maps?)?(, videos \d+ Video Players?)?\)", r.stderr), f"summary names ui files: {r.stderr}")


# ---------------------------------------------------------------------------- 3. findings

async def test_cli_ui_findings_text_and_json():
    with scratch_project() as project:
        write(project, "assets/ui/shop_llm.html", LLM_HTML)
        write(project, "assets/ui/shop_llm.css", LLM_CSS)
        code, data = run_json(project, "--ui")
        check_eq(code, 1, f"errors exit 1: {data}")
        findings = data["findings"]
        check(findings, "LLM files have findings")
        check_eq(data["errors"], sum(f["severity"] == "error" for f in findings), "error count")
        check_eq(data["warnings"], sum(f["severity"] == "warning" for f in findings), "warning count")
        for f in findings:
            check_eq(f["tool"], "ui", f"tool: {f}")
            keys = FINDING_KEYS | {"code"} | ({"suggestion"} if f.get("suggestion") else set())
            check_eq(set(f), keys, f"finding keys: {f}")
            check(f["path"].startswith("assets/ui/"), f"paths are project-relative: {f}")
            check(re.fullmatch(r"HTMLUI-[A-Z]+-\d{4}", f["code"]), f"code: {f}")
            check(f["severity"] in ("error", "warning"), f"no info at check level: {f}")
        css = [f for f in findings if f["path"] == "assets/ui/shop_llm.css"]
        check(any(f["code"] == "HTMLUI-CSS-0002" and f["line"] == line_of(LLM_CSS, "mix-blend-mode") for f in css),
              f"css diagnostics are reported at the css path and line: {css}")
        script = [f for f in findings if f["code"] == "HTMLUI-HTML-0006"]
        check_eq([(f["path"], f["line"], f["col"], f["severity"]) for f in script],
                 [("assets/ui/shop_llm.html", line_of(LLM_HTML, "<script"), col_of(LLM_HTML, "<script"), "error")],
                 "the <script> finding: an error at its exact line and column")
        check(script[0].get("suggestion") and "lua" in script[0]["suggestion"].lower(), f"the script fix names Lua: {script}")
        order = [(f["path"], f["line"], f["col"]) for f in findings]
        check_eq(order, sorted(order), "sorted by path, then line")

        r = run_check(project, "--ui")
        check_eq(r.returncode, 1, r.stderr)
        lines = r.stdout.splitlines()
        check_eq(len(lines), len(findings), f"one text line per finding: {r.stdout}")
        for line, f in zip(lines, findings):
            m = TEXT_LINE.match(line)
            check(m, f"compiler-style line `path:line:col: severity: message (fix: ...) [code]`: {line!r}")
            check_eq((m["path"], int(m["line"]), int(m["col"]), m["severity"], m["code"]),
                     (f["path"], f["line"], f["col"], f["severity"], f["code"]), "text and JSON agree")
            check_eq(bool(m["fix"]), bool(f.get("suggestion")), f"(fix: ...) when there is a suggestion: {line}")
        check(re.search(r"check: \d+ errors?, \d+ warnings? \(ui 1 file\)", r.stderr), f"summary: {r.stderr}")


# ---------------------------------------------------------------------------- 4. --files, exclusions, missing files

MISSING_HTML = """<html>
<head>
  <link rel="stylesheet" href="nowhere.css">
  <link rel="stylesheet" href="fonts.css">
</head>
<body>
  <img src="missing.png">
  <div class="icon"></div>
</body>
</html>
"""

FONTS_CSS = """@font-face { font-family: "Gone"; src: url(gone.ttf); }
body { font-family: "Gone", sans-serif; }
.icon { width: 32px; height: 32px; background-image: url(nothere.svg); }
"""

MALFORMED_HTML = """<html>
<body>
  <div class="a">
    <p>unclosed <b>bold
  <div class="b"
</body>
"""

MALFORMED_CSS = """.a { color: red;
.b { width: 10px; }}
.c { color: ; zoom: 2 }
"""


async def test_cli_ui_widget_codes():
    """S17: check --ui reports the compile's widget codes at their element: a view without its
    <template> (HTMLUI-HTML-0030, error). The runtime codes (0029 a view without a definite height,
    0031 a data-for of more than 500 elements) need a layout or a model, and 0032 (HTML in
    Markdown) is a note: none of them is a check finding."""
    html = ("<!DOCTYPE html>\n<html>\n<head><title>w</title></head>\n<body>\n"
            "<list-view items=\"rows\" style=\"height: 100px\"></list-view>\n"
            "<table-view items=\"rows\" style=\"height: 100px\"><table-column key=\"a\">A</table-column></table-view>\n"
            "<markdown>\n  Some <b>bold</b> text\n</markdown>\n"
            "</body>\n</html>\n")
    with scratch_project() as project:
        write(project, "assets/ui/widgets.html", html)
        code, data = run_json(project, "--ui", "--files", "assets/ui/widgets.html")
    findings = [f for f in data["findings"] if f["path"] == "assets/ui/widgets.html"]
    check_eq([(f["code"], f["severity"], f["line"]) for f in findings], [("HTMLUI-HTML-0030", "error", 5)],
             f"the list-view without a template, nothing else: {findings}")
    check_eq(code, 1, "an error exits 1")


async def test_cli_ui_files_and_exclusions():
    with scratch_project() as project:
        write(project, "assets/ui/x.css", ".x { mix-blend-mode: multiply; }\n")
        write(project, "assets/ui/panel.html", GOOD_HTML)
        write(project, "assets/ui/panel.css", GOOD_CSS)
        code, data = run_json(project, "--files", "assets/ui/x.css")
        check_eq(data["checked"], {"ui": 1}, "--files with one css checks only it")
        check_eq({f["path"] for f in data["findings"]}, {"assets/ui/x.css"}, f"only that css: {data['findings']}")
        check_eq(code, 0, "warnings only: exit 0")

        # outside the project: skipped (and said so)
        outside = project.parent / "outside.html"
        outside.write_text(LLM_HTML)
        r = run_check(project, "--files", str(outside))
        check_eq(r.returncode, 0, r.stderr)
        check("not in the project, skipped" in r.stderr, f"says it skipped the outside file: {r.stderr}")
        check_eq(r.stdout, "", "no finding for a file outside the project")

        # the web export's shell is a browser page, not a game UI
        yaml_path = project / "project.yaml"
        settings = yaml.safe_load(yaml_path.read_text())
        settings["web"] = dict(settings.get("web") or {}, customHtmlShell="assets/web/shell.html")
        yaml_path.write_text(yaml.safe_dump(settings, sort_keys=False))
        write(project, "assets/web/shell.html", "<html><body><canvas id=\"canvas\"></canvas>\n<script>var Module = {};</script>\n{{{ SCRIPT }}}</body></html>\n")
        # build/ and hidden directories are skipped
        write(project, "assets/build/bad.html", LLM_HTML)
        write(project, "assets/.hidden/bad.html", LLM_HTML)
        write(project, "build/ui/bad.html", LLM_HTML)
        write(project, ".jasperengine/bad.css", ".x { zoom: 2 }\n")
        code, data = run_json(project, "--ui")
        paths = {f["path"] for f in data["findings"]}
        check("assets/web/shell.html" not in paths, f"the custom web shell is not checked: {paths}")
        check(not any("build/" in p or ".hidden" in p or ".jasperengine" in p for p in paths), f"skipped dirs: {paths}")
        check_eq(data["checked"], {"ui": 2}, "panel.html and x.css only")
        code, data = run_json(project, "--files", "assets/web/shell.html", "assets/build/bad.html", "build/ui/bad.html")
        check_eq((code, data["findings"]), (0, []), "the shell and skipped dirs are skipped with --files too")

        # missing files: a linked stylesheet (at the <link>), an image, a font, an svg
        write(project, "assets/ui/missing.html", MISSING_HTML)
        write(project, "assets/ui/fonts.css", FONTS_CSS)
        code, data = run_json(project, "--files", "assets/ui/missing.html")
        check_eq(code, 1, f"missing files are errors: {data}")
        by_line = {(f["path"], f["line"]): f for f in data["findings"] if f["severity"] == "error"}
        link = by_line.get(("assets/ui/missing.html", line_of(MISSING_HTML, "nowhere.css")))
        check(link and link["code"] == "HTMLUI-GEN-0001", f"a missing linked css is an error at the <link> line: {data['findings']}")
        img = by_line.get(("assets/ui/missing.html", line_of(MISSING_HTML, "missing.png")))
        check(img and img["code"] == "HTMLUI-GEN-0001", f"a missing image is an error: {data['findings']}")
        font = by_line.get(("assets/ui/fonts.css", line_of(FONTS_CSS, "gone.ttf")))
        check(font and font["code"] == "HTMLUI-GEN-0001", f"a missing font is an error in the css: {data['findings']}")
        svg = by_line.get(("assets/ui/fonts.css", line_of(FONTS_CSS, "nothere.svg")))
        check(svg and svg["code"] == "HTMLUI-GEN-0001", f"a missing svg is an error in the css: {data['findings']}")

        # a remote font: an error that says how to import it
        write(project, "assets/ui/remote.html", LLM_HTML.replace('<link rel="stylesheet" href="shop_llm.css">\n', ""))
        code, data = run_json(project, "--files", "assets/ui/remote.html")
        remote = [f for f in data["findings"] if f["code"] == "HTMLUI-IMP-0001" and f["line"] == line_of(LLM_HTML, "fonts.googleapis")]
        check(remote and remote[0]["severity"] == "error", f"a remote font link is an error: {data['findings']}")
        # (spec: the suggestion names import_files; as implemented, phase 2's text names the importers
        # the engine has today: the editor's automatic import and `import-ui`)
        check("import" in remote[0].get("suggestion", ""), f"the fix says how to import it: {remote[0]}")

        # malformed HTML and CSS: reported, no crash
        write(project, "assets/ui/malformed.html", MALFORMED_HTML)
        write(project, "assets/ui/malformed.css", MALFORMED_CSS)
        code, data = run_json(project, "--files", "assets/ui/malformed.html", "assets/ui/malformed.css")
        check_eq(data["checked"], {"ui": 2}, f"both checked: {data}")
        css = [f for f in data["findings"] if f["path"] == "assets/ui/malformed.css"]
        check(any(f["code"] == "HTMLUI-CSS-0002" for f in css), f"the css after a syntax error is still checked: {css}")

        # unicode file and directory names
        rel = "assets/ui/menú ü/ñandú.html"
        write(project, rel, "<html><body><img src=\"pájaro.png\"></body></html>\n")
        code, data = run_json(project, "--files", rel)
        check_eq([(f["path"], f["code"]) for f in data["findings"]], [(rel, "HTMLUI-GEN-0001")], f"unicode path: {data}")
        (project / "assets/ui/menú ü/pájaro.png").write_bytes(tiny_png())
        code, data = run_json(project, "--files", rel)
        check_eq((code, data["findings"]), (0, []), f"the unicode image exists now: {data}")
        r = run_check(project, "--ui")
        check("ñandú" not in r.stdout, f"the whole-project check reads unicode paths: {r.stdout}")


# ---------------------------------------------------------------------------- 5. dedupe

async def test_cli_ui_dedupe():
    with scratch_project() as project:
        write(project, "assets/ui/shared.css", ".a { mix-blend-mode: screen; }\n.b { zoom: 2; }\n")
        doc = '<html><head><link rel="stylesheet" href="shared.css"></head><body><div class="a b">{}</div></body></html>\n'
        write(project, "assets/ui/one.html", doc.format("one"))
        write(project, "assets/ui/two.html", doc.format("two"))
        code, data = run_json(project, "--ui")
        shared = [(f["line"], f["code"]) for f in data["findings"] if f["path"] == "assets/ui/shared.css"]
        check_eq(sorted(shared), [(1, "HTMLUI-CSS-0002"), (2, "HTMLUI-CSS-0002")], "each css finding once")
        check_eq(data["checked"], {"ui": 2}, "two documents, the shared css not on its own")
        code, data = run_json(project, "--files", "assets/ui/one.html", "assets/ui/two.html", "assets/ui/shared.css")
        check_eq(len([f for f in data["findings"] if f["path"] == "assets/ui/shared.css"]), 2, "--files dedupes too")
        check_eq(data["checked"], {"ui": 3}, "with --files the css named is checked on its own")


# ---------------------------------------------------------------------------- 5b. variables of a stylesheet (p4-gaps)

VARS_CSS = """\
.card { border: var(--edge); }
.typo { border: var(--edeg); }
"""
VARS_HTML = """\
<html><head><link rel="stylesheet" href="vars.css"></head>
<body><div class="card" style="--edge: 2px solid #ff0000">card</div><div class="typo">typo</div></body></html>
"""
MIXED_CSS = """\
:root { --line: 1px solid #000000; }
.alt { --line: 2px dashed #0000ff; }
.box { border: var(--line); }
"""


async def test_cli_ui_stylesheet_variables():
    """A stylesheet named with --files is checked with the documents that load it: a variable a
    style attribute defines is no finding, one no document defines (a typo) is. A sheet no document
    loads cannot know its documents' variables: it is not judged on the ones it does not declare,
    but still on the ones it does."""
    with scratch_project() as project:
        write(project, "assets/ui/vars.css", VARS_CSS)
        write(project, "assets/ui/vars.html", VARS_HTML)
        code, data = run_json(project, "--files", "assets/ui/vars.css")
        found = [(f["path"], f["line"], f["code"]) for f in data["findings"]]
        check_eq(found, [("assets/ui/vars.css", line_of(VARS_CSS, "--edeg"), "HTMLUI-CSS-0012")],
                 "only the typo, which no document defines")
        check_eq(data["checked"], {"ui": 1}, "the css named is the file checked")
        check_eq(code, 0, "warnings only: exit 0")
        # the document finds the same
        code, data = run_json(project, "--files", "assets/ui/vars.html")
        check_eq([(f["path"], f["line"], f["code"]) for f in data["findings"]],
                 [("assets/ui/vars.css", line_of(VARS_CSS, "--edeg"), "HTMLUI-CSS-0012")], "the document's check")
        # both named: once
        code, data = run_json(project, "--files", "assets/ui/vars.html", "assets/ui/vars.css")
        check_eq(len(data["findings"]), 1, f"deduped: {data['findings']}")
        # the whole project: the same
        code, data = run_json(project, "--ui")
        check_eq([(f["path"], f["line"]) for f in data["findings"] if f["code"] == "HTMLUI-CSS-0012"],
                 [("assets/ui/vars.css", line_of(VARS_CSS, "--edeg"))], "the whole project")

        # a second document defines the typo'd name too: no finding left
        write(project, "assets/ui/vars2.html", VARS_HTML.replace('class="typo"', 'class="typo" style="--edeg: 1px solid #000000"'))
        code, data = run_json(project, "--files", "assets/ui/vars.css")
        check_eq([(f["line"], f["code"]) for f in data["findings"]], [(line_of(VARS_CSS, "--edeg"), "HTMLUI-CSS-0012")],
                 "vars.html still loads it without --edeg")
        (project / "assets/ui/vars.html").unlink()
        code, data = run_json(project, "--files", "assets/ui/vars.css")
        check_eq(data["findings"], [], "every document that loads the sheet defines both")

        # no document loads it: the variables it does not declare are not judged (a document may define them) ...
        (project / "assets/ui/vars2.html").unlink()
        code, data = run_json(project, "--files", "assets/ui/vars.css")
        check_eq((code, data["findings"]), (0, []), "a lone sheet with undeclared variables: no false positive")
        code, data = run_json(project, "--ui")
        check_eq([f for f in data["findings"] if f["path"] == "assets/ui/vars.css"], [], "nor in the whole-project check")
        # ... the ones it declares are
        write(project, "assets/ui/mixed.css", MIXED_CSS)
        code, data = run_json(project, "--files", "assets/ui/mixed.css")
        check_eq([(f["line"], f["code"]) for f in data["findings"]], [(line_of(MIXED_CSS, ".box"), "HTMLUI-CSS-0012")],
                 "a variable the sheet declares with two values")


# ---------------------------------------------------------------------------- 6. hook

async def test_hook_ui_warnings_once():
    with scratch_project() as project:
        state = project / ".jasperengine" / "check-hook-state.json"
        # a first run takes the baseline (the fixture's files are old)
        r = run_check(project, "--hook", stdin=hook_input(project, tool="Read"))
        check_eq(r.returncode, 0, r.stderr)

        css = "assets/ui/shop_llm.css"
        touch_write(project / css, LLM_CSS)
        r = run_check(project, "--hook", stdin=hook_input(project, css))
        check_eq(r.returncode, 2, f"new UI warnings exit 2: {r.stdout}{r.stderr}")
        check_eq(r.stdout, "", "hook mode writes to stderr only")
        warned = [l for l in r.stderr.splitlines() if ": warning: " in l]
        check(warned and all(l.startswith(css + ":") for l in warned), f"the css warnings: {r.stderr}")
        check(all(TEXT_LINE.match(l) for l in warned), f"compiler style: {warned}")
        check(SHOWN_ONCE in r.stderr.splitlines()[-1], f"the shown-once line comes last: {r.stderr}")

        r = run_check(project, "--hook", stdin=hook_input(project, css))
        check_eq((r.returncode, r.stdout, r.stderr), (0, "", ""), "the same input again: silent")

        # a shell edit (no file in the hook input) adding one more unsupported declaration
        time.sleep(0.02)
        subprocess.run(["sed", "-i", "s/^.grid { gap: 16px; }$/.grid { gap: 16px; zoom: 2; }/", str(project / css)], check=True)
        r = run_check(project, "--hook", stdin=hook_input(project, tool="Bash", command="sed -i ..."))
        check_eq(r.returncode, 2, f"the new warning: {r.stderr}")
        warned = [l for l in r.stderr.splitlines() if ": warning: " in l]
        check_eq(len(warned), 1, f"only the new warning: {r.stderr}")
        check("'zoom'" in warned[0] and warned[0].startswith(f"{css}:{line_of(LLM_CSS, '.grid { gap')}:"), warned[0])

        # moving code does not show a warning again (the key has no line)
        touch_write(project / css, "/* moved */\n\n" + (project / css).read_text())
        r = run_check(project, "--hook", stdin=hook_input(project, css))
        check_eq((r.returncode, r.stderr), (0, ""), "moved lines: nothing new")

        # fixing it: silent; reintroducing it shows it again
        touch_write(project / css, (project / css).read_text().replace(" zoom: 2;", ""))
        r = run_check(project, "--hook", stdin=hook_input(project, css))
        check_eq((r.returncode, r.stderr), (0, ""), "fixed: silent")
        saved = json.loads(state.read_text())
        check(not any("'zoom'" in k for k in saved["ui_reported"].get(css, [])), f"the fixed warning dropped out: {saved}")
        touch_write(project / css, (project / css).read_text().replace(".grid { gap: 16px; }", ".grid { gap: 16px; zoom: 2; }"))
        r = run_check(project, "--hook", stdin=hook_input(project, css))
        check(r.returncode == 2 and "'zoom'" in r.stderr, f"a warning fixed and brought back shows again: {r.stderr}")

        # errors always show
        html = "assets/ui/shop_llm.html"
        touch_write(project / html, LLM_HTML)
        for attempt in range(2):
            r = run_check(project, "--hook", stdin=hook_input(project, html))
            check_eq(r.returncode, 2, f"run {attempt + 1}: errors exit 2 every time: {r.stderr}")
            check(f"{html}:{line_of(LLM_HTML, '<script')}:" in r.stderr and "HTMLUI-HTML-0006" in r.stderr,
                  f"run {attempt + 1}: the <script> error: {r.stderr}")
            warned = [l for l in r.stderr.splitlines() if ": warning: " in l]
            if attempt == 0:
                check(any("<canvas>" in l for l in warned), f"the canvas warning the first time: {r.stderr}")
            else:
                check_eq(warned, [], f"run {attempt + 1}: warnings once: {r.stderr}")
                check(SHOWN_ONCE not in r.stderr, f"no shown-once line without shown warnings: {r.stderr}")

        # Codex apply_patch adding an html
        other = "assets/ui/codex.html"
        write(project, other, LLM_HTML.replace("shop_llm.css", "codex.css"))
        patch = f"*** Begin Patch\n*** Add File: {other}\n+<html>\n*** End Patch"
        r = run_check(project, "--hook", stdin=json.dumps({"hook_event_name": "PostToolUse", "tool_name": "apply_patch",
                                                            "cwd": str(project), "tool_input": {"command": patch}}))
        check_eq(r.returncode, 2, f"Codex apply_patch: {r.stderr}")
        check(f"{other}:{line_of(LLM_HTML, '<script')}:" in r.stderr, r.stderr)
        link_line = line_of(LLM_HTML, 'href="shop_llm.css"')
        check(f"{other}:{link_line}:" in r.stderr and "codex.css" in r.stderr, f"its missing css: {r.stderr}")

        saved = json.loads(state.read_text())
        check_eq(saved["version"], 2, "state version")
        check(isinstance(saved.get("ui_reported"), dict) and css in saved["ui_reported"], f"ui_reported: {saved.get('ui_reported')}")
        check(all(isinstance(k, str) and "|" in k for k in saved["ui_reported"][css]), "keys are code|message")
        log = (project / ".jasperengine" / "check-hook.log").read_text().splitlines()
        check(any("0 errors, 1 warning exit 2" in l for l in log), f"the log counts shown UI warnings: {log[-6:]}")

        # a version-1 state file (before UI tracking) is accepted
        v1 = {"version": 1, "files": {k: v for k, v in saved["files"].items() if not k.startswith("assets/")}}
        state.write_text(json.dumps(v1))
        r = run_check(project, "--hook", stdin=hook_input(project, tool="Read"))
        # version 1 did not track UI files: they are new to it, so checked (and their warnings shown) once
        check_eq(r.returncode, 2, f"a version-1 state is read: {r.stderr}")
        check("check --hook:" not in r.stderr and "HTMLUI-HTML-0006" in r.stderr, f"read, not refused: {r.stderr}")
        saved = json.loads(state.read_text())
        check_eq(saved["version"], 2, "rewritten as version 2")
        r = run_check(project, "--hook", stdin=hook_input(project, tool="Read"))
        check_eq((r.returncode, r.stderr), (0, ""), f"after the upgrade nothing is new: {r.stderr}")


# ---------------------------------------------------------------------------- 7. speed


def example_trees() -> Path | None:
    """The example templates (p4-examples): the repo's source tree."""
    root = harness.REPO / "docs" / "mcp" / "guide" / "html-ui-examples"
    return root if root.is_dir() and any(root.rglob("*.html")) else None


async def test_cli_ui_speed():
    with scratch_project() as project:
        # many documents: 40 panels and 40 LLM shops, each with its own css
        for i in range(40):
            write(project, f"assets/ui/many/panel{i}.html", GOOD_HTML.replace("panel.css", f"panel{i}.css"))
            write(project, f"assets/ui/many/panel{i}.css", GOOD_CSS)
            write(project, f"assets/ui/many/shop{i}.html", LLM_HTML.replace("shop_llm.css", f"shop{i}.css"))
            write(project, f"assets/ui/many/shop{i}.css", LLM_CSS)
        t0 = time.monotonic()
        code, data = run_json(project, "--ui")
        seconds = time.monotonic() - t0
        print(f"     [timing] check --ui, 80 documents: {seconds:.2f}s")
        check_eq(data["checked"], {"ui": 80}, "80 documents")
        check(seconds < 2.0, f"check --ui on 80 documents took {seconds:.2f}s")
        check_eq(len({f["path"] for f in data["findings"]}), 80, "every LLM document and its css have findings")

    trees = example_trees()
    check(trees is not None, "the HTML UI examples (docs/mcp/guide/html-ui-examples) are missing")
    with scratch_project() as project:
        shutil.copytree(trees, project / "assets" / "html-ui-examples")
        t0 = time.monotonic()
        code, data = run_json(project, "--ui")
        seconds = time.monotonic() - t0
        print(f"     [timing] check --ui on the example templates: {seconds:.2f}s")
        check(seconds < 2.0, f"check --ui on the templates took {seconds:.2f}s")
        check_eq((data["errors"], data["warnings"]), (0, 0), f"the templates are clean: {data['findings']}")


# ---------------------------------------------------------------------------- 8. ui-support and the matrix

async def test_ui_support_schema_and_diagnostic_codes():
    data = ui_support()
    check_eq(data["version"], 1, "version")
    check(isinstance(data.get("editor_version"), str) and data["editor_version"], "editor_version")
    kinds = {"element", "attribute", "property", "value", "function", "unit", "selector", "pseudo_class",
             "pseudo_element", "at_rule", "media_feature", "input_type"}
    supports = {"native", "rewritten", "approximated", "emulated", "fallback", "unsupported", "ignored"}
    codes = {d["code"]: d for d in data["diagnostics"]}
    check(len(codes) == len(data["diagnostics"]), "diagnostics: unique codes")
    for d in data["diagnostics"]:
        check(re.fullmatch(r"HTMLUI-[A-Z]+-\d{4}", d["code"]) and d["name"] and d["severity"] in ("error", "warning", "info"),
              f"diagnostic entry: {d}")
    seen_kinds = set()
    for f in data["features"]:
        check(f["kind"] in kinds, f"kind: {f}")
        check(f["support"] in supports, f"support: {f}")
        seen_kinds.add(f["kind"])
        if f["support"] in ("unsupported", "fallback", "approximated"):
            check(f.get("code") in codes, f"a {f['support']} feature has a registered code: {f}")
        if f.get("code"):
            check(f.get("severity") in ("error", "warning", "info"), f"the severity it is reported with: {f}")
    check_eq(seen_kinds, kinds, "every table is there")
    # the rows the matrix promises
    check_eq(support_row(data, "property", "mix-blend-mode")["support"], "unsupported", "mix-blend-mode")
    check_eq(support_row(data, "at_rule", "@container")["support"], "unsupported", "@container")
    check_eq(support_row(data, "element", "canvas")["code"], "HTMLUI-HTML-0002", "canvas")
    check_eq(support_row(data, "attribute", "on*")["code"], "HTMLUI-HTML-0005", "on*")
    check(support_row(data, "value", "sticky", "position"), "values carry their property")
    # S17 (AC15): title emulated, draggable mapped to RmlUi's drag, the views and markdown engine elements
    def global_attribute(name):
        return next(f for f in data["features"] if f["kind"] == "attribute" and f["name"] == name and f.get("element") == "*")
    check_eq(global_attribute("title")["support"], "emulated", "title")
    draggable = global_attribute("draggable")
    check(draggable["support"] == "rewritten" and draggable["note"].startswith("mapped"), f"draggable: {draggable}")
    for tag in ("list-view", "grid-view", "tree-view", "table-view", "markdown"):
        check_eq(support_row(data, "element", tag)["support"], "emulated", tag)
    easing_rows = [support_row(data, "property", name) for name in (
        "animation", "transition", "animation-timing-function", "transition-timing-function")]
    easing_rows += [support_row(data, "function", name) for name in ("cubic-bezier", "steps", "linear")]
    for prop in ("animation-timing-function", "transition-timing-function"):
        easing_rows += [support_row(data, "value", name, prop) for name in (
            "linear", "ease", "ease-in", "ease-out", "ease-in-out", "step-start", "step-end", "cubic-bezier()", "steps()", "linear()")]
    for row in easing_rows:
        check(row is not None, "the native CSS easing row exists")
        check_eq(row["support"], "native", f"CSS easing: {row}")
        check("exact" in row["note"] and "nearest" not in row["note"], f"native easing describes its exact evaluation: {row}")
        check(not row.get("code"), f"native easing has no approximation diagnostic: {row}")



# ---------------------------------------------------------------------------- 9. parity CLI / editor

PARITY_SCENE: dict = {}


async def test_parity_cli_mcp(ed: Editor):
    ed.require_tools("create_scene", "create_entity", "set_properties", "eval_lua", "editor_status")
    await ed.call("editor_status")
    # the files written outside MCP: the next call's external_changes carries their problems
    # (no document uses them yet: ProjectChecks::checkUiFiles)
    write(ed.project, "assets/ui/shop_llm.css", LLM_CSS)
    write(ed.project, "assets/ui/shop_llm.html", LLM_HTML)
    r = await ed.call_raw("editor_status")
    ext = (r.structured or {}).get("external_changes")
    ed.check(ext is not None, f"the external write is reported: {r.text[-500:]}")
    external = [p for p in ext["problems"] if p.get("code")]
    for p in external:
        ed.check(set(p) >= {"path", "line", "col", "severity", "code", "message", "suggestion"}, f"full problem entries: {p}")

    with scratch_project(ed.project) as copy:
        code, data = run_json(copy, "--ui", "--files", "assets/ui/shop_llm.html", "assets/ui/shop_llm.css")
    cli = normalized(data["findings"])
    ed.check(cli, "the CLI finds the LLM mistakes")
    ed.check_eq(normalized(external), cli, "external_changes problems == CLI findings")

    # a live document loading the same html: its diagnostics (Info excluded)
    made = await ed.call("create_scene", name="UiChecksParity", type="UI")
    PARITY_SCENE["scene"] = made["scene"]["id"]
    ent = await ed.call("create_entity", scene=PARITY_SCENE["scene"], type="ui_document", name="Shop")
    await ed.call("set_properties", scene=PARITY_SCENE["scene"], entity=ent["entity_id"],
                  values={"uidocument": {"filename": "assets/ui/shop_llm.html"}})
    values = (await ed.call("eval_lua", allow_edit_mode=True, scene=PARITY_SCENE["scene"], code=(
        "local d = UIDocument(scene, entity('Shop')); local out = {} "
        "for _, x in ipairs(d:getDiagnostics()) do if x.severity ~= 'info' then "
        "out[#out + 1] = {x.file, x.line, x.column, x.severity, x.code} end end return out")))["values"]
    live = {("assets/" + f, line, col, sev, code) for f, line, col, sev, code in (values[0] or []) if f}
    # a live document also reports what only a running RmlUi sees (fonts it has not loaded, RmlUi's
    # own messages): compare the compile's findings
    runtime_only = {"HTMLUI-CSS-0023", "HTMLUI-GEN-0006"}
    ed.check_eq({x for x in live if x[4] not in runtime_only}, cli, "a live document's diagnostics == CLI findings")

    r = await ed.call("inspect_ui", file="assets/ui/shop_llm.html")
    ed.check_eq(normalized(r["diagnostics"]), cli, "inspect_ui {file} == CLI findings")
    r = await ed.call("inspect_ui", scene=PARITY_SCENE["scene"], entity="Shop", depth=0, limit=1)
    ed.check_eq({x for x in normalized(r["diagnostics"]) if x[0] and x[4] not in runtime_only}, cli,
                "inspect_ui of the live document == CLI findings")
