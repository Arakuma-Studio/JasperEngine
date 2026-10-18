-- Checks for library shader fixes that need a particular setup to show:
--   row 1: library:toon under point lights only (solo: the sun is off), per pixel and per
--          vertex side by side, each sphere with its own identical light: the bands stay
--          clean (the lit side does not fade into the shadow colour with distance), and
--          the same light is as bright per vertex as per pixel
--   row 2: library:dissolve WORLD_NOISE on a round and on a squashed sphere: the noise
--          keeps its projection (no streaks along the stretched axis); library:triplanar
--          SPACE_OBJECT boxes, each turned and scaled on its own
--   row 3: the same triplanar boxes as instances of one batch, right behind them:
--          each instance must look like the box in front of it
-- Built alone, the camera takes close-ups in turn (wall clock, since software GL runs the
-- game slowly): the two-band toon spheres until 20 s, the three-band ones until 30 s,
-- the dissolve spheres until 40 s, then the triplanar boxes:
--   SMOKE_GAME_ENV="SL_GROUPS=checks" SMOKE_GAME_SECONDS="18 28 38 48" \
--       tools/smoke-test.sh tools/fixtures/shader-library
local Checks = {}

local TOON = "library:toon"
local DISSOLVE = "library:dissolve"
local TRIPLANAR = "library:triplanar"

local COLUMNS = 8
local SPACING = 1.9

-- an sRGB colour as the linear value material parameters hold
local function srgb(r, g, b, a)
    return Vector4(r ^ 2.2, g ^ 2.2, b ^ 2.2, a or 1.0)
end

-- moves on to cell (row, column) of the group's grid (ctx:place fills cells in order)
local function cell(ctx, row, column)
    ctx.count = row * COLUMNS + column
end

local function toon(ctx, column, lighting, configure)
    cell(ctx, 0, column)
    local mesh = Primitive(ctx.scene)
    mesh:createSphere(0.65, 48, 24)
    local p = ctx:place(mesh, 0.75)
    ctx:setShader(mesh, TOON, function(material)
        material.lighting = lighting
        material:setShaderParam("baseColor", srgb(0.95, 0.55, 0.35))
        if configure then
            configure(material)
        end
    end)

    -- its own white point light, above and in front, 3.2 away (where the inverse square
    -- falloff is about 0.1): the range reaches only this sphere (the neighbouring cells
    -- are empty), and two neighbours seen from between them are mirror images
    local light = Light(ctx.scene)
    light.type = LightType.POINT
    light:setColor(1, 1, 1)
    light.intensity = 20
    light.range = 4.0
    light.shading = LightShading.BOTH
    light.position = Vector3(p.x, p.y + 2.5, p.z + 2.0)
    table.insert(ctx.lights, light)
end

local function dissolve(ctx, column, scale)
    cell(ctx, 1, column)
    local mesh = Primitive(ctx.scene)
    mesh:createSphere(0.5, 48, 24)
    ctx:place(mesh, 0.75)
    mesh.scale = scale
    ctx:setShader(mesh, DISSOLVE, function(material)
        material:setShaderFeature("WORLD_NOISE", true)
        material:setShaderTexture("u_noiseTexture", "textures/fx_noise.png")
        material:setShaderTexture("u_baseTexture", "textures/checker.png")
        material:setShaderParam("cutoff", 0.5)
        material:setShaderParam("worldNoiseScale", 1.6)
    end)
end

local function bricks(material)
    material:setShaderTexture("u_baseTexture", "textures/bricks.png")
    material:setShaderFeature("SPACE_OBJECT", true)
end

-- the transforms of the triplanar boxes: turned and scaled each its own way
local BOXES = {
    { rotation = { 0, 30, 0 }, scale = { 1.0, 1.6, 0.6 } },
    { rotation = { 0, -20, 35 }, scale = { 0.6, 0.8, 1.2 } },
    { rotation = { -30, 45, 0 }, scale = { 1.0, 1.0, 1.0 } },
}

function Checks.build(ctx)
    local scene = ctx.scene
    ctx.lights = {}

    if ctx.solo then
        -- only the point lights, so the toon bands show theirs
        ctx.light.intensity = 0
    end

    -- row 1: toon per pixel and per vertex, two bands; then three bands with specular and rim
    toon(ctx, 0, MaterialLighting.PIXEL)
    toon(ctx, 2, MaterialLighting.VERTEX)
    local glossy = function(material)
        material:setShaderFeature("SPECULAR", true)
        material:setShaderFeature("RIM", true)
        material:setShaderParam("bands", 3)
    end
    toon(ctx, 4, MaterialLighting.PIXEL, glossy)
    toon(ctx, 6, MaterialLighting.VERTEX, glossy)

    -- row 2: dissolve's world noise, round and squashed
    dissolve(ctx, 0, Vector3(1, 1, 1))
    dissolve(ctx, 2, Vector3(2.6, 0.6, 1.0))

    -- triplanar in object space, one box per transform (columns 5 to 7)
    for i, box in ipairs(BOXES) do
        cell(ctx, 1, 4 + i)
        local mesh = Primitive(scene)
        mesh:createBox(0.8, 0.8, 0.8)
        ctx:place(mesh, 0.8)
        mesh:setRotation(box.rotation[1], box.rotation[2], box.rotation[3])
        mesh.scale = Vector3(box.scale[1], box.scale[2], box.scale[3])
        ctx:setShader(mesh, TRIPLANAR, bricks)
    end

    -- row 3: the same boxes as instances of one batch centred on column 6
    cell(ctx, 2, 6)
    local batch = Primitive(scene)
    batch:createBox(0.8, 0.8, 0.8)
    ctx:place(batch, 0.8)
    batch:createInstancedMesh()
    for i, box in ipairs(BOXES) do
        batch:addInstance(Vector3((i - 2) * SPACING, 0, 0),
            Quaternion(box.rotation[1], box.rotation[2], box.rotation[3]),
            Vector3(box.scale[1], box.scale[2], box.scale[3]), Vector4(1, 1, 1, 1))
    end
    ctx:setShader(batch, TRIPLANAR, bricks)

    ctx.start = os.time()
    ctx.view = 0
end

-- the close-ups (x of the cell centres: column c is at (c - 3.5) * SPACING)
local VIEWS = {
    { ends = 20, position = { -4.75, 1.6, 5.0 }, target = { -4.75, 0.75, 0.0 } },
    { ends = 30, position = { 2.85, 1.6, 5.0 }, target = { 2.85, 0.75, 0.0 } },
    { ends = 40, position = { -4.75, 4.5, 0.5 }, target = { -4.75, 0.7, -2.4 } },
    { ends = math.huge, position = { 4.75, 4.5, 0.5 }, target = { 4.75, 0.8, -3.6 } },
}

function Checks.update(ctx, dt)
    if not ctx.solo then
        return
    end
    local elapsed = os.time() - ctx.start
    local index = 1
    while elapsed >= VIEWS[index].ends do
        index = index + 1
    end
    if index ~= ctx.view then
        ctx.view = index
        local view = VIEWS[index]
        ctx.camera.position = Vector3(view.position[1], view.position[2], ctx.originZ + view.position[3])
        ctx.camera.target = Vector3(view.target[1], view.target[2], ctx.originZ + view.target[3])
    end
end

return Checks
