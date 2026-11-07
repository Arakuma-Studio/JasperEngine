"""Linked folders: one file, one GUID (docs/specs/S68-linked-folders.md R4-R8). A port of the sound
layer's cleanup group D (je-wt/snd-ro tools/mcp/tests/test_cleanup_group_d.py) to GUIDs in `.meta`: its
Sound Events became textures (a sidecar identity) beside its prefabs (a GUID in the file).

- a folder of assets renamed outside the editor with a link left at the old name is a move: bytes,
  GUIDs and references survive;
- a link to a project folder is one asset under two names: writes, references and instances by the
  alias path name the asset at its real path, each is listed once, a real copy made through the
  alias still gets a new GUID;
- a file link and a hard link are the file itself, a byte copy beside them is not;
- a link out of the project is a project folder: indexed under the link's name, metas beside the
  files, changes seen through it, cycles ended; a read-only target is reported, not a failure.

Oracles: files' bytes and their `guid:` lines read by Python, the metas, resolve_asset, list_prefabs,
get_prefab_info, get_entity, list_missing_assets. Runs in a copy of tools/fixtures/legacy_paths_project
(assets dir "assets", Lua dir "scripts").
"""

from __future__ import annotations

import asyncio
import hashlib
import json
import os
import shutil
import tempfile
from pathlib import Path

import pngdiff
import yaml

from harness import REPO, Editor, skip, symlinks_permitted

EDITOR = {"project": REPO / "tools" / "fixtures" / "legacy_paths_project"}
TIMEOUT = 1200
CI = True

GUID_D1 = "d1d1d1d1000011112222333344445555"


def png_bytes(red: int = 200) -> bytes:
    return pngdiff.encode(4, 4, bytes([red, 40, 40, 255]) * 16)


def guid_line(path: Path) -> str | None:
    for line in path.read_text().splitlines():
        if line.startswith("guid:"):
            return line.split(":", 1)[1].strip()
    return None


def meta_guid(path: Path) -> str:
    return yaml.safe_load(Path(str(path) + ".meta").read_text())["guid"]


def files(folder: Path, names) -> dict:
    return {name: hashlib.sha256((folder / name).read_bytes()).hexdigest() for name in names}


async def pickup(ed: Editor):
    """the database's look at the disk, now: a prefab or a link is not among what a call's auto-sync
    reports, so the poll would find them a moment later"""
    await ed.call("list_missing_assets")


async def reopen(ed: Editor):
    """the project opened again: the asset database scans it from its cache"""
    await ed.call("open_project", path=str(ed.project), unsaved="discard")


async def asset(ed: Editor, path: str) -> tuple:
    """(GUID, path) the database answers for a path"""
    found = await ed.call("resolve_asset", asset=path)
    return found.get("guid"), found.get("path")


async def listed_prefabs(ed: Editor, guid: str) -> list:
    return sorted(item["path"] for item in (await ed.call("list_prefabs", usage=False))["prefabs"] if item["guid"] == guid)


async def new_scene(ed: Editor, name: str) -> int:
    return (await ed.call("create_scene", name=name, type="3D", path=f"{name}.scene"))["scene"]["id"]


async def make_prefab(ed: Editor, sid: int, name: str, path: str) -> tuple:
    """(instance id, GUID) of a new prefab made of a box"""
    box = (await ed.call("create_entity", scene=sid, name=name, type="box"))["entity_id"]
    made = (await ed.call("create_prefab", scene=sid, entity=box, path=path))["prefabs"][0]
    ed.check_eq(made["path"], path, "the prefab's path")
    return box, made["guid"]


async def instance_prefab(ed: Editor, sid: int, entity) -> dict:
    return (await ed.call("get_prefab_info", scene=sid, entity=entity, usage=False))["prefab"]


async def light_with_mask(ed: Editor, sid: int, name: str, mask: str) -> int:
    light = (await ed.call("create_entity", scene=sid, name=name, type="empty"))["entity_id"]
    await ed.call("add_component", scene=sid, entity=light, component="light")
    await ed.call("set_properties", scene=sid, entity=light, values={"light": {"spotMask": mask}})
    return light


