"""Asset identity (docs/specs/S04-asset-identity.md): GUIDs for every asset, references by GUID,
renames that rewrite no referrer, deletes that keep references, redirects, path fallback, the
Missing Assets lists and the CLI.

The project is tools/fixtures/legacy_paths_project, saved before GUIDs: opening it writes the
metas and the guid lines (AC1 checks them byte for byte in the gtest suite). Main.scene opens,
Closed.scene stays closed. textures/mask.png is referenced by Closed.scene (Spot), prefabs/
Lamp.prefab (an instance of it is in Main.scene) and materials/Brick.material; ui/hud.html links
hud.css and textures/brick.png in its text.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

import yaml

import harness
from harness import Editor

REPO = Path(__file__).resolve().parents[3]
FIXTURE = REPO / "tools" / "fixtures" / "legacy_paths_project"
EDITOR = {"project": FIXTURE}
TIMEOUT = 900
CI = True

NEW_TOOLS = ("resolve_asset", "find_references", "get_asset_dependencies", "list_missing_assets",
             "retarget_asset_references", "clear_asset_references")


def meta_guid(ed: Editor, rel: str) -> str:
    return yaml.safe_load((ed.project / (rel + ".meta")).read_text())["guid"]


def mtimes(ed: Editor, files: list[str]) -> dict:
    return {f: (ed.project / f).stat().st_mtime_ns for f in files}


def missing_group(listing: dict, *, guid: str | None = None, last_path: str | None = None) -> dict | None:
    for group in listing["missing"]:
        if (guid and group["guid"] == guid) or (last_path and group["last_path"] == last_path):
            return group
    return None


def run_cli(*args: str, cwd: Path | None = None) -> subprocess.CompletedProcess:
    env = dict(os.environ)
    return subprocess.run(harness.editor_command() + list(args), capture_output=True, text=True, cwd=cwd, env=env, timeout=600)


async def test_tools_and_upgrade(ed: Editor):
    ed.require_tools(*NEW_TOOLS)
    await ed.call("editor_status")   # the project is open
    # every asset got an identity on open: metas beside the files, a guid line in scenes, prefabs and materials
    for rel in ["assets/textures/mask.png", "assets/models/ship.gltf", "assets/sounds/beep.wav", "assets/ui/hud.html",
                "scripts/Spinner.lua", "shaders/tint.frag"]:
        ed.check((ed.project / (rel + ".meta")).exists(), f"{rel} has no meta")
    for rel in ["Main.scene", "Closed.scene", "prefabs/Lamp.prefab", "materials/Brick.material"]:
        ed.check("\nguid: " in "\n" + (ed.project / rel).read_text(), f"{rel} has no guid line")
    mask = meta_guid(ed, "assets/textures/mask.png")
    info = await ed.call("resolve_asset", asset="assets/textures/mask.png")
    ed.check_eq(info["guid"], mask)
    ed.check_eq(info["resolved_by"], "live")
    by_guid = await ed.call("resolve_asset", asset=mask)
    ed.check_eq(by_guid["path"], "assets/textures/mask.png")
    ship = await ed.call("resolve_asset", asset="assets/models/ship.gltf")
    keys = {sub["key"] for sub in ship["sub_assets"]}
    ed.check({"mesh:Hull", "mesh:Mast", "mesh#2", "material:Paint"} <= keys, f"sub-assets: {keys}")
    sub = await ed.call("resolve_asset", asset="assets/models/ship.gltf#mesh:Hull")
    ed.check_eq(sub["resolved_by"], "sub_asset")
    # list_assets hides the metas and gives GUIDs
    listing = await ed.call("list_assets", directory="assets/textures")
    paths = [e["path"] for e in listing["entries"]]
    ed.check(not any(p.endswith(".meta") for p in paths), f"metas listed: {paths}")
    ed.check_eq({e["path"]: e.get("guid") for e in listing["entries"]}["assets/textures/mask.png"], mask)
    asset_info = await ed.call("get_asset_info", path=mask)
    ed.check_eq(asset_info["path"], "assets/textures/mask.png")


async def test_find_references_transitive(ed: Editor):
    """AC11: the material and the prefab reference the texture; transitively the scene using the prefab"""
    direct = await ed.call("find_references", asset="assets/textures/mask.png")
    owners = {(r["referrer"]["path"], r["locator"]) for r in direct["references"]}
    ed.check(("materials/Brick.material", "material/baseColorTexture") in owners, f"{owners}")
    ed.check(("Closed.scene", "entity:1001/LightComponent.spotMask") in owners, f"{owners}")
    ed.check(any(o[0] == "prefabs/Lamp.prefab" for o in owners), f"{owners}")
    spot = next(r for r in direct["references"] if r["referrer"]["path"] == "Closed.scene")
    ed.check_eq(spot["entity"]["path"], "Spot")
    ed.check_eq(spot["component"], "LightComponent")
    ed.check_eq(spot["field"], "spotMask")
    transitive = await ed.call("find_references", asset="assets/textures/mask.png", transitive=True)
    ed.check("Main.scene" in {r["referrer"]["path"] for r in transitive["references"]}, "the scene using the prefab is not listed")
    deps = await ed.call("get_asset_dependencies", asset="Closed.scene")
    ed.check({"assets/textures/mask.png", "assets/textures/brick.png", "assets/models/ship.gltf", "assets/ui/hud.html"}
             <= {d["target"]["path"] for d in deps["dependencies"]}, f"{deps}")


async def test_retarget_all_is_redirect(ed: Editor):
    """AC8: a deleted texture's references redirected to another: only its meta is written"""
    mask = meta_guid(ed, "assets/textures/mask.png")
    await ed.call("delete_asset", paths=["assets/textures/mask.png"])
    listing = await ed.call("list_missing_assets")
    group = missing_group(listing, guid=mask)
    ed.check(group is not None, f"mask is not missing: {listing}")
    ed.check({"Closed.scene", "prefabs/Lamp.prefab", "materials/Brick.material"} <= {r["owner"] for r in group["referrers"]}, f"{group}")
    ed.check_eq(group["expected_type"], "texture")
    before = mtimes(ed, ["Closed.scene", "prefabs/Lamp.prefab", "materials/Brick.material"])
    result = await ed.call("retarget_asset_references", **{"from": mask, "to": "assets/textures/rock.png", "mode": "redirect"})
    ed.check_eq(result["files_written"], ["assets/textures/rock.png.meta"])
    ed.check_eq(mtimes(ed, list(before)), before, "a referrer file was written")
    rock_meta = yaml.safe_load((ed.project / "assets/textures/rock.png.meta").read_text())
    ed.check_eq(rock_meta["formerGuids"], [mask])
    resolved = await ed.call("resolve_asset", asset=mask)
    ed.check_eq(resolved["resolved_by"], "former")
    ed.check_eq(resolved["path"], "assets/textures/rock.png")
    ed.check(missing_group(await ed.call("list_missing_assets"), guid=mask) is None, "still missing after the redirect")
    # undo: missing again; and the delete undone too
    await ed.call("undo", scope="project")
    ed.check(missing_group(await ed.call("list_missing_assets"), guid=mask) is not None, "the undo did not restore the missing state")
    await ed.call("undo", scope="project")
    ed.check((ed.project / "assets/textures/mask.png").exists() and (ed.project / "assets/textures/mask.png.meta").exists(), "the delete was not undone")
    ed.check(missing_group(await ed.call("list_missing_assets"), guid=mask) is None, "mask still missing after its delete was undone")


