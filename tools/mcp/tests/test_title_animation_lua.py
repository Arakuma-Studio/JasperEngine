"""S85 AC15 (R28-R31): a title's animations from Lua in Play: play, overlay, stop and reset, markers
(onAnimationEvent, the script method, sounds), the next chain, unknown names and time scales.
"""

import harness
from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Title Animation Lua"}
TIMEOUT = 900

DOC = "assets/anim_lua.titleart"
S: dict = {}


async def setup(ed: Editor) -> dict:
    if S.get("ed") is ed:
        return S
    S.clear()
    S["ed"] = ed
    await ed.call("tool_groups", enable=["title_art"])
    await ed.call("set_project_settings", values={"title_art.auto_bake": False})
    ops = [
        {"op": "add_animation", "name": "Idle", "duration": 0.5, "wrap": "loop"},
        {"op": "add_animation", "name": "Intro", "duration": 0.4, "next": "Idle"},
        {"op": "set_track", "animation": "Intro", "field": "opacity", "keys": [[0.0, 0.0], [0.4, 1.0]]},
        {"op": "set_marker", "animation": "Intro", "name": "hit", "time": 0.2, "args": [1, "x"]},
        {"op": "set_track", "animation": "Idle", "field": "transform.rotation", "keys": [[0.0, 0.0], [0.5, 10.0]]},
        {"op": "add_animation", "name": "Over", "duration": 0.3, "overlay": True},
        {"op": "set_track", "animation": "Over", "field": "flash.amount", "keys": [[0.0, 1.0], [0.3, 0.0]]},
    ]
    await ed.call("edit_title", path=DOC, create={"text": "WIN", "canvas": [512, 256]}, bake="never")
    await ed.call("edit_title", path=DOC, ops=ops, bake="never")
    await ed.call("export_title", path=DOC)
    made = await ed.call("create_scene", name="Hud", type="UI", path="scenes/Hud")
    S["scene"] = made["scene"]["id"]
    await ed.call("create_ui", scene=S["scene"], root={"type": "image", "name": "Logo", "layout": {"anchor": "center", "size": [256, 128]}})
    await ed.call("add_component", scene=S["scene"], entity="Logo", component="titleart")
    await ed.call("set_properties", scene=S["scene"], entity="Logo", values={"titleart": {"title": DOC}})
    await ed.call("save_scene", scene=S["scene"])
    return S


async def lua(ed: Editor, code: str):
    return (await ed.call("eval_lua", code=code))["values"]


async def play_session(ed: Editor):
    s = await setup(ed)
    await ed.call("play", action="start", scene=s["scene"])
    await ed.call("wait", frames=3)
    return s


async def test_play_events_and_next_chain(ed: Editor):
    await play_session(ed)
    try:
        await lua(ed, (
            "events = {}\n"
            "local t = object('Logo'):getTitleArt()\n"
            "t.onAnimationEvent:add('rec', function(e) events[#events + 1] = e.functionName end)\n"
            "t:play('Intro')\n"
            "return t.animation"))
        values = await lua(ed, "return object('Logo'):getTitleArt().isPlaying, object('Logo'):getTitleArt().animation")
        ed.check(values[0], "playing after play")
        ed.check_eq(values[1], "Intro")
        # 0.2 s to the marker; Intro chains into Idle at its end
        await ed.call("wait", frames=45)
        values = await lua(ed, "return table.concat(events, ','), object('Logo'):getTitleArt().animation")
        ed.check_in("hit", values[0], "the marker fired")
        ed.check_eq(values[1], "Idle", "next chains into Idle")
    finally:
        await ed.call("play", action="stop")


async def test_overlay_stop_reset_and_unknown_warns(ed: Editor):
    await play_session(ed)
    try:
        await lua(ed, "local t = object('Logo'):getTitleArt(); t:play('Idle'); t:playOverlay('Over')")
        values = await lua(ed, "local t = object('Logo'):getTitleArt(); return t.animation, t:isAnimationPlaying('Over')")
        ed.check_eq(values[0], "Idle", "an overlay leaves the base")
        ed.check(values[1], "the overlay plays")
        await lua(ed, "object('Logo'):getTitleArt():stop('Over')")
        ed.check((await lua(ed, "return object('Logo'):getTitleArt():isAnimationPlaying('Over')"))[0] is False, "the overlay stopped")
        await lua(ed, "local t = object('Logo'):getTitleArt(); t:reset(); return t.animationTime")
        ed.check((await lua(ed, "return object('Logo'):getTitleArt().isPlaying"))[0] is False, "reset stops")
        # an unknown name warns once and does nothing
        await lua(ed, "local t = object('Logo'):getTitleArt(); t:play('Nope'); t:play('Nope')")
        await ed.call("wait", frames=2)
        console = await ed.call("read_console")
        count = sum(1 for e in console["entries"] if "has no animation 'Nope'" in e["message"])
        ed.check_eq(count, 1, "the unknown name warned once")
    finally:
        await ed.call("play", action="stop")


async def test_unscaled_and_speed(ed: Editor):
    await play_session(ed)
    try:
        await lua(ed, "local t = object('Logo'):getTitleArt(); t:play('Idle', {speed = 2, unscaled = true})")
        await ed.call("wait", frames=10)
        before = (await lua(ed, "return object('Logo'):getTitleArt().animationTime"))[0]
        await lua(ed, "Engine.timeScale = 0")
        await ed.call("wait", frames=10)
        after = (await lua(ed, "return object('Logo'):getTitleArt().animationTime"))[0]
        ed.check(after != before, f"an unscaled animation runs while the scene's time is stopped ({before} -> {after})")
    finally:
        await lua(ed, "Engine.timeScale = 1")
        await ed.call("play", action="stop")
