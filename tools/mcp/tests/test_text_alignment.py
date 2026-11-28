"""Text Anchor (Unity TextMesh's), layout on demand and the em font size over MCP and Lua
(docs/specs/S94-rendering-anim-gaps.md F24-F26; AC24's MCP side, AC25's Lua play test, AC26's
field): a world-space Text reads Left / Bottom (the old anchor), set_properties writes the
anchor and the font size unit in one undo step each, the pivot booleans are gone from MCP (the
scene migration turns them into the anchor), Lua's TextAnchor table drives both properties and a
value of one is refused by the other, pivotBaseline / pivotCentered stay as Lua aliases, and
Text.width is final right after setText (R42: it read 0 until the first draw). S93's Alignment (the
lines inside the box) has its own keys and Lua properties beside the anchor."""

from harness import Editor

EDITOR = {"create": True, "name": "Text Anchor"}
CI = True

S: dict = {}


async def setup(ed: Editor) -> dict:
    if S.get("ed") is ed:
        return S
    S.clear()
    S["ed"] = ed
    made = await ed.call("create_scene", name="Plaques", type="3D", path="scenes/Plaques")
    S["scene"] = made["scene"]["id"]
    S["path"] = "scenes/Plaques.scene"
    await ed.call("create_entity", scene=S["scene"], type="camera", name="Cam", position=[0, 0, 300])
    await ed.call("set_main_camera", scene=S["scene"], entity="Cam")
    await ed.call("create_entity", scene=S["scene"], type="text", name="Door", values={"text": {"text": "12"}})
    await ed.call("save_scene", scene=S["scene"], path=S["path"])
    return S


async def text_values(ed: Editor, s: dict) -> dict:
    got = await ed.call("get_entity", scene=s["scene"], entity="Door", components=["text"])
    return got["components"]["text"]


async def test_anchor_properties(ed: Editor):
    s = await setup(ed)
    text = await text_values(ed, s)
    ed.check_eq((text["anchorHorizontal"], text["anchorVertical"]), ("left", "bottom"), f"the old anchor by default: {text}")
    ed.check("pivotBaseline" not in text and "pivotCentered" not in text, f"the pivot booleans are gone: {text}")

    await ed.call("set_properties", scene=s["scene"], entity="Door",
                  values={"text": {"anchorHorizontal": "center", "anchorVertical": "capline", "fontSizeUnit": "em"}})
    text = await text_values(ed, s)
    ed.check_eq((text["anchorHorizontal"], text["anchorVertical"], text["fontSizeUnit"]), ("center", "capline", "em"))
    await ed.check_error("set_properties", scene=s["scene"], entity="Door", values={"text": {"anchorVertical": "centre"}},
                         contains="middle")
    await ed.call("undo", scene=s["scene"])
    text = await text_values(ed, s)
    ed.check_eq((text["anchorHorizontal"], text["anchorVertical"], text["fontSizeUnit"]), ("left", "bottom", "pixel_height"),
                "one undo step")

    # S93's Alignment (the lines inside the box) is another pair of keys, set with the anchor at once
    await ed.call("set_properties", scene=s["scene"], entity="Door",
                  values={"text": {"anchorHorizontal": "right", "anchorVertical": "top",
                                   "horizontalAlignment": "justified", "verticalAlignment": "middle"}})
    text = await text_values(ed, s)
    ed.check_eq((text["anchorHorizontal"], text["anchorVertical"], text["horizontalAlignment"], text["verticalAlignment"]),
                ("right", "top", "justified", "middle"), f"anchor and alignment side by side: {text}")


async def test_lua_layout_and_anchor(ed: Editor):
    s = await setup(ed)
    cursor = (await ed.call("read_console", limit=1))["next_cursor"]
    await ed.call("play", action="start", scene=s["scene"], fixed_fps=60, auto_pause=True)
    try:
        await ed.call("wait", frames=2)
        # R42: the size right after setText, before any frame drew it
        values = (await ed.call("eval_lua", code="local t = Text(scene) t.text = 'Door 12' local w, h = t.width, t.height "
                                                 "t.text = 'Door 12 and more' return w, h, t.width"))["values"]
        ed.check(values[0] > 0 and values[1] > 0, f"width and height right after setText: {values}")
        ed.check(values[2] > values[0], f"a longer text is wider at once: {values}")

        values = (await ed.call("eval_lua", code="local t = object('Door', 'Text') t.anchorVertical = TextAnchor.Middle "
                                                 "t.anchorHorizontal = TextAnchor.Right "
                                                 "return t.anchorVertical == TextAnchor.Middle, t.anchorHorizontal == TextAnchor.Right"))["values"]
        ed.check_eq(values, [True, True], "TextAnchor drives both properties")
        values = (await ed.call("eval_lua", code="local t = object('Door', 'Text') "
                                                 "t.horizontalAlignment = TextHorizontalAlignment.JUSTIFIED "
                                                 "t.verticalAlignment = TextVerticalAlignment.BOTTOM "
                                                 "return t.anchorHorizontal == TextAnchor.Right, "
                                                 "t.horizontalAlignment == TextHorizontalAlignment.JUSTIFIED, "
                                                 "t.verticalAlignment == TextVerticalAlignment.BOTTOM"))["values"]
        ed.check_eq(values, [True, True, True], "S93's alignment beside the anchor, each its own property")
        values = (await ed.call("eval_lua", code="local t = object('Door', 'Text') t.anchorHorizontal = TextAnchor.Middle "
                                                 "return t.anchorHorizontal == TextAnchor.Right"))["values"]
        ed.check_eq(values, [True], "a vertical value is refused by the horizontal property")
        console = await ed.call("read_console", since=cursor, types=["error"])
        ed.check(any("horizontal anchor" in line.get("message", "") for line in console["entries"]),
                 f"and the Console says why: {console['entries'][-5:]}")

        values = (await ed.call("eval_lua", code="local t = object('Door', 'Text') t.pivotBaseline = true t.pivotCentered = true "
                                                 "return t.anchorVertical == TextAnchor.Baseline, "
                                                 "t.anchorHorizontal == TextAnchor.Center, t.pivotBaseline"))["values"]
        ed.check_eq(values, [True, True, True], "pivotBaseline and pivotCentered are aliases")
        values = (await ed.call("eval_lua", code="local t = object('Door', 'Text') t.fontSizeUnit = TextFontSizeUnit.Em "
                                                 "local em = t.height t.fontSizeUnit = TextFontSizeUnit.PixelHeight return em, t.height"))["values"]
        ed.check(values[0] > values[1], f"the same font size is larger as an em: {values}")
    finally:
        await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)
