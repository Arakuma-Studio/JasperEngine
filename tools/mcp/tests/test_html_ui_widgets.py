"""The HTML UI widgets through MCP (S17 R39-R41, AC14): inspect_ui's widget, tooltip, drag and reload
fields; assert_ui's virtual_window, tooltip and selected checks; edit_ui_view's actions (scroll_to, select,
activate, expand, collapse, sort) and its refusals; a send_input drag in the inventory example that
swaps two slots (read back with inspect_ui's models); a column resize by a send_input drag; a patch
reload that keeps Lua state, with its Console line.

A UI scene with one document: a 10 000-row <list-view> bound to a model, a <table-view> of servers,
a <tree-view> of quests, a button with a title tooltip and a drop zone, all filled by a Lua script.
"""

import sys
from pathlib import Path

from harness import Editor

sys.path.insert(0, str(Path(__file__).resolve().parent))

CI = True
EDITOR = {"create": True, "name": "Html Ui Widgets CI"}

TIMEOUT = 900

S: dict = {}

DOC = "assets/ui/widgets.html"

HTML = """<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <title>Widgets</title>
  <style>
    html, body { margin: 0; background-color: #1e1e1e; font-family: sans-serif; color: #ffffff; font-size: 14px; }
    #list { position: absolute; left: 20px; top: 20px; width: 300px; height: 480px; }
    .row { height: 32px; }
    .htmlui-row.selected { background-color: #2050a0; }
    #servers { position: absolute; left: 360px; top: 20px; width: 420px; height: 240px; }
    #quests { position: absolute; left: 360px; top: 300px; width: 300px; height: 200px; }
    #help { position: absolute; left: 820px; top: 20px; width: 120px; height: 40px; }
    #src { position: absolute; left: 820px; top: 100px; width: 120px; height: 40px; background-color: #505050; }
    #bin { position: absolute; left: 820px; top: 200px; width: 120px; height: 80px; background-color: #303030; }
    #bin.htmlui-drop-allowed { background-color: #00a000; }
  </style>
</head>
<body>
  <div data-model="w">
    <list-view id="list" items="items" as="item, i" item-height="32" key="id" selection="multiple">
      <template><div class="row"><span class="name">{{item.name}}</span></div></template>
    </list-view>
    <table-view id="servers" items="servers" item-height="24" key="id">
      <table-column key="name" width="200" sortable resizable>Name</table-column>
      <table-column key="ping" width="80" min-width="40" sortable resizable>Ping</table-column>
    </table-view>
    <tree-view id="quests" items="quests" item-height="24" key="id">
      <template><div class="quest">{{item.title}}</div></template>
    </tree-view>
  </div>
  <button id="help" title="Hello tip">Help</button>
  <div id="src" draggable="true">Sword</div>
  <div id="bin" dropzone="item move">Bin</div>
</body>
</html>
"""

SCRIPT = """---@class WidgetsProbe : ScriptInstance
local WidgetsProbe = {
    properties = {
        { name = "activations", type = "int", default = 0 },
        { name = "sorts", type = "int", default = 0 },
        { name = "drops", type = "int", default = 0 },
    }
}

function WidgetsProbe:init()
    self.doc = UIDocument(self.scene, self.entity)
    local items = {}
    for k = 1, 10000 do items[k] = { id = "i" .. (k - 1), name = "Item " .. (k - 1) } end
    local servers = {}
    for k = 1, 50 do servers[k] = { id = "s" .. k, name = "Server " .. k, ping = (k * 37) % 200 } end
    local quests = {
        { id = "q1", title = "Chapter 1", children = { { id = "q1a", title = "Find the map" }, { id = "q1b", title = "Cross the river" } } },
        { id = "q2", title = "Chapter 2", children = { { id = "q2a", title = "The tower" } } },
    }
    self.model = self.doc:createDataModel("w", { items = items, servers = servers, quests = quests })
    RegisterEvent(self, self.doc.onLoad, "bind")
    self:bind()
end

function WidgetsProbe:bind()
    _G.widgetsState = _G.widgetsState or { loads = 0 }
    _G.widgetsState.loads = _G.widgetsState.loads + 1
    self.doc:getElementById("list"):addEventListener("activate", function(e) self.activations = self.activations + 1 end)
    self.doc:getElementById("servers"):addEventListener("sort", function(e) self.sorts = self.sorts + 1 end)
    local src = self.doc:getElementById("src")
    src:addEventListener("dragstart", function(e) e.dataTransfer:setData("item", { name = "Sword" }) end)
    self.doc:getElementById("bin"):addEventListener("drop", function(e)
        self.drops = self.drops + 1
        _G.widgetsDropped = e.dataTransfer:getData("item").name
    end)
end

return WidgetsProbe
"""


