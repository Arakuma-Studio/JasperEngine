---
paths:
  - "engine/core/script/**"
  - "editor/**/Lua*"
---

# Lua scripting

- The Lua stubs, the editor's completion data and lua-api.json are build outputs of one parser
  (`tools/lua_api`, run by CMake when a binding changes); nothing generated is committed. After
  changing a binding, run its tests (`tools/lua_api/tests/test_lua_api.py`, the docs' Lua blocks).
- Bound getters are properties in Lua, not methods; keep names consistent with the existing
  bindings and with `docs/mcp/guide/lua-api.md`, which agents read.
- The Lua state belongs to the main thread: `JASPER_LUA_THREAD_CHECK=abort` (set by the MCP
  harness) aborts on a use from another thread.
- Test through `tools/mcp/tests/test_script.py` style MCP tests (play the scene and read the
  Console) and the unit tests of `engine/tests/core`.
