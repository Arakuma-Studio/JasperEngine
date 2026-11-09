"""S87 KTX2 export (docs/specs/S87-title-localisation-interchange.md AC23, R43): export_title's ktx2 format
cooks the exported PNG as a UI texture for a platform family; `cook-textures --decode` reads it back and
it matches the PNG within the codec's tolerance; one file per language; bad options are refused.
"""

import subprocess
import tempfile
from pathlib import Path

import harness
import pngdiff
from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Title Ktx2"}
TIMEOUT = 900

DOC = "assets/titles/art.titleart"
KTX2_MAGIC = bytes([0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A])


S: dict = {}


async def setup(ed: Editor) -> None:
    if S.get("ed") is ed:
        return
    S["ed"] = ed
    await ed.call("tool_groups", enable=["title_art"])
    await ed.call("set_project_settings", values={"title_art.auto_bake": False})
    await ed.call("edit_title", path=DOC, create={"text": "WIN", "canvas": [512, 256]}, bake="never", ops=[
        {"op": "add_rim", "layer": "Title", "values": {"width": 6}},
        {"op": "add_language", "code": "de", "source": "en"},
        {"op": "set_translation", "layer": "Title", "language": "de", "text": "GEWINN"},
    ])


def decode(ktx2: Path) -> tuple:
    out = Path(tempfile.mkdtemp(prefix="je_title_ktx2_")) / "decoded.png"
    r = subprocess.run(harness.editor_command() + ["cook-textures", "--decode", str(ktx2), str(out)], capture_output=True, text=True, timeout=300)
    assert r.returncode == 0, r.stdout + r.stderr
    return pngdiff.load(out)


async def test_ktx2_matches_the_png(ed: Editor):
    await setup(ed)
    png = await ed.call("export_title", path=DOC, format="png", options={"overwrite": True, "round_to": 4})
    ed.check(png.get("ok"), f"the PNG: {png}")
    r = await ed.call("export_title", path=DOC, format="ktx2", options={"overwrite": True, "round_to": 4, "family": "desktop", "quality": "high"})
    ed.check(r.get("ok"), f"the KTX2: {r}")
    ed.check_eq(r["files"], ["Recordings/Title Art/art/art.ktx2"], "the R40 name")
    path = ed.project / r["files"][0]
    ed.check_eq(path.read_bytes()[:12], KTX2_MAGIC, "a KTX2 file")
    ok, fraction, _ = pngdiff.compare(decode(path), pngdiff.load(ed.project / png["files"][0]), channel_tolerance=32, max_fraction=0.02, premultiplied=True)
    ed.check(ok, f"decoded within the codec's tolerance: {fraction:.4f} of the pixels differ")


async def test_ktx2_per_language(ed: Editor):
    await setup(ed)
    r = await ed.call("export_title", path=DOC, format="ktx2", languages="all", options={"overwrite": True, "family": "desktop", "quality": "low"})
    ed.check(r.get("ok"), f"exported: {r}")
    ed.check_eq(sorted(r["files"]), ["Recordings/Title Art/art/art.de.ktx2", "Recordings/Title Art/art/art.en.ktx2"], "one file per language")
    sizes = {name: decode(ed.project / name)[:2] for name in r["files"]}
    ed.check(sizes[r["files"][0]] != sizes[r["files"][1]], f"each trimmed to its own text: {sizes}")


async def test_ktx2_refusals(ed: Editor):
    await setup(ed)
    for options, needle in (({"family": "console"}, "family"), ({"quality": "max"}, "quality"), ({"mipmaps": 2}, "mipmaps")):
        msg = await ed.check_error("export_title", path=DOC, format="ktx2", options=options)
        ed.check_in("bad_option", msg, f"{options}")
        ed.check_in(needle, msg, f"{options}")