async def mask_of(ed: Editor, sid, light) -> str:
    got = await ed.call("get_entity", scene=sid, entity=light, components=["light"], properties=["spotMask"])
    return json.dumps(got["components"]["light"]["spotMask"])


def require_links():
    if not symlinks_permitted():
        skip("this process may not create symbolic links")


# -- a folder moved behind a link -------------------------------------------------------------------


async def test_a_folder_moved_behind_a_link_keeps_its_guids(ed: Editor):
    """assets/d1 (a texture and a prefab, both referenced by a saved scene) is renamed assets/d1_real
    outside the editor and assets/d1 becomes a link to it: the next call's pick-up finds both moved,
    writes nothing into their files, and their references read the real folder"""
    require_links()
    root = ed.project / "assets"
    (root / "d1").mkdir()
    (root / "d1" / "tile.png").write_bytes(png_bytes())
    # a GUID its path does not derive: a lookup by the old name must not find it by the legacy GUID
    (root / "d1" / "tile.png.meta").write_text(f"metaVersion: 1\nguid: {GUID_D1}\n")
    await pickup(ed)
    sid = await new_scene(ed, "Links D1")
    light = await light_with_mask(ed, sid, "Lamp", "assets/d1/tile.png")
    crate, prefab_guid = await make_prefab(ed, sid, "Crate", "assets/d1/Crate.prefab")
    await ed.call("save_scene", scene=sid)
    names = ("tile.png", "tile.png.meta", "Crate.prefab")
    before = files(root / "d1", names)
    tile_guid = meta_guid(root / "d1" / "tile.png")

    os.rename(root / "d1", root / "d1_real")
    os.symlink("d1_real", root / "d1")
    try:
        await pickup(ed)
        ed.check_eq(files(root / "d1_real", names), before, "the pick-up writes nothing into the moved files (no new GUID for a second path)")
        ed.check_eq(await asset(ed, "assets/d1_real/tile.png"), (tile_guid, "assets/d1_real/tile.png"), "the database has it at the real folder")
        ed.check_eq(await asset(ed, "assets/d1/tile.png"), (tile_guid, "assets/d1_real/tile.png"), "and a lookup by the link's name finds it")
        ed.check_in("d1_real/tile.png", await mask_of(ed, sid, light), "the reference follows")
        ed.check_eq(await listed_prefabs(ed, prefab_guid), ["assets/d1_real/Crate.prefab"], "the prefab is listed once")
        got = await instance_prefab(ed, sid, crate)
        ed.check_eq((got["path"], got["guid"]), ("assets/d1_real/Crate.prefab", prefab_guid), f"the instance follows: {got}")
        await ed.call("save_scene", scene=sid)

        await reopen(ed)
        ed.check_eq(files(root / "d1_real", names), before, "a reopen writes nothing into them either")
        ed.check_eq(await asset(ed, "assets/d1_real/tile.png"), (tile_guid, "assets/d1_real/tile.png"), "the GUID after a reopen")
        ed.check_eq(await listed_prefabs(ed, prefab_guid), ["assets/d1_real/Crate.prefab"], "listed once after a reopen")
        await ed.call("open_scene", scene="Links D1")
        ed.check_in("d1_real/tile.png", await mask_of(ed, "Links D1", "Lamp"), "the saved reference")
    finally:
        os.unlink(root / "d1")
        os.rename(root / "d1_real", root / "d1")
    await pickup(ed)
    ed.check_eq(files(root / "d1", names), before, "moved back: the same bytes")
    ed.check_eq(await asset(ed, "assets/d1/tile.png"), (tile_guid, "assets/d1/tile.png"), "moved back: the same GUID")


# -- a link to a project folder ---------------------------------------------------------------------


