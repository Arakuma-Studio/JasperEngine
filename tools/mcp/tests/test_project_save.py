"""Authored scene state survives model undo and Play copies.

ProjectSaveTest.cpp owns atomic file replacement, newline conversion and child-cycle command logic.
These cases keep writer policy, command integration and actual Play-copy persistence.
"""

import time
import re
import shutil

import yaml

from harness import Editor

EDITOR = {"create": True}
CI = True


# -- helpers --------------------------------------------------------------------


def shader_keys_block(text: str) -> str:
    match = re.search(r"^shaderKeys:\n(?:[ ]+.*\n|-.*\n)*", text, re.M)
    return match.group(0) if match else ""


def without_shader_keys(text: str) -> str:
    block = shader_keys_block(text)
    return text.replace(block, "") if block else text


def parent_id(got: dict):
    parent = got.get("parent")
    if isinstance(parent, dict):
        return parent.get("id")
    return parent


async def scene_ids(ed: Editor) -> dict:
    return {s["name"]: s for s in (await ed.call("list_scenes"))["scenes"]}


async def children_of(ed: Editor, scene_id) -> list:
    listed = {s["id"]: s for s in (await ed.call("list_scenes"))["scenes"]}
    return [c["id"] for c in listed[scene_id].get("child_scenes") or []]




# -- model load undo -----------------------------------------------------------------------


async def test_model_load_undo_keeps_parent(ed: Editor):
    scene = (await ed.call("create_scene", name="Models", type="3D", path="scenes/Models"))["scene"]["id"]
    models = ed.project / "assets" / "models"
    models.mkdir(parents=True, exist_ok=True)
    (models / "tri.obj").write_text("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n")
    (models / "quad.obj").write_text("v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nf 1 2 3\nf 1 3 4\n")

    holder = (await ed.call("create_entity", scene=scene, type="empty", name="Holder", position=[3, 0, 0]))["entity_id"]
    model = (await ed.call("create_entity", scene=scene, type="model", name="Tri", parent=holder,
                           model="assets/models/tri.obj", position=[0, 1, 0]))["entity_id"]
    got = await ed.call("get_entity", scene=scene, entity=model, components=["transform"], properties=["position"])
    ed.check_eq(parent_id(got), holder, f"created under Holder: {got.get('parent')}")

    await ed.call("set_model", scene=scene, entity=model, path="assets/models/quad.obj")
    await ed.call("undo", scene=scene)
    got = await ed.call("get_entity", scene=scene, entity=model, components=["model", "transform"],
                        properties=["filename", "position"])
    ed.check_eq(got["components"]["model"]["filename"], "assets/models/tri.obj", "the old model is back")
    ed.check_eq(parent_id(got), holder, f"undo detached the model from its parent: {got.get('parent')}")
    position = got["components"]["transform"]["position"]
    ed.check(all(abs(x - y) < 1e-3 for x, y in zip(position, [0, 1, 0])), f"authored local position kept: {position}")




async def test_save_keeps_crlf_of_existing_files(ed: Editor):
    """The scene/project writers opt into existing Windows line endings."""
    sid = (await ed.call("create_scene", name="Endings", type="3D", path="scenes/Endings"))["scene"]["id"]
    scene_file = ed.project / "scenes/Endings.scene"
    await ed.call("save_scene", scene=sid)
    project_file = ed.project / "project.yaml"
    for path in (scene_file, project_file):
        path.write_bytes(path.read_bytes().replace(b"\r\n", b"\n").replace(b"\n", b"\r\n"))
    await ed.call("create_entity", scene=sid, type="box", name="SavedAfterCrlf")
    await ed.call("save_scene", scene=sid)
    ed.check_in("SavedAfterCrlf", [node["name"] for node in
                                  (await ed.call("get_scene_tree", scene=sid, format="json"))["nodes"]])
    for path in (scene_file, project_file):
        data = path.read_bytes()
        ed.check(data.count(b"\n") == data.count(b"\r\n") > 0,
                 f"{path.name}: every persisted line retains CRLF")


# -- Stop and the scenes that ran as Play copies -------------------------------------------


