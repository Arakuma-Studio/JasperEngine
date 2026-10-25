"""Editor scripts: menus defined by the project's own scripts (S23 R17-R22, AC12, AC13).

A Lua file under an Editor/ folder with ---@menu annotations is found by reading it (its
top-level code does not run when the project opens), lists in list_editor_menus with its
targets, and runs through run_editor_menu: Editor.call("set_transform") on each selected
GameObject makes ONE undo step named after the item ("Raise Selected") that undoes every move.
A ---@validate function turns the item off. The file never ships: a source export has no
Editor/Tools.lua. A C++ JE_MENU_ITEM in Editor/*.cpp builds the editor-scripts library on first
use and runs the same way.
"""

import shutil

from harness import Editor, skip

CI = True
EDITOR = {"create": True, "name": "Editor Menus CI"}

TIMEOUT = 2400

TOOLS = """local M = {}

print("TOP LEVEL OF THE EDITOR SCRIPT RAN")

---@menu Tools/Test/Raise
function M.raise(ctx)
    M.raiseSelected(ctx)
end

---@menu GameObject/Test/Raise Selected [priority=10]
function M.raiseSelected(ctx)
    for _, id in ipairs(ctx.selection) do
        local got = Editor.call("get_entity", { entity = id, components = { "transform" } })
        local p = got.data.components.transform.position
        local moved = Editor.call("set_transform", { entity = id, position = { p[1], p[2] + 1, p[3] } })
        if not moved.ok then error(moved.text) end
    end
    Editor.log("raised " .. #ctx.selection)
end

---@validate raiseSelected
function M.canRaise(ctx)
    return #ctx.selection > 0
end

---@menu Tools/Test/Say Hello
function M.hello(ctx)
    print("hello from " .. ctx.menuPath)
end

---@toolbar Hello [icon=hand] [tooltip=Say hello]
function M.helloButton(ctx)
    M.hello(ctx)
end

return M
"""

HELPER = """local Helper = {}
function Helper.twice(x) return 2 * x end
return Helper
"""

CPP = """#include "script/EditorMenu.h"

JE_MENU_ITEM("Tools/Test/Make Marker", 50)
static void makeMarker(const jasperengine::EditorMenuContext& ctx) {
    const std::string result = ctx.call("create_entity", R"({"name": "CppMarker", "type": "empty"})");
    ctx.log("create_entity: " + result.substr(0, 40));
}
"""

S: dict = {}


async def setup(ed: Editor) -> dict:
    if S.get("ed") is ed:
        return S
    S.clear()
    S["ed"] = ed
    made = await ed.call("create_scene", name="Menus", type="3D", path="scenes/Menus")
    S["scene"] = made["scene"]["id"]
    await ed.call("select_scene", scene=S["scene"])
    # a runtime script tells where the Lua dir is
    script = await ed.call("create_script", kind="lua", name="Runtime", content="local R = {}\nreturn R\n")
    lua_dir = script["path"].rsplit("/", 1)[0] if "/" in script["path"] else ""
    S["runtime"] = script["path"]
    S["tools"] = (lua_dir + "/" if lua_dir else "") + "Editor/Tools.lua"
    await ed.call("write_file", path=S["tools"], content=TOOLS)
    # an Editor/ file without annotations stays a runtime module (it ships)
    S["helper"] = (lua_dir + "/" if lua_dir else "") + "Editor/Helper.lua"
    await ed.call("write_file", path=S["helper"], content=HELPER)
    await ed.call("sync_files", paths=[S["tools"], S["helper"]])
    S["a"] = (await ed.call("create_entity", scene=S["scene"], type="box", name="A"))["entity_id"]
    S["b"] = (await ed.call("create_entity", scene=S["scene"], type="box", name="B"))["entity_id"]
    await ed.call("set_transform", scene=S["scene"], entity=S["a"], position=[0, 0, 0])
    await ed.call("set_transform", scene=S["scene"], entity=S["b"], position=[2, 5, 0])
    await ed.call("save_scene", scene=S["scene"])
    await ed.call("save_project")
    return S


async def position(ed: Editor, scene, entity) -> list:
    got = await ed.call("get_entity", scene=scene, entity=entity, components=["transform"])
    return got["components"]["transform"]["position"]


