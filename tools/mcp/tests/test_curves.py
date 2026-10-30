"""S27 curves for agents (docs/specs/S27-editor-tools.md AC7-AC9; jasper://docs/editor-tools): a Tween's
ease "Curve" set and read back as curve JSON, evaluated by evaluate_curve exactly as Play moves
the GameObject (fixed fps), list_curve_presets, the force field falloff curve, and the Lua curve
API (AnimationCurve, Keyframe, Gradient, MinMaxCurve, WrapMode; 1-based key indices).
"""

import json

import harness
from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Curves"}
TIMEOUT = 900

# out and back: 1 at t = 0.5, 0 at both ends (a linear ease would end at 1)
BUMP = [[0, 0, 0, 0, "clampedAuto"], [0.5, 1, 0, 0, "clampedAuto"], [1, 0, 0, 0, "clampedAuto"]]

S: dict = {}


async def setup(ed: Editor) -> dict:
    if S.get("ed") is ed:
        return S
    S.clear()
    S["ed"] = ed
    await ed.call("tool_groups", enable=["editor_tools"])
    made = await ed.call("create_scene", name="Curves", type="3D", path="scenes/Curves")
    S["scene"] = made["scene"]["id"]
    S["cube"] = (await ed.call("create_entity", scene=S["scene"], type="cube", name="Mover"))["entity_id"]
    anim = await ed.call("edit_animation", scene=S["scene"], target=S["cube"], autoplay=True,
                         tweens=[{"type": "position", "from": [0, 0, 0], "to": [10, 0, 0], "duration": 1, "ease": "linear"}])
    S["animation"] = anim["animation"]["id"]
    S["tween"] = anim["actions"][0]["action"]["id"]
    return S


async def tween_values(ed: Editor) -> dict:
    got = await ed.call("get_entity", scene=S["scene"], entity=S["tween"], components=["tween"], detail="full")
    comp = got["components"]["tween"]
    return {k: (v.get("value", v) if isinstance(v, dict) and "value" in v else v) for k, v in comp.items()}


async def test_set_curve(ed: Editor):
    await setup(ed)
    values = await tween_values(ed)
    ed.check("curve" not in values or values.get("function") != "curve", f"the curve shows with the Curve ease only: {values}")
    await ed.call("set_properties", scene=S["scene"], entity=S["tween"], values={"tween": {"function": "curve", "curve": BUMP}})
    values = await tween_values(ed)
    ed.check_eq(values.get("function"), "curve", f"{values}")
    curve = values.get("curve")
    ed.check(isinstance(curve, list) and len(curve) == 3, f"curve JSON back: {curve}")
    ed.check_eq([k[:2] for k in curve], [[0, 0], [0.5, 1], [1, 0]], "the keys")
    ed.check(all(len(k) == 5 and k[4] == "clampedAuto" for k in curve), f"the modes kept: {curve}")
    # wraps and steps
    stepped = {"keys": [[0, 0, 0, None], [0.5, 1, 0, None], [1, 1, 0, 0]], "postWrapMode": "loop"}
    await ed.call("set_properties", scene=S["scene"], entity=S["tween"], values={"tween": {"curve": stepped}})
    got = (await tween_values(ed))["curve"]
    ed.check(isinstance(got, dict) and got.get("postWrapMode") == "loop" and got["keys"][0][3] is None, f"wrap and step: {got}")
    # errors say where
    r = await ed.call_raw("set_properties", {"scene": S["scene"], "entity": S["tween"],
                                             "values": {"tween": {"curve": [[0, 0, 0, 0, "weighted"]]}}})
    ed.check(r.is_error and "[0][4]" in r.text, f"a bad mode names its place: {r.text[:300]}")
    await ed.call("set_properties", scene=S["scene"], entity=S["tween"], values={"tween": {"curve": BUMP}})
    await ed.call("save_scene", scene=S["scene"])
    text = (ed.project / "scenes" / "Curves.scene").read_text()
    ed.check("clampedAuto" in text, "the scene file holds the curve")


async def test_evaluate_and_presets(ed: Editor):
    await setup(ed)
    times = [0, 0.25, 0.5, 0.75, 1]
    got = await ed.call("evaluate_curve", scene=S["scene"], entity=S["tween"], component="tween", property="curve", times=times)
    vals = got["values"]
    ed.check(abs(vals[2] - 1) < 1e-6 and abs(vals[0]) < 1e-6 and abs(vals[4]) < 1e-6, f"the bump: {vals}")
    ed.check(0 < vals[1] < 1 and abs(vals[1] - vals[3]) < 1e-5, f"symmetric: {vals}")
    presets = await ed.call("list_curve_presets")
    built_in = [lib for lib in presets["libraries"] if lib["scope"] == "built_in"]
    ed.check(built_in and len(built_in[0]["presets"]) >= 8, f"the built-in presets: {json.dumps(presets)[:500]}")
    names = [p["name"] for p in built_in[0]["presets"]]
    ed.check("Linear" in names, f"{names}")
    # a force field's falloff is a curve too
    field = (await ed.call("create_entity", scene=S["scene"], type="force_field", name="Field"))["entity_id"]
    await ed.call("set_properties", scene=S["scene"], entity=field, values={"forcefield": {"falloff": [[0, 1, 0, 0], [1, 0, 0, 0]]}})
    got = await ed.call("evaluate_curve", scene=S["scene"], entity=field, component="forcefield", property="falloff", times=[0.5])
    ed.check(abs(got["values"][0] - 0.5) < 1e-6, f"falloff at 0.5: {got['values']}")
    await ed.check_error("evaluate_curve", {"scene": S["scene"], "entity": S["cube"], "component": "transform",
                                            "property": "position", "times": [0]})


