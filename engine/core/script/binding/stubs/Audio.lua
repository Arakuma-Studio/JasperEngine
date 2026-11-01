---@meta _
-- Hand-written shapes of the audio mixer's option tables (engine/core/script/binding/ECSClassesLua.cpp
-- and GameObjectClassesLua.cpp, feature audio-mixer), appended to the generated stubs. Rules:
-- docs/mcp/guide/audio.md.

---@class AudioTimeOptions
---@field unscaled? boolean # unscaled time (Unity's UnscaledTime): goes on at Engine.timeScale 0. Default: the mixer's updateMode (scaled)

---@class AudioFadeOutOptions
---@field pause? boolean # pause at the end instead of stopping
---@field unscaled? boolean # unscaled time (default scaled)
