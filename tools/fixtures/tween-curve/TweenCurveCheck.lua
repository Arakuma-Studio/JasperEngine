---@class TweenCurveCheck : ScriptInstance
-- On the Position Tween: its value must be its curve at its time, and the target at 10 x the value
local TweenCurveCheck = { properties = {} }

local SAMPLES = { 0.1, 0.3, 0.5, 0.7, 0.9 }

function TweenCurveCheck:init()
    self.tween = PositionTween(self.scene, self.entity)
    self.target = GameObject(self.scene, self.tween.target)
    self.next = 1
    self.failed = false
    local peak = self.tween.curve:getKey(2)
    if self.tween.curve.length ~= 3 or peak == nil or peak.time ~= 0.5 or peak.value ~= 1 then
        Log.error("[gate] tween_curve: the exported curve is not the bump")
        self.failed = true
    end
    RegisterEngineEvent(self, "onUpdate")
end

function TweenCurveCheck:onUpdate()
    if self.failed or self.next > #SAMPLES then return end
    local state = self.tween:getTweenComponent()
    local expected = self.tween.curve:evaluate(state.time)
    local x = self.target.position.x
    if math.abs(state.value - expected) > 1e-5 or math.abs(x - 10 * state.value) > 1e-3 then
        Log.error("[gate] tween_curve mismatch at time " .. state.time .. ": value " .. state.value ..
                  ", curve " .. expected .. ", x " .. x)
        self.failed = true
        return
    end
    while self.next <= #SAMPLES and state.time >= SAMPLES[self.next] do
        Log.print("[gate] tween_curve sample " .. self.next .. " ok")
        self.next = self.next + 1
    end
    if self.next > #SAMPLES then
        Log.print("[gate] tween_curve 5 samples: value = curve(time), x = 10 value")
    end
end

return TweenCurveCheck
