"""S87 Pachislop import through edit_title create.from (docs/specs/S87-title-localisation-interchange.md
R28-R31, R57, AC14, AC15): each fixture of editor/tests/titleart/fixtures/pachislop/ imports and renders
(a regression check, not a claim of Pachislop parity: Pachislop is never run), fonts and images are
extracted to <stem>_assets/, a second import writes nothing new, builtin fonts come from S84's font pack,
a WebP asset gives image_format_unsupported, one undo removes every file, a version 3 file gives
Pachislop's message and a version 12 file pachislop_version_newer.
"""

import json
import shutil
import tempfile
from pathlib import Path

from harness import Editor

CI = True
REPO = Path(__file__).resolve().parents[3]
FIXTURES = REPO / "editor" / "tests" / "titleart" / "fixtures" / "pachislop"
FONTS = REPO / "engine" / "tests" / "core" / "titleart" / "fonts"

# S84's font pack as the editor finds it (JASPERENGINE_TITLEART_LIBRARY_DIR): Pachislop's default
# builtin font, dela-gothic-one, with its OFL text
LIBRARY = Path(tempfile.mkdtemp(prefix="je_title_import_library_"))
(LIBRARY / "fonts" / "dela-gothic-one").mkdir(parents=True)
shutil.copy(FONTS / "DelaGothicOne-Regular.ttf", LIBRARY / "fonts" / "dela-gothic-one" / "DelaGothicOne-Regular.ttf")
shutil.copy(FONTS / "DelaGothicOne-OFL.txt", LIBRARY / "fonts" / "dela-gothic-one" / "OFL.txt")

EDITOR = {"create": True, "name": "Title Import", "env": {"JASPERENGINE_TITLEART_LIBRARY_DIR": str(LIBRARY)}}
TIMEOUT = 900

FIXTURE_NAMES = ["v4_text_rims", "v4_text_rims_v11", "v6_groups_masks_clip", "v7_content_effects", "v8_lighting", "v10_rings", "v11_all_kinds"]


async def setup(ed: Editor) -> None:
    await ed.call("tool_groups", enable=["title_art", "editor_tools"])
    await ed.call("set_project_settings", values={"title_art.auto_bake": False})


async def import_file(ed: Editor, path: str, source: Path) -> dict:
    return await ed.call("edit_title", path=path, create={"from": str(source)}, bake="never")


def files_under(root: Path) -> set[str]:
    # the asset importer's working files (".x.meta.<pid>.je-tmp") come and go on the main thread
    return {str(p.relative_to(root)) for p in root.rglob("*") if p.is_file() and p.suffix != ".meta" and not p.name.startswith(".")} if root.exists() else set()


async def test_fixtures_import_and_render(ed: Editor):
    """AC14: every fixture imports, reports its notes, keeps them in import_notes and renders."""
    await setup(ed)
    for name in FIXTURE_NAMES:
        path = f"assets/titles/{name}.titleart"
        made = await import_file(ed, path, FIXTURES / f"{name}.pslot.json")
        ed.check(made.get("created"), f"{name}: created: {made}")
        report = made.get("import", {})
        ed.check_eq(report.get("kind"), "pachislop", f"{name}: the kind is detected by content")
        doc = json.loads((ed.project / path).read_text())
        ed.check_eq(len(doc.get("import_notes", [])), len(report.get("notes", [])), f"{name}: the document keeps the report's lines")
        r = await ed.call_raw("render_title", {"path": path, "image": "none"})
        ed.check(not r.is_error, f"{name} renders: {r.text[:400]}")


