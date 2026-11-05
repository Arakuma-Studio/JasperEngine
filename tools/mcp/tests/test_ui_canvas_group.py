"""Canvas Group (feature ui-canvas-group, spec section 10, MCP e2e): create_ui with canvas_group, the
group alpha in Game view pixels, canvas_group_state in get_entity / get_runtime_entity, click-through
and non-interactable groups driven with send_input and read back with eval_lua, the Lua fadeOut with
deactivate, the Component catalogue names, and the Flatten render mode's overlap pixels.

A UI scene over black (canvas 1280 x 720) on an owned empty project. Pixel values are 0-255 sRGB
as the Game view stores them: white at alpha 0.5 over black reads ~128.
"""

import asyncio
import os
import time

from harness import Editor, skip
from htmlui_pixels import Shot

EDITOR = {"create": True, "name": "Canvas Group"}
CI_EXCLUDED = ("long-running (about 40 minutes on a loaded server: a test builds and runs a real Desktop player); "
               "in the full suite (MCP_SUITE=all) on the build servers")

TIMEOUT = 900

STATE = {}

# canvas pixels (top-left origin)
P_WHITE = (150, 150)      # the panel's white image
P_OVERLAP = (150, 300)    # Front (in the panel) over Back (outside it)
P_BUTTON = (380, 130)
P_FLAT_OVERLAP = (765, 165)   # the overlap of the two squares of the Flatten board
P_FLAT_SINGLE = (715, 115)


async def shoot(ed: Editor, label: str) -> Shot:
    shot = await ed.call_raw("screenshot", {"view": "game", "max_width": 1280})
    ed.check(not shot.is_error and shot.images, f"{label}: no screenshot: {shot.text[:300]}")
    png = shot.images[0][0]
    (ed.scratch.root / f"canvas-group-{label}.png").write_bytes(png)
    return Shot(png, shot.value)


def grey(shot: Shot, point) -> int:
    r, g, b = shot.px(*point)
    return round((r + g + b) / 3)


async def scene(ed: Editor) -> str:
    if "scene" in STATE:
        return STATE["scene"]
    await ed.call("set_project_settings", values={"graphics.canvas_width": 1280, "graphics.canvas_height": 720,
                                                  "graphics.scaling_mode": "letterbox"})
    made = await ed.call("create_scene", name="Groups", type="UI")
    sid = made["scene"]["id"]
    await ed.call("set_scene_settings", scene=sid, settings={"background_color": [0, 0, 0, 1]})
    # Back is outside the panel, under Front (which is in it)
    await ed.call("create_ui", scene=sid, root={
        "type": "image", "name": "Back", "color": [0, 0, 1, 1],
        "layout": {"anchor": "top_left", "position": [100, 250], "size": [100, 100]}})
    made = await ed.call("create_ui", scene=sid, root={
        "type": "container", "name": "Panel", "canvas_group": {"alpha": 0.5},
        "layout": {"anchor": "top_left", "position": [0, 0], "size": [600, 400]},
        "children": [
            {"type": "image", "name": "White", "color": [1, 1, 1, 1],
             "layout": {"anchor": "top_left", "position": [100, 100], "size": [100, 100]}},
            {"type": "image", "name": "Front", "color": [1, 0, 0, 1],
             "layout": {"anchor": "top_left", "position": [100, 250], "size": [100, 100]}},
            {"type": "button", "name": "Btn", "text": "OK",
             "layout": {"anchor": "top_left", "position": [300, 100], "size": [160, 60]}},
        ]})
    ed.check(made, "create_ui made the panel")
    await ed.call("save_scene", scene=sid, path="scenes/Groups")
    STATE["scene"] = sid
    return sid


async def flatten_state(ed: Editor, sid) -> dict:
    got = await ed.call("get_entity", scene=sid, entity="Flat")
    state = got.get("canvas_group_state") or {}
    ed.check("flatten" in state, f"a FLATTEN group reports its layer: {state!r}")
    return state["flatten"]


async def lua(ed: Editor, code: str):
    return (await ed.call("eval_lua", code=code))["values"]


