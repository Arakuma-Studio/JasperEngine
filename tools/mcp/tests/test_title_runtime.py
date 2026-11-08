"""S81 Title Art runtime: the Title Art Renderer through MCP (docs/specs/S81-title-art-runtime.md AC2,
AC20, AC21; jasper://docs/title-art "Runtime"): get_title and render_title with `entity`, the
component's overrides as properties (one undo step, one entry per name), the Lua TitleArt API and its
sentences in Play, and a Realtime title rendering a new text while the game runs.
"""

import harness
from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Title Art Runtime"}
TIMEOUT = 900

S: dict = {}

DOC = "assets/titles/logo.titleart"


async def setup(ed: Editor) -> dict:
    if S.get("ed") is ed:
        return S
    S.clear()
    S["ed"] = ed
    await ed.call("tool_groups", enable=["title_art"])
    await ed.call("set_project_settings", values={"title_art.auto_bake": False})
    await ed.call("edit_title", path=DOC, create={"text": "JACKPOT", "canvas": [800, 400]}, bake="never",
                  ops=[{"op": "expose", "name": "glow_amount", "type": "number", "min": 0, "max": 1,
                       "targets": [{"layer": "Title", "field": "opacity"}]}])
    await ed.call("export_title", path=DOC)
    made = await ed.call("create_scene", name="Hud", type="UI", path="scenes/Hud")
    S["scene"] = made["scene"]["id"]
    await ed.call("create_ui", scene=S["scene"], root={"type": "image", "name": "Logo", "layout": {"anchor": "center", "size": [400, 200]}})
    await ed.call("add_component", scene=S["scene"], entity="Logo", component="titleart")
    await ed.call("set_properties", scene=S["scene"], entity="Logo", values={"titleart": {"title": DOC}})
    await ed.call("save_scene", scene=S["scene"])
    return S


async def test_parameters_listed_with_the_group(ed: Editor):
    """§6.5: entity, scene, wait_ready and timeout_seconds on get_title, entity and scene on render_title,
    hidden until tool_groups enables title_art."""
    default = {t.name for t in await ed.tools(groups=None)}
    ed.check("get_title" not in default and "render_title" not in default, "the title_art group is hidden by default")
    await ed.call("tool_groups", enable=["title_art"])
    listed = {t.name: t for t in await ed.tools(groups=None)}
    get_props = listed["get_title"].input_schema["properties"]
    for key in ("path", "entity", "scene", "wait_ready", "timeout_seconds"):
        ed.check(key in get_props, f"get_title lists {key}")
    render_props = listed["render_title"].input_schema["properties"]
    for key in ("path", "entity", "scene", "options"):
        ed.check(key in render_props, f"render_title lists {key}")
    ed.check("path" not in listed["get_title"].input_schema.get("required", []), "path is not required once entity exists")


async def test_bad_args(ed: Editor):
    """§6.5: both path and entity, or neither, are refused with bad_args and a fix."""
    s = await setup(ed)
    await ed.check_error("get_title", {"path": DOC, "entity": "Logo", "scene": s["scene"]}, contains="bad_args")
    await ed.check_error("get_title", {}, contains="bad_args")
    await ed.check_error("render_title", {"path": DOC, "entity": "Logo"}, contains="bad_args")
    await ed.check_error("render_title", {"path": DOC, "options": {"overrides": {"text": "A"}}}, contains="entity")


async def test_instance_state(ed: Editor):
    """§6.7: the shape of get_title {entity}; R71's particles calls."""
    s = await setup(ed)
    state = await ed.call("get_title", entity="Logo", scene=s["scene"])
    for key in ("entity", "title", "mode", "space", "ready", "pending", "pixel_path", "failed", "error", "overrides", "effective",
                "display", "live", "characters", "glyph_atlas", "particles", "module"):
        ed.check(key in state, f"get_title {{entity}} has {key}: {sorted(state)}")
    ed.check_eq(state["title"], DOC)
    ed.check_eq(state["mode"], "baked")
    ed.check_eq(state["space"], "ui")
    ed.check_eq(state["effective"].get("text"), "JACKPOT")
    labels = [p["label"] for p in state["particles"]]
    ed.check_eq(labels, ["Sparkle", "Embers", "Confetti", "Coin Shower"])
    ed.check_eq(state["particles"][0]["call"]["tool"], "create_particles")
    ed.check_eq(state["particles"][0]["call"]["args"]["parent"], state["entity"])


