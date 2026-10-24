"""Scene setting post_processing (shader library phase 4): the engine's post-processing of the main
camera. Every field is a setting `post_processing.<field>` (and a key of the `post_processing`
object, which lists the fields away from their defaults); several in one call compose into one undo
step; saved under `postProcessing:` with only the fields away from their defaults, no key at all
while everything is default (scenes saved before it existed re-save unchanged)."""

from __future__ import annotations

from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Post Processing Settings"}
TIMEOUT = 600

SCENE = "scenes/PostProcessing.scene"


def saved_text(ed: Editor) -> str:
    return (ed.project / SCENE).read_text()


async def test_post_processing_settings(ed: Editor):
    ed.require_tools("create_scene", "get_scene_settings", "set_scene_settings", "save_scene", "undo")
    made = await ed.call("create_scene", name="PostProcessing", type="3d")
    sid = made["scene"]["id"]

    got = await ed.call("get_scene_settings", scene=sid, detail=True)
    ed.check_eq(got["settings"]["post_processing"], {}, "default: nothing away from the defaults")
    ed.check("post_processing.bloom_enabled" not in got["settings"], "fields are not listed one by one")
    detail = await ed.call("get_scene_settings", scene=sid, names=["post_processing.tonemap", "post_processing.render_scale"],
                           detail=True)
    ed.check_eq(detail["settings"]["post_processing.tonemap"], "none")
    ed.check_eq(detail["schema"]["post_processing.tonemap"]["options"], ["none", "aces", "neutral", "reinhard", "agx"])
    ed.check_eq(detail["schema"]["post_processing.render_scale"]["min"], 0.5)

    await ed.call("save_scene", scene=sid, path=SCENE)
    ed.check("postProcessing" not in saved_text(ed), "default post-processing is not saved")

    # two fields in one call compose (one command from one struct)
    out = await ed.call("set_scene_settings", scene=sid, settings={
        "post_processing.bloom_enabled": True, "post_processing.bloom_intensity": 2.5,
        "post_processing.tonemap": "aces", "post_processing.vignette_color": [0.2, 0.1, 0.0]})
    ed.check_eq(out["settings"]["post_processing.bloom_enabled"], True)
    s = (await ed.call("get_scene_settings", scene=sid, names=["post_processing"]))["settings"]["post_processing"]
    ed.check_eq(s, {"tonemap": "aces", "vignette_color": [0.2, 0.1, 0.0], "bloom_enabled": True, "bloom_intensity": 2.5},
                "fields away from the defaults")

    # the object form merges
    await ed.call("set_scene_settings", scene=sid, settings={"post_processing": {"fxaa_enabled": True, "renderScale": 0.75}})
    s = (await ed.call("get_scene_settings", scene=sid, names=["post_processing"]))["settings"]["post_processing"]
    ed.check_eq(s.get("fxaa_enabled"), True)
    ed.check_eq(s.get("render_scale"), 0.75)
    ed.check_eq(s.get("tonemap"), "aces", "merged, not replaced")

    await ed.call("save_scene", scene=sid)
    text = saved_text(ed)
    ed.check("postProcessing:" in text, "saved")
    for key in ("tonemap: aces", "bloomEnabled: true", "bloomIntensity: 2.5", "fxaaEnabled: true", "renderScale: 0.75"):
        ed.check(key in text, f"{key} saved")
    ed.check("vignetteEnabled" not in text, "default fields are not saved")

    # undo of the object call, then of the field call
    await ed.call("undo")
    s = (await ed.call("get_scene_settings", scene=sid, names=["post_processing"]))["settings"]["post_processing"]
    ed.check("fxaa_enabled" not in s, "undo removes the object call's fields")
    ed.check_eq(s.get("bloom_intensity"), 2.5, "the earlier call stays")

    # refused values change nothing
    await ed.check_error("set_scene_settings", scene=sid, settings={"post_processing.tonemap": "filmic"}, contains="agx")
    await ed.check_error("set_scene_settings", scene=sid, settings={"post_processing.render_scale": 0.25}, contains="0.5")
    await ed.check_error("set_scene_settings", scene=sid, settings={"post_processing.msaa_samples": 3}, contains="1, 2 or 4")
    await ed.check_error("set_scene_settings", scene=sid, settings={"post_processing": {"bogus": 1}}, contains="unknown field")
    await ed.check_error("set_scene_settings", scene=sid, settings={"post_processing.lut_texture": "nope.png"}, contains="no file")

    # null resets everything; the key leaves the file
    await ed.call("set_scene_settings", scene=sid, settings={"post_processing": None})
    s = (await ed.call("get_scene_settings", scene=sid, names=["post_processing"]))["settings"]["post_processing"]
    ed.check_eq(s, {}, "reset")
    await ed.call("save_scene", scene=sid)
    ed.check("postProcessing" not in saved_text(ed), "reset: the key is gone")
