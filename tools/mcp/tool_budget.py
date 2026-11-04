"""Measure the size of the editor's MCP tool definitions (what an agent pays per session), and
gate it against the limits of tools/mcp/budget.json.

    uv run --no-project --with-requirements tools/mcp/requirements.txt python tools/mcp/tool_budget.py
        [--top N] [--json FILE] [--url URL] [--project DIR | --fixture] [--full]
        [--groups default|all|a,b] [--check tools/mcp/budget.json]

Starts the editor through the harness (a new empty project by default), lists the tools and
prints, per tool sorted by size, its group and the characters of name + description +
json(inputSchema) (compact JSON), then the total. --full also counts title, outputSchema and
annotations (the whole tools/list entry). ~4 characters is roughly one token.

--groups picks the listing: default (what a new session lists: core, plus the groups enabled by
default), all (every registered tool), or a comma-separated list of groups.

--check BUDGET lists every group and exits 1 naming each violation of the budget file: the
default listing (core) over its characters or tool count, a group over its limits, a tool over
the per-tool limit (or its exceptions entry), an exceptions/groups entry naming a tool or group
that does not exist. It also gates the agent guide (initialize.instructions) against the
budget's "guide" entry: the whole text, the headroom it must leave under its limit, each
"## " section, and the client cut (the sections that must come before it, and the pointer). docs/mcp/conventions.md explains the rules.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from harness import Editor, tool_meta  # noqa: E402

BUDGET_FILE = HERE / "budget.json"
CORE = "core"


def compact(value) -> str:
    return json.dumps(value, separators=(",", ":"), ensure_ascii=False)


def group_of(tool) -> str:
    """The tool's group from tools/list _meta["jasper/group"] (core when absent)."""
    return str(tool_meta(tool).get("jasper/group") or CORE)


def measure(tool, full: bool = False) -> dict:
    desc = tool.description or ""
    schema = compact(tool.input_schema or {})
    row = {"name": tool.name, "group": group_of(tool), "desc": len(desc), "schema": len(schema)}
    row["total"] = len(tool.name) + row["desc"] + row["schema"]
    if full:
        entry = tool.model_dump(mode="json", by_alias=True, exclude_none=True)
        row["full"] = len(compact(entry))
    return row


def load_budget(path: str | Path = BUDGET_FILE) -> dict:
    return json.loads(Path(path).read_text(encoding="utf-8"))


def tool_limit(budget: dict, name: str) -> int:
    entry = budget.get("exceptions", {}).get(name)
    return int(entry["limit"]) if entry else int(budget["tool_default"])


def group_limits(budget: dict, group: str) -> tuple[int, int]:
    entry = budget.get("groups", {}).get(group) or budget["group_default"]
    default = budget["group_default"]
    return int(entry.get("chars", default["chars"])), int(entry.get("tools", default["tools"]))


def check(rows: list[dict], budget: dict, known_groups: set[str] | None = None) -> list[str]:
    """Every violation of the budget by these rows (all groups listed), as readable lines."""
    problems: list[str] = []
    by_group: dict[str, list[dict]] = {}
    for r in rows:
        by_group.setdefault(r["group"], []).append(r)

    core = by_group.get(CORE, [])
    listing = budget["default_listing"]
    chars = sum(r["total"] for r in core)
    if chars > listing["chars"]:
        problems.append(f"default listing (core): {chars} chars, over {listing['chars']}: trim core tools, or put "
                        f"new tools in a group (docs/mcp/conventions.md)")
    if len(core) > listing["tools"]:
        problems.append(f"default listing (core): {len(core)} tools, over {listing['tools']}")

    for group, members in sorted(by_group.items()):
        if group == CORE:
            continue
        limit_chars, limit_tools = group_limits(budget, group)
        chars = sum(r["total"] for r in members)
        if chars > limit_chars:
            problems.append(f"group {group}: {chars} chars, over {limit_chars}")
        if len(members) > limit_tools:
            problems.append(f"group {group}: {len(members)} tools, over {limit_tools}")

    names = {r["name"] for r in rows}
    for r in rows:
        limit = tool_limit(budget, r["name"])
        if r["total"] > limit:
            problems.append(f"tool {r['name']} ({r['group']}): {r['total']} chars, over its limit {limit}")
    for name, entry in budget.get("exceptions", {}).items():
        if name not in names:
            problems.append(f"exceptions entry {name}: no such tool (remove the entry)")
        if not str(entry.get("reason", "")).strip():
            problems.append(f"exceptions entry {name}: no reason")
    if known_groups is not None:
        for group in budget.get("groups", {}):
            if group not in known_groups:
                problems.append(f"groups entry {group}: no such tool group (editor/mcp/McpToolGroups.cpp)")
        for group in by_group:
            if group not in known_groups:
                problems.append(f"group {group}: tools are registered into a group the group table lacks")
    for trade in budget.get("core_trades", []):
        tool = trade.get("tool")
        if tool not in names:
            problems.append(f"core_trades entry {tool}: no such tool")
        elif next(r for r in rows if r["name"] == tool)["group"] != CORE:
            problems.append(f"core_trades entry {tool}: the tool is not in core")
        if not trade.get("spec") or not trade.get("trimmed"):
            problems.append(f"core_trades entry {tool}: needs spec and trimmed (what core gave up for it)")
    return problems


