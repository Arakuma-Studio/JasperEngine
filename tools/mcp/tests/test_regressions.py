"""Regression tests for editor bugs found through MCP (one test per bug, newest last).

Runs in a new empty project with a 3D scene of its own.
"""

from harness import Editor

EDITOR = {"create": True, "name": "Regressions"}
TIMEOUT = 900

SCENE = {}


# -- helpers --------------------------------------------------------------------


def close(a, b, tol=1e-3):
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        return abs(a - b) <= tol
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(close(x, y, tol) for x, y in zip(a, b))
    return a == b


async def scene3d(ed: Editor) -> int:
    if "id" not in SCENE:
        made = await ed.call("create_scene", name="Regress3D", type="3D")
        SCENE["id"] = made["scene"]["id"]
    await ed.call("select_scene", scene=SCENE["id"])
    return SCENE["id"]


async def create(ed: Editor, type_: str, name: str, **args) -> int:
    made = await ed.call("create_entity", type=type_, name=name, scene=SCENE["id"], **args)
    return made["entity_id"]


async def world_position(ed: Editor, entity) -> list:
    got = await ed.call("get_entity", entity=entity, scene=SCENE["id"], detail="summary")
    return got["world"]["position"]


async def local_position(ed: Editor, entity) -> list:
    got = await ed.call("get_entity", entity=entity, scene=SCENE["id"], components=["transform"],
                        properties=["position"])
    return got["components"]["transform"]["position"]


async def tree_names(ed: Editor) -> list:
    got = await ed.call("get_scene_tree", scene=SCENE["id"], format="json")
    names = []

    def walk(nodes):
        for n in nodes:
            names.append(n["name"])
            walk(n.get("children", []))

    walk(got["nodes"])
    return names


async def snapshot(ed: Editor, ids: dict) -> dict:
    out = {}
    for name, eid in ids.items():
        out[name] = (await world_position(ed, eid), await local_position(ed, eid))
    return out


def check_snapshot(ed: Editor, got: dict, want: dict, when: str) -> None:
    bad = [f"{name}: {got.get(name)} != {want[name]}" for name in want
           if name not in got or not (close(got[name][0], want[name][0]) and close(got[name][1], want[name][1]))]
    ed.check(not bad, f"{when}: transforms changed:\n  " + "\n  ".join(bad))


async def undo(ed: Editor) -> None:
    await ed.call("undo", scene=SCENE["id"])


async def redo(ed: Editor) -> None:
    await ed.call("redo", scene=SCENE["id"])


# -- DeleteEntityCmd: several entities in one delete, then undo / redo -------------
# Undo put each restored GameObject back at the raw Transform-array index recorded at
# delete time. The camera's editor helpers (icon, frustum lines) are removed from that
# array after the delete, so the recorded indices pointed past its end: the reorder
# rotated past end() (heap corruption, a later abort in free()) and new GameObjects then
# shared another one's Transform slot.


async def test_delete_camera_and_empty_undo_redo(ed: Editor):
    await scene3d(ed)
    ids = {}
    ids["KeepA"] = await create(ed, "empty", "KeepA", position=[1, 2, 3])
    cam = await create(ed, "camera", "DelCam", position=[0, 5, -10])
    empty = await create(ed, "empty", "DelEmpty", position=[4, 0, 0])
    ids["KeepB"] = await create(ed, "empty", "KeepB", position=[-7, 1, 0])
    ids["KeepBChild"] = await create(ed, "empty", "KeepBChild", parent=ids["KeepB"], position=[0, 3, 0])
    ids["DelCam"] = cam
    ids["DelEmpty"] = empty
    names0 = await tree_names(ed)
    snap0 = await snapshot(ed, ids)
    ed.check(close(snap0["KeepBChild"][0], [-7, 4, 0]), f"child world position: {snap0['KeepBChild']}")

    # camera first, as the report did
    await ed.call("delete_entities", entities=[cam, empty], scene=SCENE["id"])
    await undo(ed)
    ed.check_eq(await tree_names(ed), names0, "hierarchy after delete + undo")
    check_snapshot(ed, await snapshot(ed, ids), snap0, "after delete + undo")

    for cycle in range(2):
        await redo(ed)
        await undo(ed)
        ed.check_eq(await tree_names(ed), names0, f"hierarchy after redo + undo #{cycle + 1}")
        check_snapshot(ed, await snapshot(ed, ids), snap0, f"after redo + undo #{cycle + 1}")

    # delete again (a new command) in the other order
    await ed.call("delete_entities", entities=[empty, cam], scene=SCENE["id"])
    await undo(ed)
    ed.check_eq(await tree_names(ed), names0, "hierarchy after reversed delete + undo")
    check_snapshot(ed, await snapshot(ed, ids), snap0, "after reversed delete + undo")

    # new GameObjects must get Transform slots of their own
    f = await create(ed, "empty", "NewF", position=[9, 9, 9])
    g = await create(ed, "empty", "NewG", position=[11, 0, 0])
    ed.check(close(await world_position(ed, f), [9, 9, 9]), "NewF reads another GameObject's transform")
    ed.check(close(await world_position(ed, g), [11, 0, 0]), "NewG reads another GameObject's transform")
    await ed.call("set_transform", entity=g, position=[50, 50, 50], scene=SCENE["id"])
    check_snapshot(ed, await snapshot(ed, ids), snap0, "after moving a new GameObject")
    await ed.call("delete_entities", entities=[f, g], scene=SCENE["id"])
    check_snapshot(ed, await snapshot(ed, ids), snap0, "after deleting the new GameObjects")
    await ed.check_alive()


