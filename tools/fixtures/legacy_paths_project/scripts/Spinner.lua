---@class Spinner : ScriptInstance
local Helpers = require("util.helpers")
local Spinner = {
    properties = {
        -- AC19: an Asset script property, set in Main.scene (by GUID: it survives a rename)
        { name = "portrait", displayName = "Portrait", type = "Asset", assetType = "texture" },
    }
}

-- S04: the Asset module answers in the editor's Play and in exported games (assetmap.bin).
-- 4f17... is the GUID textures/brick.png got when the project was upgraded (its legacy GUID).
function Spinner:init()
    local guid = "4f172276042c39a8da88bc7b7635e7e1"
    local path = Asset.path(guid)
    if path ~= "textures/brick.png" then
        print("[ERROR] [S04] Asset.path(" .. guid .. ") = " .. tostring(path))
    elseif Asset.guid("textures/brick.png") ~= guid then
        print("[ERROR] [S04] Asset.guid = " .. tostring(Asset.guid("textures/brick.png")))
    elseif not Asset.exists(guid) or Asset.ref(guid).type ~= "texture" then
        print("[ERROR] [S04] Asset.exists / Asset.ref")
    else
        print("[S04] Asset.path " .. guid .. " = " .. path)
    end
    if self.portrait then
        if self.portrait.valid then
            print("[S04] portrait " .. self.portrait.path)
        else
            print("[ERROR] [S04] portrait " .. tostring(self.portrait.path) .. " does not resolve")
        end
    end
end

return Spinner