async def test_a_link_to_a_project_folder_is_one_asset_under_two_names(ed: Editor):
    """assets/d2-link links to assets/d2: the texture and the prefab there are one asset each, at their
    real path, whichever name a write, a reference or an instance uses; a real copy is still a copy"""
    require_links()
    root = ed.project / "assets"
    (root / "d2").mkdir()
    (root / "d2" / "tile.png").write_bytes(png_bytes())
    await pickup(ed)
    sid = await new_scene(ed, "Links D2")
    _, prefab_guid = await make_prefab(ed, sid, "Barrel", "assets/d2/Barrel.prefab")
    await ed.call("save_scene", scene=sid)
    prefab_file = files(root / "d2", ["Barrel.prefab"])
    tile_guid = meta_guid(root / "d2" / "tile.png")
    os.symlink("d2", root / "d2-link")
    try:
        await pickup(ed)
        ed.check_eq(meta_guid(root / "d2" / "tile.png"), tile_guid, "the link's pick-up leaves the GUID")
        ed.check(not (root / "d2-link" / "tile.png.meta").is_symlink(), "(the meta is the real one, through the link)")
        ed.check_eq(await asset(ed, "assets/d2-link/tile.png"), (tile_guid, "assets/d2/tile.png"), "a lookup by the alias")

        # a write through the alias: the same asset, and no second identity
        (root / "d2" / "notes.txt").write_text("first\n")
        await pickup(ed)
        notes = await asset(ed, "assets/d2/notes.txt")
        await ed.call("write_file", path="assets/d2-link/notes.txt", content="second\n")
        await pickup(ed)
        ed.check_eq((root / "d2" / "notes.txt").read_text(), "second\n", "written through the alias")
        ed.check_eq(await asset(ed, "assets/d2-link/notes.txt"), notes, "it keeps its GUID")

        # references and instances made by the alias path name the asset at its real path
        light = await light_with_mask(ed, sid, "Lamp", "assets/d2-link/tile.png")
        await ed.call("save_scene", scene=sid)
        saved = (ed.project / "Links D2.scene").read_text()
        ed.check_in(tile_guid, saved, "the saved reference names the texture by its GUID")
        ed.check_in("d2/tile.png", saved, "with the real path as its hint")
        ed.check("d2-link" not in saved, f"not the alias: {await mask_of(ed, sid, light)}")
        made = await ed.call("instantiate_prefab", scene=sid, prefab="assets/d2-link/Barrel.prefab", name="Barrel B")
        got = await instance_prefab(ed, sid, made["instance"]["id"])
        ed.check_eq((got["path"], got["guid"]), ("assets/d2/Barrel.prefab", prefab_guid), f"an instance by the alias: {got}")
        await ed.call("prefab_mode", action="open", prefab="assets/d2-link/Barrel.prefab", auto_save=False)
        stages = (await ed.call("prefab_mode", action="status"))["stages"]
        ed.check_eq([stage["prefab"] for stage in stages], ["assets/d2/Barrel.prefab"], f"Prefab Mode by the alias opens the asset: {stages}")
        await ed.call("prefab_mode", action="close", unsaved="discard")
        await ed.call("save_scene", scene=sid)
        await pickup(ed)
        ed.check_eq(files(root / "d2", ["Barrel.prefab"]), prefab_file, "the prefab file is not rewritten")
        ed.check_eq(await listed_prefabs(ed, prefab_guid), ["assets/d2/Barrel.prefab"], "the prefab is listed once")

        # a real copy, made through the alias, is a new asset
        shutil.copy(root / "d2/Barrel.prefab", root / "d2-link/Copy.prefab")
        shutil.copy(root / "d2/tile.png", root / "d2-link/tile2.png")
        shutil.copy(root / "d2/tile.png.meta", root / "d2-link/tile2.png.meta")
        await pickup(ed)
        copied = guid_line(root / "d2/Copy.prefab")
        ed.check(copied and copied != prefab_guid, f"the copy has a GUID of its own: {copied}")
        ed.check_eq(guid_line(root / "d2/Barrel.prefab"), prefab_guid, "the original keeps its GUID")
        ed.check_eq(await asset(ed, "assets/d2/Copy.prefab"), (copied, "assets/d2/Copy.prefab"), "the database has the copy's")
        ed.check(meta_guid(root / "d2/tile2.png") != tile_guid, "the copy of a texture and its meta too")
        ed.check_eq(meta_guid(root / "d2/tile.png"), tile_guid, "the original texture keeps its own")

        names = ("tile.png.meta", "tile2.png.meta", "Barrel.prefab", "Copy.prefab")
        held = files(root / "d2", names)
        await reopen(ed)
        ed.check_eq(files(root / "d2", names), held, "a reopen writes nothing")
        ed.check_eq(await asset(ed, "assets/d2/tile.png"), (tile_guid, "assets/d2/tile.png"), "the GUID after a reopen")
        ed.check_eq(await listed_prefabs(ed, prefab_guid), ["assets/d2/Barrel.prefab"], "the prefab once after a reopen")
    finally:
        os.unlink(root / "d2-link")
    await pickup(ed)
    ed.check_eq(await asset(ed, "assets/d2/tile.png"), (tile_guid, "assets/d2/tile.png"), "the link gone: nothing changed")