async def test_group_alpha_in_pixels_and_state(ed: Editor):
    sid = await scene(ed)
    shot = await shoot(ed, "alpha-half")
    white = grey(shot, P_WHITE)
    ed.check(abs(white - 128) <= 6, f"white at group alpha 0.5 over black reads {white}, expected ~128")

    got = await ed.call("get_entity", scene=sid, entity="Panel/White")
    state = got.get("canvas_group_state")
    ed.check(state and abs(state["inherited_alpha"] - 0.5) < 1e-4, f"canvas_group_state: {state!r}")
    ed.check(state["group"] == "Panel" and state["interactable"] and state["blocks_raycasts"], f"canvas_group_state: {state!r}")

    # alpha 0: the background
    await ed.call("set_properties", scene=sid, entity="Panel", values={"canvasgroup": {"alpha": 0}})
    shot = await shoot(ed, "alpha-zero")
    ed.check(grey(shot, P_WHITE) <= 6, f"group alpha 0 shows the background, got {shot.px(*P_WHITE)}")
    # writes are clamped
    await ed.call("set_properties", scene=sid, entity="Panel", values={"canvasgroup": {"alpha": 3}})
    props = await ed.call("get_entity", scene=sid, entity="Panel", components=["canvasgroup"])
    alpha = props["components"]["canvasgroup"]["alpha"]
    ed.check(abs((alpha["value"] if isinstance(alpha, dict) else alpha) - 1.0) < 1e-6, f"alpha clamped to 1: {alpha!r}")
    await ed.call("set_properties", scene=sid, entity="Panel", values={"canvasgroup": {"alpha": 0.5}})
    await ed.call("save_scene", scene=sid, path="scenes/Groups")


async def test_catalogue_names(ed: Editor):
    sid = await scene(ed)
    types = await ed.call("list_component_types")
    text = repr(types)
    ed.check("canvasgroup" in text and "Canvas Group" in text, "list_component_types lists the Canvas Group")
    made = await ed.call("create_entity", scene=sid, type="canvas_group", name="Container")
    ent = made["entity_id"]
    await ed.call("remove_component", scene=sid, entity=ent, component="Canvas Group")
    await ed.call("add_component", scene=sid, entity=ent, component="Canvas Group")
    got = await ed.call("get_entity", scene=sid, entity=ent, components=["canvasgroup"])
    ed.check("canvasgroup" in got["components"], f"add_component \"Canvas Group\" resolved: {list(got['components'])}")
    await ed.call("delete_entities", scene=sid, entity=ent)
    await ed.call("save_scene", scene=sid, path="scenes/Groups")


async def test_play_click_through_and_interactable(ed: Editor):
    sid = await scene(ed)
    started = await ed.call("play", action="start", scene=sid)
    ed.check(started, "play started")
    try:
        await ed.call("wait", frames=3)
        await lua(ed, """
            clicks = {back = 0, front = 0, btn = 0}
            object('Back', 'Image'):getUIComponent().onClick = function() clicks.back = clicks.back + 1 end
            object('Panel/Front', 'Image'):getUIComponent().onClick = function() clicks.front = clicks.front + 1 end
            object('Panel/Btn', 'Button'):getUIComponent().onClick = function() clicks.btn = clicks.btn + 1 end
            return true""")
        runtime = await ed.call("get_runtime_entity", entity="Panel/White")
        state = runtime.get("canvas_group_state")
        ed.check(state and abs(state["inherited_alpha"] - 0.5) < 1e-4, f"runtime canvas_group_state: {state!r}")

        # a blocking group: the front image is hit
        await ed.call("send_input", events=[{"type": "mouse", "action": "click", "x": P_OVERLAP[0], "y": P_OVERLAP[1]}],
                      space="canvas", frames_after=2)
        got = await lua(ed, "return clicks")
        ed.check(got[0]["front"] == 1 and got[0]["back"] == 0, f"blocking: the front is clicked: {got!r}")

        # click-through: the back one
        await ed.call("set_runtime_property", entity="Panel", component="canvasgroup", property="blocksRaycasts", value=False)
        await ed.call("send_input", events=[{"type": "mouse", "action": "click", "x": P_OVERLAP[0], "y": P_OVERLAP[1]}],
                      space="canvas", frames_after=2)
        got = await lua(ed, "return clicks")
        ed.check(got[0]["back"] == 1 and got[0]["front"] == 1, f"click-through: the back is clicked: {got!r}")
        await ed.call("set_runtime_property", entity="Panel", component="canvasgroup", property="blocksRaycasts", value=True)

        # a button counts, then not in a non-interactable group (and shows the disabled grey)
        await ed.call("send_input", events=[{"type": "mouse", "action": "click", "x": P_BUTTON[0], "y": P_BUTTON[1]}],
                      space="canvas", frames_after=2)
        before = (await lua(ed, "return clicks.btn"))[0]
        ed.check(before == 1, f"the button clicks when interactable: {before}")
        normal = await shoot(ed, "button-normal")
        await ed.call("set_runtime_property", entity="Panel", component="canvasgroup", property="interactable", value=False)
        await ed.call("send_input", events=[{"type": "mouse", "action": "click", "x": P_BUTTON[0], "y": P_BUTTON[1]}],
                      space="canvas", frames_after=2)
        after = (await lua(ed, "return clicks.btn"))[0]
        ed.check(after == 1, f"no click on a non-interactable button: {after}")
        disabled = await shoot(ed, "button-disabled")
        p = (P_BUTTON[0] - 60, P_BUTTON[1] - 20)
        ed.check(grey(disabled, p) < grey(normal, p) - 10, f"the disabled grey: {normal.px(*p)} -> {disabled.px(*p)}")
        ed.check((await lua(ed, "return RectTransform(scene, entity('Panel/Btn')):isInteractable()"))[0] is False,
                 "isInteractable() is false")

        # fadeOut with deactivate (GamePowerups)
        await lua(ed, "CanvasGroup(scene, entity('Panel')):fadeOut(0.25, {deactivate = true}); return true")
        await ed.call("wait", game_seconds=0.4)
        got = await lua(ed, "local g = CanvasGroup(scene, entity('Panel')); return {g.alpha, GameObject(scene, entity('Panel')):isActive()}")
        ed.check(abs(got[0][0]) < 1e-6 and got[0][1] is False, f"faded out and deactivated: {got!r}")
    finally:
        await ed.call("play", action="stop")


