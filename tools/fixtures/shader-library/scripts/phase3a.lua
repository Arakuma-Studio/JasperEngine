-- Shader library phase 3, shaders A: the scene-depth readers library:particle SOFT /
-- NEAR_FADE, library:unlit SOFT / NEAR_FADE, library:rim INTERSECTION,
-- library:projected_decal and library:depth_fade. Two bands (x left to right):
--
--   front band (z 0 .. -4.6 from the group's origin)
--     soft points: a diagonal wall with particle points (soft.png) crossing it and the
--       floor; the left half without SOFT (hard lines where they cut the wall and the
--       ground), the right half with SOFT and NEAR_FADE (no line, they thin out instead)
--     force field: a rim shield (cull off) half sunk in the ground with INTERSECTION, a
--       crate poking through it and a bump under its edge: the band follows the ground
--       line, the bump and the crate
--     decals: an instanced pair of arrow decals on flat ground (one turned and tinted, as
--       wide as long: no stretching), stairs with an arrow decal box over them (on the
--       treads, faded on the risers by ANGLE_FADE), a third arrow box with ANGLE_FADE off
--       over a second flight (stretched down the risers)
--   back band (z -5.2 .. -11)
--     instanced soft quads: a second diagonal wall with additive glow quads of one
--       instanced unlit mesh crossing it and the floor; left without SOFT, right with SOFT
--     fog cards: depth_fade cards crossing the ground, a rock and a wall; the left card
--       with SOFT off (a hard line at the ground), an additive light shaft (EDGE_FADE)
--     decals: a multiply dirt decal over uneven ground (bumps and a ramp), a large arrow
--       box the "inside" views put the camera in, and a wall decal (box turned so its Y
--       axis leaves the wall; the floor inside the box is faded by the angle)
--
--   near fade: two lines of floating points running away from the "near" view's camera,
--     the left one with NEAR_FADE (invisible right at the camera, whole from 1.5 units),
--     the right one without
--
-- SL_GROUPS=phase3a frames the group; PH3A_VIEW=<view> a close-up: soft, near (the near-fade
-- lines from their start), quads, field, decals, stairs (the angle-faded flight
-- up close), decals_b, fog, inside (the
-- camera inside the large decal box), enter (the camera moving into that box and out, 7 s
-- outside, 3 s in, 7 s inside, 3 s out), ortho (orthographic camera over the whole group).
--   SMOKE_GAME_ENV="SL_GROUPS=phase3a" SMOKE_GAME_RUNS="soft PH3A_VIEW=soft; near PH3A_VIEW=near;
--       quads PH3A_VIEW=quads; field PH3A_VIEW=field; decals PH3A_VIEW=decals; stairs PH3A_VIEW=stairs;
--       decals_b PH3A_VIEW=decals_b; fog PH3A_VIEW=fog; inside PH3A_VIEW=inside;
--       ortho PH3A_VIEW=ortho" tools/smoke-test.sh tools/fixtures/shader-library
local Phase3a = {}

local PARTICLE = "library:particle"
local UNLIT = "library:unlit"
local RIM = "library:rim"
local DECAL = "library:projected_decal"
local FADE = "library:depth_fade"

-- the group's rows (ShaderLibrary places the next group after them)
local ROWS = 5

local function keep(ctx, object)
    table.insert(ctx.objects, object)
    return object
end

-- a lit box (walls, steps, crates) in the ground's grey or a colour
local function box(ctx, size, position, rotationY, colour)
    local b = keep(ctx, Primitive(ctx.scene))
    b:createBox(size.x, size.y, size.z)
    b.position = position
    b:setRotation(0, rotationY or 0, 0)
    local c = colour or Vector3(0.55, 0.5, 0.45)
    b:setColor(c.x, c.y, c.z)
    return b
end

-- a lit sphere flattened into the ground (a bump, a rock)
local function bump(ctx, position, scale, colour)
    local s = keep(ctx, Primitive(ctx.scene))
    s:createSphere(0.5, 24, 12)
    s.position = position
    s.scale = scale
    local c = colour or Vector3(0.45, 0.47, 0.42)
    s:setColor(c.x, c.y, c.z)
    return s
end

-- points along a wall's centre line (crossing it) and on the floor in front of it
local function softPoints(ctx, origin, angle, soft)
    local p = keep(ctx, Points(ctx.scene))
    p:setTexture("textures/soft.png")
    p.position = origin
    local colours = { Vector4(1.0, 0.6, 0.2, 1), Vector4(1.0, 0.85, 0.3, 1), Vector4(0.4, 0.75, 1.0, 1) }
    local dirX, dirZ = math.cos(math.rad(angle)), -math.sin(math.rad(angle))
    local n = 0
    for i = -1, 1 do
        for _, y in ipairs({ 0.35, 0.95 }) do
            n = n + 1
            -- on the wall line (it cuts every square through the middle)
            p:addPoint(Vector3(dirX * i * 0.75, y, dirZ * i * 0.75), colours[n % 3 + 1], 1.1)
        end
        -- on the floor, in front of the wall: the lower half under the ground
        p:addPoint(Vector3(dirX * i * 0.75 + 0.15, 0.1, dirZ * i * 0.75 + 0.9), colours[(n + 1) % 3 + 1], 1.0)
    end
    p.customShader = PARTICLE
    if soft then
        p:setShaderFeature("SOFT", true)
        p:setShaderParam("softness", 0.5)
        p:setShaderFeature("NEAR_FADE", true)
    end
    return p
end

-- the near-fade lines: two rows of points floating away from the near view's camera (at
-- 0.4 to 3.4 units), the left one with NEAR_FADE (it fades in with distance), the right
-- one without (the closest points fill the view)
local NEAR_CAMERA = Vector3(-2.4, 1.5, 0.4)
local NEAR_TARGET = Vector3(-6.4, 1.2, -1.0)

