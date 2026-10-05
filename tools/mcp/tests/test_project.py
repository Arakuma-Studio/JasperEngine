"""W5 project tools: get_project_summary, get/set_project_settings, edit_build_scenes,
edit_project_build_file, save_project, create_project, open_project.

Runs on a scratch copy of the TappyPlane fixture (Main Scene + HUD Scene). The project switching
tests come last: they leave the editor on other projects.
"""

from pathlib import Path

import yaml

from harness import Editor, ToolError, find_key


def project_yaml(ed: Editor) -> dict:
    return yaml.safe_load((ed.project / "project.yaml").read_text())


async def build_names(ed: Editor) -> list[str]:
    r = await ed.call("edit_build_scenes")
    return [s["name"] for s in r["scenes_in_build"]]


# -- summary ------------------------------------------------------------------


async def test_summary(ed: Editor):
    s = await ed.call("get_project_summary")
    ed.check_eq(s["name"], "TappyPlane")
    ed.check_eq(Path(s["path"]).resolve(), ed.project.resolve(), "path")
    # the harness's scratch projects live in the temp directory, which the editor calls temporary
    ed.check(isinstance(s["temporary"], bool), "temporary")
    ed.check_eq([x["name"] for x in s["scenes_in_build"]], ["Main Scene", "HUD Scene"])
    ed.check(s["scenes_in_build"][0]["start"], "index 0 is the start scene")
    ed.check_eq(s["start_scene_id"], s["scenes_in_build"][0]["id"])
    ed.check_eq(s["assets_dir"], "assets")
    ed.check_eq(s["lua_dir"], "scripts")
    ed.check_in(s["cxx_standard"], (17, 20, 23))
    ed.check_eq(s["platform"], "linux")
    ed.check(s["engine_version"], "engine_version")
    for legacy in ("set_project_script_dirs", "set_project_cxx_standard", "update_project_build_file",
                   "set_start_scene", "add_project_scene"):
        ed.check(not ed.has_tool(legacy), f"legacy {legacy} should be superseded")


# -- settings -----------------------------------------------------------------


async def test_settings_get(ed: Editor):
    r = await ed.call("get_project_settings")
    s = r["settings"]
    ed.check_eq(s["player.product_name"], "TappyPlane")
    ed.check_eq(s["graphics.canvas_width"], 1280)
    ed.check_eq(s["graphics.scaling_mode"], "letterbox")
    ed.check_eq(s["graphics.texture_strategy"], "resize")
    ed.check_eq(s["player.window_mode"], "windowed")
    ed.check("android.orientation" not in s, "platform overrides are left out by default")
    ed.check_in("preferences.clear_on_play", s)

    a = await ed.call("get_project_settings", category="android", describe=True)
    ed.check_in("android.orientation", a["settings"])
    ed.check_eq(a["describe"]["android.orientation"]["type"], "enum")
    ed.check_in("landscape", a["describe"]["android.orientation"]["values"])

    k = await ed.call("get_project_settings", keys=["player.window_width", "physics2d"])
    ed.check_eq(set(k["settings"]), {"player.window_width", "physics2d.enabled", "physics2d.layer_collision_matrix"})

    msg = await ed.call("get_project_settings", keys=["player.nope"], expect_error=True)
    ed.check_in("player.window_width", msg, "unknown key lists the category's keys")
    await ed.call("get_project_settings", category="bogus", expect_error=True)


