-- Shadows (spec S10): the bench scene of docs/rendering/shadow-budgets.md, built at runtime so
-- every variant compiles in the game: 1 sun, 4 spot lights, 2 point lights, 2,000 static
-- casters (staticShadowCaster: cached) and 20 animated skinned/morphing dynamic casters.
--   tools/smoke-test.sh tools/fixtures/shadows
--
-- SHADOWS_WALK=1 walks the camera at 5 m/s (the "walking camera" budget row); without it the
-- camera holds still (the "static scene and camera" row). SHADOWS_QUALITY=low|medium|high|ultra
-- forces a tier; SHADOWS_STATIC=<n> and SHADOWS_DYNAMIC=<n> set the static and dynamic caster
-- counts (default 2000 and 20: with 0 dynamic casters and a still camera every shadow slot is
-- cached, and the frame has no shadow pass at all; JASPERENGINE_FRAME_STATS=1 lists its passes).
-- SHADOWS_LIGHTS=sun keeps only the sun and SHADOWS_SUN=none|hard|soft sets its shadow (default
-- soft): the "receiver, full-screen sun" row is the camera pass with soft against none.
-- Every 120 frames the game logs its frame rate and the shadow tier it renders.
local Shadows = { properties = {} }

local function log(fmt, ...)
    print("[SH] " .. string.format(fmt, ...))
end

local function fail(fmt, ...)
    -- the smoke test fails on an [ERROR] in the game log
    print("[ERROR] [SH] " .. string.format(fmt, ...))
end

local QUALITIES = { low = ShadowQuality.LOW, medium = ShadowQuality.MEDIUM, high = ShadowQuality.HIGH, ultra = ShadowQuality.ULTRA }

function Shadows:init()
    RegisterEngineEvent(self, "onUpdate")
    local scene = self.scene

    self.time = 0
    self.frames = 0
    self.walk = os.getenv("SHADOWS_WALK") == "1"
    -- a GameObject made here lives as long as its Lua object: keep them all
    self.objects = {}
    local function keep(object)
        table.insert(self.objects, object)
        return object
    end

    local quality = QUALITIES[os.getenv("SHADOWS_QUALITY") or ""]
    if quality then
        scene.shadowQuality = quality
    end
    scene:setGlobalIllumination(0.15, Vector3(0.6, 0.65, 0.8))
    scene.backgroundColor = Vector4(0.42, 0.55, 0.72, 1.0)

    local camera = keep(Camera(scene))
    camera.type = CameraType.CAMERA_PERSPECTIVE
    camera.position = Vector3(0, 6, 22)
    camera.target = Vector3(0, 1, 0)
    scene:setCamera(camera)
    self.camera = camera

    local sun = keep(Light(scene))
    sun.type = LightType.DIRECTIONAL
    sun:setDirection(-0.45, -1.0, -0.35)
    sun.intensity = 2.5
    local SUN_SHADOWS = { none = ShadowType.NONE, hard = ShadowType.HARD, soft = ShadowType.SOFT }
    sun.shadowType = SUN_SHADOWS[os.getenv("SHADOWS_SUN") or ""] or ShadowType.SOFT
    local otherLights = os.getenv("SHADOWS_LIGHTS") ~= "sun"

    for i = 0, otherLights and 3 or -1 do
        local spot = keep(Light(scene))
        spot.type = LightType.SPOT
        spot.position = Vector3(-12 + i * 8, 7, -4 + (i % 2) * 8)
        spot:setDirection(0.1, -1, 0.05)
        spot.intensity = 120
        spot.range = 18
        spot:setConeAngle(30, 45)
        spot.shadowType = (i % 2 == 0) and ShadowType.SOFT or ShadowType.HARD
    end

    for i = 0, otherLights and 1 or -1 do
        local point = keep(Light(scene))
        point.type = LightType.POINT
        point.position = Vector3(-6 + i * 12, 3.5, 6)
        point:setColor(1.0, 0.85, 0.6)
        point.intensity = 60
        point.range = 12
        point.shadowType = ShadowType.SOFT
    end

    local ground = keep(Primitive(scene))
    ground:createPlane(160, 160)
    ground:setColor(0.55, 0.57, 0.52)
    ground.staticShadowCaster = true

    -- static casters on a grid (pillars, boxes), cached in the static shadow layer
    local count = tonumber(os.getenv("SHADOWS_STATIC") or "") or 2000
    local side = math.ceil(math.sqrt(count))
    for n = 0, count - 1 do
        local x = (n % side) - side * 0.5
        local z = math.floor(n / side) - side * 0.5
        local caster = keep(Primitive(scene))
        if n % 3 == 0 then
            caster:createCylinder(0.25, 1.6)
            caster.position = Vector3(x * 1.6, 0.8, z * 1.6)
        else
            caster:createBox(0.6, 0.9, 0.6)
            caster.position = Vector3(x * 1.6, 0.45, z * 1.6)
        end
        caster:setColor(0.7 + (n % 5) * 0.05, 0.7, 0.66)
        caster.staticShadowCaster = true
    end

    -- dynamic casters: skinned and morphing, moving every frame
    self.dynamic = {}
    local dynamicCount = tonumber(os.getenv("SHADOWS_DYNAMIC") or "") or 20
    for i = 0, dynamicCount - 1 do
        local model = keep(Model(scene))
        if not model:loadModel("skinned_morph.gltf") then
            fail("skinned_morph.gltf did not load")
            break
        end
        table.insert(self.dynamic, { model = model, phase = i * 0.7, radius = 3 + (i % 5) * 1.5 })
    end
    log("bench: %d static casters, %d dynamic, quality %s", count, #self.dynamic, os.getenv("SHADOWS_QUALITY") or "auto")
end

function Shadows:onUpdate()
    local scene = self.scene
    self.frames = self.frames + 1
    self.time = self.time + Engine.deltatime

    for _, d in ipairs(self.dynamic) do
        local a = self.time * 0.6 + d.phase
        d.model.position = Vector3(math.cos(a) * d.radius, 1.0 + 0.3 * math.sin(a * 2.0), math.sin(a) * d.radius)
        d.model:setMorphWeight(0, 0.5 + 0.5 * math.sin(a * 3.0))
    end

    if self.walk then
        local z = 22 - (self.time * 5.0) % 40
        self.camera.position = Vector3(0, 6, z)
        self.camera.target = Vector3(0, 1, z - 22)
    end

    if self.frames % 120 == 0 then
        log("frame %d (%.2f s): %.0f fps, tier %d", self.frames, self.time, Engine.framerate, scene.effectiveShadowTier)
    end
end

return Shadows
