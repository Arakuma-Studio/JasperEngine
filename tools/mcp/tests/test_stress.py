"""Public transport liveness and framing contracts on an owned empty project.

Mixed deferred batches must resume, concurrent sessions must not misroute replies, and
cancellation/disconnection/backpressure must leave the editor usable. HTTP framing, bounded
results, a human modal, minimized preview cleanup and project replacement have distinct owners.
"""

from __future__ import annotations

import asyncio
import json
import os
import shutil
import socket
import subprocess
import time
import uuid

from harness import HTTP_TOKEN, Editor, RawEditor, skip


def _free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


PORT = _free_port()
EDITOR = {"create": True, "name": "Transport Contracts", "xvfb": "harness", "extra_args": ["--http-port", str(PORT)]}
CI = True
TIMEOUT = 900


# -- a minimal HTTP/1.1 client (keep-alive, chunked, raw sockets) -----------------------


class Http:
    """One keep-alive connection to the editor's /mcp endpoint."""

    def __init__(self, port: int = PORT):
        self.port = port
        self.reader: asyncio.StreamReader | None = None
        self.writer: asyncio.StreamWriter | None = None
        self.session: str | None = None

    async def connect(self) -> "Http":
        self.reader, self.writer = await asyncio.open_connection("127.0.0.1", self.port, limit=64 * 1024 * 1024)
        return self

    def close(self) -> None:
        if self.writer:
            self.writer.close()
            self.writer = None

    def head(self, body_len: int | None, extra: dict | None = None, accept: str = "application/json, text/event-stream") -> bytes:
        lines = ["POST /mcp HTTP/1.1", f"Host: 127.0.0.1:{self.port}", "Content-Type: application/json", f"Accept: {accept}",
                 f"Authorization: Bearer {HTTP_TOKEN}"]
        if body_len is not None:
            lines.append(f"Content-Length: {body_len}")
        if self.session:
            lines.append(f"Mcp-Session-Id: {self.session}")
        for k, v in (extra or {}).items():
            lines.append(f"{k}: {v}")
        return ("\r\n".join(lines) + "\r\n\r\n").encode()

    async def read_response(self, timeout: float = 120.0) -> tuple[int, dict, bytes]:
        async def read():
            while True:
                status_line = (await self.reader.readline()).decode()
                if not status_line:
                    raise ConnectionError("connection closed before a response")
                status = int(status_line.split()[1])
                headers = {}
                while True:
                    line = (await self.reader.readline()).decode()
                    if line in ("\r\n", "\n", ""):
                        break
                    k, _, v = line.partition(":")
                    headers[k.strip().lower()] = v.strip()
                if status == 100:
                    continue  # interim response
                if "content-length" in headers:
                    body = await self.reader.readexactly(int(headers["content-length"]))
                else:
                    body = await self.reader.read()  # SSE: until the server closes
                return status, headers, body
        return await asyncio.wait_for(read(), timeout)

    async def post(self, message: dict | bytes, *, timeout: float = 120.0, extra: dict | None = None) -> tuple[int, dict, bytes]:
        body = message if isinstance(message, bytes) else json.dumps(message).encode()
        self.writer.write(self.head(len(body), extra) + body)
        await self.writer.drain()
        status, headers, data = await self.read_response(timeout)
        if headers.get("mcp-session-id"):
            self.session = headers["mcp-session-id"]
        return status, headers, data

    async def rpc(self, method: str, params: dict | None = None, *, id=None, timeout: float = 120.0) -> dict:
        msg = {"jsonrpc": "2.0", "id": id if id is not None else uuid.uuid4().hex[:8], "method": method}
        if params is not None:
            msg["params"] = params
        status, headers, body = await self.post(msg, timeout=timeout)
        if status != 200:
            raise AssertionError(f"HTTP {status} for {method}: {body[:300]!r}")
        if headers.get("content-type", "").startswith("text/event-stream"):
            events = [json.loads(line[5:]) for line in body.decode().splitlines() if line.startswith("data:")]
            return events[-1]
        return json.loads(body)

    async def initialize(self) -> "Http":
        r = await self.rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                          "clientInfo": {"name": "stress-http", "version": "1"}})
        assert "result" in r, r
        status, _, _ = await self.post({"jsonrpc": "2.0", "method": "notifications/initialized"})
        assert status == 202, status
        return self

    async def call(self, tool: str, args: dict | None = None, *, id=None, timeout: float = 120.0) -> dict:
        r = await self.rpc("tools/call", {"name": tool, "arguments": args or {}}, id=id, timeout=timeout)
        assert "result" in r, f"{tool}: {r}"
        return r["result"]


