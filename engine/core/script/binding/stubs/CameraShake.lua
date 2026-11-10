---@meta _
-- Hand-written shapes of the camera shake arguments (engine/core/script/binding/GameObjectClassesLua.cpp,
-- feature camera-shake), appended to the generated stubs. Rules: docs/mcp/guide/camera.md.

---The built-in Shake Presets (CameraShake.getPresetNames())
---@alias ShakePresetName "explosion"|"light_explosion"|"earthquake"|"punch"|"shock"|"electrocute"|"drunk"|"offroad"|"jumpscare_2d"|"eh_shake"|"entity_shake"|"invader_drunk"|"invaders_explosion"|"handheld"|"vibration"|"bump"|"pixel_hit"|"pixel_heavy"|"pixel_rumble"

---A Shake Preset given inline (the .shake keys, lowerCamelCase); a missing key takes its default
---@class ShakePreset
---@field type? "one_shot"|"sustained"|"kick" # one_shot fades in, then out; sustained holds until stop; kick: a decaying whole-pixel jolt over fadeOut seconds (default one_shot)
---@field strength? number # amplitude multiplier (default 1)
---@field roughness? number # noise speed: higher is jerkier (default 1)
---@field fadeIn? number # seconds (default 0: instant)
---@field fadeOut? number # seconds (default 0: instant)
---@field positionInfluence? Vector3 # per-axis amplitude in units (2D: tenths of the view height; default 1, 1, 1)
---@field rotationInfluence? Vector3 # per-axis amplitude in degrees (default 1, 1, 1)
---@field units? "view"|"pixels" # what strength is in: view units, or art pixels (default view)
---@field tick? number # Hz: the offset changes only on ticks (default 0: every frame; a kick steps at 60)

---Options of a shake call
---@class ShakeOptions
---@field strength? number # strengthScale (default 1)
---@field roughness? number # roughnessScale (default 1)
---@field seed? integer # the noise seed: the same seed shakes the same way (default random)
---@field unscaled? boolean # unscaled time for this shake (default: the shaker's unscaledTime)
---@field fadeIn? number # overrides the preset's fade in (seconds)
---@field fadeOut? number # overrides the preset's fade out (seconds)

---What HitStop.play takes besides the seconds
---@class HitStopOptions
---@field timeScale? number # the game's speed while it runs, 0..1 (default 0: frozen)
---@field exclude? GameObject[] # these GameObjects (and their children) keep running for this hit
---@field only? GameObject[] # only these GameObjects (and their children) freeze; the world runs
---@field shake? ShakePresetName|string|{amplitude: number, seconds: number} # a shake on unscaled time: a preset, a .shake path or a pixel kick
