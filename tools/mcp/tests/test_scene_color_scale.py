"""The public scene-colour scale setting preserves its default-omission file contract."""

import yaml

from harness import Editor

EDITOR = {"create": True, "name": "Scene Color Scale"}
SCENE = "scenes/SceneColorScale.scene"


def saved_scene(ed: Editor) -> dict:
    return yaml.safe_load((ed.project / SCENE).read_text())["scene"]


async def test_scene_color_scale(ed: Editor):
    sid = (await ed.call("create_scene", name="SceneColorScale", type="3d", path=SCENE))["scene"]["id"]
    await ed.call("save_scene", scene=sid)
    ed.check("sceneColorScale" not in saved_scene(ed), "default half is omitted for older-scene compatibility")
    await ed.call("set_scene_settings", scene=sid, settings={"scene_color_scale": "full"})
    await ed.call("save_scene", scene=sid)
    ed.check_eq(saved_scene(ed)["sceneColorScale"], "full")
    await ed.call("close_scene", scene=sid, unsaved="discard")
    sid = (await ed.call("open_scene", scene=SCENE))["scene"]["id"]
    ed.check_eq((await ed.call("get_scene_settings", scene=sid, names=["scene_color_scale"]))["settings"]["scene_color_scale"], "full")
    await ed.call("set_scene_settings", scene=sid, settings={"scene_color_scale": "half"})
    await ed.call("save_scene", scene=sid)
    ed.check("sceneColorScale" not in saved_scene(ed), "returning to the default removes the key")