async def http_client() -> Http:
    return await (await Http().connect()).initialize()


# -- batch regression ---------------------------------------------------------------


async def test_large_batches(ed: Editor):
    """The maximum-size grouped edit and mixed deferred/error calls both complete."""
    tag = uuid.uuid4().hex[:6]
    out = await ed.batch([("create_entity", {"name": f"Bulk{tag}_{i}", "type": "empty"}) for i in range(200)],
                         undo_group=True)
    ed.check_eq(out["succeeded"], 200, f"200 creates: {out['failed']} failed")
    found = await ed.call("find_entities", name=f"Bulk{tag}_*", limit=1000)
    ed.check_eq(len(found["entities"]), 200, "created GameObjects")
    await ed.call("undo")
    found = await ed.call("find_entities", name=f"Bulk{tag}_*", limit=1000)
    ed.check_eq(len(found["entities"]), 0, "one undo removes the whole batch")

    # deferred calls (frames, screenshots, waits) in the middle of a batch resume it
    calls = []
    for i in range(4):
        calls.append(("editor_status", {}))
        if i == 1:
            calls.append(("wait", {"frames": 3}))
        if i == 2:
            calls.append(("screenshot", {"view": "scene", "max_width": 160}))
        if i == 3:
            calls.append(("no_such_tool", {}))   # an error inside, stop_on_error false
    out = await ed.batch(calls, stop_on_error=False)
    ed.check_eq(len(out["results"]), len(calls), "every call has a result")
    ed.check_eq(out["failed"], 1, "only the unknown tool fails inside the mixed deferred batch")
    await ed.check_alive()


# -- concurrency ----------------------------------------------------------------------


async def test_concurrent_http_and_stdio(ed: Editor):
    """Two HTTP sessions and the stdio client call at once; every answer matches its request."""
    clients = [await http_client() for _ in range(2)]

    async def http_worker(c: Http, n: int):
        for i in range(8):
            rid = f"c{n}-{i}"
            if i % 4 == 0:
                r = await c.rpc("tools/call", {"name": "create_entity",
                                               "arguments": {"name": f"Http{n}_{i}", "type": "empty"}}, id=rid)
            elif i % 4 == 1:
                r = await c.rpc("tools/call", {"name": "find_entities", "arguments": {"limit": 5}}, id=rid)
            elif i % 4 == 2:
                r = await c.rpc("ping", id=rid)
            else:
                r = await c.rpc("tools/call", {"name": "wait", "arguments": {"frames": 2}}, id=rid)
            assert r.get("id") == rid, f"answer to {rid} has id {r.get('id')}"
            assert "result" in r, r

    async def stdio_worker():
        for i in range(6):
            if i % 3 == 0:
                await ed.call("editor_status")
            elif i % 3 == 1:
                await ed.batch([("find_entities", {"limit": 3}), ("wait", {"frames": 1})])
            else:
                await ed.ping()

    await asyncio.wait_for(asyncio.gather(stdio_worker(), *[http_worker(c, n) for n, c in enumerate(clients)]), 600)
    names = [f"Http{n}_{i}" for n in range(2) for i in range(0, 8, 4)]
    found = await ed.call("find_entities", name="Http*", limit=1000)
    ed.check_eq(len(found["entities"]), len(names), "every HTTP create landed once")
    await ed.batch([("delete_entities", {"entities": [e["id"] for e in found["entities"]]})])
    for c in clients:
        c.close()
    await ed.check_alive()


# -- cancellation and disconnects ---------------------------------------------------------


