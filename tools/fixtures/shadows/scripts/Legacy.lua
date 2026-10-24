-- A scene saved before the S10 shadows (Legacy.scene: scene format 0, a sun whose shadowBias is
-- the old 0.002 depth units, shadow quality Low): it loads with Bias 1 texel and quality Auto.
-- The script puts a camera, a ground and a few boxes under that sun.
local Legacy = { properties = {} }

function Legacy:init()
    local scene = self.scene
    self.objects = {}
    local camera = Camera(scene)
    camera.type = CameraType.CAMERA_PERSPECTIVE
    camera.position = Vector3(0, 7, 12)
    camera.target = Vector3(0, 0.5, 0)
    scene:setCamera(camera)
    table.insert(self.objects, camera)

    local ground = Primitive(scene)
    ground:createPlane(40, 40)
    ground:setColor(0.6, 0.6, 0.58)
    table.insert(self.objects, ground)

    for i = 0, 4 do
        local box = Primitive(scene)
        box:createBox(1, 1 + i * 0.5, 1)
        box.position = Vector3(-4 + i * 2, 0.5 + i * 0.25, 0)
        box:setColor(0.9, 0.75, 0.5)
        table.insert(self.objects, box)
    end
end

return Legacy
