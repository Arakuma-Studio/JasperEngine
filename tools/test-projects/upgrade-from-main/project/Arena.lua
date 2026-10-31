-- Arena 2D director: three balls on Rigidbody 2Ds; the arrow keys push the first one.
---@class Arena : ScriptInstance
local Arena = {
    properties = {
        { name = "duration", displayName = "Duration (frames)", type = "int", default = 180 },
        { name = "push", displayName = "Push (m/s per step)", type = "float", default = 0.12 },
        { name = "cloud", displayName = "Cloud", type = "Sprite" },
    }
}

local T = require("lib.transcript")

function Arena:init()
    T.enter("Arena2D")
    T.schedule("Arena2D", { { Input.KEY_RIGHT, 30, 60 }, { Input.KEY_LEFT, 90, 105 } })
    self.balls = {}
    for i = 1, 3 do
        local e = self.scene:findEntity("Ball " .. i)
        if e ~= 0 then self.balls[#self.balls + 1] = Rigidbody2D(self.scene, e) end
    end
    self.hits = 0
    RegisterEvent(self, self.scene:getPhysicsSystem().beginContact2D, "onBeginContact")
    RegisterEngineEvent(self, "onFixedUpdate")
    RegisterEngineEvent(self, "onPostUpdate")
    T.log("setup", #self.balls, self.cloud ~= nil)
end

function Arena:onBeginContact(bodyA, shapeA, bodyB, shapeB)
    self.hits = self.hits + 1
end

function Arena:onFixedUpdate()
    local ball = self.balls[1]
    if not ball then return end
    local dir = 0
    if T.held(Input.KEY_RIGHT) then dir = dir + 1 end
    if T.held(Input.KEY_LEFT) then dir = dir - 1 end
    if dir ~= 0 then
        ball:applyLinearImpulseToCenter(Vector2(dir * self.push * ball:getMass(), 0), true)
    end
end

function Arena:onPostUpdate()
    local f = T.frame
    if f % 10 == 0 then
        for i, ball in ipairs(self.balls) do
            T.log("ball", i, ball.position, ball.linearVelocity, ball.angle)
        end
    end
    if f % 30 == 0 and self.cloud then
        T.log("cloud", self.cloud.position)
    end
    if f == self.duration then
        T.log("summary", self.hits > 0)
        T.finish()
    end
    T.step()
end

return Arena
