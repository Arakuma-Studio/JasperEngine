"""Data Assets (docs/specs/S95-lua-runtime-gaps.md REQ-20 to REQ-24, AC-21): edit_data_asset lists the
data classes, creates a .dataasset, gets and sets its values (checked first, one undo step), reports
values of no field; get_asset_info shows the class and values; a script property typed with the class
and a list of them hold assets, and Play reads them as shared tables (DataAsset.load, loadAll)."""

from __future__ import annotations

import json
import re

from harness import Editor

CI = True
EDITOR = {"create": True, "name": "DataAssets"}

ITEM = """---@dataasset
-- an item of the shop
local ItemData = { properties = {
    { name = "displayName", type = "string", default = "Item" },
    { name = "damage", type = "int", default = 5 },
    { name = "tags", type = "list", of = "string" },
    { name = "upgrade", type = "ItemData" },
} }
function ItemData:describe() return self.displayName .. ":" .. self.damage end
return ItemData
"""

SHOP = """local Shop = { properties = {
    { name = "featured", type = "ItemData" },
    { name = "stock", type = "list", of = "ItemData" },
} }
function Shop:init()
    local all = DataAsset.loadAll("ItemData")
    print("shop " .. self.featured:describe() .. " " .. #self.stock .. " " .. tostring(rawequal(self.stock[1], self.featured)) ..
          " " .. #all .. " " .. self.stock[2]:describe() .. " " .. tostring(rawequal(self.stock[2].upgrade, self.featured)))
end
return Shop
"""

SWORD = "assets/items/sword.dataasset"
SHIELD = "assets/items/shield.dataasset"


def shop_lines(entries: list[dict]) -> list[str]:
    return [e["message"] for e in entries if e["message"].startswith("shop ")]


async def test_the_group_lists_it_once_enabled(ed: Editor):
    ed.check("edit_data_asset" not in [t.name for t in await ed.tools(groups=None)], "hidden until scripting is enabled")
    await ed.call("tool_groups", enable=["scripting"])
    ed.check_in("edit_data_asset", [t.name for t in await ed.tools(groups=None)], "the scripting group lists it")


