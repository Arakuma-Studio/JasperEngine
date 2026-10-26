"""The Lua API as data (S23 R29, R29a, R46; AC19's MCP part).

jasper://lua-api is the build's lua-api.json (classes, enums, globals, events, the hash);
jasper://lua-api?class=Vector3 is one class. The Lua doc test's runtime half: every ```lua block
of the agent guide and the Lua docs compiles through check_script, and every ```lua run block
runs in a Play fixture through eval_lua without raising.
"""

import json
import os
import sys

from mcp.shared.exceptions import MCPError

from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Lua API CI"}

TIMEOUT = 600

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from lua_api import doctest  # noqa: E402


async def read_text(ed: Editor, uri: str) -> str:
    got = await ed.session.read_resource(uri)
    return "".join(getattr(c, "text", "") or "" for c in got.contents)


async def test_lua_api_resource(ed: Editor):
    api = json.loads(await read_text(ed, "jasper://lua-api"))
    for key in ("format", "formatVersion", "hash", "engineVersion", "classes", "enums", "globals", "events"):
        ed.check(key in api, f"lua-api.json has {key}: {sorted(api)[:20]}")
    names = {c["name"] for c in api["classes"]}
    for name in ("Vector3", "Scene", "GameObject", "Engine"):
        ed.check(name in names, f"class {name} is listed")
    ed.check_eq(api.get("unresolved", []), [], "nothing unresolved")
    one = json.loads(await read_text(ed, "jasper://lua-api?class=Vector3"))
    cls = one
    ed.check_eq(cls["name"], "Vector3", f"one class: {str(one)[:300]}")
    ed.check(cls["methods"], "Vector3 has methods")
    try:
        await ed.session.read_resource("jasper://lua-api?class=NoSuchClass")
    except MCPError as e:
        ed.check("NoSuchClass" in str(e), f"the error names the class: {e}")
    else:
        ed.fail("an unknown class should fail")


async def test_lua_api_reference_doc(ed: Editor):
    listed = await ed.session.list_resources()
    uris = {str(r.uri) for r in listed.resources}
    ed.check("jasper://docs/lua-api-reference" in uris, "the generated reference is a doc")
    reference = await read_text(ed, "jasper://docs/lua-api-reference")
    ed.check("Vector3" in reference, "the reference lists Vector3")


async def test_doc_blocks_compile_and_run(ed: Editor):
    files = doctest.doc_files(ROOT)
    ed.check(files, "the Lua docs are found")
    compiled = 0
    failures = []
    for path in files:
        rel = os.path.relpath(path, ROOT)
        for line, _run, code in doctest.blocks(path):
            r = await ed.call("check_script", code=code, language="lua")
            # check_script also runs the top level: a block that needs a script's self fails
            # there, after it compiled
            errors = [e for e in r.get("errors", []) if not e.get("message", "").startswith("runtime error at load")]
            if r.get("ok") is False and errors:
                failures.append(f"{rel}:{line + errors[0].get('line', 1) - 1}: does not compile: {errors[0].get('message')}")
            compiled += 1
    ed.check(compiled > 0, "some blocks were compiled")
    ed.check(not failures, "\n".join(failures))

    runs = doctest.runtime_blocks(files, ROOT)
    ed.check(runs, "the docs have ```lua run blocks")
    made = await ed.call("create_scene", name="Doc Fixture", type="3D", path="scenes/DocFixture")
    scene = made["scene"]["id"]
    await ed.call("create_entity", scene=scene, type="box", name="Box")
    await ed.call("save_scene", scene=scene, path="scenes/DocFixture.scene")
    started = await ed.call("play", action="start", scene=scene)
    ed.check_eq(started["state"], "playing", "the fixture plays")
    try:
        for where, code in runs:
            r = await ed.call_raw("eval_lua", {"code": code})
            ed.check(not r.is_error, f"{where}: the run block raised: {r.text[:500]}")
    finally:
        await ed.call("play", action="stop")