async def lua(ed: Editor, code: str, **args) -> list:
    return (await ed.call("eval_lua", code=code, **args))["values"]


async def probe(ed: Editor, field: str):
    return (await lua(ed, f"return script('Widgets', 'WidgetsProbe').{field}"))[0]


async def widget(ed: Editor, selector: str) -> dict:
    tree = await ed.call("inspect_ui", entity=S["entity"], selector=selector)
    ed.check(tree["elements"] and "widget" in tree["elements"][0], f"inspect_ui {selector} has a widget: {tree['elements'][:1]}")
    return tree["elements"][0]["widget"]


async def drive(ed: Editor, **args) -> dict:
    r = await ed.call_raw("edit_ui_view", {"entity": S["entity"], **args})
    ed.check(not r.is_error, f"edit_ui_view {args}: {r.text[:300]}")
    return r.structured


async def checks(ed: Editor, *items) -> dict:
    r = await ed.call_raw("assert_ui", {"entity": S["entity"], "checks": list(items), "screenshot": "never"})
    ed.check(not r.is_error, f"assert_ui {items}: {r.text[:300]}")
    return r.structured


async def test_setup_and_play(ed: Editor):
    made = await ed.call("create_scene", name="HtmlUiWidgets", type="UI")
    S["scene"] = made["scene"]["id"]
    await ed.call("write_file", path=DOC, content=HTML)
    ent = await ed.call("create_entity", scene=S["scene"], type="ui_document", name="Widgets")
    S["entity"] = ent["entity_id"]
    await ed.call("set_properties", scene=S["scene"], entity=S["entity"], values={"uidocument": {"filename": DOC}})
    await ed.call("create_script", kind="lua", name="WidgetsProbe", content=SCRIPT, scene=S["scene"], entity=S["entity"])
    await ed.call("save_scene", scene=S["scene"], path="scenes/HtmlUiWidgets")
    await ed.call("wait", frames=5)

    # a group tool (S31): hidden until the session enables html_ui, callable by name either way
    default = {t.name for t in await ed.tools(groups=None)}
    ed.check("edit_ui_view" not in default, "edit_ui_view is hidden until html_ui is enabled")
    await ed.call("tool_groups", enable=["html_ui"])
    ed.check("edit_ui_view" in {t.name for t in await ed.tools(groups=None)}, "listed once html_ui is enabled")

    # edit mode: edit_ui_view refuses (the views' data come from Lua)
    r = await ed.call_raw("edit_ui_view", {"entity": S["entity"], "selector": "#list", "action": "scroll_to", "index": 5})
    ed.check(r.is_error and "start play first" in r.text, f"edit_ui_view in edit mode: {r.text}")

    await ed.console_since(None, limit=1)
    ed.check(await ed.call("play", action="start", scene=S["scene"], fixed_fps=60), "play started")
    await ed.call("wait", frames=10)


async def test_inspect_widget(ed: Editor):
    w = await widget(ed, "#list")
    ed.check_eq((w["kind"], w["count"], w["source"], w["selection"]), ("list", 10000, "model:w", "multiple"), "the list view")
    ed.check(0 < w["pool"] <= w["pool_bound"] and w["first"] == 0 and w["last"] >= 14, f"the window at rest: {w}")
    ed.check(w["layout_boundary"] and w["row_aliases"], f"the fork patches are in use: {w}")
    tree = await ed.call("inspect_ui", entity=S["entity"])
    ed.check(tree["tooltip"] == {"visible": False} and tree["drag"] == {"active": False},
             f"no tooltip, no drag: {tree.get('tooltip')}, {tree.get('drag')}")
    line = next((t for t in (await ed.call_raw("inspect_ui", {"entity": S["entity"], "selector": "#list"})).text.splitlines()
                 if "list-view" in t), "")
    ed.check("list[0.." in line and "of 10000, pool" in line, f"the text line names the window: {line!r}")


