"""Changing the assets directory through set_project_settings
(docs/superpowers/specs/2026-11-04-project-window-design.md 6.9, Doriax 35c8e31da, c3aed7ee9):
a preflight refuses before anything moves (nothing changed, the reason said), dry_run reports
the plan and changes nothing, the change reports what moved, what was rewritten and the folders
it emptied. On tools/fixtures/textures (assets dir "assets")."""

from pathlib import Path

from harness import Editor

REPO = Path(__file__).resolve().parents[3]
EDITOR = {"project": REPO / "tools" / "fixtures" / "textures"}
CI = True


async def assets_dir(ed: Editor) -> str:
    return (await ed.call("get_project_settings", keys=["editor.assets_dir"]))["settings"]["editor.assets_dir"]


async def test_a_differing_destination_is_refused(ed: Editor):
    clash = ed.project / "content/textures/photo.png"
    clash.parent.mkdir(parents=True, exist_ok=True)
    clash.write_bytes(b"another photo")
    msg = await ed.check_error("set_project_settings", values={"editor.assets_dir": "content"}, contains="already exists and differs")
    ed.check_in("Nothing was changed", msg)
    ed.check_eq(await assets_dir(ed), "assets", "the root stayed")
    ed.check((ed.project / "assets/textures/ao.png").exists(), "nothing moved")
    ed.check_eq(clash.read_bytes(), b"another photo", "the other file is untouched")
    clash.unlink()


async def test_dry_run_changes_nothing(ed: Editor):
    plan = await ed.call("set_project_settings", values={"editor.assets_dir": "content2"}, dry_run=True)
    moved = {(m["from"], m["to"]) for m in plan["relocation"]["moved"]}
    ed.check(("assets/textures/ao.png", "content2/textures/ao.png") in moved, f"the plan: {moved}")
    ed.check_in("project.yaml", plan["relocation"]["rewritten"])
    ed.check(not (ed.project / "content2").exists(), "a dry run makes no folder")
    ed.check_eq(await assets_dir(ed), "assets")


async def test_the_change_reports_moves_and_removes_emptied_folders(ed: Editor):
    # a texture alone in its folder, referenced by a material
    solo = ed.project / "assets/solo/one.png"
    solo.parent.mkdir(parents=True, exist_ok=True)
    solo.write_bytes((ed.project / "assets/textures/sprite.png").read_bytes())
    await ed.call("write_file", path="materials/Solo.material",
                  content="name: materials/Solo.material\nbaseColorTexture: {textureType: 2D, source: path, path: solo/one.png}\n")
    await ed.call("sync_files", paths=["assets/solo/one.png", "materials/Solo.material"])
    done = await ed.call("set_project_settings", values={"editor.assets_dir": "content"})
    relocation = done["relocation"]
    ed.check_eq(await assets_dir(ed), "content")
    ed.check((ed.project / "content/solo/one.png").exists(), "the referenced file moved")
    ed.check(not (ed.project / "assets/solo").exists(), "the folder the move emptied is gone")
    ed.check_in("assets/solo", relocation["removed_folders"])
    ed.check_eq(relocation["failed_saves"], [])
    ed.check((ed.project / "assets/textures/gradient.png").exists(), "unreferenced files stay")
    ed.check_in("solo/one.png", (ed.project / "materials/Solo.material").read_text())
