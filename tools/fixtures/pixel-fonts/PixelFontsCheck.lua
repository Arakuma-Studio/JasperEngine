---@class PixelFontsCheck : ScriptInstance
-- S51 AC22 (docs/specs/S51-pixel-fonts.md R34): in the exported game the bitmap fonts load from their
-- cooked .jfont (the sources are not shipped), measure as the editor does and draw at whole scales
local PixelFontsCheck = { properties = {} }

function PixelFontsCheck:init()
    self.frames = 0
    RegisterEngineEvent(self, "onUpdate")
end

function PixelFontsCheck:onUpdate()
    self.frames = self.frames + 1
    if self.frames ~= 10 then return end
    local hud, err = BitmapFont.load("assets/fonts/hud.bitmapfont")
    if hud == nil then
        Log.error("[gate] pixel_fonts: assets/fonts/hud.bitmapfont does not load: " .. tostring(err))
        return
    end
    local w, h = hud:measure("SCORE 120", 2)
    Log.print("[gate] pixel_fonts hud " .. hud.nativeSize .. " px, " .. hud.glyphCount .. " glyphs, SCORE 120 x2 " .. w .. "x" .. h)
    local pixel = BitmapFont.load("builtin:pixel")
    Log.print("[gate] pixel_fonts builtin " .. pixel.nativeSize .. " px, " .. pixel.glyphCount .. " glyphs")
    local title = Text(self.scene, self.scene:findEntity("Title"))
    Log.print("[gate] pixel_fonts Title x" .. title.pixelScale .. " (" .. title.effectiveFontSize .. ")")
    local styled = Text(self.scene, self.scene:findEntity("Styled"))
    Log.print("[gate] pixel_fonts Styled outline " .. math.floor(styled.outlineWidth) .. " neighbors " .. styled.outlineNeighbors)
end

return PixelFontsCheck