async def test_rename_touches_no_referrers(ed: Editor):
    """AC2: a texture used by a closed scene, an open scene, a prefab and a material is renamed"""
    mask = meta_guid(ed, "assets/textures/mask.png")
    # the open scene references it too, saved
    await ed.call("add_component", scene="Main.scene", entity="Speaker", component="light")
    await ed.call("set_properties", scene="Main.scene", entity="Speaker",
                  values={"light": {"spotMask": "assets/textures/mask.png"}})
    await ed.call("save_scene", scene="Main.scene")
    ed.check(mask in (ed.project / "Main.scene").read_text(), "the saved scene does not hold the texture's GUID")
    referrers = ["Main.scene", "Closed.scene", "prefabs/Lamp.prefab", "materials/Brick.material", "project.yaml"]
    before = mtimes(ed, referrers)
    result = await ed.call("move_asset", path="assets/textures/mask.png", to="assets/textures/moved/m2.png")
    ed.check_eq(sorted(result["files_written"]), ["assets/textures/moved/m2.png", "assets/textures/moved/m2.png.meta"])
    ed.check_eq(result["guid"], mask)
    ed.check_eq(mtimes(ed, referrers), before, "a referrer file was written")
    ed.check(not (ed.project / "assets/textures/mask.png.meta").exists(), "the meta stayed behind")
    # the open scene follows in memory
    entity = await ed.call("get_entity", scene="Main.scene", entity="Speaker", components=["light"], properties=["spotMask"])
    ed.check("assets/textures/moved/m2.png" in json.dumps(entity), f"{entity}")
    # the closed scene, opened, binds it by GUID
    await ed.call("open_scene", scene="Closed.scene")
    spot = await ed.call("get_entity", scene="Closed.scene", entity="Spot", components=["light"], properties=["spotMask"])
    ed.check("assets/textures/moved/m2.png" in json.dumps(spot), f"{spot}")
    ed.check_eq((await ed.call("resolve_asset", asset=mask))["resolved_by"], "live")
    await ed.call("close_scene", scene="Closed.scene")
    # back, for the tests after
    await ed.call("undo", scope="project")
    ed.check((ed.project / "assets/textures/mask.png").exists(), "the move was not undone")


