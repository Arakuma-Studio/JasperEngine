"""Component UI Buttons (feature ui-components, spec section 10, MCP e2e): Button edits from MCP
refresh the button and a ui.color / ui.texture it overwrites is refused; create_ui's texture and
Selectable keys; the Canvas Renderer rows a transition owns are read-only in the Inspector; a
9-patch button's label is not cut by its margins (Game view pixels); get_entity's resolved
navigation; and a keyboard and gamepad menu in Play (selection, Submit, the stick back to the
game) read back with get_play_state, get_runtime_entity and eval_lua.

A UI scene over black (canvas 1280 x 720) on an owned empty project.
"""

import struct
import zlib

from harness import Editor

EDITOR = {"create": True, "name": "UI Buttons"}
CI = True
TIMEOUT = 600

STATE = {}


def png(w: int, h: int, rgb=(255, 255, 255)) -> bytes:
    def chunk(tag: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + bytes(rgb) * w for _ in range(h))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


async def scene(ed: Editor) -> int:
    if "scene" in STATE:
        return STATE["scene"]
    (ed.project / "assets").mkdir(parents=True, exist_ok=True)
    (ed.project / "assets" / "btn.png").write_bytes(png(8, 8, (255, 255, 255)))
    (ed.project / "assets" / "btn_pressed.png").write_bytes(png(8, 8, (90, 90, 90)))
    await ed.call("set_project_settings", values={"graphics.canvas_width": 1280, "graphics.canvas_height": 720,
                                                  "graphics.scaling_mode": "letterbox"})
    made = await ed.call("create_scene", name="Buttons", type="UI")
    sid = made["scene"]["id"]
    await ed.call("set_scene_settings", scene=sid, settings={"background_color": [0, 0, 0, 1]})
    await ed.call("save_scene", scene=sid, path="scenes/Buttons")
    STATE["scene"] = sid
    return sid


def component_values(got: dict, component: str) -> dict:
    values = got["components"][component]
    return {k: (v["value"] if isinstance(v, dict) and "value" in v else v) for k, v in values.items()}


async def frame(ed: Editor):
    # the editor lays the edited scene out each frame it draws
    await ed.call("screenshot", view="game", max_width=320)


async def test_button_field_edit_refreshes_and_ui_color_refused(ed: Editor):
    sid = await scene(ed)
    await ed.call("create_ui", scene=sid, root={
        "type": "button", "name": "Edit", "text": "Edit",
        "layout": {"anchor": "top_left", "position": [40, 600], "size": [160, 60]}})
    await ed.call("set_properties", scene=sid, entity="Edit", values={"button": {"colorNormal": [1, 0, 0, 1]}})
    await frame(ed)
    got = await ed.call("get_entity", scene=sid, entity="Edit", components=["ui"])
    color = component_values(got, "ui")["color"]
    ed.check([round(c, 3) for c in color] == [1, 0, 0, 1], f"an MCP edit of button.colorNormal redraws the button: ui.color {color!r}")

    refused = await ed.call_raw("set_properties", {"scene": sid, "entity": "Edit", "values": {"ui": {"color": [0, 1, 0, 1]}}})
    ed.check(refused.is_error and "button.colorNormal" in refused.text, f"ui.color on a Button is refused: {refused.text[:300]}")

    # once the transition leaves the colour alone, it is the image's to set
    await ed.call("set_properties", scene=sid, entity="Edit", values={"button": {"transition": "sprite_swap"},
                                                                     "ui": {"color": [0, 1, 0, 1]}})
    await frame(ed)
    got = await ed.call("get_entity", scene=sid, entity="Edit", components=["ui"])
    color = component_values(got, "ui")["color"]
    ed.check([round(c, 3) for c in color] == [0, 1, 0, 1], f"with sprite_swap ui.color is kept: {color!r}")
    await ed.call("delete_entities", scene=sid, entity="Edit")
    await ed.call("save_scene", scene=sid, path="scenes/Buttons")


