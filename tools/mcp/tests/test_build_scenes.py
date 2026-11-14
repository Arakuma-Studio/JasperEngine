"""The build list is Unity's Build Settings (S98 REQ-28, D6): project.yaml `scenes:` lists the
scenes in the build, and opening a scene file leaves it alone.

- open_scene on a file project.yaml does not list opens it out of the build, and neither the open
  nor a save of it writes project.yaml; edit_build_scenes list names it as open, not in the build;
  get_build_info leaves it out; the workspace reopens it with the project, still out of the build;
- edit_build_scenes add puts it in; a listed scene that takes an unlisted one as a child scene
  brings it into the build; closing an unlisted scene takes it out of the editor;
- create_scene still adds a new scene to the build.
"""

import shutil

from harness import Editor

CI = True


def build_names(listed: dict) -> list:
    return [s["name"] for s in listed["scenes_in_build"]]


def open_names(listed: dict) -> list:
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
            ed.check("Extra" not in build_names(listed), f"not in the build: {listed}")
            ed.check_eq(open_names(listed), ["Extra"], f"listed as open, not in the build: {listed}")
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
            ed.check_eq(open_names(await ed.call("edit_build_scenes")), ["Extra"], "still out of the build")
            ed.check(project_file.read_bytes() == listed_bytes, "reopening the project did not list it")

            # a listed scene using an unlisted one as a child scene brings it into the build
            child = (await ed.call("open_scene", scene="scenes/Other.scene"))["scene"]["id"]
            await ed.call("set_child_scene", scene="Main", child=child)
            listed = await ed.call("edit_build_scenes")
            ed.check("Other" in build_names(listed), f"a listed scene's child scene is in the build: {listed}")
            ed.check("Other.scene" in project_file.read_text(), "project.yaml lists it")

            # add puts the opened one in
            added = await ed.call("edit_build_scenes", action="add", scene="Extra")
            ed.check("Extra" in build_names(added), f"add puts it in: {added}")
            ed.check_eq(open_names(added), [], "nothing is left out")
            ed.check("scenes/Extra.scene" in project_file.read_text(), "project.yaml lists it")

            # create_scene still adds the new scene to the build
            await ed.call("create_scene", name="Fresh", type="3D", path="scenes/Fresh")
            ed.check("scenes/Fresh.scene" in project_file.read_text(), "create_scene lists the new scene")
            ed.check("Fresh" in build_names(await ed.call("edit_build_scenes")), "and it is in the build")
    finally:
        for editor in (first, second):
            if editor and editor.scratch:
                shutil.rmtree(editor.scratch.root, ignore_errors=True)
