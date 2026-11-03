-- The main camera follows the Player from an offset, smoothly.
---@class CameraFollow : ScriptInstance
local CameraFollow = {
    properties = {
        { name = "target", displayName = "Target", type = "GameObject" },
        { name = "offset", displayName = "Offset", type = "vector3", default = { 0, 6, 11 } },
        { name = "smoothing", displayName = "Smoothing", type = "float", default = 4.0 },
    }
}

function CameraFollow:init()
    self.camera = Camera(self.scene, self.entity)
    RegisterEngineEvent(self, "onUpdate")
end

function CameraFollow:onUpdate()
    if not self.target then return end
    local t = self.target:getWorldPosition()
    local p = self.camera.position
    local k = math.min(1, Engine.deltatime * self.smoothing)
    local goal = Vector3(t.x + self.offset.x, t.y + self.offset.y, t.z + self.offset.z)
    self.camera.position = p + (goal - p) * k
    self.camera:setTarget(t.x, t.y, t.z)
end

return CameraFollow
