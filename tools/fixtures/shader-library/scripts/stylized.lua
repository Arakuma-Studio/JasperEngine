-- The stylized library shaders, each in its lighting modes and with each feature and keyword:
-- library:directional_tint (cubes like Kenney's example: top yellow-orange, X faces purple,
-- Z faces red), library:matcap (REPLACE, MULTIPLY, ADD, rim), library:fake_lit (the default
-- fake shading, matcap shine, baked shadow texture on UV1 and UV2, vertex AO, emission, axis
-- fog, vertex lights), library:toon (bands, specular, rim, ramp, received shadows; per pixel,
-- mixed and per vertex), library:triplanar (world and object space, floor/wall/ceiling
-- textures, dirt; unlit, per pixel and per vertex) and library:vertex_color (unlit, per
-- vertex, per pixel, textured, instanced). Three static point lights over the lit rows, one
-- of them per vertex only (mixed materials split them). Textures and the blob models are
-- original, generated procedurally.
local Stylized = {}

local TINT = "library:directional_tint"
local MATCAP = "library:matcap"
local FAKE = "library:fake_lit"
local TOON = "library:toon"
local TRIPLANAR = "library:triplanar"
local VERTEX_COLOR = "library:vertex_color"

-- an sRGB colour as the linear value material parameters hold
local function srgb(r, g, b, a)
    return Vector4(r ^ 2.2, g ^ 2.2, b ^ 2.2, a or 1.0)
end

local function sphere(ctx, radius)
    local mesh = Primitive(ctx.scene)
    mesh:createSphere(radius or 0.65, 40, 20)
    ctx:place(mesh, radius or 0.65)
    return mesh
end

local function box(ctx, w, h, d)
    local mesh = Primitive(ctx.scene)
    mesh:createBox(w, h, d)
    ctx:place(mesh, h / 2)
    return mesh
end

local function torus(ctx)
    local mesh = Primitive(ctx.scene)
    mesh:createTorus(0.45, 0.2, 40, 20)
    ctx:place(mesh, 0.75)
    mesh:setRotation(65, 0, 0)
    return mesh
end

local function model(ctx, path, y)
    local mesh = Model(ctx.scene)
    if not mesh:loadModel(path) then
        ctx:fail("%s did not load", path)
    end
    ctx:place(mesh, y or 0.75)
    return mesh
end

-- a little figure: a capsule body and a sphere head, both drawn with the same shader
local function character(ctx, shader, configure)
    local body = Primitive(ctx.scene)
    body:createCapsule(0.32, 0.75, 32, 8)
    local p = ctx:place(body, 0.7)
    local head = Primitive(ctx.scene)
    head:createSphere(0.3, 32, 16)
    head.position = Vector3(p.x, p.y + 0.78, p.z)
    table.insert(ctx.objects, head)
    ctx:setShader(body, shader, configure)
    ctx:setShader(head, shader, configure)
    return body, head
end

local function shader(ctx, mesh, name, lighting, configure)
    ctx:setShader(mesh, name, function(material, i)
        if lighting then
            material.lighting = lighting
        end
        if configure then
            configure(material, i)
        end
    end)
end

local function features(material, list)
    for _, define in ipairs(list) do
        material:setShaderFeature(define, true)
    end
end

