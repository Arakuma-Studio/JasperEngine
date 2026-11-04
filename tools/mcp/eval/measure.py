# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""Model-visible size of a recorded surface (dump_surface.py), per listing and per tool.

    python3 tools/mcp/eval/measure.py SURFACE_DIR/surface.json [--top 10] [--json]

What reaches the model: each tool's name, description and compact JSON inputSchema (title,
annotations, outputSchema and _meta stay in the client), plus the instructions. Two client
shapes: "full" (Codex and clients that send every schema on every request) and "deferred"
(Claude Code's tool search: the instructions and the prefixed tool names until a tool is
loaded, then that tool's full definition). Tokens are bytes / 4, an approximation.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

CORE = "core"
PREFIX = "mcp__jasper__"


def group_of(tool: dict) -> str:
    return str((tool.get("_meta") or {}).get("jasper/group") or CORE)


def tool_bytes(tool: dict) -> int:
    schema = json.dumps(tool.get("inputSchema") or {}, separators=(",", ":"), ensure_ascii=False)
    return len((tool["name"] + (tool.get("description") or "") + schema).encode("utf-8"))


def listing(surface: dict, groups: str) -> list[dict]:
    if groups == "all":
        return surface["tools"]
    enabled = set(surface.get("default_groups") or [CORE])
    return [t for t in surface["tools"] if group_of(t) in enabled]


def sizes(surface: dict) -> dict:
    instructions = len((surface.get("instructions") or "").encode("utf-8"))
    out = {"instructions_bytes": instructions, "listings": {}}
    for name in ("default", "all"):
        tools = listing(surface, name)
        full = sum(tool_bytes(t) for t in tools) + instructions
        deferred = sum(len((PREFIX + t["name"]).encode("utf-8")) + 1 for t in tools) + instructions
        out["listings"][name] = {"tools": len(tools), "full_bytes": full, "deferred_bytes": deferred,
                                 "full_tokens": full // 4, "deferred_tokens": deferred // 4}
    out["per_tool"] = sorted(({"name": t["name"], "group": group_of(t), "bytes": tool_bytes(t)}
                              for t in surface["tools"]), key=lambda r: -r["bytes"])
    return out


def report(surface: dict, top: int = 0) -> str:
    s = sizes(surface)
    lines = [f"instructions: {s['instructions_bytes']} bytes (~{s['instructions_bytes'] // 4} tokens)"]
    if surface.get("project_agents_md_bytes"):
        b = surface["project_agents_md_bytes"]
        lines.append(f"a new game's AGENTS.md (CLAUDE.md imports it): {b} bytes (~{b // 4} tokens)")
    for name, row in s["listings"].items():
        lines.append(f"{name:8s} {row['tools']:4d} tools  full {row['full_bytes']:7d} bytes (~{row['full_tokens']} tok)"
                     f"  deferred {row['deferred_bytes']:6d} bytes (~{row['deferred_tokens']} tok)")
    rows = s["per_tool"][:top] if top else s["per_tool"]
    lines += [f"  {r['bytes']:6d}  {r['group']:16s} {r['name']}" for r in rows]
    return "\n".join(lines) + "\n"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("surface", type=Path)
    ap.add_argument("--top", type=int, default=10)
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()
    surface = json.loads(a.surface.read_text(encoding="utf-8"))
    print(json.dumps(sizes(surface), indent=1) if a.json else report(surface, a.top), end="")
    return 0


if __name__ == "__main__":
    sys.exit(main())
