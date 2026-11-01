-- The port of the game's S/VertexLighting/VertexLightsUpdater.cs (shader globals spec 1.6): the
-- colours, positions and radii of four lights, the GameObjects named Light0 .. Light3 of its
-- scene, pushed every frame as global shader values for the shaders lit by them
-- (assets/shaders/vertex_lights.vert). Unity's Shader.SetGlobalColor / SetGlobalVector calls,
-- one for one.

---@class VertexLightsUpdater : ScriptInstance
local VertexLightsUpdater = {
    properties = {
        { name = "radius", displayName = "Radius", type = "float", default = 5.0 },
        { name = "color0", displayName = "Color 0", type = "color4", default = {1, 1, 1, 1} },
        { name = "color1", displayName = "Color 1", type = "color4", default = {0, 0, 0, 1} },
        { name = "color2", displayName = "Color 2", type = "color4", default = {0, 0, 0, 1} },
        { name = "color3", displayName = "Color 3", type = "color4", default = {0, 0, 0, 1} },
    }
}

function VertexLightsUpdater:init()
    self.colorIds = {}
    for i = 0, 3 do
        self.colorIds[i + 1] = Shader.propertyToID("_VertexLightColor" .. i)
    end
    self.positionX = Shader.propertyToID("_LightPositionX")
    self.positionY = Shader.propertyToID("_LightPositionY")
    self.positionZ = Shader.propertyToID("_LightPositionZ")
    self.radiusId = Shader.propertyToID("_LightRadius")
    RegisterEngineEvent(self, "onUpdate")
end

function VertexLightsUpdater:onUpdate()
    local x, y, z, r = {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}
    local colors = {self.color0, self.color1, self.color2, self.color3}
    for i = 1, 4 do
        local entity = self.scene:findEntity("Light" .. (i - 1))
        if entity ~= NULL_ENTITY then
            local p = GameObject(self.scene, entity):getWorldPosition()
            x[i], y[i], z[i], r[i] = p.x, p.y, p.z, self.radius
            Shader.setGlobalColor(self.colorIds[i], colors[i])
        else
            -- no light: black, out of reach
            Shader.setGlobalColor(self.colorIds[i], Vector4(0, 0, 0, 1))
        end
    end
    Shader.setGlobalVector(self.positionX, Vector4(x[1], x[2], x[3], x[4]))
    Shader.setGlobalVector(self.positionY, Vector4(y[1], y[2], y[3], y[4]))
    Shader.setGlobalVector(self.positionZ, Vector4(z[1], z[2], z[3], z[4]))
    Shader.setGlobalVector(self.radiusId, Vector4(r[1], r[2], r[3], r[4]))
end

return VertexLightsUpdater
