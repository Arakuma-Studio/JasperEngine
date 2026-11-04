---@meta _
-- Hand-written shapes of the audio mixer's option tables (engine/core/script/binding/ECSClassesLua.cpp
-- and GameObjectClassesLua.cpp, feature audio-mixer), appended to the generated stubs. Rules:
-- docs/mcp/guide/audio.md.

---@class AudioTimeOptions
---@field unscaled? boolean # unscaled time (Unity's UnscaledTime): goes on at Engine.timeScale 0. Default: the mixer's updateMode (scaled)

---@class AudioFadeOutOptions
---@field pause? boolean # pause at the end instead of stopping
---@field unscaled? boolean # unscaled time (default scaled)

---@class AudioPlayOptions
---@field target? GameObject # 3D, follows it; stops when it is destroyed
---@field position? Vector3 # 3D, at a fixed place
---@field scene? Scene # the owner: it holds and stops with it. Default the target's scene, else the main scene
---@field volume? number # linear, as AudioSource.volume (default 1)
---@field pitch? number # relative speed, as AudioSource.pitch (default 1)
---@field group? string # a mixer group name, as AudioSource.outputGroup. Default the event's, else Master
---@field delay? number # seconds before it starts (default 0)
---@field unscaled? boolean # the delay runs on unscaled time (default false)
