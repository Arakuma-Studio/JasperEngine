-- Scene inputs fixture (shader library phase 3): fixture-only fork shaders reading the
-- opaque scene's depth and colour through includes/scene.glsl, every API function:
--
--   shaders/si_world_grid   opaque ground and bumps with white lines on the integer world grid
--   shaders/si_depth_grid   a box volume (left) drawing thin red lines on the same grid, from
--                           the world position it reconstructs from the scene depth
--                           (sceneWorldPosition): red inside white when it is right
--   shaders/si_intersection force-field glow where a surface meets the ground (sceneLinearDepth,
--                           fragmentLinearDepth, screenUV): a sphere sunk in the ground, a row of
--                           instanced cubes, and the extra pass of a faint blue sphere
--   shaders/si_refraction   glass sphere and box over coloured blocks (sceneColor offset by the
--                           normal; samples landing on something in front fall back)
--   shaders/si_haze         a heat-haze sprite wobbling the blocks behind it (sceneColor)
--   shaders/si_soft_points  orange points crossing the ground, fading where they meet it
--
-- A render-to-texture camera shows the same area from the right on the screen at the top
-- right. Without these shaders nothing here makes a depth pre-pass or a colour copy.
--
--   tools/smoke-test.sh tools/fixtures/scene-inputs
--   SI_ORTHO=1        orthographic main camera
--   SI_FIXED=1        fixed resolution 640x360
--   SI_PREPASS=1      the camera depth pre-pass setting on
--   SI_SSAO=1         SSAO on (its depth pre-pass is the scene depth)
--   SI_SSR=1          SSR on (its G-buffer depth is the scene depth)
--   SI_FULL=1         Scene Color Scale full (half by default)
--   SI_POST=1         a post-process pass sampling the depth (shaders/si_post_depth): its depth
--                     pre-pass is the scene depth (the sky darkened a little shows it ran)
--   SI_2D=1           the main camera draws in hierarchy order (transparentSort off): the
--                     colour copy is taken right before the first draw reading it
--   SI_INVISIBLE=1    the glass shows the background unchanged (strength 0, white, no rim)
--   SI_MIRROR=1       a mirror on the left reflecting the depth grid
--   SI_SCENE=identity no scene-input shader: 6 lights (directional and spot with shadows,
--                     three points, a second spot), fog; compare with a build without phase 3
--   SI_SCENE=hidden   the identity scene plus a glass sphere behind the camera: no pass reads
--                     the scene, so no depth pre-pass or copy runs (the log says so)
--   SI_SCENE=saved    Saved.scene (generate.py): the shaders on GameObjects saved in a scene, as
--                     the editor's views draw them and the export collects their variants
--   SI_SPAWN=depth    the identity scene; a reader of the scene depth (si_intersection) made in
--                     view at frame 60, destroyed at 100, made again at 140, destroyed at 180.
--   SI_SPAWN=color    the same with a reader of the scene colour (si_haze). Run with
--                     JASPERENGINE_FRAME_STATS=all: each spawn draws every mesh (no frame with
--                     only the sky and the transparent ones) and remakes no pipeline but its own
--                     and the depth programs the first spawn adds (no reload of every mesh on
--                     each spawn or destruction)
--   SI_RESIZE=1       the render-to-texture camera's target changes size every frame (a window
--                     drag): the scene-input targets of the old sizes go (images alive stay level)
--   SI_SPLIT=1        the identity scene plus models/figure.gltf (one mesh, three submeshes) whose
--                     torso shows the scene behind it tinted red (si_refraction, blended, no
--                     offset) while its backpack and head stay opaque: they draw in the opaque
--                     queue, so the green backpack behind the torso shows through it; and a library:spin_bob box (a @depth program moving it) in
--                     front: with SI_SSR=1 the pass makes its own depth pre-pass instead of
--                     taking the G-buffer's ("0 shared depths" in the Scene inputs line)
--   SI_ENV=1          the identity scene under a sky with a cube map (textures/sky_cross.png:
--                     blue above, brown below): mirror balls (shaders/si_environment, sharp and
--                     rough) reflecting the engine's environment (@require environment; magenta
--                     without one), and library:glass with REFLECTION_MAP given the same cube map
--                     from Lua (Material:setShaderCubeTexture)
local SceneInputs = { properties = {} }

local function log(fmt, ...)
    print("[SI] " .. string.format(fmt, ...))
end

local function fail(fmt, ...)
    print("[ERROR] [SI] " .. string.format(fmt, ...))
end

local function env(name)
    return os.getenv(name) == "1"
end

