"""Text in every language (unicode-text spec, docs/superpowers/specs/2026-11-04-unicode-text-design.md):
typing as Windows and Android deliver it (send_input per_unit: one UTF-16 unit per event) keeps an
emoji whole in a component InputField and in Engine.onCharInput; paths in project.yaml are stored
with '/'; editor_status reports the editor's fallback fonts and the CJK font preference round-trips.

A UI scene over black (canvas 1280 x 720) on an owned empty project.
"""

import asyncio
import json

from harness import Editor

EDITOR = {"create": True, "name": "Unicode Text"}
CI = True

TIMEOUT = 600

EMOJI = "\U0001F600"
P_FIELD = (200, 120)   # canvas pixels inside the field


async def lua(ed: Editor, code: str):
    return (await ed.call("eval_lua", code=code))["values"]


async def test_per_unit_typing_keeps_emoji(ed: Editor):
    ed.require_tools("create_ui", "play", "send_input", "eval_lua")
    await ed.call("set_project_settings", values={"graphics.canvas_width": 1280, "graphics.canvas_height": 720,
                                                  "graphics.scaling_mode": "letterbox"})
    made = await ed.call("create_scene", name="Typing", type="UI")
    sid = made["scene"]["id"]
    await ed.call("create_ui", scene=sid, root={
        "type": "input_field", "name": "Field", "placeholder": "name",
        "layout": {"anchor": "top_left", "position": [100, 100], "size": [400, 40]}})
    await ed.call("save_scene", scene=sid, path="scenes/Typing")
    ed.check(await ed.call("play", action="start", scene=sid), "play started")
    try:
        await ed.call("wait", frames=3)
        await lua(ed, """
            typed = {}
            Engine.onCharInput:add("unicode-text-probe", function(c) typed[#typed + 1] = c end)
            return true""")
        sent = await ed.call_raw("send_input", {"events": [
            {"type": "mouse", "action": "click", "x": P_FIELD[0], "y": P_FIELD[1]},
            {"type": "text", "text": "a" + EMOJI + "b", "per_unit": True}], "frames_after": 2})
        ed.check(not sent.is_error, f"send_input per_unit: {sent.text[:300]}")
        values = await lua(ed, "return object('Field', 'InputField').text, #typed, typed[1], typed[2], typed[3]")
        ed.check_eq(values[0], "a" + EMOJI + "b", "the InputField holds the emoji whole")
        ed.check_eq(values[1:], [3, "a", EMOJI, "b"], "onCharInput got three whole characters")

        # a backspace erases the whole emoji, not half of it
        await ed.call("send_input", events=[{"type": "key", "key": "backspace"},
                                            {"type": "text", "text": "\b", "per_unit": True}], frames_after=2)
        text = (await lua(ed, "return object('Field', 'InputField').text"))[0]
        ed.check(text in ("a" + EMOJI, "a"), f"backspaces remove whole characters: {text!r}")
        ed.check("�" not in text, f"no replacement character: {text!r}")

        refused = await ed.call_raw("send_input", {"events": [{"type": "text", "text": "x", "per_unit": "yes"}]})
        ed.check(refused.is_error and "per_unit" in refused.text, f"a non-boolean per_unit is refused: {refused.text[:200]}")
    finally:
        await lua(ed, "Engine.onCharInput:remove('unicode-text-probe'); return true")
        await ed.call("play", action="stop")


async def test_project_yaml_paths_are_portable(ed: Editor):
    ed.require_tools("set_project_settings", "write_file")
    (ed.project / "icons").mkdir(exist_ok=True)
    (ed.project / "icons" / "app.png").write_bytes(b"\x89PNG\r\n\x1a\n")
    # as an agent on Windows writes it: a backslash is a separator on every platform
    await ed.call("set_project_settings", values={"player.window_icon": "icons\\app.png"})
    text = (ed.project / "project.yaml").read_text(encoding="utf-8")
    ed.check("icons/app.png" in text and "icons\\app.png" not in text,
             f"project.yaml stores the icon with '/': {[l for l in text.splitlines() if 'Icon' in l]}")


async def test_editor_fonts_status_and_preference(ed: Editor):
    ed.require_tools("editor_status", "get_project_settings", "set_project_settings")
    fonts = None
    for _ in range(50):
        fonts = (await ed.call("editor_status"))["editor_fonts"]
        if fonts["state"] in ("ready", "none"):
            break
        await asyncio.sleep(0.1)
    ed.check(fonts["state"] in ("ready", "none"), f"the font search ended: {json.dumps(fonts)}")
    ed.check(fonts["state"] != "ready" or fonts["fallbacks"], f"ready means a fallback: {json.dumps(fonts)}")
    ed.check(fonts["effective_language"] in ("ja", "ko", "zh-Hans", "zh-Hant"), f"effective_language: {fonts}")

    got = await ed.call("get_project_settings", keys=["preferences.cjk_font_language"])
    ed.check_eq(got.get("settings", got)["preferences.cjk_font_language"], "auto", "the preference defaults to auto")
    await ed.call("set_project_settings", values={"preferences.cjk_font_language": "korean"})
    got = await ed.call("get_project_settings", keys=["preferences.cjk_font_language"])
    ed.check_eq(got.get("settings", got)["preferences.cjk_font_language"], "korean", "the preference is set")
    status = (await ed.call("editor_status"))["editor_fonts"]
    ed.check_eq(status["cjk_font_language"], "korean", "editor_status reports the preference")
    await ed.check_error("set_project_settings", values={"preferences.cjk_font_language": "klingon"})
    await ed.call("set_project_settings", values={"preferences.cjk_font_language": "auto"})
