-- Level 1 director: counts the frames, plays the pickup feedback, prints the transcript and
-- moves on to the next scene.
---@class Level : ScriptInstance
local Level = {
    properties = {
        { name = "duration", displayName = "Duration (frames)", type = "int", default = 360 },
        { name = "nextScene", displayName = "Next Scene", type = "string", default = "Arena2D" },
        { name = "pickupSound", displayName = "Pickup Sound", type = "AudioSource" },
        { name = "burst", displayName = "Pickup Burst", type = "ParticleSystem" },
        { name = "hud", displayName = "HUD", type = "UIDocument" },
    }
}

local T = require("lib.transcript")

function Level:init()
    T.enter("Level1")
    T.schedule("Level1", {
        { Input.KEY_RIGHT, 20, 80 }, { Input.KEY_UP, 80, 130 }, { Input.KEY_SPACE, 140, 142 },
        { Input.KEY_LEFT, 150, 210 }, { Input.KEY_DOWN, 210, 250 }, { Input.KEY_SPACE, 260, 262 },
    })
    self.player = GameObject(self.scene, self.scene:findEntity("Player"))
    self.body = Rigidbody(self.scene, self.player.entity)
    -- physics state is read from the bodies: a GameObject's Transform is drawn between the last
    -- two fixed steps (physics interpolation), as far as the frame's leftover time, which a
    -- loading frame changes
    self.crates = {}
    for i = 1, 3 do
        local e = self.scene:findEntity("Crate " .. i)
        if e ~= 0 then self.crates[#self.crates + 1] = { name = "Crate " .. i, body = Rigidbody(self.scene, e) } end
    end
    RegisterEngineEvent(self, "onUpdate")
    RegisterEngineEvent(self, "onPostUpdate")
    T.log("setup", #self.crates, T.state.gems or 0, self.pickupSound ~= nil, self.burst ~= nil, self.hud ~= nil)
end

function Level:onUpdate()
    local pending = T.state.pending
    if pending and #pending > 0 then
        for _, position in ipairs(pending) do
            if self.pickupSound then
                self.pickupSound:stop()
                self.pickupSound:play()
                T.state.sfx = T.state.sfx + 1
            end
            if self.burst then
                local fx = GameObject(self.scene, self.burst.target)
                fx.position = position
                self.burst:start()
                T.state.bursts = T.state.bursts + 1
            end
        end
        T.state.pending = {}
    end
end

function Level:onPostUpdate()
    local f = T.frame
    if f % 10 == 0 then
        T.log("player", self.body.position, self.body.linearVelocity, T.state.score, T.state.pickups)
    end
    if f % 30 == 0 then
        for _, crate in ipairs(self.crates) do
            T.log("crate", crate.name, crate.body.position, crate.body.rotation)
        end
        if self.hud then
            local score = self.hud:getElementById("score")
            local gems = self.hud:getElementById("gems")
            T.log("hud", score and score.textContent or "?", gems and gems.textContent or "?",
                  gems ~= nil and gems.classList:contains("all"))
        end
        T.log("fx", T.state.sfx, T.state.bursts, self.burst ~= nil and self.burst:isRunning())
    end
    if f == self.duration then
        T.log("summary", T.state.score, T.state.pickups, T.state.pulses)
        SceneManager.loadScene(self.nextScene)
    end
    T.step()
end

return Level
