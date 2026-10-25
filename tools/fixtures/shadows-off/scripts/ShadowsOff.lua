-- A lit scene without shadows (spec S10 AC15): a sun that casts none, a camera, a ground and a
-- few boxes, built with what every engine version has, so the same project measures the passes
-- and draws of a build before the shadows and after (tools/smoke-test.sh with
-- SMOKE_GAME_ENV=JASPERENGINE_FRAME_STATS=1): shadows that are off cost nothing.
local ShadowsOff = { properties = {} }

function ShadowsOff:init()
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

return ShadowsOff
