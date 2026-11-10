---@class PixelFxLevel : ScriptInstance
-- S48 AC26: the scene Transition.loadScene switched to, logged once the reveal has finished
local PixelFxLevel = { properties = {} }

function PixelFxLevel:init()
    self.logged = false
    RegisterEngineEvent(self, "onUpdate")
end

function PixelFxLevel:onUpdate()
    if not self.logged and not Transition.active then
        self.logged = true
        Log.print("[gate] pixel_fx level revealed")
    end
end

return PixelFxLevel
