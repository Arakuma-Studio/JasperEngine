-- A gem: spins, and is collected when the Player comes close.
---@class Gem : ScriptInstance
local Gem = {
    properties = {
        { name = "value", displayName = "Value", type = "int", default = 1 },
        { name = "spinSpeed", displayName = "Spin Speed", type = "float", default = 90.0 },
        { name = "radius", displayName = "Pickup Radius", type = "float", default = 1.1 },
    }
}

local T = require("lib.transcript")

function Gem:init()
    self.go = GameObject(self.scene, self.entity)
    local player = self.scene:findEntity("Player")
    -- the courier's body: its Transform is interpolated for drawing (see Level.lua)
    self.player = player ~= 0 and Rigidbody(self.scene, player) or nil
    self.angle = 0
    self.collected = false
    T.state.gems = (T.state.gems or 0) + 1
    RegisterEngineEvent(self, "onUpdate")
end

function Gem:onUpdate()
    if self.collected then return end
    self.angle = (self.angle + self.spinSpeed * Engine.deltatime) % 360
    self.go:setRotation(0, self.angle, 0)
    if self.player then
        local p = self.player.position
        local g = self.go:getWorldPosition()
        local dx, dz = p.x - g.x, p.z - g.z
        if dx * dx + dz * dz < self.radius * self.radius and math.abs(p.y - g.y) < 2.0 then
            self:collect()
        end
    end
end

function Gem:collect()
    self.collected = true
    T.state.score = T.state.score + self.value
    T.state.pickups = T.state.pickups + 1
    T.log("pickup", self.go.name, self.value, T.state.score)
    -- the Director plays the pickup sound and the burst where the gem was
    T.state.pending = T.state.pending or {}
    table.insert(T.state.pending, self.go:getWorldPosition())
    self.go:setActive(false)
end

return Gem