def guide_sections(text: str) -> list[tuple[str, int]]:
    """The guide's "## " sections as (heading, UTF-8 bytes); the text before the first is "(intro)"."""
    sections: list[tuple[str, int]] = []
    heading, start = "(intro)", 0
    lines = text.splitlines(keepends=True)
    offset = 0
    for line in lines:
        if line.startswith("## "):
            if offset > start:
                sections.append((heading, len(text[start:offset].encode())))
            heading, start = line[3:].strip(), offset
        offset += len(line)
    sections.append((heading, len(text[start:].encode())))
    return sections


def check_guide(text: str, budget: dict) -> list[str]:
    """Every violation of the budget's "guide" entry by this guide text."""
    entry = budget["guide"]
    problems: list[str] = []
    size = len(text.encode())
    limit, headroom = int(entry["bytes"]), int(entry.get("headroom_bytes", 0))
    if size > limit:
        problems.append(f"agent guide: {size} bytes, over {limit}: move area detail into "
                        f"docs/mcp/guide/<area>.md and keep a digest (docs/mcp/conventions.md)")
    elif limit - size < headroom:
        problems.append(f"agent guide: {size} bytes leaves {limit - size} of the {headroom} bytes of headroom "
                        f"under {limit} that later specs need: move area detail into docs/mcp/guide/<area>.md "
                        f"(jasper://docs/<area>) and keep a one-line pointer; never shrink the headroom or "
                        f"raise the limit (docs/mcp/conventions.md)")
    problems += check_client_cut(text, entry)
    own = entry.get("sections", {})
    present = set()
    for heading, nbytes in guide_sections(text):
        present.add(heading)
        limit = int(own[heading]["limit"]) if heading in own else int(entry["section_bytes"])
        if nbytes > limit:
            problems.append(f"agent guide section {heading!r}: {nbytes} bytes, over {limit}: move its detail "
                            f"into jasper://docs/<area> (docs/mcp/guide/)")
    for heading, value in own.items():
        if heading not in present:
            problems.append(f"guide sections entry {heading!r}: no such section (remove the entry)")
        if not str(value.get("reason", "")).strip():
            problems.append(f"guide sections entry {heading!r}: no reason")
    return problems


def utf16_len(text: str) -> int:
    """Length as JavaScript clients count it (UTF-16 code units)."""
    return len(text.encode("utf-16-le")) // 2


def section_ends(text: str) -> dict[str, int]:
    """Each "## " section's end, in UTF-16 code units from the start (the intro is "(intro)")."""
    ends: dict[str, int] = {}
    heading, offset = "(intro)", 0
    for line in text.splitlines(keepends=True):
        if line.startswith("## "):
            ends[heading] = utf16_len(text[:offset])
            heading = line[3:].strip()
        offset += len(line)
    ends[heading] = utf16_len(text)
    return ends


def check_client_cut(text: str, entry: dict) -> list[str]:
    """The guide's "client_cut": clients that keep only the first chars of the instructions (Claude
    Code: 4096) must still get the listed sections whole and the pointer to the rest."""
    cut = entry.get("client_cut")
    if cut is None:
        return []   # budget.json has one (test_tool_budget.py test_budget_file_shape); made-up budgets may not
    missing = [k for k in ("chars", "sections", "pointer", "reason") if not cut.get(k)]
    if missing:
        return [f"guide client_cut lacks {', '.join(missing)}"]
    problems: list[str] = []
    chars = int(cut["chars"])
    ends = section_ends(text)
    for heading in cut["sections"]:
        if heading not in ends:
            problems.append(f"guide client_cut section {heading!r}: no such section")
        elif ends[heading] > chars:
            problems.append(f"agent guide section {heading!r} ends at character {ends[heading]}, after the "
                            f"{chars}-character client cut: put it before the cut or move detail to its doc")
    intro_end = ends.get("(intro)", 0)
    if cut["pointer"] not in text[:intro_end]:
        problems.append(f"agent guide intro lacks the pointer {cut['pointer']!r} for clients that cut it")
    return problems


