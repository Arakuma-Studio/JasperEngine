# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""Scene integrity (docs/superpowers/specs/2026-11-04-scene-integrity-design.md): copied scenes get
their own id and name, child scenes are edited on open scenes only, untitled scenes are listed by
Save All, move_entity refuses what would only reorder the list, a child scene shown inline beside
a running game draws, and the Scene view follows a scene the game loads in place of the played one.

The headless Project tests (editor/tests/project/SceneIntegrityTest.cpp) own the file rules: save
over another scene, scenes outside the project, project.yaml entries, id clashes at load.
"""

import asyncio
import re

from harness import Editor

EDITOR = {"create": True}
CI = True


async def dialog(ed: Editor):
    return (await ed.call("editor_status")).get("dialog")


async def wait_dialog(ed: Editor, title: str, seconds: float = 20.0):
    for _ in range(int(seconds / 0.25)):
        d = await dialog(ed)
        if d and d.get("title") == title:
            return d
        await asyncio.sleep(0.25)
    return None


def root_value(text: str, key: str) -> str:
    match = re.search(rf"^{key}:\s*(.*)$", text, re.M)
    return match.group(1).strip().strip('"') if match else ""


async def scenes_by_name(ed: Editor) -> dict:
    return {s["name"]: s for s in (await ed.call("list_scenes"))["scenes"]}


async def test_copied_scene_gets_its_own_id_and_name(ed: Editor):
    level = (await ed.call("create_scene", name="Level", type="3D", path="scenes/Level"))["scene"]["id"]
    original = (ed.project / "scenes" / "Level.scene").read_text()

    copied = await ed.call("move_asset", path="scenes/Level.scene", to="scenes/Level Copy.scene", copy=True)
    copy_file = ed.project / "scenes" / "Level Copy.scene"
    text = copy_file.read_text()
    copy_id = int(root_value(text, "id"))
    ed.check(copy_id != level, f"the copy has its own scene id: {copy_id} vs {level}")
    ed.check_eq(root_value(text, "name"), "Level Copy", "the name followed the file")
    ed.check_eq(copied["copied_scenes"], [{"path": "scenes/Level Copy.scene", "scene_id": copy_id, "name": "Level Copy"}])
    ed.check_eq(root_value(original, "id"), str(level), "the original keeps its id")

    # the copy opens as a scene of its own, beside the original
    opened = await ed.call("open_scene", scene="scenes/Level Copy.scene")
    ed.check_eq(opened["scene"]["id"], copy_id)
    listed = await scenes_by_name(ed)
    ed.check(listed["Level"]["id"] != listed["Level Copy"]["id"], f"two ids: {listed}")

    # a copy of a folder: each scene in it gets a new id
    await ed.call("create_scene", name="Hud", type="UI", path="pack/Hud")
    await ed.call("move_asset", path="pack", to="pack2", copy=True)
    hud_ids = {root_value((ed.project / p / "Hud.scene").read_text(), "id") for p in ("pack", "pack2")}
    ed.check_eq(len(hud_ids), 2, f"the folder copy's scene has its own id: {hud_ids}")


async def test_child_scenes_are_edited_on_open_scenes(ed: Editor):
    parent = (await ed.call("create_scene", name="Closed", type="3D", path="scenes/Closed"))["scene"]["id"]
    child = (await ed.call("create_scene", name="Part", type="3D", path="scenes/Part"))["scene"]["id"]
    await ed.call("close_scene", scene=parent)
    before = (ed.project / "scenes" / "Closed.scene").read_text()

    await ed.check_error("set_child_scene", scene=parent, child=child, contains="open_scene")
    ed.check_eq((ed.project / "scenes" / "Closed.scene").read_text(), before, "the closed scene's file is unchanged")
    listed = await scenes_by_name(ed)
    ed.check(not listed["Closed"].get("child_scenes"), f"nothing attached: {listed['Closed']}")

    await ed.call("open_scene", scene="scenes/Closed.scene")
    await ed.call("set_child_scene", scene=parent, child=child)
    listed = await scenes_by_name(ed)
    ed.check_eq([c["id"] for c in listed["Closed"]["child_scenes"]], [child], "attached once open")


async def test_save_all_lists_untitled_scenes(ed: Editor):
    await ed.call("create_scene", name="Saved", type="3D", path="scenes/Saved")
    untitled = (await ed.call("create_scene", name="Untitled Draft", type="3D"))["scene"]["id"]
    saved = await ed.call_raw("save_scene", {"all": True})
    skipped = [s["id"] for s in saved.structured.get("skipped", [])]
    ed.check(untitled in skipped, f"an untouched untitled scene is listed as skipped: {saved.text}")


async def test_move_entity_next_to_a_skybox_is_refused(ed: Editor):
    scene = (await ed.call("create_scene", name="Sky", type="3D", path="scenes/Sky"))["scene"]["id"]
    sky = (await ed.call("create_entity", scene=scene, type="skybox", name="Sky"))["entity_id"]
    await ed.call("create_entity", scene=scene, type="cube", name="First")
    cube = (await ed.call("create_entity", scene=scene, type="cube", name="Box"))["entity_id"]
    await ed.call("save_scene", scene=scene)
    before = (ed.project / "scenes" / "Sky.scene").read_text()

    await ed.check_error("move_entity", scene=scene, entity=cube, after=sky, contains="parent")
    await ed.check_error("move_entity", scene=scene, entity=cube, before=sky, contains="Transform")
    await ed.call("save_scene", scene=scene)
    ed.check_eq((ed.project / "scenes" / "Sky.scene").read_text(), before, "nothing moved")

    # among the GameObjects with a Transform, by index
    await ed.call("move_entity", scene=scene, entity=cube, parent=None, index=0)
    tree = await ed.call("get_scene_tree", scene=scene)
    names = [n["name"] for n in tree["roots"] if n["name"] in ("First", "Box")]
    ed.check_eq(names, ["Box", "First"], f"placed first: {tree['roots']}")
    # a Skybox still moves among the others (it keeps its gap)
    await ed.call("move_entity", scene=scene, entity=sky, after=cube)


async def test_inline_child_beside_a_running_game(ed: Editor):
    """A child scene shown inline in a stopped scene's view while another scene plays was updated
    with no camera: its UI polygon's flip threw and the editor terminated."""
    host = (await ed.call("create_scene", name="Host", type="3D", path="scenes/Host"))["scene"]["id"]
    hud = (await ed.call("create_scene", name="Overlay", type="UI", path="scenes/Overlay"))["scene"]["id"]
    await ed.call("create_entity", scene=hud, type="polygon", name="Shape")
    await ed.call("save_scene", scene=hud)
    await ed.call("set_child_scene", scene=host, child=hud)
    await ed.call("save_scene", scene=host)
    await ed.call("close_scene", scene=hud)
    game = (await ed.call("create_scene", name="Game", type="3D", path="scenes/Game"))["scene"]["id"]
    await ed.call("create_entity", scene=game, type="camera", name="Cam", position=[0, 0, 5])
    await ed.call("set_main_camera", scene=game, entity="Cam")
    await ed.call("save_scene", scene=game)

    started = await ed.call("play", action="start", scene=game)
    ed.check_eq(started["state"], "playing")
    try:
        # the Host tab shown beside the game, its child loaded inline (the Hierarchy's Show Contents)
        await ed.call("select_scene", scene=host)
        await ed.call("set_child_scene", scene=host, child=hud, loaded_inline=True)
        await ed.call("wait", frames=20)
        state = await ed.call("get_play_state")
        ed.check_eq(state["state"], "playing", "the editor and the game run on")
        await ed.check_error("set_child_scene", scene=host, child=game, contains="during Play")
    finally:
        await ed.call("play", action="stop")


