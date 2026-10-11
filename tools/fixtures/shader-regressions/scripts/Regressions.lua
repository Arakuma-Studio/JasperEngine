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
--   6. alpha, right of the front row over a checker wall: built-in Lit objects whose
--      MaterialAlphaMode AUTO takes transparency from the base texture's alpha. A Sprite with
--      its own image (a ring), two Sprites sharing the character image with the library sprite
--      of 5, and two quads sharing a third use of it (a user that found the GPU texture already
--      made had no image data, read opaque and wrote a dark square): every one shows its shape
--      over the checker, no square.
--   7. leak, far right: a model of two submeshes whose second waits for its shader (and, the
--      last time, its texture) while the first is ready; made and destroyed again, each time
--      the live pipeline count goes back to the count before it (a load waiting for a later
--      submesh left the first submesh's pipelines behind on every try). Runs once the scene
--      has settled and logs "[SR] leak check passed"; then one stays: a flat orange box (the
--      library:unlit submesh) beside a shaded Lit one.
--   8. a second Skybox (red), created after the procedural sky of 1: only the first added is
--      the scene's sky (logged as a warning), so the sky stays blue and nothing is red.
--   9. fork paths: the orange pillar of 5 names its fork assets-relative ("shaders/no_shadow"),
--      the green one project-relative ("assets/shaders/no_shadow"); project.yaml's
--      shaderAdditions makes the export precompile a variant of each (it could not find the
--      first), and the game compiles neither at runtime.
--  10. probe order, seen only in 1's reflection (behind the cameras, facing the sphere): a red
--      panel that shows only inside a stencil mask (library:stencil_mask, sortPriority -1)
--      created after it. The probe capture drew its opaque meshes in transform order, so the
--      panel tested the stencil before the mask wrote it and vanished from the reflection: a
--      red square in the reflection, left of the pillars' (SR_FOCUS=probe; the main view and
--      the close-up see the panel and the mask from behind, culled).
--
--   tools/smoke-test.sh tools/fixtures/shader-regressions
--   SMOKE_GAME_SECONDS="ready 70" tools/smoke-test.sh tools/fixtures/shader-regressions  (7 done)
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

    -- 8. a second Skybox, red: a scene has one sky, the first added (the engine warns once)
    local extraSky = Skybox(scene)
    extraSky:setColor(1.0, 0.0, 0.0)
    self.extraSky = extraSky

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

-- 10. a stencil reader created before its mask, reflected by the probe of 1 only
local function buildProbeOrder(self, scene)
    local function plane(z, size)
        local p = Primitive(scene)
        p:createPlane(size, size)
        -- facing the sphere (-Z): the main and focus cameras see their back, culled
        p:setRotation(-90, 0, 0)
        p.position = Vector3(-6.0, 1.4, z)
        p.castShadows = false
        p.receiveLights = false
        return p
    end
    local panel = plane(6.0, 4.0)
    panel:setColor(0.9, 0.1, 0.1)
    configure(panel, function(material)
        local s = MaterialStencil()
        s.compare = MaterialStencilCompare.EQUAL
        s.ref = 16
        s.readMask = 240
        s.writeMask = 240
        material.stencil = s
    end)
    local mask = plane(5.8, 1.6)
    configure(mask, function(material)
        material.shader = "library:stencil_mask"
        material.sortPriority = -1
    end)
    self.probePanel, self.probeMask = panel, mask
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
        -- the same fork by its project-relative path (9)
        material.shader = "assets/shaders/no_shadow"
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

local function buildAlpha(self, scene)
    local wall = Primitive(scene)
    wall:createWall(4.8, 1.8, 3)
    wall.position = Vector3(2.65, 0.9, 2.5)
    wall:setTexture("textures/checker.png")
    wall.receiveLights = false

    local function sprite(x, image)
        local s = Sprite(scene)
        s:setTexture(image)
        s:setSize(64, 64)
        s:setScale(0.9 / 64)
        s.pivotPreset = PivotPreset.CENTER
        s.castShadows = false
        s.position = Vector3(x, 0.9, 3.0)
        return s
    end
    local function quad(x, image)
        local q = Primitive(scene)
        q:createWall(0.9, 0.9)
        q.position = Vector3(x, 0.9, 3.0)
        q:setTexture(image)
        q.castShadows = false
        return q
    end
    self.alphaObjects = {
        sprite(0.85, "textures/ring_alpha.png"),
        sprite(1.75, "textures/character.png"),
        sprite(2.65, "textures/character.png"),
        quad(3.55, "textures/character.png"),
        quad(4.45, "textures/character.png"),
    }