-- the material of every submesh, changed by apply, set back
local function configure(mesh, apply)
    local count = mesh:getNumSubmeshes()
    if count == 0 then
        fail("mesh has no submesh to configure")
    end
    for i = 0, count - 1 do
        local material = mesh:getMaterial(i)
        apply(material)
        mesh:setMaterial(i, material)
    end
end

local function unlit(material, r, g, b, a)
    material.shader = "library:unlit"
    material:setShaderParam("baseColor", Vector4(r, g, b, a or 1.0))
end

local function grid(material)
    material.shader = "shaders/si_world_grid"
end

local function buildInputs(self, scene, keep)
    scene.backgroundColor = Vector4(0.08, 0.1, 0.16, 1.0)

    local sun = keep(Light(scene))
    sun.type = LightType.DIRECTIONAL
    sun:setDirection(-0.3, -1.0, -0.4)
    sun.intensity = 1.5
    sun.shadows = true

    -- the ground and bumps with the world grid
    local ground = keep(Primitive(scene))
    ground:createPlane(22, 16)
    ground.position = Vector3(0, 0, 0)
    configure(ground, grid)
    local tilted = keep(Primitive(scene))
    tilted:createBox(1.8, 1.8, 1.8)
    tilted.position = Vector3(-5.2, 0.4, 0.2)
    tilted:setRotation(25, 30, 15)
    configure(tilted, grid)
    local bump = keep(Primitive(scene))
    bump:createSphere(1.1)
    bump.position = Vector3(-2.6, 0.0, 1.2)
    configure(bump, grid)

    -- the depth grid volume around them
    local boxMin = Vector3(-7.4, -0.5, -2.6)
    local boxMax = Vector3(-0.8, 3.0, 3.4)
    local volume = keep(Primitive(scene))
    volume:createBox(boxMax.x - boxMin.x, boxMax.y - boxMin.y, boxMax.z - boxMin.z)
    volume.position = Vector3((boxMin.x + boxMax.x) * 0.5, (boxMin.y + boxMax.y) * 0.5, (boxMin.z + boxMax.z) * 0.5)
    volume.castShadows = false
    configure(volume, function(m)
        m.shader = "shaders/si_depth_grid"
        m:setShaderParam("boxMin", boxMin)
        m:setShaderParam("boxMax", boxMax)
    end)

    -- coloured blocks behind the glass and the haze
    local colors = { {0.95, 0.2, 0.15}, {0.95, 0.75, 0.1}, {0.2, 0.75, 0.3}, {0.2, 0.45, 0.95}, {0.75, 0.25, 0.9} }
    for i = 1, 5 do
        local block = keep(Primitive(scene))
        block:createBox(1.4, 3.2, 0.6)
        block.position = Vector3(-1.4 + i * 1.5, 1.6, -4.2)
        local c = colors[i]
        configure(block, function(m) unlit(m, c[1], c[2], c[3]) end)
    end

    -- intersection glow: a sphere sunk in the ground
    local glow = keep(Primitive(scene))
    glow:createSphere(1.3)
    glow.position = Vector3(1.4, 0.35, 1.6)
    configure(glow, function(m) m.shader = "shaders/si_intersection" end)

    -- the same shader on an instanced batch of cubes crossing the ground
    local cubes = keep(Primitive(scene))
    cubes:createBox(0.8, 0.8, 0.8)
    cubes:createInstancedMesh()
    for i = 0, 5 do
        cubes:addInstance(Vector3(-1.0 + i * 1.1, 0.15 + (i % 2) * 0.2, 4.6), Quaternion(i * 15, Vector3(0, 1, 0)), Vector3(1, 1, 1))
    end
    configure(cubes, function(m)
        m.shader = "shaders/si_intersection"
        m:setShaderParam("glowColor", Vector4(1.6, 0.6, 0.2, 1.0))
    end)

    -- and as the extra pass of a faint blended sphere
    local passSphere = keep(Primitive(scene))
    passSphere:createSphere(0.9)
    passSphere.position = Vector3(4.4, 0.25, 2.0)
    configure(passSphere, function(m)
        unlit(m, 0.3, 0.5, 1.0, 0.25)
        m.blendMode = MaterialBlendMode.ALPHA
        m.extraPass.shader = "shaders/si_intersection"
        m.extraPass:setShaderParam("glowColor", Vector4(0.6, 1.6, 0.4, 1.0))
    end)

    -- glass over the blocks
    local invisible = env("SI_INVISIBLE")
    local function glass(m)
        m.shader = "shaders/si_refraction"
        if invisible then
            m:setShaderParam("strength", 0.0)
            m:setShaderParam("tint", Vector4(1, 1, 1, 1))
            m:setShaderParam("rim", 0.0)
        end
    end
    local glassSphere = keep(Primitive(scene))
    glassSphere:createSphere(1.2)
    glassSphere.position = Vector3(1.9, 1.7, -2.4)
    glassSphere.castShadows = false
    configure(glassSphere, glass)
    local glassBox = keep(Primitive(scene))
    glassBox:createBox(1.6, 2.2, 0.5)
    glassBox.position = Vector3(5.0, 1.5, -2.6)
    glassBox:setRotation(0, 30, 10)
    glassBox.castShadows = false
    configure(glassBox, glass)

    -- heat haze: a sprite in front of the left blocks
    local haze = keep(Sprite(scene))
    haze:setTexture("textures/checker.png")
    haze:setSize(64, 64)
    haze:setScale(2.6 / 64)
    haze.pivotPreset = PivotPreset.CENTER
    haze.receiveLights = false
    haze.castShadows = false
    haze.position = Vector3(-0.6, 2.2, -3.2)
    configure(haze, function(m) m.shader = "shaders/si_haze" end)

    -- soft points crossing the ground: the lowest ones are cut by it with a soft edge
    local points = keep(Points(scene))
    points.customShader = "shaders/si_soft_points"
    points.position = Vector3(7.2, 0.0, 2.6)
    for i = 0, 8 do
        local x = (i % 3) * 1.0 - 1.0
        local z = math.floor(i / 3) * 1.0 - 1.0
        points:addPoint(Vector3(x, -0.1 + (i % 3) * 0.25, z), Vector4(1, 1, 1, 1), 1.0, 0.0)
    end

    -- a mirror at the left, facing the depth grid: its reflection camera reads the scene too
    -- (its depth pre-pass is drawn mirrored back so its faces keep their winding): the red
    -- lines stay inside the white ones in the reflection
    if env("SI_MIRROR") then
        local mirror = keep(Primitive(scene))
        mirror:createBox(0.1, 3.0, 7.0)
        mirror.position = Vector3(-8.0, 1.5, 1.0)
        mirror:setRotation(0, -40, 0)
        mirror.castShadows = false
        mirror:setAsMirror(Vector3(1, 0, 0))
        log("mirror on the left")
    end

    -- a render-to-texture camera from the right, on a screen at the top right
    local rtt = keep(Camera(scene))
    rtt.type = CameraType.CAMERA_PERSPECTIVE
    rtt.position = Vector3(9.0, 4.5, 4.0)
    rtt.target = Vector3(1.0, 0.8, 0.0)
    rtt.renderToTexture = true
    rtt:setFramebufferSize(512, 288)
    self.rtt = rtt
    local screen = keep(Primitive(scene))
    screen:createBox(5.6, 3.15, 0.05)
    screen.position = Vector3(6.6, 5.6, -5.0)
    screen.receiveLights = false
    screen.castShadows = false
    screen:setTexture(rtt:getFramebuffer())