async def test_data_assets_from_class_to_play(ed: Editor):
    listed = await ed.call("edit_data_asset", action="list")
    ed.check_eq(listed["classes"], [], "no data class yet")
    ed.check("---@dataasset" in listed.get("next", ""), "the next step says how to make one")
    await ed.call("create_script", kind="lua", name="ItemData", content=ITEM)
    listed = await ed.call("edit_data_asset", action="list")
    ed.check_eq([c["class"] for c in listed["classes"]], ["ItemData"])
    ed.check_eq(listed["classes"][0]["fields"], [{"name": "displayName", "type": "string"}, {"name": "damage", "type": "int"},
                                                 {"name": "tags", "type": "list:string"}, {"name": "upgrade", "type": "ItemData"}])

    made = await ed.call("edit_data_asset", action="create", path=SWORD, **{"class": "ItemData"},
                         values={"displayName": "Sword", "damage": 12})
    ed.check(made["written"] and len(made["guid"]) == 32, f"created with a GUID: {made}")
    ed.check_eq(made["values"]["tags"], [], "a field not given has its default")
    # the path Lua loads it by (relative to the assets folder), as the next step names it
    load_path = re.search(r'DataAsset\.load\("([^"]+)"\)', made["next"]).group(1)
    ed.check(load_path.endswith("items/sword.dataasset"), f"next: {made['next']}")
    on_disk = json.loads((ed.project / SWORD).read_text())
    ed.check_eq((on_disk["version"], on_disk["class"], on_disk["values"]["damage"]), (1, "ItemData", 12))
    ed.check((ed.project / (SWORD + ".meta")).is_file(), "its GUID is in a .meta")
    await ed.check_error("edit_data_asset", action="create", path=SWORD, **{"class": "ItemData"}, contains="exists")

    # set: every value checked first, nothing written when one is wrong
    before = (ed.project / SWORD).read_text()
    await ed.check_error("edit_data_asset", action="set", path=SWORD, values={"damage": "lots", "displayName": "Big"}, contains="damage (int)")
    await ed.check_error("edit_data_asset", action="set", path=SWORD, values={"weight": 3}, contains="displayName, damage, tags, upgrade")
    ed.check_eq((ed.project / SWORD).read_text(), before, "a refused set writes nothing")
    changed = await ed.call("edit_data_asset", action="set", path=SWORD, values={"damage": 20, "tags": ["sharp", "steel"]})
    ed.check_eq((changed["values"]["damage"], changed["values"]["tags"]), (20, ["sharp", "steel"]))
    await ed.call("undo", scope="project")
    got = await ed.call("edit_data_asset", action="get", path=SWORD)
    ed.check_eq((got["values"]["damage"], got["values"]["tags"]), (12, []), "one undo step puts the values back")

    shield = await ed.call("edit_data_asset", action="create", path=SHIELD, **{"class": "ItemData"},
                           values={"displayName": "Shield", "damage": 2, "upgrade": made["guid"]})
    ed.check_eq(shield["values"]["upgrade"]["guid"], made["guid"], "a reference by GUID")
    await ed.check_error("edit_data_asset", action="set", path=SHIELD, values={"upgrade": "assets/nothing.dataasset"}, contains="no asset")

    # a field renamed in the class: its value stays in the file, the check reports it
    data = json.loads((ed.project / SHIELD).read_text())
    data["values"]["armour"] = 4
    (ed.project / SHIELD).write_text(json.dumps(data))
    await ed.call("sync_files")
    got = await ed.call("edit_data_asset", action="get", path=SHIELD)
    ed.check_eq(got["unknown_values"], {"armour": 4})
    ed.check(any("armour" in p for p in got["problems"]), f"reported: {got['problems']}")
    info = await ed.call("get_asset_info", path=SHIELD)
    ed.check_eq((info["type"], info["data_asset"]["class"], info["data_asset"]["values"]["displayName"]), ("data_asset", "ItemData", "Shield"))

    # a script property typed with the class, and a list of them
    entity = (await ed.call("create_entity", name="Shop", type="empty"))["entity_id"]
    script = await ed.call("create_script", kind="lua", name="Shop", content=SHOP, entity=entity)
    types = {p["name"]: p["type"] for p in script["script"]["properties"]}
    ed.check_eq(types, {"featured": "ItemData", "stock": "list:ItemData"})
    await ed.check_error("set_script_properties", entity=entity, values={"featured": "assets/items"}, contains="featured")
    await ed.call("set_script_properties", entity=entity, values={"featured": SWORD, "stock": [made["guid"], SHIELD]})
    listed = (await ed.call("list_scripts", entity=entity))["scripts"][0]
    stock = next(p for p in listed["properties"] if p["name"] == "stock")["value"]
    ed.check_eq([item["guid"] for item in stock], [made["guid"], shield["guid"]], "the list holds both by GUID")

    await ed.console_since(None)
    await ed.call("play", action="start", auto_pause=True, fixed_fps=60)
    try:
        await ed.call("wait", frames=2)
        ed.check_eq(shop_lines(await ed.console_since()), ["shop Sword:12 2 true 2 Shield:2 true"],
                    "Play: one shared table per asset, loadAll, references resolved")
        value = await ed.call("eval_lua", code="local s = assert(DataAsset.load('" + load_path + "')); s.damage = 99; "
                                                "return DataAsset.load('" + made["guid"] + "').damage, DataAsset.classOf(s)")
        ed.check_eq(value["values"], [99, "ItemData"])
    finally:
        await ed.call("play", action="stop")
    ed.check_eq(json.loads((ed.project / SWORD).read_text())["values"]["damage"], 12, "nothing is written back after Play")