LOADER = """local loader = {}
function loader:init()
    RegisterEngineEvent(self, "onUpdate")
end
function loader:onUpdate()
    if not self.done then
        self.done = true
        SceneManager.loadScene("Arena")
    end
end
return loader
"""


async def test_scene_view_follows_the_scene_the_game_loads(ed: Editor):
    menu = (await ed.call("create_scene", name="Menu", type="3D", path="scenes/Menu"))["scene"]["id"]
    await ed.call("create_entity", scene=menu, type="camera", name="MenuCam", position=[0, 0, 5])
    await ed.call("set_main_camera", scene=menu, entity="MenuCam")
    loader = (await ed.call("create_entity", scene=menu, type="empty", name="Loader"))["entity_id"]
    await ed.call("create_script", kind="lua", name="Loader", content=LOADER, entity=loader, scene=menu)
    await ed.call("save_scene", scene=menu)
    arena = (await ed.call("create_scene", name="Arena", type="3D", path="scenes/Arena"))["scene"]["id"]
    await ed.call("create_entity", scene=arena, type="camera", name="ArenaCam", position=[0, 20, 40])
    await ed.call("set_main_camera", scene=arena, entity="ArenaCam")
    await ed.call("create_entity", scene=arena, type="cube", name="Pillar", position=[30, 0, 0], scale=[2, 2, 2])
    await ed.call("save_scene", scene=arena)
    await ed.call("select_scene", scene=menu)

    started = await ed.call("play", action="start", scene=menu)
    ed.check_eq(started["state"], "playing")
    try:
        await ed.call("wait", frames=6)
        state = await ed.call("get_play_state")
        shown = state["scene_view_scene_id"]
        ed.check(shown != menu, f"the Scene view draws the loaded scene: {state['running_scenes']}")
        # the editor camera looks at the Arena's pillar: a click there picks it
        await ed.call("set_scene_camera", scene=menu, position=[30, 0, 8], target=[30, 0, 0])
        await ed.call("wait", frames=2)
        got = await ed.call("pick_at", scene=menu, u=0.5, v=0.5)
        hit = got.get("would_select") or {}
        ed.check_eq(hit.get("scene"), shown, f"picked in the loaded scene: {got}")
        names = [h.get("name") for h in got.get("hits", [])]
        ed.check("Pillar" in names, f"the pillar is under the view's centre: {got}")
    finally:
        await ed.call("play", action="stop")