async def test_delete_two_last_empties_reversed_undo(ed: Editor):
    """No camera: two GameObjects last in the array, deleted later-first, then undone."""
    await scene3d(ed)
    x1 = await create(ed, "empty", "TailX1", position=[2, 0, 0])
    x2 = await create(ed, "empty", "TailX2", position=[3, 0, 0])
    names0 = await tree_names(ed)
    await ed.call("delete_entities", entities=[x2, x1], scene=SCENE["id"])
    await undo(ed)
    ed.check_eq(await tree_names(ed), names0, "hierarchy after reversed delete + undo")
    ed.check(close(await world_position(ed, x1), [2, 0, 0]), "TailX1 position")
    ed.check(close(await world_position(ed, x2), [3, 0, 0]), "TailX2 position")
    n = await create(ed, "empty", "AfterTail", position=[-5, -5, -5])
    ed.check(close(await world_position(ed, n), [-5, -5, -5]), "a new GameObject reads another's transform")
    ed.check(close(await world_position(ed, x2), [3, 0, 0]), "TailX2 changed when a GameObject was created")
    await ed.call("delete_entities", entities=[n, x1, x2], scene=SCENE["id"])
    await ed.check_alive()


# -- play: a Lua init() that fails at startup is an error, not a success ------------
# play {action: start} returned success with the script error only under problems, so an
# agent could miss that its game never initialized.

BROKEN_INIT = """local BrokenInit = {
}

function BrokenInit:init()
    error("boom from init")
end

return BrokenInit
"""


async def test_play_reports_lua_init_error(ed: Editor):
    sid = await scene3d(ed)
    holder = await create(ed, "empty", "BrokenHolder")
    await ed.call("create_script", kind="lua", name="BrokenInit", content=BROKEN_INIT, entity=holder)
    await ed.call("save_scene", scene=sid, path="scenes/Regress3D")
    r = await ed.call_raw("play", {"action": "start", "scene": sid, "timeout_seconds": 120})
    try:
        ed.check(r.is_error, f"play start should report the failed init(): {r.text[:500]}")
        ed.check("boom from init" in r.text, f"the script error is in the text: {r.text[:500]}")
        ed.check((r.structured or {}).get("script_errors"), f"structured script_errors: {r.structured!r}")
    finally:
        await ed.call_raw("play", {"action": "stop"})
    # a clean start is still a success
    await ed.call("detach_script", entity=holder, script="BrokenInit", scene=sid)
    ok = await ed.call_raw("play", {"action": "start", "scene": sid, "timeout_seconds": 120})
    ed.check(not ok.is_error, f"a clean play start failed: {ok.text[:500]}")
    await ed.call("play", action="stop")


