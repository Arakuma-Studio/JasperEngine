"""A fake JasperEngine editor MCP server, for validating the harness without the real editor.

Speaks the same command line as the editor:

    fake_server.py mcp [--project <dir|project.yaml>] [--create <dir> [--name N]] [--headless]
                       [--http-port N]

stdio by default; with --http-port it serves Streamable HTTP on 127.0.0.1:N/mcp instead.
With --headless and no DISPLAY it spawns `Xvfb -displayfd` like the editor and kills it at exit.
Writes chatter to stderr (never stdout), and a few tools mimic the editor's:
editor_status, create_entity, list_scene_entities, undo/redo, batch, screenshot, read_console.
"""

from __future__ import annotations

import argparse
import atexit
import base64
import contextlib
import ctypes
import os
import signal
import struct
import subprocess
import sys
import zlib
from pathlib import Path
from typing import Any

from mcp import types
from mcp.server.mcpserver import MCPServer
from mcp.server.mcpserver.exceptions import ToolError

TYPES = ("object", "empty", "box", "sphere", "plane", "sprite", "camera", "text")
INSTRUCTIONS = "Fake JasperEngine editor. Inspect, edit with tools, save, play, screenshot, read the console, fix."


def log(msg: str) -> None:
    print(f"[fake-editor] {msg}", file=sys.stderr, flush=True)


def tiny_png(w: int = 4, h: int = 3, rgb=(200, 40, 40)) -> bytes:
    def chunk(tag: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + bytes(rgb) * w for _ in range(h))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def die_with_parent() -> None:
    """Linux: the child gets SIGTERM when this process dies, even by SIGKILL."""
    with contextlib.suppress(Exception):
        ctypes.CDLL("libc.so.6", use_errno=True).prctl(1, signal.SIGTERM)  # PR_SET_PDEATHSIG


def start_xvfb() -> str | None:
    r, w = os.pipe()
    proc = subprocess.Popen(["Xvfb", "-displayfd", str(w), "-screen", "0", "1600x900x24", "-nolisten", "tcp"],
                            pass_fds=(w,), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            preexec_fn=die_with_parent)
    os.close(w)
    data = b""
    with os.fdopen(r, "rb") as f:
        while not data.endswith(b"\n"):
            b = f.read(1)
            if not b:
                break
            data += b
    if not data.strip():
        proc.kill()
        return None
    def stop(*_):
        if proc.poll() is None:
            proc.terminate()
            proc.wait()

    atexit.register(stop)
    for sig in (signal.SIGTERM, signal.SIGHUP):  # atexit does not run on a signal
        signal.signal(sig, lambda *_: (stop(), os._exit(0)))
    return ":" + data.decode().strip()


class State:
    def __init__(self, args: argparse.Namespace):
        self.args = args
        self.next_id = 100
        self.entities: dict[int, dict] = {1: {"id": 1, "name": "Main Camera", "type": "camera", "position": [0, 0, 0]}}
        self.undo: list[tuple[str, Any]] = []
        self.redo: list[tuple[str, Any]] = []
        self.console: list[dict] = [{"id": 1, "type": "info", "message": "Fake editor started"}]

    def say(self, kind: str, message: str) -> None:
        self.console.append({"id": len(self.console) + 1, "type": kind, "message": message})


