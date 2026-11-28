"""Core protocol tests (docs/mcp/DESIGN.md: transports, registry, calls, batch, legacy tools).

Tests requiring tools absent from the fake server skip through ed.require_tools.
"""

import asyncio
import base64
import contextlib
import json
import os
import re
import socket
import stat
import sys
import time
import uuid

import yaml
import tool_budget
from mcp.client.stdio import StdioServerParameters

from harness import (HTTP_TOKEN, Editor, ProtocolError, RawEditor, Scratch, TestFailure, _env_flag, find_key,
                     launch_spec, skip)

CI = True
EDITOR = {"create": True, "name": "MCP Protocol"}

PROTOCOL_VERSIONS = {"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"}

async def scene_entity_ids(ed: Editor) -> set[int]:
    result = await ed.call("find_entities", limit=100000)
    return {entity["id"] for entity in result["entities"]}


# -- lifecycle --------------------------------------------------------------


async def test_initialize(ed: Editor):
    r = ed.init_result
    ed.check(r is not None, "no initialize result")
    ed.check_in(r.protocol_version, PROTOCOL_VERSIONS, "negotiated protocol version")
    ed.check(r.server_info and r.server_info.name, "serverInfo.name is empty")
    ed.check(r.capabilities and r.capabilities.tools is not None, "server does not declare the tools capability")
    ed.check(ed.instructions.strip(), "initialize.instructions (the agent guide) is empty")
    # sent on every session: its limits are budget.json's "guide" entry (S31; detail goes in docs/mcp/guide)
    limit = tool_budget.load_budget()["guide"]["bytes"]
    size = len(ed.instructions.encode("utf-8"))
    ed.check(size <= limit, f"initialize instructions are {size} bytes, over {limit} (tools/mcp/budget.json)")


async def test_tools_list(ed: Editor):
    from jsonschema import Draft202012Validator, SchemaError

    tools = await ed.tools()
    ed.check(len(tools) > 0, "tools/list is empty")
    names = [t.name for t in tools]
    dupes = sorted({n for n in names if names.count(n) > 1})
    ed.check(not dupes, f"duplicate tool names: {dupes}")
    bad = []
    for t in tools:
        schema = t.input_schema or {}
        try:
            Draft202012Validator.check_schema(schema)
        except SchemaError as error:
            bad.append(f"{t.name}: invalid JSON Schema: {error.message}")
        if schema.get("type") != "object":
            bad.append(f"{t.name}: inputSchema.type is {schema.get('type')!r}, not 'object'")
    ed.check(not bad, "bad tool definitions:\n  " + "\n  ".join(bad))


# -- errors -------------------------------------------------------------------


async def test_unknown_tool(ed: Editor):
    name = "no_such_tool_" + uuid.uuid4().hex[:8]
    try:
        r = await ed.call_raw(name, {})
        ed.check(r.is_error and name in r.text, f"unknown tool was not identified: {r.text[:300]}")
    except ProtocolError as error:
        ed.check(name in str(error), f"unknown tool was rejected for the wrong reason: {error}")
    await ed.ping()  # still alive


async def test_bad_argument_is_tool_error(ed: Editor):
    """Agent mistakes are isError results that say what is wrong, not crashes or protocol errors."""
    ed.require_tools("create_entity")
    msg = await ed.call("create_entity", name="Bad", type="definitely_not_a_type", expect_error=True)
    ed.check("type" in msg.lower(), f"invalid entity type was rejected for the wrong reason: {msg}")
    await ed.ping()


# S99 R1-R5, AC1: each misspelled name gets the one nearest valid name, a name nothing is close to gets
# the valid ones or the call that lists them, and an internal fault gets advice and a fault id that
# editor.log holds with the detail. (what, tool, arguments, the suggestion or None, what the text
# must also name: the list or the call that lists it)
NAME_CASES = [
    ("misspelled tool", "create_entty", {}, "create_entity", "tool_groups"),
    ("misspelled argument", "create_entity", {"nam": "X"}, "name", "Arguments: changes, components, model, name"),
    ("nested key", "place_with_physics", {"entities": ["Crate"], "mode": "move", "target": {"dleta": [1, 0, 0]}}, "delta",
     "(allowed: delta, position"),
    ("enum value", "create_entity", {"type": "cylindr"}, "cylinder", "Types: empty, bare, cube"),
    ("docs area", "read_file", {"path": "jasper://docs/lightng"}, "lighting", "jasper://docs"),
    ("resource uri", "read_file", {"path": "jasper://guid"}, "jasper://guide", "jasper://docs"),
    ("camelCase key", "search_engine_api", {"query": "x", "maxDocResults": 2}, "max_doc_results",
     "Arguments: include_docs, max_doc_results"),
    ("missing underscore", "find_entities", {"withcomponents": True}, "with_components", "Arguments: active, component"),
    ("no close tool", "zzqx_wvut", {}, None, "tool_groups"),
    ("no close argument", "create_entity", {"zzqxwvut": 1}, None, "Arguments: changes, components, model, name"),
    ("no close nested key", "place_with_physics", {"entities": ["Crate"], "mode": "move", "target": {"zzqxwvut": 1}}, None,
     "(allowed: delta, position"),
    ("no close docs area", "read_file", {"path": "jasper://docs/zzqxwvut"}, None, "Documents:"),
    ("no close enum value", "create_entity", {"type": "zzqxwvut"}, None, "Types: empty, bare, cube"),
]

# (exception raised by selftest_echo's handler, what editor.log holds of it); runtime_error is the
# normal error path: its message is the agent's
FAULT_CASES = [("out_of_range", "logic_error: vector"), ("invalid_argument", "logic_error: selftest boom"),
               ("bad_alloc", "bad_alloc"), ("bad_function_call", "bad_function_call"),
               ("json_out_of_range", "json out_of_range"), ("unknown", "an exception that is not a std::exception")]


