---@class CaptureProbe : ScriptInstance
local CaptureProbe = { properties = {} }

function CaptureProbe:init()
    self.frame = 0
    self.source = AudioSource(self.scene, self.entity)
    RegisterEngineEvent(self, "onUpdate")
end

function CaptureProbe:onUpdate()
    self.frame = self.frame + 1
    if self.frame == 60 then
        print("capture probe frame 60 playing " .. tostring(self.source:isPlaying()))
        Audio.mark("frame 60")
    end
end

return CaptureProbe