async def test_text_references_broken(ed: Editor):
    """AC20: a stylesheet linked from an HTML document is moved: the link is reported, not rewritten"""
    html_before = (ed.project / "assets/ui/hud.html").read_bytes()
    result = await ed.call("move_asset", path="assets/ui/hud.css", to="assets/ui/hud-moved.css")
    broken = result["text_references_broken"]
    ed.check(any(b["referrer"] == "assets/ui/hud.html" and b["line"] == 4 for b in broken), f"{broken}")
    ed.check_eq((ed.project / "assets/ui/hud.html").read_bytes(), html_before, "the HTML was rewritten")
    await ed.call("undo", scope="project")


async def test_delete_keeps_references(ed: Editor):
    """AC10: deleting a model leaves the closed scene byte-identical; the undo resolves it again"""
    closed_before = (ed.project / "Closed.scene").read_bytes()
    deleted = await ed.call("delete_asset", paths=["assets/models/ship.gltf"])
    ed.check_eq(deleted["deleted"][0]["path"], "assets/models/ship.gltf")
    ed.check_eq((ed.project / "Closed.scene").read_bytes(), closed_before, "the closed scene changed")
    listing = await ed.call("list_missing_assets")
    group = missing_group(listing, last_path="assets/models/ship.gltf")
    ed.check(group is not None, f"{listing}")
    boat = [r for r in group["referrers"] if r["owner"] == "Closed.scene"]
    ed.check(boat and boat[0]["entity"]["path"] == "Boat" and boat[0]["component"] == "ModelComponent"
             and boat[0]["field"] == "filename", f"{group}")
    await ed.call("undo", scope="project")
    ed.check(missing_group(await ed.call("list_missing_assets"), last_path="assets/models/ship.gltf") is None,
             "the model is still missing after its delete was undone")


async def test_path_fallback_and_prefab_rule(ed: Editor):
    """AC9: an unknown GUID with a hint at an existing file binds by path, except for prefabs"""
    original = (ed.project / "Closed.scene").read_bytes()
    closed = yaml.safe_load(original)
    for entity in closed["entities"]:
        if entity["name"] == "Hud":
            entity["components"]["uidocument"]["filenameGuid"] = "0123456789abcdef0123456789abcdef"
        if entity["name"] == "Lantern":
            entity["components"]["prefab"] = {"name": "Lamp", "path": "prefabs/Lamp.prefab", "guid": "fedcba9876543210fedcba9876543210"}
    # changed outside the editor (write_file refuses scenes): the asset tools look at the disk first
    (ed.project / "Closed.scene").write_text(yaml.safe_dump(closed, sort_keys=False))
    listing = await ed.call("list_missing_assets")
    healed = [e for e in listing["resolved_by_path"] if e["path"] == "assets/ui/hud.html"]
    ed.check(healed and healed[0]["referenced_guid"] == "0123456789abcdef0123456789abcdef", f"{listing['resolved_by_path']}")
    ed.check(missing_group(listing, guid="fedcba9876543210fedcba9876543210") is not None, "a prefab bound by path")
    # Keep: the GUID becomes a former GUID of the file
    hud_meta = "assets/ui/hud.html.meta"
    await ed.call("retarget_asset_references", **{"from": "0123456789abcdef0123456789abcdef", "to": "assets/ui/hud.html"})
    ed.check("0123456789abcdef0123456789abcdef" in (ed.project / hud_meta).read_text(), "the meta has no former GUID")
    await ed.call("undo", scope="project")
    ed.check("0123456789abcdef0123456789abcdef" not in (ed.project / hud_meta).read_text(), "the undo kept the former GUID")
    (ed.project / "Closed.scene").write_bytes(original)
    await ed.call("list_missing_assets")