async def test_create_ui_button_keys(ed: Editor):
    sid = await scene(ed)
    made = await ed.call("create_ui", scene=sid, root={
        "type": "button", "name": "Keys", "text": "Keys", "texture": "assets/btn.png",
        "textures": {"pressed": "assets/btn_pressed.png"}, "transition": "color_tint_and_sprite_swap",
        "fade_duration": 0.1, "scale": {"hovered": 1.05}, "colors": {"selected": [1, 1, 0, 1]},
        "navigation": {"mode": "vertical", "wrap_around": True}, "select_on_enable": True,
        "layout": {"anchor": "top_left", "position": [240, 600], "size": [160, 60]}})
    ed.check(made, "create_ui made the button")
    got = await ed.call("get_entity", scene=sid, entity="Keys", components=["button"])
    button = component_values(got, "button")
    ed.check("btn.png" in str(button["textureNormal"]), f"texture sets the Button's Normal texture: {button['textureNormal']!r}")
    ed.check("btn_pressed.png" in str(button["texturePressed"]), f"textures.pressed: {button['texturePressed']!r}")
    ed.check(abs(button["fadeDuration"] - 0.1) < 1e-6 and abs(button["scaleHovered"] - 1.05) < 1e-6, f"fade, scale: {button!r}")
    ed.check(button["navigationMode"] == "vertical" and button["wrapAround"] is True and button["selectOnEnable"] is True,
             f"navigation and select_on_enable: {button!r}")
    bad = await ed.call_raw("create_ui", {"scene": sid, "root": {"type": "button", "name": "Bad", "navigation": "sideways"}})
    ed.check(bad.is_error, "an unknown navigation mode fails")
    bad = await ed.call_raw("create_ui", {"scene": sid, "root": {"type": "image", "name": "NoButton", "select_on_enable": True}})
    ed.check(bad.is_error and "for buttons" in bad.text, f"Selectable keys are for buttons: {bad.text[:200]}")
    await ed.call("delete_entities", scene=sid, entity="Keys")
    await ed.call("save_scene", scene=sid, path="scenes/Buttons")


async def test_inspector_rows_read_only(ed: Editor):
    sid = await scene(ed)
    await ed.call("tool_groups", enable=["editor_ux"])
    await ed.call("create_ui", scene=sid, root={
        "type": "button", "name": "Rows", "text": "Rows",
        "layout": {"anchor": "top_left", "position": [440, 600], "size": [160, 60]}})
    got = await ed.call("find_entities", scene=sid, name="Rows")
    entity = got["entities"][0]["id"]
    await ed.call("focus_window", window="inspector")
    await ed.call("select_entities", scene=sid, entities=[entity])
    layout = await ed.call("get_inspector_layout", scene=sid, entities=[entity])
    rows = {(r.get("component"), r.get("path")): r for r in layout["rows"]}
    color = next((r for (c, p), r in rows.items() if p == "color" and "ui" in str(c).lower()), None)
    ed.check(color is not None and color.get("read_only"), f"the Canvas Renderer's Color row is read-only: {color!r}")
    interactable = [r for r in layout["rows"] if r.get("label") == "Interactable"]
    ed.check(interactable, "the Button shows Unity's Interactable")
    await ed.call("delete_entities", scene=sid, entity="Rows")
    await ed.call("save_scene", scene=sid, path="scenes/Buttons")


async def test_nine_patch_label_not_clipped(ed: Editor):
    sid = await scene(ed)
    for name, x, margins in (("Plain", 700, 0), ("Patched", 950, 18)):
        await ed.call("create_ui", scene=sid, root={
            "type": "button", "name": name, "text": "PLAY", "font_size": 24,
            "layout": {"anchor": "top_left", "position": [x, 100], "size": [200, 40]}})
        await ed.call("set_properties", scene=sid, entity=name, values={"image": {
            "patchMarginTop": margins, "patchMarginBottom": margins, "patchMarginLeft": margins, "patchMarginRight": margins}})
    await frame(ed)
    from htmlui_pixels import Shot
    shot_raw = await ed.call_raw("screenshot", {"view": "game", "max_width": 1280})
    ed.check(not shot_raw.is_error and shot_raw.images, f"screenshot: {shot_raw.text[:200]}")
    png_bytes = shot_raw.images[0][0]
    (ed.scratch.root / "nine-patch-label.png").write_bytes(png_bytes)
    shot = Shot(png_bytes, shot_raw.value)

    def dark(x0: int) -> int:
        return sum(1 for x in range(x0 + 20, x0 + 180, 2) for y in range(100, 140) if sum(shot.px(x, y)) < 240)

    plain, patched = dark(700), dark(950)
    ed.check(plain > 20, f"the plain button's label draws ({plain} dark samples)")
    ed.check(patched >= plain * 0.8, f"the 9-patch button's label is whole: {patched} dark samples vs {plain}")
    for name in ("Plain", "Patched"):
        await ed.call("delete_entities", scene=sid, entity=name)
    await ed.call("save_scene", scene=sid, path="scenes/Buttons")


async def menu(ed: Editor, sid: int):
    await ed.call("create_ui", scene=sid, root={
        "type": "container", "name": "Menu",
        "layout": {"anchor": "top_left", "position": [100, 100], "size": [300, 400]},
        "children": [
            {"type": "button", "name": "Resume", "text": "Resume", "select_on_enable": True,
             "navigation": {"mode": "vertical", "wrap_around": True},
             "colors": {"selected": [1, 1, 0, 1]},
             "layout": {"anchor": "top_left", "position": [0, 0], "size": [300, 60]}},
            {"type": "button", "name": "Options", "text": "Options", "navigation": "vertical",
             "layout": {"anchor": "top_left", "position": [0, 100], "size": [300, 60]}},
            {"type": "button", "name": "Quit", "text": "Quit", "navigation": {"mode": "vertical", "wrap_around": True},
             "layout": {"anchor": "top_left", "position": [0, 200], "size": [300, 60]}},
        ]})


