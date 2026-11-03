-- The courier: a dynamic Rigidbody pushed by the arrow keys, Space jumps.
---@class Player : ScriptInstance
local Player = {
    properties = {
        { name = "acceleration", displayName = "Acceleration", type = "float", default = 14.0 },
        { name = "jumpSpeed", displayName = "Jump Speed", type = "float", default = 5.5 },
    }
}

local T = require("lib.transcript")

function Player:init()
    self.go = GameObject(self.scene, self.entity)
    self.body = Rigidbody(self.scene, self.entity)
    self.jumps = 0
    RegisterEngineEvent(self, "onFixedUpdate")
    RegisterEngineEvent(self, "onUpdate")
end

function Player:onFixedUpdate()
    local fx, fz = 0, 0
    if T.held(Input.KEY_RIGHT) then fx = fx + 1 end
    if T.held(Input.KEY_LEFT) then fx = fx - 1 end
    if T.held(Input.KEY_UP) then fz = fz - 1 end
    if T.held(Input.KEY_DOWN) then fz = fz + 1 end
    if fx ~= 0 or fz ~= 0 then
        -- a force of mass x acceleration: the body's mass comes from its collider and density
        local f = self.body.mass * self.acceleration
        self.body:applyForce(Vector3(fx * f, 0, fz * f))
    end
end

function Player:onUpdate()
    if T.pressed(Input.KEY_SPACE) then
        local v = self.body.linearVelocity
        self.body.linearVelocity = Vector3(v.x, self.jumpSpeed, v.z)
        self.jumps = self.jumps + 1
        T.log("jump", self.jumps)
    end
end

return Player