async def wait_for_pending(ed: Editor, active: bool):
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        if ((await ed.call("editor_status"))["waiting_calls"] > 0) == active:
            return
    ed.fail(f"pending work never became {active}")


async def test_http_cancel_mid_wait(ed: Editor):
    """A same-session cancellation releases an actually pending HTTP call."""
    a = await http_client()
    b = await (await Http().connect()).initialize()
    b.session = a.session  # same MCP session, second connection
    t0 = time.monotonic()
    pending = asyncio.ensure_future(a.rpc("tools/call", {"name": "wait", "arguments": {"seconds": 30}}, id="long-1"))
    await wait_for_pending(ed, True)
    status, _, _ = await b.post({"jsonrpc": "2.0", "method": "notifications/cancelled",
                                 "params": {"requestId": "long-1", "reason": "test"}})
    ed.check_eq(status, 202, "cancel notification accepted")
    r = await asyncio.wait_for(pending, 20)
    ed.check(time.monotonic() - t0 < 15, "the cancelled wait still ran to its end")
    ed.check("error" in r or r.get("result", {}).get("isError"), f"cancelled call answered as success: {r}")
    # the connection and the server keep working
    r = await a.call("editor_status")
    ed.check(not r.get("isError"), "call after cancel")
    a.close()
    b.close()
    await ed.check_alive()


async def test_http_disconnect_mid_call(ed: Editor):
    """Clients that vanish mid-call, mid-headers or mid-body leave the server healthy."""
    c = await http_client()
    session = c.session
    # mid-call: the answer has nobody to go to
    body = json.dumps({"jsonrpc": "2.0", "id": 7, "method": "tools/call",
                       "params": {"name": "wait", "arguments": {"seconds": 2}}}).encode()
    c.writer.write(c.head(len(body)) + body)
    await c.writer.drain()
    c.close()
    # mid-headers and mid-body
    for chunk in (b"POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Le",
                  b"POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 500\r\n\r\n{\"jsonrpc\":"):
        r, w = await asyncio.open_connection("127.0.0.1", PORT)
        w.write(chunk)
        await w.drain()
        w.close()
    # a session-bound call on a fresh connection still works, and so does stdio
    d = Http()
    await d.connect()
    d.session = session
    r = await d.call("editor_status")
    ed.check(not r.get("isError"), "call after disconnects")
    d.close()
    await wait_for_pending(ed, False)  # wait for the abandoned deferred call to finish
    await ed.check_alive()
    await ed.call("editor_status")


async def raw_pending(raw: RawEditor, rid: int, active: bool):
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        rid += 1
        raw.send({"jsonrpc": "2.0", "id": rid, "method": "tools/call",
                  "params": {"name": "editor_status", "arguments": {}}})
        result = await raw.response(rid)
        if (result["result"]["structuredContent"]["waiting_calls"] > 0) == active:
            return rid
    raw.fail(f"pending work never became {active}")


async def test_stdio_cancel_mid_wait():
    """Over stdio: a cancelled call gets no answer, later calls do, nothing hangs."""
    async with RawEditor(create=True) as raw:
        await raw.initialize()
        raw.send({"jsonrpc": "2.0", "id": 10, "method": "tools/call",
                  "params": {"name": "wait", "arguments": {"seconds": 30}}})
        marker = await raw_pending(raw, 200, True)
        raw.send({"jsonrpc": "2.0", "method": "notifications/cancelled", "params": {"requestId": 10}})
        raw.send({"jsonrpc": "2.0", "id": 11, "method": "ping"})
        await raw.response(11, 30)
        raw.send({"jsonrpc": "2.0", "id": 12, "method": "tools/call", "params": {"name": "editor_status", "arguments": {}}})
        r = await raw.response(12, 60)
        if "result" not in r:
            raw.fail(f"editor_status after a cancel: {r}")
        # a batch cancelled while a nested call waits
        raw.send({"jsonrpc": "2.0", "id": 13, "method": "tools/call", "params": {"name": "batch", "arguments": {
            "calls": [{"tool": "wait", "arguments": {"seconds": 20}}, {"tool": "editor_status", "arguments": {}}]}}})
        marker = await raw_pending(raw, marker, True)
        raw.send({"jsonrpc": "2.0", "method": "notifications/cancelled", "params": {"requestId": 13}})
        raw.send({"jsonrpc": "2.0", "id": 14, "method": "tools/call", "params": {"name": "editor_status", "arguments": {}}})
        t0 = time.monotonic()
        await raw.response(14, 60)
        if time.monotonic() - t0 > 15:
            raw.fail("a call after a cancelled batch waited for the batch's nested wait")
        await raw_pending(raw, marker, False)
        if any(m.get("id") in (10, 13) for m in raw.messages):
            raw.fail("a cancelled request was answered (MCP: no response after cancellation)")
        if raw.bad_lines:
            raw.fail(f"non-JSON-RPC stdout: {raw.bad_lines[:3]!r}")


