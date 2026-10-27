-- S22's test project: pointer lock and touch phases (main.cpp says where it runs)
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
