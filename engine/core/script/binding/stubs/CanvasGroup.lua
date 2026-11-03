---@meta _
-- Hand-written shapes of the CanvasGroup fades (engine/core/script/binding/GameObjectClassesLua.cpp,
-- feature ui-canvas-group), appended to the generated stubs. Rules: docs/mcp/guide/ui.md, "Canvas Group".

---The options of CanvasGroup:fadeTo, fadeIn and fadeOut
---@class CanvasGroupFadeOptions
---@field ease? EaseType # the curve (default EaseType.LINEAR)
---@field unscaled? boolean # runs at Engine time scale 0, a pause menu (default false: scaled time)
---@field deactivate? boolean # deactivates the GameObject when the fade completes, after onComplete (default false)
---@field onComplete? fun() # called on the main thread when the fade completes (not when it is stopped)