async def test_settings_set(ed: Editor):
    r = await ed.call("set_project_settings", values={
        "player.window_width": 800,
        "player.window_height": 600,
        "player.window_title": "Tappy MCP",
        "player.identifier": "com.example.tappy",
        "graphics.scaling_mode": "crop",
        "quality.vsync": False,
        "scripting.cxx_standard": 20,
        "tags_and_layers.tags": ["Enemy", "Coin"],
        "tags_and_layers.layers": {"8": "Obstacles"},
        "android.orientation": "landscape",
        "android.abis": ["arm64-v8a"],
    })
    # layer names a matrix uses must exist before the call
    r2 = await ed.call("set_project_settings", values={"physics2d.layer_collision_matrix": [["Obstacles", "Water"], [8, 8]]})
    got = dict(r["settings"], **r2["settings"])
    ed.check_eq(got["player.window_width"], 800)
    ed.check_eq(got["graphics.scaling_mode"], "crop")
    ed.check_eq(got["tags_and_layers.tags"], ["Enemy", "Coin"])
    ed.check_eq(got["tags_and_layers.layers"].get("8"), "Obstacles")
    pairs = {tuple(sorted(map(str, p))) for p in got["physics2d.layer_collision_matrix"]}
    ed.check_eq(pairs, {("Obstacles", "Water"), ("Obstacles", "Obstacles")})

    # persisted in project.yaml at once
    y = project_yaml(ed)
    ed.check_eq(y.get("windowWidth"), 800)
    ed.check_eq(y.get("windowHeight"), 600)
    ed.check_eq(y.get("scalingMode"), "crop")

    back = (await ed.call("get_project_settings", keys=["quality", "scripting.cxx_standard", "android.abis"]))["settings"]
    ed.check_eq(back["quality.vsync"], False)
    ed.check_eq(back["scripting.cxx_standard"], 20)
    ed.check_eq(back["android.abis"], ["arm64-v8a"])

    # one bad value changes nothing
    msg = await ed.call("set_project_settings", values={"player.window_width": 1024, "graphics.scaling_mode": "zoom"},
                        expect_error=True)
    ed.check_in("letterbox", msg, "lists the valid values")
    ed.check_in("Nothing was changed", msg)
    now = (await ed.call("get_project_settings", keys=["player.window_width"]))["settings"]
    ed.check_eq(now["player.window_width"], 800)

    for bad in ({"scripting.cxx_standard": 14}, {"player.identifier": "nodots"}, {"tags_and_layers.tags": ["Player"]},
                {"tags_and_layers.layers": {"0": "Mine"}}, {"player.window_icon": "missing.png"},
                {"scripting.script_dirs": ["../outside"]}, {"physics.layer_collision_matrix": [["NoSuchLayer", 1]]}):
        await ed.call("set_project_settings", values=bad, expect_error=True)

    # restore what later tests (builds) care about
    await ed.call("set_project_settings", values={"player.window_width": 1280, "player.window_height": 720,
                                                  "scripting.cxx_standard": 17, "quality.vsync": True})


async def test_script_dirs(ed: Editor):
    (ed.project / "cpp").mkdir(exist_ok=True)
    r = await ed.call("set_project_settings", values={"scripting.script_dirs": ["cpp"]})
    ed.check_eq(r["settings"]["scripting.script_dirs"], ["cpp"])
    s = await ed.call("get_project_summary")
    ed.check_eq(s["script_dirs"], ["cpp"])
    await ed.call("set_project_settings", values={"scripting.script_dirs": []})


async def test_preferences(ed: Editor):
    for value in (True, False):
        r = await ed.call("set_project_settings", values={"preferences.clear_on_play": value,
                                                          "preferences.error_pause": value})
        ed.check_eq(r["settings"]["preferences.clear_on_play"], value)
        ed.check_eq(r["settings"]["preferences.error_pause"], value)
    r = await ed.call("set_project_settings", values={"preferences.build_jobs": 3})
    ed.check_eq(r["settings"]["preferences.build_jobs"], 3)
    info = await ed.call("get_build_info")
    ed.check_eq(info["desktop"]["kit"]["build_jobs"], 3)


# -- scenes in build ------------------------------------------------------------


async def test_build_scenes(ed: Editor):
    ed.check_eq(await build_names(ed), ["Main Scene", "HUD Scene"])

    r = await ed.call("edit_build_scenes", action="move", scene="HUD Scene", index=0)
    ed.check_eq([s["name"] for s in r["scenes_in_build"]], ["HUD Scene", "Main Scene"])
    ed.check(r["scenes_in_build"][0]["start"], "moved to 0 = start")
    hud_id = r["scenes_in_build"][0]["id"]
    ed.check_eq(project_yaml(ed)["startSceneId"], hud_id)

    r = await ed.call("edit_build_scenes", action="set_start", scene="Main Scene.scene")
    ed.check_eq([s["name"] for s in r["scenes_in_build"]], ["Main Scene", "HUD Scene"])

    r = await ed.call("edit_build_scenes", action="remove", scene="HUD Scene")
    ed.check_eq([s["name"] for s in r["scenes_in_build"]], ["Main Scene"])
    ed.check((ed.project / "HUD Scene.scene").exists(), "remove keeps the file")
    ed.check_eq([s["filepath"] for s in project_yaml(ed)["scenes"]], ["Main Scene.scene"])

    await ed.call("edit_build_scenes", action="add", scene="Nope.scene", expect_error=True)
    r = await ed.call("edit_build_scenes", action="add", scene="HUD Scene.scene", index=0)
    ed.check_eq([s["name"] for s in r["scenes_in_build"]], ["HUD Scene", "Main Scene"])
    r = await ed.call("edit_build_scenes", action="move", scene="Main Scene", index=0)
    ed.check_eq([s["name"] for s in r["scenes_in_build"]], ["Main Scene", "HUD Scene"])

    await ed.call("edit_build_scenes", action="move", scene="Main Scene", index=7, expect_error=True)
    await ed.call("edit_build_scenes", action="set_start", expect_error=True)
    await ed.call("edit_build_scenes", action="frobnicate", scene="Main Scene", expect_error=True)