async def test_save_dialog_refuses_another_scenes_file(ed: Editor):
    """The Save Scene dialog warned "will be replaced" and wrote over another scene's file."""
    await ed.call("create_scene", name="Taken", type="3D", path="Taken")
    taken = (ed.project / "Taken.scene").read_bytes()
    draft = (await ed.call("create_scene", name="Draft", type="3D"))["scene"]["id"]
    await ed.call("select_scene", scene=draft)
    await ed.call("create_entity", scene=draft, name="Dirty", type="empty")

    await ed.call("run_command", id="file.save")
    d = await wait_dialog(ed, "Save Scene")
    ed.check(d, "Ctrl+S on the untitled scene asks")
    r = await ed.call_raw("respond_dialog", {"choice": "ok", "values": {"name": "Taken", "folder": "."}})
    ed.check(r.is_error and "is the file of scene" in r.text, f"refused: {r.text[:300]}")
    d = await dialog(ed)
    ed.check(d and "is the file of scene" in d.get("error", ""), f"the dialog says why: {d}")
    ed.check_eq((ed.project / "Taken.scene").read_bytes(), taken, "Taken.scene is untouched")

    r = await ed.call_raw("respond_dialog", {"choice": "ok", "values": {"name": "../Outside"}})
    ed.check(r.is_error, f"a name leaving the folder is refused: {r.text[:300]}")
    await ed.call("respond_dialog", choice="cancel")
    ed.check_eq(await dialog(ed), None, "cancelled")
    listed = await scenes_by_name(ed)
    ed.check_eq(listed["Draft"].get("path", ""), "", f"a cancelled dialog gives no path: {listed['Draft']}")


async def test_save_all_asks_for_untitled_scenes(ed: Editor):
    """Save All saved only scenes with changes: an untouched untitled scene was never saved."""
    await ed.call("create_scene", name="Saved Before", type="3D", path="scenes/SavedBefore")
    await ed.call("create_scene", name="Untouched", type="3D")
    await ed.call("run_command", id="file.saveAll")
    d = await wait_dialog(ed, "Save Scene")
    ed.check(d, f"Save All asks where to save the untitled scene: {await dialog(ed)}")
    await ed.call("respond_dialog", choice="ok", values={"name": "Untouched", "folder": "scenes"})
    for _ in range(80):
        if (ed.project / "scenes" / "Untouched.scene").exists():
            break
        await asyncio.sleep(0.25)
    ed.check((ed.project / "scenes" / "Untouched.scene").exists(), "saved where the dialog said")
    # a project in the temporary folder then asks where the project goes
    if await wait_dialog(ed, "Save Project", seconds=10):
        await ed.call("respond_dialog", choice="cancel")