async def test_assets_fonts_and_reimport(ed: Editor):
    """AC15: fonts and images extracted, the GIF as a PNG, the WebP refused with a note, builtin fonts
    from the pack (once per project), and a second import writes nothing new."""
    await setup(ed)
    made = await import_file(ed, "assets/kinds/kinds.titleart", FIXTURES / "v11_all_kinds.pslot.json")
    folder = ed.project / "assets" / "kinds" / "kinds_assets"
    ed.check_eq(files_under(folder), {"Oswald.ttf", "badge.svg", "grad.png", "flag.png"}, "the embedded font and images, the GIF as a PNG")
    codes = [n.get("code") for n in made["import"]["notes"]]
    ed.check("image_format_unsupported" in codes, f"the WebP asset is refused with a note: {codes}")
    ed.check("font_unavailable" in codes, f"a font neither embedded nor in the pack is noted: {codes}")
    again = await ed.call("edit_title", path="assets/kinds/kinds.titleart", create={"from": str(FIXTURES / "v11_all_kinds.pslot.json"), "overwrite": True},
                          bake="never")
    ed.check_eq(again["import"]["files"], [], "re-importing writes nothing new (SHA-256 reuse)")
    ed.check_eq(files_under(folder), {"Oswald.ttf", "badge.svg", "grad.png", "flag.png"}, "and no _2 copies appear")

    pack = await import_file(ed, "assets/pack/pack.titleart", FIXTURES / "v4_text_rims.pslot.json")
    # TitleArt/Fonts/<pack id>/ sits in the project's assets directory (wherever the project keeps it)
    fonts = [p for p in ed.project.rglob("DelaGothicOne-Regular.ttf") if p.parent.name == "dela-gothic-one" and p.parent.parent.name == "Fonts"]
    ed.check_eq(len(fonts), 1, f"the builtin font comes from the pack, once: {fonts}")
    ed.check(fonts and (fonts[0].parent / "OFL.txt").exists(), "with its OFL text")
    doc = json.loads((ed.project / "assets/pack/pack.titleart").read_text())
    ed.check_eq((doc["layers"][0].get("font") or {}).get("path"), "TitleArt/Fonts/dela-gothic-one/DelaGothicOne-Regular.ttf",
                "the layer names the pack font")
    second = await import_file(ed, "assets/pack/pack2.titleart", FIXTURES / "v4_text_rims.pslot.json")
    ed.check(not any("TitleArt/Fonts" in f for f in second["import"]["files"]), f"the pack font is imported once per project: {second['import']['files']}")
    ed.check(pack.get("created"), "created")


async def test_one_undo_removes_every_file(ed: Editor):
    """R57: "Import Pachislop Project (x.pslot.json)" is one undo step removing the document and its assets."""
    await setup(ed)
    path = "assets/undo/kinds.titleart"
    made = await import_file(ed, path, FIXTURES / "v11_all_kinds.pslot.json")
    ed.check_in("Import Pachislop Project (v11_all_kinds.pslot.json)", json.dumps(made.get("undo", {})), "the undo step's name")
    ed.check((ed.project / path).exists(), "the document exists")
    ed.check(files_under(ed.project / "assets/undo/kinds_assets"), "its assets exist")
    await ed.call("undo", scope="project")
    ed.check(not (ed.project / path).exists(), "undo removes the document")
    ed.check_eq(files_under(ed.project / "assets/undo/kinds_assets"), set(), "and every asset it wrote")


async def test_versions_refused(ed: Editor):
    """R28, AC15: a v3 file gives Pachislop's message, a v12 file pachislop_version_newer; nothing is written."""
    await setup(ed)
    scratch = Path(tempfile.mkdtemp(prefix="je_title_import_"))
    comp = {"width": 100, "height": 100}
    for version, expected in ((3, "This is a version 3 project; this version of the app opens version 4 to 11 projects only"),
                              (12, "pachislop_version_newer")):
        source = scratch / f"v{version}.pslot.json"
        source.write_text(json.dumps({"format": "pslot", "version": version, "project": {"version": version, "comp": comp, "layers": []}}))
        text = await ed.check_error("edit_title", path=f"assets/refused/v{version}.titleart", create={"from": str(source)})
        ed.check_in(expected, text, f"version {version}")
        ed.check(not (ed.project / f"assets/refused/v{version}.titleart").exists(), "nothing was written")
    shutil.rmtree(scratch, ignore_errors=True)