# Lua math operators: every metamethod / operand combination of the math classes. Vector3's
# __div was bound through a C-style cast to an operator/(const Vector3&) Vector3 does not have:
# `v / w` read a float out of a pointer (undefined behaviour) and `v / 2` raised an error.
# (name, Lua expression, result kind, expected) -- kind "err" = must raise a Lua error.
MATH_CASES = [
    # Vector2
    ("v2 add", "Vector2(6, 8) + Vector2(2, 4)", "Vector2", [8, 12]),
    ("v2 sub", "Vector2(6, 8) - Vector2(2, 4)", "Vector2", [4, 4]),
    ("v2 mul v", "Vector2(6, 8) * Vector2(2, 4)", "Vector2", [12, 32]),
    ("v2 mul n", "Vector2(6, 8) * 2", "Vector2", [12, 16]),
    ("v2 n mul", "2 * Vector2(6, 8)", "Vector2", [12, 16]),
    ("v2 div n", "Vector2(6, 8) / 2", "Vector2", [3, 4]),
    ("v2 div v", "Vector2(6, 8) / Vector2(2, 4)", "Vector2", [3, 2]),
    ("v2 n div", "24 / Vector2(6, 8)", "Vector2", [4, 3]),
    ("v2 unm", "-Vector2(6, 8)", "Vector2", [-6, -8]),
    ("v2 eq", "Vector2(6, 8) == Vector2(6, 8)", "boolean", True),
    ("v2 ne", "Vector2(6, 8) == Vector2(6, 9)", "boolean", False),
    ("v2 eq other type", "Vector2(6, 8) == Vector3(6, 8, 0)", "boolean", False),
    ("v2 lt", "Vector2(1, 2) < Vector2(3, 4)", "boolean", True),
    ("v2 lt partial", "Vector2(1, 5) < Vector2(3, 4)", "boolean", False),
    ("v2 gt", "Vector2(3, 4) > Vector2(1, 2)", "boolean", True),
    ("v2 le", "Vector2(1, 4) <= Vector2(3, 4)", "boolean", True),
    ("v2 le partial", "Vector2(1, 5) <= Vector2(3, 4)", "boolean", False),
    ("v2 ge", "Vector2(3, 4) >= Vector2(3, 2)", "boolean", True),
    ("v2 div string", "Vector2(6, 8) / 'x'", "err", None),
    # Vector3
    ("v3 add", "Vector3(6, 8, 10) + Vector3(2, 4, 5)", "Vector3", [8, 12, 15]),
    ("v3 sub", "Vector3(6, 8, 10) - Vector3(2, 4, 5)", "Vector3", [4, 4, 5]),
    ("v3 mul v", "Vector3(6, 8, 10) * Vector3(2, 4, 5)", "Vector3", [12, 32, 50]),
    ("v3 mul n", "Vector3(6, 8, 10) * 2", "Vector3", [12, 16, 20]),
    ("v3 n mul", "2 * Vector3(6, 8, 10)", "Vector3", [12, 16, 20]),
    ("v3 div n", "Vector3(6, 8, 10) / 2", "Vector3", [3, 4, 5]),
    ("v3 div n int var", "local n = 4; return Vector3(6, 8, 10) / n", "Vector3", [1.5, 2, 2.5]),
    ("v3 div v", "Vector3(6, 8, 10) / Vector3(2, 4, 5)", "Vector3", [3, 2, 2]),
    ("v3 n div", "40 / Vector3(4, 8, 10)", "Vector3", [10, 5, 4]),
    ("v3 unm", "-Vector3(6, 8, 10)", "Vector3", [-6, -8, -10]),
    ("v3 eq", "Vector3(6, 8, 10) == Vector3(6, 8, 10)", "boolean", True),
    ("v3 ne", "Vector3(6, 8, 10) ~= Vector3(6, 8, 11)", "boolean", True),
    ("v3 eq other type", "Vector3(1, 0, 0) == Quaternion()", "boolean", False),
    ("v3 lt", "Vector3(1, 2, 3) < Vector3(2, 3, 4)", "boolean", True),
    ("v3 gt", "Vector3(1, 2, 3) > Vector3(2, 3, 4)", "boolean", False),
    ("v3 le", "Vector3(1, 3, 3) <= Vector3(2, 3, 4)", "boolean", True),
    ("v3 ge", "Vector3(2, 3, 4) >= Vector3(2, 3, 5)", "boolean", False),
    ("v3 constant div", "Vector3.UNIT_SCALE / 4", "Vector3", [0.25, 0.25, 0.25]),
    # a const reference (AABB.minimum): LuaBridge gives it the const class metatable
    ("v3 const div n", "AABB(Vector3(2, 4, 6), Vector3(8, 8, 8)).minimum / 2", "Vector3", [1, 2, 3]),
    ("v3 const div v", "AABB(Vector3(2, 4, 6), Vector3(8, 8, 8)).minimum / Vector3(2, 4, 3)", "Vector3", [1, 1, 2]),
    ("v3 const n mul", "2 * AABB(Vector3(2, 4, 6), Vector3(8, 8, 8)).minimum", "Vector3", [4, 8, 12]),
    ("v3 const n div", "12 / AABB(Vector3(2, 4, 6), Vector3(8, 8, 8)).minimum", "Vector3", [6, 3, 2]),
    ("v3 const eq", "AABB(Vector3(2, 4, 6), Vector3(8, 8, 8)).minimum == Vector3(2, 4, 6)", "boolean", True),
    ("v3 const le", "AABB(Vector3(2, 4, 6), Vector3(8, 8, 8)).minimum <= Vector3(2, 4, 6)", "boolean", True),
    ("v3 div string", "Vector3(6, 8, 10) / 'x'", "err", None),
    ("v3 string div", "'x' / Vector3(6, 8, 10)", "err", None),
    ("v3 div nil", "Vector3(6, 8, 10) / nil", "err", None),
    ("v3 add n", "Vector3(6, 8, 10) + 1", "err", None),
    # Vector4
    ("v4 add", "Vector4(6, 8, 10, 12) + Vector4(2, 4, 5, 6)", "Vector4", [8, 12, 15, 18]),
    ("v4 sub", "Vector4(6, 8, 10, 12) - Vector4(2, 4, 5, 6)", "Vector4", [4, 4, 5, 6]),
    ("v4 mul v", "Vector4(6, 8, 10, 12) * Vector4(2, 4, 5, 6)", "Vector4", [12, 32, 50, 72]),
    ("v4 mul n", "Vector4(6, 8, 10, 12) * 0.5", "Vector4", [3, 4, 5, 6]),
    ("v4 n mul", "0.5 * Vector4(6, 8, 10, 12)", "Vector4", [3, 4, 5, 6]),
    ("v4 div n", "Vector4(6, 8, 10, 12) / 2", "Vector4", [3, 4, 5, 6]),
    ("v4 div v", "Vector4(6, 8, 10, 12) / Vector4(2, 4, 5, 6)", "Vector4", [3, 2, 2, 2]),
    ("v4 n div", "120 / Vector4(6, 8, 10, 12)", "Vector4", [20, 15, 12, 10]),
    ("v4 unm", "-Vector4(6, 8, 10, 12)", "Vector4", [-6, -8, -10, -12]),
    ("v4 eq", "Vector4(6, 8, 10, 12) == Vector4(6, 8, 10, 12)", "boolean", True),
    ("v4 lt", "Vector4(1, 2, 3, 4) < Vector4(2, 3, 4, 5)", "boolean", True),
    ("v4 le", "Vector4(1, 2, 3, 6) <= Vector4(2, 3, 4, 5)", "boolean", False),
    # Quaternion (w, x, y, z)
    ("q add", "Quaternion(1, 2, 3, 4) + Quaternion(1, 1, 1, 1)", "Quaternion", [2, 3, 4, 5]),
    ("q sub", "Quaternion(1, 2, 3, 4) - Quaternion(1, 1, 1, 1)", "Quaternion", [0, 1, 2, 3]),
    ("q mul q", "Quaternion(1, 0, 0, 0) * Quaternion(0, 1, 0, 0)", "Quaternion", [0, 1, 0, 0]),
    ("q mul v", "Quaternion(90, Vector3(0, 0, 1)) * Vector3(1, 0, 0)", "Vector3", [0, 1, 0]),
    ("q mul n", "Quaternion(1, 2, 3, 4) * 2", "Quaternion", [2, 4, 6, 8]),
    ("q n mul", "2 * Quaternion(1, 2, 3, 4)", "Quaternion", [2, 4, 6, 8]),
    ("q unm", "-Quaternion(1, 2, 3, 4)", "Quaternion", [-1, -2, -3, -4]),
    ("q eq", "Quaternion(1, 2, 3, 4) == Quaternion(1, 2, 3, 4)", "boolean", True),
    ("q eq other type", "Quaternion() == Vector4(0, 0, 0, 1)", "boolean", False),
    # Matrix3 (get(col, row))
    ("m3 add", "Matrix3() + Matrix3()", "Matrix3", "diag", [2, 2, 2]),
    ("m3 sub", "Matrix3.scaleMatrix(3) - Matrix3()", "Matrix3", "diag", [2, 2, 2]),
    ("m3 mul m", "Matrix3.scaleMatrix(2) * Matrix3.scaleMatrix(Vector3(1, 2, 3))", "Matrix3", "diag", [2, 4, 6]),
    ("m3 mul v", "Matrix3.scaleMatrix(Vector3(2, 3, 4)) * Vector3(1, 1, 1)", "Vector3", [2, 3, 4]),
    ("m3 mul n", "Matrix3() * 5", "Matrix3", "diag", [5, 5, 5]),
    ("m3 n mul", "5 * Matrix3()", "Matrix3", "diag", [5, 5, 5]),
    ("m3 eq", "Matrix3() == Matrix3()", "boolean", True),
    ("m3 eq other type", "Matrix3() == Matrix4()", "boolean", False),
    # Matrix4
    ("m4 add", "Matrix4() + Matrix4()", "Matrix4", "diag", [2, 2, 2, 2]),
    ("m4 sub", "Matrix4.scaleMatrix(3) - Matrix4()", "Matrix4", "diag", [2, 2, 2, 0]),
    ("m4 mul m", "Matrix4.scaleMatrix(2) * Matrix4.scaleMatrix(Vector3(1, 2, 3))", "Matrix4", "diag", [2, 4, 6, 1]),
    ("m4 mul v3", "Matrix4.translateMatrix(Vector3(1, 2, 3)) * Vector3(1, 1, 1)", "Vector3", [2, 3, 4]),
    ("m4 mul v4", "Matrix4.translateMatrix(Vector3(1, 2, 3)) * Vector4(1, 1, 1, 1)", "Vector4", [2, 3, 4, 1]),
    ("m4 mul n", "Matrix4() * 5", "Matrix4", "diag", [5, 5, 5, 5]),
    ("m4 n mul", "5 * Matrix4()", "Matrix4", "diag", [5, 5, 5, 5]),
    ("m4 eq", "Matrix4() == Matrix4()", "boolean", True),
    ("m4 ne", "Matrix4() == Matrix4.scaleMatrix(2)", "boolean", False),
    # Plane, Rect
    ("plane unm", "local p = Plane(Vector3(0, 1, 0), 2); local q = -p; "
     "return {p:getDistance(Vector3(0, 5, 0)), q:getDistance(Vector3(0, 5, 0))}", "table", [3, -3]),
    ("plane eq", "Plane(Vector3(0, 1, 0), 2) == Plane(Vector3(0, 1, 0), 2)", "boolean", True),
    ("plane eq other type", "Plane(Vector3(0, 1, 0), 2) == Vector3(0, 1, 0)", "boolean", False),
    ("rect eq", "Rect(1, 2, 3, 4) == Rect(1, 2, 3, 4)", "boolean", True),
    ("rect ne", "Rect(1, 2, 3, 4) == Rect(1, 2, 3, 5)", "boolean", False),
    ("rect eq other type", "Rect(1, 2, 3, 4) == Vector4(1, 2, 3, 4)", "boolean", False),
]

