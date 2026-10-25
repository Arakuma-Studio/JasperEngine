---
paths:
  - "engine/core/script/**"
  - "editor/**/Lua*"
---

# Lua scripting

- After changing a binding, regenerate the Lua stubs (`generate_lua_stubs.py`) and the editor's
  completion data (`generate_api_suggestions.py`); commit their output with the change.
- Bound getters are properties in Lua, not methods; keep names consistent with the existing
  bindings and with `docs/mcp/guide/lua-api.md`, which agents read.
- The Lua state belongs to the main thread: `JASPER_LUA_THREAD_CHECK=abort` (set by the MCP
  harness) aborts on a use from another thread.
- Test through `tools/mcp/tests/test_script.py` style MCP tests (play the scene and read the
  Console) and the unit tests of `engine/tests/core`.
