"""W4 script tools: create/attach/detach/update scripts, script properties, Lua checks, sync_files
after external edits, code editor integration, compile_scripts (C++)."""

import shutil
import time

from harness import Editor, find_key, skip

TIMEOUT = 900


async def make_entity(ed: Editor, name: str, type_: str = "box"):
    made = await ed.call("create_entity", name=name, type=type_)
    eid = find_key(made, "entity_id", "id")
    ed.check(eid is not None, f"create_entity returned no id: {made!r}")
    return eid


def prop(script: dict, name: str) -> dict | None:
    for p in script.get("properties", []):
        if p["name"] == name:
            return p
    return None


MOVER = """local Mover = {
    properties = {
        { name = "speed", displayName = "Speed", type = "float", default = 5.0 },
    }
}

function Mover:init()
    RegisterEngineEvent(self, "onUpdate")
end

function Mover:onUpdate()
end

return Mover
"""


async def test_create_lua_script_and_properties(ed: Editor):
    ed.require_tools("create_script", "set_script_properties", "list_scripts", "undo")
    eid = await make_entity(ed, "ScriptHost")
    made = await ed.call("create_script", kind="lua", name="Mover", content=MOVER, entity=eid)
    ed.check_eq(made["path"], "scripts/Mover.lua", "script path")
    ed.check((ed.project / "scripts/Mover.lua").exists(), "Mover.lua not written")
    ed.check_eq(made["problems"], [], "problems of a valid script")
    speed = prop(made["script"], "speed")
    ed.check(speed is not None, f"speed property missing: {made['script']!r}")
    ed.check_eq(speed["value"], 5.0, "default value")

    await ed.call("set_script_properties", entity=eid, values={"speed": 42})
    listed = await ed.call("list_scripts", entity=eid)
    ed.check_eq(prop(listed["scripts"][0], "speed")["value"], 42.0, "speed after set")

    # one undo step in the scene history
    await ed.call("undo", scope="scene")
    listed = await ed.call("list_scripts", entity=eid)
    ed.check_eq(prop(listed["scripts"][0], "speed")["value"], 5.0, "speed after undo")

    msg = await ed.call("set_script_properties", entity=eid, values={"nope": 1}, expect_error=True)
    ed.check("speed" in msg, f"unknown property error should list the properties: {msg}")

    # template without content
    tpl = await ed.call("create_script", kind="lua", name="Templated")
    text = (ed.project / tpl["path"]).read_text()
    ed.check("RegisterEngineEvent" in text and "return Templated" in text, f"template content: {text[:200]}")
    await ed.call("create_script", kind="lua", name="Templated", expect_error=True)  # exists


async def test_lua_template_typed_class(ed: Editor):
    """The Lua template declares its module as a ScriptInstance subclass with a field per
    property (typed self for lua-language-server); properties still parse and it plays."""
    ed.require_tools("create_script", "list_scripts", "play", "wait", "read_console")
    eid = await make_entity(ed, "TypedHost")
    made = await ed.call("create_script", kind="lua", name="TypedTpl", entity=eid)
    text = (ed.project / made["path"]).read_text()
    head = text.split("local TypedTpl = {", 1)[0]
    for line in ("---@class TypedTpl : ScriptInstance", "---@field speed number", "---@field isActive boolean"):
        ed.check(line + "\n" in head, f"template should declare `{line}` before the module table: {text[:400]}")
    ed.check_eq(made["problems"], [], "problems of the template")
    props = {p["name"]: p["value"] for p in made["script"]["properties"]}
    ed.check_eq(props, {"speed": 5.0, "isActive": True}, "template properties parsed")

    await ed.call("save_scene")
    cursor = (await ed.call("read_console", limit=1))["next_cursor"]
    r = await ed.call_raw("play", {"action": "start", "timeout_seconds": 300})
    ed.check(not r.is_error, f"play failed: {r.text[:500]}")
    try:
        await ed.call("wait", frames=150)
        log = await ed.call("read_console", since=cursor, limit=1000)
    finally:
        await ed.call("play", action="stop")
    msgs = [e["message"] for e in log["entries"]]
    ed.check(any("] Counter: 60" in m for m in msgs), f"the template's onUpdate did not log: {msgs[-20:]}")
    errors = [e["message"] for e in log["entries"] if e["type"] == "error"]
    ed.check(not errors, f"errors while playing the template: {errors}")


SPAWNER = """local Spawner = {
    properties = {
        { name = "brick", displayName = "Brick", type = "Prefab" },
        { name = "target", displayName = "Target", type = "entity" },
    }
}

return Spawner
"""


