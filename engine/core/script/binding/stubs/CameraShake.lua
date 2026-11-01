---@meta _
-- Hand-written shapes of the camera shake arguments (engine/core/script/binding/GameObjectClassesLua.cpp,
-- feature camera-shake), appended to the generated stubs. Rules: docs/mcp/guide/camera.md.

---The built-in Shake Presets (CameraShake.getPresetNames())
---@alias ShakePresetName "explosion"|"light_explosion"|"earthquake"|"punch"|"shock"|"electrocute"|"drunk"|"offroad"|"jumpscare_2d"|"eh_shake"|"entity_shake"|"invader_drunk"|"invaders_explosion"|"handheld"|"vibration"|"bump"

---A Shake Preset given inline (the .shake keys, lowerCamelCase); a missing key takes its default
---@class ShakePreset
---@field type? "one_shot"|"sustained" # one_shot fades in, then out; sustained holds until stop (default one_shot)
---@field strength? number # amplitude multiplier (default 1)
---@field roughness? number # noise speed: higher is jerkier (default 1)
---@field fadeIn? number # seconds (default 0: instant)
---@field fadeOut? number # seconds (default 0: instant)
---@field positionInfluence? Vector3 # per-axis amplitude in units (2D: tenths of the view height; default 1, 1, 1)
---@field rotationInfluence? Vector3 # per-axis amplitude in degrees (default 1, 1, 1)

---Options of a shake call
---@class ShakeOptions
---@field strength? number # strengthScale (default 1)
---@field roughness? number # roughnessScale (default 1)
---@field seed? integer # the noise seed: the same seed shakes the same way (default random)
---@field unscaled? boolean # unscaled time for this shake (default: the shaker's unscaledTime)
---@field fadeIn? number # overrides the preset's fade in (seconds)
---@field fadeOut? number # overrides the preset's fade out (seconds)