async def test_ui_widget_actions(ed: Editor):
    d = await drive(ed, selector="#list", action="scroll_to", index=5000, align="start")
    ed.check_eq((d["done"], d["first"], d["count"]), (True, 5000, 10000), "scroll_to 5000 (start)")
    await ed.call("wait", frames=2)
    w = await widget(ed, "#list")
    ed.check(w["first"] == 5000 and w["scrollTop"] == 5000 * 32, f"inspect_ui sees the scroll: {w}")
    row = (await lua(ed, "return object('Widgets', 'UIDocument'):getElementById('list').itemsView:getRow(5001):querySelector('.name').textContent"))[0]
    ed.check_eq(row, "Item 5000", "the row shows its item")

    d = await drive(ed, selector="#list", action="select", index=5003)
    ed.check_eq((d["selected"], d["active"]), ([5003], 5003), "select 5003")
    d = await drive(ed, selector="#list", action="select", key="i5004")
    ed.check_eq(d["selected"], [5004], "select by key")
    await drive(ed, selector="#list", action="activate", index=5004)
    ed.check_eq(await probe(ed, "activations"), 1, "activate fired the activate event")

    d = await drive(ed, selector="#quests", action="expand", key="q1")
    ed.check(d["done"] and d["count"] == 4, f"expand q1: {d}")
    d = await drive(ed, selector="#quests", action="collapse", key="q1")
    ed.check(d["done"] and d["count"] == 2, f"collapse q1: {d}")

    await drive(ed, selector="#servers", action="sort", column="ping", direction="desc")
    ed.check_eq(await probe(ed, "sorts"), 1, "sort fired the sort event (the script sorts its data)")
    w = await widget(ed, "#servers")
    ed.check_eq(w.get("sort"), {"key": "ping", "direction": "desc"}, "inspect_ui reports the sort")

    for args, text in (({"selector": "#help", "action": "select", "index": 0}, "is not a list-view"),
                       ({"selector": "#list", "action": "select", "index": 20000}, "needs an index of 0..9999"),
                       ({"selector": "#list", "action": "select", "key": "nope"}, "no item has the key"),
                       ({"selector": "#servers", "action": "sort", "column": "nope"}, "no sortable column"),
                       ({"selector": "#list", "action": "jump"}, "action must be")):
        r = await ed.call_raw("edit_ui_view", {"entity": S["entity"], **args})
        ed.check(r.is_error and text in r.text, f"edit_ui_view {args}: {r.text}")


async def test_assert_widget_checks(ed: Editor):
    data = await checks(ed, {"type": "virtual_window", "selector": "#list"},
                        {"type": "virtual_window", "selector": "#list", "max": 40},
                        {"type": "selected", "selector": "#list", "count": 1},
                        {"type": "virtual_window", "selector": "#list", "max": 2},
                        {"type": "selected", "selector": "#list", "min": 2})
    ed.check_eq([c["passed"] for c in data["checks"]], [True, True, True, False, False], f"virtual_window / selected: {data['checks']}")
    data = await checks(ed, {"type": "virtual_window", "selector": "#help"})
    ed.check(not data["passed"], f"virtual_window on a button fails: {data['checks']}")


async def test_tooltip(ed: Editor):
    await ed.call("send_input", events=[{"type": "mouse", "action": "move", "target": "#help"}], frames_after=45)
    tree = await ed.call("inspect_ui", entity=S["entity"])
    tip = tree["tooltip"]
    ed.check(tip["visible"] and tip["text"] == "Hello tip" and tip["side"] in ("bottom", "top") and "help" in tip["source"],
             f"the title tooltip after its delay: {tip}")
    data = await checks(ed, {"type": "tooltip", "selector": "#help", "text": "Hello tip"},
                        {"type": "tooltip", "selector": "#help", "text": "Other"},
                        {"type": "tooltip", "selector": "#src"})
    ed.check_eq([c["passed"] for c in data["checks"]], [True, False, False], f"tooltip checks: {data['checks']}")
    await ed.call("send_input", events=[{"type": "mouse", "action": "move", "x": 5, "y": 700}], frames_after=3)
    ed.check_eq((await ed.call("inspect_ui", entity=S["entity"]))["tooltip"], {"visible": False}, "hidden on leave")


async def test_drag(ed: Editor):
    await ed.call("send_input", events=[{"type": "mouse", "action": "drag", "target": "#src", "to_target": "#bin", "steps": 8}],
                  frames_after=5)
    ed.check_eq(await probe(ed, "drops"), 1, "the drop reached the dropzone")
    ed.check_eq(await lua(ed, "return _G.widgetsDropped, UIDrag.isActive()"), ["Sword", False], "the payload, the drag ended")
    # a programmatic drag in progress shows in inspect_ui
    await lua(ed, """local d = object('Widgets', 'UIDocument')
        UIDrag.begin(d:getElementById('src'), { data = { item = { name = 'Axe' } }, effectAllowed = 'move' })
        UIDrag.over(d:getElementById('bin'))""")
    drag = (await ed.call("inspect_ui", entity=S["entity"]))["drag"]
    ed.check(drag["active"] and drag["programmatic"] and drag["allowed"] and drag["types"] == ["item"] and "bin" in drag["target"],
             f"the drag in progress: {drag}")
    await lua(ed, "UIDrag.drop(object('Widgets', 'UIDocument'):getElementById('bin'))")
    ed.check_eq(await probe(ed, "drops"), 2, "UIDrag.drop dropped")
    ed.check_eq((await ed.call("inspect_ui", entity=S["entity"]))["drag"], {"active": False}, "no drag")