async def test_flatten_overlap_pixels(ed: Editor):
    sid = await scene(ed)
    # two overlapping opaque white squares in a group at 0.5 over black
    await ed.call("create_ui", scene=sid, root={
        "type": "container", "name": "Flat", "canvas_group": {"alpha": 0.5, "render_mode": "per_element"},
        "layout": {"anchor": "top_left", "position": [700, 100], "size": [200, 200]},
        "children": [
            {"type": "image", "name": "A", "color": [1, 1, 1, 1], "layout": {"anchor": "top_left", "position": [0, 0], "size": [100, 100]}},
            {"type": "image", "name": "B", "color": [1, 1, 1, 1], "layout": {"anchor": "top_left", "position": [50, 50], "size": [100, 100]}},
        ]})
    shot = await shoot(ed, "per-element")
    overlap = grey(shot, P_FLAT_OVERLAP)
    ed.check(abs(overlap - 191) <= 4, f"Per Element: the overlap stacks, {overlap} (expected ~191)")
    ed.check(abs(grey(shot, P_FLAT_SINGLE) - 128) <= 4, f"Per Element single: {shot.px(*P_FLAT_SINGLE)}")

    await ed.call("set_properties", scene=sid, entity="Flat", values={"canvasgroup": {"renderMode": "flatten"}})
    shot = await shoot(ed, "flatten")
    overlap = grey(shot, P_FLAT_OVERLAP)
    ed.check(abs(overlap - 128) <= 4, f"Flatten: the overlap looks like one layer, {overlap} (expected ~128)")
    ed.check(abs(grey(shot, P_FLAT_SINGLE) - 128) <= 4, f"Flatten single: {shot.px(*P_FLAT_SINGLE)}")
    # the panel elsewhere is unchanged
    ed.check(abs(grey(shot, P_WHITE) - 128) <= 6, f"the other group: {shot.px(*P_WHITE)}")
    flat = await flatten_state(ed, sid)
    ed.check(flat["layered"] and flat["scene_layers_last_frame"] >= 1, f"the runtime switch made a layer: {flat!r}")

    # the Scene view and the Game view both draw it: each keeps its pooled target, none is
    # created again frame after frame
    await ed.call_raw("screenshot", {"view": "scene", "max_width": 640})
    await shoot(ed, "flatten-again")
    before = (await flatten_state(ed, sid))["scene_layer_targets_created"]
    for _ in range(3):
        await ed.call_raw("screenshot", {"view": "scene", "max_width": 640})
        await shoot(ed, "flatten-views")
    after = await flatten_state(ed, sid)
    ed.check(after["scene_layer_targets_created"] == before, f"layer targets recreated across frames: {before} -> {after!r}")

    # at alpha 1 nothing is layered (spec 14) and the squares are white
    await ed.call("set_properties", scene=sid, entity="Flat", values={"canvasgroup": {"alpha": 1}})
    shot = await shoot(ed, "flatten-opaque")
    ed.check(grey(shot, P_FLAT_OVERLAP) >= 250, f"Flatten at alpha 1: {shot.px(*P_FLAT_OVERLAP)}")
    flat = await flatten_state(ed, sid)
    ed.check(not flat["layered"] and flat["scene_layers_last_frame"] == 0, f"no layer at alpha 1: {flat!r}")
    await ed.call("delete_entities", scene=sid, entity="Flat")
    await ed.call("save_scene", scene=sid, path="scenes/Groups")