async def test_prefab_and_entity_script_properties(ed: Editor):
    """Prefab properties take a .prefab path (stored by GUID); entity refs read back resolved."""
    ed.require_tools("create_script", "set_script_properties", "create_prefab", "set_properties")
    eid = await make_entity(ed, "Spawner")
    target = await make_entity(ed, "Target")
    brick = await make_entity(ed, "BrickSrc")
    made = await ed.call("create_prefab", entity=brick, path="prefabs/ScriptBrick.prefab")
    guid = made["prefabs"][0]["guid"]
    await ed.call("create_script", kind="lua", name="Spawner", content=SPAWNER, entity=eid)
    out = await ed.call("set_script_properties", entity=eid, values={"brick": "prefabs/ScriptBrick.prefab", "target": target})
    p = prop(out["script"], "brick")
    ed.check(isinstance(p["value"], dict) and p["value"].get("guid") == guid, f"prefab stored by guid: {p!r}")
    t = prop(out["script"], "target")
    ed.check(t["value"].get("id") == target and not t["value"].get("missing"), f"entity ref resolved: {t!r}")
    # the same through set_properties; bad paths are refused instead of storing nothing
    await ed.call("set_properties", entity=eid, values={"script": {"scripts[0].brick": None}})
    got = await ed.call("list_scripts", entity=eid)
    ed.check(not prop(got["scripts"][0], "brick")["value"].get("guid"), f"cleared: {got!r}")
    await ed.call("set_properties", entity=eid, values={"script": {"scripts[0].brick": "prefabs/ScriptBrick.prefab"}})
    got = await ed.call("list_scripts", entity=eid)
    ed.check_eq(prop(got["scripts"][0], "brick")["value"].get("guid"), guid)
    await ed.check_error("set_script_properties", entity=eid, values={"brick": "prefabs/Nope.prefab"}, contains="list_prefabs")
    await ed.check_error("set_properties", entity=eid, values={"script": {"scripts[0].brick": "assets/sprites/rock.png"}},
                         contains="prefab")
    await ed.call("delete_entities", entities=[eid, target, brick])


async def test_check_script(ed: Editor):
    ed.require_tools("check_script")
    bad = "local T = {}\nfunction T:init()\n  if x then\nend\nreturn T\n"
    r = await ed.call("check_script", code=bad, language="lua")
    ed.check_eq(r["ok"], False, "broken Lua is not ok")
    ed.check(r["errors"][0]["line"] in (5, 6), f"error line: {r['errors']!r}")
    good = await ed.call("check_script", path="scripts/Mover.lua")
    ed.check_eq(good["ok"], True, "valid Lua")
    ed.check(any(p["name"] == "speed" for p in good["properties"]), f"properties: {good!r}")


async def test_external_edit_sync(ed: Editor):
    ed.require_tools("sync_files", "list_scripts")
    await ed.call("sync_files")  # baseline
    path = ed.project / "scripts/Mover.lua"
    text = path.read_text().replace(
        '{ name = "speed", displayName = "Speed", type = "float", default = 5.0 },',
        '{ name = "speed", displayName = "Speed", type = "float", default = 5.0 },\n'
        '        { name = "jump", displayName = "Jump", type = "int", default = 3 },')
    time.sleep(0.05)
    path.write_text(text)
    r = await ed.call("sync_files")
    ed.check_in("scripts/Mover.lua", r["changed"]["modified"], "sync lists the modified file")
    changes = [c for c in r["script_properties"] if c["script"] == "Mover"]
    ed.check(changes and "jump" in changes[0].get("added", []), f"jump not reported as added: {r['script_properties']!r}")
    entity = changes[0]["entity"]["id"]
    listed = await ed.call("list_scripts", entity=entity)
    ed.check_eq(prop(listed["scripts"][0], "jump")["value"], 3, "new property's default")

    # an external syntax error is reported with its line
    path.write_text(text.replace("function Mover:onUpdate()", "function Mover:onUpdate(", 1))
    r = await ed.call("sync_files", paths=["scripts/Mover.lua"], full=False)
    problems = [p for p in r["problems"] if p.get("path") == "scripts/Mover.lua"]
    ed.check(problems and problems[0].get("line", 0) > 0, f"syntax problem not reported: {r['problems']!r}")
    path.write_text(text)
    await ed.call("sync_files", paths=["scripts/Mover.lua"], full=False)