async def test_stdio_slow_reader_does_not_freeze_editor():
    """A stdio client that stops reading stdout (its pipe fills with large answers) must not
    block the editor: the main thread keeps serving the HTTP transport and ping still works
    once the client reads again."""
    port = _free_port()
    async with RawEditor(create=True, extra_args=["--http-port", str(port)]) as raw:
        await raw.initialize()
        raw.send({"jsonrpc": "2.0", "id": 2, "method": "tools/call", "params": {"name": "write_file", "arguments": {
            "path": "stress/slow.txt", "content": "0123456789abcdef0123456789abcdef\n" * 6000}}})
        await raw.response(2, 60)
        # stop reading stdout at the pipe (asyncio would otherwise keep buffering it)
        stdout_pipe = raw.proc._transport.get_pipe_transport(1)
        stdout_pipe.pause_reading()
        for i in range(8):   # ~200 KB per answer, far over a pipe buffer
            raw.send({"jsonrpc": "2.0", "id": 100 + i, "method": "tools/call", "params": {
                "name": "read_file", "arguments": {"path": "stress/slow.txt", "max_lines": 6000}}})
        await raw.proc.stdin.drain()
        import array
        import fcntl
        import termios
        pipe = stdout_pipe.get_extra_info("pipe")
        capacity = fcntl.fcntl(pipe.fileno(), fcntl.F_GETPIPE_SZ)
        deadline = time.monotonic() + 30
        while True:
            available = array.array("i", [0])
            fcntl.ioctl(pipe.fileno(), termios.FIONREAD, available, True)
            if available[0] >= capacity:
                break
            if time.monotonic() >= deadline:
                raw.fail("the controlled stdout pipe never filled")
            await asyncio.sleep(0.01)
        c = await Http(port).connect()
        await c.initialize()
        t0 = time.monotonic()
        for _ in range(2):
            r = await c.call("editor_status", timeout=30)
            assert not r.get("isError"), r
        if time.monotonic() - t0 > 20:
            raw.fail("HTTP calls were slow while the stdio client did not read")
        c.close()
        # read again: every answer arrives
        stdout_pipe.resume_reading()
        for i in range(8):
            await raw.response(100 + i, 120)
        raw.send({"jsonrpc": "2.0", "id": 999, "method": "ping"})
        await raw.response(999, 30)


# -- stdio framing ----------------------------------------------------------------------------


async def test_stdio_framing_edge_cases():
    """A valid message split across writes and several CRLF-delimited requests in one write."""
    async with RawEditor(create=True) as raw:
        await raw.initialize()
        stdin = raw.proc.stdin
        # One valid message split over writes; the receiver must preserve the byte stream.
        msg = json.dumps({"jsonrpc": "2.0", "id": 20, "method": "tools/call",
                          "params": {"name": "editor_status", "arguments": {}}}).encode() + b"\n"
        for i in range(0, len(msg), 7):
            stdin.write(msg[i:i + 7])
            await stdin.drain()

        await raw.response(20, 60)
        # three in one write, CRLF endings, blank lines between
        three = b"".join(json.dumps({"jsonrpc": "2.0", "id": i, "method": "ping"}).encode() + b"\r\n\r\n"
                         for i in (21, 22, 23))
        stdin.write(three)
        await stdin.drain()
        for i in (21, 22, 23):
            await raw.response(i, 30)
        if raw.bad_lines:
            raw.fail(f"non-JSON-RPC stdout: {raw.bad_lines[:3]!r}")


