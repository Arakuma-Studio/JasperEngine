-- Stencil fixture (shader library phase 2). The top row draws six cases straight to the
-- screen; the wide screen below shows the same six cases drawn by a render-to-texture
-- camera (a copy of the row built far below the scene). Left to right:
--
--   A. inside: a mask square (colour writes off, no depth write, stencil always/replace 1,
--      sortPriority -1) in front of a red sphere that tests stencil EQUAL 1: the sphere
--      shows only inside the square; yellow lines (a LineRenderer with the same test)
--      cross the case and show only inside the square too.
--   B. outside: the same mask with ref 2 over a green sphere testing NOT_EQUAL 2: the
--      sphere has a square hole showing the backdrop.
--   C. draw once: two copies of one translucent mesh of three overlapping squares; the
--      left blends every layer (darker where the squares overlap), the right tests
--      NOT_EQUAL 3 and replaces it, so each pixel blends once (one flat colour).
--   D. sprite mask: a character sprite drawn by shaders/sprite_mask (a fork whose manifest
--      says stencil=always ref=4 pass=replace colorwrite=none, cutting out alpha < 0.5)
--      masks a checker sprite testing EQUAL 4: a checkered character silhouette.
--   E. priority: a mask square BEHIND an orange box that tests EQUAL 5. Opaque objects
--      draw nearest first, so the box would draw before its mask and vanish; the mask's
--      sortPriority -1 draws it first: an orange box clipped to the square.
--   F. library sprite mask: the same character drawn by library:sprite_mask (its manifest:
--      blend=opaque, stencil always ref 16 on bits 0xF0, no colour), sortPriority 0, IN FRONT
--      of an orange-tinted checker sprite testing EQUAL 16 on 0xF0. The mask is in the opaque
--      queue, which draws before the blended sprites whatever their distance: an orange
--      checkered character (a mask in the transparent queue, farthest first, would draw
--      after its reader here, and the sprite would vanish).
--
--   tools/smoke-test.sh tools/fixtures/stencil
--   STENCIL_PREPASS=1   the camera depth pre-pass on (stencil readers leave it)
--   STENCIL_EFFECTS=1   SSAO and SSR on (the scene renders offscreen first)
--   STENCIL_SCENE=effects  no stencil at all: a glossy floor, a chrome sphere and boxes
--                          with SSAO and SSR, to compare with a build before stencil
local Stencil = { properties = {} }

local function log(fmt, ...)
    print("[STENCIL] " .. string.format(fmt, ...))
end

local function fail(fmt, ...)
    print("[ERROR] [STENCIL] " .. string.format(fmt, ...))
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

local function stencilState(compare, ref, pass)
    local s = MaterialStencil()
    s.compare = compare
    s.ref = ref
    s.pass = pass or MaterialStencilOp.KEEP
    return s
end

-- library:unlit in a flat colour
local function unlit(material, r, g, b, a)
    material.shader = "library:unlit"
    material:setShaderParam("baseColor", Vector4(r, g, b, a or 1.0))
end

local function backdrop(scene, origin, x)
    local wall = Primitive(scene)
    wall:createBox(3.4, 3.4, 0.1)
    wall.position = origin + Vector3(x, 0.0, -1.6)
    wall.castShadows = false
    configure(wall, function(m) unlit(m, 0.18, 0.2, 0.26) end)
    return wall
end

-- a square that writes ref into the stencil and nothing else
local function maskSquare(scene, position, ref)
    local mask = Primitive(scene)
    mask:createBox(1.7, 1.7, 0.02)
    mask.position = position
    mask.castShadows = false
    configure(mask, function(m)
        unlit(m, 1.0, 0.0, 1.0) -- magenta if colour writes were not off
        m.colorWrite = MaterialColorWrite.NONE
        m.depthWrite = MaterialDepthWrite.DISABLED
        m.stencil = stencilState(MaterialStencilCompare.ALWAYS, ref, MaterialStencilOp.REPLACE)
        m.sortPriority = -1
    end)
    return mask
end

