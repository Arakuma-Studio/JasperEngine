---@meta _
-- Hand-written: Performance is bound with the Lua C API (engine/core/script/binding/ProfilerClassesLua.cpp),
-- which the generator does not read.

---Seconds left out of the window, per cause
---@class PerformanceExcluded
---@field paused number # the app paused or hidden
---@field resume number # the 2 s after a resume, a focus regain or the window visible again
---@field loading number # scenes loading, and the second after a scene load
---@field stall number # a frame of 1 s or more (a breakpoint, the background)
---@field game number # sampling turned off by the game
---@field reasons table<string, number> # the game's reasons (setSampling(false, reason)); past 8 names, "other"

---@class PerformanceFps
---@field avg number # frames / seconds
---@field p50 number # 1000 / frameMs.p50
---@field p10 number # 1000 / frameMs.p90: the rate 90 % of frames reach

---@class PerformanceFrameMs
---@field p50 number
---@field p90 number
---@field p99 number
---@field max number

---@class PerformanceCpuMs
---@field avg number # the frame's CPU work (presenting excluded), waits included
---@field p90 number

---@class PerformanceGpuMs
---@field avg number
---@field frames integer # frames that had a GPU time (only while something times the GPU)

---@class PerformanceThermal
---@field nominal number # seconds in each ThermalState
---@field fair number
---@field serious number
---@field critical number

---@class PerformanceHistogram
---@field edgesMs number[] # each bin's lower edge
---@field counts integer[]

---The current window of gameplay frames (Performance.getSummary, Performance.onSample). With no
---committed second only seconds, frames and excluded are set.
---@class PerformanceSummary
---@field seconds number # committed gameplay seconds
---@field frames integer
---@field excluded PerformanceExcluded
---@field fps? PerformanceFps
---@field frameMs? PerformanceFrameMs
---@field cpuMs? PerformanceCpuMs
---@field mainThreadCpuMs? number # the frame thread's CPU per frame (its thread clock); nil on the web
---@field mainThreadCpu? string # "thread_clock" or "unavailable"
---@field gpuMs? PerformanceGpuMs
---@field hitches? integer # frames of 250 ms to 1 s
---@field thermal? PerformanceThermal
---@field tier? DeviceTier # the tier in effect at the window's end
---@field tierChanges? integer
---@field gpu? string
---@field driver? string
---@field quirks? string[] # the active GPU quirks
---@field backend? string
---@field platform? string
---@field version? string
---@field buildId? integer
---@field histogram? PerformanceHistogram # with getSummary{histogram = true}

---The gameplay frame sampler of every build, for the game's own analytics (the engine sends
---nothing; ask the player before you do). Off by default: one branch per frame.
---@class Performance
Performance = {}

---Starts (true) or stops sampling; off, reason names the excluded time in summary.excluded.reasons
---@param on boolean
---@param reason? string
function Performance.setSampling(on, reason) end

---@return boolean
function Performance.isSampling() end

---The current window, without resetting it
---@param options? {histogram?: boolean}
---@return PerformanceSummary
function Performance.getSummary(options) end

---Starts a new window now (sampling stays as it is)
function Performance.reset() end

---Calls fn(summary) each time the window reaches `seconds` of gameplay (30 to 3600, default 300),
---then starts a new one; onSample(nil) removes it. Cleared when Play stops.
---@param fn fun(summary: PerformanceSummary)|nil
---@param seconds? integer
function Performance.onSample(fn, seconds) end
