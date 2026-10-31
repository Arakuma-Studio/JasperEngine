-- The HUD child scene (the engine's own UI components): a score Text, a time Progressbar.
---@class LegacyHud : ScriptInstance
local LegacyHud = {
    properties = {
        { name = "label", displayName = "Score Label", type = "Text" },
        { name = "timeBar", displayName = "Time Bar", type = "Progressbar" },
        { name = "duration", displayName = "Duration (frames)", type = "int", default = 360 },
    }
}

local T = require("lib.transcript")

function LegacyHud:init()
    RegisterEngineEvent(self, "onUpdate")
end

function LegacyHud:onUpdate()
    -- first what the previous frame showed (its score was final), then this frame's values
    if T.scene == "Level1" and T.frame % 30 == 0 and self.label and self.timeBar then
        T.log("legacy_hud", self.label.text, self.timeBar.value)
    end
    if self.label then self.label.text = "Score " .. T.state.score end
    if self.timeBar then self.timeBar.value = math.min(1, T.frame / self.duration) end
end

return LegacyHud
