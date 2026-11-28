---@meta _
-- Hand-written: SaveGame is bound with the Lua C API (engine/core/script/binding/SaveGameLua.cpp),
-- which the generator does not read.

---What a failed call returns (also the second value of a failed read)
---@class SaveError
---@field code "name"|"encode"|"too_large"|"not_found"|"corrupt"|"newer_version"|"newer_format"|"migration"|"io"|"quota"|"unavailable"|"cancelled"
---@field message string
---@field path? string # where in the value an encode failed ("$.player.onHit")

---@class SaveWriteOptions
---@field namespace? string # default "main"; 1-32 characters of a-z, 0-9, _ and -
---@field meta? table # the slot's metadata for save menus (at most 4 KiB); nil keeps the previous
---@field requires? table<string, integer> # content the save needs (names to minimum versions), stored and returned only
---@field replaceAll? boolean # drop every other namespace of the slot
---@field force? boolean # write over a slot of a newer version or format
---@field onComplete? fun(ok: boolean, err: SaveError|nil) # on the main thread, in a later frame
---@field owner? any # a script self, a GameObject or a Scene: its callback is dropped when it goes

---@class SaveReadOptions
---@field namespace? string # default "main"
---@field owner? any # readAsync: as SaveWriteOptions.owner

---What read returns with the data
---@class SaveInfo
---@field slot string
---@field version integer # the data version (after migrations)
---@field generation integer
---@field savedAt number # UTC seconds
---@field namespaces string[]
---@field meta table|nil
---@field requires table|nil
---@field migratedFrom? integer # the version the migrations started from
---@field recoveredFrom? integer # the generation used because newer ones were damaged
---@field problems? string[] # one per damaged file skipped ("slot1.g41.jsave: checksum mismatch")

---One slot of SaveGame.list()
---@class SaveSlotInfo
---@field slot string
---@field version integer
---@field savedAt number # UTC seconds
---@field size integer # bytes of the current generation
---@field generations integer # generation files on disk
---@field namespaces string[]
---@field meta table|nil
---@field requires table|nil
---@field status "ok"|"recovered"|"corrupt"|"newer_version"|"newer_format"

---One generation of SaveGame.getBackups()
---@class SaveBackupInfo
---@field generation integer
---@field savedAt number
---@field version integer
---@field size integer
---@field valid boolean
---@field problem? string

---Save slots (docs/saved-data.md): named slots of namespaces holding Lua values (tables, numbers,
---strings, booleans, Vector2/3/4, Quaternion, Json.null), written off the main thread, kept in
---several checksummed generations and upgraded by the game's migrations. Unreal's SaveGame slots;
---Unity has none built in (PlayerPrefs is UserSettings).
---@class SaveGame
---@field version integer # the data version every write stamps (default 1); reads of older slots run the migrations
SaveGame = {}

---Queues a write of `value` (encoded now: later changes to the table do not reach the save)
---@param slot string # 1-48 characters of a-z, 0-9, _ and -
---@param value any
---@param options? SaveWriteOptions
---@return boolean ok
---@return SaveError|nil err
function SaveGame.write(slot, value, options) end

---One namespace (default main), pending writes included; older versions run the migrations
---@param slot string
---@param options? SaveReadOptions
---@return any|nil data
---@return SaveInfo|SaveError info
function SaveGame.read(slot, options) end

---As read, with the file read on a worker; fn(data, info) runs on the main thread
---@param slot string
---@param fn fun(data: any|nil, info: SaveInfo|SaveError)
---@param options? SaveReadOptions
function SaveGame.readAsync(slot, fn, options) end

---Every slot, newest first (headers only)
---@return SaveSlotInfo[]
function SaveGame.list() end

---One slot as list() shows it
---@param slot string
---@return SaveSlotInfo|nil
---@return SaveError|nil err
function SaveGame.info(slot) end

---@param slot string
---@return boolean
function SaveGame.exists(slot) end

---Removes every generation of the slot
---@param slot string
---@return boolean ok
---@return SaveError|nil err
function SaveGame.delete(slot) end

---Blocks until the queued writes (of `slot`, or all) are on disk, at most 2 s
---@param slot? string
---@return boolean ok
---@return SaveError|nil err
function SaveGame.flush(slot) end

---Every generation of the slot, newest first
---@param slot string
---@return SaveBackupInfo[]|nil
---@return SaveError|nil err
function SaveGame.getBackups(slot) end

---Writes that generation's data as a new newest generation (the chosen file stays)
---@param slot string
---@param generation integer
---@param onComplete? fun(ok: boolean, err: SaveError|nil)
---@return boolean ok
---@return SaveError|nil err
function SaveGame.restoreBackup(slot, generation, onComplete) end

---Registers fn, which upgrades a slot's namespaces from fromVersion to fromVersion + 1
---@param fromVersion integer
---@param fn fun(namespaces: table<string, any>, info: {slot: string, fromVersion: integer, toVersion: integer}): table<string, any>
function SaveGame.addMigration(fromVersion, fn) end

---Drops the slots kept in memory (they reload on the next read)
---@param slot? string
function SaveGame.unload(slot) end

---Storage the saves use: the web's site storage (used and quota) or the volume's free space (-1: unknown)
---@return {used: integer, quota: integer, free: integer}
function SaveGame.storage() end
