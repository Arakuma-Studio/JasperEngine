---@class PlayerMove : ScriptInstance
-- Moves its sprite with the arrow keys or WASD.
local PlayerMove = { properties = {
    { name = "speed", type = "float", default = 300.0 },
} }

function PlayerMove:init()
    self.go = GameObject(self.scene, self.entity)
    RegisterEngineEvent(self, "onUpdate")
end

local function held(a, b)
    return Input.isKeyPressed(a) or Input.isKeyPressed(b)
end

function PlayerMove:onUpdate()
    local x, y = 0, 0
    if held(Input.KEY_LEFT, Input.KEY_A) then x = x - 1 end
    if held(Input.KEY_RIGHT, Input.KEY_D) then x = x + 1 end
    if held(Input.KEY_UP, Input.KEY_W) then y = y + 1 end
    if held(Input.KEY_DOWN, Input.KEY_S) then y = y - 1 end
    if x ~= 0 or y ~= 0 then
        local step = self.speed * Engine.deltatime
        self.go.position = self.go.position + Vector3(x * step, y * step, 0)
    end
end

return PlayerMove