async def test_overrides_are_properties(ed: Editor):
    """AC2: set_properties {values: {titleart: {"overrides.text"}}} is one undo step; an unknown name lists the exposed ones."""
    s = await setup(ed)
    await ed.call("set_properties", scene=s["scene"], entity="Logo", values={"titleart": {"overrides.text": "WIN"}})
    state = await ed.call("get_title", entity="Logo", scene=s["scene"])
    ed.check_eq(state["overrides"].get("text"), "WIN")
    await ed.call("set_properties", scene=s["scene"], entity="Logo", values={"titleart": {"overrides.glow_amount": 0.5}})
    state = await ed.call("get_title", entity="Logo", scene=s["scene"])
    ed.check_eq(state["overrides"].get("glow_amount"), 0.5)
    await ed.call("undo", scope="scene")
    state = await ed.call("get_title", entity="Logo", scene=s["scene"])
    ed.check("glow_amount" not in state["overrides"], f"one undo step takes the last entry: {state['overrides']}")
    ed.check_eq(state["overrides"].get("text"), "WIN", "the earlier entry stays")
    await ed.check_error("set_properties", {"scene": s["scene"], "entity": "Logo", "values": {"titleart": {"overrides.glowamount": 1}}}, contains="Exposed: text, glow_amount")
    await ed.call("set_properties", scene=s["scene"], entity="Logo", values={"titleart": {"overrides.text": None}})
    state = await ed.call("get_title", entity="Logo", scene=s["scene"])
    ed.check_eq(state["overrides"], {}, "null removes the entry")


async def test_lua_sentences(ed: Editor):
    """AC20 (Baked part): the §6.6 sentences, read-only fields, and parameter reads in Play."""
    s = await setup(ed)
    await ed.call("play", action="start", scene=s["scene"])
    try:
        await ed.call("wait", frames=3)
        r = await ed.call("eval_lua", code=(
            "local t = object('Logo'):getTitleArt()\n"
            "local ok, err = pcall(function() t.text = 'WIN' end)\n"
            "local ok2, err2 = pcall(function() t.renderMode = TitleArtRenderMode.REALTIME end)\n"
            "return t.title, t.renderMode == TitleArtRenderMode.BAKED, ok, err, ok2, err2, t:getParameter('text'), t:hasParameter('glow_amount')"))
        values = r["values"]
        ed.check_eq(values[0], DOC)
        ed.check_eq(values[1], True)
        ed.check_eq(values[2], False, "text on a Baked title raises")
        ed.check_in("text cannot change on a Baked title (its pixels are the asset's bake). Set Render Mode to Realtime", values[3])
        ed.check_eq(values[4], False, "renderMode is read-only")
        ed.check_in("read-only", values[5])
        ed.check_eq(values[6], "JACKPOT")
        ed.check_eq(values[7], True)
    finally:
        await ed.call("play", action="stop")


async def test_realtime_text_change(ed: Editor):
    """AC21: in Play, a Realtime title takes a new text from Lua and is ready with it; render_title's
    what-if overrides leave the instance unchanged."""
    s = await setup(ed)
    await ed.call("set_properties", scene=s["scene"], entity="Logo", values={"titleart": {"renderMode": "realtime"}})
    await ed.call("play", action="start", scene=s["scene"])
    try:
        await ed.call("wait", frames=3)
        await ed.call("eval_lua", code="object('Logo'):getTitleArt().text = 'WIN'")
        state = await ed.call("get_title", entity="Logo", scene=s["scene"], wait_ready=True, timeout_seconds=60)
        ed.check(state.get("ready") is True, f"ready after the change: {state.get('pending')} {state.get('error')}")
        ed.check_eq(state["effective"].get("text"), "WIN")
        ed.check(state["pixel_path"] in ("float", "rgba8"), f"pixel path: {state['pixel_path']}")
        rendered = await ed.call("render_title", entity="Logo", scene=s["scene"], options={"overrides": {"text": "A"}}, image="none")
        ed.check(isinstance(rendered, dict), f"render_title {{entity}}: {rendered}")
        after = await ed.call("get_title", entity="Logo", scene=s["scene"])
        ed.check_eq(after["effective"].get("text"), "WIN", "a what-if leaves the instance alone")
    finally:
        await ed.call("play", action="stop")