# -- file links and hard links ----------------------------------------------------------------------


async def test_file_links_and_hard_links_are_the_same_file(ed: Editor):
    """beside assets/d3/Keg.prefab: a symbolic link to it, a hard link to it and a copy of it; beside a
    texture, a hard link. The links are the file itself (no GUID written, the asset stays where it is,
    either name finds it); the copy gets a new GUID; both after a pick-up and after a reopen"""
    require_links()
    root = ed.project / "assets" / "d3"
    root.mkdir()
    (root / "tile.png").write_bytes(png_bytes())
    await pickup(ed)
    sid = await new_scene(ed, "Links D3")
    _, prefab_guid = await make_prefab(ed, sid, "Keg", "assets/d3/Keg.prefab")
    await ed.call("save_scene", scene=sid)
    prefab_file = files(root, ["Keg.prefab"])
    tile_guid = meta_guid(root / "tile.png")
    os.symlink("Keg.prefab", root / "KegSym.prefab")
    os.link(root / "Keg.prefab", root / "KegHard.prefab")
    shutil.copy(root / "Keg.prefab", root / "KegCopy.prefab")
    os.link(root / "tile.png", root / "hard.png")
    try:
        for when in ("after the pick-up", "after a reopen"):
            if when == "after a reopen":
                await reopen(ed)
                await ed.call("open_scene", scene="Links D3")
            else:
                await pickup(ed)
            ed.check_eq(files(root, ["Keg.prefab"]), prefab_file, f"{when}: the prefab's file is not rewritten")
            copied = guid_line(root / "KegCopy.prefab")
            ed.check(copied and copied != prefab_guid, f"{when}: the copy has a GUID of its own: {copied}")
            for name in ("Keg.prefab", "KegSym.prefab", "KegHard.prefab"):
                ed.check_eq(await asset(ed, f"assets/d3/{name}"), (prefab_guid, "assets/d3/Keg.prefab"), f"{when}: {name} names the prefab")
            ed.check_eq(await asset(ed, "assets/d3/KegCopy.prefab"), (copied, "assets/d3/KegCopy.prefab"), f"{when}: the copy is its own asset")
            prefabs = (await ed.call("list_prefabs", usage=False))["prefabs"]
            guids = {item["path"]: item["guid"] for item in prefabs if item["path"].startswith("assets/d3/") and item["guid"] != copied}
            ed.check_eq(guids, {"assets/d3/Keg.prefab": prefab_guid, "assets/d3/KegHard.prefab": prefab_guid},
                        f"{when}: the file link is the file, the hard link a file with the prefab's GUID")
            ed.check_eq(await asset(ed, "assets/d3/hard.png"), (tile_guid, "assets/d3/tile.png"), f"{when}: the texture's hard link")
            ed.check(not (root / "hard.png.meta").exists(), f"{when}: a hard link gets no meta of its own")
    finally:
        for name in ("KegSym.prefab", "KegHard.prefab", "hard.png"):
            os.unlink(root / name)
    await pickup(ed)
    ed.check_eq(files(root, ["Keg.prefab"]), prefab_file, "the links gone: the file as it was")
    ed.check_eq(await asset(ed, "assets/d3/Keg.prefab"), (prefab_guid, "assets/d3/Keg.prefab"), "and its GUID")