def print_guide(text: str, budget: dict) -> None:
    sections = sorted(guide_sections(text), key=lambda s: s[1], reverse=True)
    entry = budget["guide"]
    size, limit = len(text.encode()), int(entry["bytes"])
    print(f"guide: {size} of {limit} bytes, {limit - size} left (headroom {entry.get('headroom_bytes', 0)} "
          f"required), {len(sections)} sections; largest: " +
          ", ".join(f"{h} {n}" for h, n in sections[:5]))
    cut = entry.get("client_cut")
    if cut:
        inside = [h for h, end in section_ends(text).items() if end <= int(cut["chars"])]
        print(f"client cut at {cut['chars']} characters keeps: " + ", ".join(inside))


async def known_tool_groups(ed: Editor) -> set[str]:
    """The group table, as the tool_groups tool reports it."""
    result = await ed.call("tool_groups")
    return {g["name"] for g in result["groups"]}


def print_rows(rows: list[dict], top: int, full: bool) -> None:
    shown = rows[:top] if top else rows
    header = f"{'tool':40} {'group':16} {'desc':>6} {'schema':>7} {'total':>7}" + (f" {'full':>7}" if full else "")
    print(header)
    for r in shown:
        line = f"{r['name']:40} {r['group']:16} {r['desc']:6d} {r['schema']:7d} {r['total']:7d}"
        if full:
            line += f" {r['full']:7d}"
        print(line)
    total = sum(r["total"] for r in rows)
    desc = sum(r["desc"] for r in rows)
    schema = sum(r["schema"] for r in rows)
    print("-" * len(header))
    summary = f"{len(rows)} tools: total {total} chars (~{total // 4} tokens); descriptions {desc}, schemas {schema}"
    if full:
        summary += f"; full tools/list entries {sum(r['full'] for r in rows)}"
    print(summary)
    groups: dict[str, list[dict]] = {}
    for r in rows:
        groups.setdefault(r["group"], []).append(r)
    if len(groups) > 1 or CORE not in groups:
        print("groups: " + ", ".join(f"{g} {len(m)} tools {sum(r['total'] for r in m)} chars"
                                     for g, m in sorted(groups.items())))


async def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--top", type=int, default=0, help="only print the N largest tools")
    ap.add_argument("--json", help="also write the rows as JSON to this file")
    ap.add_argument("--url", help="read tools/list from a running editor over HTTP")
    ap.add_argument("--project", help="copy of this project instead of a new empty one")
    ap.add_argument("--fixture", action="store_true", help="use the default fixture project")
    ap.add_argument("--full", action="store_true", help="also measure the whole tools/list entry")
    ap.add_argument("--groups", default="default", help="default | all | a,b (the listing to measure)")
    ap.add_argument("--check", metavar="BUDGET", help="gate against this budget file (lists every group)")
    args = ap.parse_args()

    groups: str | list[str] | None
    if args.check or args.groups == "all":
        groups = "all"
    elif args.groups == "default":
        groups = None
    else:
        groups = [g.strip() for g in args.groups.split(",") if g.strip()]

    if args.url:
        ed = Editor(url=args.url)
    elif args.project or args.fixture:
        ed = Editor(project=args.project)
    else:
        ed = Editor(create=True, name="ToolBudget")
    async with ed:
        tools = await ed.tools(groups=groups)
        known = await known_tool_groups(ed) if args.check else None
        guide = ed.instructions

    rows = sorted((measure(t, args.full) for t in tools), key=lambda r: r["total"], reverse=True)
    print_rows(rows, args.top, args.full)
    if args.json:
        Path(args.json).write_text(json.dumps(rows, indent=1))
    if args.check:
        budget = load_budget(args.check)
        print_guide(guide, budget)
        problems = check(rows, budget, known) + check_guide(guide, budget)
        for p in problems:
            print(f"BUDGET: {p}")
        if problems:
            print(f"{len(problems)} budget violation(s) of {args.check}")
            return 1
        print(f"within {args.check}")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
