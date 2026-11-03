---@meta _
-- Hand-written: Crash is bound with the Lua C API (engine/core/script/binding/CrashClassesLua.cpp),
-- which the generator does not read.

---A report an earlier session left: a crash, a hang, or an exit without either
---@class CrashReport
---@field id string # the report folder's name
---@field kind "crash"|"hang"|"abnormal_exit"
---@field reason string # "SIGSEGV (invalid memory access)", "not responding (Loading)" ...
---@field time string # UTC, ISO 8601
---@field version string
---@field product string
---@field handled boolean
---@field files string[] # crash.txt, log-tail.txt, crash.dmp, hang-001.txt ... (read them with the file API)

---Crash and hang reports of earlier sessions (docs/crash-diagnostics.md). The engine never sends a
---report anywhere: a hook decides, and should ask the player first (reports hold paths and log lines).
---@class Crash
Crash = {}

---Calls fn once per pending report of an earlier session, now; fn returning true marks it handled
---@param fn fun(report: CrashReport): boolean
function Crash.setReportHook(fn) end

---The reports of earlier sessions nobody handled yet, newest first
---@return CrashReport[]
function Crash.getPendingReports() end

---Marks a report handled (it is not offered again); false for an unknown id
---@param id string
---@return boolean
function Crash.markHandled(id) end

---Whether the previous session ended without a clean shutdown
---@return boolean
function Crash.lastSessionEndedAbnormally() end

---A key: value line written into this session's crash.txt (16 tags; "" removes one)
---@param key string
---@param value string
function Crash.setTag(key, value) end