async def test_column_resize_by_drag(ed: Editor):
    """AC6: dragging a header edge resizes the column and every pooled cell equally."""
    widths = """local d = object('Widgets', 'UIDocument')
        local head = d:querySelector('#servers .htmlui-header-cell')
        local grip = head:querySelector('.htmlui-column-resizer'):getBoundingClientRect()
        local cells = d:querySelectorAll('#servers .htmlui-cell[data-column="name"]')
        return head:getBoundingClientRect().width, cells[1]:getBoundingClientRect().width,
               cells[#cells]:getBoundingClientRect().width, grip.left + grip.width / 2, grip.top + grip.height / 2"""
    before = await lua(ed, widths)
    ed.check(abs(before[0] - 200) < 1.5 and abs(before[1] - before[0]) < 1.5, f"the name column at 200 px: {before}")
    x, y = before[3], before[4]
    await ed.call("send_input", events=[{"type": "mouse", "action": "drag", "x": x, "y": y, "to_x": x + 60, "to_y": y, "steps": 6}],
                  frames_after=3)
    after = await lua(ed, widths)
    ed.check(abs(after[0] - 260) < 3 and abs(after[1] - after[0]) < 1.5 and abs(after[2] - after[0]) < 1.5,
             f"header and cells widened by 60 px together: {before[:3]} -> {after[:3]}")


async def test_inventory_drag_swaps(ed: Editor):
    """AC14: in the inventory example, a send_input drag from slot 0 to slot 5 swaps the items (inspect_ui models)."""
    await ed.call("play", action="stop")
    made = await ed.call("create_scene", name="HtmlUiWidgetsInventory", type="UI")
    inv = await ed.call("create_ui_document", scene=made["scene"]["id"], html="assets/ui/bag.html", template="inventory")
    await ed.call("play", action="start", scene=made["scene"]["id"], fixed_fps=60)
    try:
        await ed.call("wait", frames=60)
        before = (await ed.call("inspect_ui", entity=inv["entity"]["id"], models=True))["models"]["inventory"]["items"]
        await ed.call("send_input", events=[{"type": "mouse", "action": "drag", "target": ".slot:nth-child(1)",
                                             "to_target": ".slot:nth-child(6)", "steps": 8}], frames_after=5)
        after = (await ed.call("inspect_ui", entity=inv["entity"]["id"], models=True))["models"]["inventory"]["items"]
        ed.check(before[0]["id"] != before[5]["id"], f"two different slots: {before[0]['id']!r}, {before[5]['id']!r}")
        ed.check_eq((after[0]["id"], after[5]["id"]), (before[5]["id"], before[0]["id"]), "slot 0 and slot 5 swapped")
    finally:
        await ed.call("play", action="stop")
    await ed.call("play", action="start", scene=S["scene"], fixed_fps=60)
    await ed.call("wait", frames=10)


async def test_patch_reload_keeps_state(ed: Editor):
    """A saved edit patches the playing document: the list's scroll and selection and the Lua state stay."""
    await drive(ed, selector="#list", action="scroll_to", index=300, align="start")
    await drive(ed, selector="#list", action="select", index=301)
    await lua(ed, "_G.keptHandle = object('Widgets', 'UIDocument'):getElementById('help')")
    await ed.console_since(None, limit=1)
    await ed.call("write_file", path=DOC, content=HTML.replace(">Help</button>", ">Help me</button>"))
    await ed.call("wait", frames=10)
    tree = await ed.call("inspect_ui", entity=S["entity"], reload=True)
    report = tree["reload"]
    ed.check(report and report["mode"] == "patch" and report["updated"] >= 1 and report["replaced"] == 0 and report["fallbackReason"] is None,
             f"a patch: {report}")
    ed.check_eq(await lua(ed, "local h = _G.keptHandle; return h:isValid(), h.textContent, _G.widgetsState.loads"),
                [True, "Help me", 1], "the same element, its new text, no onLoad")
    w = await widget(ed, "#list")
    ed.check(w["first"] == 300 and w["selected"] == [301], f"scroll and selection kept: {w}")
    lines = [e.get("message", "") for e in await ed.console_since(limit=200)]
    ed.check(any(line.startswith("[HTML UI] Patched ") and "widgets.html" in line for line in lines),
             f"one Console line for the reload: {lines[-5:]}")


async def test_stop_and_errors(ed: Editor):
    await ed.call("play", action="stop")
    lines = [e.get("message", "") for e in await ed.console_since(types=["error", "warning"], limit=1000)]
    ed.check(not lines, f"errors or warnings: {lines[:10]}")