# -- a link out of the project ----------------------------------------------------------------------


async def test_a_root_swapped_for_a_link_is_not_scanned(ed: Editor):
    """test_root_attacks' swap with the database's look at the disk forced: there a scan lands in the
    window only on a loaded machine. The root is pinned, so the scan neither reads nor writes through it"""
    require_links()
    outside = Path(tempfile.mkdtemp(prefix="jasper-swapped-root-"))
    root = ed.project / "assets"
    aside = ed.project / "assets_swapped_aside"
    try:
        # the editor has the project open, roots pinned: else this is a root linked before the load
        await pickup(ed)
        (outside / "keep.txt").write_text("x")
        (outside / "tile.png").write_bytes(png_bytes())
        os.rename(root, aside)
        os.symlink(outside, root)
        await pickup(ed)
        ed.check_eq(sorted(os.listdir(outside)), ["keep.txt", "tile.png"], "nothing written through the swapped root")
        ed.check_eq((await asset(ed, "assets/tile.png"))[0], None, "and nothing of it is an asset")
        # a save does not pin the root again
        await ed.call_raw("save_project", {})
        r = await ed.call_raw("write_file", {"path": "assets/pwn.txt", "content": "pwned\n"})
        ed.check(r.is_error, f"a write into the swapped root after a save: {r.text[:300]}")
        ed.check_eq(sorted(os.listdir(outside)), ["keep.txt", "tile.png"], "still nothing written through it")
        out = ed.scratch.root / "export-swapped-root"
        r = await ed.call_raw("build_project", {"mode": "source", "output_dir": str(out), "platform": "linux", "backends": ["opengl"],
                                                "overwrite": True, "timeout_seconds": 300})
        ed.check(r.is_error or (r.structured or {}).get("status") == "failed", f"an export through a swapped root is refused: {r.text[:400]}")
        ed.check(not (out / "project" / "assets" / "keep.txt").exists(), "and ships nothing of what the root leads to now")
    finally:
        if root.is_symlink():
            root.unlink()
        if aside.exists():
            os.rename(aside, root)
        shutil.rmtree(outside, ignore_errors=True)
    await pickup(ed)
    r = await ed.call_raw("write_file", {"path": "assets/back.txt", "content": "ok\n"})
    ed.check(not r.is_error, f"the root back: writes work again: {r.text[:300]}")


async def test_an_export_stops_when_a_root_is_swapped_during_it(ed: Editor):
    """build_project's preflight sees a root swapped before it starts. Swapped once the export runs (held by
    JASPER_TEST_EXPORT_HOLD, the state the Build window and the CLI start in, which have no preflight), the game
    would ship whatever the root leads to now: the export fails and says why"""
    require_links()
    base = Path(tempfile.mkdtemp(prefix="jasper-export-swap-"))
    hold, held = base / "hold", base / "hold.held"
    outside = base / "outside"
    outside.mkdir()
    (outside / "keep.txt").write_text("secret\n")
    try:
        # a new project: this fixture's HTML document, missing once its root moves, would fail the export for its own reason
        async with Editor(create=True, name="Export Swap", label="export-swap", env={"JASPER_TEST_EXPORT_HOLD": str(hold)}) as swap:
            project = Path((await swap.call("get_project_summary"))["path"])   # (made by the editor: it is there now)
            root, aside = project / "assets", project / "assets_export_aside"
            root.mkdir(exist_ok=True)
            (project / "scripts").mkdir(exist_ok=True)
            await swap.call("set_project_settings", values={"editor.assets_dir": "assets", "scripting.lua_dir": "scripts"})
            await swap.call("save_project", untitled_scene_dir=".")
            await pickup(swap)
            out = swap.scratch.root / "export-swapped"
            hold.write_text("hold\n")
            job = await swap.call("build_project", mode="source", output_dir=str(out), platform="linux", backends=["opengl"],
                                  allow_missing_assets=True, overwrite=True, timeout_seconds=900, wait=False)
            job_id = job.get("job_id") or job.get("id")
            swap.check(job_id, f"build_project with wait false gives a job: {job}")
            for _ in range(6000):
                if held.exists():
                    break
                await asyncio.sleep(0.05)
            swap.check(held.exists(), "the export reached the hold")
            os.rename(root, aside)
            os.symlink(outside, root)
            hold.unlink()
            done = {}
            for _ in range(1800):
                done = await swap.call("get_job", job_id=job_id)
                if done.get("status") not in ("running", "queued", "pending"):
                    break
                await asyncio.sleep(0.25)
            root.unlink()
            os.rename(aside, root)
            swap.check(not (out / "project" / "assets" / "keep.txt").exists(), f"nothing of what the root leads to now ships: {str(done)[:400]}")
            swap.check_eq(done.get("status"), "failed", f"the export fails: {str(done)[:400]}")
            swap.check_in("no longer leads where it did", json.dumps(done), "and says why")
    finally:
        shutil.rmtree(base, ignore_errors=True)


