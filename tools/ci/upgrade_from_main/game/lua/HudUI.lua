-- The HTML HUD of Level 1: the score and the gems collected.
---@class HudUI : ScriptInstance
local HudUI = { properties = {} }

local T = require("lib.transcript")

function HudUI:init()
    self.doc = UIDocument(self.scene, self.entity)
    RegisterEngineEvent(self, "onUpdate")
end

function HudUI:onUpdate()
    local score = self.doc:getElementById("score")
    local gems = self.doc:getElementById("gems")
    if score then score.textContent = tostring(T.state.score) end
    if gems then
        gems.textContent = "gems " .. T.state.pickups .. "/" .. (T.state.gems or 0)
        if T.state.gems and T.state.pickups >= T.state.gems then gems.classList:add("all") end
    end
end

return HudUI
