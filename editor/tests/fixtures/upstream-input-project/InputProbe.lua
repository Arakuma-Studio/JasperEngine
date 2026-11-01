local InputProbe = { properties = {} }

function InputProbe:init()
    RegisterEngineEvent(self, "onUpdate")
end

function InputProbe:onUpdate()
    _G.dash = Input.getButtonDown("Dash")
end

return InputProbe
