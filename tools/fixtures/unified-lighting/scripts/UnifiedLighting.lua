-- Unified lighting: more moving lights than the per-object cap, every light shading option,
-- materials lit per pixel, per vertex and mixed, shadows from a directional and a spot light,
-- a light culling mask, an instanced batch and an older fork with the legacy lights block.
-- Built at runtime, so every variant compiles in the game.
--   tools/smoke-test.sh tools/fixtures/unified-lighting
--
-- Rows, front to back: PIXEL, MIXED, VERTEX materials (vertex lights are diffuse only: the
-- metallic primitives show only their ambient there; the glTF sphere and box are dielectric). The cap switches 8 -> 4 -> 8 while
-- playing (a capacity tier change reloads every lit mesh).
local UnifiedLighting = { properties = {} }

local function log(fmt, ...)
    print("[UL] " .. string.format(fmt, ...))
end

local function fail(fmt, ...)
    -- the smoke test fails on an [ERROR] in the game log
    print("[ERROR] [UL] " .. string.format(fmt, ...))
end

local function setLighting(mesh, lighting)
    for s = 0, mesh:getNumSubmeshes() - 1 do
        local m = mesh:getMaterial(s)
        m.lighting = lighting
        mesh:setMaterial(s, m)
    end
end

local COLORS = {
    {1.0, 0.25, 0.2}, {0.2, 0.45, 1.0}, {0.3, 1.0, 0.35}, {1.0, 0.8, 0.2},
    {0.9, 0.3, 1.0}, {0.2, 1.0, 0.9}, {1.0, 0.55, 0.15}, {0.6, 0.6, 1.0},
}
local SHADINGS = { LightShading.BOTH, LightShading.PIXEL, LightShading.VERTEX }

