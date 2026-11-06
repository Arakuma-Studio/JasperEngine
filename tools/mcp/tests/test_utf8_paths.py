"""Non-ASCII paths: the project lives in a non-ASCII directory, and files, folders, scenes,
prefabs, materials and textures get non-ASCII names. The tools take and return paths as UTF-8
(on Windows a std::string path is the ANSI code page, so they convert both ways); on Linux this
proves nothing regressed. Runs on a new project in a scratch directory whose name is not ASCII.
"""

from harness import Editor

EDITOR = {"create": True, "name": "Utf8Paths", "scratch_prefix": "jasper-mcp-ünïcødé-日本-"}
CI = True

FOLDER = "assets/dossier été"
TEXT = FOLDER + "/café ☕.txt"


async def test_project_path_is_utf8(ed: Editor):
    ed.require_tools("get_project_summary", "editor_status")
    summary = await ed.call("get_project_summary")
    ed.check_in("jasper-mcp-ünïcødé-日本-", summary["path"], "the project path comes back as UTF-8")
    ed.check_eq(summary["path"], str(ed.project), "get_project_summary path")
    status = await ed.call("editor_status")
    ed.check_eq(status["project"]["path"], str(ed.project), "editor_status project path")


async def test_files_and_folders(ed: Editor):
    ed.require_tools("create_folder", "write_file", "read_file", "list_assets", "search_files", "move_asset",
                     "get_asset_info", "delete_asset")
    if not (ed.project / "assets").is_dir():
        await ed.call("create_folder", path="assets")
    made = await ed.call("create_folder", path=FOLDER)
    ed.check_eq(made["path"], FOLDER)
    ed.check((ed.project / FOLDER).is_dir(), "the folder exists on disk under its UTF-8 name")

    written = await ed.call("write_file", path=TEXT, content="crème brûlée\nnaïve 🙂\n")
    ed.check_eq(written["path"], TEXT)
    ed.check_eq((ed.project / TEXT).read_text(encoding="utf-8"), "crème brûlée\nnaïve 🙂\n")
    read = await ed.call_raw("read_file", {"path": TEXT})
    ed.check(not read.is_error, read.text)
    ed.check_eq(read.structured["path"], TEXT)
    ed.check_in("brûlée", read.text)

    listed = await ed.call("list_assets", directory=FOLDER)
    ed.check_in(TEXT, [e["path"] for e in listed["entries"]], "list_assets path")
    by_name = await ed.call("list_assets", query="café")
    ed.check_in(TEXT, [e["path"] for e in by_name["entries"]], "list_assets query on a non-ASCII name")
    by_glob = await ed.call("list_assets", pattern=FOLDER + "/*.txt")
    ed.check_in(TEXT, [e["path"] for e in by_glob["entries"]], "list_assets pattern with a non-ASCII folder")

    found = await ed.call("search_files", pattern="brûlée", glob=FOLDER + "/*.txt")
    ed.check(any(m.get("path") == TEXT for m in found.get("matches", [])), f"search_files in a non-ASCII folder: {found}")
    found = await ed.call("search_files", pattern="naïve", glob="café ☕.txt")
    ed.check(any(m.get("path") == TEXT for m in found.get("matches", [])), f"search_files glob on a non-ASCII name: {found}")

    info = await ed.call("get_asset_info", path=TEXT)
    ed.check_eq(info["path"], TEXT)
    moved_to = FOLDER + "/thé vert.txt"
    await ed.call("move_asset", path=TEXT, to=moved_to)
    ed.check((ed.project / moved_to).is_file() and not (ed.project / TEXT).exists(), "moved under a non-ASCII name")
    await ed.call("delete_asset", paths=[moved_to])
    ed.check(not (ed.project / moved_to).exists(), "deleted")


async def test_scene_paths(ed: Editor):
    ed.require_tools("create_scene", "save_scene", "list_scenes", "close_scene", "open_scene")
    made = await ed.call("create_scene", name="Scène", type="3D")
    sid = made["scene"]["id"]
    saved = await ed.call("save_scene", scene=sid, path="scenes/niveau été")
    ed.check_eq(saved["scene"]["path"], "scenes/niveau été.scene")
    ed.check((ed.project / "scenes/niveau été.scene").is_file(), "scene file written under its UTF-8 name")
    scenes = {s["id"]: s for s in (await ed.call("list_scenes"))["scenes"]}
    ed.check_eq(scenes[sid]["path"], "scenes/niveau été.scene")
    await ed.call("close_scene", scene=sid, unsaved="discard")
    opened = await ed.call("open_scene", scene="scenes/niveau été.scene", close_previous=False)
    ed.check_eq(opened["scene"]["id"], sid, "open_scene by a non-ASCII path")
    reopened = await ed.call("open_scene", scene="scenes/niveau été.scene")
    ed.check_eq(reopened["scene"]["id"], sid, "an open scene by its non-ASCII path")