local function buildCases(scene, origin, keep)
    -- A. visible only inside the mask
    local ax = -9.0
    keep[#keep + 1] = backdrop(scene, origin, ax)
    keep[#keep + 1] = maskSquare(scene, origin + Vector3(ax, 0.0, 0.8), 1)
    local inside = Primitive(scene)
    inside:createSphere(1.25)
    inside.position = origin + Vector3(ax, 0.0, 0.0)
    configure(inside, function(m)
        unlit(m, 0.9, 0.12, 0.1)
        -- edited in place on the material's own stencil
        m.stencil.compare = MaterialStencilCompare.EQUAL
        m.stencil.ref = 1
    end)
    keep[#keep + 1] = inside
    local lines = LineRenderer(scene)
    lines.position = origin + Vector3(ax, 0.0, 1.5)
    for i = -3, 3 do
        local o = i * 0.45
        lines:addLine(Vector3(-1.6 + o, -1.6, 0.0), Vector3(1.6 + o, 1.6, 0.0), Vector4(1.0, 0.9, 0.1, 1.0))
    end
    lines.stencil = stencilState(MaterialStencilCompare.EQUAL, 1)
    keep[#keep + 1] = lines

    -- B. visible only outside the mask
    local bx = -5.4
    keep[#keep + 1] = backdrop(scene, origin, bx)
    keep[#keep + 1] = maskSquare(scene, origin + Vector3(bx, 0.0, 0.8), 2)
    local outside = Primitive(scene)
    outside:createSphere(1.25)
    outside.position = origin + Vector3(bx, 0.0, 0.0)
    configure(outside, function(m)
        unlit(m, 0.15, 0.75, 0.25)
        m.stencil = stencilState(MaterialStencilCompare.NOT_EQUAL, 2)
    end)
    keep[#keep + 1] = outside

    -- C. draw-once transparency: one mesh of three overlapping squares; the left one
    -- stacks its overlaps, the right one blends each pixel once
    local cx = -1.8
    keep[#keep + 1] = backdrop(scene, origin, cx)
    for side = 0, 1 do
        local overlap = Model(scene)
        if not overlap:loadModel("models/overlap_quads.gltf") then
            fail("models/overlap_quads.gltf did not load")
        end
        overlap.position = origin + Vector3(cx + (side == 0 and -0.82 or 0.82), 0.0, 0.0)
        overlap.castShadows = false
        configure(overlap, function(m)
            unlit(m, 0.25, 0.65, 1.0, 0.45)
            m.blendMode = MaterialBlendMode.ALPHA
            m.depthWrite = MaterialDepthWrite.DISABLED
            if side == 1 then
                m.stencil = stencilState(MaterialStencilCompare.NOT_EQUAL, 3, MaterialStencilOp.REPLACE)
            end
        end)
        keep[#keep + 1] = overlap
    end

    -- D. a sprite masked by a sprite-shaped mask (alpha cutoff)
    local dx = 1.8
    keep[#keep + 1] = backdrop(scene, origin, dx)
    local masked = Sprite(scene)
    masked:setTexture("textures/checker.png")
    masked:setSize(64, 64)
    masked:setScale(2.8 / 64)
    masked.pivotPreset = PivotPreset.CENTER
    masked.receiveLights = false
    masked.position = origin + Vector3(dx, 0.0, 0.0)
    configure(masked, function(m)
        m.shader = "library:sprite"
        m.stencil = stencilState(MaterialStencilCompare.EQUAL, 4)
    end)
    keep[#keep + 1] = masked
    local mask = Sprite(scene)
    mask:setTexture("textures/character.png")
    mask:setSize(64, 64)
    mask:setScale(2.8 / 64)
    mask.pivotPreset = PivotPreset.CENTER
    mask.receiveLights = false
    mask.position = origin + Vector3(dx, 0.0, 0.3)
    configure(mask, function(m)
        m.shader = "shaders/sprite_mask" -- stencil and colour write from its manifest
        m.sortPriority = -1
    end)
    keep[#keep + 1] = mask

    -- E. sortPriority: the mask behind its reader still draws first
    local ex = 5.4
    keep[#keep + 1] = backdrop(scene, origin, ex)
    keep[#keep + 1] = maskSquare(scene, origin + Vector3(ex, 0.0, -1.2), 5)
    local reader = Primitive(scene)
    reader:createBox(1.5, 1.5, 1.5)
    reader.position = origin + Vector3(ex, 0.0, 0.6)
    reader:setRotation(25, 35, 0)
    configure(reader, function(m)
        unlit(m, 1.0, 0.55, 0.1)
        m.stencil = stencilState(MaterialStencilCompare.EQUAL, 5)
    end)
    keep[#keep + 1] = reader

    -- F. library:sprite_mask in front of its reader, no sort priority (opaque queue)
    local fx = 9.0
    keep[#keep + 1] = backdrop(scene, origin, fx)
    local libraryMasked = Sprite(scene)
    libraryMasked:setTexture("textures/checker.png")
    libraryMasked:setSize(64, 64)
    libraryMasked:setScale(2.8 / 64)
    libraryMasked.pivotPreset = PivotPreset.CENTER
    libraryMasked.receiveLights = false
    libraryMasked.position = origin + Vector3(fx, 0.0, 0.0)
    libraryMasked:setColor(1.0, 0.55, 0.15)
    configure(libraryMasked, function(m)
        m.shader = "library:sprite"
        local s = stencilState(MaterialStencilCompare.EQUAL, 16)
        s.readMask = 0xF0
        s.writeMask = 0
        m.stencil = s
    end)
    keep[#keep + 1] = libraryMasked
    local libraryMask = Sprite(scene)
    libraryMask:setTexture("textures/character.png")
    libraryMask:setSize(64, 64)
    libraryMask:setScale(2.8 / 64)
    libraryMask.pivotPreset = PivotPreset.CENTER
    libraryMask.receiveLights = false
    libraryMask.position = origin + Vector3(fx, 0.0, 0.3)
    configure(libraryMask, function(m)
        m.shader = "library:sprite_mask" -- stencil, colour write and queue from its manifest
    end)
    keep[#keep + 1] = libraryMask
end

local function buildStencilScene(self, scene)
    -- orthographic, so each case lines up with its backdrop
    local camera = Camera(scene)
    camera:setOrtho(-11.2, 11.2, -6.3, 6.3, 0.1, 100.0)
    camera.position = Vector3(0.0, 0.0, 15.5)
    camera.target = Vector3(0.0, 0.0, 0.0)
    scene:setCamera(camera)
    self.camera = camera

    self.keep = {}
    buildCases(scene, Vector3(0.0, 2.9, 0.0), self.keep)

    -- the same cases far below, seen by a render-to-texture camera
    local rttOrigin = Vector3(0.0, -100.0, 0.0)
    buildCases(scene, rttOrigin, self.keep)
    local rtt = Camera(scene)
    rtt:setOrtho(-11.0, 11.0, -3.44, 3.44, 0.1, 100.0)
    rtt.position = rttOrigin + Vector3(0.0, 0.0, 8.6)
    rtt.target = rttOrigin
    rtt.renderToTexture = true
    rtt:setFramebufferSize(1024, 320)
    self.rtt = rtt

    local screen = Primitive(scene)
    screen:createBox(16.0, 5.0, 0.05)
    screen.position = Vector3(0.0, -2.9, 0.0)
    screen.receiveLights = false
    screen.castShadows = false
    screen:setTexture(rtt:getFramebuffer())
    self.screen = screen

    if os.getenv("STENCIL_PREPASS") == "1" then
        scene.depthPrepassEnabled = true
        log("depth pre-pass on")
    end
    if os.getenv("STENCIL_EFFECTS") == "1" then
        scene.ssaoEnabled = true
        scene.ssrEnabled = true
        log("SSAO and SSR on")
    end
end

-- no stencil anywhere: what SSAO and SSR draw, comparable with a build before stencil
local function buildEffectsScene(self, scene)
    local camera = Camera(scene)
    camera.type = CameraType.CAMERA_PERSPECTIVE
    camera.position = Vector3(0.0, 3.2, 8.0)
    camera.target = Vector3(0.0, 0.6, 0.0)
    scene:setCamera(camera)

    local light = Light(scene)
    light.type = LightType.DIRECTIONAL
    light:setDirection(-0.4, -1.0, -0.6)
    light.intensity = 2.5
    light.shadows = true

    local floor = Model(scene)
    if not floor:loadModel("models/glossy_floor.gltf") then
        fail("models/glossy_floor.gltf did not load")
    end
    local sphere = Model(scene)
    if not sphere:loadModel("models/chrome_sphere.gltf") then
        fail("models/chrome_sphere.gltf did not load")
    end
    sphere.position = Vector3(-1.2, 1.0, 0.0)

    local boxes = {}
    local colors = { {0.85, 0.2, 0.15}, {0.2, 0.7, 0.3}, {0.2, 0.35, 0.9} }
    for i = 1, 3 do
        local box = Primitive(scene)
        box:createBox(1.0, 1.0 + i * 0.4, 1.0)
        box.position = Vector3(0.6 + (i - 1) * 1.3, (1.0 + i * 0.4) * 0.5, -0.6 + (i - 2) * 0.5)
        box:setColor(colors[i][1], colors[i][2], colors[i][3])
        boxes[i] = box
    end
    -- a box corner against a wall: SSAO darkens the creases
    local wall = Primitive(scene)
    wall:createBox(8.0, 3.0, 0.2)
    wall.position = Vector3(0.0, 1.5, -2.2)
    wall:setColor(0.8, 0.78, 0.72)

    scene.ssaoEnabled = true
    scene.ssrEnabled = true
    self.camera, self.light, self.floor, self.sphere, self.boxes, self.wall = camera, light, floor, sphere, boxes, wall
    log("effects scene: SSAO and SSR on")
end

function Stencil:init()
    RegisterEngineEvent(self, "onUpdate")
    local scene = self.scene

    local which = os.getenv("STENCIL_SCENE") or "stencil"
    local build = (which == "effects") and buildEffectsScene or buildStencilScene
    local ok, err = pcall(build, self, scene)
    if ok then
        log("built %s scene", which)
    else
        fail("%s scene failed to build: %s", which, tostring(err))
    end
    self.frames = 0
end

function Stencil:onUpdate()
    self.frames = self.frames + 1
    if self.frames % 100 == 0 then
        log("frame %d: %d variants compiling", self.frames, ShaderPool.getPendingShaderCount())
    end
end

return Stencil