# -- HTTP framing --------------------------------------------------------------------------


async def test_http_keepalive_chunked_expect(ed: Editor):
    """Several requests on one keep-alive connection, a chunked body, Expect: 100-continue."""
    c = await http_client()
    for i in range(10):
        r = await c.rpc("ping", id=f"ka{i}")
        ed.check_eq(r.get("id"), f"ka{i}", "keep-alive answer id")
    # chunked body
    body = json.dumps({"jsonrpc": "2.0", "id": "chunked", "method": "tools/call",
                       "params": {"name": "editor_status", "arguments": {}}}).encode()
    chunks = b""
    for i in range(0, len(body), 10):
        part = body[i:i + 10]
        chunks += f"{len(part):x}\r\n".encode() + part + b"\r\n"
    chunks += b"0\r\n\r\n"
    c.writer.write(c.head(None, {"Transfer-Encoding": "chunked"}) + chunks)
    await c.writer.drain()
    status, _, data = await c.read_response()
    ed.check_eq(status, 200, f"chunked body: {data[:200]!r}")
    ed.check_eq(json.loads(data).get("id"), "chunked", "chunked answer id")
    # Expect: 100-continue: the body follows the interim response
    body = json.dumps({"jsonrpc": "2.0", "id": "expect", "method": "ping"}).encode()
    c.writer.write(c.head(len(body), {"Expect": "100-continue"}))
    await c.writer.drain()
    line = await asyncio.wait_for(c.reader.readline(), 10)
    ed.check(b"100" in line, f"no 100 Continue: {line!r}")
    await c.reader.readline()  # blank line ending the interim response
    c.writer.write(body)
    await c.writer.drain()
    status, _, data = await c.read_response()
    ed.check_eq(json.loads(data).get("id"), "expect", "Expect: 100-continue answer")
    c.close()


# -- results ---------------------------------------------------------------------------------


async def test_large_result_is_capped(ed: Editor):
    """Results over the server cap (256 KB of text, 1 MB of structuredContent) arrive truncated
    with a note, and the server keeps going."""
    line = "0123456789abcdef0123456789abcdef\n"
    await ed.call("write_file", path="stress/big.txt", content=line * 20000)   # ~660 KB
    r = await ed.call_raw("read_file", {"path": "stress/big.txt", "max_lines": 20000})
    ed.check(len(r.text) <= 260 * 1024, f"read_file result not capped: {len(r.text)} bytes")
    ed.check("truncated by the MCP server" in r.text, f"no truncation note: ...{r.text[-200:]!r}")
    # a batch of several large results: text and structuredContent both capped
    calls = [{"tool": "read_file", "arguments": {"path": "stress/big.txt", "max_lines": 20000}} for _ in range(6)]
    r = await ed.call_raw("batch", {"calls": calls})
    ed.check(not r.is_error, f"batch failed: {r.text[:300]}")
    ed.check(len(r.text) <= 260 * 1024, f"batch text not capped: {len(r.text)} bytes")
    structured = r.structured or {}
    ed.check(len(json.dumps(structured)) <= 1100 * 1024, "structuredContent not capped")
    await ed.call("delete_asset", paths=["stress/big.txt"])
    await ed.check_alive()


# -- dialogs, minimized window, project load --------------------------------------------------


def _xdotool(display: str, *args: str) -> str:
    env = dict(os.environ, DISPLAY=display)
    return subprocess.run(["xdotool", *args], env=env, capture_output=True, text=True, timeout=20).stdout.strip()


async def _editor_window(ed: Editor) -> tuple[str, str]:
    if not shutil.which("xdotool") or not ed._xvfb or not ed._xvfb.display:
        skip("needs xdotool and the harness Xvfb")
    display = ed._xvfb.display
    wins = _xdotool(display, "search", "--onlyvisible", "--class", "").split()
    if not wins:
        wins = _xdotool(display, "search", "--name", ".").split()
    if not wins:
        skip("no X window found for the editor")
    return display, wins[-1]


