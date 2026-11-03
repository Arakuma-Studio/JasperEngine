"""Project settings, build ordering and safe project switching through MCP."""

from pathlib import Path

import yaml

from harness import Editor

EDITOR = {"create": True, "name": "Project Contracts"}
CI = True


def project_yaml(ed: Editor) -> dict:
    return yaml.safe_load((ed.project / "project.yaml").read_text())


async def test_settings_set(ed: Editor):
    described = await ed.call("get_project_settings", keys=["graphics.scaling_mode"], describe=True)
    ed.check_in("crop", described["describe"]["graphics.scaling_mode"]["values"],
                "the configuration metadata advertises the supported requested mode")
    await ed.call("set_project_settings", values={
        "player.window_width": 800,
        "player.window_height": 600,
        "graphics.scaling_mode": "crop",
        "android.abis": ["arm64-v8a"],
        "tags_and_layers.layers": {"8": "Obstacles", "9": "ProbeWater"},
        "preferences.build_jobs": 3,
    })
    await ed.call("set_project_settings", values={"tags_and_layers.layers": {"8": "Crates"},
                                                  "physics2d.layer_collision_matrix": [[8, 9], [8, 8]]})
    got = (await ed.call("get_project_settings", keys=["player.window_width", "android.abis",
                                                     "tags_and_layers.layers", "physics2d.layer_collision_matrix"]))["settings"]
    ed.check_eq(got["player.window_width"], 800)
    ed.check_eq(got["android.abis"], ["arm64-v8a"])
    ed.check_eq(got["tags_and_layers.layers"]["9"], "ProbeWater", "layer updates merge by index")
    ed.check_eq({tuple(sorted(pair)) for pair in got["physics2d.layer_collision_matrix"]},
                {("Crates", "ProbeWater"), ("Crates", "Crates")})
    saved = project_yaml(ed)
    ed.check_eq([saved["windowWidth"], saved["windowHeight"], saved["scalingMode"]], [800, 600, "crop"])
    ed.check_eq((await ed.call("get_build_info"))["desktop"]["kit"]["build_jobs"], 3)

    # Validation is atomic, including the persisted project file.
    before = (ed.project / "project.yaml").read_bytes()
    await ed.check_error("set_project_settings", values={"player.window_width": 1024,
                                                         "graphics.scaling_mode": "zoom"}, contains="scaling_mode")
    ed.check_eq((await ed.call("get_project_settings", keys=["player.window_width"]))["settings"]["player.window_width"], 800)
    ed.check_eq((ed.project / "project.yaml").read_bytes(), before)
    (ed.scratch.root / "outside").mkdir(exist_ok=True)
    await ed.check_error("set_project_settings", values={"scripting.script_dirs": ["../outside"]}, contains="inside")


async def test_build_scenes(ed: Editor):
    first = (await ed.call("create_scene", name="BuildFirst", type="3D", path="scenes/BuildFirst"))["scene"]["id"]
    second = (await ed.call("create_scene", name="BuildSecond", type="UI", path="scenes/BuildSecond"))["scene"]["id"]
    third = (await ed.call("create_scene", name="BuildThird", type="3D", path="scenes/BuildThird"))["scene"]["id"]
    for sid in (first, second, third):
        await ed.call("save_scene", scene=sid)
    await ed.call("edit_build_scenes", action="set_start", scene=first)
    reordered = await ed.call("edit_build_scenes", action="move", scene=third, index=1)
    ed.check_eq([s["id"] for s in reordered["scenes_in_build"]], [first, third, second])
    ed.check_eq(project_yaml(ed)["startSceneId"], first, "moving a non-start entry preserves the start scene")
    moved = await ed.call("edit_build_scenes", action="move", scene=second, index=0)
    ed.check_eq(moved["scenes_in_build"][0]["id"], second)
    ed.check_eq(project_yaml(ed)["startSceneId"], second)

    # Removing a build entry preserves the saved scene and its authored contents.
    scene_file = ed.project / "scenes/BuildSecond.scene"
    before = scene_file.read_bytes()
    await ed.call("edit_build_scenes", action="remove", scene=second)
    ed.check_eq(scene_file.read_bytes(), before)
    ed.check("scenes/BuildSecond.scene" not in [s["filepath"] for s in project_yaml(ed)["scenes"]],
             "removed scene is absent from the build manifest")
    added = await ed.call("edit_build_scenes", action="add", scene="scenes/BuildSecond.scene", index=0)
    ed.check_eq(added["scenes_in_build"][0]["name"], "BuildSecond")
    ed.check_eq(project_yaml(ed)["startSceneId"], added["scenes_in_build"][0]["id"])


async def test_project_build_file(ed: Editor):
    content = "target_compile_definitions(${JASPERENGINE_TARGET} PRIVATE MCP_CONTRACT=1)\n"
    await ed.call("edit_project_build_file", content=content)
    ed.check_eq((ed.project / "ProjectBuild.cmake").read_text(), content)
    ed.check_eq((await ed.call("edit_project_build_file"))["content"], content)
    ed.check_eq((await ed.call("get_project_summary"))["has_project_build_file"], True)
    await ed.call("edit_project_build_file", content="")
    ed.check(not (ed.project / "ProjectBuild.cmake").exists(), "empty content deletes the hook")
    ed.check_eq((await ed.call("get_project_summary"))["has_project_build_file"], False)


async def test_create_and_open(ed: Editor):
    original = ed.project.resolve()
    original_scene = (await ed.call("create_scene", name="SwitchOriginal", type="3D",
                                    path="scenes/SwitchOriginal"))["scene"]["id"]
    await ed.call("save_scene", scene=original_scene)
    await ed.call("create_entity", scene=original_scene, name="UnsavedBox", type="box")
    new_dir = ed.scratch.root / "created"
    await ed.check_error("create_project", path=str(new_dir), contains="unsaved")
    ed.check(not (new_dir / "project.yaml").exists(), "cancel creates no project")
    ed.check_eq(Path((await ed.call("editor_status"))["project"]["path"]).resolve(), original)

    created = await ed.call("create_project", path=str(new_dir), name="Switched Project", unsaved="discard")
    ed.check_eq(created["name"], "Switched Project")
    await ed.call("create_entity", name="SavedMarker", type="box")
    saved = await ed.call("save_project", untitled_scene_dir="scenes")
    scene_path = saved["saved"]["scenes"][0]["path"]
    ed.check((new_dir / scene_path).is_file(), "Save All persists the newly created scene")
    moved_dir = ed.scratch.root / "moved" / "Project"
    moved = await ed.call("save_project", path=str(moved_dir))
    ed.check_eq(Path(moved["path"]).resolve(), moved_dir.resolve())
    ed.check(not new_dir.exists() and (moved_dir / scene_path).is_file(),
             "Save As moves the temporary project with its saved scene")
    new_dir = moved_dir

    reopened = await ed.call("open_project", path=str(original / "project.yaml"))
    ed.check_eq(Path(reopened["path"]).resolve(), original)
    await ed.call("open_scene", scene="scenes/SwitchOriginal.scene")
    tree = await ed.call("get_scene_tree", scene="SwitchOriginal", format="json")
    ed.check("UnsavedBox" not in [node["name"] for node in tree["nodes"]], "discard did not persist the edit")
    await ed.call("open_project", path=str(new_dir))
    tree = await ed.call("get_scene_tree", format="json")
    ed.check_in("SavedMarker", [node["name"] for node in tree["nodes"]], "saved state survives project reopening")
    await ed.call("open_project", path=str(original))
