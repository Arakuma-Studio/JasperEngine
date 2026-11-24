# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""The eval's MCP server: the editor's real tools/list and instructions, canned results.

    python3 stub_server.py --surface SURFACE.json --case CASE.json --log CALLS.jsonl

SURFACE.json is what dump_surface.py recorded from a built editor (every tool definition with
its group, the instructions), so a sample always sees the current text. CASE.json is one case
of cases.py (its mocks). Every call is appended to CALLS.jsonl. Nothing here touches an editor,
a project or the network.

Argument checks are the registry's (McpRegistry::addTool): an unknown top-level key fails with
the editor's own message; a missing required key fails like the handlers do ("<key> is
required."). tool_groups is answered from state, so enabling a group lists its tools
(notifications/tools/list_changed), as the editor does.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from grade import args_match as partial_match  # noqa: E402  (the checks' matcher: re:, *)

CORE = "core"


def group_of(tool: dict) -> str:
    return str((tool.get("_meta") or {}).get("jasper/group") or CORE)


class Stub:
    def __init__(self, surface: dict, case: dict, log_path: Path | None):
        self.tools = {t["name"]: t for t in surface["tools"]}
        self.order = [t["name"] for t in surface["tools"]]
        self.instructions = surface.get("instructions", "")
        self.enabled = set(surface.get("default_groups") or [CORE])
        self.enabled |= set(case.get("groups_enabled") or [])
        self.mocks = [dict(m) for m in case.get("mocks", [])]
        self.resources = surface.get("resources") or []
        self.templates = surface.get("resource_templates") or []
        self.contents = surface.get("resource_contents") or {}
        self.recorded = surface.get("recorded") or {}
        self.log_path = log_path
        self.calls = 0
        self.seen: list[dict] = []  # the calls that succeeded so far ({tool, args}), for mocks with "after"
        self.refused: list[str] = []  # the tools that returned an error, for mocks with "after_error"

    # -- listing ---------------------------------------------------------------------------
    def listed(self) -> list[dict]:
        return [self.tools[n] for n in self.order if group_of(self.tools[n]) in self.enabled]

    # -- calls -----------------------------------------------------------------------------
    def validate(self, name: str, args: dict) -> str | None:
        schema = self.tools[name].get("inputSchema") or {}
        props = schema.get("properties") or {}
        for key in args:
            if key not in props:
                valid = ", ".join(props) or "none"
                return f'Unknown argument "{key}" for {name}. Arguments: {valid}.'
        for key in schema.get("required") or []:
            if key not in args or args[key] is None:
                return f"{key} is required."
        return None

    def tool_groups(self, args: dict) -> dict:
        """The editor's tool_groups result (CoreTools.cpp): its text, with the recorded descriptions."""
        known = (self.recorded.get("tool_groups") or {}).get("structured") or {}
        info = {g["name"]: g for g in known.get("groups") or []}
        names = list(info) or sorted({group_of(t) for t in self.tools.values()})
        names += sorted({group_of(t) for t in self.tools.values()} - set(names))
        changed = False
        for name in list(args.get("enable") or []) + list(args.get("disable") or []):
            if name != "all" and name not in names:
                return error(f"Unknown tool group \"{name}\". Groups: {', '.join(names)}.")
        for name in args.get("enable") or []:
            add = set(names) if name == "all" else {name}
            changed |= not add <= self.enabled
            self.enabled |= add
        for name in args.get("disable") or []:
            if name != CORE:
                changed |= name in self.enabled
                self.enabled.discard(name)
        rows = []
        for g in names:
            tools = [n for n in self.order if group_of(self.tools[n]) == g]
            chars = sum(len(n) + len(self.tools[n].get("description") or "") +
                        len(json.dumps(self.tools[n].get("inputSchema") or {}, separators=(",", ":"))) for n in tools)
            rows.append({"name": g, "enabled": g in self.enabled, "tools": tools, "chars": chars,
                         "description": (info.get(g) or {}).get("description", "")})
        on = [r for r in rows if r["enabled"]]
        text = (f"tools/list shows {', '.join(r['name'] for r in on)} ({sum(len(r['tools']) for r in on)} tools).\n" +
                "\n".join(f"{r['name']}{' [listed]' if r['enabled'] else ''}: {len(r['tools'])} tools, {r['chars']} chars. "
                          f"{r['description']}" for r in rows))
        return {"content": [{"type": "text", "text": text}],
                "structuredContent": {"groups": rows, "changed": changed},
                "isError": False, "_changed": changed}

    def answer(self, name: str, args: dict) -> dict:
        for mock in self.mocks:
            if mock.get("tool") != name or mock.get("_used"):
                continue
            if mock.get("after_error") and mock["after_error"] not in self.refused:
                continue
            if mock.get("after") and not any(partial_match(after_spec(mock["after"]), c) for c in self.seen):
                continue
            if not partial_match(mock.get("args") or {}, args):
                continue
            if mock.get("once"):
                mock["_used"] = True
            return result(mock.get("result") or {})
        if name == "tool_groups":
            return self.tool_groups(args)
        if name == "read_file" and str(args.get("path", "")).startswith("jasper://"):
            return self.read_doc(str(args["path"]), args)
        return error(f"eval stub: no canned result for {name} with these arguments.", unmocked=True)

    def read_doc(self, uri: str, args: dict) -> dict:
        """read_file of a jasper:// doc, as AssetTools.cpp's readResourceFile answers it (lineWindow)."""
        if uri not in self.contents:
            return error(f"Resource not found: {uri} The documents: read_file jasper://docs.")
        lines = self.contents[uri].split("\n")
        if lines and lines[-1] == "":
            lines.pop()
        start = max(1, int(args.get("start_line") or 1))
        window = lines[start - 1:start - 1 + min(max(1, int(args.get("max_lines") or 2000)), 20000)]
        text = f"{uri} lines {start}-{start + len(window) - 1} of {len(lines)}:\n" + "".join(line + "\n" for line in window)
        return {"content": [{"type": "text", "text": text}], "isError": False}

    def call(self, name: str, args) -> dict:
        self.calls += 1
        if name not in self.tools:
            res = error(f"Unknown tool: {name}")
        elif not isinstance(args, dict):
            res = error("tools/call arguments must be an object.")
        else:
            problem = self.validate(name, args)
            res = error(problem) if problem else self.answer(name, args)
            if not res.get("isError"):
                self.seen.append({"tool": name, "args": args})
            else:
                self.refused.append(name)
        self.log({"n": self.calls, "tool": name, "args": args, "is_error": res.get("isError", False),
                  "unmocked": res.pop("_unmocked", False), "text": first_text(res)})
        return res

    def log(self, row: dict) -> None:
        if self.log_path:
            with self.log_path.open("a", encoding="utf-8") as f:
                f.write(json.dumps(row, ensure_ascii=False) + "\n")