end

-- no scene input: what a build without them draws the same
local function buildIdentity(self, scene, keep)
    scene.backgroundColor = Vector4(0.1, 0.12, 0.16, 1.0)

    local sun = keep(Light(scene))
    sun.type = LightType.DIRECTIONAL
    sun:setDirection(-0.4, -1.0, -0.6)
    sun.intensity = 1.2
    sun.shadows = true

    local spot = keep(Light(scene))
    spot.type = LightType.SPOT
    spot.position = Vector3(-2.0, 6.0, 3.0)
    spot:setDirection(0.3, -1.0, -0.4)
    spot.intensity = 60
    spot.range = 14
    spot:setConeAngle(20, 35)
    spot.shadows = true

    local pointColors = { {1.0, 0.3, 0.2}, {0.2, 0.5, 1.0}, {0.3, 1.0, 0.4} }
    for i = 1, 3 do
        local point = keep(Light(scene))
        point.type = LightType.POINT
        point.position = Vector3(-4.0 + i * 2.5, 1.2, 1.5 - i)
        local c = pointColors[i]
        point:setColor(c[1], c[2], c[3])
        point.intensity = 10
        point.range = 5
    end

    local spot2 = keep(Light(scene))
    spot2.type = LightType.SPOT
    spot2.position = Vector3(4.0, 5.0, 1.0)
    spot2:setDirection(-0.2, -1.0, -0.2)
    spot2:setColor(1.0, 0.85, 0.6)
    spot2.intensity = 40
    spot2.range = 10
    spot2:setConeAngle(25, 40)

    local fog = keep(Fog(scene))
    fog.type = FogType.LINEAR
    fog:setColor(0.1, 0.12, 0.16)
    fog:setLinearStartEnd(8, 30)

    local floor = keep(Primitive(scene))
    floor:createPlane(20, 20)
    floor:setColor(0.7, 0.7, 0.72)
    local wall = keep(Primitive(scene))
    wall:createBox(12.0, 4.0, 0.3)
    wall.position = Vector3(0.0, 2.0, -3.5)
    wall:setColor(0.8, 0.78, 0.72)
    local shapes = { {-3.0, "box", {0.9, 0.3, 0.2}}, {-1.0, "sphere", {0.3, 0.8, 0.4}}, {1.0, "box", {0.3, 0.4, 0.9}}, {3.0, "sphere", {0.9, 0.8, 0.3}} }
    for i, shape in ipairs(shapes) do
        local object = keep(Primitive(scene))
        if shape[2] == "box" then
            object:createBox(1.2, 1.2 + i * 0.3, 1.2)
            object.position = Vector3(shape[1], (1.2 + i * 0.3) * 0.5, -0.5)
            object:setRotation(0, i * 20, 0)
        else
            object:createSphere(0.7)
            object.position = Vector3(shape[1], 0.7, 0.5)
        end
        object:setColor(shape[3][1], shape[3][2], shape[3][3])
    end
    -- an opaque box casting no shadow: no depth program from the shadows alone
    local noShadow = keep(Primitive(scene))
    noShadow:createBox(0.8, 0.8, 0.8)
    noShadow.position = Vector3(4.6, 0.4, 1.6)
    noShadow.castShadows = false
    noShadow:setColor(0.8, 0.5, 0.9)

    -- a translucent pane (transparent queue, no scene input)
    local pane = keep(Primitive(scene))
    pane:createBox(2.0, 1.5, 0.05)
    pane.position = Vector3(0.0, 1.0, 1.8)
    pane.castShadows = false
    configure(pane, function(m) unlit(m, 0.4, 0.8, 1.0, 0.35); m.blendMode = MaterialBlendMode.ALPHA end)

    if env("SI_SPLIT") then
        local figure = keep(Model(scene))
        if figure:loadModel("models/figure.gltf") then
            figure.position = Vector3(-4.4, 0.0, 2.4)
            figure.castShadows = false
            local count = figure:getNumSubmeshes()
            if count ~= 3 then
                fail("the figure has %d submeshes, not 3", count)
            end
            local refracting = false
            for i = 0, count - 1 do
                local material = figure:getMaterial(i)
                -- the torso's base colour (0.85, 0.75, 0.2): the model's material names are not kept
                local c = material.baseColorFactor
                if c.x > 0.8 and c.y > 0.7 and c.z < 0.3 then
                    material.shader = "shaders/si_refraction"
                    material:setShaderParam("strength", 0.0)
                    material:setShaderParam("rim", 0.0)
                    material:setShaderParam("tint", Vector4(1.0, 0.6, 0.6, 1.0))
                    figure:setMaterial(i, material)
                    refracting = true
                end
            end
            if not refracting then
                fail("the figure has no torso material (base colour 0.85, 0.75, 0.2)")
            end
        else
            fail("models/figure.gltf did not load")
        end
        local bob = keep(Primitive(scene))
        bob:createBox(0.6, 0.6, 0.6)
        bob.position = Vector3(2.2, 0.6, 2.4)
        configure(bob, function(m) m.shader = "library:spin_bob" end)
        log("split figure and a spin_bob box")
    end

    if env("SI_ENV") then
        local sky = keep(Skybox(scene))
        sky:setTexture("textures/sky_cross.png")
        for i, roughness in ipairs({0.0, 0.6}) do
            local ball = keep(Primitive(scene))
            ball:createSphere(0.8)
            ball.position = Vector3(-4.6 + i * 1.9, 2.6, 1.0)
            ball.castShadows = false
            configure(ball, function(m)
                m.shader = "shaders/si_environment"
                m:setShaderParam("roughness", roughness)
            end)
        end
        local glassBall = keep(Primitive(scene))
        glassBall:createSphere(0.8)
        glassBall.position = Vector3(1.6, 2.6, 1.0)
        glassBall.castShadows = false
        configure(glassBall, function(m)
            m.shader = "library:glass"
            m:setShaderFeature("REFLECTION_MAP", true)
            m:setShaderParam("reflectionColor", Vector4(1, 1, 1, 1))
            m:setShaderParam("reflectionStrength", 2.0)
            m:setShaderCubeTexture("u_reflectionTexture", "textures/sky_cross.png")
        end)
        log("environment balls under a cube map sky")
    end

    if os.getenv("SI_SCENE") == "hidden" then
        -- behind the camera: loaded, so the scene has an object reading it, but in no pass
        local hidden = keep(Primitive(scene))
        hidden:createSphere(1.0)
        hidden.position = Vector3(0.0, 4.0, 40.0)
        hidden.castShadows = false
        configure(hidden, function(m) m.shader = "shaders/si_refraction" end)
        log("a glass sphere behind the camera")
    end