async def test_stubs_know_canvas_group(ed: Editor):
    hits = await ed.call("search_engine_api", query="CanvasGroup fadeOut", max_results=5)
    ed.check("fadeOut" in repr(hits), f"the API search finds the fade helpers: {hits!r}"[:400])


FLATTEN_SCRIPT = """local AddFlatten = {}
function AddFlatten:start()
    -- a FLATTEN group made by a script in a scene that has none (spec 7.10: the export ships the
    -- composite whatever the scenes hold)
    local group = CanvasGroup(self.scene)
    group.alpha = 0.5
    group.renderMode = CanvasGroupRenderMode.FLATTEN
    local root = group:getGameObject()
    for i = 0, 1 do
        local image = Image(self.scene)
        image:setSize(200, 200)
        image.color = Vector4(1, 1, 1, 1)
        image.position = Vector3(100 + i * 100, 100 + i * 100, 0)
        root:addChild(image)
        image.entityOwned = false
    end
    group.entityOwned = false
    print("flatten group added")
end
return AddFlatten
"""


async def test_export_script_added_flatten_group(ed: Editor):
    """An exported player where a script adds a FLATTEN group to a scene without any composites
    it: the overlap reads like one layer (~128 over black), not stacked (~191)."""
    if os.environ.get("JASPER_SLOW_BUILD", "") in ("", "0"):
        skip("set JASPER_SLOW_BUILD=1 to build and run a real Desktop player (compiles the engine)")
    made = await ed.call("create_scene", name="Exported", type="UI")
    sid = made["scene"]["id"]
    await ed.call("set_scene_settings", scene=sid, settings={"background_color": [0, 0, 0, 1]})
    holder = (await ed.call("create_entity", scene=sid, type="empty", name="Holder"))["entity_id"]
    await ed.call("create_script", kind="lua", name="AddFlatten", content=FLATTEN_SCRIPT, scene=sid, entity=holder)
    await ed.call("save_scene", scene=sid, path="scenes/Exported")
    # every open scene is saved (the project's untitled default too: a build generates them all)
    await ed.call("save_project", untitled_scene_dir="scenes")
    await ed.call("edit_build_scenes", action="set_start", scene=sid)
    await ed.call("set_project_settings", values={
        "graphics.runtime_shaders": False,
        "preferences.build_jobs": 5,
        "player.window_width": 1280,
        "player.window_height": 720,
    })
    out = ed.scratch.root / "export-flatten"
    t0 = time.monotonic()
    r = await ed.call("build_project", mode="desktop", output_dir=str(out), build_jobs=5, timeout_seconds=30)
    job = r
    while job["status"] == "running":
        await asyncio.sleep(15)
        job = await ed.call("get_job", job_id=r["job_id"], log_lines=10)
        ed.check(time.monotonic() - t0 < 7200, f"build still running: {job}")
    ed.check(job["status"] == "succeeded", f"{job}"[:2000])
    raw = await ed.call_raw("run_build", {"seconds": 6, "screenshot_after_seconds": 4, "max_width": 1280})
    ed.check(not raw.is_error and not raw.value["crashed"], raw.text[:1500])
    ed.check("flatten group added" in str(raw.value["log"]), f"the script ran: {raw.value['log']!r}"[:1500])
    png = raw.images[0][0]
    (ed.scratch.root / "canvas-group-export.png").write_bytes(png)
    from htmlui_pixels import decode_png
    w, h, ch, rows = decode_png(png)
    sx, sy = w / 1280, h / 720
    def px(x, y):
        row = rows[int(y * sy)]
        i = int(x * sx) * ch
        return round(sum(row[i:i + 3]) / 3)
    overlap, single = px(250, 250), px(150, 150)
    ed.check(abs(overlap - 128) <= 6 and abs(single - 128) <= 6,
             f"the export composites the layer: overlap {overlap}, single {single} (expected ~128 both)")
