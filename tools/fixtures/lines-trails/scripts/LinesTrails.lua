-- Lines and trails fixture (lines and trails spec 9). The script builds every case at run
-- time in front of a perspective camera, a grid of Line Renderers (top) and moving Trail
-- Renderers (bottom). Left to right, top row:
--
--   1. hairline: the 1-pixel segments of an old Line Renderer (no width)
--   2. width 0.4 polyline, miter corners, a red -> blue gradient
--   3. width 0.4, round corners (6) and round caps (8), a thinning width curve 1 -> 0.2
--   4. a closed loop (a square), width 0.25, bevelled sharp corner
--   5. Transform Z alignment, turned 60 degrees around X (seen foreshortened)
--   6. 4-pixel wide (widthUnit Pixels), the same on screen at any depth
-- second row: the four texture modes of a two-colour texture (Stretch, Tile, Distribute
-- Per Segment, Repeat Per Segment), a local-space line on a scaled, rotating GameObject, and
-- the B lightning (library:line_flipbook, 8 rows at 30 fps, additive);
-- bottom: three trails on circling GameObjects (one on a cube: a trail coexists with a mesh),
-- the bullet trail of the Unity game (0.3 s, 0.223 -> 0, orange fading out).
--
--   tools/smoke-test.sh tools/fixtures/lines-trails   (SMOKE_BACKEND=gles3 / vulkan too)
--   LINES_TRAILS_BENCH=1   50 trails of 64 points circling, the frame time logged every 100
--                          frames (with JASPERENGINE_FRAME_STATS=1 the engine's own stats)
local LinesTrails = { properties = {} }

local function log(fmt, ...)
    print("[LINES] " .. string.format(fmt, ...))
end

local function fail(fmt, ...)
    print("[ERROR] [LINES] " .. string.format(fmt, ...))
end

local function polyline(scene, keep, position, points, width)
    local line = LineRenderer(scene)
    line.position = position
    line:setPositions(points)
    line.widthMultiplier = width
    keep[#keep + 1] = line
    return line
end

local function zigzag(scale)
    return { Vector3(-1.0, -0.6, 0) * scale, Vector3(-0.3, 0.6, 0) * scale, Vector3(0.3, -0.6, 0) * scale, Vector3(1.0, 0.6, 0) * scale }
end

local function buildLines(self, scene)
    local keep = self.keep
    local top, mid = 2.6, 0.0

    -- 1. the old hairline segments
    local hair = LineRenderer(scene)
    hair.position = Vector3(-7.5, top, 0)
    for i = 0, 4 do
        hair:addLine(Vector3(-1, -0.8 + i * 0.4, 0), Vector3(1, -0.8 + i * 0.4 + 0.3, 0), Vector4(0.9, 0.9, 0.2, 1))
    end
    keep[#keep + 1] = hair

    -- 2. miter corners and a gradient
    local miter = polyline(scene, keep, Vector3(-4.5, top, 0), zigzag(1.0), 0.4)
    local g = Gradient()
    g:setKeys({ {0, Vector3(1, 0.1, 0.1)}, {1, Vector3(0.1, 0.3, 1)} }, { {0, 1}, {1, 1} })
    miter.colorGradient = g

    -- 3. round corners and caps, a thinning curve
    local round = polyline(scene, keep, Vector3(-1.5, top, 0), zigzag(1.0), 0.4)
    round.numCornerVertices = 6
    round.numCapVertices = 8
    round.widthCurve = AnimationCurve.linear(0, 1, 1, 0.2)
    round.startColor = Vector4(0.2, 1, 0.4, 1)
    round.endColor = Vector4(0.2, 1, 0.4, 1)

    -- 4. a closed square with a sharp spike (bevelled)
    local loop = polyline(scene, keep, Vector3(1.5, top, 0),
        { Vector3(-0.9, -0.9, 0), Vector3(0.9, -0.9, 0), Vector3(0.9, 0.9, 0), Vector3(0.7, -0.4, 0), Vector3(-0.9, 0.9, 0) }, 0.25)
    loop.loop = true
    loop.startColor = Vector4(1, 0.7, 0.1, 1)
    loop.endColor = Vector4(1, 0.7, 0.1, 1)

    -- 5. Transform Z alignment, turned
    local flat = polyline(scene, keep, Vector3(4.5, top, 0), zigzag(1.0), 0.5)
    flat.alignment = LineAlignment.TransformZ
    flat.rotation = Quaternion(60, Vector3(1, 0, 0))
    flat.startColor = Vector4(0.8, 0.3, 1, 1)
    flat.endColor = Vector4(0.8, 0.3, 1, 1)

    -- 6. 4 pixels wide, receding in depth
    local px = polyline(scene, keep, Vector3(7.5, top, 0), { Vector3(-1, -0.8, 2), Vector3(1, 0.8, -6) }, 4)
    px.widthUnit = LineWidthUnit.Pixels
    px.startColor = Vector4(1, 1, 1, 1)

    -- texture modes
    local modes = { LineTextureMode.Stretch, LineTextureMode.Tile, LineTextureMode.DistributePerSegment, LineTextureMode.RepeatPerSegment }
    for i, mode in ipairs(modes) do
        local textured = polyline(scene, keep, Vector3(-7.5 + (i - 1) * 3.0, mid, 0),
            { Vector3(-1.2, 0, 0), Vector3(-0.2, 0.5, 0), Vector3(1.2, -0.5, 0) }, 0.45)
        textured.texture = "textures/halves.png"
        textured.textureMode = mode
        textured.textureScale = Vector2(mode == LineTextureMode.Tile and 2.0 or 1.0, 1.0)
    end

    -- a local-space line on a scaled GameObject: the points scale, the width does not
    local localLine = polyline(scene, keep, Vector3(4.5, mid, 0), { Vector3(-0.5, -0.5, 0), Vector3(0.5, 0.5, 0) }, 0.2)
    localLine.useWorldSpace = false
    localLine.scale = Vector3(2.5, 1.0, 1.0)
    localLine.startColor = Vector4(0.3, 0.9, 0.9, 1)
    localLine.endColor = Vector4(0.3, 0.9, 0.9, 1)
    self.spinner = localLine

    -- the B lightning: a 2-point world-space line, width 0.35, an 8-row flipbook, additive
    local bolt = LineRenderer(scene)
    bolt.useWorldSpace = true
    bolt.positionCount = 2
    bolt:setPosition(0, Vector3(6.4, mid - 0.8, 0))
    bolt:setPosition(1, Vector3(8.6, mid + 0.8, 0))
    bolt.widthMultiplier = 0.35
    bolt.customShader = "library:line_flipbook"
    bolt:setShaderTexture("u_baseTexture", "textures/lightning.png")
    bolt:setShaderParam("rows", 8)
    bolt:setShaderParam("columns", 1)
    bolt:setShaderParam("fps", 30)
    keep[#keep + 1] = bolt
end

local function trailed(scene, keep, mesh)
    local object
    if mesh then
        object = Primitive(scene)
        object:createBox(0.3, 0.3, 0.3)
    else
        object = GameObject(scene)
    end
    local trail = TrailRenderer(scene, object.entity)
    keep[#keep + 1] = object
    return object, trail
end

local function buildTrails(self, scene)
    local keep = self.keep
    self.movers = {}
    for i = 1, 3 do
        local object, trail = trailed(scene, keep, i == 2)
        trail.time = 0.8
        trail.widthMultiplier = 0.3
        trail.widthCurve = AnimationCurve.linear(0, 1, 1, 0)
        local g = Gradient()
        g:setKeys({ {0, Vector3(1, 1, 1)}, {1, Vector3(0.2 + i * 0.25, 0.4, 1 - i * 0.25)} }, { {0, 1}, {1, 0} })
        trail.colorGradient = g
        trail.numCapVertices = (i == 3) and 6 or 0
        self.movers[#self.movers + 1] = { object = object, centre = Vector3(-6 + i * 3.0, -2.6, 0), radius = 0.9, speed = 2.0 + i * 0.5 }
    end
    -- the Invaders bullet: up and down
    local bullet, trail = trailed(scene, keep, false)
    trail.time = 0.3
    trail.minVertexDistance = 0.1
    trail.widthMultiplier = 1
    local curve = AnimationCurve()
    curve:addKey(0, 0.22307944)
    curve:addKey(1, 0)
    trail.widthCurve = curve
    local orange = Gradient()
    orange:setKeys({ {0, Vector3(1, 0.5886716, 0)}, {1, Vector3(1, 0.5886716, 0)} }, { {0, 1}, {1, 0} })
    trail.colorGradient = orange
    self.bullet = bullet
end

local function buildBench(self, scene)
    local keep = self.keep
    self.movers = {}
    for i = 1, 50 do
        local object, trail = trailed(scene, keep, false)
        trail.time = 64 / 60
        trail.minVertexDistance = 0
        trail.widthMultiplier = 0.08
        local row, col = math.floor((i - 1) / 10), (i - 1) % 10
        self.movers[#self.movers + 1] = { object = object, centre = Vector3(-8 + col * 1.8, -3 + row * 1.6, 0), radius = 0.6, speed = 3.0 + i * 0.05 }
    end
end

function LinesTrails:init()
    RegisterEngineEvent(self, "onUpdate")
    local scene = self.scene
    self.keep = {}
    self.time = 0
    self.frames = 0
    self.bench = os.getenv("LINES_TRAILS_BENCH") == "1"

    local camera = Camera(scene)
    camera.type = CameraType.CAMERA_PERSPECTIVE
    camera.position = Vector3(0, 0, 14)
    camera.target = Vector3(0, 0, 0)
    scene:setCamera(camera)
    self.keep[#self.keep + 1] = camera

    local build = self.bench and buildBench or function(s, sc) buildLines(s, sc); buildTrails(s, sc) end
    local ok, err = pcall(build, self, scene)
    if ok then
        log("built %s", self.bench and "the bench (50 trails)" or "the lines and trails")
    else
        fail("build failed: %s", tostring(err))
    end
end

function LinesTrails:onUpdate()
    local dt = Engine.deltatime
    self.time = self.time + dt
    self.frames = self.frames + 1
    for _, m in ipairs(self.movers or {}) do
        local a = self.time * m.speed
        m.object.position = m.centre + Vector3(math.cos(a), math.sin(a), 0) * m.radius
    end
    if self.bullet then
        self.bullet.position = Vector3(-8.5, -3.2 + ((self.time * 6) % 2.4), 0)
    end
    if self.spinner then
        self.spinner.rotation = Quaternion(self.time * 40, Vector3(0, 0, 1))
    end
    if self.frames % 100 == 0 then
        log("frame %d: %.2f ms (%d variants compiling)", self.frames, dt * 1000, ShaderPool.getPendingShaderCount())
    end
end

return LinesTrails