async def test_errors_suggest_names_and_advise_faults():
    import shutil
    import tempfile
    from pathlib import Path
    logdir = Path(tempfile.mkdtemp(prefix="je-fault-log-"))
    calls_log = logdir / "calls.jsonl"
    try:
        await errors_suggest_names_and_advise_faults(calls_log)
    finally:
        shutil.rmtree(logdir, ignore_errors=True)


async def errors_suggest_names_and_advise_faults(calls_log):
    # the call log (JASPER_MCP_LOG) runs too: a fault is logged as error_kind "internal", never its text
    env = {"JASPER_MCP_SELFTEST_GROUP": "1", "JASPER_MCP_LOG": str(calls_log)}
    async with Editor(create=True, name="MCP Errors", env=env) as ed:
        await ed.call("create_entity", name="Crate", type="cube")
        problems = []
        for what, tool, args, suggestion, lists in NAME_CASES:
            try:
                r = await ed.call_raw(tool, args)
                text = r.text if r.is_error else f"(not an error) {r.text}"
            except ProtocolError as error:   # tools/call of an unknown tool is a JSON-RPC error
                text = str(error)
            # inside batch an unknown tool is a step's error result, with the same text
            if tool == "create_entty" or tool == "zzqx_wvut":
                step = (await ed.call("batch", calls=[{"tool": tool, "arguments": args}]))["results"][0]
                if step["ok"] or text.split(": ", 2)[-1] not in step["text"]:
                    problems.append(f"{what} in batch: {step['text']!r} lacks {text!r}")
            suggested = re.findall(r"did you mean '([^']*)'", text)
            if suggested != ([suggestion] if suggestion else []):
                problems.append(f"{what}: suggests {suggested}, not {suggestion!r}: {text!r}")
            if lists not in text:
                problems.append(f"{what}: names no next action ({lists!r}): {text!r}")

        log = ed.scratch.root / "editor.log"
        for raised, detail in FAULT_CASES:
            r = await ed.call_raw("selftest_echo", {"throw": raised, "text": "boom"})
            fault = (r.structured or {}).get("fault", "")
            advice = (f"Internal error in selftest_echo (not caused by your arguments); details in editor.log as fault "
                      f"{fault}. Check the state with editor_status, retry once, and report it if it repeats.")
            if not (r.is_error and fault and r.text.startswith(advice)):
                problems.append(f"{raised}: not the fault advice: {r.text!r} {r.structured!r}")
                continue
            line = f"MCP fault {fault} in selftest_echo: {detail}"
            for _ in range(100):   # the log is written by its own thread
                if line in (log.read_text(errors="replace") if log.exists() else ""):
                    break
                await asyncio.sleep(0.1)
            else:
                text = log.read_text(errors="replace") if log.exists() else ""
                problems.append(f"{raised}: editor.log lacks {line!r}; its fault lines: "
                                f"{[entry for entry in text.splitlines() if 'MCP fault' in entry][-8:]}")
        r = await ed.call_raw("selftest_echo", {"throw": "runtime_error", "text": "boom"})
        if not (r.is_error and r.text.startswith("Tool selftest_echo failed: selftest boom")):
            problems.append(f"runtime_error: its message is not the error: {r.text!r}")

        # the faults and the runtime_error, as the call log records them (its writer has its own thread)
        expected = [("internal", raised) for raised, _ in FAULT_CASES] + [("error", "runtime_error")]
        deadline = time.monotonic() + 30
        while True:
            rows = [row for row in call_log_lines(calls_log) if row.get("tool") == "selftest_echo"]
            if len(rows) >= len(expected) or time.monotonic() > deadline:
                break
            await asyncio.sleep(0.05)
        kinds = [(row.get("ok"), row.get("error_kind")) for row in rows]
        if kinds != [(False, kind) for kind, _ in expected]:
            problems.append(f"call log error kinds {kinds}, expected {[(False, kind) for kind, _ in expected]}")
        logged = calls_log.read_bytes() if calls_log.exists() else b""
        if b"boom" in logged or b"fault" in logged:
            problems.append("a fault's text or id reached the call log")
        ed.check(not problems, "\n".join(problems))
        await ed.ping()


# -- edits through commands -------------------------------------------------------


async def test_batch(ed: Editor):
    ed.require_tools("batch", "create_entity", "find_entities", "undo")
    names = ["McpBatchA_" + uuid.uuid4().hex[:6], "McpBatchB_" + uuid.uuid4().hex[:6]]
    out = await ed.batch([("create_entity", {"name": n, "type": "box"}) for n in names], undo_group=True)
    results = out["results"]
    ed.check(isinstance(results, list) and len(results) == 2, f"batch should return 2 results: {out!r}")
    ids = [find_key(r, "entity_id", "id") for r in results]
    ed.check(all(i is not None for i in ids), f"batch results lack ids: {out!r}")
    listed = await scene_entity_ids(ed)
    ed.check(all(i in listed for i in ids), f"batch-created {ids} not all listed")

    # undo_group: both creations are one undo step
    await ed.call("undo")
    listed = await scene_entity_ids(ed)
    ed.check(not any(i in listed for i in ids), f"one undo should remove the whole batch; still listed: "
                                                f"{[i for i in ids if i in listed]}")


async def test_batch_stop_on_error(ed: Editor):
    ed.require_tools("batch", "create_entity", "find_entities")
    name = "NeverMade_" + uuid.uuid4().hex
    out = await ed.call("batch", calls=[
        {"tool": "no_such_tool_xyz", "arguments": {}},
        {"tool": "create_entity", "arguments": {"name": name, "type": "box"}},
    ], stop_on_error=True)
    results = out["results"]
    ed.check(isinstance(results, list) and len(results) == 1,
             f"stop_on_error should stop after the failing call: {out!r}")
    found = await ed.call("find_entities", name=name)
    ed.check(not any(entity["name"] == name for entity in found["entities"]),
             "stop_on_error executed a later creation")


# -- stdio transport, raw ------------------------------------------------------


