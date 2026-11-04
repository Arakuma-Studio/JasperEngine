---
paths:
  - "editor/mcp/**"
  - "tools/mcp/**"
---

# MCP server and tools

Harness: `from harness import Editor`; `async with Editor(create=True) as ed`, then
`await ed.call("tool", arg=...)`. `ed.tools()` lists every group; `ed.tools(groups=None)` is what
a new session sees. `RawEditor` and `HttpSession` speak raw JSON-RPC over stdio and HTTP. New
test files declare `CI = True` and run on `EDITOR = {"create": True}` or a `tools/fixtures` project.
