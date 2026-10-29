-- The splines smoke project (docs/specs/S18-splines.md): a road and a wall saved in the scene, a
-- lake and a follower made here through the Spline Lua API, so an exported game builds both
-- (tools/smoke-test.sh tools/fixtures/splines).
local SplinesSmoke = { properties = {} }

function SplinesSmoke:init()
    local scene = self.scene
    local camera = Camera(scene)
    camera.type = CameraType.CAMERA_PERSPECTIVE
    camera.position = Vector3(0, 26, 34)
    camera.target = Vector3(0, 0, -2)
    scene:setCamera(camera)
    self.camera = camera

    local ground = Primitive(scene)
    ground:createPlane(90, 70)
    ground.position = Vector3(0, -0.05, 0)
    ground:setColor(0.36, 0.5, 0.3)
    self.ground = ground

    -- a filled closed spline: the lake
    local lake = GameObject(scene)
    local shape = lake:getSpline()
    shape.type = SplineType.CatmullRom
    for i = 0, 7 do
        local a = i * math.pi / 4
        shape:addKnot(Vector3(14 + 6 * math.cos(a), 0.02, 12 + 4 * math.sin(a)))
    end
    shape.closed = true
    lake:getSplineExtrude().shape = SplineExtrudeShape.Fill
    self.lake = lake

    -- a cart following the road at constant speed, back and forth
    local road = scene:findEntity("Road")
    local cart = Primitive(scene)
    cart:createBox(1.5, 1, 2.5)
    cart:setColor(0.9, 0.3, 0.2)
    local animate = GameObject(scene, cart.entity):getSplineAnimate()
    animate.spline = road
    animate.method = SplineAnimateMethod.Speed
    animate.speed = 8
    animate.loop = SplineLoop.PingPong
    animate.verticalOffset = 0.6
    animate:play()
    self.cart = cart

    local length = GameObject(scene, road):getSpline():getLength()
    Log.print("splines smoke: road " .. string.format("%.1f", length) .. " m, lake " .. shape:getKnotCount() .. " knots, cart " .. tostring(animate.playing))
end

return SplinesSmoke
