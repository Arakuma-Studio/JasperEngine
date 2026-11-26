---@meta _
-- Hand-written: Journal is bound with the Lua C API (engine/core/script/binding/CrashClassesLua.cpp),
-- which the generator does not read.

---An entry of the Gameplay Journal
---@class JournalEntry
---@field time number # seconds since the start
---@field frame integer
---@field kind string
---@field text string # the whole line: "+412.031 f24718 level.start name=forest-2 seed=42"

---The Gameplay Journal (docs/crash-diagnostics.md): recent game events kept in memory and saved with
---crash, hang and user reports. Events, not frames: a level start, a boss phase, a purchase.
---@class Journal
---@field enabled boolean # read-only: false when Project Settings > Player > Crash Handling > Gameplay Journal (KiB) is 0
Journal = {}

---Writes an entry: kind is [A-Za-z0-9_.:-]{1,48}; fields (at most 12) are strings, numbers or booleans
---@param kind string
---@param fields? table<string, string|number|boolean>
function Journal.log(kind, fields) end

---journal.txt as a report would hold it now
---@return string
function Journal.getText() end

---The newest n entries (50), oldest first
---@param n? integer
---@return JournalEntry[]
function Journal.getEntries(n) end

---Empties the journal (a new run of a level, a privacy reset)
function Journal.clear() end