async def test_calls_while_dialog_open(ed: Editor):
    """A human-only modal (Save Scene for an untitled scene, opened with Ctrl+S): reads work,
    edits are refused naming respond_dialog, batches answer per call, respond_dialog closes it."""
    display, win = await _editor_window(ed)
    made = await ed.call("create_scene", name="Untitled Stress", type="3D")
    sid = made["scene"]["id"]
    await ed.call("select_scene", scene=sid)
    await ed.call("create_entity", name="Dirty", type="empty", scene=sid)
    _xdotool(display, "windowfocus", "--sync", win)
    await ed.call("focus_window", window="scene")
    try:
        _xdotool(display, "keydown", "ctrl+s")
        dialog = None
        try:
            for _ in range(40):
                st = await ed.call("editor_status")
                dialog = st.get("dialog")
                if dialog:
                    break
        finally:
            _xdotool(display, "keyup", "ctrl+s")
        ed.check(dialog, "Save on an untitled scene opens its documented modal")
        # Reads are allowed while a modal blocks edits; a frame wait is itself an edit.
        await ed.call("find_entities", limit=5, scene=sid)
        await ed.call("get_scene_tree", scene=sid)
        msg = await ed.call("create_entity", name="Blocked", type="empty", scene=sid, expect_error=True)
        ed.check_in("respond_dialog", msg)
        out = await ed.batch([("editor_status", {}), ("create_entity", {"name": "Blocked2", "type": "empty"})],
                             stop_on_error=False)
        ed.check_eq(out["failed"], 1, "the edit inside the batch is refused, the read is not")
        await ed.call("respond_dialog", choice="cancel")
        st = await ed.call("editor_status")
        ed.check(not st.get("dialog"), f"dialog still open: {st.get('dialog')}")
        await ed.call("create_entity", name="AfterDialog", type="empty", scene=sid)
    finally:
        st = await ed.call("editor_status")
        if st.get("dialog"):
            await ed.call("respond_dialog", choice="cancel")
        await ed.call("close_scene", scene=sid, unsaved="discard")


async def test_selection_changes_while_window_unmapped(ed: Editor):
    """The reported hang: the Inspector queues a one-shot preview render (a light's direction,
    a mesh's material) and the selection changes in a frame that renders nothing (minimized
    window, or a skipped frame under load). Its cleanup loop then neither erased the unused
    preview (its scene still queued) nor advanced: the main thread spun forever (~70% CPU),
    calls were never run, ping still answered."""
    display, win = await _editor_window(ed)
    made = await ed.call("create_scene", name="PreviewHang", type="3D")
    sid = made["scene"]["id"]
    await ed.call("select_scene", scene=sid)
    _xdotool(display, "windowunmap", "--sync", win)
    try:
        for i in range(1):
            # each create selects the new GameObject: the Inspector shows its preview
            await ed.call("create_entity", name=f"Sun{i}", type="directional_light", scene=sid, timeout=30)
            await ed.call("wait", frames=3, timeout=30)
            await ed.call("create_entity", name=f"Box{i}", type="box", scene=sid, timeout=30)
            await ed.call("wait", frames=3, timeout=30)
            await ed.call("create_entity", name=f"Empty{i}", type="empty", scene=sid, timeout=30)
            await ed.call("wait", frames=3, timeout=30)
            await ed.call("editor_status", timeout=30)
    finally:
        _xdotool(display, "windowmap", "--sync", win)
    await ed.call("close_scene", scene=sid, unsaved="discard", timeout=30)
    await ed.check_alive()


async def test_calls_during_project_switch(ed: Editor):
    client = await http_client()
    target = ed.scratch.root / "switched"
    try:
        response, _ = await asyncio.gather(
            client.call("editor_status"),
            ed.call("create_project", path=str(target), name="Switched", unsaved="discard"))
        ed.check(not response.get("isError"), response)
        current = await client.call("editor_status")
        ed.check_eq(current["structuredContent"]["project"]["name"], "Switched",
                    "the established HTTP session resolves the new project")
        ed.check_eq((await ed.call("get_project_summary"))["name"], "Switched")
    finally:
        client.close()
