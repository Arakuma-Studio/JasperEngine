#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""Smoke test of the editor's MCP server core (stdio + Streamable HTTP).

Runs the real editor under Xvfb against a scratch copy of a project and checks the
protocol plumbing: initialize, tools/list (schemas), a read tool, an edit + undo,
batch with undo_group, editor_status, protocol errors, HTTP token/sessions/SSE/Origin,
and that the editor exits when the client disconnects.

    uv run --no-project --with mcp --with jsonschema --with httpx \
        python tools/mcp/core_smoke.py --editor build/linux/editor-install/bin/jasperengine-editor \
        [--project build/fixture-unity]

Self-contained on purpose: the fuller harness lives next to it in tools/mcp/.
"""

import argparse
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import traceback

import anyio
import httpx
import jsonschema
from mcp import ClientSession
from mcp.client.stdio import StdioServerParameters, stdio_client
from mcp.client.streamable_http import create_mcp_http_client, streamable_http_client

FAILURES = []
# settings.yaml mcp.http_token of the scratch editors: HTTP requires the bearer token
HTTP_TOKEN = "core-smoke-token"


def check(condition, message):
    if condition:
        print(f"  ok   {message}")
    else:
        print(f"  FAIL {message}")
        FAILURES.append(message)
    return condition


def text_of(result):
    return "\n".join(getattr(c, "text", "") for c in result.content if getattr(c, "type", "") == "text")


def structured(result):
    return getattr(result, "structured_content", None) or getattr(result, "structuredContent", None) or {}


def is_error(result):
    return bool(getattr(result, "is_error", getattr(result, "isError", False)))


def scratch(project_src):
    work = tempfile.mkdtemp(prefix="mcp-core-smoke-")
    os.makedirs(os.path.join(work, "home"))
    shutil.copytree(project_src, os.path.join(work, "project"))
    with open(os.path.join(work, "settings.yaml"), "w") as f:
        f.write(f"mcp:\n  http_token: {HTTP_TOKEN}\n")
    return work


def editor_env(work):
    env = dict(os.environ)
    env["HOME"] = os.path.join(work, "home")
    env.pop("DISPLAY", None)  # --headless starts a private Xvfb
    return env


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def count_entities(data):
    for key in ("entities", "items", "gameobjects"):
        if isinstance(data.get(key), list):
            return len(data[key])
    return data.get("count", data.get("entity_count"))


CORE_TOOLS = {"batch", "get_job", "cancel_job", "undo", "redo", "editor_status", "respond_dialog"}
TOOL_SAMPLE = {"get_project_summary", "create_entity", "find_entities", "set_properties",
               "build_project", "save_scene", "get_entity"}


async def exercise(session, label):
    """The checks shared by both transports."""
    tools = (await session.list_tools()).tools
    names = {t.name for t in tools}
    check(CORE_TOOLS <= names, f"[{label}] core tools listed")
    check(TOOL_SAMPLE <= names, f"[{label}] module tools listed ({len(tools)} tools)")
    check(not ({"undo_editor", "redo_editor", "list_scene_entities", "inspect_entity", "export_project"} & names),
          f"[{label}] dropped legacy tools are gone")
    bad = []
    for tool in tools:
        schema = getattr(tool, "input_schema", None) or getattr(tool, "inputSchema", None)
        try:
            jsonschema.Draft202012Validator.check_schema(schema)
            if schema.get("type") != "object":
                bad.append(tool.name + " (not an object)")
        except Exception as e:  # noqa: BLE001
            bad.append(f"{tool.name}: {e}")
    check(not bad, f"[{label}] every inputSchema is valid JSON Schema {bad[:3]}")

    status = await session.call_tool("editor_status", {})
    data = structured(status)
    check(not is_error(status) and data.get("project", {}).get("name"), f"[{label}] editor_status: {text_of(status)[:100]}")
    scene_id = data.get("project", {}).get("selected_scene_id")

    summary = await session.call_tool("get_project_summary", {})
    check(not is_error(summary) and structured(summary).get("scenes_in_build"), f"[{label}] get_project_summary")

    async def entity_count():
        r = await session.call_tool("find_entities", {"scene": scene_id, "limit": 100000})
        return count_entities(structured(r))

    before = await entity_count()
    created = await session.call_tool("create_entity", {"scene": scene_id, "type": "empty", "name": "McpSmoke"})
    check(not is_error(created), f"[{label}] create_entity: {text_of(created)[:100]}")
    after = await entity_count()
    check(before is not None and after == before + 1, f"[{label}] entity added ({before} -> {after})")
    undone = await session.call_tool("undo", {"scene": scene_id})
    check(not is_error(undone), f"[{label}] undo: {text_of(undone)[:80]}")
    check(await entity_count() == before, f"[{label}] undo removed it")
    redone = await session.call_tool("redo", {"scene": scene_id})
    check(not is_error(redone) and await entity_count() == before + 1, f"[{label}] redo restored it")
    await session.call_tool("undo", {"scene": scene_id})

    batch = await session.call_tool("batch", {
        "undo_group": True,
        "calls": [
            {"tool": "create_entity", "arguments": {"scene": scene_id, "type": "empty", "name": "BatchA"}},
            {"tool": "create_entity", "arguments": {"scene": scene_id, "type": "cube", "name": "BatchB"}},
            {"tool": "editor_status", "arguments": {}},
        ]})
    bdata = structured(batch)
    check(not is_error(batch) and bdata.get("succeeded") == 3, f"[{label}] batch ran 3 calls: {text_of(batch)[:120]}")
    check(scene_id in bdata.get("undo_group", {}).get("scene_ids", []), f"[{label}] batch grouped the scene's edits")
    check(await entity_count() == before + 2, f"[{label}] batch added 2 GameObjects")
    await session.call_tool("undo", {"scene": scene_id})
    check(await entity_count() == before, f"[{label}] one undo reverted the whole batch")

    failing = await session.call_tool("batch", {"calls": [
        {"tool": "get_entity", "arguments": {"scene": scene_id, "entity": 999999}},
        {"tool": "editor_status", "arguments": {}}]})
    check(structured(failing).get("stopped_early") is True, f"[{label}] batch stop_on_error")

    try:
        r = await session.call_tool("no_such_tool", {})
        check(False, f"[{label}] unknown tool is a JSON-RPC error (got a result: {text_of(r)[:60]})")
    except Exception as e:  # noqa: BLE001
        check("Unknown tool" in str(e) or "-32602" in str(e) or "no_such_tool" in str(e),
              f"[{label}] unknown tool -> JSON-RPC error ({e})")

    jobs = await session.call_tool("get_job", {})
    check(not is_error(jobs), f"[{label}] get_job lists jobs")
    missing = await session.call_tool("get_job", {"job_id": "job-404"})
    check(is_error(missing), f"[{label}] get_job on an unknown id is a tool error")
    return scene_id


async def stdio_test(editor, project_src):
    print("stdio transport")
    work = scratch(project_src)
    params = StdioServerParameters(command=editor, args=["mcp", "--project", os.path.join(work, "project"), "--headless"],
                                   env=editor_env(work), cwd=work)
    async with stdio_client(params) as streams:
        async with ClientSession(streams[0], streams[1]) as session:
            init = await session.initialize()
            check(bool(init.instructions), "initialize returns instructions")
            check(init.server_info.name == "jasperengine-editor", f"serverInfo {init.server_info.name}")
            await exercise(session, "stdio")
    shutil.rmtree(work, ignore_errors=True)


def raw_stdio_test(editor, project_src):
    """stdout carries only JSON-RPC lines, and the editor exits when stdin closes."""
    print("raw stdio")
    work = scratch(project_src)
    proc = subprocess.Popen([editor, "mcp", "--project", os.path.join(work, "project"), "--headless"],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            env=editor_env(work), cwd=work)
    requests = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize",
         "params": {"protocolVersion": "2024-11-05", "capabilities": {}, "clientInfo": {"name": "raw", "version": "1"}}},
        {"jsonrpc": "2.0", "method": "notifications/initialized"},
        {"jsonrpc": "2.0", "id": 2, "method": "ping"},
        {"jsonrpc": "2.0", "id": 3, "method": "tools/call", "params": {"name": "editor_status", "arguments": {}}},
        {"jsonrpc": "2.0", "id": 4, "method": "no/such/method"},
    ]
    for r in requests:
        proc.stdin.write((json.dumps(r) + "\n").encode())
    proc.stdin.write(b"this is not json\n")
    proc.stdin.flush()
    lines = []
    deadline = time.time() + 120
    while len(lines) < 5 and time.time() < deadline:
        line = proc.stdout.readline()
        if not line:
            break
        lines.append(line.decode())
    t0 = time.time()
    proc.stdin.close()
    try:
        code = proc.wait(timeout=60)
    except subprocess.TimeoutExpired:
        proc.kill()
        code = None
    rest = proc.stdout.read().decode()
    lines += [l for l in rest.splitlines() if l.strip()]
    stderr = proc.stderr.read().decode()

    parsed = []
    for line in lines:
        try:
            parsed.append(json.loads(line))
        except ValueError:
            parsed.append(None)
    check(all(p is not None and p.get("jsonrpc") == "2.0" for p in parsed), f"stdout has only JSON-RPC lines ({len(lines)} lines)")
    by_id = {p.get("id"): p for p in parsed if p}
    check(by_id.get(1, {}).get("result", {}).get("protocolVersion") == "2024-11-05", "protocol version 2024-11-05 negotiated")
    check(by_id.get(2, {}).get("result") == {}, "ping")
    check(by_id.get(3, {}).get("result", {}).get("isError") is False, "tools/call over raw stdio")
    check(by_id.get(4, {}).get("error", {}).get("code") == -32601, "unknown method -> -32601")
    check(any(p and p.get("error", {}).get("code") == -32700 for p in parsed), "bad JSON -> -32700")
    check(code == 0, f"editor exited cleanly after stdin closed (code {code}, {time.time() - t0:.1f}s)")
    check("stdin closed" in stderr, "stderr reports the disconnect")
    if FAILURES:
        print(stderr[-2000:])
    shutil.rmtree(work, ignore_errors=True)


async def http_test(editor, project_src):
    print("Streamable HTTP transport")
    work = scratch(project_src)
    port = free_port()
    proc = subprocess.Popen([editor, "mcp", "--project", os.path.join(work, "project"), "--headless", "--http-port", str(port)],
                            stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                            env=editor_env(work), cwd=work)
    url = f"http://127.0.0.1:{port}/mcp"
    try:
        deadline = time.time() + 120
        while time.time() < deadline:
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=1):
                    break
            except OSError:
                time.sleep(0.5)
        async with httpx.AsyncClient(timeout=60) as client:
            r = await client.post(url, json={"jsonrpc": "2.0", "id": 1, "method": "ping"})
            check(r.status_code == 401, f"no bearer token -> 401 ({r.status_code})")
        async with httpx.AsyncClient(timeout=60, headers={"Authorization": f"Bearer {HTTP_TOKEN}"}) as client:
            r = await client.get(url)
            check(r.status_code == 405, f"GET /mcp -> 405 ({r.status_code})")
            r = await client.post(url, json={"jsonrpc": "2.0", "id": 1, "method": "ping"}, headers={"Origin": "http://evil.example"})
            check(r.status_code == 403, f"foreign Origin -> 403 ({r.status_code})")
            r = await client.post(url, json={"jsonrpc": "2.0", "id": 1, "method": "ping"})
            check(r.status_code == 400, f"no session -> 400 ({r.status_code})")
            r = await client.post(url, headers={"Accept": "application/json, text/event-stream"},
                                  json={"jsonrpc": "2.0", "id": 1, "method": "initialize",
                                        "params": {"protocolVersion": "2025-06-18", "capabilities": {},
                                                   "clientInfo": {"name": "raw-http", "version": "1"}}})
            sid = r.headers.get("mcp-session-id")
            check(r.status_code == 200 and sid, f"initialize issues Mcp-Session-Id ({sid})")
            headers = {"Mcp-Session-Id": sid, "Accept": "application/json, text/event-stream"}
            r = await client.post(url, headers=headers, json={"jsonrpc": "2.0", "method": "notifications/initialized"})
            check(r.status_code == 202, f"notification -> 202 ({r.status_code})")
            r = await client.post(url, headers=headers, json={"jsonrpc": "2.0", "id": 2, "method": "tools/call",
                                  "params": {"name": "editor_status", "arguments": {}, "_meta": {"progressToken": "p1"}}})
            sse_ok = r.headers.get("content-type", "").startswith("text/event-stream") and '"id":2' in r.text
            check(sse_ok, "progressToken + Accept SSE -> text/event-stream response")
            r = await client.delete(url, headers={"Mcp-Session-Id": sid})
            check(r.status_code == 200, f"DELETE ends the session ({r.status_code})")
            r = await client.post(url, headers=headers, json={"jsonrpc": "2.0", "id": 3, "method": "ping"})
            check(r.status_code == 404, f"ended session -> 404 ({r.status_code})")

        async with create_mcp_http_client(headers={"Authorization": f"Bearer {HTTP_TOKEN}"}) as http, \
                streamable_http_client(url, http_client=http) as streams:
            async with ClientSession(streams[0], streams[1]) as session:
                init = await session.initialize()
                check(bool(init.instructions), "[http] initialize returns instructions")
                await exercise(session, "http")
                progress = []
                r = await session.call_tool("editor_status", {}, progress_callback=lambda *a: progress.append(a))
                check(not is_error(r), "[http] call with a progress callback (SSE response)")
    finally:
        proc.stdin.close()
        try:
            code = proc.wait(timeout=60)
        except subprocess.TimeoutExpired:
            proc.kill()
            code = None
        check(code == 0, f"[http] editor exited after stdin closed (code {code})")
        if FAILURES:
            print(proc.stderr.read().decode()[-2000:])
        shutil.rmtree(work, ignore_errors=True)


def main():
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    parser = argparse.ArgumentParser()
    parser.add_argument("--editor", default=os.path.join(root, "build/linux/editor-install/bin/jasperengine-editor"))
    parser.add_argument("--project", default=None,
                        help="Project to copy into a scratch directory (never modified). Default: the "
                             "default fixture (JASPER_FIXTURE, build/fixture-unity, /fixture, ~/je/fixture-unity)")
    parser.add_argument("--only", choices=["stdio", "raw", "http"])
    args = parser.parse_args()
    editor = os.path.abspath(args.editor)
    if args.project is None:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import harness
        try:
            args.project = str(harness.default_fixture())
        except harness.HarnessError as e:
            sys.exit(f"core_smoke: {e}")
    project = os.path.abspath(args.project)

    tests = {"raw": lambda: raw_stdio_test(editor, project),
             "stdio": lambda: anyio.run(stdio_test, editor, project),
             "http": lambda: anyio.run(http_test, editor, project)}
    for name, run in tests.items():
        if args.only and args.only != name:
            continue
        try:
            run()
        except Exception:  # noqa: BLE001
            traceback.print_exc()
            FAILURES.append(f"{name}: exception")
    print(f"\n{len(FAILURES)} failure(s)")
    for failure in FAILURES:
        print(" -", failure)
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
