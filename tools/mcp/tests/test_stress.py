"""Stress and robustness tests for the MCP server core (transports, dispatch, deferral, jobs).

The core agent once saw the stdio server stop answering right after a `batch` call (the editor
busy at ~70% CPU, stdin still open). These tests hammer the paths that could hang or race:
rapid sequential calls, large batches (with deferred calls inside), concurrent HTTP clients
next to the stdio client, cancellation mid-wait, clients that disconnect mid-call, Play
start/stop loops, a modal dialog, an unmapped (minimized) window, project switches, and the
stdio / HTTP framing edge cases (split messages, long lines, invalid UTF-8, keep-alive,
chunked bodies, Expect: 100-continue, oversized results).

The editor runs on a harness-owned Xvfb (xdotool reaches its window) with the HTTP transport
on a free port next to stdio.
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

from harness import Editor, RawEditor, skip


def _free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


PORT = _free_port()
EDITOR = {"xvfb": "harness", "extra_args": ["--http-port", str(PORT)]}
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
        lines = ["POST /mcp HTTP/1.1", f"Host: 127.0.0.1:{self.port}", "Content-Type: application/json", f"Accept: {accept}"]
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


def text_of(result: dict) -> str:
    return "".join(c.get("text", "") for c in result.get("content", []) if c.get("type") == "text")


# -- sequential volume ---------------------------------------------------------------


async def test_rapid_sequential_calls(ed: Editor):
    """Hundreds of back-to-back calls of every flavour; each answers promptly."""
    start = time.monotonic()
    slowest = 0.0
    for i in range(300):
        t0 = time.monotonic()
        kind = i % 5
        if kind == 0:
            await ed.call("editor_status")
        elif kind == 1:
            await ed.call("find_entities", limit=20)
        elif kind == 2:
            await ed.call("get_job")
        elif kind == 3:
            await ed.ping()
        else:
            await ed.call("get_scene_tree")
        slowest = max(slowest, time.monotonic() - t0)
    total = time.monotonic() - start
    ed.check(slowest < 20, f"one call took {slowest:.1f}s")
    print(f"     300 sequential calls in {total:.1f}s (slowest {slowest:.2f}s)")
    await ed.check_alive()


async def test_large_batches(ed: Editor):
    """Max-size batches, batches with deferred calls inside, failing calls, repeated."""
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
    for round_ in range(5):
        calls = []
        for i in range(30):
            calls.append(("editor_status", {}))
            if i % 10 == 5:
                calls.append(("wait", {"frames": 3}))
            if i % 15 == 7:
                calls.append(("screenshot", {"view": "scene", "max_width": 160}))
            if i % 12 == 3:
                calls.append(("no_such_tool", {}))   # an error inside, stop_on_error false
        t0 = time.monotonic()
        out = await ed.batch(calls, stop_on_error=False)
        ed.check_eq(len(out["results"]), len(calls), f"round {round_}: every call has a result")
        ed.check(out["failed"] >= 1, "the unknown tool failed inside the batch")
        ed.check(time.monotonic() - t0 < 120, "batch with deferred calls took too long")
    await ed.check_alive()


async def test_batch_then_more_calls(ed: Editor):
    """The reported hang: calls right after a batch (stdio), many times."""
    for i in range(20):
        await ed.batch([("create_entity", {"name": f"After{i}", "type": "empty"}),
                        ("wait", {"frames": 1}),
                        ("delete_entities", {"entities": [f"After{i}"]})])
        await asyncio.wait_for(ed.ping(), 15)
        await ed.call("editor_status", timeout=30)


# -- concurrency ----------------------------------------------------------------------


async def test_concurrent_http_and_stdio(ed: Editor):
    """Four HTTP sessions and the stdio client call at once; every answer matches its request."""
    clients = [await http_client() for _ in range(4)]

    async def http_worker(c: Http, n: int):
        for i in range(40):
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
        for i in range(60):
            if i % 3 == 0:
                await ed.call("editor_status")
            elif i % 3 == 1:
                await ed.batch([("find_entities", {"limit": 3}), ("wait", {"frames": 1})])
            else:
                await ed.ping()

    await asyncio.wait_for(asyncio.gather(stdio_worker(), *[http_worker(c, n) for n, c in enumerate(clients)]), 600)
    names = [f"Http{n}_{i}" for n in range(4) for i in range(0, 40, 4)]
    found = await ed.call("find_entities", name="Http*", limit=1000)
    ed.check_eq(len(found["entities"]), len(names), "every HTTP create landed once")
    await ed.batch([("delete_entities", {"entities": [e["id"] for e in found["entities"]]})])
    for c in clients:
        c.close()
    await ed.check_alive()


async def test_http_parallel_connections_burst(ed: Editor):
    """50 connections opened at once, one call each (accept loop, thread per connection)."""
    async def one(i: int):
        c = await http_client()
        try:
            r = await c.call("editor_status")
            assert not r.get("isError"), r
        finally:
            c.close()
    await asyncio.wait_for(asyncio.gather(*[one(i) for i in range(50)]), 300)
    await ed.check_alive()


# -- cancellation and disconnects ---------------------------------------------------------


async def test_http_cancel_mid_wait(ed: Editor):
    """notifications/cancelled from another connection ends a waiting HTTP call at once."""
    a = await http_client()
    b = await (await Http().connect()).initialize()
    b.session = a.session  # same MCP session, second connection
    t0 = time.monotonic()
    pending = asyncio.ensure_future(a.rpc("tools/call", {"name": "wait", "arguments": {"seconds": 30}}, id="long-1"))
    await asyncio.sleep(1.0)
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
    await asyncio.sleep(2.5)  # the abandoned wait finishes; its write to a closed socket must not hurt
    await ed.check_alive()
    await ed.call("editor_status")


async def test_stdio_cancel_mid_wait():
    """Over stdio: a cancelled call gets no answer, later calls do, nothing hangs."""
    async with RawEditor() as raw:
        await raw.initialize()
        raw.send({"jsonrpc": "2.0", "id": 10, "method": "tools/call",
                  "params": {"name": "wait", "arguments": {"seconds": 30}}})
        await asyncio.sleep(1.0)
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
        await asyncio.sleep(1.0)
        raw.send({"jsonrpc": "2.0", "method": "notifications/cancelled", "params": {"requestId": 13}})
        raw.send({"jsonrpc": "2.0", "id": 14, "method": "tools/call", "params": {"name": "editor_status", "arguments": {}}})
        t0 = time.monotonic()
        await raw.response(14, 60)
        if time.monotonic() - t0 > 15:
            raw.fail("a call after a cancelled batch waited for the batch's nested wait")
        await asyncio.sleep(1.0)
        if any(m.get("id") in (10, 13) for m in raw.messages):
            raw.fail("a cancelled request was answered (MCP: no response after cancellation)")
        if raw.bad_lines:
            raw.fail(f"non-JSON-RPC stdout: {raw.bad_lines[:3]!r}")


async def test_stdio_slow_reader_does_not_freeze_editor():
    """A stdio client that stops reading stdout (its pipe fills with large answers) must not
    block the editor: the main thread keeps serving the HTTP transport and ping still works
    once the client reads again."""
    port = _free_port()
    async with RawEditor(extra_args=["--http-port", str(port)]) as raw:
        await raw.initialize()
        raw.send({"jsonrpc": "2.0", "id": 2, "method": "tools/call", "params": {"name": "write_file", "arguments": {
            "path": "stress/slow.txt", "content": "0123456789abcdef0123456789abcdef\n" * 6000}}})
        await raw.response(2, 60)
        # stop reading stdout at the pipe (asyncio would otherwise keep buffering it)
        stdout_pipe = raw.proc._transport.get_pipe_transport(1)
        stdout_pipe.pause_reading()
        for i in range(40):   # ~200 KB per answer, far over a pipe buffer
            raw.send({"jsonrpc": "2.0", "id": 100 + i, "method": "tools/call", "params": {
                "name": "read_file", "arguments": {"path": "stress/slow.txt", "max_lines": 6000}}})
        await raw.proc.stdin.drain()
        await asyncio.sleep(3.0)
        c = await Http(port).connect()
        await c.initialize()
        t0 = time.monotonic()
        for _ in range(5):
            r = await c.call("editor_status", timeout=30)
            assert not r.get("isError"), r
        if time.monotonic() - t0 > 20:
            raw.fail("HTTP calls were slow while the stdio client did not read")
        c.close()
        # read again: every answer arrives
        stdout_pipe.resume_reading()
        for i in range(40):
            await raw.response(100 + i, 120)
        raw.send({"jsonrpc": "2.0", "id": 999, "method": "ping"})
        await raw.response(999, 30)


# -- stdio framing ----------------------------------------------------------------------------


async def test_stdio_framing_edge_cases():
    """Split messages, several per write, CRLF, garbage, invalid UTF-8, a very long line."""
    async with RawEditor() as raw:
        await raw.initialize()
        stdin = raw.proc.stdin
        # one message split over several writes with pauses
        msg = json.dumps({"jsonrpc": "2.0", "id": 20, "method": "tools/call",
                          "params": {"name": "editor_status", "arguments": {}}}).encode() + b"\n"
        for i in range(0, len(msg), 7):
            stdin.write(msg[i:i + 7])
            await stdin.drain()
            await asyncio.sleep(0.01)
        await raw.response(20, 60)
        # three in one write, CRLF endings, blank lines between
        three = b"".join(json.dumps({"jsonrpc": "2.0", "id": i, "method": "ping"}).encode() + b"\r\n\r\n"
                         for i in (21, 22, 23))
        stdin.write(three)
        await stdin.drain()
        for i in (21, 22, 23):
            await raw.response(i, 30)
        # garbage: a parse error, then business as usual
        stdin.write(b"this is not json\n")
        raw.send({"jsonrpc": "2.0", "id": 24, "method": "ping"})
        await raw.response(24, 30)
        if not any(m.get("error", {}).get("code") == -32700 for m in raw.messages):
            raw.fail("garbage line got no parse error")
        # invalid UTF-8 inside a string: still a valid request
        bad = (b'{"jsonrpc":"2.0","id":25,"method":"tools/call","params":{"name":"find_entities",'
               b'"arguments":{"name":"caf\xe9\xff\xfe"}}}\n')
        stdin.write(bad)
        await stdin.drain()
        r = await raw.response(25, 60)
        if "result" not in r:
            raw.fail(f"invalid UTF-8 in an argument broke the request: {r}")
        # a 20 MB line (a huge argument): answered, and the line after it too
        huge = json.dumps({"jsonrpc": "2.0", "id": 26, "method": "tools/call",
                           "params": {"name": "find_entities", "arguments": {"name": "x" * (20 * 1024 * 1024)}}})
        t0 = time.monotonic()
        stdin.write(huge.encode() + b"\n")
        raw.send({"jsonrpc": "2.0", "id": 27, "method": "ping"})
        await stdin.drain()
        await raw.response(26, 120)
        await raw.response(27, 60)
        if time.monotonic() - t0 > 60:
            raw.fail(f"a 20 MB line took {time.monotonic() - t0:.0f}s")
        # JSON-RPC batch arrays are not supported: an error, not silence
        stdin.write(b'[{"jsonrpc":"2.0","id":28,"method":"ping"}]\n')
        raw.send({"jsonrpc": "2.0", "id": 29, "method": "ping"})
        await raw.response(29, 30)
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
    # invalid UTF-8 in an HTTP body
    bad = (b'{"jsonrpc":"2.0","id":"utf8","method":"tools/call","params":{"name":"find_entities",'
           b'"arguments":{"name":"\xc3\x28"}}}')
    status, _, data = await c.post(bad)
    ed.check_eq(status, 200, f"invalid UTF-8 body: {data[:200]!r}")
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


# -- Play ------------------------------------------------------------------------------------


async def test_play_start_stop_loop(ed: Editor):
    """Play start/stop, waiting and not waiting, with calls in between; ends stopped."""
    for i in range(4):
        await ed.call("play", action="start", timeout_seconds=180)
        await ed.call("get_play_state")
        await ed.call("wait", frames=5)
        await ed.call("play", action="stop")
    # fire-and-forget starts and stops back to back
    for i in range(4):
        await ed.call_raw("play", {"action": "start", "wait": False})
        await ed.call("get_play_state")
        await ed.call_raw("play", {"action": "stop"})
    for _ in range(120):
        state = await ed.call("get_play_state")
        if not state.get("playing") and state.get("state", "stopped") in ("stopped", None):
            break
        await ed.call_raw("play", {"action": "stop"})
        await asyncio.sleep(0.5)
    st = await ed.call("editor_status")
    ed.check(not st["playing"], f"still playing after the loop: {st}")
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
    await ed.call("wait", frames=5)
    _xdotool(display, "key", "--clearmodifiers", "ctrl+s")
    dialog = None
    for _ in range(40):
        st = await ed.call("editor_status")
        dialog = st.get("dialog")
        if dialog:
            break
        await asyncio.sleep(0.25)
    if not dialog:
        skip("Ctrl+S did not open a dialog under this Xvfb (no window focus?)")
    # reads still work
    await ed.call("find_entities", limit=5, scene=sid)
    await ed.call("get_scene_tree", scene=sid)
    # edits are refused with the way out
    msg = await ed.call("create_entity", name="Blocked", type="empty", scene=sid, expect_error=True)
    ed.check_in("respond_dialog", msg)
    out = await ed.batch([("editor_status", {}), ("create_entity", {"name": "Blocked2", "type": "empty"})],
                         stop_on_error=False)
    ed.check_eq(out["failed"], 1, "the edit inside the batch is refused, the read is not")
    for i in range(30):
        await ed.call("editor_status")
    await ed.call("respond_dialog", choice=dialog["choices"][0] if dialog.get("choices") else "cancel")
    st = await ed.call("editor_status")
    ed.check(not st.get("dialog"), f"dialog still open: {st.get('dialog')}")
    await ed.call("create_entity", name="AfterDialog", type="empty", scene=sid)
    await ed.call("close_scene", scene=sid, unsaved="discard")


async def test_calls_while_window_unmapped(ed: Editor):
    """The editor window unmapped (as minimized): calls, waits on frames and screenshots answer
    (successfully or with an error) instead of hanging."""
    display, win = await _editor_window(ed)
    _xdotool(display, "windowunmap", "--sync", win)
    try:
        await asyncio.sleep(0.5)
        for _ in range(10):
            await ed.call("editor_status", timeout=30)
            await ed.call("wait", frames=3, timeout=60)
        await ed.batch([("create_entity", {"name": "Hidden", "type": "empty"}), ("wait", {"frames": 2}),
                        ("delete_entities", {"entities": ["Hidden"]})])
        r = await ed.call_raw("screenshot", {"view": "scene", "max_width": 160}, timeout=120)
        print(f"     screenshot while unmapped: {'error: ' + r.text[:100] if r.is_error else 'ok'}")
    finally:
        _xdotool(display, "windowmap", "--sync", win)
    await ed.check_alive()


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
        await asyncio.sleep(0.5)
        for i in range(3):
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
    """Calls over HTTP keep getting answers while the editor switches projects (stdio)."""
    c = await http_client()
    target = ed.scratch.root / "switched"
    stop = asyncio.Event()
    answered = 0

    async def pinger():
        nonlocal answered
        while not stop.is_set():
            r = await c.rpc("tools/call", {"name": "editor_status", "arguments": {}}, timeout=300)
            assert "result" in r, r
            answered += 1
            await asyncio.sleep(0.05)

    task = asyncio.ensure_future(pinger())
    await asyncio.sleep(0.3)
    await ed.call("create_project", path=str(target), name="Switched", unsaved="discard", timeout=300)
    st = await ed.call("editor_status")
    ed.check(str(target) in st["project"]["path"] or "Switched" in st["project"]["name"], f"switched? {st['project']}")
    stop.set()
    await asyncio.wait_for(task, 300)
    ed.check(answered > 0, "no HTTP answer during the switch")
    c.close()
    await ed.check_alive()
