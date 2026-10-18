"""Scene setting scene_color_scale (shader library phase 3): the resolution of the scene-colour copy
that shaders requiring scenecolor sample. Set and read back through MCP, saved as `sceneColorScale`
only when it differs from the default (half), so scenes saved before it existed re-save unchanged."""

from __future__ import annotations

import re

from harness import Editor

EDITOR = {"create": True, "name": "Scene Color Scale"}
TIMEOUT = 600

SCENE = "scenes/SceneColorScale.scene"
KEY = re.compile(r"^\s*sceneColorScale:\s*(\S+)\s*$", re.M)


def saved_value(ed: Editor) -> str | None:
    """The sceneColorScale written in the saved scene file, None when absent."""
    text = (ed.project / SCENE).read_text()
    found = KEY.findall(text)
    ed.check(len(found) <= 1, f"sceneColorScale written {len(found)} times")
    return found[0] if found else None


async def test_scene_color_scale(ed: Editor):
    ed.require_tools("create_scene", "get_scene_settings", "set_scene_settings", "save_scene")
    made = await ed.call("create_scene", name="SceneColorScale", type="3d")
    sid = made["scene"]["id"]

    got = await ed.call("get_scene_settings", scene=sid, detail=True)
    ed.check_eq(got["settings"]["scene_color_scale"], "half", "default")
    ed.check_eq(got["schema"]["scene_color_scale"]["options"], ["full", "half"])
    ed.check_eq(got["schema"]["scene_color_scale"]["default"], "half")

    # the default is not written
    await ed.call("save_scene", scene=sid, path=SCENE)
    ed.check_eq(saved_value(ed), None, "default half is not saved")

    out = await ed.call("set_scene_settings", scene=sid, settings={"scene_color_scale": "full"})
    ed.check_eq(out["settings"]["scene_color_scale"], "full")
    s = (await ed.call("get_scene_settings", scene=sid, names=["scene_color_scale"]))["settings"]
    ed.check_eq(s["scene_color_scale"], "full", "read back")
    await ed.call("save_scene", scene=sid)
    ed.check_eq(saved_value(ed), "full", "full is saved")

    await ed.call("set_scene_settings", scene=sid, settings={"scene_color_scale": "half"})
    await ed.call("save_scene", scene=sid)
    ed.check_eq(saved_value(ed), None, "back to half: the key is gone")

    await ed.check_error("set_scene_settings", scene=sid, settings={"scene_color_scale": "quarter"}, contains="half")
    s = (await ed.call("get_scene_settings", scene=sid, names=["scene_color_scale"]))["settings"]
    ed.check_eq(s["scene_color_scale"], "half", "a refused value changes nothing")
