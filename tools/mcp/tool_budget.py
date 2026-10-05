"""Measure the size of the editor's MCP tool definitions (what an agent pays per session).

    uv run --no-project --with mcp --with pyyaml python tools/mcp/tool_budget.py
        [--top N] [--json FILE] [--url URL] [--project DIR | --fixture] [--full]

Starts the editor through the harness (a new empty project by default), lists every tool and
prints, per tool sorted by size, the characters of name + description + json(inputSchema)
(compact JSON), then the total. --full also counts title, outputSchema and annotations (the
whole tools/list entry). ~4 characters is roughly one token.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from harness import Editor  # noqa: E402


def compact(value) -> str:
    return json.dumps(value, separators=(",", ":"), ensure_ascii=False)


def measure(tool, full: bool) -> dict:
    desc = tool.description or ""
    schema = compact(tool.input_schema or {})
    row = {"name": tool.name, "desc": len(desc), "schema": len(schema)}
    row["total"] = len(tool.name) + row["desc"] + row["schema"]
    if full:
        entry = tool.model_dump(mode="json", by_alias=True, exclude_none=True)
        row["full"] = len(compact(entry))
    return row


async def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--top", type=int, default=0, help="only print the N largest tools")
    ap.add_argument("--json", help="also write the rows as JSON to this file")
    ap.add_argument("--url", help="read tools/list from a running editor over HTTP")
    ap.add_argument("--project", help="copy of this project instead of a new empty one")
    ap.add_argument("--fixture", action="store_true", help="use the default fixture project")
    ap.add_argument("--full", action="store_true", help="also measure the whole tools/list entry")
    args = ap.parse_args()

    if args.url:
        ed = Editor(url=args.url)
    elif args.project or args.fixture:
        ed = Editor(project=args.project)
    else:
        ed = Editor(create=True, name="ToolBudget")
    async with ed:
        tools = await ed.tools()

    rows = sorted((measure(t, args.full) for t in tools), key=lambda r: r["total"], reverse=True)
    shown = rows[: args.top] if args.top else rows
    header = f"{'tool':40} {'desc':>6} {'schema':>7} {'total':>7}" + (f" {'full':>7}" if args.full else "")
    print(header)
    for r in shown:
        line = f"{r['name']:40} {r['desc']:6d} {r['schema']:7d} {r['total']:7d}"
        if args.full:
            line += f" {r['full']:7d}"
        print(line)
    total = sum(r["total"] for r in rows)
    desc = sum(r["desc"] for r in rows)
    schema = sum(r["schema"] for r in rows)
    print("-" * len(header))
    summary = f"{len(rows)} tools: total {total} chars (~{total // 4} tokens); descriptions {desc}, schemas {schema}"
    if args.full:
        summary += f"; full tools/list entries {sum(r['full'] for r in rows)}"
    print(summary)
    if args.json:
        Path(args.json).write_text(json.dumps(rows, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
