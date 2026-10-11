-- Regression scene of the shader library engine fixes, one screenshot, left to right:
--
--   1. probe: a chrome sphere lit only by a reflection probe that captures the
--      library:procedural_sky (the sky's frame matrices were flipped in the capture, so the
--      reflection showed the ground on top): blue sky on the upper half, dark ground below.
--   2. prepass: with the camera depth pre-pass on, a Lit box (red) and a library:unlit box
--      (green) whose materials test depth LESS (they failed against their own pre-pass
--      depth and vanished): both visible.
--   3. rtt: a render-to-texture of an opaque library:unlit box writing alpha 0.3 over a
--      grey wall, shown alpha blended on a quad in front of a magenta wall (the opaque
--      pipeline left alpha 0.3 in the target: a magenta hole): an orange box, no magenta.
--   4. terrain: a Terrain drawn with library:simple_lit and a checker base map (library
--      shaders drew flat, unscaled patches): a lit, height-mapped checkered hill.
--   5. front row, on a ground: two pillars, the left Lit, the right a fork whose manifest
--      says @state shadows=off (only the left casts a shadow); a third with the fork's
--      GLOW feature (@state[GLOW] blend=additive: see-through); and a Sprite whose
--      library:sprite shader is set right after it is created, over its submeshes (a new
--      Sprite reported none before its first update, so the shader was dropped).
--
--   tools/smoke-test.sh tools/fixtures/shader-regressions
--   SMOKE_GAME_ENV="SR_FOCUS=probe" tools/smoke-test.sh tools/fixtures/shader-regressions
local Regressions = { properties = {} }

local function log(fmt, ...)
    print("[SR] " .. string.format(fmt, ...))
end

local function fail(fmt, ...)
    print("[ERROR] [SR] " .. string.format(fmt, ...))
end

-- the material of submesh 0, changed by configure, set back
local function configure(mesh, apply)
    local material = mesh:getMaterial(0)
    apply(material)
    mesh:setMaterial(0, material)
end

local function buildProbe(self, scene)
    local sky = Skybox(scene)
    sky.customShader = "library:procedural_sky"
    self.sky = sky

    local sphere = Model(scene)
    if not sphere:loadModel("models/chrome_sphere.gltf") then
        fail("models/chrome_sphere.gltf did not load")
        return
    end
    sphere.position = Vector3(-6.0, 1.4, 0.0)
    sphere.scale = Vector3(1.4, 1.4, 1.4)
    sphere.renderInReflectionProbes = false
    sphere.receiveIBL = true
    self.sphere = sphere

    local probe = ReflectionProbe(scene)
    probe.position = Vector3(-6.0, 1.4, 0.0)
    probe.resolution = 128
    probe.includeSky = true
    probe:setBoxSize(40, 40, 40)
    self.probe = probe
end

local function buildPrepass(self, scene)
    scene.depthPrepassEnabled = true

    local lit = Primitive(scene)
    lit:createBox(1.2, 1.2, 1.2)
    lit.position = Vector3(-2.6, 0.8, 0.0)
    lit:setColor(0.85, 0.15, 0.1)
    configure(lit, function(material)
        material.depthTest = MaterialDepthTest.LESS
    end)

    local unlit = Primitive(scene)
    unlit:createBox(1.2, 1.2, 1.2)
    unlit.position = Vector3(-1.0, 0.8, 0.0)
    configure(unlit, function(material)
        material.shader = "library:unlit"
        material:setShaderParam("baseColor", Vector4(0.1, 0.8, 0.2, 1.0))
        material.depthTest = MaterialDepthTest.LESS
    end)
    self.prepassLit, self.prepassUnlit = lit, unlit
end

local function buildRtt(self, scene)
    -- what the texture camera sees, out of the main camera's view
    local wall = Primitive(scene)
    wall:createPlane(10, 10)
    wall.position = Vector3(0, -50, -1)
    wall:setRotation(90, 0, 0)
    wall:setColor(0.5, 0.5, 0.5)
    wall.receiveLights = false

    local box = Primitive(scene)
    box:createBox(1.2, 1.2, 1.2)
    box.position = Vector3(0, -50, 0.5)
    box:setRotation(20, 30, 0)
    configure(box, function(material)
        material.shader = "library:unlit" -- @state blend=opaque
        material:setShaderParam("baseColor", Vector4(1.0, 0.5, 0.05, 0.3))
    end)

    local camera = Camera(scene)
    camera.type = CameraType.CAMERA_PERSPECTIVE
    camera.position = Vector3(0, -50, 4)
    camera.target = Vector3(0, -50, 0)
    camera.renderToTexture = true
    camera:setFramebufferSize(256, 256)

    -- the texture, alpha blended, in front of a magenta wall
    local backdrop = Primitive(scene)
    backdrop:createBox(2.6, 2.6, 0.1)
    backdrop.position = Vector3(2.4, 1.4, -0.4)
    backdrop:setColor(1.0, 0.0, 1.0)
    backdrop.receiveLights = false

    local screen = Primitive(scene)
    screen:createBox(2.2, 2.2, 0.05)
    screen.position = Vector3(2.4, 1.4, 0.0)
    screen.receiveLights = false
    screen:setTexture(camera:getFramebuffer())
    configure(screen, function(material)
        material.blendMode = MaterialBlendMode.ALPHA
    end)
    self.rttWall, self.rttBox, self.rttCamera, self.rttBackdrop, self.rttScreen = wall, box, camera, backdrop, screen
