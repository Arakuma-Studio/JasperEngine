---@meta _
-- Hand-written: Profiler is bound with the Lua C API (engine/core/script/binding/ProfilerClassesLua.cpp),
-- which the generator does not read.

---The last frame's statistics (Profiler.frameStats)
---@class ProfilerFrameStats
---@field drawCalls integer
---@field setPassCalls integer # pipelines applied (Unity's SetPass calls)
---@field instances integer
---@field triangles integer
---@field cpuMs number # the frame's CPU work, presenting and waiting excluded
---@field fps number
---@field gpuMs? number # the game frame's GPU time, while GPU timing runs (it arrives a few frames late)

---The CPU profiler (Window > Analysis > Profiler). Samples are scopes in the profiler's CPU
---Usage and Hierarchy; in a release game (no profiler) every function does nothing, enabled
---stays false, and frameStats still works.
---@class Profiler
---@field enabled boolean # whether the profiler records (setting it starts or stops recording)
Profiler = {}

---Opens a sample; close it with Profiler.endSample (samples nest)
---@param name string
function Profiler.beginSample(name) end

---Closes the innermost sample; one without a beginSample is ignored (logged once)
function Profiler.endSample() end

---Calls fn(...) inside a sample named name and returns what fn returns (an error in fn is rethrown)
---@param name string
---@param fn function
---@param ... any
---@return any ...
function Profiler.sample(name, fn, ...) end

---Sets a monitor's value for this frame (the Profiler's Monitors module, get_frame_times monitors)
---@param name string
---@param value number
function Profiler.counter(name, value) end

---The last frame's draw calls, instances, triangles, CPU ms, fps and (while timed) GPU ms
---@return ProfilerFrameStats
function Profiler.frameStats() end