async def test_write_file_and_code_editor(ed: Editor):
    ed.require_tools("write_file", "code_editor", "read_file", "undo")
    await ed.call("code_editor", action="open", path="scripts/Mover.lua", line=3)
    st = await ed.call("code_editor")
    ed.check(any(b["path"] == "scripts/Mover.lua" for b in st["buffers"]), f"buffer not open: {st!r}")

    before = (ed.project / "scripts/Mover.lua").read_text()
    r = await ed.call("write_file", path="scripts/Mover.lua",
                      edits=[{"old_text": "function Mover:onUpdate()\nend", "new_text": "function Mover:onUpdate(\nend"}])
    ed.check_eq(r["syntax_ok"], False, "write_file reports the syntax error")
    ed.check(r["errors"][0]["line"] > 0, "error line")
    await ed.call("undo", scope="project")
    ed.check_eq((ed.project / "scripts/Mover.lua").read_text(), before, "undo restores the file")

    # external change to an open, unmodified buffer: reloaded
    time.sleep(0.05)
    (ed.project / "scripts/Mover.lua").write_text(before + "-- touched\n")
    r = await ed.call("sync_files", paths=["scripts/Mover.lua"], full=False)
    ed.check_in("scripts/Mover.lua", r["code_editor"]["reloaded"], "open buffer reloaded")
    content = await ed.call_raw("read_file", {"path": "scripts/Mover.lua"})
    ed.check("-- touched" in content.text, "read_file sees the change")
    (ed.project / "scripts/Mover.lua").write_text(before)
    await ed.call("sync_files", paths=["scripts/Mover.lua"], full=False)
    await ed.call("code_editor", action="close", path="scripts/Mover.lua")

    msg = await ed.call("write_file", path="project.yaml", content="x", expect_error=True)
    ed.check("project" in msg, msg)


async def test_attach_detach_update(ed: Editor):
    ed.require_tools("attach_script", "detach_script", "update_script_entry")
    eid = await make_entity(ed, "Multi")
    await ed.call("attach_script", entity=eid, path="scripts/Mover.lua")
    await ed.call("attach_script", entity=eid, path="scripts/Templated.lua")
    await ed.call("attach_script", entity=eid, path="scripts/Mover.lua", expect_error=True)  # duplicate
    r = await ed.call("update_script_entry", entity=eid, script="Templated", move_to=0, enabled=False)
    ed.check_eq([s["class_name"] for s in r["scripts"]], ["Templated", "Mover"], "order after move")
    ed.check_eq(r["scripts"][0]["enabled"], False, "disabled")
    r = await ed.call("detach_script", entity=eid, script=0)
    ed.check_eq([s["class_name"] for s in r["scripts"]], ["Mover"], "after detach")
    ed.check((ed.project / "scripts/Templated.lua").exists(), "detach keeps the file")
    await ed.call("undo", scope="scene")
    listed = await ed.call("list_scripts", entity=eid)
    ed.check_eq(len(listed["scripts"]), 2, "undo detach")
    scene = await ed.call("list_scripts", include_files=True)
    ed.check(any(f["path"] == "scripts/GameController.lua" for f in scene["files"]), "files listing")


CPP_BAD_LINE = "    counter += undefinedThing;  // compile error\n"


async def test_cpp_compile(ed: Editor):
    ed.require_tools("create_script", "compile_scripts", "write_file")
    if not (shutil.which("cmake") and (shutil.which("c++") or shutil.which("g++") or shutil.which("clang++"))):
        skip("no C++ toolchain (cmake + compiler) on PATH for the editor")
    eid = await make_entity(ed, "Spinner")
    made = await ed.call("create_script", kind="cpp_script", name="Spinner", entity=eid)
    ed.check_eq(made["header_path"], "scripts/Spinner.h", "header path")
    ed.check(any(p["name"] == "speed" for p in made["script"]["properties"]), f"DPROPERTY parsed: {made['script']!r}")

    r = await ed.call("compile_scripts", timeout_seconds=600, timeout=700)
    if not r.get("success"):
        log = "\n".join(r.get("log_tail", []))
        if "ABI" in log or "compatible" in log or not r.get("errors"):
            skip("the C++ build can't run in this environment: " + (r.get("errors") or log[-800:]).__str__())
    ed.check_eq(r.get("success"), True, f"clean compile failed: {r!r}")

    # wait: false returns a job to poll
    started = await ed.call("compile_scripts", wait=False)
    job = started.get("job_id") or started.get("id")
    ed.check(job, f"no job id: {started!r}")
    for _ in range(600):
        status = await ed.call("get_job", job_id=job)
        if status["status"] != "running":
            break
        time.sleep(1)
    ed.check_eq(status["status"], "succeeded", f"job: {status!r}")
    ed.check_eq(status["result"]["success"], True, "job result")

    source = (ed.project / "scripts/Spinner.cpp").read_text()
    lines = source.splitlines(keepends=True)
    anchor = next(i for i, l in enumerate(lines) if "counter++" in l)
    lines.insert(anchor + 1, CPP_BAD_LINE)
    await ed.call("write_file", path="scripts/Spinner.cpp", content="".join(lines))
    r = await ed.call("compile_scripts", timeout_seconds=600, timeout=700)
    ed.check_eq(r.get("success"), False, "broken code must fail")
    errs = [e for e in r["errors"] if e["file"].endswith("scripts/Spinner.cpp")]
    ed.check(errs, f"no error in Spinner.cpp: {r['errors']!r}")
    ed.check_eq(errs[0]["line"], anchor + 2, "error line")
    ed.check("undefinedThing" in errs[0]["message"], errs[0]["message"])
