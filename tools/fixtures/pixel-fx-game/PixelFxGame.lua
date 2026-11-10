---@class PixelFxGame : ScriptInstance
-- S48 AC26 (docs/specs/S48-pixel-fx.md): the runtime Pixel FX in an exported game, logged for smoke-expect.txt.
-- PFX_STAGE holds a static state for the export test's frame comparison (tools/mcp/tests/test_pixel_fx_export.py):
-- unset, the whole flow (ending on the revealed Level scene); idle, set or covered
local PixelFxGame = { properties = {} }

function PixelFxGame:init()
    self.frames = 0
    self.stage = os.getenv("PFX_STAGE") or ""
    RegisterEngineEvent(self, "onUpdate")
end

function PixelFxGame:find(name)
    return GameObject(self.scene, self.scene:findEntity(name))
end

function PixelFxGame:onUpdate()
    self.frames = self.frames + 1
    if self.stage == "" then
        self:flow()
    elseif self.frames == 5 then
        self:hold()
    end
end

function PixelFxGame:flow()
    if self.frames == 5 then
        local dissolve = PixelFX.play(self:find("Enemy"), "dissolve", 0.4)
        Log.print("[gate] pixel_fx dissolve " .. tostring(dissolve.valid))
        HitStop.play(0.05)
        Log.print("[gate] pixel_fx hit_stop " .. tostring(HitStop.active))
        PixelFX.glitch(0.2, 0.8)
        Log.print("[gate] pixel_fx glitch burst")
    elseif self.frames == 30 then
        local handle = Transition.loadScene("Level", "iris", 0.5, {
            onCovered = function() Log.print("[gate] pixel_fx covered") end })
        Log.print("[gate] pixel_fx load_scene " .. tostring(handle.valid))
    end
end

function PixelFxGame:hold()
    if self.stage == "set" then
        PixelFX.set(self:find("Enemy"), "dissolve", 0.5)
        PixelFX.set(self:find("Fade"), "fade", 0.5)
        Log.print("[gate] pixel_fx stage set")
    elseif self.stage == "covered" then
        Transition.cover("iris", 0.3, { onCovered = function() Log.print("[gate] pixel_fx stage covered") end })
    else
        Log.print("[gate] pixel_fx stage " .. self.stage)
    end
end

return PixelFxGame
