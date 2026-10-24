---
paths:
  - "editor/mcp/**"
  - "tools/mcp/**"
---

# MCP server and tools

The rule book is `docs/mcp/conventions.md`; keep these in mind on every change here:

- Tool names `verb_noun`, snake_case parameter keys, schemas from `editor/mcp/McpSchema.h`
  only (the keyword subset), closed objects (`test_tool_conventions.py`).
- A new tool goes into its spec's group (`editor/mcp/McpToolGroups.cpp`), not core, unless the
  spec moves it into core and trims core by as much (`core_trades` in `tools/mcp/budget.json`).
- Limits live only in `tools/mcp/budget.json`; check with
  `tools/mcp/tool_budget.py --check tools/mcp/budget.json`. Never raise a limit inside a test.
- Descriptions at most 400 characters; shared conventions go in the agent guide
  (`editor/mcp/McpInstructions.cpp`) or `docs/mcp/guide/`. The guide holds the workflow and one
  short digest per area (its limits: the `guide` entry of `tools/mcp/budget.json`); an area's
  detail goes in `docs/mcp/guide/<area>.md` (`jasper://docs/<area>`), which the digest names.
- A schema must not depend on project state: the CLI builds the registry offline
  (`buildToolRegistry`) and compares it with the live listing.
- Every tool has a test in `tools/mcp/tests/` (`test_tool_coverage.py`); regenerate
  `docs/mcp/TOOLS.md` with `tools/mcp/gen_tool_docs.py` (`--check` in `test_docs.py`).

Harness: `from harness import Editor`; `async with Editor(create=True) as ed`, then
`await ed.call("tool", arg=...)`. `ed.tools()` lists every group; `ed.tools(groups=None)` is what
a new session sees. `RawEditor` and `HttpSession` speak raw JSON-RPC over stdio and HTTP. New
test files declare `CI = True` and run on `EDITOR = {"create": True}` or a `tools/fixtures` project.
