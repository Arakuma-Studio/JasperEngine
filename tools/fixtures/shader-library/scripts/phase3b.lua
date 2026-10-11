-- Shader library phase 3, scene-reading shaders (B): library:water's DEPTH_FOAM, ABSORPTION
-- and REFRACTION, library:glass and library:heat_haze.
--
--   front, four pools of water (y 0.7) over a checker bed sloping from 0.7 deep at the front
--   up out of the water at the back, each with a rock (a sphere) and a tilted block crossing
--   the surface, a front lip and divider walls; left to right:
--     1. every feature, pixel lit (sun glint)
--     2. DEPTH_FOAM: a white band hugging the shore, the rocks and the dividers
--     3. ABSORPTION: clear at the shore, darker with depth, the rock's submerged part fading
--     4. REFRACTION: the checker bed bent by the ripples; the rocks and dividers above the
--        surface are not bent into the water
--   back left, glass before a wall of sharp vertical stripes (z: the wall at -9 from the
--   group's first row):
--     1. a clear pane 0.4 in front of the wall, turned 35 degrees (the stripes shift)
--     2. a frosted pane (BLUR) 1.0 in front
--     3. a green, thick, absorbing pane with a normal map, 1.6 in front
--     4. a glass sphere 0.9 in front (a lens: the stripes turned over), a red post in front
--        of it (the post is not refracted, the sphere around it still is)
--     5. a glass sphere 2.0 in front, pixel lit (sun glint), tinted blue
--   back right, heat haze before a checker grid wall:
--     1. a fire (opaque, alpha-clipped orange and yellow flames) with a haze quad over and above it
--        (UV soft edge, SOFT: no line where it meets the ground)
--     2. a haze sphere (FRESNEL edge: a portal)
--     3. a haze quad masked by a soft texture (MASK_TEXTURE), vertex alpha 0.8, its lower
--        part under the ground (SOFT)
--
-- SL_GROUPS=phase3b frames the group; P3B_VIEW=<view> a close-up: water (the four pools),
-- water_low (a grazing view of pools 1 and 2), water_edge (pool 4 across the screen's left
-- border: no smeared edge), glass, haze.
--   SMOKE_GAME_ENV="SL_GROUPS=phase3b" SMOKE_GAME_RUNS="water P3B_VIEW=water; water_low P3B_VIEW=water_low;
--       water_edge P3B_VIEW=water_edge; glass P3B_VIEW=glass; haze P3B_VIEW=haze" tools/smoke-test.sh tools/fixtures/shader-library
local Phase3b = {}

local WATER = "library:water"
local GLASS = "library:glass"
local HAZE = "library:heat_haze"
local UNLIT = "library:unlit"

local WATER_Y = 0.7
local POOL_WIDTH = 3.6
local POOL_LENGTH = 4.4
-- the bed rises from y 0 at the front to 1 at the back: the shore line is 0.7 of the way
local BED_SLOPE = math.deg(math.atan(1.0 / POOL_LENGTH))

local function add(ctx, object, position)
    object.position = position
    table.insert(ctx.objects, object)
    return object
end

local function unlit(ctx, mesh, texture, tiling, color)
    ctx:setShader(mesh, UNLIT, function(material)
        if texture then
            material:setShaderTexture("u_baseTexture", texture)
        end
        if tiling then
            material:setShaderParam("tiling", tiling)
        end
        if color then
            material:setShaderParam("baseColor", color)
        end
    end)
end

local function box(ctx, w, h, d, position, color)
    local mesh = Primitive(ctx.scene)
    mesh:createBox(w, h, d)
    add(ctx, mesh, position)
    mesh:setColor(color[1], color[2], color[3])
    return mesh
end

-- a pool centred at x, z: a sloping checker bed rising out of the water at the back, a rock
-- and a tilted block crossing the surface, a front lip, a divider on its left, the water
local function pool(ctx, x, z, configure)
    local scene = ctx.scene

    local bed = Primitive(scene)
    bed:createBox(POOL_WIDTH, 0.1, POOL_LENGTH / math.cos(math.rad(BED_SLOPE)))
    add(ctx, bed, Vector3(x, 0.45, z))
    bed:setRotation(BED_SLOPE, 0, 0)
    unlit(ctx, bed, "textures/checker.png", Vector4(4, 5, 0, 0), Vector4(0.85, 0.85, 0.8, 1))

    local rock = Primitive(scene)
    rock:createSphere(0.42, 32, 16)
    add(ctx, rock, Vector3(x - 0.7, 0.62, z + 0.9))
    rock:setColor(0.45, 0.42, 0.4)

    box(ctx, 0.5, 1.0, 0.5, Vector3(x + 0.8, 0.62, z + 0.3), { 0.55, 0.5, 0.45 }):setRotation(25, 35, 15)
    box(ctx, POOL_WIDTH, 0.8, 0.12, Vector3(x, 0.4, z + POOL_LENGTH / 2), { 0.6, 0.6, 0.62 })
    box(ctx, 0.12, 1.0, POOL_LENGTH, Vector3(x - POOL_WIDTH / 2, 0.5, z), { 0.6, 0.6, 0.62 })

    local water = Primitive(scene)
    water:createPlane(POOL_WIDTH, POOL_LENGTH)
    add(ctx, water, Vector3(x, WATER_Y, z))
    ctx:setShader(water, WATER, function(material)
        material:setShaderTexture("u_normalTexture", "textures/fx_water_normal.png")
        material:setShaderTexture("u_noiseTexture", "textures/fx_noise.png")
        material:setShaderParam("layerScale", 2.0)
        material:setShaderParam("secondLayerScale", 1.3)
        material:setShaderParam("foamAmount", 0.08)
        configure(material)
    end)
    return water
end

local function buildWater(ctx, z0)
    local z = z0 - 2.6
    local xs = { -1.5 * POOL_WIDTH, -0.5 * POOL_WIDTH, 0.5 * POOL_WIDTH, 1.5 * POOL_WIDTH }
    pool(ctx, xs[1], z, function(material)
        material.lighting = MaterialLighting.PIXEL
        material:setShaderFeature("DEPTH_FOAM", true)
        material:setShaderFeature("ABSORPTION", true)
        material:setShaderFeature("REFRACTION", true)
    end)
    pool(ctx, xs[2], z, function(material)
        material:setShaderFeature("DEPTH_FOAM", true)
    end)
    pool(ctx, xs[3], z, function(material)
        material:setShaderFeature("ABSORPTION", true)
        material:setShaderParam("absorptionDepth", 0.7)
    end)
    pool(ctx, xs[4], z, function(material)
        material:setShaderFeature("REFRACTION", true)
        material:setShaderParam("refractionStrength", 0.06)
    end)
    -- the last divider, on the right
    box(ctx, 0.12, 1.0, POOL_LENGTH, Vector3(xs[4] + POOL_WIDTH / 2, 0.5, z), { 0.6, 0.6, 0.62 })
end

local function glass(ctx, mesh, configure)
    mesh.castShadows = false
    ctx:setShader(mesh, GLASS, configure)
    return mesh
end

local function buildGlass(ctx, z0)
    local scene = ctx.scene
    local wallZ = z0 - 9.0
    local wall = Primitive(scene)
    wall:createBox(7.4, 2.6, 0.2)
    add(ctx, wall, Vector3(-3.9, 1.3, wallZ))
    -- sharp vertical stripes: the checker along u only
    unlit(ctx, wall, "textures/checker.png", Vector4(2, 0.001, 0, 0))

    local function pane(x, distance)
        local mesh = Primitive(scene)
        mesh:createBox(1.1, 1.5, 0.05)
        add(ctx, mesh, Vector3(x, 0.8, wallZ + distance))
        return mesh
    end

    glass(ctx, pane(-6.8, 0.4)):setRotation(0, 35, 0)
    glass(ctx, pane(-5.3, 1.0), function(material)
        material:setShaderFeature("BLUR", true)
        material:setShaderParam("blurRadius", 2.5)
    end)
    glass(ctx, pane(-3.8, 1.6), function(material)
        material:setShaderFeature("NORMAL_MAP", true)
        material:setShaderTexture("u_normalTexture", "textures/fx_water_normal.png")
        material:setShaderParam("tiling", Vector4(2, 2, 0, 0))
        material:setShaderParam("thickness", 0.4)
        material:setShaderParam("absorption", 3.0)
        material:setShaderParam("absorptionColor", Vector4(0.3, 0.75, 0.45, 1))
    end)

    local sphere = Primitive(scene)
    sphere:createSphere(0.6, 48, 24)
    add(ctx, sphere, Vector3(-2.2, 0.75, wallZ + 0.9))
    glass(ctx, sphere, function(material)
        material:setShaderParam("thickness", 0.5)
    end)
    -- a post in front of the sphere: not refracted into it
    local post = Primitive(scene)
    post:createBox(0.12, 1.4, 0.12)
    add(ctx, post, Vector3(-2.05, 0.7, wallZ + 1.8))
    post:setColor(0.85, 0.15, 0.1)

    local lit = Primitive(scene)
    lit:createSphere(0.5, 48, 24)
    add(ctx, lit, Vector3(-0.9, 0.6, wallZ + 2.0))
    glass(ctx, lit, function(material)
        material.lighting = MaterialLighting.PIXEL
        material:setShaderParam("tint", Vector4(0.7, 0.85, 1.0, 1))
        material:setShaderParam("thickness", 0.4)
    end)
end

local function haze(ctx, mesh, configure)
    mesh.castShadows = false
    ctx:setShader(mesh, HAZE, function(material)
        material:setShaderTexture("u_noiseTexture", "textures/fx_noise.png")
        if configure then
            configure(material)
        end
    end)
    return mesh
end

local function buildHaze(ctx, z0)
    local scene = ctx.scene
    local wallZ = z0 - 9.0
    local wall = Primitive(scene)
    wall:createBox(7.4, 2.6, 0.2)
    add(ctx, wall, Vector3(3.9, 1.3, wallZ))
    unlit(ctx, wall, "textures/checker.png", Vector4(12, 4, 0, 0), Vector4(0.9, 0.9, 0.9, 1))

    -- the fire: opaque flame shapes (in the scene colour, so the haze bends them)
    for i, flame in ipairs({ { 0.7, 1.1, 0.0, Vector4(1.0, 0.4, 0.08, 1) }, { 0.4, 0.7, 0.02, Vector4(1.0, 0.85, 0.3, 1) } }) do
        local fire = Primitive(scene)
        fire:createWall(flame[1], flame[2])
        add(ctx, fire, Vector3(1.6, flame[2] / 2, wallZ + 1.5 + flame[3]))
        fire.castShadows = false
        ctx:setShader(fire, UNLIT, function(material)
            material:setShaderTexture("u_baseTexture", "textures/soft.png")
            material:setShaderParam("baseColor", flame[4])
            material:setShaderFeature("ALPHA_CLIP", true)
            material:setShaderParam("alphaCutoff", 0.3)
        end)
    end
    local over = Primitive(scene)
    over:createWall(1.2, 1.9)
    add(ctx, over, Vector3(1.6, 0.95, wallZ + 1.7))
    haze(ctx, over, function(material)
        material:setShaderFeature("SOFT", true)
        material:setShaderParam("strength", 0.02)
    end)

    local portal = Primitive(scene)
    portal:createSphere(0.6, 48, 24)
    add(ctx, portal, Vector3(3.8, 0.9, wallZ + 1.2))
    haze(ctx, portal, function(material)
        material:setShaderFeature("EDGE_FADE_FRESNEL", true)
        -- a closed volume: its far side's UV seam would show through
        material.cullMode = MaterialCullMode.BACK
        material:setShaderParam("strength", 0.03)
        material:setShaderParam("tint", Vector4(0.85, 0.9, 1.0, 1))
    end)

    local masked = Primitive(scene)
    masked:createWall(1.6, 1.6)
    add(ctx, masked, Vector3(5.9, 0.5, wallZ + 1.0))
    haze(ctx, masked, function(material)
        material:setShaderFeature("MASK_TEXTURE", true)
        material:setShaderFeature("EDGE_FADE_NONE", true)
        material:setShaderFeature("SOFT", true)
        material:setShaderTexture("u_maskTexture", "textures/soft.png")
        material:setShaderParam("strength", 0.045)
        material:setShaderParam("tint", Vector4(1, 1, 1, 0.8))
    end)
end

-- P3B_VIEW cameras: position and target, from the group's first row at x 0
local VIEWS = {
    water = { Vector3(0, 7.0, 6.0), Vector3(0, 0.3, -2.4) },
    water_low = { Vector3(-3.6, 1.1, 0.2), Vector3(-3.6, 0.6, -3.0) },
    water_edge = { Vector3(4.4, 2.0, 0.4), Vector3(6.0, 0.6, -2.4) },
    glass = { Vector3(-3.7, 1.9, -2.6), Vector3(-3.7, 0.8, -9.0) },
    haze = { Vector3(4.0, 1.8, -2.6), Vector3(4.0, 0.9, -9.0) },
}

function Phase3b.build(ctx)
    local z0 = ctx.originZ
    buildWater(ctx, z0)
    buildGlass(ctx, z0)
    buildHaze(ctx, z0)
    -- the space this group takes (the next group starts after it), and its object count
    ctx.rows = 5
    ctx.count = #ctx.objects

    local view = os.getenv("P3B_VIEW")
    if ctx.solo or (view and view ~= "") then
        local v = VIEWS[view or ""] or { Vector3(0, 9.0, 5.0), Vector3(0, 0.0, -5.0) }
        if view and view ~= "" and not VIEWS[view] then
            ctx:fail("P3B_VIEW names unknown view %s", view)
        end
        local origin = Vector3(0, 0, z0)
        ctx.camera.position = origin + v[1]
        ctx.camera.target = origin + v[2]
    end
end

return Phase3b