FX_DOC = "assets/titles/glow.titleart"


async def test_lua_live_effect_params(ed: Editor):
    """S83 AC27 (S81 R9, R66): with fx:light_glow.intensity exposed as glow and fx:light_glow.radius as
    glow_radius on a Realtime title, setParameter('glow', 2) changes the next frame with no re-render (a
    live compose), and setParameter('glow_radius', 300) causes exactly one re-render."""
    s = await setup(ed)
    await ed.call("edit_title", path=FX_DOC, create={"text": "WIN", "canvas": [800, 400]}, bake="never",
                  ops=[{"op": "add_effect", "layer": "Title", "type": "light_glow"},
                       {"op": "expose", "name": "glow", "type": "number", "min": 0, "max": 10,
                        "targets": [{"layer": "Title", "field": "fx:light_glow.intensity"}]},
                       {"op": "expose", "name": "glow_radius", "type": "number", "min": 4, "max": 1000,
                        "targets": [{"layer": "Title", "field": "fx:light_glow.radius"}]}])
    await ed.call("export_title", path=FX_DOC)
    await ed.call("create_ui", scene=s["scene"], root={"type": "image", "name": "Glow", "layout": {"anchor": "top", "size": [400, 200]}})
    await ed.call("add_component", scene=s["scene"], entity="Glow", component="titleart")
    await ed.call("set_properties", scene=s["scene"], entity="Glow", values={"titleart": {"title": FX_DOC, "renderMode": "realtime"}})
    checked = await ed.call("check_title", path=FX_DOC, checks=["fx_live_budget"])
    ed.check_eq(checked["checks"][0]["status"], "PASS", f"glow composes within the mobile live budget: {checked['checks'][0]}")
    await ed.call("play", action="start", scene=s["scene"])
    try:
        state = await ed.call("get_title", entity="Glow", scene=s["scene"], wait_ready=True, timeout_seconds=120)
        ed.check(state.get("ready") is True, f"the first render: {state.get('pending')} {state.get('error')}")
        before = state["live"]
        ed.check(before["input_bytes"] > 0, f"the render kept the live inputs: {before}")
        await ed.call("eval_lua", code="object('Glow'):getTitleArt():setParameter('glow', 2)")
        await ed.call("wait", frames=2)
        live = await ed.call("get_title", entity="Glow", scene=s["scene"])
        ed.check(live.get("ready") is True, f"the next frame shows it: {live.get('pending')} {live['live']}")
        ed.check_eq(live["effective"].get("glow"), 2)
        ed.check_eq(live["live"]["renders"], before["renders"], f"no re-render: {live['live']}")
        ed.check_eq(live["live"]["composes"], before["composes"] + 1, f"one live compose: {live['live']}")
        await ed.call("eval_lua", code="object('Glow'):getTitleArt():setParameter('glow_radius', 300)")
        after = await ed.call("get_title", entity="Glow", scene=s["scene"], wait_ready=True, timeout_seconds=120)
        ed.check(after.get("ready") is True, f"ready after the radius: {after.get('pending')} {after.get('error')}")
        ed.check_eq(after["live"]["renders"], before["renders"] + 1, f"exactly one re-render: {after['live']}")
        ed.check_eq(after["live"]["composes"], live["live"]["composes"])
    finally:
        await ed.call("play", action="stop")


async def test_hdr_on_8bit_reads_the_light(ed: Editor):
    """S83 §6.11 fx_hdr_on_8bit (info): a check_title render of the light at 1/4 measures the light above 4.0."""
    await setup(ed)
    path = "assets/titles/hdr.titleart"
    await ed.call("edit_title", path=path, create={"text": "HOT", "canvas": [600, 300]}, bake="never",
                  ops=[{"op": "add_effect", "layer": "Title", "type": "light_glow"},
                       {"op": "set", "layer": "Title", "values": {"fx:light_glow.intensity": 10}}])
    r = await ed.call("check_title", path=path, checks=["fx_hdr_on_8bit"])
    check = r["checks"][0]
    ed.check_eq(check["status"], "PASS", f"info, never a warning: {check}")
    ed.check(check["measured"]["over_4_fraction"] > 0.005, f"light at intensity 10 is above 4.0 somewhere: {check}")
    ed.check_in("info:", check["message"])