async def test_prefab_paths(ed: Editor):
    ed.require_tools("create_entity", "create_prefab", "list_prefabs", "instantiate_prefab", "get_prefab_info")
    made = await ed.call("create_scene", name="Préfabs", type="3D")
    sid = made["scene"]["id"]
    box = (await ed.call("create_entity", scene=sid, name="Boîte", type="box"))["entity_id"]
    prefab = "prefabs/boîte ü.prefab"
    created = await ed.call("create_prefab", scene=sid, entity=box, path=prefab)
    ed.check_eq(created["prefabs"][0]["path"], prefab)
    ed.check((ed.project / prefab).is_file(), "prefab written under its UTF-8 name")
    listed = await ed.call("list_prefabs", filter="boîte")
    ed.check_in(prefab, [p["path"] for p in listed["prefabs"]], "list_prefabs filter on a non-ASCII name")
    placed = await ed.call("instantiate_prefab", scene=sid, prefab=prefab)
    info = await ed.call("get_prefab_info", scene=sid, entity=placed["instance"]["id"])
    ed.check_eq(info["prefab"]["path"], prefab, "get_prefab_info of an instance")


async def test_material_and_texture_paths(ed: Editor):
    ed.require_tools("generate_placeholder_art", "edit_tilemap", "get_tilemap", "create_entity", "get_entity",
                     "create_material", "link_material")
    texture = "assets/sprites/pièce ☆.png"
    art = await ed.call("generate_placeholder_art", path=texture, width=16, height=16, shape="circle", color=[1, 0.8, 0, 1])
    ed.check_eq(art["path"], texture)
    made = await ed.call("create_scene", name="Matériaux", type="3D")
    sid = made["scene"]["id"]
    made = await ed.call("edit_tilemap", scene=sid, create={"name": "Carte"}, texture=texture,
                         tileset={"columns": 1, "rows": 1, "names": ["pièce"]}, cell_size=[16, 16],
                         ops=[{"op": "paint", "grid": "#", "legend": {"#": "pièce"}}])
    read = await ed.call("get_tilemap", scene=sid, entity=made["tilemap"]["id"])
    ed.check_eq(read["texture"], texture, "the texture path comes back as sent")

    material = "assets/matériaux/métal é.material"
    box = (await ed.call("create_entity", scene=sid, name="Cube", type="box"))["entity_id"]
    created = await ed.call("create_material", path=material, values={"baseColorTexture": texture})
    ed.check_eq(created["path"], material)
    ed.check((ed.project / material).is_file(), "material written under its UTF-8 name")
    linked = await ed.call("link_material", scene=sid, entity=box, material=material)
    ed.check_in(material, str(linked), "link_material names the material file")
    got = await ed.call("get_entity", scene=sid, entity=box, components="Mesh")
    ed.check_in(material, str(got), "the linked material file comes back as sent")


async def test_names_outside_every_ansi_code_page(ed: Editor):
    """Chinese and emoji names (outside every Windows ANSI code page): created, saved, listed,
    moved, and walked when the assets root moves (the walks that threw on Windows before the
    UTF-8 code page)."""
    ed.require_tools("create_folder", "write_file", "create_scene", "save_scene", "list_assets", "move_asset",
                     "set_project_settings", "get_project_settings")
    folder = "关卡 😀"
    await ed.call("create_folder", path=folder)
    await ed.call("write_file", path=folder + "/说明.txt", content="第一关\n")
    made = await ed.call("create_scene", name="第一关", type="3D")
    sid = made["scene"]["id"]
    saved = await ed.call("save_scene", scene=sid, path=folder + "/第一关")
    ed.check_eq(saved["scene"]["path"], folder + "/第一关.scene")
    listed = await ed.call("list_assets", directory=folder)
    ed.check_in(folder + "/说明.txt", [e["path"] for e in listed["entries"]], "list_assets under a CJK + emoji folder")
    await ed.call("move_asset", path=folder + "/说明.txt", to=folder + "/ゲーム.txt")
    ed.check((ed.project / folder / "ゲーム.txt").is_file(), "moved to a Japanese name")

    # the assets root moves under a Korean name: the change walks the whole project, these names
    # included (it moves the referenced files and rewrites the references)
    await ed.call("set_project_settings", values={"editor.assets_dir": "자산"})
    got = await ed.call("get_project_settings", keys=["editor.assets_dir"])
    ed.check_eq(got.get("settings", got)["editor.assets_dir"], "자산", "the assets root moved")
    await ed.call("set_project_settings", values={"editor.assets_dir": "."})