async def test_items_are_found_by_reading_the_file(ed: Editor):
    s = await setup(ed)
    listed = await ed.call("list_editor_menus")
    items = {i["path"]: i for i in listed["items"]}
    ed.check("GameObject/Test/Raise Selected" in items, f"the menu item is listed: {listed}")
    raise_item = items["GameObject/Test/Raise Selected"]
    ed.check_eq(raise_item["language"], "lua", "language")
    ed.check_eq(sorted(raise_item["targets"]), ["gameobject", "main"], "GameObject/ items show in the main menu and the Hierarchy")
    ed.check_eq(raise_item["priority"], 10, "priority")
    ed.check_eq(raise_item["source"]["file"], s["tools"], "the source file")
    ed.check_eq(raise_item["source"]["line"], 10, "the annotation's line")
    ed.check("Tools/Test/Raise" in items and items["Tools/Test/Raise"]["source"]["line"] == 5, f"Tools/Test/Raise: {items.get('Tools/Test/Raise')}")
    ed.check_eq(items["Tools/Test/Raise"]["targets"], ["main"], "Tools/ items are main-menu items")
    ed.check(not any(i["source"]["file"].endswith("Helper.lua") for i in listed["items"]), "an Editor/ file without annotations has no items")
    ed.check_eq(raise_item.get("validate"), "canRaise", "the validator")
    ed.check("Hello" in items and items["Hello"]["kind"] == "toolbar", f"the toolbar item: {items.get('Hello')}")
    ed.check_eq(items["Hello"].get("icon"), "hand", "the toolbar icon")
    toolbar = await ed.call("list_editor_menus", target="toolbar")
    ed.check_eq([i["path"] for i in toolbar["items"]], ["Hello"], "target filter")
    console = [e["message"] for e in await ed.console_since(None, limit=2000)]
    ed.check(not any("TOP LEVEL OF THE EDITOR SCRIPT RAN" in m for m in console),
             "the editor script's top-level code did not run on open or scan")
    # an editor script is not a runtime module
    r = await ed.call("check_script", path=s["tools"])
    ed.check(r.get("ok", True) is not False, f"the editor script compiles: {r}")


async def test_raise_selected_is_one_undo_step(ed: Editor):
    s = await setup(ed)
    before = [await position(ed, s["scene"], s["a"]), await position(ed, s["scene"], s["b"])]
    r = await ed.call("run_editor_menu", path="GameObject/Test/Raise Selected", scene=s["scene"], selection=[s["a"], s["b"]])
    ed.check_eq(r["ok"], True, f"the item ran: {r}")
    ed.check_in("raised 2", r["log"], "Editor.log reaches the result")
    after = [await position(ed, s["scene"], s["a"]), await position(ed, s["scene"], s["b"])]
    for b, a in zip(before, after):
        ed.check(abs(a[1] - (b[1] + 1)) < 1e-4, f"raised by 1: {b} -> {a}")
    ed.check("Raise Selected" in (r.get("undo_name") or ""), f"the undo step is named after the item: {r}")

    status = await ed.call("editor_status")
    ed.check("Raise Selected" in (status["undo"]["undo_name"] or ""), f"the Edit menu names it: {status['undo']}")
    done = await ed.call("undo", scope="timeline", scene=s["scene"])
    ed.check_eq(len(done["undone"]), 1, f"one step: {done}")
    back = [await position(ed, s["scene"], s["a"]), await position(ed, s["scene"], s["b"])]
    for b, a in zip(before, back):
        ed.check(max(abs(x - y) for x, y in zip(a, b)) < 1e-4, f"one undo puts both back: {b} vs {a}")


