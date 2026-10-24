---@meta _
-- Asset identity (docs/specs/S04-asset-identity.md): the Asset module, written as raw Lua C
-- functions the bindings cannot describe. Hand-written fragment (engine/core/script/binding/stubs),
-- appended to the generated stubs.

---GUID <-> path lookups of the project's assets: in the editor's Play from its asset database,
---in exported games from assetmap.bin (loaded on the first call). Paths are relative to the root
---the asset type uses in references: the assets directory for textures, models, audio, fonts,
---UI documents and data files, the project for scenes, prefabs, materials, shaders and scripts.
---```lua
---local path = Asset.path("4b1e9c0d2a7f4e31b6c58d0f9e2a7c13")  -- "textures/hero.png"
---local ref = Asset.ref("textures/hero.png")                    -- ref.guid, ref.type, ref.valid
---```
Asset = {}

---The current path of the asset with this GUID (32 hex digits; a former GUID or a sub-asset's
---too), nil when no asset has it.
---@param guid string
---@return string|nil
function Asset.path(guid) end

---The GUID of the asset at this path, nil when the path is no asset.
---@param path string
---@return string|nil
function Asset.guid(path) end

---Whether an asset has this GUID or path.
---@param guidOrPath string
---@return boolean
function Asset.exists(guidOrPath) end

---A reference to the asset with this GUID or path.
---@param guidOrPath string
---@return AssetRef
function Asset.ref(guidOrPath) end
