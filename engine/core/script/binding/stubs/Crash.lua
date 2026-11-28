---@meta _
-- Hand-written: Crash is bound with the Lua C API (engine/core/script/binding/CrashClassesLua.cpp),
-- which the generator does not read.

---A report an earlier session left: a crash, a hang, or an exit without either
---@class CrashReport
---@field id string # the report folder's name
---@field kind "crash"|"hang"|"abnormal_exit"|"user"
---@field reason string # "SIGSEGV (invalid memory access)", "not responding (Loading)" ...
---@field time string # UTC, ISO 8601
---@field version string
---@field product string
---@field handled boolean
---@field files string[] # crash.txt, log-tail.txt, journal.txt, crash.dmp, hang-001.txt, user.txt, screenshot.jpg ... (Crash.readReportFile reads them)
---@field category? string # a user report's category

---What Crash.createReport writes
---@class CrashReportOptions
---@field note string # what the player wrote (required, at most 4000 bytes)
---@field category? string # [a-z0-9_]{1,32}, "other" by default
---@field fields? table<string, string|number|boolean> # at most 16 field.<key> lines in user.txt
---@field screenshot? boolean # the frame Crash.beginReport captured (true)
---@field logTail? boolean # log-tail.txt (Project Settings > Player > Crash Handling > Include Log Tail)
---@field done? fun(report: CrashReport|nil, err: string|nil) # write on a job; called on the main thread after

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

---"Report a Problem": captures the frame being drawn; fn(ok, err) runs the first frame after, where the game
---opens its own form (so the form is not in the image). ok: the draft holds a screenshot.
---@param fn? fun(ok: boolean, err: string|nil)
---@param options? {maxWidth: integer?, maxBytes: integer?} # the JPEG's cap: 1280 px wide, 600 KiB
function Crash.beginReport(fn, options) end

---Writes a user report (<session>-user<N>: user.txt, device.json, journal.txt, log-tail.txt, screenshot.jpg).
---Nothing is sent: upload its files with Http and Crash.markHandled(id) once that succeeded, else it is
---offered to Crash.setReportHook at the next start. nil and the reason ("too_many_pending: ...") otherwise.
---@param options CrashReportOptions
---@return CrashReport|nil
---@return string|nil
function Crash.createReport(options) end

---Drops the draft Crash.beginReport captured (the form's Cancel)
function Crash.cancelReport() end

---A report file's bytes (at most 8 MiB), e.g. for an Http body; nil, "not_found" otherwise
---@param id string
---@param name string # "user.txt", "screenshot.jpg" ...
---@return string|nil
---@return string|nil
function Crash.readReportFile(id, name) end