async def test_stdio_raw():
    """Every stdout line is a JSON-RPC 2.0 message (the editor's own prints go to stderr), the
    earliest supported legacy version is echoed, protocol errors are typed, ping works after
    malformed input, and closing stdin shuts the editor down cleanly."""
    async with RawEditor(create=True, name="MCP Protocol") as raw:
        init = await raw.initialize(version="2024-11-05")
        if "error" in init:
            raw.fail(f"initialize failed: {init['error']}")
        if init["result"].get("protocolVersion") != "2024-11-05":
            raw.fail(f"protocol version not echoed: {init['result'].get('protocolVersion')}")
        raw.send({"jsonrpc": "2.0", "id": 2, "method": "tools/list"})
        tools = (await raw.response(2))["result"]["tools"]
        raw.send({"jsonrpc": "2.0", "id": 3, "method": "ping"})
        pong = await raw.response(3)
        if pong.get("result") != {}:
            raw.fail(f"ping must return an empty result object: {pong}")
        if any(t["name"] == "editor_status" for t in tools):
            raw.send({"jsonrpc": "2.0", "id": 4, "method": "tools/call",
                      "params": {"name": "editor_status", "arguments": {}}})
            called = await raw.response(4)
            if "result" not in called or called["result"].get("isError"):
                raw.fail(f"editor_status over legacy stdio failed: {called}")
        if any(t["name"] == "create_entity" for t in tools):
            raw.send({"jsonrpc": "2.0", "id": 5, "method": "tools/call",
                      "params": {"name": "create_entity", "arguments": {"name": "RawBox", "type": "box"}}})
            await raw.response(5)
        raw.send({"jsonrpc": "2.0", "id": 6, "method": "no/such/method"})
        err = await raw.response(6)
        if err.get("error", {}).get("code") != -32601:
            raw.fail(f"unknown legacy method must return -32601: {err}")
        if not _env_flag("JASPER_FAKE"):
            # The SDK-backed fake does not emit the editor's parse-error response.
            raw.send("this is not json")
            malformed = await raw.response(None)
            if malformed.get("error", {}).get("code") != -32700:
                raw.fail(f"malformed JSON must return -32700: {malformed}")
        raw.send({"jsonrpc": "2.0", "id": 7, "method": "ping"})
        pong = await raw.response(7)
        if pong.get("result") != {}:
            raw.fail(f"ping after a protocol error must still work: {pong}")
        if raw.bad_lines:
            raw.fail(f"{len(raw.bad_lines)} non-JSON-RPC stdout line(s), first: {raw.bad_lines[:3]!r}")
        code = await raw.close_stdin_and_wait(30)
        if code != 0:
            raw.fail(f"editor must exit cleanly after stdin closes, got {code!r}")
        if raw.bad_lines:
            raw.fail(f"non-JSON-RPC stdout during shutdown: {raw.bad_lines[:3]!r}")


async def test_version_fallback():
    """An unknown client version gets the newest supported one."""
    async with RawEditor(create=True, name="MCP Protocol") as raw:
        init = await raw.initialize(version="1999-01-01")
        v = init.get("result", {}).get("protocolVersion")
        if v not in PROTOCOL_VERSIONS:
            raw.fail(f"unsupported client version should negotiate a supported one, got {v!r}: {init}")


# -- 2026-07-28 (stateless) next to the initialize-era versions --------------------------------

MODERN = "2026-07-28"
META = {"io.modelcontextprotocol/protocolVersion": MODERN,
        "io.modelcontextprotocol/clientInfo": {"name": "jasper-protocol-test", "version": "1"},
        "io.modelcontextprotocol/clientCapabilities": {}}


def modern_params(**params) -> dict:
    return {**params, "_meta": dict(META)}


def check_cache_hints(raw, result: dict, what: str) -> None:
    if result.get("resultType") != "complete":
        raw.fail(f"{what}: resultType should be \"complete\": {result}")
    if not isinstance(result.get("ttlMs"), int) or result["ttlMs"] < 0 or result.get("cacheScope") not in ("public", "private"):
        raw.fail(f"{what}: a cacheable result needs ttlMs >= 0 and cacheScope: {result}")


async def test_stdio_modern_without_initialize():
    """A 2026-07-28 client never initializes: server/discover and every request carry _meta;
    the same process still serves an initialize-era client afterwards (dual-era)."""
    async with RawEditor(create=True, name="Protocol CI") as raw:
        raw.send({"jsonrpc": "2.0", "id": 1, "method": "server/discover", "params": modern_params()})
        discover = (await raw.response(1)).get("result")
        if not discover or MODERN not in discover.get("supportedVersions", []):
            raw.fail(f"server/discover should list {MODERN}: {discover}")
        if "tools" not in discover.get("capabilities", {}):
            raw.fail(f"server/discover has no tools capability: {discover}")
        check_cache_hints(raw, discover, "server/discover")

        raw.send({"jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": modern_params()})
        listed = (await raw.response(2)).get("result", {})
        check_cache_hints(raw, listed, "tools/list")
        names = [t["name"] for t in listed.get("tools", [])]
        if "editor_status" in names:
            raw.send({"jsonrpc": "2.0", "id": 3, "method": "tools/call",
                      "params": modern_params(name="editor_status", arguments={})})
            called = (await raw.response(3)).get("result", {})
            if called.get("resultType") != "complete" or called.get("isError"):
                raw.fail(f"tools/call over 2026-07-28: {called}")

        # a version the server does not speak: UnsupportedProtocolVersionError naming the supported ones
        raw.send({"jsonrpc": "2.0", "id": 4, "method": "tools/list",
                  "params": {"_meta": {**META, "io.modelcontextprotocol/protocolVersion": "2099-01-01"}}})
        error = (await raw.response(4)).get("error", {})
        if error.get("code") != -32022 or MODERN not in (error.get("data") or {}).get("supported", []):
            raw.fail(f"an unknown version should be -32022 with data.supported: {error}")
        raw.send({"jsonrpc": "2.0", "id": 5, "method": "no/such/method", "params": modern_params()})
        if (await raw.response(5)).get("error", {}).get("code") != -32601:
            raw.fail("an unknown 2026-07-28 method should be -32601")

        if raw.bad_lines:
            raw.fail(f"non-JSON-RPC stdout: {raw.bad_lines[:3]!r}")
        if _env_flag("JASPER_FAKE"):
            return  # the SDK's server keeps a stdio connection in the era it started with
        # then an initialize-era client on the same stdio (a dual-era server MAY serve both)
        init = await raw.initialize(version="2025-11-25", id=6)
        if init.get("result", {}).get("protocolVersion") != "2025-11-25":
            raw.fail(f"initialize after 2026-07-28 requests: {init}")
        raw.send({"jsonrpc": "2.0", "id": 7, "method": "tools/list"})
        legacy = (await raw.response(7)).get("result", {})
        if "resultType" in legacy:
            raw.fail("an initialize-era tools/list should not carry resultType")
        if raw.bad_lines:
            raw.fail(f"non-JSON-RPC stdout: {raw.bad_lines[:3]!r}")