async def test_a_root_that_was_a_link_when_the_project_opened_is_a_root(ed: Editor):
    """The assets root is linked in when the editor starts on the project: pinned so, it is the assets
    directory, with its data files (a .txt is an asset only there) and metas beside them, and a swap this is not"""
    require_links()
    base = Path(tempfile.mkdtemp(prefix="jasper-linked-root-"))
    # a sibling of the project's scratch folder: the old "../../name" assets directory held no file by
    # lexically_relative, while a deeper target ("../../a/b") held everything by accident
    outside = Path(tempfile.mkdtemp(prefix="jasper-linked-root-target-"))
    try:
        (outside / "keep.txt").write_text("x")
        (outside / "tile.png").write_bytes(png_bytes())
        source = base / "project"
        shutil.copytree(EDITOR["project"], source)
        shutil.rmtree(source / "assets")
        os.symlink(outside, source / "assets")
        async with Editor(project=source) as linked:
            await pickup(linked)
            ed.check_eq(sorted(os.listdir(outside)), ["keep.txt", "keep.txt.meta", "tile.png", "tile.png.meta"], "both are assets of the root")
            ed.check((await asset(linked, "assets/keep.txt"))[0], "the data file has its GUID")
            r = await linked.call_raw("build_project", {"mode": "source", "output_dir": str(linked.scratch.root / "export-linked-root"), "platform": "linux",
                                                        "backends": ["opengl"], "overwrite": True, "timeout_seconds": 300})
            ed.check("no longer leads where it did" not in r.text, f"a root linked at the load is not a swapped one: {r.text[:400]}")
    finally:
        shutil.rmtree(base, ignore_errors=True)
        shutil.rmtree(outside, ignore_errors=True)


async def test_a_link_beside_the_asset_roots_is_left_alone(ed: Editor):
    """`lib` at the project root leads out of it, and is under neither the assets nor the Lua root: what the
    editor scans and writes stops there, as a read of it still works"""
    require_links()
    outside = Path(tempfile.mkdtemp(prefix="jasper-linked-beside-"))
    try:
        (outside / "tile.png").write_bytes(png_bytes())
        (outside / "notes.txt").write_text("beside the roots\n")
        os.symlink(outside, ed.project / "lib")
        await pickup(ed)
        ed.check_eq(sorted(os.listdir(outside)), ["notes.txt", "tile.png"], "no meta beside a file of that folder")
        read = await ed.call_raw("read_file", {"path": "lib/notes.txt"})
        ed.check(not read.is_error and "beside the roots" in read.text, f"a read through it works: {read.text[:300]}")
        ed.check_eq(sorted(os.listdir(outside)), ["notes.txt", "tile.png"], "and writes nothing")
    finally:
        link = ed.project / "lib"
        if link.is_symlink():
            link.unlink()
        shutil.rmtree(outside, ignore_errors=True)