async def test_validate_and_errors(ed: Editor):
    s = await setup(ed)
    await ed.call("select_entities", scene=s["scene"], entities=[])
    # the validator runs when a menu opens; run_editor_menu runs the item regardless, with an empty selection
    r = await ed.call("run_editor_menu", path="GameObject/Test/Raise Selected", scene=s["scene"])
    ed.check_in("raised 0", r["log"], "nothing selected: nothing moved")
    ed.check("undo_group" not in r or not r["undo_group"].get("undo"), f"no edit, no undo step: {r}")
    hello = await ed.call("run_editor_menu", path="Tools/Test/Say Hello", scene=s["scene"])
    ed.check_in("hello from Tools/Test/Say Hello", hello["log"], "print goes to the run's log")
    missing = await ed.call("run_editor_menu", path="Tools/No Such Item", expect_error=True)
    ed.check_in("list_editor_menus", missing, "names the tool that lists the items")

    # a script error: the run fails with file:line, the scene untouched
    broken = TOOLS.replace('Editor.log("raised "', 'error("boom") Editor.log("raised "')
    await ed.call("write_file", path=s["tools"], content=broken)
    await ed.call("sync_files", paths=[s["tools"]])
    r = await ed.call_raw("run_editor_menu", {"path": "GameObject/Test/Raise Selected", "scene": s["scene"], "selection": [s["a"]]})
    ed.check(r.is_error, f"the error fails the run: {r.text[:400]}")
    ed.check("Tools.lua:" in r.text and "boom" in r.text, f"file:line and the message: {r.text[:400]}")
    await ed.call("write_file", path=s["tools"], content=TOOLS)
    await ed.call("sync_files", paths=[s["tools"]])


async def test_editor_scripts_never_ship(ed: Editor):
    s = await setup(ed)
    out = ed.scratch.root / "export-menus"
    r = await ed.call("build_project", mode="source", output_dir=str(out), platform="linux", timeout_seconds=900, timeout=1000)
    ed.check_eq(r["status"], "succeeded", f"{r}")
    shipped = [p for p in out.rglob("*.lua")]
    ed.check(any(p.name == "Runtime.lua" for p in shipped), f"runtime Lua ships: {[str(p) for p in shipped][:20]}")
    ed.check(any(p.name == "Helper.lua" for p in shipped), "an Editor/ file without annotations is a runtime script: it ships")
    ed.check(not any(p.name == "Tools.lua" for p in shipped), f"the editor script does not: {[str(p) for p in shipped if p.name == 'Tools.lua']}")


async def test_cpp_menu_item(ed: Editor):
    if not (shutil.which("cmake") and (shutil.which("c++") or shutil.which("g++") or shutil.which("clang++"))):
        skip("no C++ toolchain (cmake + compiler) on PATH")
    s = await setup(ed)
    await ed.call("write_file", path="Editor/Marker.cpp", content=CPP)
    await ed.call("sync_files", paths=["Editor/Marker.cpp"])
    listed = await ed.call("list_editor_menus", target="main")
    items = {i["path"]: i for i in listed["items"]}
    ed.check("Tools/Test/Make Marker" in items, f"the C++ item is found before any build: {listed}")
    ed.check_eq(items["Tools/Test/Make Marker"]["language"], "cpp", "language")
    ed.check_eq(items["Tools/Test/Make Marker"]["priority"], 50, "priority")

    r = await ed.call_raw("run_editor_menu", {"path": "Tools/Test/Make Marker", "scene": s["scene"]}, timeout=1500)
    if r.is_error and ("build tools" in r.text or "cmake" in r.text.lower() and "not found" in r.text.lower()):
        skip("the C++ editor scripts can't build here: " + r.text[:300])
    ed.check(not r.is_error, f"the C++ item ran: {r.text[:1500]}")
    data = r.structured or {}
    ed.check("Make Marker" in (data.get("undo_name") or ""), f"one undo step named after the item: {data}")
    found = await ed.call("find_entities", scene=s["scene"], name="CppMarker")
    ed.check_eq(len(found["entities"]), 1, f"the tool call made the entity: {found}")
    await ed.call("undo", scope="timeline", scene=s["scene"])
    found = await ed.call("find_entities", scene=s["scene"], name="CppMarker")
    ed.check_eq(len(found["entities"]), 0, "undo removes it")
    status = await ed.call("script_status")
    ed.check(status["editor_cpp"]["state"] in ("up_to_date", "none"), f"the editor scripts row: {status['editor_cpp']}")
    # the game's build does not compile the editor script
    exported = ed.scratch.root / "export-menus-cpp"
    r = await ed.call("build_project", mode="source", output_dir=str(exported), platform="linux", timeout_seconds=900, timeout=1000)
    ed.check(not any(p.name == "Marker.cpp" for p in exported.rglob("*.cpp")), "the C++ editor script does not ship")