async def test_tween_curve_play(ed: Editor):
    await setup(ed)
    await ed.call("set_properties", scene=S["scene"], entity=S["tween"], values={"tween": {"function": "curve", "curve": BUMP}})
    await ed.call("save_scene", scene=S["scene"])
    frames = 40
    xs = []
    await ed.call("play", action="start", scene=S["scene"], auto_pause=True, fixed_fps=30)
    try:
        for _ in range(frames):
            await ed.call("play", action="step", frames=1)
            pos = harness.find_key(await ed.call("get_runtime_entity", entity=S["cube"]), "position")
            pos = pos.get("value", pos) if isinstance(pos, dict) else pos
            xs.append(float(pos[0]))
    finally:
        await ed.call("play", action="stop")
    ed.check(9.9 <= max(xs) <= 10.0 + 1e-3, f"the tween reaches the bump's top: {xs}")
    ed.check(abs(xs[-1]) < 1e-3, f"and comes back (a linear ease ends at 10): {xs}")
    # frame by frame it is 10 x curve(t) on the 1/30 s grid
    times = [k / 30 for k in range(frames + 2)]
    curve = (await ed.call("evaluate_curve", scene=S["scene"], entity=S["tween"], component="tween", property="curve",
                           times=[min(t, 1.0) for t in times]))["values"]
    matches = [offset for offset in range(3)
               if all(abs(xs[k] - 10 * curve[min(k + offset, len(curve) - 1)]) < 1e-3 for k in range(frames))]
    ed.check(matches, f"Play follows the curve frame by frame: play {xs[:12]} vs curve {[round(10 * c, 4) for c in curve[:12]]}")
    print(f"[tween_curve] x at frames 0..{frames - 1}: {[round(x, 4) for x in xs]}")


LUA = """
local results = {}
local c = AnimationCurve.linear(0, 0, 1, 2)
results.linear = c:evaluate(0.5)
results.length = c.length
local i = c:addKey(0.5, 3)
results.added = i
results.key2 = c:getKey(2).value
results.moved = c:moveKey(2, Keyframe(0.25, 4))
results.afterMove = c:getKey(2).time
results.removed = c:removeKey(2)
results.lengthAfter = c.length
results.badIndex = c:getKey(9) == nil
c.postWrapMode = WrapMode.Loop
results.loop = c:evaluate(1.5)
results.clampAlias = WrapMode.Clamp == WrapMode.Constant
results.constant = AnimationCurve.constant(0, 1, 5):evaluate(0.3)
local k = Keyframe(0, 1, 0, 0)
k.inTangentMode = TangentMode.Linear
results.mode = k.inTangentMode == TangentMode.Linear
local g = Gradient()
g:setKeys({ GradientColorKey(Vector3(1, 0, 0), 0), GradientColorKey(Vector3(0, 0, 1), 1) },
          { GradientAlphaKey(1, 0), GradientAlphaKey(0, 1) })
local mid = g:evaluate(0.5)
results.gradient = { mid.x, mid.y, mid.z, mid.w }
results.gradientKeys = #g.colorKeys
local m = MinMaxCurve.twoConstants(1, 3)
results.minmax = { m:evaluate(0, 0), m:evaluate(0, 1), m:evaluate(0, 0.5) }
results.ease = EaseType.CURVE ~= nil
return results
"""


async def test_lua_api(ed: Editor):
    await setup(ed)
    r = await ed.call("eval_lua", code=LUA, allow_edit_mode=True)
    v = r["values"][0]
    ed.check_eq(v["linear"], 1, f"AnimationCurve.linear(0,0,1,2):evaluate(0.5): {v}")
    ed.check_eq(v["length"], 2)
    ed.check_eq(v["added"], 2, "addKey returns the 1-based index")
    ed.check_eq(v["key2"], 3)
    ed.check_eq(v["moved"], 2)
    ed.check(abs(v["afterMove"] - 0.25) < 1e-6, f"{v}")
    ed.check(v["removed"] is True and v["lengthAfter"] == 2, f"{v}")
    ed.check(v["badIndex"] is True, "an index out of range is nil, not an error")
    ed.check(abs(v["loop"] - 1) < 1e-5, f"Loop wraps 1.5 to 0.5: {v['loop']}")
    ed.check(v["clampAlias"] is True and abs(v["constant"] - 5) < 1e-6 and v["mode"] is True, f"{v}")
    ed.check(abs(v["gradient"][0] - v["gradient"][2]) < 0.05 and abs(v["gradient"][3] - 0.5) < 1e-5, f"the gradient's middle: {v['gradient']}")
    ed.check_eq(v["gradientKeys"], 2)
    ed.check_eq(v["minmax"], [1, 3, 2], "MinMaxCurve between two constants")
    ed.check(v["ease"] is True, "EaseType.CURVE")
