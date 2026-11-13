---@meta _
-- Hand-written: DataAsset is bound with the Lua C API (engine/core/script/binding/DataAssetClassesLua.cpp),
-- which the generator does not read.

---Data Assets, Unity's ScriptableObjects (docs/specs/S95-lua-runtime-gaps.md, lua-api 2.4.3). A data
---class is a Lua module whose first lines hold `---@dataasset`: its `properties` are the fields of
---its assets, its functions their methods. An asset is a table of its values (references resolved:
---other Data Assets as their tables), one per asset and run; changes made to it in the game are
---never written to its file.
---```lua
---local sword = DataAsset.load("items/sword.dataasset")   -- sword.damage, sword:describe()
---for _, item in ipairs(DataAsset.loadAll("ItemData")) do print(item.name) end
---```
---@class DataAsset
DataAsset = {}

---The shared table of the .dataasset at path (relative to the assets folder, or asset://...), or its
---GUID; nil and why ("cannot open items/axe.dataasset", "no data class 'ItemData' ...") when it does not load
---@param pathOrGuid string
---@return table|nil asset
---@return string|nil error
function DataAsset.load(pathOrGuid) end

---Every Data Asset of the class the game ships, sorted by path (an asset that does not load is left
---out, with a warning); nil and why for an unknown class
---@param className string
---@return table[]|nil assets
---@return string|nil error
function DataAsset.loadAll(className) end

---A new table of the class with its fields' defaults, of no file (DataAsset.pathOf gives nil);
---nil and why for an unknown class
---@param className string
---@return table|nil asset
---@return string|nil error
function DataAsset.create(className) end

---The class of a Data Asset's table, nil for any other value
---@param asset any
---@return string|nil
function DataAsset.classOf(asset) end

---The file of a Data Asset's table (relative to the assets folder), nil for a created one or any other value
---@param asset any
---@return string|nil
function DataAsset.pathOf(asset) end