async def test_sdk_client_negotiates_modern_over_stdio():
    """The MCP SDK's own client in its default mode (server/discover, falling back to
    initialize) speaks 2026-07-28 with the editor and runs tools that way."""
    from mcp.client.client import Client
    scratch = Scratch.make(create=True)
    ok = False
    try:
        argv, env, cwd = launch_spec(scratch, create=True)
        params = StdioServerParameters(command=argv[0], args=argv[1:], env=env, cwd=str(cwd))
        async with Client(params, mode="auto", read_timeout_seconds=180) as client:
            if client.protocol_version != MODERN:
                raise TestFailure(f"the SDK client negotiated {client.protocol_version!r}, not {MODERN}")
            tools = await client.list_tools()
            names = [t.name for t in tools.tools]
            if "editor_status" not in names:
                raise TestFailure(f"tools/list over 2026-07-28 lacks editor_status: {names[:10]}")
            r = await client.call_tool("editor_status", {})
            if r.is_error:
                raise TestFailure(f"editor_status over 2026-07-28: {r}")
        ok = True
    finally:
        if ok:
            scratch.remove()
        else:
            print(f"[harness] scratch kept: {scratch.root}", file=sys.stderr)


# -- Streamable HTTP: bearer token, both eras, read-only mode ----------------------------------


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


async def http_post(port: int, body, headers: dict | None = None, token: str | None = HTTP_TOKEN,
                    timeout: float = 120.0, before_close=None, method: str = "POST"):
    """One POST on its own connection: (status, lower-case headers, the JSON-RPC message or None).
    before_close: await a condition, then hang up without reading (returns None)."""
    reader, writer = await asyncio.open_connection("127.0.0.1", port)
    data = body if isinstance(body, bytes) else json.dumps(body).encode()
    lines = [f"{method} /mcp HTTP/1.1", f"Host: 127.0.0.1:{port}", "Content-Type: application/json",
             "Accept: application/json, text/event-stream", f"Content-Length: {len(data)}", "Connection: close"]
    if token:
        lines.append(f"Authorization: Bearer {token}")
    lines += [f"{k}: {v}" for k, v in (headers or {}).items()]
    writer.write(("\r\n".join(lines) + "\r\n\r\n").encode() + data)
    await writer.drain()
    if before_close is not None:
        try:
            await before_close()
        finally:
            writer.close()
            await writer.wait_closed()
        return None
    raw = await asyncio.wait_for(reader.read(), timeout)
    writer.close()
    head, _, payload = raw.partition(b"\r\n\r\n")
    head_lines = head.decode("latin-1").split("\r\n")
    status = int(head_lines[0].split()[1])
    got = {}
    for line in head_lines[1:]:
        k, _, v = line.partition(":")
        got[k.strip().lower()] = v.strip()
    message = None
    if got.get("content-type", "").startswith("text/event-stream"):
        events = [json.loads(line[5:]) for line in payload.decode().splitlines() if line.startswith("data:")]
        message = events[-1] if events else None
    elif payload:
        with contextlib.suppress(ValueError):
            message = json.loads(payload)
    return status, got, message


def modern_headers(method: str, name: str | None = None, version: str = MODERN) -> dict:
    headers = {"MCP-Protocol-Version": version, "Mcp-Method": method}
    if name is not None:
        headers["Mcp-Name"] = name
    return headers


def b64_header(value: str) -> str:
    return "=?base64?" + base64.b64encode(value.encode()).decode() + "?="


