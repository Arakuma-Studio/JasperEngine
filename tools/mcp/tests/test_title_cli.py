"""S87 batch import and export from a shell (docs/specs/S87-title-localisation-interchange.md R62, AC26):
`import-title-art` of strings files offline (no editor process), `export-title-art` of the strings
formats offline and of a render format over one headless editor, `--json` lines, and the exit codes
0 (all done), 1 (some failed) and 2 (usage). The commands run on a copy of the test's project, so the
test editor's project lock is not in the way.
"""

import asyncio
import json
import shutil
import subprocess
import tempfile
from pathlib import Path

import harness
from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Title CLI"}
TIMEOUT = 900

DOC = "assets/titles/cli.titleart"


async def prepared(ed: Editor) -> Path:
    await ed.call("tool_groups", enable=["title_art"])
    await ed.call("set_project_settings", values={"title_art.auto_bake": False})
    await ed.call("edit_title", path=DOC, create={"text": "BIG WIN", "canvas": [640, 320]}, bake="never", ops=[
        {"op": "add_language", "code": "de", "source": "en"},
        {"op": "set_translation", "layer": "Title", "language": "de", "text": "GEWINN"},
    ])
    await ed.call("save_project")
    copy = Path(tempfile.mkdtemp(prefix="je_title_cli_")) / "project"
    shutil.copytree(ed.project, copy, ignore=shutil.ignore_patterns("*.lock", "lock", "build"))
    return copy


def run(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run(harness.editor_command() + list(args), capture_output=True, text=True, timeout=600)


async def test_cli_strings_offline(ed: Editor):
    project = await prepared(ed)
    out = project / "Recordings" / "strings"
    r = await asyncio.to_thread(run, "export-title-art", str(project / DOC), "--project", str(project), "--format", "csv",
                                "--out", str(out), "--json")
    ed.check_eq(r.returncode, 0, f"export ok: {r.stdout} {r.stderr}")
    line = json.loads(r.stdout.strip().splitlines()[-1])
    ed.check(line["ok"], f"--json prints the result: {line}")
    csv = (out / "cli.csv").read_text(encoding="utf-8-sig")
    ed.check_in("cli.title", csv, "the key")
    # translate and import it back, offline
    edited = csv.replace("GEWINN", "GROSSER GEWINN")
    strings = project / "Recordings" / "strings" / "edited.csv"
    strings.write_text(edited, encoding="utf-8-sig")
    r = await asyncio.to_thread(run, "import-title-art", str(strings), "--project", str(project), "--title", DOC, "--json")
    ed.check_eq(r.returncode, 0, f"import ok: {r.stdout} {r.stderr}")
    doc = json.loads((project / DOC).read_text())
    ed.check_eq(doc["layers"][0]["translations"]["de"]["text"], "GROSSER GEWINN", "the document took the translation")


async def test_cli_exit_codes(ed: Editor):
    project = await prepared(ed)
    r = await asyncio.to_thread(run, "export-title-art", str(project / DOC), "--project", str(project))
    ed.check_eq(r.returncode, 2, "no --format: usage")
    r = await asyncio.to_thread(run, "export-title-art", str(project / DOC), "--project", str(project), "--format", "gif")
    ed.check_eq(r.returncode, 2, "an unknown format: usage")
    r = await asyncio.to_thread(run, "import-title-art", str(project / "missing.svg"), "--project", str(project))
    ed.check_eq(r.returncode, 1, "a file that fails: 1, listed")
    ed.check_in("missing.svg", r.stderr, "the failed input is named")


async def test_cli_render_batch(ed: Editor):
    """A render format over one headless editor for the whole batch."""
    project = await prepared(ed)
    second = project / "assets" / "titles" / "cli2.titleart"
    shutil.copy(project / DOC, second)
    text = json.loads(second.read_text())
    text["guid"] = "0123456789abcdef0123456789abcdef"
    second.write_text(json.dumps(text))
    r = await asyncio.to_thread(run, "export-title-art", str(project / "assets" / "titles" / "cli*.titleart"), "--project", str(project),
                                "--format", "png", "--languages", "all", "--json")
    ed.check_eq(r.returncode, 0, f"export ok: {r.stdout[-2000:]} {r.stderr[-2000:]}")
    lines = [json.loads(l) for l in r.stdout.strip().splitlines() if l.startswith("{")]
    ed.check_eq(len(lines), 2, "one result per document")
    for name in ("cli", "cli2"):
        for code in ("en", "de"):
            ed.check((project / "Recordings" / "Title Art" / name / f"{name}.{code}.png").exists(), f"{name}.{code}.png written")