def after_spec(after) -> dict:
    """A mock's "after": a tool name, or {tool, args} (args: a partial match, as the checks use)."""
    spec = {"tool": after} if isinstance(after, str) else after
    return {"tool": spec["tool"], "args": spec.get("args") or {}}


def result(spec: dict) -> dict:
    """A recorded result (or a hand-written one marked real: shaped like the editor's) goes out as the
    editor sent it, text and structuredContent; Claude Code then shows the model only the
    structuredContent. Any other hand-written result sends its text alone, so the model reads what
    the case states, with its structured `next` after it when the text lacks it: the editor's hint is
    in structuredContent, the part Claude Code shows."""
    text = spec.get("text", "")
    hint = (spec.get("structured") or {}).get("next") if isinstance(spec.get("structured"), dict) else None
    if not spec.get("real") and isinstance(hint, str) and hint and hint not in text:
        text = f"{text}\n{hint}" if text else hint
    out = {"content": [{"type": "text", "text": text}], "isError": bool(spec.get("is_error"))}
    if spec.get("real") and spec.get("structured") is not None:
        out["structuredContent"] = spec["structured"]
    return out


def error(text: str, unmocked: bool = False) -> dict:
    out = {"content": [{"type": "text", "text": text}], "isError": True}
    if unmocked:
        out["_unmocked"] = True
    return out


def first_text(res: dict) -> str:
    for item in res.get("content") or []:
        if item.get("type") == "text":
            return item.get("text", "")[:300]
    return ""


def serve(stub: Stub, inp=sys.stdin, out=sys.stdout) -> None:
    def send(msg: dict) -> None:
        out.write(json.dumps(msg, ensure_ascii=False) + "\n")
        out.flush()

    for line in inp:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except ValueError:
            continue
        method, mid, params = msg.get("method"), msg.get("id"), msg.get("params") or {}
        if mid is None:
            continue  # notifications
        if method == "initialize":
            send({"jsonrpc": "2.0", "id": mid, "result": {
                "protocolVersion": params.get("protocolVersion", "2025-06-18"),
                "capabilities": {"tools": {"listChanged": True}, "resources": {}},
                "serverInfo": {"name": "jasperengine-editor", "version": "eval"},
                "instructions": stub.instructions}})
        elif method == "tools/list":
            send({"jsonrpc": "2.0", "id": mid, "result": {"tools": stub.listed()}})
        elif method == "tools/call":
            res = stub.call(params.get("name", ""), params.get("arguments", {}))
            changed = res.pop("_changed", False)
            send({"jsonrpc": "2.0", "id": mid, "result": res})
            if changed:
                send({"jsonrpc": "2.0", "method": "notifications/tools/list_changed"})
        elif method == "ping":
            send({"jsonrpc": "2.0", "id": mid, "result": {}})
        elif method == "resources/list":
            send({"jsonrpc": "2.0", "id": mid, "result": {"resources": stub.resources}})
        elif method == "resources/templates/list":
            send({"jsonrpc": "2.0", "id": mid, "result": {"resourceTemplates": stub.templates}})
        elif method == "resources/read":
            uri = params.get("uri", "")
            stub.log({"n": 0, "resource": uri})
            if uri in stub.contents:
                send({"jsonrpc": "2.0", "id": mid, "result": {"contents": [
                    {"uri": uri, "mimeType": "text/markdown", "text": stub.contents[uri]}]}})
            else:
                send({"jsonrpc": "2.0", "id": mid, "error": {"code": -32602, "message": f"Unknown resource: {uri}"}})
        elif method == "prompts/list":
            send({"jsonrpc": "2.0", "id": mid, "result": {"prompts": []}})
        else:
            send({"jsonrpc": "2.0", "id": mid, "error": {"code": -32601, "message": f"Method not found: {method}"}})


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--surface", required=True)
    ap.add_argument("--case", required=True)
    ap.add_argument("--log")
    a = ap.parse_args()
    surface = json.loads(Path(a.surface).read_text(encoding="utf-8"))
    case = json.loads(Path(a.case).read_text(encoding="utf-8"))
    serve(Stub(surface, case, Path(a.log) if a.log else None))
    return 0


if __name__ == "__main__":
    sys.exit(main())