async def wait_for_port(raw, port: int, timeout: float = 180.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        with contextlib.suppress(OSError):
            _, w = await asyncio.open_connection("127.0.0.1", port)
            w.close()
            return
        await asyncio.sleep(0.25)
    raw.fail(f"the editor did not listen on HTTP port {port}")


async def test_http_token_and_protocol_eras():
    """HTTP needs the bearer token; an initialize-era client gets its Mcp-Session-Id as before;
    a 2026-07-28 client needs no session but must mirror MCP-Protocol-Version / Mcp-Method /
    Mcp-Name (base64 when not ASCII) in headers; closing the connection cancels its call."""
    if _env_flag("JASPER_FAKE"):
        skip("the fake server's HTTP transport has no token or header checks")
    port = free_port()
    os.environ["JASPER_MCP_TRACE"] = "1"   # the cancel check reads the trace on stderr
    try:
        raw_cm = RawEditor(create=True, name="Protocol CI", extra_args=["--http-port", str(port)])
        raw = await raw_cm.__aenter__()
    finally:
        os.environ.pop("JASPER_MCP_TRACE", None)
    try:
        await raw.initialize()
        await wait_for_port(raw, port)
        ping = {"jsonrpc": "2.0", "id": 1, "method": "ping"}

        # the harness put the token in settings.yaml: it moved to mcp_http_token, owner-only
        token_file = raw.scratch.root / "mcp_http_token"
        if not token_file.is_file() or token_file.read_text().strip() != HTTP_TOKEN:
            raw.fail(f"the token should be in {token_file}")
        if stat.S_IMODE(token_file.stat().st_mode) != 0o600:
            raw.fail(f"mcp_http_token should be mode 0600, is {oct(stat.S_IMODE(token_file.stat().st_mode))}")
        if HTTP_TOKEN in (raw.scratch.root / "settings.yaml").read_text():
            raw.fail("settings.yaml should no longer hold the token")

        # the token
        for token, what in ((None, "no token"), ("wrong-" + HTTP_TOKEN, "a wrong token")):
            status, headers, _ = await http_post(port, ping, token=token)
            if status != 401 or headers.get("www-authenticate", "").lower() != "bearer":
                raw.fail(f"{what}: expected 401 with WWW-Authenticate: Bearer, got {status} {headers}")
        status, _, _ = await http_post(port, ping, headers={"Authorization": "Basic " + HTTP_TOKEN}, token=None)
        if status != 401:
            raw.fail(f"a Basic credential is not the bearer token: {status}")

        status, _, _ = await http_post(port, ping, headers={"Origin": "http://evil.example"})
        if status != 403:
            raw.fail(f"a foreign Origin must be refused to prevent DNS rebinding: {status}")
        status, _, _ = await http_post(port, {}, method="GET")
        if status != 405:
            raw.fail(f"GET /mcp has no server-initiated stream and must return 405: {status}")

        # initialize era: a session, required afterwards; a bad MCP-Protocol-Version header is refused
        status, _, _ = await http_post(port, ping)
        if status != 400:
            raw.fail(f"an initialize-era request without Mcp-Session-Id should be 400, got {status}")
        status, headers, init = await http_post(port, {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
            "protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "raw-http", "version": "1"}}})
        sid = headers.get("mcp-session-id")
        if status != 200 or not sid or init["result"]["protocolVersion"] != "2025-06-18":
            raw.fail(f"initialize over HTTP: {status} {headers} {init}")
        legacy = {"Mcp-Session-Id": sid, "MCP-Protocol-Version": "2025-06-18"}
        status, _, listed = await http_post(port, {"jsonrpc": "2.0", "id": 2, "method": "tools/list"}, headers=legacy)
        if status != 200 or "resultType" in listed["result"] or not listed["result"]["tools"]:
            raw.fail(f"initialize-era tools/list: {status} {str(listed)[:300]}")
        status, _, _ = await http_post(port, {"jsonrpc": "2.0", "id": 3, "method": "ping"},
                                       headers={"Mcp-Session-Id": sid, "MCP-Protocol-Version": "1999-01-01"})
        if status != 400:
            raw.fail(f"an unsupported MCP-Protocol-Version header should be 400, got {status}")

        status, _, _ = await http_post(port, {"jsonrpc": "2.0", "method": "notifications/initialized"},
                                      headers=legacy)
        if status != 202:
            raw.fail(f"a notification must return 202: {status}")
        status, headers, streamed = await http_post(port, {
            "jsonrpc": "2.0", "id": "streamed", "method": "tools/call", "params": {
                "name": "editor_status", "arguments": {}, "_meta": {"progressToken": "p1"}}}, headers=legacy)
        # S31: every protocol waits for the first message; a call answered before any progress
        # notification goes as plain JSON, one that reports progress first opens the stream
        content_type = headers.get("content-type", "")
        if (status != 200 or not (content_type.startswith("text/event-stream") or content_type.startswith("application/json"))
                or not streamed or streamed.get("id") != "streamed" or "result" not in streamed):
            raw.fail(f"a progressToken with Accept SSE must get the correlated result: {status} {headers} {streamed}")
        status, _, _ = await http_post(port, {}, headers=legacy, method="DELETE")
        if status != 200:
            raw.fail(f"DELETE must end a legacy session: {status}")
        status, _, _ = await http_post(port, ping, headers=legacy)
        if status != 404:
            raw.fail(f"an ended session must return 404: {status}")

        # 2026-07-28: no session, headers mirror the body
        discover = {"jsonrpc": "2.0", "id": "d1", "method": "server/discover", "params": modern_params()}
        status, headers, answer = await http_post(port, discover, headers=modern_headers("server/discover"))
        if status != 200 or "mcp-session-id" in headers:
            raw.fail(f"server/discover: {status} {headers} (no session id is minted in 2026-07-28)")
        result = answer["result"]
        if result["supportedVersions"][0] != MODERN or not set(result["supportedVersions"]) & PROTOCOL_VERSIONS:
            raw.fail(f"supportedVersions: {result['supportedVersions']}")
        if (result.get("resultType"), result.get("ttlMs"), result.get("cacheScope")) != ("complete", 0, "private"):
            raw.fail(f"server/discover hints: {result}")
        if result.get("_meta", {}).get("io.modelcontextprotocol/serverInfo", {}).get("name") != "jasperengine-editor":
            raw.fail(f"server/discover _meta serverInfo: {result.get('_meta')}")

        call = {"jsonrpc": "2.0", "id": 5, "method": "tools/call", "params": modern_params(name="editor_status", arguments={})}
        status, _, answer = await http_post(port, call, headers=modern_headers("tools/call", "editor_status"))
        if status != 200 or answer["result"].get("resultType") != "complete" or answer["result"].get("isError"):
            raw.fail(f"tools/call over 2026-07-28: {status} {str(answer)[:300]}")
        status, _, answer = await http_post(port, call, headers=modern_headers("tools/call", b64_header("editor_status")))
        if status != 200 or "result" not in answer:
            raw.fail(f"a base64 Mcp-Name is decoded before the check: {status} {answer}")

        mismatches = {
            "no MCP-Protocol-Version": {"Mcp-Method": "tools/call", "Mcp-Name": "editor_status"},
            "another MCP-Protocol-Version": modern_headers("tools/call", "editor_status", version="2025-11-25"),
            "no Mcp-Method": {"MCP-Protocol-Version": MODERN, "Mcp-Name": "editor_status"},
            "another Mcp-Method": modern_headers("tools/list", "editor_status"),
            "no Mcp-Name": modern_headers("tools/call"),
            "another Mcp-Name": modern_headers("tools/call", "create_entity"),
        }
        for what, headers in mismatches.items():
            status, _, answer = await http_post(port, call, headers=headers)
            code = (answer or {}).get("error", {}).get("code")
            if status != 400 or code != -32020:
                raw.fail(f"{what}: expected 400 HeaderMismatch (-32020), got {status} {answer}")
        future = {"jsonrpc": "2.0", "id": 6, "method": "tools/list",
                  "params": {"_meta": {**META, "io.modelcontextprotocol/protocolVersion": "2099-01-01"}}}
        status, _, answer = await http_post(port, future, headers=modern_headers("tools/list", version="2099-01-01"))
        if status != 400 or answer["error"]["code"] != -32022 or MODERN not in answer["error"]["data"]["supported"]:
            raw.fail(f"an unsupported version: expected 400 -32022 with data.supported, got {status} {answer}")
        status, _, answer = await http_post(port, {"jsonrpc": "2.0", "id": 7, "method": "no/such/method", "params": modern_params()},
                                            headers=modern_headers("no/such/method"))
        if status != 404 or answer["error"]["code"] != -32601:
            raw.fail(f"an unknown 2026-07-28 method: expected 404 -32601, got {status} {answer}")
        status, _, answer = await http_post(port, {"jsonrpc": "2.0", "id": 8, "method": "initialize", "params": modern_params()},
                                            headers=modern_headers("initialize"))
        if status != 404:
            raw.fail(f"2026-07-28 has no initialize: expected 404, got {status} {answer}")

        # Disconnect only after the real request is pending; release of that public
        # waiting state proves cancellation without depending on trace-log wording.
        probe_id = 100

        async def pending_calls():
            nonlocal probe_id
            probe_id += 1
            raw.send({"jsonrpc": "2.0", "id": probe_id, "method": "tools/call",
                      "params": {"name": "editor_status", "arguments": {}}})
            response = await raw.response(probe_id)
            return response["result"]["structuredContent"]["waiting_calls"]

        async def wait_for_pending():
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline:
                if await pending_calls() > 0:
                    return
                await asyncio.sleep(0.01)
            raw.fail("the HTTP wait never entered the pending state")

        waiting = {"jsonrpc": "2.0", "id": "cancel-me", "method": "tools/call",
                   "params": modern_params(name="wait", arguments={"seconds": 60})}
        await http_post(port, waiting, headers=modern_headers("tools/call", "wait"),
                        before_close=wait_for_pending)
        deadline = time.monotonic() + 30
        while await pending_calls() != 0:
            if time.monotonic() >= deadline:
                raw.fail("closing the HTTP connection did not cancel its pending call")
            await asyncio.sleep(0.01)
        status, _, _ = await http_post(port, call, headers=modern_headers("tools/call", "editor_status"))
        if status != 200:
            raw.fail(f"the server after a cancelled call: {status}")

        # the SDK's client over HTTP with the token: discovers 2026-07-28 and calls a tool
        from mcp import ClientSession
        from mcp.client.client import Client
        from mcp.client.streamable_http import create_mcp_http_client, streamable_http_client
        async with create_mcp_http_client(headers={"Authorization": f"Bearer {HTTP_TOKEN}"}) as http:
            async with Client(streamable_http_client(f"http://127.0.0.1:{port}/mcp", http_client=http),
                              mode="auto", read_timeout_seconds=120) as client:
                if client.protocol_version != MODERN:
                    raw.fail(f"the SDK client negotiated {client.protocol_version!r} over HTTP")
                r = await client.call_tool("editor_status", {})
                if r.is_error:
                    raw.fail(f"SDK client editor_status over HTTP: {r}")
        # The initialize-era SDK must also decode the SSE tool response it requests
        # when a caller supplies a progress callback.
        async with create_mcp_http_client(headers={"Authorization": f"Bearer {HTTP_TOKEN}"}) as http:
            async with streamable_http_client(f"http://127.0.0.1:{port}/mcp", http_client=http) as streams:
                async with ClientSession(streams[0], streams[1]) as session:
                    await session.initialize()
                    async def on_progress(*_):
                        pass

                    r = await session.call_tool("editor_status", {}, progress_callback=on_progress)
                    if r.is_error:
                        raw.fail(f"initialize-era SDK failed to decode its SSE tool response: {r}")
    except BaseException:
        raw.failed = True
        raise
    finally:
        await raw_cm.__aexit__(None, None, None)


async def test_http_read_only_mode():
    """With "Allow changes" off (settings.yaml mcp.allow_changes: false) HTTP clients of both eras
    only see and run read-only tools, batch included, and not with arguments that write files
    (screenshot save_path); the stdio client keeps every tool."""
    if _env_flag("JASPER_FAKE"):
        skip("the fake server has no read-only mode")
    import harness
    original = harness.Scratch.make

    def make(*args, **kwargs):
        scratch = original(*args, **kwargs)
        path = scratch.root / "settings.yaml"
        settings = yaml.safe_load(path.read_text())
        settings.setdefault("mcp", {})["allow_changes"] = False
        settings["mcp"].pop("http_token", None)
        path.write_text(yaml.safe_dump(settings, sort_keys=False))
        token_file = scratch.root / "mcp_http_token"
        token_file.write_text(HTTP_TOKEN + "\n")
        token_file.chmod(0o644)
        return scratch

    port = free_port()
    harness.Scratch.make = make
    try:
        raw_cm = RawEditor(create=True, name="Protocol CI", extra_args=["--http-port", str(port)])
        raw = await raw_cm.__aenter__()
    finally:
        harness.Scratch.make = original
    try:
        await raw.initialize()
        await wait_for_port(raw, port)
        raw.send({"jsonrpc": "2.0", "id": 2, "method": "tools/list"})
        stdio_tools = {t["name"] for t in (await raw.response(2))["result"]["tools"]}
        if "create_entity" not in stdio_tools or "batch" not in stdio_tools:
            raw.fail("the stdio client should keep every tool in read-only mode")

        status, _, listed = await http_post(port, {"jsonrpc": "2.0", "id": 1, "method": "tools/list", "params": modern_params()},
                                            headers=modern_headers("tools/list"))
        tools = listed["result"]["tools"]
        writers = [t["name"] for t in tools if not t.get("annotations", {}).get("readOnlyHint")]
        names = {t["name"] for t in tools}
        if status != 200 or writers or "editor_status" not in names or "batch" in names:
            raw.fail(f"HTTP tools/list in read-only mode: {status}, tools that change things: {writers}")

        status, headers, init = await http_post(port, {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
            "protocolVersion": "2025-11-25", "capabilities": {}, "clientInfo": {"name": "raw-http", "version": "1"}}})
        session = {"Mcp-Session-Id": headers["mcp-session-id"]}
        if "READ-ONLY" not in init["result"]["instructions"]:
            raw.fail("the instructions of a read-only HTTP session should say so")
        _, _, legacy = await http_post(port, {"jsonrpc": "2.0", "id": 2, "method": "tools/list"}, headers=session)
        if {t["name"] for t in legacy["result"]["tools"]} != names:
            raw.fail("initialize-era and 2026-07-28 HTTP clients should see the same read-only tools")

        def call(id_, name, arguments):
            return {"jsonrpc": "2.0", "id": id_, "method": "tools/call", "params": {"name": name, "arguments": arguments}}

        _, _, made = await http_post(port, call(3, "create_entity", {"name": "Nope", "type": "box"}), headers=session)
        result = made.get("result", {})
        if not result.get("isError") or "Allow changes" not in json.dumps(result):
            raw.fail(f"create_entity over HTTP in read-only mode should be refused: {made}")
        _, _, batch = await http_post(port, call(4, "batch", {"calls": [
            {"tool": "editor_status", "arguments": {}},
            {"tool": "create_entity", "arguments": {"name": "Smuggled", "type": "box"}}]}), headers=session)
        if not batch.get("result", {}).get("isError"):
            raw.fail(f"batch over HTTP in read-only mode should be refused: {batch}")
        _, _, status_call = await http_post(port, call(5, "editor_status", {}), headers=session)
        if status_call.get("result", {}).get("isError"):
            raw.fail(f"a read-only tool over HTTP in read-only mode: {status_call}")
        # nothing was made: the stdio client finds neither GameObject
        raw.send({"jsonrpc": "2.0", "id": 6, "method": "tools/call",
                  "params": {"name": "find_entities", "arguments": {"limit": 100000}}})
        found = json.dumps((await raw.response(6))["result"])
        if '"Nope"' in found or '"Smuggled"' in found:
            raw.fail(f"a write slipped through: {found[:300]}")
        # an existing token file others could read is owner-only now (and its token is used)
        mode = stat.S_IMODE((raw.scratch.root / "mcp_http_token").stat().st_mode)
        if mode != 0o600:
            raw.fail(f"a 0644 mcp_http_token should become 0600, is {oct(mode)}")

        # a read-only tool whose arguments write a file is refused too, and writes nothing
        project = raw.scratch.project
        before = (project / "project.yaml").read_bytes()
        for save_path in ("assets/read_only_shot.png", "project.yaml"):
            _, _, shot = await http_post(port, call(8, "screenshot", {"view": "game", "save_path": save_path}),
                                         headers=session, timeout=300)
            result = shot.get("result", {})
            if not result.get("isError") or "Allow changes" not in json.dumps(result):
                raw.fail(f"screenshot save_path={save_path!r} over HTTP in read-only mode should be refused: {str(shot)[:300]}")
        if (project / "assets/read_only_shot.png").exists() or (project / "project.yaml").read_bytes() != before:
            raw.fail("a refused screenshot save_path wrote its file")
        # without save_path it is a read and runs
        _, _, shot = await http_post(port, call(9, "screenshot", {"view": "game", "max_width": 64}), headers=session, timeout=300)
        if shot.get("result", {}).get("isError"):
            raw.fail(f"screenshot without save_path in read-only mode: {str(shot)[:300]}")
        # save_path stays inside the project, also for clients that may change things
        outside = raw.scratch.root / "outside_shot.png"
        raw.send({"jsonrpc": "2.0", "id": 10, "method": "tools/call", "params": {"name": "screenshot", "arguments": {
            "view": "game", "max_width": 64, "save_path": str(outside)}}})
        r = (await raw.response(10, 300))["result"]
        if not r.get("isError") or outside.exists():
            raw.fail(f"screenshot save_path outside the project should be refused: {str(r)[:300]}")

        # the stdio client may still change things
        raw.send({"jsonrpc": "2.0", "id": 7, "method": "tools/call",
                  "params": {"name": "create_entity", "arguments": {"name": "ViaStdio", "type": "box"}}})
        if (await raw.response(7))["result"].get("isError"):
            raw.fail("the stdio client should keep its tools in read-only mode")
    except BaseException:
        raw.failed = True
        raise
    finally:
        await raw_cm.__aexit__(None, None, None)


# -- call log (S99 R17, R18, AC5) ---------------------------------------------


CALL_LOG_FIELDS = ["ts", "tool", "group", "ms", "ok", "error_kind", "result_bytes", "client"]
CALL_LOG_CAP = 2048


def call_log_lines(path) -> list[dict]:
    """The calls in <path>.1 then <path>: each a complete JSON line."""
    rows = []
    for f in (path.with_name(path.name + ".1"), path):
        if f.exists():
            rows += [json.loads(line) for line in f.read_text().splitlines()]
    return rows


async def call_log_wait(raw: RawEditor, path, count: int) -> list[dict]:
    """The log once it holds `count` calls (the writer flushes on its own thread)."""
    deadline = time.monotonic() + 30
    while True:
        rows = call_log_lines(path)
        if len(rows) >= count or time.monotonic() > deadline:
            return rows
        await asyncio.sleep(0.05)


async def test_call_log():
    """JASPER_MCP_LOG: one line per tools/call with exactly the fields, in order; no argument, path,
    entity name or result text ever reaches it (a secret-looking value in an argument, a key and a
    name is absent); the client is the initialize handshake's name; past the cap
    (JASPER_TEST_MCP_LOG_MAX_BYTES) the file moves to <file>.1 whole lines at a time. Then Log Calls
    To File (settings.yaml mcp.log_calls) writes mcp-calls.jsonl beside settings.yaml."""
    import datetime
    import re
    import shutil
    import tempfile
    from pathlib import Path

    secret = "sk-SECRET-" + uuid.uuid4().hex
    logdir = Path(tempfile.mkdtemp(prefix="je-call-log-"))
    log = logdir / "calls.jsonl"
    env = {"JASPER_MCP_LOG": str(log), "JASPER_TEST_MCP_LOG_MAX_BYTES": str(CALL_LOG_CAP)}
    try:
        async with RawEditor(create=True, name="MCP Protocol", env=env) as raw:
            init = await raw.initialize()
            if "error" in init:
                raw.fail(f"initialize failed: {init['error']}")
            started = datetime.datetime.now(datetime.timezone.utc) - datetime.timedelta(seconds=1)
            calls = [("editor_status", {}), ("create_entity", {"name": secret, "type": "box"}),
                     ("find_entities", {"name": secret}), ("create_entity", {"name": "Box", "bogus_" + secret: 1})]
            results = []
            for i, (tool, args) in enumerate(calls, start=10):
                raw.send({"jsonrpc": "2.0", "id": i, "method": "tools/call", "params": {"name": tool, "arguments": args}})
                results.append((await raw.response(i))["result"])
            if results[1].get("isError") or not results[3].get("isError"):
                raw.fail(f"create_entity should succeed, the unknown argument fail: {results[1]!r:.300} {results[3]!r:.300}")
            rows = await call_log_wait(raw, log, len(calls))
            ended = datetime.datetime.now(datetime.timezone.utc) + datetime.timedelta(seconds=1)
            if len(rows) != len(calls):
                raw.fail(f"{len(rows)} log lines for {len(calls)} calls: {rows}")
            for row, (tool, _), result in zip(rows, calls, results):
                if list(row) != CALL_LOG_FIELDS:
                    raw.fail(f"log line fields {list(row)}, expected exactly {CALL_LOG_FIELDS}")
                if row["tool"] != tool or row["group"] != "core" or row["client"] != "jasper-harness-raw":
                    raw.fail(f"tool, group or client wrong in {row}")
                ts = datetime.datetime.fromisoformat(row["ts"].replace("Z", "+00:00"))
                if not started <= ts <= ended or not re.fullmatch(r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z", row["ts"]):
                    raw.fail(f"ts {row['ts']} is not the call's UTC time")
                if not isinstance(row["ms"], (int, float)) or not 0 <= row["ms"] <= (ended - started).total_seconds() * 1000:
                    raw.fail(f"ms {row['ms']!r} out of range")
                sent = len(json.dumps(result, separators=(",", ":"), ensure_ascii=False).encode())
                if not isinstance(row["result_bytes"], int) or abs(row["result_bytes"] - sent) > max(16, sent // 50):
                    raw.fail(f"result_bytes {row['result_bytes']!r}, the result sent was {sent} bytes")
            if [r["ok"] for r in rows] != [True, True, True, False] or [r["error_kind"] for r in rows] != [None, None, None, "error"]:
                raw.fail(f"ok / error_kind wrong: {[(r['ok'], r['error_kind']) for r in rows]}")

            # past the cap: <file>.1 holds the older whole lines, the file the newer ones
            rotated = log.with_name(log.name + ".1")
            next_id = 100
            while not rotated.exists() and next_id < 200:
                raw.send({"jsonrpc": "2.0", "id": next_id, "method": "tools/call", "params": {"name": "editor_status", "arguments": {}}})
                await raw.response(next_id)
                next_id += 1
                await call_log_wait(raw, log, len(calls) + next_id - 100)
            if not rotated.exists():
                raw.fail(f"no {rotated.name} after {next_id - 100} more calls with a {CALL_LOG_CAP}-byte cap")
            for f in (rotated, log):
                if f.stat().st_size > CALL_LOG_CAP:
                    raw.fail(f"{f.name} is {f.stat().st_size} bytes, over the {CALL_LOG_CAP}-byte cap")
            rows = call_log_lines(log)
            if any(list(r) != CALL_LOG_FIELDS for r in rows) or rows[-1]["tool"] != "editor_status":
                raw.fail(f"rotated lines must stay whole calls: {rows}")
            if [r["ts"] for r in rows] != sorted(r["ts"] for r in rows):
                raw.fail(f"{rotated.name} must hold the older calls")
            text = rotated.read_bytes() + log.read_bytes()
            if secret.encode() in text or b"bogus_" in text:
                raw.fail("an argument reached the call log")
        if secret.encode() in log.read_bytes():
            raise TestFailure("an argument reached the call log at exit")

        # the MCP Server window's Log Calls To File, saved per user
        async with RawEditor(create=True, name="MCP Protocol",
                             settings={"mcp": {"http_token": HTTP_TOKEN, "log_calls": True}}) as raw:
            await raw.initialize()
            raw.send({"jsonrpc": "2.0", "id": 2, "method": "tools/call", "params": {"name": "editor_status", "arguments": {}}})
            await raw.response(2)
            default_log = raw.scratch.root / "mcp-calls.jsonl"
            rows = await call_log_wait(raw, default_log, 1)
            if [r.get("tool") for r in rows] != ["editor_status"]:
                raw.fail(f"Log Calls To File should write {default_log.name} beside settings.yaml: {rows}")
    finally:
        shutil.rmtree(logdir, ignore_errors=True)


async def test_call_log_is_off_by_default(ed: Editor):
    """No JASPER_MCP_LOG and no Log Calls To File: nothing is written (an open log creates its file at start)."""
    await ed.call("editor_status")
    ed.check(not (ed.scratch.root / "mcp-calls.jsonl").exists(), "the call log is opt-in")
