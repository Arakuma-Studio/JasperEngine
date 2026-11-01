---@meta _
-- Hand-written shapes of the native bridge (engine/core/script/binding/NativeClassesLua.cpp,
-- feature native-bridge), appended to the generated stubs. The guide: docs/mcp/guide/native-bridge.md.

---Error codes of Native.call / Native.callAsync (errors are values).
---@alias NativeErrorCode
---| "not_found" # no C++ plugin, platform plugin or Lua mock of that name
---| "no_method" # the plugin has no such method
---| "bad_args" # an argument cannot convert (or a wrong count)
---| "exception" # native code threw (the message has its class and text)
---| "unsupported_type" # native code returned something JSON cannot hold
---| "invalid_utf8" # a string argument is not valid UTF-8
---| "cancelled" # call:cancel(), its owner went or Play stopped
---| "timeout" # options.timeout passed
---| "async_only" # an asynchronous method (or a JavaScript Promise) called with Native.call
---| "not_ready" # Android: the activity is not created yet
---| "unavailable" # the plugin exists for other platforms only ("Java/Kotlin plugins run only on Android")

---Options of Native.callAsync (all optional).
---@class NativeCallOptions
---@field thread? "main"|"background" # where the native method runs: the UI/main thread (default) or a worker; the callback always runs on the game's main thread
---@field owner? ScriptInstance|EntityHandle|Scene # the call ends silently (no callback; native sees isCancelled) when its owner goes: `owner = self` in per-scene scripts
---@field timeout? number # seconds (real time) before the call fails with "timeout" (0: none, the default)

---What an asynchronous Lua mock returns to complete its call later (Native.pending()).
---@class NativePending