function UnifiedLighting:init()
    RegisterEngineEvent(self, "onUpdate")
    local scene = self.scene

    self.time = 0
    self.frames = 0
    self.stage = 0
    -- a GameObject made here lives as long as its Lua object: keep them all
    self.objects = {}
    local function keep(object)
        table.insert(self.objects, object)
        return object
    end

    scene.maxLightsPerObject = 8
    scene:setGlobalIllumination(0.12, Vector3(0.6, 0.65, 0.8))
    scene.backgroundColor = Vector4(0.05, 0.06, 0.08, 1.0)

    local camera = keep(Camera(scene))
    camera.type = CameraType.CAMERA_PERSPECTIVE
    camera.position = Vector3(0, 9, 15)
    camera.target = Vector3(0, 0.5, -0.5)
    scene:setCamera(camera)

    -- shadows: a dim sun and a static spot over the middle row
    local sun = keep(Light(scene))
    sun.type = LightType.DIRECTIONAL
    sun:setDirection(-0.4, -1.0, -0.5)
    sun.intensity = 0.8
    sun.shadows = true

    local spot = keep(Light(scene))
    spot.type = LightType.SPOT
    spot.position = Vector3(0, 7, 2)
    spot:setDirection(0, -1, -0.15)
    spot.intensity = 90
    spot.range = 15
    spot:setConeAngle(25, 45)
    spot:setSpotMask("mask.png")
    spot.shadows = true

    -- 24 moving point and spot lights: three times the cap, each shading option
    self.lights = {}
    for i = 1, 24 do
        local light = keep(Light(scene))
        local spotLight = (i % 4 == 0)
        light.type = spotLight and LightType.SPOT or LightType.POINT
        local c = COLORS[(i - 1) % #COLORS + 1]
        light:setColor(c[1], c[2], c[3])
        light.intensity = spotLight and 40 or 12
        light.range = spotLight and 9 or 5
        if spotLight then
            light:setConeAngle(25, 40)
            light:setDirection(0, -1, 0)
        end
        light.shading = SHADINGS[(i - 1) % 3 + 1]
        self.lights[i] = { light = light, phase = i * 0.9, radius = 2.5 + (i % 5) * 1.3, height = 1.2 + (i % 3) * 0.6 }
    end

    -- leaves out layer 5: the white sphere at the back right stays unlit by it
    local excluding = keep(Light(scene))
    excluding.type = LightType.POINT
    excluding.position = Vector3(5.5, 1.5, -4.0)
    excluding:setColor(1.0, 0.1, 0.1)
    excluding.intensity = 25
    excluding.range = 5
    excluding.cullingMask = 0xFFFFFFFF & ~(1 << 5)

    -- a tiled floor: each tile picks the lights near it
    for x = -2, 1 do
        for z = -2, 1 do
            local tile = keep(Primitive(scene))
            tile:createPlane(5, 5)
            tile.position = Vector3(x * 5 + 2.5, 0, z * 5 + 2.5)
            tile:setColor(0.6, 0.6, 0.62)
        end
    end

    -- rows of objects lit per pixel, mixed and per vertex
    local rows = {
        { lighting = MaterialLighting.PIXEL, z = 3.0 },
        { lighting = MaterialLighting.MIXED, z = 0.0 },
        { lighting = MaterialLighting.VERTEX, z = -3.0 },
    }
    for _, row in ipairs(rows) do
        -- dielectric (Primitive materials are metallic, which has no diffuse for vertex lights)
        local sphere = keep(Model(scene))
        if not sphere:loadModel("sphere.gltf") then
            fail("sphere.gltf did not load")
        end
        sphere.position = Vector3(-4.0, 0.8, row.z)
        setLighting(sphere, row.lighting)

        local cube = keep(Primitive(scene))
        cube:createBox(1.3, 1.3, 1.3)
        cube.position = Vector3(-1.4, 0.65, row.z)
        cube:setColor(0.95, 0.8, 0.5)
        setLighting(cube, row.lighting)

        local cylinder = keep(Primitive(scene))
        cylinder:createCylinder(0.6, 1.6)
        cylinder.position = Vector3(1.2, 0.8, row.z)
        cylinder:setColor(0.5, 0.8, 0.95)
        setLighting(cylinder, row.lighting)

        -- normal map, vertex colors and no tangents, from a model file
        local bumpy = keep(Model(scene))
        if not bumpy:loadModel("bumpy_box.gltf") then
            fail("bumpy_box.gltf did not load")
        end
        bumpy.position = Vector3(3.6, 0.8, row.z)
        setLighting(bumpy, row.lighting)
    end

    -- an instanced batch is one object for its lights
    local rings = keep(Primitive(scene))
    rings:createTorus(0.4, 0.13)
    rings:setColor(0.95, 0.95, 0.95)
    rings:createInstancedMesh()
    for i = 0, 7 do
        local a = i * math.pi / 4
        rings:addInstance(Vector3(6.5 + math.cos(a) * 1.2, 0.4 + (i % 2) * 0.3, 2.5 + math.sin(a) * 1.2))
    end
    setLighting(rings, MaterialLighting.MIXED)

    -- on layer 5, next to the excluding light
    local excluded = keep(Model(scene))
    if not excluded:loadModel("sphere.gltf") then
        fail("sphere.gltf did not load")
    end
    excluded.position = Vector3(6.5, 0.8, -3.5)
    excluded.layer = 5

    -- a fork written before per-object lights: the engine fills its u_fs_lighting
    local forked = keep(Primitive(scene))
    forked:createCylinder(0.5, 1.4)
    forked.position = Vector3(-6.5, 0.7, 0.0)
    forked:setColor(0.9, 0.9, 0.9)
    forked.customShader = "assets/shaders/tinted"

    log("%d lights, cap %d", #self.lights + 3, scene.maxLightsPerObject)
end

function UnifiedLighting:onUpdate()
    local scene = self.scene
    self.frames = self.frames + 1
    self.time = self.time + Engine.deltatime

    for _, entry in ipairs(self.lights) do
        local a = self.time * 0.6 + entry.phase
        entry.light.position = Vector3(math.cos(a) * entry.radius, entry.height + math.sin(a * 1.7) * 0.4, math.sin(a) * entry.radius * 0.8)
    end

    -- a capacity tier change reloads every lit mesh; nothing may fail or go missing
    if self.stage == 0 and self.time >= 2.0 then
        self.stage = 1
        scene.maxLightsPerObject = 4
        log("frame %d: cap %d", self.frames, scene.maxLightsPerObject)
    elseif self.stage == 1 and self.time >= 4.0 then
        self.stage = 2
        scene.maxLightsPerObject = 8
        log("frame %d: cap %d", self.frames, scene.maxLightsPerObject)
    end

    if self.frames % 100 == 0 then
        log("frame %d (%.2f s): %.0f fps, %d variants compiling", self.frames, self.time, Engine.framerate,
            ShaderPool.getPendingShaderCount())
    end
end

return UnifiedLighting
