---
paths:
  - "engine/core/script/**"
  - "editor/**/Lua*"
---

# Lua scripting

- After changing a binding, rebuild (`docs/scripting.md`) and run
  `tools/lua_api/tests/test_lua_api.py`, which also runs the doc test on the docs' Lua blocks.
- Bound getters are properties in Lua, not methods; keep names consistent with the existing
  bindings and with `docs/mcp/guide/lua-api.md`, which agents read.
- The Lua state belongs to the main thread: `JASPER_LUA_THREAD_CHECK=abort` (set by the MCP
  harness) aborts on a use from another thread.
- Test through MCP tests that play the scene and read the Console
  (`tools/mcp/tests/test_script_reload.py`) and the unit tests of `engine/tests/core`.