# -- ProjectBuild.cmake ---------------------------------------------------------


async def test_project_build_file(ed: Editor):
    r = await ed.call("edit_project_build_file")
    ed.check_eq(r["exists"], False)
    await ed.call("edit_project_build_file", content="target_link_libraries(game PRIVATE m)\n", expect_error=True)
    content = "target_compile_definitions(${JASPERENGINE_TARGET} PRIVATE MCP_TEST=1)\n"
    await ed.call("edit_project_build_file", content=content)
    ed.check_eq((ed.project / "ProjectBuild.cmake").read_text(), content)
    r = await ed.call("edit_project_build_file")
    ed.check_eq(r["content"], content)
    ed.check_eq((await ed.call("get_project_summary"))["has_project_build_file"], True)
    await ed.call("edit_project_build_file", content="")
    ed.check(not (ed.project / "ProjectBuild.cmake").exists(), "empty content deletes the file")


# -- save / create / open -------------------------------------------------------


async def test_save_project(ed: Editor):
    r = await ed.call("save_project")
    ed.check(isinstance(r["saved"]["scenes"], list), "saved.scenes")
    await ed.call("save_project", path="not/absolute", expect_error=True)


async def test_create_and_open(ed: Editor):
    original = ed.project.resolve()
    new_dir = ed.scratch.root / "created" / "FreshGame"

    # an unsaved edit blocks the switch unless told what to do with it
    if ed.has_tool("create_entity"):
        await ed.call("create_entity", name="UnsavedBox", type="box")
        msg = await ed.call("create_project", path=str(new_dir), expect_error=True)
        ed.check_in("unsaved", msg)
        ed.check(not (new_dir / "project.yaml").exists(), "nothing created on cancel")

    await ed.call("create_project", path="relative/dir", expect_error=True)
    await ed.call("create_project", path=str(original), expect_error=True)  # a project is there

    r = await ed.call("create_project", path=str(new_dir), name="Fresh Game", unsaved="discard")
    ed.check_eq(r["name"], "Fresh Game")
    ed.check_eq(Path(r["path"]).resolve(), new_dir.resolve())
    ed.check((new_dir / "project.yaml").exists(), "project.yaml written")
    ed.check_eq(r["scenes_in_build"], [], "the new scene is untitled")
    ed.check_eq(len(r["untitled_scenes"]), 1)

    status = await ed.call("editor_status")
    ed.check_eq(Path(status["project"]["path"]).resolve(), new_dir.resolve())

    # the discarded edit did not reach the original project's scene
    ed.check("UnsavedBox" not in (original / "Main Scene.scene").read_text(), "discard dropped the edit")

    s = await ed.call("save_project", untitled_scene_dir="scenes")
    ed.check_eq(len(s["saved"]["scenes"]), 1)
    path = s["saved"]["scenes"][0]["path"]
    ed.check(path.startswith("scenes/") and path.endswith(".scene"), f"saved to {path}")
    ed.check((new_dir / path).exists(), "scene file written")
    ed.check_eq(len(s["scenes_in_build"]), 1)
    ed.check(s["scenes_in_build"][0]["start"], "the only scene is the start scene")

    await ed.call("set_project_settings", values={"player.product_name": "Fresh Renamed"})
    ed.check_eq(yaml.safe_load((new_dir / "project.yaml").read_text())["name"], "Fresh Renamed")

    # Save As: a project in the temp directory (all of the harness's are) moves to path
    if r["temporary"]:
        moved = ed.scratch.root / "moved" / "FreshGame"
        m = await ed.call("save_project", path=str(moved))
        ed.check_eq(Path(m["path"]).resolve(), moved.resolve())
        ed.check((moved / path).exists(), "the scene moved along")
        ed.check(not new_dir.exists(), "the old directory is gone")
        new_dir = moved

    await ed.call("open_project", path=str(new_dir / "missing"), expect_error=True)
    r = await ed.call("open_project", path=str(original / "project.yaml"))
    ed.check_eq(r["name"], "TappyPlane")
    ed.check_eq(Path(r["path"]).resolve(), original)
    ed.check_eq([x["name"] for x in r["scenes_in_build"]], ["Main Scene", "HUD Scene"])

    r = await ed.call("open_project", path=str(original))
    ed.check_in("already open", (await ed.call_raw("open_project", {"path": str(original)})).text)

    r = await ed.call("open_project", path=str(new_dir))
    ed.check_eq(r["name"], "Fresh Renamed")