def build(args: argparse.Namespace) -> MCPServer:
    st = State(args)
    server = MCPServer("jasperengine-editor-fake", instructions=INSTRUCTIONS, version="0.0-fake")

    def project_dir() -> str | None:
        p = args.project or args.create
        return str(Path(p).resolve()) if p else None

    @server.tool(description="Editor state: project, cwd, HOME, DISPLAY, settings.yaml, play mode.")
    def editor_status() -> dict:
        settings = Path("settings.yaml")
        return {
            "project": project_dir(), "cwd": os.getcwd(), "home": os.environ.get("HOME"),
            "display": os.environ.get("DISPLAY"), "libgl_software": os.environ.get("LIBGL_ALWAYS_SOFTWARE"),
            "settings_yaml": settings.read_text() if settings.exists() else None,
            "playing": False, "argv": sys.argv[1:],
        }

    def create(name: str, type: str = "object", parent_id: int | None = None,
               position: dict | list | None = None, scene_id: int | None = None) -> dict:
        if type not in TYPES:
            raise ToolError(f"Unsupported GameObject type {type!r}. Valid types: {', '.join(TYPES)}")
        if parent_id is not None and parent_id not in st.entities:
            raise ToolError(f"Parent GameObject {parent_id} not found. Valid ids: {sorted(st.entities)}")
        eid = st.next_id
        st.next_id += 1
        st.entities[eid] = {"id": eid, "name": name, "type": type, "parent": parent_id, "position": position}
        st.undo.append([eid])
        st.redo.clear()
        print(f"stdout noise from create_entity {name}", flush=True)  # must not reach the protocol: see main()
        st.say("info", f"Created {name}")
        return {"scene_id": scene_id or 1, "entity_id": eid, "path": name}

    @server.tool(description="Create a GameObject (legacy shape: scene_id?, name, type, parent_id?, position).")
    def create_entity(name: str, type: str = "object", parent_id: int | None = None,
                      position: dict | None = None, scene_id: int | None = None) -> dict:
        return create(name, type, parent_id, position, scene_id)

    @server.tool(description="List GameObjects of a scene.")
    def list_scene_entities(scene_id: int | None = None) -> dict:
        return {"scene_id": scene_id or 1, "entities": list(st.entities.values())}

    def do_undo() -> str:
        if not st.undo:
            raise ToolError("Nothing to undo in scene history.")
        ids = st.undo.pop()
        st.redo.append([st.entities.pop(eid) for eid in ids])
        return "Undid last scene command."

    @server.tool(description="Undo the last editor command.")
    def undo(scope: str = "scene", scene_id: int | None = None) -> str:
        return do_undo()

    @server.tool(description="Redo the last undone editor command.")
    def redo(scope: str = "scene", scene_id: int | None = None) -> str:
        if not st.redo:
            raise ToolError("Nothing to redo in scene history.")
        ents = st.redo.pop()
        for ent in ents:
            st.entities[ent["id"]] = ent
        st.undo.append([ent["id"] for ent in ents])
        return "Redid last scene command."

    @server.tool(description="Run several tool calls in one round trip: calls=[{tool, arguments}].")
    def batch(calls: list[dict], undo_group: bool = False, stop_on_error: bool = True) -> dict:
        impl = {"create_entity": create, "list_scene_entities": list_scene_entities, "undo": do_undo,
                "editor_status": editor_status}
        results = []
        depth = len(st.undo)
        for c in calls:
            fn = impl.get(c.get("tool"))
            try:
                if not fn:
                    raise ToolError(f"unknown tool {c.get('tool')}")
                results.append({"tool": c["tool"], "ok": True, "result": fn(**(c.get("arguments") or {}))})
            except Exception as e:  # noqa: BLE001
                results.append({"tool": c.get("tool"), "ok": False, "error": str(e)})
                if stop_on_error:
                    break
        if undo_group and len(st.undo) > depth + 1:  # merge this batch's edits into one undo step
            merged = [eid for step in st.undo[depth:] for eid in step]
            del st.undo[depth:]
            st.undo.append(merged)
        return {"results": results}

    @server.tool(description="Screenshot the Scene or Game view as a PNG image.")
    def screenshot(view: str = "game", width: int | None = None, height: int | None = None) -> types.CallToolResult:
        if view not in ("game", "scene"):
            return types.CallToolResult(is_error=True, content=[types.TextContent(
                type="text", text=f"Unknown view {view!r}; use game or scene.")])
        png = tiny_png()
        return types.CallToolResult(content=[
            types.ImageContent(type="image", data=base64.b64encode(png).decode(), mime_type="image/png"),
            types.TextContent(type="text", text=f"{view} view, 4x3"),
        ])

    @server.tool(description="Console entries after cursor `since` (an entry id).")
    def read_console(since: int | None = None, limit: int = 200) -> dict:
        items = [e for e in st.console if since is None or e["id"] > since][:limit]
        return {"entries": items, "next_cursor": items[-1]["id"] if items else since}

    @server.tool(description="Always fails, with a message that says how to fix the call.")
    def fail_always(reason: str = "because") -> str:
        raise ToolError(f"Failed {reason}. Valid choices: a, b, c.")

    return server


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["mcp"])
    ap.add_argument("--project")
    ap.add_argument("--create")
    ap.add_argument("--name", default="Project")
    ap.add_argument("--headless", action="store_true")
    ap.add_argument("--http-port", type=int)
    ap.add_argument("--mcp-port", type=int)
    args = ap.parse_args()

    if args.create:
        Path(args.create).mkdir(parents=True, exist_ok=True)
        (Path(args.create) / "project.yaml").write_text(f"name: {args.name}\n")
    if args.headless and not os.environ.get("DISPLAY"):
        disp = start_xvfb()
        if disp:
            os.environ["DISPLAY"] = disp
            log(f"started Xvfb on {disp}")

    server = build(args)
    port = args.http_port or args.mcp_port
    log(f"serving {'http :' + str(port) if port else 'stdio'}; project={args.project or args.create}")
    if port:
        import anyio
        anyio.run(lambda: server.run_streamable_http_async(port=port))
        return

    # Like the editor: keep the original stdout for the protocol and point fd 1 at stderr,
    # so stray prints (the one in create_entity) can't corrupt it.
    import io

    import anyio
    import mcp.server.stdio as sstdio

    proto = os.fdopen(os.dup(1), "wb", buffering=0)
    os.dup2(2, 1)
    if os.environ.get("FAKE_DIRTY_STDOUT"):  # harness self-test: a server that corrupts stdout
        proto.write(b"Loading project...\n")
    stdin = anyio.wrap_file(io.TextIOWrapper(os.fdopen(os.dup(0), "rb", buffering=0), encoding="utf-8"))
    stdout = anyio.wrap_file(io.TextIOWrapper(proto, encoding="utf-8", line_buffering=True))

    async def run() -> None:
        async with sstdio.stdio_server(stdin=stdin, stdout=stdout) as (r, w):
            low = server._lowlevel_server
            await low.run(r, w, low.create_initialization_options())

    anyio.run(run)

if __name__ == "__main__":
    main()