function Stylized.build(ctx)
    local scene = ctx.scene

    -- directional_tint: the manifest defaults are the reference image's colours, on an
    -- axis-aligned cube (the tint follows the world normal: a turned face blends two colours)
    shader(ctx, box(ctx, 1.1, 1.1, 1.1), TINT)

    -- a base map with tiling, a darker overall colour, turned: its side faces blend
    local textured = box(ctx, 1.1, 1.1, 1.1)
    textured:setRotation(0, -25, 0)
    shader(ctx, textured, TINT, nil, function(material)
        material:setShaderTexture("u_baseTexture", "textures/checker.png")
        material:setShaderParam("tiling", Vector4(2, 2, 0, 0))
        material:setShaderParam("color", srgb(0.85, 0.85, 0.85))
    end)

    -- the blend across a curved surface
    shader(ctx, sphere(ctx), TINT)

    -- every colour white: the neutral setting, the base map unchanged
    local neutral = torus(ctx)
    shader(ctx, neutral, TINT, nil, function(material)
        material:setShaderTexture("u_baseTexture", "textures/checker.png")
        material:setShaderParam("colorA", Vector4(1, 1, 1, 1))
        material:setShaderParam("colorB", Vector4(1, 1, 1, 1))
        material:setShaderParam("colorC", Vector4(1, 1, 1, 1))
    end)

    -- matcap: REPLACE with the studio matcap (the shading is the image)
    shader(ctx, sphere(ctx), MATCAP, nil, function(material)
        material:setShaderTexture("u_matcapTexture", "textures/matcap_studio.png")
    end)

    -- MULTIPLY: the studio matcap shades a checker base map
    local multiplied = torus(ctx)
    shader(ctx, multiplied, MATCAP, nil, function(material)
        material:setShaderFeature("MATCAP_MODE_MULTIPLY", true)
        material:setShaderTexture("u_matcapTexture", "textures/matcap_studio.png")
        material:setShaderTexture("u_baseTexture", "textures/checker.png")
        material:setShaderParam("tiling", Vector4(4, 1, 0, 0))
        material:setShaderParam("baseColor", srgb(0.6, 0.85, 1.0))
    end)

    -- ADD: a red base plus the shine matcap, shaped by gain and power
    local added = model(ctx, "models/sphere.gltf", 0.8)
    shader(ctx, added, MATCAP, nil, function(material)
        material:setShaderFeature("MATCAP_MODE_ADD", true)
        material:setShaderTexture("u_matcapTexture", "textures/matcap_shine.png")
        material:setShaderParam("baseColor", srgb(0.75, 0.12, 0.1))
        material:setShaderParam("matcapGain", 1.3)
        material:setShaderParam("matcapPower", 1.5)
    end)

    -- REPLACE tinted, with a cyan rim
    local rimmed = Primitive(scene)
    rimmed:createCapsule(0.35, 0.7, 32, 8)
    ctx:place(rimmed, 0.75)
    shader(ctx, rimmed, MATCAP, nil, function(material)
        material:setShaderTexture("u_matcapTexture", "textures/matcap_studio.png")
        material:setShaderParam("baseColor", srgb(0.95, 0.8, 0.55))
        material:setShaderFeature("RIM", true)
        material:setShaderParam("rimColor", srgb(0.3, 0.85, 1.0))
        material:setShaderParam("rimPower", 3.0)
    end)

    -- fake_lit: the defaults on a sphere and on a figure
    shader(ctx, sphere(ctx), FAKE)
    character(ctx, FAKE, function(material)
        material:setShaderParam("baseColor", srgb(1.0, 0.72, 0.45))
    end)

    -- MATCAP: gold, the additive shine
    local gold = model(ctx, "models/sphere.gltf", 0.8)
    shader(ctx, gold, FAKE, nil, function(material)
        material:setShaderFeature("MATCAP", true)
        material:setShaderTexture("u_matcapTexture", "textures/matcap_shine.png")
        material:setShaderParam("baseColor", srgb(0.85, 0.6, 0.15))
        material:setShaderParam("shadowColor", srgb(0.45, 0.25, 0.1))
        material:setShaderParam("matcapGain", 1.2)
        material:setShaderParam("matcapPower", 1.4)
    end)

    -- SHADOW_TEXTURE on a box: no second UV set, so the first
    local baked = box(ctx, 1.0, 1.0, 1.0)
    baked:setRotation(0, 30, 0)
    shader(ctx, baked, FAKE, nil, function(material)
        material:setShaderFeature("SHADOW_TEXTURE", true)
        material:setShaderTexture("u_shadowTexture", "textures/bake_shadow.png")
        material:setShaderParam("baseColor", srgb(0.55, 0.85, 0.6))
    end)

    -- SHADOW_TEXTURE on the blob's second UV set
    local bakedUV2 = model(ctx, "models/occluded_blob.gltf")
    shader(ctx, bakedUV2, FAKE, nil, function(material)
        material:setShaderFeature("SHADOW_TEXTURE", true)
        material:setShaderTexture("u_shadowTexture", "textures/bake_shadow.png")
        material:setShaderParam("baseColor", srgb(0.6, 0.75, 1.0))
    end)

    -- VERTEX_AO: the blob's occlusion in its vertex colours
    local occluded = model(ctx, "models/occluded_blob.gltf")
    shader(ctx, occluded, FAKE, nil, function(material)
        material:setShaderFeature("VERTEX_AO", true)
        material:setShaderParam("baseColor", srgb(0.95, 0.9, 0.8))
    end)

    -- VERTEX_AO on instances tinted by their instance colours (a dark red one included): the
    -- occlusion is the raw vertex colour, so every instance is shaded alike, each in its tint
    local occludedBatch = model(ctx, "models/occluded_blob.gltf", 0.3)
    occludedBatch:createInstancedMesh()
    local batchTints = { srgb(1.0, 0.35, 0.3), srgb(0.35, 1.0, 0.45), srgb(0.4, 0.55, 1.0), srgb(0.25, 0.1, 0.1) }
    for i = 1, 4 do
        local x = ((i - 1) % 2 - 0.5) * 0.55
        local z = (math.floor((i - 1) / 2) - 0.5) * 0.55
        occludedBatch:addInstance(Vector3(x, 0, z), Quaternion(), Vector3(0.45, 0.45, 0.45), batchTints[i])
    end
    shader(ctx, occludedBatch, FAKE, nil, function(material)
        material:setShaderFeature("VERTEX_AO", true)
    end)
    ctx:log("instanced VERTEX_AO batch at x %.2f z %.2f", occludedBatch.position.x, occludedBatch.position.z)

    -- EMISSION: a dark tint with a glowing checker
    local glowing = box(ctx, 1.0, 1.0, 1.0)
    glowing:setRotation(0, -30, 0)
    shader(ctx, glowing, FAKE, nil, function(material)
        material:setShaderFeature("EMISSION", true)
        material:setShaderTexture("u_emissionTexture", "textures/checker.png")
        material:setShaderParam("emissionColor", srgb(1.0, 0.45, 0.1))
        material:setShaderParam("baseColor", srgb(0.25, 0.25, 0.3))
    end)

    -- vertex lighting: the point lights add their diffuse to the fake shading
    shader(ctx, sphere(ctx), FAKE, MaterialLighting.VERTEX)

    -- AXIS_FOG: fog rising from the ground (the camera is 7.5 above it), on a tall column
    local column = box(ctx, 0.7, 1.8, 0.7)
    column:setRotation(0, 20, 0)
    shader(ctx, column, FAKE, nil, function(material)
        material:setShaderFeature("AXIS_FOG", true)
        material:setShaderParam("fogAxis", Vector3(0, -1, 0))
        material:setShaderParam("fogStart", 6.0)
        material:setShaderParam("fogLength", 1.4)
        material:setShaderParam("baseColor", srgb(0.95, 0.5, 0.55))
    end)

    -- every feature, per vertex
    local everything = model(ctx, "models/occluded_blob.gltf")
    shader(ctx, everything, FAKE, MaterialLighting.VERTEX, function(material)
        features(material, { "MATCAP", "SHADOW_TEXTURE", "VERTEX_AO", "EMISSION", "AXIS_FOG" })
        material:setShaderTexture("u_matcapTexture", "textures/matcap_shine.png")
        material:setShaderTexture("u_shadowTexture", "textures/bake_shadow.png")
        material:setShaderTexture("u_emissionTexture", "textures/checker.png")
        material:setShaderParam("emissionColor", srgb(0.1, 0.25, 0.35))
        material:setShaderParam("baseColor", srgb(0.7, 0.55, 0.95))
        material:setShaderParam("fogAxis", Vector3(0, -1, 0))
        material:setShaderParam("fogStart", 6.6)
        material:setShaderParam("fogLength", 1.0)
    end)

    -- toon: the defaults (two bands), per pixel, on the scene's light and shadows
    local toonColour = srgb(0.95, 0.55, 0.35)
    shader(ctx, sphere(ctx), TOON, nil, function(material)
        material:setShaderParam("baseColor", toonColour)
    end)

    -- three bands, the hard specular and the rim (lit side only)
    local glossy = model(ctx, "models/sphere.gltf", 0.8)
    shader(ctx, glossy, TOON, nil, function(material)
        features(material, { "SPECULAR", "RIM" })
        material:setShaderParam("baseColor", srgb(0.35, 0.6, 0.95))
        material:setShaderParam("bands", 3)
    end)

    -- the ramp texture, with specular, on a figure
    character(ctx, TOON, function(material)
        features(material, { "RAMP", "SPECULAR" })
        material:setShaderTexture("u_rampTexture", "textures/toon_ramp.png")
        material:setShaderParam("baseColor", srgb(0.95, 0.85, 0.75))
    end)

    -- the same sphere per pixel (above), mixed and per vertex: the point lights band per
    -- pixel, the vertex-only one adds smooth light; per vertex the light is banded as a whole
    shader(ctx, sphere(ctx), TOON, MaterialLighting.MIXED, function(material)
        material:setShaderParam("baseColor", toonColour)
    end)
    shader(ctx, sphere(ctx), TOON, MaterialLighting.VERTEX, function(material)
        material:setShaderParam("baseColor", toonColour)
        material:setShaderParam("bands", 3)
    end)

    -- a torus with the rim on both sides and every feature on
    local ring = torus(ctx)
    shader(ctx, ring, TOON, nil, function(material)
        features(material, { "SPECULAR", "RIM" })
        material:setShaderParam("baseColor", srgb(0.4, 0.85, 0.45))
        material:setShaderParam("rimLitOnly", 0.0)
        material:setShaderParam("rimColor", srgb(1.0, 0.95, 0.6))
    end)

    -- shadows received, banded: a tile under a floating box, both toon
    local tile = Primitive(scene)
    tile:createPlane(1.7, 1.7)
    local tilePosition = ctx:place(tile, 0.02)
    shader(ctx, tile, TOON, nil, function(material)
        material:setShaderParam("baseColor", srgb(0.9, 0.9, 0.85))
    end)
    local caster = Primitive(scene)
    caster:createBox(0.6, 0.15, 0.6)
    caster.position = Vector3(tilePosition.x + 0.25, 1.1, tilePosition.z + 0.35)
    table.insert(ctx.objects, caster)
    shader(ctx, caster, TOON, nil, function(material)
        material:setShaderParam("baseColor", srgb(0.95, 0.75, 0.3))
    end)

    -- triplanar: world space, unlit, the bricks across a sphere and a tall box
    local bricks = function(material)
        material:setShaderTexture("u_baseTexture", "textures/bricks.png")
    end
    shader(ctx, sphere(ctx), TRIPLANAR, nil, bricks)
    local tall = box(ctx, 0.9, 1.7, 0.9)
    tall:setRotation(0, 30, 0)
    shader(ctx, tall, TRIPLANAR, nil, bricks)

    -- object space on a scaled box: the bricks keep their size and move with the box
    local scaled = Primitive(scene)
    scaled:createBox(1, 1, 1)
    ctx:place(scaled, 0.4)
    scaled.scale = Vector3(1.4, 0.8, 0.9)
    scaled:setRotation(0, -25, 0)
    shader(ctx, scaled, TRIPLANAR, nil, function(material)
        bricks(material)
        material:setShaderFeature("SPACE_OBJECT", true)
    end)

    -- floor, walls and ceiling: the floor texture on the top, the bricks on the sides
    local roomTextures = function(material)
        material:setShaderFeature("TOP_BOTTOM", true)
        material:setShaderTexture("u_baseTexture", "textures/bricks.png")
        material:setShaderTexture("u_topTexture", "textures/floor_tiles.png")
        material:setShaderTexture("u_bottomTexture", "textures/ceiling_planks.png")
    end
    local room = box(ctx, 1.2, 1.0, 1.2)
    room:setRotation(0, 40, 0)
    shader(ctx, room, TRIPLANAR, nil, roomTextures)

    -- in object space, tipped over towards the camera: the ceiling planks on its underside
    local tipped = box(ctx, 1.0, 1.0, 1.0)
    tipped.position = Vector3(tipped.position.x, 0.8, tipped.position.z)
    tipped:setRotation(-125, 30, 0)
    shader(ctx, tipped, TRIPLANAR, nil, function(material)
        roomTextures(material)
        material:setShaderFeature("SPACE_OBJECT", true)
    end)

    -- the dirt overlay
    local dirty = box(ctx, 1.1, 1.1, 1.1)
    dirty:setRotation(0, -35, 0)
    shader(ctx, dirty, TRIPLANAR, nil, function(material)
        roomTextures(material)
        material:setShaderFeature("NOISE_MASK", true)
        material:setShaderTexture("u_noiseTexture", "textures/noise_stylized.png")
    end)

    -- lit per pixel and per vertex (Lambert), every feature
    local litTriplanar = function(material)
        roomTextures(material)
        material:setShaderFeature("NOISE_MASK", true)
        material:setShaderTexture("u_noiseTexture", "textures/noise_stylized.png")
        material:setShaderParam("dirtStrength", 0.4)
    end
    shader(ctx, sphere(ctx), TRIPLANAR, MaterialLighting.PIXEL, litTriplanar)
    shader(ctx, sphere(ctx), TRIPLANAR, MaterialLighting.VERTEX, litTriplanar)

    -- vertex_color: the painted blob unlit, per vertex and per pixel
    shader(ctx, model(ctx, "models/painted_blob.gltf"), VERTEX_COLOR)
    shader(ctx, model(ctx, "models/painted_blob.gltf"), VERTEX_COLOR, MaterialLighting.VERTEX)
    shader(ctx, model(ctx, "models/painted_blob.gltf"), VERTEX_COLOR, MaterialLighting.PIXEL)

    -- textured and tinted
    shader(ctx, model(ctx, "models/painted_blob.gltf"), VERTEX_COLOR, MaterialLighting.PIXEL, function(material)
        material:setShaderFeature("TEXTURE", true)
        material:setShaderTexture("u_baseTexture", "textures/checker.png")
        material:setShaderParam("tiling", Vector4(4, 2, 0, 0))
        material:setShaderParam("baseColor", srgb(1.0, 0.9, 0.8))
    end)

    -- the bumpy box's RGB vertex colours per pixel
    shader(ctx, model(ctx, "models/bumpy_box.gltf", 0.8), VERTEX_COLOR, MaterialLighting.PIXEL)

    -- instanced: a mesh without vertex colours takes the instance colours
    local stack = Primitive(scene)
    stack:createBox(0.5, 0.3, 0.5)
    ctx:place(stack, 0.0)
    stack:createInstancedMesh()
    local colours = { srgb(1, 0.3, 0.3), srgb(0.3, 1, 0.4), srgb(0.35, 0.55, 1), srgb(1, 0.9, 0.3) }
    for i = 1, 4 do
        stack:addInstance(Vector3(0, 0.15 + (i - 1) * 0.32, 0), Quaternion(), Vector3(1, 1, 1), colours[i])
    end
    shader(ctx, stack, VERTEX_COLOR, MaterialLighting.VERTEX)

    -- three static point lights over the lit rows: per pixel or per vertex as the
    -- material says (both), per pixel only, per vertex only
    local minX, maxX, minZ, maxZ = math.huge, -math.huge, math.huge, -math.huge
    for _, object in ipairs(ctx.objects) do
        local p = object.position
        minX, maxX = math.min(minX, p.x), math.max(maxX, p.x)
        minZ, maxZ = math.min(minZ, p.z), math.max(maxZ, p.z)
    end
    local lights = {
        { 0.25, 0.30, { 1.0, 0.55, 0.3 }, LightShading.BOTH },
        { 0.70, 0.45, { 0.35, 0.6, 1.0 }, LightShading.PIXEL },
        { 0.45, 0.80, { 0.5, 1.0, 0.45 }, LightShading.VERTEX },
    }
    for _, entry in ipairs(lights) do
        local light = Light(scene)
        light.type = LightType.POINT
        light:setColor(entry[3][1], entry[3][2], entry[3][3])
        light.intensity = 6
        light.range = 4.5
        light.shading = entry[4]
        light.position = Vector3(minX + (maxX - minX) * entry[1], 1.8, maxZ - (maxZ - minZ) * entry[2])
    end
end

return Stylized