async def test_navigation_resolved_without_play(ed: Editor):
    sid = await scene(ed)
    await menu(ed, sid)
    await frame(ed)
    got = await ed.call("get_entity", scene=sid, entity="Menu/Resume")
    nav = (got.get("selectable") or {}).get("navigation") or {}
    ed.check(nav.get("down") and nav["down"]["path"].endswith("Options"), f"down from Resume: {nav!r}")
    ed.check(nav.get("up") and nav["up"]["path"].endswith("Quit"), f"Wrap Around: up from Resume is Quit: {nav!r}")
    ed.check(nav.get("left") is None, f"vertical: nothing left: {nav!r}")
    ed.check(nav.get("next") and nav["next"]["path"].endswith("Options"), f"Tab: {nav!r}")
    await ed.call("save_scene", scene=sid, path="scenes/Buttons")


async def lua(ed: Editor, code: str):
    return (await ed.call("eval_lua", code=code))["values"]


async def test_keyboard_and_gamepad_menu_in_play(ed: Editor):
    sid = await scene(ed)
    got = await ed.call("find_entities", scene=sid, name="Menu")
    if not got.get("entities"):
        await menu(ed, sid)
        await ed.call("save_scene", scene=sid, path="scenes/Buttons")
    started = await ed.call("play", action="start", scene=sid)
    ed.check(started, "play started")
    try:
        await ed.call("wait", frames=3)
        await lua(ed, """
            clicks = {resume = 0, options = 0, quit = 0}
            gameKeys = 0
            gameAxis = nil
            object('Menu/Resume', 'Button').onClick = function() clicks.resume = clicks.resume + 1 end
            object('Menu/Options', 'Button').onClick = function() clicks.options = clicks.options + 1 end
            object('Menu/Quit', 'Button').onClick = function() clicks.quit = clicks.quit + 1 end
            Engine.onKeyDown:add('t', function() gameKeys = gameKeys + 1 end)
            Engine.onGamepadAxisMove:add('t', function(pad, axis, value) if axis == Input.GAMEPAD_AXIS_LEFT_X then gameAxis = value end end)
            return true""")
        state = await ed.call("get_play_state")
        selected = state["input"]["ui_selected"]
        ed.check(selected and selected[0]["entity"]["path"].endswith("Resume"), f"Select On Enable: {selected!r}")
        runtime = await ed.call("get_runtime_entity", entity="Menu/Resume")
        ed.check((runtime.get("selectable") or {}).get("state") == "selected", f"Resume shows Selected: {runtime.get('selectable')!r}")

        await ed.call("send_input", events=[{"type": "key", "key": "down"}, {"type": "key", "key": "down"},
                                            {"type": "key", "key": "enter"}], frames_after=2)
        got = await lua(ed, "return {clicks.resume, clicks.options, clicks.quit, gameKeys}")
        ed.check(got[0] == [0, 0, 1, 0], f"down, down, Enter clicks Quit and the game got none of them: {got!r}")

        await ed.call("send_input", events=[{"type": "gamepad", "button": "dpad_down"}, {"type": "gamepad", "button": "a"}],
                      frames_after=2)
        got = await lua(ed, "return {clicks.resume, clicks.options, clicks.quit}")
        ed.check(got[0] == [1, 0, 1], f"the d-pad wraps to Resume and A clicks it: {got!r}")

        # the stick held while the menu goes reaches the game
        await ed.call("send_input", events=[{"type": "gamepad", "axis": "left_x", "value": 0.9}], frames_after=1)
        await lua(ed, "UI.setSelected(scene, nil); return true")
        await ed.call("wait", frames=2)
        got = await lua(ed, "return gameAxis")
        ed.check(got and got[0] is not None and abs(got[0] - 0.9) < 1e-3, f"the held stick is the game's again: {got!r}")
        await ed.call("send_input", events=[{"type": "gamepad", "axis": "left_x", "value": 0.0}], frames_after=1)

        # nothing selected: Space is the game's
        await ed.call("send_input", events=[{"type": "key", "key": "space"}], frames_after=1)
        got = await lua(ed, "return gameKeys")
        ed.check(got[0] == 1, f"with nothing selected the game gets the key: {got!r}")
        state = await ed.call("get_play_state")
        ed.check(state["input"]["ui_selected"] == [], f"no selection: {state['input']['ui_selected']!r}")
    finally:
        await ed.call("play", action="stop")
