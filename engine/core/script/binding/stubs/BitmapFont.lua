---@meta _
-- Pixel fonts (docs/specs/S51-pixel-fonts.md R29): the BitmapFont functions written as raw Lua C
-- functions the bindings cannot describe. Hand-written fragment (engine/core/script/binding/stubs),
-- appended to the generated stubs.

---A bitmap font (.bitmapfont, .fnt or "builtin:pixel") and its metrics, in whole font pixels.
---nil and the reason when it does not load.
---```lua
---local hud = BitmapFont.load("assets/fonts/hud.bitmapfont")
---local w, h = hud:measure("SCORE 120", 2)
---```
---@param path string
---@return BitmapFont|nil font
---@return string|nil error
function BitmapFont.load(path) end

---The width and height of the text (newlines split lines), whole pixels at `scale` (default 1):
---the same layout a Text draws.
---@param text string
---@param scale? integer
---@return integer width
---@return integer height
function BitmapFont:measure(text, scale) end
