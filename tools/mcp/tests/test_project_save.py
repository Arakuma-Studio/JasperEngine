"""Project saving and the child scene graph (docs/mcp/DESIGN.md).

- A scene that ran as a Play copy (a child scene of the played one) keeps its authored file at
  Stop: only the shader keys the copy found are written into it, never its played state.
- A child scene that would make a cycle is refused, also by redo/undo, and a project whose files
  already hold a cycle still plays (the walks over child scenes used to recurse forever).
- Undoing a model load keeps the model GameObject under its parent.

Runs in new empty projects, no fixture needed. The file-level checks (a failed save keeps the old
file, symlinks, permissions) are gtests: editor/tests/ProjectSaveTest.cpp.
"""

import asyncio
import re
import shutil

import yaml

from harness import Editor

EDITOR = {"create": True}
CI = True
TIMEOUT = 900


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


# -- child scene cycles --------------------------------------------------------------------


async def test_child_scene_cycle_refused(ed: Editor):
    a = (await ed.call("create_scene", name="CycleA", type="3D", path="scenes/CycleA"))["scene"]["id"]
    b = (await ed.call("create_scene", name="CycleB", type="3D", path="scenes/CycleB"))["scene"]["id"]
    c = (await ed.call("create_scene", name="CycleC", type="3D", path="scenes/CycleC"))["scene"]["id"]

    await ed.call("set_child_scene", scene=a, child=b)
    await ed.call("set_child_scene", scene=b, child=c)
    await ed.check_error("set_child_scene", scene=b, child=a, contains="cycle")
    await ed.check_error("set_child_scene", scene=c, child=a, contains="cycle")
    ed.check_eq(await children_of(ed, b), [c], "B keeps only C")
    ed.check_eq(await children_of(ed, c), [], "C took nothing")
    # a grandchild may also be a direct child
    await ed.call("set_child_scene", scene=a, child=c)
    ed.check_eq(sorted(await children_of(ed, a)), sorted([b, c]))
    await ed.call("undo", scene=a)

    # A -> B undone, B takes A: redoing A -> B would close the cycle
    await ed.call("undo", scene=a)
    ed.check_eq(await children_of(ed, a), [], "A -> B undone")
    await ed.call("set_child_scene", scene=b, child=a)
    await ed.check_error("redo", scene=a, contains="cycle")
    ed.check_eq(await children_of(ed, a), [], "the refused redo changed nothing")
    # once B lets go of A, the redo applies
    await ed.call("undo", scene=b)
    await ed.call("redo", scene=a)
    ed.check_eq(await children_of(ed, a), [b], "A -> B redone")

    # a removal whose undo would now close a cycle
    await ed.call("set_child_scene", scene=a, child=b, attached=False)
    await ed.call("set_child_scene", scene=c, child=a)
    await ed.check_error("undo", scene=a, contains="cycle")
    ed.check_eq(await children_of(ed, a), [], "the refused undo changed nothing")
    await ed.call("undo", scene=c)
    await ed.call("undo", scene=a)
    ed.check_eq(await children_of(ed, a), [b], "the removal undone")
    await ed.check_alive()


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
    before = got["components"]["transform"]["position"]

    await ed.call("set_model", scene=scene, entity=model, path="assets/models/quad.obj")
    await ed.call("undo", scene=scene)
    got = await ed.call("get_entity", scene=scene, entity=model, components=["model", "transform"],
                        properties=["filename", "position"])
    ed.check_eq(got["components"]["model"]["filename"], "assets/models/tri.obj", "the old model is back")
    ed.check_eq(parent_id(got), holder, f"undo detached the model from its parent: {got.get('parent')}")
    position = got["components"]["transform"]["position"]
    ed.check(all(abs(x - y) < 1e-3 for x, y in zip(position, before)), f"position kept: {position} vs {before}")
    tree = await ed.call("get_scene_tree", scene=scene, format="json")
    holder_node = next(n for n in tree["nodes"] if n["name"] == "Holder")
    ed.check_in("Tri", [n["name"] for n in holder_node.get("children", [])], "Tri listed under Holder")


# -- line endings ---------------------------------------------------------------------------


async def test_save_keeps_crlf_of_existing_files(ed: Editor):
    """A scene and project.yaml checked out with \\r\\n (a Windows checkout) are saved with \\r\\n
    again, so one save does not show every line as changed; a new scene file gets \\n."""
    scene = (await ed.call("create_scene", name="Endings", type="3D", path="scenes/Endings"))["scene"]["id"]
    await ed.call("create_entity", scene=scene, type="cube", name="First")
    await ed.call("save_scene", scene=scene)
    scene_file = ed.project / "scenes" / "Endings.scene"
    first = scene_file.read_bytes()
    ed.check(b"\r\n" not in first, "a new scene file is written with \\n")

    scene_file.write_bytes(first.replace(b"\n", b"\r\n"))
    project_file = ed.project / "project.yaml"
    project_file.write_bytes(project_file.read_bytes().replace(b"\r\n", b"\n").replace(b"\n", b"\r\n"))

    await ed.call("create_entity", scene=scene, type="cube", name="Second")
    await ed.call("save_scene", scene=scene)
    saved = scene_file.read_bytes()
    ed.check(b"Second" in saved, "the edit was saved")
    ed.check(saved.count(b"\n") == saved.count(b"\r\n"), "every line of the scene still ends with \\r\\n")
    ed.check_in("Second", [n["name"] for n in (await ed.call("get_scene_tree", scene=scene, format="json"))["nodes"]])
    ed.check(yaml.safe_load(saved.decode()), "the \\r\\n scene file parses")
    project = project_file.read_bytes()
    ed.check(project.count(b"\n") == project.count(b"\r\n"), "project.yaml still ends its lines with \\r\\n")


# -- Stop and the scenes that ran as Play copies -------------------------------------------


async def test_stop_keeps_child_scene_file():
    """Main plays with Hud as its child scene. Hud runs as a Play copy, so Stop writes only the
    shader keys the copy found into Hud.scene, never the copy's played state. A third editor then
    plays a project whose files hold a child scene cycle."""
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
            await ed.call("set_child_scene", scene=main, child=hud)
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
            await ed.call("wait", seconds=0.5)
            stopped = await ed.call("play", action="stop")
            ed.check_eq(stopped["state"], "stopped", "stop")

            # the save on Stop runs on the editor's main thread after the call returns
            text = hud_file.read_text()
            for _ in range(60):
                text = hud_file.read_text()
                if shader_keys_block(text):
                    break
                await ed.call("editor_status")
                await asyncio.sleep(0.25)
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
