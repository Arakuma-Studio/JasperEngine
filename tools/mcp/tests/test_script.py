"""Public script editing contracts on a small project owned by this module."""

from harness import Editor

EDITOR = {"create": True, "name": "Script Tools"}

MOVER = """local Mover = {
    properties = {
        { name = "speed", type = "float", default = 5.0 },
    }
}
function Mover:init()
    RegisterEngineEvent(self, "onUpdate")
end
function Mover:onUpdate()
end
return Mover
"""


def prop(script: dict, name: str) -> dict:
    return next(p for p in script["properties"] if p["name"] == name)


async def make_entity(ed: Editor, name: str):
    return (await ed.call("create_entity", name=name, type="box"))["entity_id"]


async def test_create_lua_script_and_properties(ed: Editor):
    entity = await make_entity(ed, "PropertyHost")
    made = await ed.call("create_script", kind="lua", name="PropertyProbe", content=MOVER, entity=entity)
    path = ed.project / made["path"]
    ed.check_eq(path.read_text(), MOVER, "script source is written without alteration")
    ed.check_eq(prop(made["script"], "speed")["value"], 5.0)
    await ed.check_error("create_script", kind="lua", name="PropertyProbe", content="return {}", contains="exists")
    ed.check_eq(path.read_text(), MOVER, "creating an existing script does not overwrite user code")

    await ed.call("set_script_properties", entity=entity, values={"speed": 42})
    listed = await ed.call("list_scripts", entity=entity)
    ed.check_eq(prop(listed["scripts"][0], "speed")["value"], 42.0)
    await ed.call("undo", scope="scene")
    listed = await ed.call("list_scripts", entity=entity)
    ed.check_eq(prop(listed["scripts"][0], "speed")["value"], 5.0, "one undo restores the property")
    await ed.check_error("set_script_properties", entity=entity, values={"speed": 17, "unknown": 1}, contains="speed")
    listed = await ed.call("list_scripts", entity=entity)
    ed.check_eq(prop(listed["scripts"][0], "speed")["value"], 5.0, "an invalid update changes nothing")


async def test_prefab_and_entity_script_properties(ed: Editor):
    entity = await make_entity(ed, "ReferenceHost")
    target = await make_entity(ed, "ReferenceTarget")
    brick = await make_entity(ed, "PrefabSource")
    made = await ed.call("create_prefab", entity=brick, path="prefabs/ScriptBrick.prefab")
    guid = made["prefabs"][0]["guid"]
    source = """return { properties = {
    { name = "brick", type = "Prefab" },
    { name = "target", type = "entity" },
} }
"""
    script = await ed.call("create_script", kind="lua", name="ReferenceProbe", content=source, entity=entity)
    out = await ed.call("set_script_properties", entity=entity,
                        values={"brick": "prefabs/ScriptBrick.prefab", "target": target})
    ed.check_eq(prop(out["script"], "brick")["value"].get("guid"), guid)
    reference = prop(out["script"], "target")["value"]
    ed.check(reference.get("id") == target and not reference.get("missing"), "entity property resolves its target")
    await ed.call("set_properties", entity=entity, values={"script": {"scripts[0].brick": None}})
    cleared = (await ed.call("list_scripts", entity=entity))["scripts"][0]
    ed.check(not prop(cleared, "brick")["value"].get("guid"), "None clears a prefab property")
    await ed.call("set_properties", entity=entity,
                  values={"script": {"scripts[0].brick": "prefabs/ScriptBrick.prefab"}})
    restored = (await ed.call("list_scripts", entity=entity))["scripts"][0]
    ed.check_eq(prop(restored, "brick")["value"].get("guid"), guid)
    ed.check((ed.project / script["path"]).is_file(), "wrong-type fixture exists")
    await ed.check_error("set_properties", entity=entity,
                         values={"script": {"scripts[0].brick": script["path"]}}, contains="prefab")


async def test_check_script(ed: Editor):
    good = await ed.call("check_script", code=MOVER, language="lua")
    ed.check(good["ok"], good)
    ed.check({"name": "speed", "type": "float"} in good["properties"], "checker exposes the authored property schema")
    broken = await ed.call("check_script", code="local T = {}\n\nlocal x = = 1\nreturn T\n", language="lua")
    ed.check_eq(broken["ok"], False)
    ed.check_eq(broken["errors"][0]["line"], 3, "syntax error points at the authored broken line")
    bad_default = await ed.call("check_script", code='return { properties = {{ name = "speed", type = "float", default = "fast" }} }',
                                language="lua")
    ed.check_eq(bad_default["ok"], False)
    ed.check(bad_default["errors"] and "speed" in str(bad_default["errors"]), "invalid property default has a located property error")
    await ed.check_alive()


async def test_write_file_reports_syntax_and_undoes(ed: Editor):
    made = await ed.call("create_script", kind="lua", name="WriteProbe", content=MOVER)
    path = made["path"]
    broken = MOVER.replace("function Mover:onUpdate()", "function Mover:onUpdate(")
    result = await ed.call("write_file", path=path, content=broken)
    ed.check_eq(result["syntax_ok"], False)
    ed.check(result["errors"][0]["line"] > 0, "write_file exposes the Lua diagnostic location")
    await ed.call("undo", scope="project")
    ed.check_eq((ed.project / path).read_text(), MOVER, "undo restores the authored source bytes")
    before = (ed.project / "project.yaml").read_bytes()
    await ed.check_error("write_file", path="project.yaml", content="x", contains="project")
    ed.check_eq((ed.project / "project.yaml").read_bytes(), before, "protected project configuration is not overwritten")


async def test_attach_detach_update(ed: Editor):
    entity = await make_entity(ed, "MultipleScripts")
    first = await ed.call("create_script", kind="lua", name="FirstProbe", content=MOVER)
    second_source = "return { properties = {} }\n"
    second = await ed.call("create_script", kind="lua", name="SecondProbe", content=second_source)
    await ed.call("attach_script", entity=entity, path=first["path"])
    await ed.call("attach_script", entity=entity, path=second["path"])
    await ed.check_error("attach_script", entity=entity, path=first["path"], contains="already")
    moved = await ed.call("update_script_entry", entity=entity, script="SecondProbe", move_to=0, enabled=False)
    ed.check_eq([s["class_name"] for s in moved["scripts"]], ["SecondProbe", "FirstProbe"])
    ed.check_eq(moved["scripts"][0]["enabled"], False)
    detached = await ed.call("detach_script", entity=entity, script=0)
    ed.check_eq([s["class_name"] for s in detached["scripts"]], ["FirstProbe"])
    ed.check_eq((ed.project / second["path"]).read_text(), second_source, "detaching keeps source files")
    await ed.call("undo", scope="scene")
    restored = (await ed.call("list_scripts", entity=entity))["scripts"]
    ed.check_eq([s["class_name"] for s in restored], ["SecondProbe", "FirstProbe"])
    files = (await ed.call("list_scripts", include_files=True))["files"]
    ed.check({first["path"], second["path"]} <= {f["path"] for f in files}, "authored script files remain discoverable")
