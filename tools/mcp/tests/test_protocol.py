"""Core protocol tests (docs/mcp/DESIGN.md: transports, registry, calls, batch, legacy tools).

Tests that need tools not built yet skip themselves (ed.require_tools / ed.first_tool), so this
file passes cleanly while the server grows.
"""

import re
import uuid

from harness import Editor, ProtocolError, RawEditor, ToolError, entity_id_of, find_entities, find_key

PROTOCOL_VERSIONS = {"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"}

# The tool that lists a scene's GameObjects
LIST_TOOLS = ("find_entities",)
UNDO_TOOLS = ("undo",)
REDO_TOOLS = ("redo",)


async def scene_entities(ed: Editor) -> list[dict]:
    return find_entities(await ed.call(ed.first_tool(*LIST_TOOLS), limit=100000))


async def has_entity(ed: Editor, entity_id) -> bool:
    return any(entity_id_of(e) == entity_id for e in await scene_entities(ed))


async def create_box(ed: Editor, name: str):
    """create_entity; returns the new GameObject id."""
    ed.require_tools("create_entity")
    made = await ed.call("create_entity", name=name, type="box", position=[1, 2, 3])
    eid = find_key(made, "entity_id", "id")
    ed.check(eid is not None, f"create_entity returned no id: {made!r}")
    return eid


# -- lifecycle --------------------------------------------------------------


async def test_initialize(ed: Editor):
    r = ed.init_result
    ed.check(r is not None, "no initialize result")
    ed.check_in(r.protocol_version, PROTOCOL_VERSIONS, "negotiated protocol version")
    ed.check(r.server_info and r.server_info.name, "serverInfo.name is empty")
    ed.check(r.capabilities and r.capabilities.tools is not None, "server does not declare the tools capability")
    ed.check(ed.instructions.strip(), "initialize.instructions (the agent guide) is empty")


async def test_ping(ed: Editor):
    await ed.ping()


async def test_tools_list(ed: Editor):
    tools = await ed.tools()
    ed.check(len(tools) > 0, "tools/list is empty")
    names = [t.name for t in tools]
    dupes = sorted({n for n in names if names.count(n) > 1})
    ed.check(not dupes, f"duplicate tool names: {dupes}")
    bad = []
    for t in tools:
        if not re.fullmatch(r"[a-z][a-z0-9_]*", t.name):
            bad.append(f"{t.name}: name is not snake_case")
        if not (t.description or "").strip():
            bad.append(f"{t.name}: empty description")
        schema = t.input_schema or {}
        if schema.get("type") != "object":
            bad.append(f"{t.name}: inputSchema.type is {schema.get('type')!r}, not 'object'")
        props = schema.get("properties", {})
        if not isinstance(props, dict):
            bad.append(f"{t.name}: inputSchema.properties is not an object")
        for req in schema.get("required", []) or []:
            if req not in props:
                bad.append(f"{t.name}: required {req!r} is not in properties")
    ed.check(not bad, "bad tool definitions:\n  " + "\n  ".join(bad))


async def test_editor_status(ed: Editor):
    ed.require_tools("editor_status")
    status = await ed.call("editor_status")
    ed.check(status, f"editor_status returned nothing: {status!r}")


async def test_resources_guide(ed: Editor):
    """jasper://guide carries the same agent guide as initialize.instructions (once resources land)."""
    caps = ed.init_result.capabilities
    if not caps or caps.resources is None:
        from harness import skip
        skip("resources capability not declared yet")
    listed = await ed.session.list_resources()
    uris = [str(r.uri) for r in listed.resources]
    if "jasper://guide" not in uris:
        from harness import skip
        skip("jasper://guide not registered yet")
    got = await ed.session.read_resource("jasper://guide")
    text = "".join(getattr(c, "text", "") or "" for c in got.contents)
    ed.check(len(text) > 200, "jasper://guide is (nearly) empty")


# -- errors -------------------------------------------------------------------


async def test_unknown_tool(ed: Editor):
    try:
        r = await ed.call_raw("no_such_tool_" + uuid.uuid4().hex[:8], {})
        ed.check(r.is_error, f"an unknown tool succeeded: {r.text[:300]}")
    except ProtocolError:
        pass  # DESIGN: unknown tool is a JSON-RPC error; an isError result is tolerated
    await ed.ping()  # still alive


async def test_bad_argument_is_tool_error(ed: Editor):
    """Agent mistakes are isError results that say what is wrong, not crashes or protocol errors."""
    ed.require_tools("create_entity")
    try:
        msg = await ed.call("create_entity", name="Bad", type="definitely_not_a_type", expect_error=True)
    except ProtocolError as e:
        msg = str(e)  # schema enum validation may reject it at the protocol level: also fine
    ed.check(msg.strip(), "error without a message")
    await ed.ping()


# -- edits through commands -------------------------------------------------------


