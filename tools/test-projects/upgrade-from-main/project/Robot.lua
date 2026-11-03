-- The robot waves with a Pose Animation on its Pose Animator.
---@class Robot : ScriptInstance
local Robot = {
    properties = {
        { name = "speed", displayName = "Speed", type = "float", default = 1.0 },
    }
}

local T = require("lib.transcript")

function Robot:init()
    self.anim = Model(self.scene, self.entity):getPoseAnimator()
    self.started = -1
    self.finished = 0
    self.anim.onFinish = function() self.finished = self.finished + 1 end
    RegisterEngineEvent(self, "onUpdate")
end

function Robot:onUpdate()
    if self.started < 0 then
        -- the model loads asynchronously: the timeline starts with the animation
        if self.anim:isLoaded() then
            self.anim:setSpeed(self.speed)
            self.anim:play()
            self.started = T.frame
            T.logAt("robot", 0, "poses", self.anim:getPoseCount(), self.anim:getPoseName(0), self.anim:getLength())
        end
        return
    end
    local n = T.frame - self.started
    if n % 20 == 0 and n <= 240 then
        T.logAt("robot", n, "pose", self.anim:getTime(), self.anim:isPlaying(), self.finished)
    end
end

return Robot
