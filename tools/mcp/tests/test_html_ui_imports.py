"""Automatic imports of remote resources (phase 2, p2-importers, spec §3.13.4), in the real editor.

A UI Document whose HTML links a remote stylesheet (that references a remote image), a remote
<img> and a URL that does not exist loads in edit mode; the editor's auto-import queue downloads
what the manifest lacks on its worker, through JASPERENGINE_IMPORT_MIRROR (no network: the
mirror is built here, the editor inherits the environment):
- "[HTML UI] importing <url>" lines and an HTMLUI-IMP-0004 note per import in the Console;
- assets/htmlui-imports/manifest.json lists both URLs, their files exist, the stylesheet's
  url() points at the local copy of the image;
- the missing URL is one HTMLUI-IMP-0002 error and is not retried automatically (a reload of the
  document does not download it again);
- the document reloads after the batch ("imported 2 remote resources" line).
Needs the HTML stage (p2-html: compat collects the references of an .html); skipped without it.
"""

import asyncio
import json
import os
import shutil
import tempfile
import time
import urllib.parse
import zlib
import struct
from pathlib import Path

from harness import Editor, skip

TIMEOUT = 600

REPO = Path(__file__).resolve().parents[3]
S: dict = {}

DOC = "assets/ui/imports.html"
STYLE_URL = "https://cdn.example.com/theme/style.css"
IMAGE_URL = "https://images.example.com/import-logo.png"
MISSING_URL = "https://images.example.com/missing.png"

HTML = f"""<!DOCTYPE html>
<html>
<head>
<link rel="preconnect" href="https://cdn.example.com">
<link rel="stylesheet" href="{STYLE_URL}">
</head>
<body>
<div class="panel">Imports</div>
<img src="{IMAGE_URL}" alt="logo">
<img src="{MISSING_URL}" alt="gone">
</body>
</html>
"""

STYLE = """.panel { width: 200px; height: 100px; background-image: url(../img/panel.png); }\n"""


def png(width: int, height: int, rgb: tuple) -> bytes:
    raw = b"".join(b"\x00" + bytes(rgb) * width for _ in range(height))

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def mirror_file(root: Path, url: str, content: bytes) -> None:
    parts = urllib.parse.urlsplit(url)
    path_and_query = parts.path + ("?" + parts.query if parts.query else "")
    target = root / parts.hostname / urllib.parse.quote(path_and_query, safe="")
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(content)


# the mirror, set before the Editor starts (the harness copies os.environ)
MIRROR = Path(tempfile.mkdtemp(prefix="jasper-import-mirror-"))
mirror_file(MIRROR, STYLE_URL, STYLE.encode())
mirror_file(MIRROR, "https://cdn.example.com/theme/img/panel.png", png(4, 4, (255, 0, 0)))
mirror_file(MIRROR, IMAGE_URL, png(4, 4, (0, 0, 255)))
os.environ["JASPERENGINE_IMPORT_MIRROR"] = str(MIRROR)


def project_dir(ed: Editor) -> Path:
    return ed.scratch.root / "project"


def manifest(ed: Editor) -> dict:
    path = project_dir(ed) / "assets" / "htmlui-imports" / "manifest.json"
    if not path.is_file():
        return {}
    return json.loads(path.read_text())


async def messages(ed: Editor) -> list:
    return [e.get("message", "") for e in await ed.console_since(None, limit=5000)]


async def wait_messages(ed: Editor, needle: str, seconds: float = 30.0) -> list:
    deadline = time.monotonic() + seconds
    while True:
        found = [m for m in await messages(ed) if needle in m]
        if found:
            return found
        if time.monotonic() > deadline:
            ed.fail(f"no Console line with {needle!r} after {seconds} s")
        await asyncio.sleep(0.25)


async def test_setup(ed: Editor):
    if not (REPO / "engine/core/htmlui/compat/DocumentLoader.h").is_file():
        skip("the HTML stage (p2-html) is not in this checkout: compat collects no references of an .html")
    await ed.call("write_file", path=DOC, content=HTML)
    made = await ed.call("create_scene", name="HtmlUiImports", type="UI")
    S["scene"] = made["scene"]["id"]
    ent = await ed.call("create_entity", scene=S["scene"], type="ui_document", name="Imports")
    S["entity"] = ent["entity_id"]
    await ed.call("set_properties", scene=S["scene"], entity=S["entity"], values={"uidocument": {"filename": DOC}})
    await ed.call("save_scene", scene=S["scene"], path="scenes/HtmlUiImports")


async def test_auto_import(ed: Editor):
    if "scene" not in S:
        skip("setup skipped")
    await wait_messages(ed, "imported 2 remote resources", 60.0)
    lines = await messages(ed)
    for url in (STYLE_URL, IMAGE_URL):
        ed.check(any(f"[HTML UI] importing {url}" in m for m in lines), f"no 'importing {url}' line")
    ed.check(sum("[HTMLUI-IMP-0004]" in m for m in lines) >= 2, "an HTMLUI-IMP-0004 note per import")
    failed = [m for m in lines if "[HTMLUI-IMP-0002]" in m and MISSING_URL in m]
    ed.check_eq(len(failed), 1, "one download error for the missing URL")
    ed.check("ui/imports.html:" in failed[0], f"the error points at the document: {failed[0]}")
    # preconnect hints are no resources
    ed.check(not any(m.endswith("importing https://cdn.example.com") for m in lines), "a preconnect hint was imported")

    entries = {e["url"]: e for e in manifest(ed).get("entries", [])}
    for url in (STYLE_URL, IMAGE_URL):
        ed.check_in(url, entries, "manifest entry")
        for file in [entries[url]["path"]] + entries[url]["files"]:
            ed.check((project_dir(ed) / "assets" / file).is_file(), f"{file} exists")
    ed.check(MISSING_URL not in entries, "nothing recorded for the failed URL")
    css = (project_dir(ed) / "assets" / entries[STYLE_URL]["path"]).read_text()
    ed.check("url(\"../img/panel.png\")" in css, f"the stylesheet's image is local: {css}")


async def test_failed_url_not_retried(ed: Editor):
    if "scene" not in S:
        skip("setup skipped")
    before = sum(f"importing {MISSING_URL}" in m for m in await messages(ed))
    # a reload of the document (the file changes): no automatic retry of the failed URL
    await ed.call("write_file", path=DOC, content=HTML.replace("Imports</div>", "Imports again</div>"))
    await asyncio.sleep(3.0)
    after = sum(f"importing {MISSING_URL}" in m for m in await messages(ed))
    ed.check_eq(after, before, "the failed URL is not retried in the same session")


async def test_inspector_panel(ed: Editor):
    """The Inspector's Remote resources section (H10); the screenshot is for a human to look at."""
    if "scene" not in S:
        skip("setup skipped")
    await ed.call("select_scene", scene=S["scene"])
    await ed.call("select_entities", scene=S["scene"], entity=S["entity"])
    await ed.call("focus_window", window="inspector")
    await asyncio.sleep(1.0)
    shot = await ed.call_raw("screenshot", {"view": "editor", "max_width": 1600})
    ed.check(not shot.is_error and shot.images, f"editor screenshot: {shot.text[:300]}")
    out = ed.scratch.root / "html-ui-imports-inspector.png"
    out.write_bytes(shot.images[0][0])
    print(f"inspector screenshot: {out}")


async def test_cleanup():
    shutil.rmtree(MIRROR, ignore_errors=True)