end

-- 7: one model of two submeshes per try; the second's shader is a variant nothing else uses
local LEAK_TRIES = {
    { "VERTEX_COLOR" },
    { "ALPHA_CLIP" },
    { "VERTEX_COLOR", "ALPHA_CLIP" },
    { texture = "textures/stripes.png" },
}

local function makeLeakModel(scene, try)
    local model = Model(scene)
    if not model:loadModel("models/two_parts.gltf") then
        fail("models/two_parts.gltf did not load")
        return nil
    end
    model.position = Vector3(5.7, 0.5, 3.6)
    model:setRotation(0, -20, 0)
    model.scale = Vector3(0.8, 0.8, 0.8)
    if not try then
        -- both submeshes Lit, as loaded
    elseif try.texture then
        -- Lit as the first submesh, with an image no one loaded yet
        local material = model:getMaterial(1)
        material.baseColorTexture:setPath(try.texture)
        model:setMaterial(1, material)
    else
        local material = model:getMaterial(1)
        material.shader = "library:unlit"
        material:setShaderParam("baseColor", Vector4(0.95, 0.55, 0.15, 1.0))
        for _, feature in ipairs(try) do
            material:setShaderFeature(feature, true)
        end
        model:setMaterial(1, material)
    end
    return model
end

-- the scene is settled when no shader has been compiling for a while
local function settled(self)
    if ShaderPool.getPendingShaderCount() > 0 then
        self.quietFrames = 0
        return false
    end
    self.quietFrames = (self.quietFrames or 0) + 1
    return self.quietFrames >= 30
end

local function updateLeak(self, scene)
    local leak = self.leak
    if leak.done or not settled(self) then
        return
    end
    local live = ObjectRender.getLivePipelineCount()
    if leak.state == "baseline" then
        -- a first model of the same layout, so the first submesh's variant is ready on every try
        if not leak.warm then
            leak.warm = makeLeakModel(scene, nil)
            if leak.warm then
                leak.warm.position = Vector3(0, -80, 0)
            end
            self.quietFrames = 0
            return
        end
        leak.baseline = live
        leak.try = 1
        leak.model = makeLeakModel(scene, LEAK_TRIES[1])
        leak.state = "loading"
        self.quietFrames = 0
    elseif leak.state == "loading" then
        log("leak try %d: %d pipelines over the baseline %d with the model", leak.try, live - leak.baseline, leak.baseline)
        GameObject.destroyImmediate(leak.model)
        leak.model = nil
        leak.state = "destroyed"
        self.quietFrames = 0
    elseif leak.state == "destroyed" then
        if live ~= leak.baseline then
            fail("leak try %d: %d pipelines after the model was destroyed, %d before it (%d left behind)",
                 leak.try, live, leak.baseline, live - leak.baseline)
            leak.done = true
            return
        end
        if leak.try == #LEAK_TRIES then
            log("leak check passed (%d tries)", #LEAK_TRIES)
            -- the one left in the scene
            leak.model = makeLeakModel(scene, LEAK_TRIES[1])
            leak.done = true
            return
        end
        leak.try = leak.try + 1
        leak.model = makeLeakModel(scene, LEAK_TRIES[leak.try])
        leak.state = "loading"
        self.quietFrames = 0
    end
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

    for _, case in ipairs({ {"probe", buildProbe}, {"prepass", buildPrepass}, {"rtt", buildRtt}, {"terrain", buildTerrain}, {"front", buildFront}, {"alpha", buildAlpha}, {"probe order", buildProbeOrder} }) do
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
                      front = Vector3(-3.2, 0.8, 3.0), alpha = Vector3(2.65, 0.9, 3.0),
                      leak = Vector3(5.7, 0.5, 3.6) }
    if focus and targets[focus] then
        local target = targets[focus]
        camera.target = target
        camera.position = target + Vector3(0.0, 1.0, 5.0)
        log("focus on %s", focus)
    end
    self.frames = 0
    self.leak = { state = "baseline" }
end

function Regressions:onUpdate()
    self.frames = self.frames + 1
    updateLeak(self, self.scene)
    if self.frames % 100 == 0 then
        log("frame %d: %d variants compiling", self.frames, ShaderPool.getPendingShaderCount())
    end
end

return Regressions