async def test_clear_is_undoable(ed: Editor):
    mask = meta_guid(ed, "assets/textures/mask.png")
    await ed.call("delete_asset", paths=["assets/textures/mask.png"])
    material_before = (ed.project / "materials/Brick.material").read_bytes()
    result = await ed.call("clear_asset_references", guid=mask, referrers=["materials/Brick.material|material/baseColorTexture"])
    ed.check_eq(result["files_written"], ["materials/Brick.material"])
    ed.check(mask not in (ed.project / "materials/Brick.material").read_text(), "the reference was not cleared")
    await ed.call("undo", scope="project")
    ed.check_eq((ed.project / "materials/Brick.material").read_bytes(), material_before, "the undo did not restore the bytes")
    await ed.call("undo", scope="project")


async def test_cli_check_and_migrate(ed: Editor):
    """AC14 and R32: assets migrate writes each referrer once, idempotent; assets check reports missing"""
    work = Path(tempfile.mkdtemp(prefix="jasper-assets-"))
    try:
        project = work / "project"
        shutil.copytree(FIXTURE, project)
        dump_before = work / "before.yaml"
        run_cli("scene-dump", "--project", str(project), "--out", str(dump_before))
        first = run_cli("assets", "migrate", "--project", str(project))
        ed.check(first.returncode == 0, first.stderr)
        written = set(json.loads(first.stdout)["files_written"])
        ed.check({"Main.scene", "Closed.scene", "prefabs/Lamp.prefab", "materials/Brick.material", "project.yaml"} <= written, f"{written}")
        ed.check("filenameGuid" in (project / "Closed.scene").read_text(), "the closed scene has no GUID references")
        second = run_cli("assets", "migrate", "--project", str(project))
        ed.check_eq(json.loads(second.stdout)["files_written"], [], "a second migrate wrote files")
        dump_after = work / "after.yaml"
        run_cli("scene-dump", "--project", str(project), "--out", str(dump_after))

        def strip(value):
            if isinstance(value, dict):
                return {k: strip(v) for k, v in value.items() if not (k == "guid" or k == "guids" or k.endswith("Guid"))}
            if isinstance(value, list):
                return [strip(v) for v in value]
            return value
        ed.check_eq(strip(yaml.safe_load(dump_after.read_text())), strip(yaml.safe_load(dump_before.read_text())),
                    "the scenes differ after migrate apart from GUID keys")

        check = run_cli("assets", "check", "--project", str(project))
        ed.check(check.returncode == 0, check.stdout + check.stderr)
        ed.check_eq(json.loads(check.stdout)["missing"], [])
        (project / "assets/sounds/beep.wav").unlink()
        check = run_cli("assets", "check", "--project", str(project))
        ed.check(check.returncode == 1, "assets check did not fail on a missing asset")
        ed.check(any(g["last_path"] == "assets/sounds/beep.wav" for g in json.loads(check.stdout)["missing"]), check.stdout)
    finally:
        shutil.rmtree(work, ignore_errors=True)


async def test_lua_asset_api_in_play(ed: Editor):
    """R31: the Asset module answers from the asset database in Play"""
    brick = meta_guid(ed, "assets/textures/brick.png")
    await ed.call("play", action="start", scene="Main.scene")
    path = await ed.call("eval_lua", code=f'return Asset.path("{brick}")')
    ed.check("textures/brick.png" in json.dumps(path), f"{path}")
    guid = await ed.call("eval_lua", code='return Asset.guid("textures/brick.png")')
    ed.check(brick in json.dumps(guid), f"{guid}")
    ref = await ed.call("eval_lua", code='local r = Asset.ref("textures/brick.png") return r.type .. " " .. tostring(r.valid)')
    ed.check("texture true" in json.dumps(ref), f"{ref}")
    await ed.call("play", action="stop")
