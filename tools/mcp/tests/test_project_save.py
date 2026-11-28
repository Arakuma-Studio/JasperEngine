"""Authored scene state survives model undo and Play copies.

ProjectSaveTest.cpp owns atomic file replacement, newline conversion and child-cycle command logic.
These cases keep writer policy, command integration and actual Play-copy persistence.

The build list is Unity's Build Settings (S98 REQ-28, D6): project.yaml `scenes:` lists the
scenes in the build, and opening a scene file leaves it alone (test_open_unlisted_scene_stays_out):
- open_scene on a file project.yaml does not list opens it out of the build, and neither the open
  nor a save of it writes project.yaml; edit_build_scenes list names it as open, not in the build;
  get_build_info leaves it out; the workspace reopens it with the project, still out of the build;
- edit_build_scenes add puts it in; a listed scene that takes an unlisted one as a child scene
  brings it into the build; closing an unlisted scene takes it out of the editor;
- create_scene still adds a new scene to the build.
"""

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

        # Without its saved keys, the Play copy finds keys its file lacks: Stop once wrote them into
        # it, and now writes no scene file (S98 REQ-27; the keys go to ShaderWarmup.json)
        hud_file = project / "scenes" / "Hud.scene"
        authored = without_shader_keys(hud_file.read_text())
        hud_file.write_text(authored)
        authored_bytes = hud_file.read_bytes()

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

            # what Stop queues runs on the editor's main thread after the call returns
            for _ in range(5):
                await ed.call("editor_status")
            ed.check(hud_file.read_bytes() == authored_bytes, "Stop left Hud.scene as it was, shader keys included")

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


# -- the build list (S98 REQ-28) ---------------------------------------------------


def build_list_names(listed: dict) -> list:
    return [s["name"] for s in listed["scenes_in_build"]]


def open_list_names(listed: dict) -> list:
    return [s["name"] for s in listed.get("open_not_in_build", [])]


async def test_open_unlisted_scene_stays_out():
    """AC28: open_scene an unlisted file and save it: project.yaml is unchanged, edit_build_scenes
    list shows it as not in the build, add puts it in."""
    first = Editor(create=True, keep=True, label="build-scenes-open")
    second = None
    try:
        async with first as ed:
            main = (await ed.call("create_scene", name="Main", type="3D", path="scenes/Main"))["scene"]["id"]
            await ed.call("create_entity", scene=main, type="cube", name="Floor")
            # two scene files project.yaml does not list (written by the editor, then taken off the list)
            for name in ("Extra", "Other"):
                made = (await ed.call("create_scene", name=name, type="3D", path=f"scenes/{name}"))["scene"]["id"]
                await ed.call("create_entity", scene=made, type="cube", name=f"{name}Box")
                await ed.call("save_scene", scene=made)
                await ed.call("edit_build_scenes", action="remove", scene=made)
            await ed.call("save_scene", all=True)
            project_file = ed.project / "project.yaml"
            listed_bytes = project_file.read_bytes()
            ed.check("Extra.scene" not in listed_bytes.decode(), f"Extra is off the list: {listed_bytes.decode()}")

            opened = await ed.call("open_scene", scene="scenes/Extra.scene")
            extra = opened["scene"]["id"]
            ed.check(project_file.read_bytes() == listed_bytes, "opening an unlisted scene does not write project.yaml")
            await ed.call("create_entity", scene=extra, type="sphere", name="Added")
            await ed.call("save_scene", scene=extra)
            ed.check("Added" in (ed.project / "scenes" / "Extra.scene").read_text(), "the scene file saved")
            ed.check(project_file.read_bytes() == listed_bytes, "saving it does not write project.yaml either")

            listed = await ed.call("edit_build_scenes")
            ed.check("Extra" not in build_list_names(listed), f"not in the build: {listed}")
            ed.check_eq(open_list_names(listed), ["Extra"], f"listed as open, not in the build: {listed}")
            ed.check("not in the build" in (await ed.call_raw("edit_build_scenes", {})).text, "the text says so")
            info = await ed.call("get_build_info")
            ed.check("Extra" not in [s["name"] for s in info["scenes"]], f"no build has it: {info['scenes']}")
            await ed.check_error("edit_build_scenes", action="set_start", scene=extra, contains="edit_build_scenes add")

            # closing an unlisted scene takes it out of the editor, project.yaml untouched
            other = (await ed.call("open_scene", scene="scenes/Other.scene"))["scene"]["id"]
            await ed.call("close_scene", scene=other)
            ids = [s["id"] for s in (await ed.call("list_scenes"))["scenes"]]
            ed.check(other not in ids, f"the closed unlisted scene left the editor: {ids}")
            ed.check(project_file.read_bytes() == listed_bytes, "closing it did not write project.yaml")
            await ed.call("select_scene", scene=extra)
            project = ed.project

        # the workspace reopens it, still out of the build
        second = Editor(project=project, keep=True, label="build-scenes-reopen")
        async with second as ed:
            project_file = ed.project / "project.yaml"
            listed_bytes = project_file.read_bytes()
            scenes = {s["name"]: s for s in (await ed.call("list_scenes"))["scenes"]}
            ed.check("Extra" in scenes and scenes["Extra"].get("opened"), f"the workspace reopened Extra: {scenes}")
            ed.check_eq(open_list_names(await ed.call("edit_build_scenes")), ["Extra"], "still out of the build")
            ed.check(project_file.read_bytes() == listed_bytes, "reopening the project did not list it")

            # a listed scene using an unlisted one as a child scene brings it into the build
            child = (await ed.call("open_scene", scene="scenes/Other.scene"))["scene"]["id"]
            await ed.call("set_child_scene", scene="Main", child=child)
            listed = await ed.call("edit_build_scenes")
            ed.check("Other" in build_list_names(listed), f"a listed scene's child scene is in the build: {listed}")
            ed.check("Other.scene" in project_file.read_text(), "project.yaml lists it")

            # add puts the opened one in
            added = await ed.call("edit_build_scenes", action="add", scene="Extra")
            ed.check("Extra" in build_list_names(added), f"add puts it in: {added}")
            ed.check_eq(open_list_names(added), [], "nothing is left out")
            ed.check("scenes/Extra.scene" in project_file.read_text(), "project.yaml lists it")

            # create_scene still adds the new scene to the build
            await ed.call("create_scene", name="Fresh", type="3D", path="scenes/Fresh")
            ed.check("scenes/Fresh.scene" in project_file.read_text(), "create_scene lists the new scene")
            ed.check("Fresh" in build_list_names(await ed.call("edit_build_scenes")), "and it is in the build")
    finally:
        for editor in (first, second):
            if editor and editor.scratch:
                shutil.rmtree(editor.scratch.root, ignore_errors=True)