local function nearLines(ctx, o)
    local dir = NEAR_TARGET - NEAR_CAMERA
    dir:normalize()
    local side = Vector3(-dir.z, 0, dir.x)
    side:normalize()
    for line = 0, 1 do
        local p = keep(ctx, Points(ctx.scene))
        p:setTexture("textures/soft.png")
        p.position = o + NEAR_CAMERA + side * (line == 0 and -0.3 or 0.3) - Vector3(0, 0.12, 0)
        for _, d in ipairs({ 0.4, 0.7, 1.0, 1.4, 1.9, 2.6, 3.4 }) do
            p:addPoint(dir * d, Vector4(0.5, 0.9, 1.0, 1), 0.3)
        end
        p.customShader = PARTICLE
        if line == 0 then
            p:setShaderFeature("NEAR_FADE", true)
        end
    end
end

local function buildSoftPoints(ctx, o)
    -- a wall from front left to back right
    box(ctx, Vector3(4.6, 1.8, 0.16), o + Vector3(-5.2, 0.9, -2.2), 25)
    local angle = 25
    local dx, dz = math.cos(math.rad(angle)), -math.sin(math.rad(angle))
    softPoints(ctx, o + Vector3(-5.2 - dx * 1.2, 0, -2.2 - dz * 1.2), angle, false)
    ctx.softPoints = softPoints(ctx, o + Vector3(-5.2 + dx * 1.2, 0, -2.2 + dz * 1.2), angle, true)
end

local function buildForceField(ctx, o)
    local centre = o + Vector3(-0.6, 0, -2.3)
    -- a crate half inside the shield, a bump under its front edge
    box(ctx, Vector3(0.8, 0.8, 0.8), centre + Vector3(0.9, 0.3, 0.4), 30, Vector3(0.6, 0.45, 0.3))
    bump(ctx, centre + Vector3(-0.5, -0.05, 1.3), Vector3(1.4, 0.6, 0.9))

    local shield = keep(ctx, Primitive(ctx.scene))
    shield:createSphere(1.5, 48, 24)
    shield.position = centre
    ctx:setShader(shield, RIM, function(m)
        m:setShaderFeature("INTERSECTION", true)
        m:setShaderParam("fill", 0.03)
        m:setShaderParam("intensity", 1.2)
        m.cullMode = MaterialCullMode.OFF
    end)