end

function SceneInputs:init()
    RegisterEngineEvent(self, "onUpdate")
    local scene = self.scene
    self.objects = {}
    self.frames = 0
    -- the saved scene (generate.py): GameObjects from the scene file, no Lua
    if os.getenv("SI_SCENE") == "saved" then
        log("loading the saved scene")
        SceneManager.loadScene("Saved")
        return
    end
    local function keep(object)
        table.insert(self.objects, object)
        return object
    end

    local which = os.getenv("SI_SCENE") or "inputs"
    local camera = keep(Camera(scene))
    if env("SI_ORTHO") then
        camera:setOrtho(-9.6, 9.6, -5.4, 5.4, 0.1, 100.0)
        log("orthographic camera")
    else
        camera.type = CameraType.CAMERA_PERSPECTIVE
    end
    camera.position = Vector3(0.0, 6.5, 13.0)
    camera.target = Vector3(0.6, 0.9, 0.0)
    scene:setCamera(camera)
    self.camera = camera
    if env("SI_2D") then
        camera.transparentSort = false
        log("hierarchy-order camera")
    end

    if os.getenv("SI_SPAWN") or env("SI_ENV") or env("SI_SPLIT") then
        which = os.getenv("SI_SPAWN") and "spawn" or (env("SI_ENV") and "environment" or "split")
    end
    local build = (which == "identity" or which == "hidden" or which == "spawn" or which == "environment" or which == "split") and buildIdentity or buildInputs
    local ok, err = pcall(build, self, scene, keep)
    if ok then
        log("built %s scene", which)
    else
        fail("%s scene failed to build: %s", which, tostring(err))
    end

    if env("SI_FIXED") then
        scene.fixedResolutionEnabled = true
        scene:setFixedResolutionSize(640, 360)
        log("fixed resolution 640x360")
    end
    if env("SI_PREPASS") then
        scene.depthPrepassEnabled = true
        log("depth pre-pass on")
    end
    if env("SI_SSAO") then
        scene.ssaoEnabled = true
        log("SSAO on")
    end
    if env("SI_SSR") then
        scene.ssrEnabled = true
        log("SSR on")
    end
    if env("SI_POST") then
        local post = PostProcessPass()
        post.shader = "shaders/si_post_depth"
        scene.postProcessPasses = { post }
        log("depth-sampling post pass on")
    end
    if env("SI_FULL") then
        scene.sceneColorScale = SceneColorScale.FULL
        log("scene colour copy at full resolution")
    end
    self.frames = 0
