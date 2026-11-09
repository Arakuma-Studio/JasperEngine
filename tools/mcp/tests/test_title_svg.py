"""S87 SVG interchange through MCP (docs/specs/S87-title-localisation-interchange.md R34, R41, AC18):
edit_title create.from an SVG outside the project (the file copied into <stem>_assets/), the
import_svg op with split parts written beside the copy, and one undo removing every file the
import wrote.
"""

from pathlib import Path

from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Title SVG"}
TIMEOUT = 600

LOGO = """<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" width="160" height="96" viewBox="0 0 160 96">
  <g id="back"><rect x="8" y="8" width="96" height="48" fill="#ff8800"/></g>
  <g id="front"><circle cx="110" cy="50" r="30" fill="#2266ff"/><text x="10" y="90">Hi</text></g>
</svg>
"""


def outside(ed: Editor) -> Path:
    """A folder beside the scratch project: a file from outside the project."""
    folder = Path(ed.project).parent / "svg-sources"
    folder.mkdir(parents=True, exist_ok=True)
    return folder


async def setup(ed: Editor) -> None:
    await ed.call("tool_groups", enable=["title_art", "editor_tools"])
    await ed.call("set_project_settings", values={"title_art.auto_bake": False})


async def test_create_from_svg(ed: Editor):
    """create.from an SVG: a canvas of its viewport, one Vector layer, the file copied in."""
    await setup(ed)
    source = outside(ed) / "badge.svg"
    source.write_text(LOGO)
    r = await ed.call("edit_title", path="assets/titles/badge.titleart", create={"from": str(source)}, bake="never")
    ed.check_eq(r["import"]["kind"], "svg", "the kind is detected by content")
    ed.check_eq(r["import"]["files"], ["assets/titles/badge_assets/badge.svg"], "the copy is the import's one file")
    ed.check((ed.project / "assets/titles/badge_assets/badge.svg").read_text() == LOGO, "the copy is the original's bytes")
    got = await ed.call("get_title", path="assets/titles/badge.titleart", detail="full")
    ed.check_eq([got["canvas"]["width"], got["canvas"]["height"]], [160, 96], "the canvas is the SVG's viewport")
    ed.check_eq(len(got["layers"]), 1, "one layer")
    bad = await ed.call_raw("edit_title", {"path": "assets/titles/other.titleart", "create": {"from": str(source), "canvas": [64, 64]}})
    ed.check(bad.is_error and "bad_option" in bad.text, f"create.from takes no canvas: {bad.text[:200]}")


async def test_import_svg_split_and_undo(ed: Editor):
    """import_svg split groups: parts beside the copy, a group of layers, notes, one undo removes every file."""
    await setup(ed)
    await ed.call("edit_title", path="assets/titles/logo.titleart", create={"text": "Logo"}, bake="never")
    source = outside(ed) / "logo.svg"
    source.write_text(LOGO)
    r = await ed.call("edit_title", path="assets/titles/logo.titleart", bake="never",
                      ops=[{"op": "import_svg", "file": str(source), "split": "groups", "mode": "shape"}])
    written = [ed.project / "assets/titles/logo_assets" / name for name in ("logo.svg", "logo_back.svg", "logo_front.svg")]
    for f in written:
        ed.check(f.exists(), f"{f.name} written")
    got = await ed.call("get_title", path="assets/titles/logo.titleart", detail="full")
    names = [layer.get("path") for layer in got["layers"]]
    ed.check("logo/back" in names and "logo/front" in names, f"one layer per group under a group: {names}")
    notes = [p for p in r.get("problems", []) if p.get("code") == "import_note"]
    ed.check(any("text" in n.get("message", "") for n in notes), f"the <text> of the front part is noted: {notes}")
    await ed.call("undo", scope="project")
    for f in written:
        ed.check(not f.exists(), f"one undo removes {f.name}")
    got = await ed.call("get_title", path="assets/titles/logo.titleart")
    ed.check_eq(len(got["layers"]), 1, "and the layers")