async def test_stop_keeps_child_scene_file():
    """Stop preserves authored child state; legacy on-disk cycles remain safe to play."""
    first = Editor(create=True, keep=True, label="save-stop-setup")
    second = None
    third = None
    try:
        async with first as ed:
            main = (await ed.call("create_scene", name="Main", type="3D", path="scenes/Main"))["scene"]["id"]
            await ed.call("create_entity", scene=main, type="camera", name="Main Camera", position=[0, 2, 8])
            await ed.call("set_main_camera", scene=main, entity="Main Camera")
            await ed.call("create_entity", scene=main, type="cube", name="Floor", scale=[4, 0.2, 4])
            hud = (await ed.call("create_scene", name="Hud", type="3D", path="scenes/Hud"))["scene"]["id"]
            await ed.call("create_entity", scene=hud, type="camera", name="Hud Camera", position=[0, 0, 8])
            await ed.call("set_main_camera", scene=hud, entity="Hud Camera")
            await ed.call("create_entity", scene=hud, type="cube", name="Box", position=[1, 2, 3])
            await ed.call("set_child_scene", scene=main, child=hud, start_active=False)
            await ed.call("set_child_scene", scene=main, child=hud, start_active=True)
            await ed.call("undo", scene=main)
            listed = {s["id"]: s for s in (await ed.call("list_scenes"))["scenes"]}
            ed.check_eq(listed[main]["child_scenes"][0]["start_active"], False)
            await ed.call("redo", scene=main)
            await ed.check_error("set_child_scene", scene=hud, child=main, contains="cycle")
            ed.check_eq(await children_of(ed, hud), [], "a refused cycle leaves the graph unchanged")
            await ed.call("save_scene", all=True)
            project = ed.project

        # Without its saved keys, the Play copy finds new ones, which Stop then saves
        hud_file = project / "scenes" / "Hud.scene"
        authored = without_shader_keys(hud_file.read_text())
        hud_file.write_text(authored)

        second = Editor(project=project, keep=True, label="save-stop-play")
        async with second as ed:
            hud_file = ed.project / "scenes" / "Hud.scene"
            await ed.call_raw("open_scene", {"scene": "scenes/Main.scene"})
            await ed.call("select_scene", scene="Main")
            started = await ed.call("play", action="start", scene="Main")
            ed.check_eq(started["state"], "playing", f"play start: {started}")
            await ed.call("set_runtime_property", scene="Hud", entity="Box", component="transform",
                          values={"position": [50, 60, 70]})
            await ed.call("wait", frames=2)
            stopped = await ed.call("play", action="stop")
            ed.check_eq(stopped["state"], "stopped", "stop")

            # the save on Stop runs on the editor's main thread after the call returns
            text = hud_file.read_text()
            deadline = time.monotonic() + 10
            while not shader_keys_block(text) and time.monotonic() < deadline:
                await ed.call("editor_status")
                text = hud_file.read_text()
            ed.check(shader_keys_block(text), "Stop wrote the shader keys the Play copy found")
            ed.check_eq(yaml.safe_load(without_shader_keys(text)), yaml.safe_load(authored),
                        "Stop changed more of Hud.scene than its shaderKeys")

            opened = await ed.call("open_scene", scene="scenes/Hud.scene")
            box = await ed.call("get_entity", scene=opened["scene"]["id"], entity="Box",
                                components=["transform"], properties=["position"])
            position = box["components"]["transform"]["position"]
            ed.check(all(abs(x - y) < 1e-3 for x, y in zip(position, [1, 2, 3])),
                     f"Hud.scene holds the played position: {position}")

            # a cycle on disk, as a hand edit or an older editor could leave it
            main_id = (await scene_ids(ed))["Main"]["id"]
            cyclic = yaml.safe_load(hud_file.read_text())
            cyclic["childScenes"] = [{"id": main_id, "startActive": True}]
            hud_file.write_text(yaml.safe_dump(cyclic, sort_keys=False))
            cycle_project = ed.project

        third = Editor(project=cycle_project, keep=True, label="save-stop-cycle")
        async with third as ed:
            await ed.call_raw("open_scene", {"scene": "scenes/Main.scene"})
            await ed.call("select_scene", scene="Main")
            started = await ed.call("play", action="start", scene="Main")
            ed.check_eq(started["state"], "playing", f"a child scene cycle on disk keeps Play working: {started}")
            stopped = await ed.call("play", action="stop")
            ed.check_eq(stopped["state"], "stopped", "stop")
            await ed.check_alive()
    finally:
        for editor in (first, second, third):
            if editor and editor.scratch:
                shutil.rmtree(editor.scratch.root, ignore_errors=True)