end

-- a projected decal box: size (x, height, z), turned by rotation (degrees)
local function decalBox(ctx, position, size, rotation, configure)
    local d = keep(ctx, Primitive(ctx.scene))
    d:createBox(1, 1, 1)
    d.position = position
    d.scale = size
    d:setRotation(rotation.x, rotation.y, rotation.z)
    ctx:setShader(d, DECAL, function(m)
        m:setShaderTexture("u_baseTexture", "textures/fx_decal.png")
        m.sortPriority = -1
        if configure then
            configure(m)
        end
    end)
    return d
end

-- four steps rising towards -z, starting at front (the lowest step's front edge)
local function stairs(ctx, front, width)
    for i = 0, 3 do
        local h = 0.22 * (i + 1)
        box(ctx, Vector3(width, h, 0.45), front + Vector3(0, h * 0.5, -0.225 - i * 0.45))
    end
end

local function buildDecalsFront(ctx, o)
    -- flat ground: one instanced box, two decals (the second turned, smaller, tinted)
    local pair = keep(ctx, Primitive(ctx.scene))
    pair:createBox(1, 1, 1)
    pair.position = o + Vector3(2.6, 0, -1.4)
    pair:createInstancedMesh()
    pair:addInstance(Vector3(0, 0, 0), Quaternion(), Vector3(1.5, 1.0, 1.5), Vector4(1, 1, 1, 1))
    pair:addInstance(Vector3(0.2, 0, -2.0), Quaternion(0, 40, 0), Vector3(1.1, 1.0, 1.1), Vector4(0.5, 1.0, 0.6, 1))
    ctx:setShader(pair, DECAL, function(m)
        m:setShaderTexture("u_baseTexture", "textures/fx_decal.png")
        m.sortPriority = -1
    end)

    -- stairs with an arrow over them (angle fade on), then the same without it
    stairs(ctx, o + Vector3(4.7, 0, -0.6), 1.4)
    decalBox(ctx, o + Vector3(4.7, 0.5, -1.4), Vector3(1.3, 1.6, 1.8), Vector3(0, 0, 0))
    stairs(ctx, o + Vector3(6.6, 0, -0.6), 1.4)
    decalBox(ctx, o + Vector3(6.6, 0.5, -1.4), Vector3(1.3, 1.6, 1.8), Vector3(0, 0, 0), function(m)
        m:setShaderFeature("ANGLE_FADE", false)
    end)
end

local function buildDecalsBack(ctx, o)
    -- uneven ground: bumps and a ramp under a multiply dirt decal
    bump(ctx, o + Vector3(2.4, -0.05, -6.2), Vector3(1.2, 0.5, 1.0))
    bump(ctx, o + Vector3(3.3, -0.1, -7.0), Vector3(1.0, 0.6, 1.3))
    local ramp = box(ctx, Vector3(1.0, 0.1, 1.6), o + Vector3(3.6, 0.2, -5.8))
    ramp:setRotation(-15, 20, 0)
    decalBox(ctx, o + Vector3(2.9, 0.2, -6.4), Vector3(2.6, 1.4, 2.6), Vector3(0, 15, 0), function(m)
        m:setShaderTexture("u_baseTexture", "textures/fx_dirt.png")
        m:setShaderFeature("DECAL_BLEND_MULTIPLY", true)
        m:setShaderParam("tint", Vector4(0.6, 0.45, 0.3, 1))
    end)

    -- the large box the inside views stand in (the camera's eye at 1.4 is inside it)
    ctx.insideBox = decalBox(ctx, o + Vector3(6.0, 0, -7.0), Vector3(3.0, 4.0, 3.0), Vector3(0, 0, 0), function(m)
        m:setShaderParam("tiling", Vector4(2, 2, 0, 0))
        m:setShaderParam("tint", Vector4(0.6, 0.85, 1.0, 1))
    end)

    -- a wall with a decal on it (the box turned so its Y axis points out of the wall,
    -- reaching down into the floor, which the angle fade leaves alone)
    box(ctx, Vector3(3.6, 2.2, 0.2), o + Vector3(4.4, 1.1, -10.6))
    decalBox(ctx, o + Vector3(3.6, 0.6, -10.5), Vector3(1.4, 0.6, 1.4), Vector3(90, 0, 0))
end

local function buildSoftQuads(ctx, o)
    box(ctx, Vector3(4.6, 1.8, 0.16), o + Vector3(-5.2, 0.9, -7.8), 25)
    local angle = 25
    local dx, dz = math.cos(math.rad(angle)), -math.sin(math.rad(angle))
    local colours = { Vector4(0.3, 0.8, 1.0, 1), Vector4(1.0, 0.5, 0.9, 1), Vector4(0.5, 1.0, 0.4, 1) }
    for side = 0, 1 do
        local quads = keep(ctx, Primitive(ctx.scene))
        quads:createPlane(1.1, 1.1)
        local centre = o + Vector3(-5.2 + dx * (side * 2.4 - 1.2), 0, -7.8 + dz * (side * 2.4 - 1.2))
        quads.position = centre
        quads:createInstancedMesh()
        local facing = Quaternion(90, 0, 0)
        local n = 0
        for i = -1, 1 do
            for _, y in ipairs({ 0.35, 0.95 }) do
                n = n + 1
                quads:addInstance(Vector3(dx * i * 0.75, y, dz * i * 0.75), facing, Vector3(1, 1, 1), colours[n % 3 + 1])
            end
            quads:addInstance(Vector3(dx * i * 0.75 + 0.15, 0.1, dz * i * 0.75 + 0.9), facing, Vector3(1, 1, 1), colours[(n + 1) % 3 + 1])
        end
        ctx:setShader(quads, UNLIT, function(m)
            m:setShaderTexture("u_baseTexture", "textures/soft.png")
            m.blendMode = MaterialBlendMode.ADDITIVE
            m.cullMode = MaterialCullMode.OFF
            if side == 1 then
                m:setShaderFeature("SOFT", true)
                m:setShaderParam("softness", 0.5)
                m:setShaderFeature("NEAR_FADE", true)
            else
                -- the hard reference: the same look without SOFT
                m.depthWrite = MaterialDepthWrite.DISABLED
            end
        end)
        quads.castShadows = false
    end
end

-- a vertical card (a plane stood up facing +z, turned by angleY)
local function card(ctx, position, width, height, angleY, configure)
    local c = keep(ctx, Primitive(ctx.scene))
    c:createPlane(width, height)
    c.position = position
    c:setRotation(90, angleY or 0, 0)
    ctx:setShader(c, FADE, configure)
    return c
end

local function fog(m, tint)
    m:setShaderTexture("u_baseTexture", "textures/noise.png")
    m:setShaderParam("tiling", Vector4(1.5, 1.0, 0, 0))
    m:setShaderFeature("SCROLL", true)
    m:setShaderFeature("MASK", true)
    m:setShaderTexture("u_maskTexture", "textures/soft.png")
    m:setShaderParam("tint", tint or Vector4(0.85, 0.88, 0.95, 0.9))
end

local function buildFog(ctx, o)
    -- a rock and a wall the cards cross
    bump(ctx, o + Vector3(0.6, 0, -6.6), Vector3(1.0, 1.1, 0.9), Vector3(0.4, 0.38, 0.35))
    box(ctx, Vector3(0.2, 1.6, 2.4), o + Vector3(-1.1, 0.8, -8.6))

    -- SOFT off: a hard line where it meets the ground
    card(ctx, o + Vector3(-1.4, 0.5, -5.6), 1.8, 1.6, 0, function(m)
        fog(m)
        m:setShaderFeature("SOFT", false)
    end)
    -- soft: crossing the ground and the rock
    card(ctx, o + Vector3(0.6, 0.45, -6.6), 2.4, 1.8, 0, function(m) fog(m) end)
    -- soft: crossing the ground and the wall at an angle
    card(ctx, o + Vector3(-0.9, 0.5, -8.4), 2.6, 1.8, 40, function(m)
        fog(m, Vector4(0.75, 0.85, 1.0, 0.9))
    end)
    -- a light shaft: additive, tilted into the ground, fading edge-on
    local shaft = card(ctx, o + Vector3(1.6, 1.1, -9.0), 0.9, 3.0, -20, function(m)
        m:setShaderTexture("u_baseTexture", "textures/fx_streaks.png")
        m:setShaderFeature("FADE_BLEND_ADDITIVE", true)
        m:setShaderFeature("EDGE_FADE", true)
        m:setShaderFeature("MASK", true)
        m:setShaderTexture("u_maskTexture", "textures/soft.png")
        m:setShaderParam("tint", Vector4(1.0, 0.85, 0.5, 0.8))
        m:setShaderParam("softness", 0.6)
    end)
    shaft:setRotation(75, -20, 15)
end

-- PH3A_VIEW cameras: position and target, relative to the group's origin
local VIEWS = {
    soft = { Vector3(-5.0, 2.0, 2.4), Vector3(-5.0, 0.5, -2.3) },
    near = { NEAR_CAMERA, NEAR_TARGET },
    quads = { Vector3(-5.0, 2.0, -3.2), Vector3(-5.0, 0.5, -7.9) },
    field = { Vector3(-0.6, 2.4, 2.8), Vector3(-0.6, 0.3, -2.2) },
    decals = { Vector3(4.6, 4.6, 2.6), Vector3(4.6, 0.0, -2.0) },
    stairs = { Vector3(4.7, 1.6, 0.6), Vector3(4.7, 0.4, -1.4) },
    decals_b = { Vector3(4.6, 4.8, -2.6), Vector3(4.6, 0.0, -8.0) },
    fog = { Vector3(1.9, 1.5, -3.7), Vector3(-0.4, 0.5, -7.6) },
    inside = { Vector3(6.0, 1.4, -5.9), Vector3(6.0, 0.0, -8.4) },
    enter = { Vector3(6.0, 1.4, -2.4), Vector3(6.0, 0.0, -10.0) },
    ortho = { Vector3(0.0, 9.0, 4.0), Vector3(0.0, 0.0, -5.5) },
}

function Phase3a.build(ctx)
    local o = Vector3(0, 0, ctx.originZ)
    buildSoftPoints(ctx, o)
    nearLines(ctx, o)
    buildForceField(ctx, o)
    buildDecalsFront(ctx, o)
    buildSoftQuads(ctx, o)
    buildFog(ctx, o)
    buildDecalsBack(ctx, o)
    ctx.rows = ROWS
    ctx.count = ROWS * 8

    local view = os.getenv("PH3A_VIEW")
    ctx.origin = o
    if ctx.solo or (view and view ~= "") then
        local v = VIEWS[view or ""] or { Vector3(0, 9.5, 5.0), Vector3(0, 0, -5.4) }
        if view and view ~= "" and not VIEWS[view] then
            ctx:fail("PH3A_VIEW names unknown view %s", view)
        end
        ctx.camera.position = o + v[1]
        ctx.camera.target = o + v[2]
        if view == "ortho" then
            ctx.camera:setOrtho(-8.5, 8.5, -4.8, 4.8, 0.1, 100.0)
            ctx:log("orthographic main camera")
        end
        ctx.enter = view == "enter"
    end
end

function Phase3a.update(ctx, dt)
    if not ctx.enter then
        return
    end
    -- 7 s outside, 3 s in, 7 s inside, 3 s out
    ctx.time = (ctx.time or 0) + dt
    local t = ctx.time % 20
    local k
    if t < 7 then k = 0
    elseif t < 10 then k = (t - 7) / 3
    elseif t < 17 then k = 1
    else k = 1 - (t - 17) / 3 end
    local from, to = VIEWS.enter[1], VIEWS.inside[1]
    ctx.camera.position = ctx.origin + from + (to - from) * k
    ctx.camera.target = ctx.origin + VIEWS.enter[2]
end

return Phase3a