MATH_LUA = """
local cases = {...}
local out = {}
local function comps(v, kind)
    if kind == "Vector2" then return {v.x, v.y} end
    if kind == "Vector3" then return {v.x, v.y, v.z} end
    if kind == "Vector4" then return {v.x, v.y, v.z, v.w} end
    if kind == "Quaternion" then return {v.w, v.x, v.y, v.z} end
    if kind == "Matrix3" then return {v:get(0, 0), v:get(1, 1), v:get(2, 2), v:get(0, 1), v:get(1, 0)} end
    if kind == "Matrix4" then return {v:get(0, 0), v:get(1, 1), v:get(2, 2), v:get(3, 3), v:get(0, 1), v:get(3, 0)} end
    return v
end
for i, c in ipairs(cases) do
    local src = c[2]
    if not src:find("return") then src = "return " .. src end
    local fn = assert(load(src))
    local ok, res = pcall(fn)
    local r = {name = c[1], ok = ok}
    if ok then
        r.type = type(res) == "userdata" and (tostring(res):match("^(%w+)%(") or "userdata") or type(res)
        local cok, vals = pcall(comps, res, c[3])
        if cok then r.value = vals else r.value = "unreadable: " .. tostring(vals) end
    else
        r.error = tostring(res)
    end
    out[i] = r
end
return out
"""


