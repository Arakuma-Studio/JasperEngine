---@meta _
-- Hand-written shapes of the PixelFX player's arguments (engine/core/script/binding/PixelFxClassesLua.cpp,
-- S48), appended to the generated stubs. Rules: docs/mcp/guide/pixel-fx.md.

---A PixelFX effect
---@alias PixelFXEffectName "flash"|"fade_out"|"fade_in"|"blink"|"dissolve"|"undissolve"|"petrify"|"freeze"|"poison"|"outline_pulse"

---Options of PixelFX.play
---@class PixelFXOptions
---@field ease? string|integer # an ease name ("linear", "quad_out", "sine_in_out", ...) or an EaseType (default linear)
---@field unscaled? boolean # on unscaled time: neither Time.timeScale nor a hit-stop slows it
---@field color? string|Vector4 # flash, outline_pulse: "#rrggbb" or an sRGB Vector4 (default the material's)
---@field count? integer # blink, outline_pulse: how many (default 3)
---@field ramp? string # dissolve, petrify, freeze, poison: an N x 1 ramp strip or "<palette>.palette#ramp:<name>" (default the material's)
---@field embers? string|boolean # dissolve: a Pixel Particles preset ("ember"; true is "ember") or a .particles path, emitted where the front burns
---@field emberRate? number # embers per second at most (default 40)
---@field hold? number # petrify, freeze, poison: seconds held at full strength (default half the duration)
---@field onFinished? fun() # once, when the effect ends by itself