end

local function buildTerrain(self, scene)
    local terrain = Terrain(scene)
    terrain:setHeightMap("textures/noise.png")
    terrain.size = 6
    terrain.maxHeight = 1.2
    terrain.textureBaseTiles = 4
    terrain.position = Vector3(7.0, 0.0, -1.0)
    configure(terrain, function(material)
        material.shader = "library:simple_lit"
        material:setShaderTexture("u_baseTexture", "textures/checker.png")
        material:setShaderParam("baseColor", Vector4(0.9, 0.85, 0.7, 1.0))
    end)
    self.terrain = terrain
end

local function buildFront(self, scene)
    local ground = Primitive(scene)
    ground:createPlane(8, 3)
    ground.position = Vector3(-3.0, 0.0, 3.2)
    ground:setColor(0.75, 0.75, 0.72)

    local function pillar(x, configureShader)
        local p = Primitive(scene)
        p:createBox(0.5, 1.6, 0.5)
        p.position = Vector3(x, 0.8, 3.0)
        p:setColor(0.3, 0.45, 0.8)
        if configureShader then
            configure(p, configureShader)
        end
        return p
    end
    self.litPillar = pillar(-5.5)
    self.noShadowPillar = pillar(-4.0, function(material)
        material.shader = "shaders/no_shadow"
        material:setShaderParam("baseColor", Vector4(0.9, 0.6, 0.1, 1.0))
    end)
    self.glowPillar = pillar(-2.5, function(material)
        material.shader = "shaders/no_shadow"
        material:setShaderParam("baseColor", Vector4(0.2, 0.9, 0.4, 1.0))
        material:setShaderFeature("GLOW", true)
    end)

    local sprite = Sprite(scene)
    sprite:setTexture("textures/character.png")
    sprite:setSize(64, 64)
    sprite:setScale(1.5 / 64)
    sprite.pivotPreset = PivotPreset.CENTER
    sprite.castShadows = false
    sprite.receiveLights = false
    sprite.position = Vector3(-1.0, 0.8, 3.0)
    local count = sprite:getNumSubmeshes()
    if count == 0 then
        fail("a new Sprite reports no submesh: its material shader would be dropped")
    end
    for i = 0, count - 1 do
        local material = sprite:getMaterial(i)
        material.shader = "library:sprite"
        material:setShaderFeature("GRAYSCALE", true)
        sprite:setMaterial(i, material)
    end
    log("new sprite: %d submesh(es)", count)
    self.sprite = sprite
end

function Regressions:init()
    RegisterEngineEvent(self, "onUpdate")
    local scene = self.scene

    local camera = Camera(scene)
    camera.type = CameraType.CAMERA_PERSPECTIVE
    camera.position = Vector3(0.5, 5.0, 11.0)
    camera.target = Vector3(0.5, 0.8, 0.0)
    scene:setCamera(camera)

    local light = Light(scene)
    light.type = LightType.DIRECTIONAL
    light:setDirection(-0.4, -1.0, -0.5)
    light.intensity = 2.5
    light.shadows = true
    self.camera, self.light = camera, light

    for _, case in ipairs({ {"probe", buildProbe}, {"prepass", buildPrepass}, {"rtt", buildRtt}, {"terrain", buildTerrain}, {"front", buildFront} }) do
        local ok, err = pcall(case[2], self, scene)
        if ok then
            log("built %s", case[1])
        else
            fail("%s failed to build: %s", case[1], tostring(err))
        end
    end
    -- SR_FOCUS=probe|prepass|rtt|terrain|front: a close-up of one case
    local focus = os.getenv("SR_FOCUS")
    local targets = { probe = Vector3(-6.0, 1.4, 0.0), prepass = Vector3(-1.8, 0.8, 0.0),
                      rtt = Vector3(2.4, 1.4, 0.0), terrain = Vector3(7.0, 0.5, -1.0),
                      front = Vector3(-3.2, 0.8, 3.0) }
    if focus and targets[focus] then
        local target = targets[focus]
        camera.target = target
        camera.position = target + Vector3(0.0, 1.0, 5.0)
        log("focus on %s", focus)
    end
    self.frames = 0
end

function Regressions:onUpdate()
    self.frames = self.frames + 1
    if self.frames % 100 == 0 then
        log("frame %d: %d variants compiling", self.frames, ShaderPool.getPendingShaderCount())
    end
end

return Regressions
