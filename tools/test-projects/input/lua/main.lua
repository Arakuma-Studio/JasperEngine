-- S22's test project: pointer lock, touch phases and the input scheme (main.cpp says where it runs);
-- the scheme's switches also drive FormUI.lua's text field
Engine.mouseMode = MouseMode.CAPTURED

local logged = {}
local downs = {}

local function once(text)
    if not logged[text] then
        logged[text] = true
        Log.print(text)
    end
end

Engine.onUpdate:add("gate", function()
    local lock = Input.getPointerLockState()
    if lock == Input.POINTER_PENDING then once("[gate] pointer pending") end
    if lock == Input.POINTER_LOCKED then once("[gate] pointer locked") end

    for i = 0, Input.getTouchCount() - 1 do
        local touch = Input.getTouch(i)
        if touch.phase == Input.TOUCH_BEGAN or touch.began then once("[gate] touch " .. touch.pointer .. " began") end
        if touch.phase == Input.TOUCH_ENDED then once("[gate] touch " .. touch.pointer .. " ended") end
        if Input.getTouchDown(touch.pointer) then
            downs[touch.pointer] = (downs[touch.pointer] or 0) + 1
            Log.print("[gate] touch " .. touch.pointer .. " down " .. downs[touch.pointer])
        end
    end
    if Input.getTouchCount() >= 2 then once("[gate] two touches in a frame") end
end)

-- the input scheme (input-device-awareness AC18): each switch, as the player's device changes
InputActions.onSchemeChanged = function(scheme, previous)
    Log.print("[gate] scheme " .. scheme)
    if FormUIOnScheme then FormUIOnScheme() end
end

-- the Replay menu and its input script (lua/replay.lua)
require("replay")
