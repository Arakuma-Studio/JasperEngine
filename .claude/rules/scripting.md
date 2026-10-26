---
paths:
  - "engine/core/script/**"
  - "editor/**/Lua*"
---

# Lua scripting

- After changing a binding, rebuild: `tools/lua_api` (one parser, `docs/scripting.md`) regenerates
  the Lua API data (lua-api.json), the Lua stubs and the editor's completion data; commit their output with the change.
- Bound getters are properties in Lua, not methods; keep names consistent with the existing
  bindings and with `docs/mcp/guide/lua-api.md`, which agents read.
- The Lua state belongs to the main thread: `JASPER_LUA_THREAD_CHECK=abort` (set by the MCP
  harness) aborts on a use from another thread.
- Test through `tools/mcp/tests/test_script.py` style MCP tests (play the scene and read the
  Console) and the unit tests of `engine/tests/core`.