async def test_lua_math_operators(ed: Editor):
    await scene3d(ed)
    lua_cases = "\n".join("{%s, %s, %s}," % (_lua_str(c[0]), _lua_str(c[1]), _lua_str(c[2])) for c in MATH_CASES)
    code = MATH_LUA.replace("local cases = {...}", "local cases = {\n" + lua_cases + "\n}")
    got = await ed.call("eval_lua", code=code, allow_edit_mode=True, max_items=1000)
    results = got["values"][0]
    ed.check_eq(len(results), len(MATH_CASES), "one result per case")
    bad = []
    for case, r in zip(MATH_CASES, results):
        name, expr, kind = case[0], case[1], case[2]
        if kind == "err":
            if r.get("ok"):
                bad.append(f"{name}: `{expr}` should raise an error, got {r.get('type')} {r.get('value')}")
            continue
        if not r.get("ok"):
            bad.append(f"{name}: `{expr}` raised {r.get('error')}")
            continue
        if r.get("type") != kind:
            bad.append(f"{name}: `{expr}` gave a {r.get('type')} ({r.get('value')}), want a {kind}")
            continue
        value = r.get("value")
        # a matrix: its diagonal, then two off-diagonal elements that must be 0
        want = case[4] + [0, 0] if case[3] == "diag" else case[3]
        if not close(value, want):
            bad.append(f"{name}: `{expr}` = {value}, want {want}")
    ed.check(not bad, "Lua math operators:\n  " + "\n  ".join(bad))


