-- The beacon bobs with an Animation Clip on its Animation Player; the clip's event calls
-- onBeaconPulse.
---@class Beacon : ScriptInstance
local Beacon = {
    properties = {
        { name = "clip", displayName = "Clip", type = "string", default = "Hover" },
    }
}

local T = require("lib.transcript")

function Beacon:init()
    self.go = GameObject(self.scene, self.entity)
    self.player = self.go:getAnimationPlayer()
    self.pulses = 0
    self.started = -1
    RegisterEngineEvent(self, "onUpdate")
end

function Beacon:onUpdate()
    if not self.player then return end
    if self.started < 0 then
        -- clips load asynchronously: the timeline starts with the clip
        if self.player:isLoaded() then
            self.player:play(self.clip)
            self.started = T.frame
        end
        return
    end
    local n = T.frame - self.started
    if n % 15 == 0 and n <= 240 then
        local state = self.player:getState(self.clip)
        T.logAt("beacon", n, "clip", state.time, state.normalizedTime, self.player:isPlaying(self.clip),
                self.go.position, self.pulses)
    end
end

function Beacon:onBeaconPulse(strength, label)
    self.pulses = self.pulses + 1
    T.state.pulses = T.state.pulses + 1
    T.logAt("beacon", T.frame - self.started, "pulse", self.pulses, strength, label)
end

return Beacon
