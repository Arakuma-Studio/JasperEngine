"""S85 AC16 (R27): an .anim keying titleart.animationTime scrubs a Drive Animation in Play."""

import harness
from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Title Animation Clips"}
TIMEOUT = 600

DOC = "assets/driven.titleart"
CLIP = "assets/anim/scrub.anim"


async def test_a_clip_scrubs_the_drive_animation(ed: Editor):
    await ed.call("tool_groups", enable=["title_art", "animation"])
    await ed.call("set_project_settings", values={"title_art.auto_bake": False})
    await ed.call("edit_title", path=DOC, create={"text": "WIN"}, bake="never", ops=[
        {"op": "add_animation", "name": "Intro", "duration": 1.0},
        {"op": "set_track", "animation": "Intro", "field": "opacity", "keys": [[0, 0], [1, 1]]},
    ])
    await ed.call("export_title", path=DOC)
    made = await ed.call("create_scene", name="Hud", type="UI", path="scenes/Hud")
    sid = made["scene"]["id"]
    await ed.call("create_ui", scene=sid, root={"type": "image", "name": "Logo", "layout": {"anchor": "center", "size": [256, 128]}})
    await ed.call("add_component", scene=sid, entity="Logo", component="titleart")
    await ed.call("set_properties", scene=sid, entity="Logo", values={"titleart": {"title": DOC, "driveAnimation": "Intro", "playAutomatically": False}})

    got = await ed.call("edit_animation", scene=sid, clip=CLIP, create={}, root="Logo", length=1.0, tracks=[
        {"path": "", "channel": "property", "component": "titleart", "property": "animationTime", "keys": [{"time": 0, "value": 0}, {"time": 1, "value": 1}]},
    ])
    bindings = got.get("bindings") or {}
    ed.check(bindings.get("resolved") == bindings.get("total") == 1, f"animationTime binds: {bindings!r}")
    await ed.call("add_component", scene=sid, entity="Logo", component="animationplayer",
                  values={"animationplayer": {"clips": [{"clip": CLIP}], "playAutomatically": True}})
    await ed.call("save_scene", scene=sid)

    await ed.call("play", action="start", scene=sid)
    try:
        await ed.call("wait", frames=8)
        early = await ed.call("get_title", entity="Logo", scene=sid)
        await ed.call("wait", frames=20)
        later = await ed.call("get_title", entity="Logo", scene=sid)
        ed.check(early["animation_time"] < later["animation_time"], f"the clip advances the driven time: {early['animation_time']} -> {later['animation_time']}")
        ed.check(later["animation_time"] <= 1.0, str(later["animation_time"]))
    finally:
        await ed.call("play", action="stop")
