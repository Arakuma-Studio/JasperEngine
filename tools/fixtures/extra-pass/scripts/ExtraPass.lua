-- Extra pass fixture (shader library phase 2, Material.extraPass), one screenshot:
--
--   back row, left to right:
--     1. a hard-edged cube with an AFTER inverted-hull outline along its smoothed normals
--        (shaders/outline, @require smoothnormals): a closed black outline, corners included
--     2. the same with RAW_NORMALS (the mesh's split face normals): the outline breaks open
--        at every corner and edge
--     3. a skinned hard-edged column (bones and a morph target) bending back and forth: its
--        outline follows the skinning, closed at its corners
--     4. an instanced batch of four cubes: each instance outlined (the pass is instanced too)
--     5. a sphere with a BEFORE pass (the hull with back faces and no depth write): an orange
--        rim around a white sphere (the sphere draws over the hull)
--     6. the same pass AFTER the sphere: an orange ball (the hull covers the sphere)
--     7. an alpha-blended blue box with a BEFORE outline lit by the pixel lights
--        (shaders/lit_outline): a lit pass, drawn before a transparent submesh in the
--        transparent queue. The box looks filled: the hull's lit back faces show through the
--        glass (an inverted hull fills a blended object's whole shape, before it as seen
--        through it, after it as drawn over it). So it is no outline for a blended object:
--        the Outline (hull) preset gives a blended material case 12's technique instead
--        (stencil mark + library:outline_stencil, Visible only)
--   front row:
--     8. a Sprite with a BEFORE drop shadow (shaders/silhouette: its image's shape, offset)
--     9. a Sprite with the same pass AFTER: a dark shape over it
--    10. a Tilemap of three tiles, each with a drop shadow
--    11. a render-to-texture camera's view of an outlined cube, on a quad (a pass in an
--        RTT camera)
--    12. a sphere writing stencil 1 with an AFTER hull that tests stencil not-equal 1 (the
--        pass's own stencil state): an orange rim around a white sphere, drawn after it
--        (compare 6, the same hull without stencil, which covers its sphere)
--   The light casts shadows on the ground: the outlined objects cast the shadow of their
--   shape, not of the hull (the extra pass is in no shadow or depth pass).
--
--   EP_FOCUS=cubes|column|batch|halo|glass|sprites|rtt|stencil: a close-up of one case.
--   EP_SCENE=overhead: 40 plain cubes, no extra pass; EP_SCENE=overhead_pass: the same 40,
--   ten of them outlined. With JASPERENGINE_READY_SIGNAL the engine logs "Render objects:
--   ..." once loaded: the submeshes without a pass allocate nothing (extra passes allocated
--   = extra passes drawn), and the pipelines differ by those of the ten passes only.
--
--   EP_SCENE=figure: models/figure.gltf, three submeshes over one vertex buffer (torso,
--   a backpack hidden behind it, head), each with library shaders on every submesh. Left: the
--   See-through silhouette preset behind a low wall: blue where the wall hides the figure,
--   nothing over the visible torso (the backpack behind it is the figure's own: a pass
--   interleaved with the submesh draws showed it through the torso). Middle: the Outline
--   (stencil) preset behind a wall up to the neck: one orange outline around the whole
--   figure, over the wall too, no line across it where the torso meets the head (the torso's
--   pass drew there before the head had marked the stencil). Right: the Outline (hull) preset,
--   closed; the three submeshes share one smoothed-normal buffer ("Render objects": 2
--   smoothed-normal buffers, one per outlined figure). EP_FOCUS=silhouette|stencil|hull.
--
--   tools/smoke-test.sh tools/fixtures/extra-pass
--   SMOKE_GAME_RUNS="overhead EP_SCENE=overhead; overhead_pass EP_SCENE=overhead_pass" tools/smoke-test.sh tools/fixtures/extra-pass
local ExtraPass = { properties = {} }

local OUTLINE = "assets/shaders/outline"
local LIT_OUTLINE = "assets/shaders/lit_outline"
local SILHOUETTE = "assets/shaders/silhouette"

local function log(fmt, ...)
    print("[EP] " .. string.format(fmt, ...))
end

local function fail(fmt, ...)
    print("[ERROR] [EP] " .. string.format(fmt, ...))
end

-- the extra pass of every submesh, set by configure(pass, material)
local function setExtraPass(mesh, configure)
    if os.getenv("EP_NOPASS") then return end
    local count = mesh:getNumSubmeshes()
    if count == 0 then
        fail("a mesh has no submesh to put an extra pass on")
    end
    for i = 0, count - 1 do
        local material = mesh:getMaterial(i)
        configure(material.extraPass, material)
        mesh:setMaterial(i, material)
        if mesh:getMaterial(i).extraPass.shader == "" then
            fail("the extra pass did not stay on the material")
        end
    end
end

local function outline(width, colour)
    return function(pass)
        pass.shader = OUTLINE
        pass:setShaderParam("width", width)
        if colour then
            pass:setShaderParam("outlineColor", colour)
        end
    end
end

local function box(scene, position, size, r, g, b)
    local p = Primitive(scene)
    p:createBox(size, size, size)
    p.position = position
    p:setColor(r, g, b)
    return p
end

local function buildBack(self, scene)
    -- 1, 2: smoothed against raw normals on a hard-edged cube
    self.smoothCube = box(scene, Vector3(-7.5, 1.0, 0.0), 1.1, 0.85, 0.3, 0.25)
    self.smoothCube:setRotation(25, 40, 0)
    setExtraPass(self.smoothCube, outline(0.07))
    self.rawCube = box(scene, Vector3(-5.2, 1.0, 0.0), 1.1, 0.85, 0.3, 0.25)
    self.rawCube:setRotation(25, 40, 0)
    setExtraPass(self.rawCube, function(pass)
        outline(0.07)(pass)
        pass:setShaderFeature("RAW_NORMALS", true)
        if not pass:isShaderFeatureEnabled("RAW_NORMALS") then
            fail("RAW_NORMALS is not on the pass")
        end
    end)

    -- 3: skinned and morphed
    local column = Model(scene)
    if column:loadModel("models/skinned_column.gltf") then
        column.position = Vector3(-2.9, 0.0, 0.0)
        column:setMorphWeight(0, 0.6)
        setExtraPass(column, outline(0.05, Vector4(0.05, 0.02, 0.15, 1.0)))
        self.column = column
        self.tip = column:getBone("Tip")
        if not self.tip then
            fail("the column has no Tip bone")
        end
    else
        fail("models/skinned_column.gltf did not load")
    end

    -- 4: instanced
    local batch = Primitive(scene)
    batch:createBox(0.5, 0.5, 0.5)
    batch.position = Vector3(-0.7, 0.0, 0.0)
    batch:setColor(0.9, 0.9, 0.9)
    batch:createInstancedMesh()
    local colours = { Vector4(1, 0.3, 0.3, 1), Vector4(0.3, 1, 0.4, 1), Vector4(0.35, 0.5, 1, 1), Vector4(1, 1, 0.3, 1) }
    for i = 1, 4 do
        batch:addInstance(Vector3((i % 2) * 0.7 - 0.35, 0.35 + (i - 1) * 0.55, 0.0), Quaternion(), Vector3(1, 1, 1), colours[i])
    end
    setExtraPass(batch, outline(0.04))
    self.batch = batch

    -- 5, 6: before and after the main draw, the same pass (back faces, no depth write)
    local function halo(order)
        return function(pass)
            outline(0.18, Vector4(1.0, 0.45, 0.05, 1.0))(pass)
            pass.cullMode = MaterialCullMode.BACK
            pass.depthWrite = MaterialDepthWrite.DISABLED
            pass.order = order
        end
    end
    self.before = Primitive(scene)
    self.before:createSphere(0.6, 32, 16)
    self.before.position = Vector3(1.6, 1.0, 0.0)
    self.before:setColor(0.95, 0.95, 0.95)
    setExtraPass(self.before, halo(MaterialPassOrder.BEFORE))
    self.after = Primitive(scene)
    self.after:createSphere(0.6, 32, 16)
    self.after.position = Vector3(3.8, 1.0, 0.0)
    self.after:setColor(0.95, 0.95, 0.95)
    setExtraPass(self.after, halo(MaterialPassOrder.AFTER))

    -- 7: transparent, with a lit pass
    local glass = box(scene, Vector3(6.2, 1.0, 0.0), 1.1, 0.2, 0.45, 1.0)
    setExtraPass(glass, function(pass, material)
        pass.shader = LIT_OUTLINE
        pass:setShaderParam("width", 0.06)
        -- before it: the blended box writes no depth, so a hull after it would cover it;
        -- before it, its back faces show through the glass instead (the case shows a lit
        -- pass with a blended submesh, not a blended object's outline)
        pass.order = MaterialPassOrder.BEFORE
        material.blendMode = MaterialBlendMode.ALPHA
    end)
    glass:setColor(0.2, 0.45, 1.0, 0.45)
    self.glass = glass
end

local function silhouette(order, colour)
    return function(pass, material)
        -- (alpha mode AUTO: blended, from the image's alpha, also for the sprites and the
        -- tilemap that share it)
        pass.shader = SILHOUETTE
        -- behind the image before it, in front of it after it
        local z = (order == MaterialPassOrder.BEFORE) and -0.5 or 0.5
        pass:setShaderParam("offset", Vector3(6.0, -6.0, z))
        pass:setShaderParam("shadowColor", colour)
        pass.order = order
    end
end

local function buildFront(self, scene)
    local function sprite(x, order, colour)
        local s = Sprite(scene)
        s:setTexture("textures/character.png")
        s:setSize(64, 64)
        s:setScale(1.6 / 64)
        s.pivotPreset = PivotPreset.CENTER
        s.castShadows = false
        s.receiveLights = false
        s.position = Vector3(x, 0.9, 5.0)
        setExtraPass(s, silhouette(order, colour))
        return s
    end
    -- 8, 9
    self.spriteBefore = sprite(-5.6, MaterialPassOrder.BEFORE, Vector4(0.0, 0.0, 0.0, 0.65))
    self.spriteAfter = sprite(-3.9, MaterialPassOrder.AFTER, Vector4(0.1, 0.0, 0.3, 0.65))

    -- 10
    if os.getenv("EP_NOTILEMAP") then return end
    local tilemap = Tilemap(scene)
    tilemap:addRect(0, "character", "textures/character.png", Rect(0, 0, 64, 64))
    for i = 0, 2 do
        tilemap:addTile("character", Vector2(i * 70, 0), 64, 64)
    end
    tilemap:setScale(1.0 / 64)
    tilemap.position = Vector3(-2.5, 0.4, 5.0)
    tilemap.castShadows = false
    tilemap.receiveLights = false
    setExtraPass(tilemap, silhouette(MaterialPassOrder.BEFORE, Vector4(0.0, 0.0, 0.0, 0.65)))
    self.tilemap = tilemap
end

local function buildRtt(self, scene)
    -- what the texture camera sees, out of the main camera's view
    local wall = Primitive(scene)
    wall:createPlane(10, 10)
    wall.position = Vector3(0, -50, -1)
    wall:setRotation(90, 0, 0)
    wall:setColor(0.55, 0.6, 0.7)
    wall.receiveLights = false

    local cube = box(scene, Vector3(0, -50, 0.5), 1.2, 0.95, 0.75, 0.2)
    cube:setRotation(25, 35, 0)
    cube.receiveLights = false
    setExtraPass(cube, outline(0.08))

    local camera = Camera(scene)
    camera.type = CameraType.CAMERA_PERSPECTIVE
    camera.position = Vector3(0, -50, 4)
    camera.target = Vector3(0, -50, 0)
    camera.renderToTexture = true
    camera:setFramebufferSize(256, 256)

    local screen = Primitive(scene)
    screen:createBox(1.4, 1.4, 0.05)
    screen.position = Vector3(1.6, 0.75, 5.0)
    screen.receiveLights = false
    screen:setTexture(camera:getFramebuffer())
    self.rttCube, self.rttCamera, self.rttScreen = cube, camera, screen
end

local function buildStencil(self, scene)
    -- 12: QuickOutline style: the sphere writes stencil 1, its AFTER hull (back faces, no
    -- depth write) draws only where the stencil is not 1: an orange rim, the sphere uncovered
    local sphere = Primitive(scene)
    sphere:createSphere(0.6, 32, 16)
    sphere.position = Vector3(3.6, 0.75, 5.0)
    sphere:setColor(0.95, 0.95, 0.95)
    setExtraPass(sphere, function(pass, material)
        material.stencil.compare = MaterialStencilCompare.ALWAYS
        material.stencil.ref = 1
        material.stencil.pass = MaterialStencilOp.REPLACE
        outline(0.15, Vector4(1.0, 0.45, 0.05, 1.0))(pass)
        pass.cullMode = MaterialCullMode.BACK
        pass.depthWrite = MaterialDepthWrite.DISABLED
        pass.stencil.compare = MaterialStencilCompare.NOT_EQUAL
        pass.stencil.ref = 1
        pass.order = MaterialPassOrder.AFTER
    end)
    self.stencilSphere = sphere
end

-- EP_SCENE=figure: library presets on every submesh of a three-submesh model (one vertex
-- buffer), as the editor's presets set them (editor/material/MaterialPresets.cpp)
local function buildFigures(self, scene)
    local function figure(x, preset)
        local model = Model(scene)
        if not model:loadModel("models/figure.gltf") then
            fail("models/figure.gltf did not load")
            return nil
        end
        model.position = Vector3(x, 0.0, 0.0)
        local count = model:getNumSubmeshes()
        if count ~= 3 then
            fail("the figure has %d submeshes, not 3", count)
        end
        for i = 0, count - 1 do
            local material = model:getMaterial(i)
            preset(material)
            model:setMaterial(i, material)
        end
        return model
    end
    local function wall(x, height)
        local w = Primitive(scene)
        w:createBox(1.6, height, 0.08)
        w.position = Vector3(x, height * 0.5, 0.7)
        w:setColor(0.32, 0.34, 0.38)
        return w
    end
    local function pass(material, shader, order)
        material.extraPass = MaterialPass()
        material.extraPass.shader = shader
        material.extraPass.order = order
        -- occluders (priority 0) draw first, so the pass sees them
        material.sortPriority = math.max(material.sortPriority, 1)
    end

    self.silhouetteFigure = figure(-2.2, function(material)
        pass(material, "library:silhouette", MaterialPassOrder.BEFORE)
    end)
    self.lowWall = wall(-2.2, 0.6)

    self.stencilFigure = figure(0.0, function(material)
        local s = MaterialStencil()
        s.compare = MaterialStencilCompare.ALWAYS
        s.ref = 1
        s.writeMask = 1
        s.pass = MaterialStencilOp.REPLACE
        s.depthFail = MaterialStencilOp.REPLACE
        material.stencil = s
        pass(material, "library:outline_stencil", MaterialPassOrder.AFTER)
    end)
    self.tallWall = wall(0.0, 1.45)

    self.hullFigure = figure(2.2, function(material)
        material.extraPass = MaterialPass()
        material.extraPass.shader = "library:outline"
        material.extraPass.order = MaterialPassOrder.AFTER
    end)
end

local function buildOverhead(self, scene, withPasses)
    local count = 0
    for row = 0, 3 do
        for column = 0, 9 do
            local cube = box(scene, Vector3(-6.75 + column * 1.5, 0.5 + row * 1.3, 0.0), 0.8, 0.3 + 0.07 * column, 0.4, 0.9 - 0.15 * row)
            cube:setRotation(20, 30 + column * 5, 0)
            if withPasses and row == 1 then
                setExtraPass(cube, outline(0.06))
                count = count + 1
            end
        end
    end
    log("overhead scene: 40 cubes, %d with an extra pass", count)
end

function ExtraPass:init()
    RegisterEngineEvent(self, "onUpdate")
    local scene = self.scene

    local camera = Camera(scene)
    camera.type = CameraType.CAMERA_PERSPECTIVE
    camera.position = Vector3(-0.6, 4.2, 11.5)
    camera.target = Vector3(-0.6, 1.0, 0.5)
    scene:setCamera(camera)

    local light = Light(scene)
    light.type = LightType.DIRECTIONAL
    light:setDirection(-0.4, -1.0, -0.5)
    light.intensity = 2.5
    light.shadows = true
    self.camera, self.light = camera, light

    local ground = Primitive(scene)
    ground:createPlane(22, 13)
    ground.position = Vector3(-0.6, 0.0, 2.5)
    ground:setColor(0.75, 0.75, 0.72)

    local mode = os.getenv("EP_SCENE") or "main"
    if mode == "overhead" or mode == "overhead_pass" then
        buildOverhead(self, scene, mode == "overhead_pass")
    elseif mode == "figure" then
        buildFigures(self, scene)
        camera.position = Vector3(0.0, 2.0, 6.5)
        camera.target = Vector3(0.0, 0.9, 0.0)
    else
        for _, case in ipairs({ {"back", buildBack}, {"front", buildFront}, {"rtt", buildRtt}, {"stencil", buildStencil} }) do
            local ok, err = pcall(case[2], self, scene)
            if ok then
                log("built %s", case[1])
            else
                fail("%s failed to build: %s", case[1], tostring(err))
            end
        end
    end
    -- EP_FOCUS: a close-up of one case
    local focus = os.getenv("EP_FOCUS")
    local targets = { cubes = Vector3(-6.35, 1.0, 0.0), column = Vector3(-2.9, 1.0, 0.0), batch = Vector3(-0.7, 1.0, 0.0),
                      halo = Vector3(2.7, 1.0, 0.0), glass = Vector3(6.2, 1.0, 0.0), sprites = Vector3(-3.9, 0.8, 5.0),
                      rtt = Vector3(1.6, 0.75, 5.0), stencil = Vector3(3.6, 0.75, 5.0) }
    if mode == "figure" then
        targets = { silhouette = Vector3(-2.2, 0.9, 0.0), stencil = Vector3(0.0, 0.9, 0.0), hull = Vector3(2.2, 0.9, 0.0) }
    end
    if focus and targets[focus] then
        local target = targets[focus]
        camera.target = target
        camera.position = target + ((mode == "figure") and Vector3(0.0, 0.6, 3.2) or Vector3(0.0, 1.2, 4.5))
        log("focus on %s", focus)
    end
    log("scene %s", mode)
    self.time = 0
    self.frames = 0
end

function ExtraPass:onUpdate()
    local dt = 1 / 60
    self.time = self.time + dt
    self.frames = self.frames + 1
    -- the column bends back and forth (its outline follows)
    if self.tip then
        self.tip:setRotation(0, 0, math.sin(self.time * 1.5) * 35)
    end
    if self.frames % 100 == 0 then
        log("frame %d: %d variants compiling", self.frames, ShaderPool.getPendingShaderCount())
    end
end

return ExtraPass