def _lua_str(s: str) -> str:
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


# Lua's global print goes through Log::print (the Console), not stdout: any number of values of
# any type, joined with tabs as standard Lua does
PRINTER = """local Printer = {}

function Printer:init()
    print("lua-print-probe", 42, nil, true, Vector3(1, 2, 3))
end

return Printer
"""


async def test_lua_print_goes_to_console(ed: Editor):
    sid = await scene3d(ed)
    holder = await create(ed, "empty", "PrintHolder")
    await ed.call("create_script", kind="lua", name="Printer", content=PRINTER, entity=holder)
    await ed.call("save_scene", scene=sid, path="scenes/Regress3D")
    cursor = (await ed.call("read_console", limit=1))["next_cursor"]
    r = await ed.call_raw("play", {"action": "start", "scene": sid, "timeout_seconds": 120})
    try:
        ed.check(not r.is_error, f"play start failed: {r.text[:500]}")
        found = await ed.call("read_console", since=cursor)
    finally:
        await ed.call_raw("play", {"action": "stop"})
        await ed.call("detach_script", entity=holder, script="Printer", scene=sid)
    lines = [(e["type"], e["message"]) for e in found["entries"]]
    want = "lua-print-probe\t42\tnil\ttrue\tVector3(1.000000, 2.000000, 3.000000)"
    ed.check(any(t == "info" and want in m for t, m in lines), f"print output in the Console: {lines}")


# eval_lua's helpers (print, script, object, entity, getScene, scene) were set on its environment
# after the environment's __newindex = _G: they landed in the game's globals, so the game's print
# then wrote through a dead eval context (a crash in play after an edit-mode eval_lua)
async def test_eval_lua_helpers_stay_out_of_globals(ed: Editor):
    await scene3d(ed)
    got = await ed.call("eval_lua", code="return type(script), type(object), type(getScene)", allow_edit_mode=True)
    ed.check_eq(got["values"], ["function", "function", "function"], "helpers inside eval_lua")
    got = await ed.call("eval_lua", allow_edit_mode=True, code=(
        "return rawget(_G, 'script') == nil, rawget(_G, 'object') == nil, rawget(_G, 'getScene') == nil, "
        "rawget(_G, 'entity') == nil, rawget(_G, 'scene') == nil"))
    ed.check_eq(got["values"], [True, True, True, True, True], "eval_lua helpers leaked into _G")