end

-- SI_SPAWN: a reader made and destroyed while the scene runs
local function spawnReader(self)
    local kind = os.getenv("SI_SPAWN")
    local reader = Primitive(self.scene)
    if kind == "color" then
        reader:createBox(2.4, 1.6, 0.1)
        reader.position = Vector3(-1.5, 1.4, 2.2)
        configure(reader, function(m) m.shader = "shaders/si_haze" end)
    else
        reader:createSphere(1.0)
        reader.position = Vector3(-1.5, 0.3, 2.2)
        configure(reader, function(m) m.shader = "shaders/si_intersection" end)
    end
    reader.castShadows = false
    self.spawned = reader
    log("frame %d: %s reader made", self.frames, kind)
end

local function destroyReader(self)
    if self.spawned then
        local entity = self.spawned.entity
        self.spawned.entityOwned = false
        self.spawned = nil
        self.scene:destroyEntity(entity)
        log("frame %d: reader destroyed", self.frames)
    end
end

function SceneInputs:onUpdate()
    self.frames = self.frames + 1
    if os.getenv("SI_SPAWN") then
        if self.frames == 60 or self.frames == 140 then
            spawnReader(self)
        elseif self.frames == 100 or self.frames == 180 then
            destroyReader(self)
        end
    end
    if self.rtt and env("SI_RESIZE") then
        -- a size each frame, as a window being dragged
        local step = self.frames % 40
        self.rtt:setFramebufferSize(400 + step * 4, 225 + step * 2)
    end
    if self.frames % 100 == 0 then
        log("frame %d: %d variants compiling", self.frames, ShaderPool.getPendingShaderCount())
    end
end

return SceneInputs