async def test_a_link_out_of_the_project_is_a_project_folder(ed: Editor):
    require_links()
    outside = Path(tempfile.mkdtemp(prefix="jasper-linked-library-"))
    assets = ed.project / "assets"
    try:
        (outside / "sub").mkdir()
        (outside / "tile.png").write_bytes(png_bytes())
        (outside / "sub" / "notes.txt").write_text("in a linked folder\n")
        os.symlink(outside, outside / "loop")
        os.symlink(ed.project, assets / "up")
        os.symlink(outside, assets / "lib")
        await pickup(ed)
        tile_guid = meta_guid(outside / "tile.png")
        ed.check_eq(await asset(ed, "assets/lib/tile.png"), (tile_guid, "assets/lib/tile.png"), "indexed by its project path, the meta beside the file")
        ed.check((outside / "sub" / "notes.txt.meta").is_file(), "data files too")
        ed.check_eq(await asset(ed, "assets/lib/loop/tile.png"), (tile_guid, "assets/lib/tile.png"), "a path through the cycle is the same asset")
        listed = await ed.call("list_assets", directory="assets/lib", recursive=True)
        paths = [e["path"] for e in listed["entries"]]
        ed.check_in("assets/lib/sub/notes.txt", paths, "listed as a folder of the project")
        ed.check(not any("/loop/" in p for p in paths), f"the cycle ends: {paths}")
        ed.check("assets/up" not in json.dumps(await ed.call("list_assets", directory="assets", type="texture", recursive=True)),
                 "a link to a folder holding the project is left out")

        sid = await new_scene(ed, "Links Out")
        light = await light_with_mask(ed, sid, "Lamp", "assets/lib/tile.png")
        await ed.call("save_scene", scene=sid)
        saved = (ed.project / "Links Out.scene").read_text()
        ed.check_in(tile_guid, saved, "referenced by GUID")
        ed.check_in("lib/tile.png", saved, "with its project path")
        ed.check(".." not in await mask_of(ed, sid, light), "never by where it leads")
        ed.check_eq((await ed.call("list_missing_assets"))["missing"], [], "nothing is missing")

        # the watcher and the pick-up see changes through the link
        (outside / "tile.png").write_bytes(png_bytes(120))
        st = (outside / "tile.png").stat()
        os.utime(outside / "tile.png", ns=(st.st_atime_ns, st.st_mtime_ns + 2_000_000_000))
        result = await ed.call_raw("editor_status")
        report = result.structured.get("external_changes") or {}
        ed.check_in("assets/lib/tile.png", json.dumps(report), f"seen through the link: {report}")

        # a read-only target is reported, not a failure (root writes through a read-only folder: a
        # folder at the meta's name makes the write fail the same way)
        (outside / "locked").mkdir()
        (outside / "locked" / "tile.png").write_bytes(png_bytes(90))
        as_root = os.geteuid() == 0
        if as_root:
            (outside / "locked" / "tile.png.meta" / "blocker").mkdir(parents=True)
        else:
            os.chmod(outside / "locked", 0o555)
        try:
            await pickup(ed)
            ed.check((await asset(ed, "assets/lib/locked/tile.png"))[0], "it has its GUID")
            ed.check(not (outside / "locked" / "tile.png.meta").is_file(), "no meta there")
            listing = await ed.call("list_missing_assets")
            ed.check_in("assets/lib/locked/tile.png", listing.get("read_only_linked", []), f"the Missing list says so: {listing}")
            shown = await ed.call_raw("list_missing_assets", {})
            ed.check_in("assets/lib/locked/tile.png", shown.text, "and its text names it: a client may show the model only the text")
            await pickup(ed)
        finally:
            os.chmod(outside / "locked", 0o755)
    finally:
        for link in (assets / "lib", assets / "up"):
            if link.is_symlink():
                link.unlink()
        shutil.rmtree(outside, ignore_errors=True)
    await pickup(ed)