async def test_create_entity_undo_redo(ed: Editor):
    ed.require_tools("create_entity")
    undo = ed.first_tool(*UNDO_TOOLS)
    ed.first_tool(*LIST_TOOLS)
    name = "McpBox_" + uuid.uuid4().hex[:6]
    eid = await create_box(ed, name)
    ed.check(await has_entity(ed, eid), f"created GameObject {eid} ({name}) is not listed")

    await ed.call(undo)
    ed.check(not await has_entity(ed, eid), f"GameObject {eid} still exists after {undo}")

    if any(ed.has_tool(t) for t in REDO_TOOLS):
        await ed.call(ed.first_tool(*REDO_TOOLS))
        ed.check(await has_entity(ed, eid), f"GameObject {eid} is missing after redo")
        await ed.call(undo)


async def test_batch(ed: Editor):
    ed.require_tools("batch", "create_entity")
    ed.first_tool(*LIST_TOOLS)
    names = ["McpBatchA_" + uuid.uuid4().hex[:6], "McpBatchB_" + uuid.uuid4().hex[:6]]
    out = await ed.batch([("create_entity", {"name": n, "type": "box"}) for n in names], undo_group=True)
    results = out.get("results") if isinstance(out, dict) else out
    ed.check(isinstance(results, list) and len(results) == 2, f"batch should return 2 results: {out!r}")
    ids = [find_key(r, "entity_id", "id") for r in results]
    ed.check(all(i is not None for i in ids), f"batch results lack ids: {out!r}")
    listed = {entity_id_of(e) for e in await scene_entities(ed)}
    ed.check(all(i in listed for i in ids), f"batch-created {ids} not all listed")

    # undo_group: both creations are one undo step
    await ed.call(ed.first_tool(*UNDO_TOOLS))
    listed = {entity_id_of(e) for e in await scene_entities(ed)}
    ed.check(not any(i in listed for i in ids), f"one undo should remove the whole batch; still listed: "
                                                f"{[i for i in ids if i in listed]}")


async def test_batch_stop_on_error(ed: Editor):
    ed.require_tools("batch", "create_entity")
    out = await ed.call("batch", calls=[
        {"tool": "no_such_tool_xyz", "arguments": {}},
        {"tool": "create_entity", "arguments": {"name": "NeverMade", "type": "box"}},
    ], stop_on_error=True)
    results = out.get("results") if isinstance(out, dict) else out
    ed.check(isinstance(results, list) and len(results) == 1,
             f"stop_on_error should stop after the failing call: {out!r}")


# -- stdio transport, raw ------------------------------------------------------


async def test_stdio_raw():
    """Every stdout line is a JSON-RPC 2.0 message (the editor's own prints go to stderr), the
    client's protocol version is echoed, ping works, and closing stdin shuts the editor down."""
    async with RawEditor() as raw:
        init = await raw.initialize(version="2025-06-18")
        if "error" in init:
            raw.fail(f"initialize failed: {init['error']}")
        if init["result"].get("protocolVersion") != "2025-06-18":
            raw.fail(f"protocol version not echoed: {init['result'].get('protocolVersion')}")
        raw.send({"jsonrpc": "2.0", "id": 2, "method": "tools/list"})
        tools = (await raw.response(2))["result"]["tools"]
        raw.send({"jsonrpc": "2.0", "id": 3, "method": "ping"})
        await raw.response(3)
        if any(t["name"] == "editor_status" for t in tools):
            raw.send({"jsonrpc": "2.0", "id": 4, "method": "tools/call",
                      "params": {"name": "editor_status", "arguments": {}}})
            await raw.response(4)
        if any(t["name"] == "create_entity" for t in tools):
            raw.send({"jsonrpc": "2.0", "id": 5, "method": "tools/call",
                      "params": {"name": "create_entity", "arguments": {"name": "RawBox", "type": "box"}}})
            await raw.response(5)
        raw.send({"jsonrpc": "2.0", "id": 6, "method": "no/such/method"})
        err = await raw.response(6)
        if "error" not in err:
            raw.fail(f"unknown method should be a JSON-RPC error: {err}")
        if raw.bad_lines:
            raw.fail(f"{len(raw.bad_lines)} non-JSON-RPC stdout line(s), first: {raw.bad_lines[:3]!r}")
        code = await raw.close_stdin_and_wait(30)
        if code is None:
            raw.fail("editor did not exit within 30s of stdin closing")
        if raw.bad_lines:
            raw.fail(f"non-JSON-RPC stdout during shutdown: {raw.bad_lines[:3]!r}")


async def test_version_fallback():
    """An unknown client version gets the newest supported one."""
    async with RawEditor() as raw:
        init = await raw.initialize(version="1999-01-01")
        v = init.get("result", {}).get("protocolVersion")
        if v not in PROTOCOL_VERSIONS:
            raw.fail(f"unsupported client version should negotiate a supported one, got {v!r}: {init}")
