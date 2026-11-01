---@class Rotator : ScriptInstance
-- Turns its GameObject around the vertical axis.
local Rotator = { properties = {
    { name = "degreesPerSecond", type = "float", default = 45.0 },
} }

function Rotator:init()
    self.go = GameObject(self.scene, self.entity)
    self.angle = 0
    RegisterEngineEvent(self, "onUpdate")
end

function Rotator:onUpdate()
    self.angle = (self.angle + self.degreesPerSecond * Engine.deltatime) % 360
    self.go:setRotation(0, self.angle, 0)
end

return Rotator
