-- Title screen: an HTML UI menu. Enter (or a click on Play) starts the game.
---@class Title : ScriptInstance
local Title = {
    properties = {
        { name = "nextScene", displayName = "Next Scene", type = "string", default = "Level1" },
        { name = "startDelay", displayName = "Start Delay (frames)", type = "int", default = 6 },
    }
}

local T = require("lib.transcript")

function Title:init()
    T.enter("Title")
    T.schedule("Title", { { Input.KEY_ENTER, 30, 32 } })
    self.doc = UIDocument(self.scene, self.entity)
    self.startAt = -1
    self.clicks = 0
    self.onPlay = function(event)
        self.clicks = self.clicks + 1
        self:start("click")
    end
    RegisterEvent(self, self.doc.onLoad, "bind")
    self:bind()
    RegisterEngineEvent(self, "onUpdate")
    RegisterEngineEvent(self, "onPostUpdate")
end

function Title:bind()
    local play = self.doc:getElementById("play")
    if play then play:addEventListener("click", self.onPlay) end
end

function Title:start(how)
    if self.startAt >= 0 then return end
    self.startAt = T.frame + self.startDelay
    local status = self.doc:getElementById("status")
    if status then
        status.textContent = "Starting (" .. how .. ")"
        status.classList:add("ready")
    end
    T.log("start", how)
end

function Title:onUpdate()
    if T.pressed(Input.KEY_ENTER) then self:start("enter") end
    local ticks = self.doc:getElementById("ticks")
    if ticks then ticks.textContent = "frame " .. T.frame end
    if self.startAt >= 0 and T.frame == self.startAt then
        T.log("load", self.nextScene)
        SceneManager.loadScene(self.nextScene)
    end
end

function Title:onPostUpdate()
    if T.frame % 10 == 0 then
        local ticks = self.doc:getElementById("ticks")
        local status = self.doc:getElementById("status")
        T.log("ui", ticks and ticks.textContent or "?", status and status.textContent or "?",
              status ~= nil and status.classList:contains("ready"), self.clicks)
    end
    T.step()
end

return Title
