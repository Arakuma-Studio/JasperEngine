-- The effect, nature and animated library shaders, each with its features and keywords:
-- library:rim, hologram, dissolve, panner, flipbook, water, foliage, spin_bob and decal.
-- The ones that move vertices or discard fragments cast shadows on the ground from their
-- depth programs: the swaying plants, the spinning and bobbing pickups, the dissolving
-- spheres (with the holes), the clipped flipbook. Decals lie on the ground and on a wall.
-- The see-through ones (rim, hologram, water, decal, the blended flipbooks) cast no shadow
-- by their shaders' state (@state shadows=off), with the meshes' castShadows left on; the
-- blended panner cards keep castShadows off themselves (panner is opaque by default).
-- Assets: textures/fx_*.png and models/fx_*.gltf, from effects_assets.py.
local Effects = {}

local RIM = "library:rim"
local HOLOGRAM = "library:hologram"
local DISSOLVE = "library:dissolve"
local PANNER = "library:panner"
local FLIPBOOK = "library:flipbook"
local WATER = "library:water"
local FOLIAGE = "library:foliage"
local SPIN_BOB = "library:spin_bob"
local DECAL = "library:decal"

-- a quad standing up (createWall: +Z normal, upright UVs), tilted back towards the camera
local function card(ctx, width, height)
    local mesh = Primitive(ctx.scene)
    mesh:createWall(width, height or width)
    mesh:setRotation(-20, 0, 0)
    return mesh
end

local function model(ctx, path)
    local mesh = Model(ctx.scene)
    if not mesh:loadModel(path) then
        ctx:fail("%s did not load", path)
    end
    return mesh
end

local function sphere(ctx, radius)
    local mesh = Primitive(ctx.scene)
    mesh:createSphere(radius, 32, 16)
    return mesh
end

-- a mesh placed beside the cell's main one (not a cell of its own)
local function extra(ctx, object, position)
    object.position = position
    table.insert(ctx.objects, object)
    return object
end

local function rimAndHologram(ctx)
    -- rim: the plain fresnel glow, additive
    local glow = sphere(ctx, 0.65)
    ctx:place(glow, 0.8)
    ctx:setShader(glow, RIM)

    -- rim: a force field, both faces, with the scrolling hexagon pattern
    local shield = sphere(ctx, 0.7)
    ctx:place(shield, 0.8)
    ctx:setShader(shield, RIM, function(material)
        material:setShaderFeature("TEXTURE", true)
        material:setShaderTexture("u_patternTexture", "textures/fx_hex.png")
        material:setShaderParam("rimColor", Vector4(0.2, 1.0, 0.35, 1.0))
        material:setShaderParam("textureTiling", Vector4(6, 3, 0, 0))
        material:setShaderParam("intensity", 1.2)
        material.cullMode = MaterialCullMode.OFF
    end)

    -- hologram: procedural scanlines on a torus knot stand-in, additive
    local ring = Primitive(ctx.scene)
    ring:createTorus(0.45, 0.16)
    ctx:place(ring, 0.8)
    ring:setRotation(70, 0, 0)
    ctx:setShader(ring, HOLOGRAM)

    -- hologram: scanlines from a texture, in orange
    local box = Primitive(ctx.scene)
    box:createBox(0.9, 1.1, 0.9)
    ctx:place(box, 0.7)
    ctx:setShader(box, HOLOGRAM, function(material)
        material:setShaderFeature("SCANLINE_TEXTURE", true)
        material:setShaderTexture("u_scanlineTexture", "textures/fx_scanlines.png")
        material:setShaderParam("tint", Vector4(0.7, 0.25, 0.04, 1.0))
        material:setShaderParam("scanlineDensity", 4)
    end)

    -- hologram: BLEND_MODE ALPHA, which alone makes it alpha blended (@state[...]): a
    -- dark-tinted figure that covers what is behind it instead of adding light
    local figure = model(ctx, "models/sphere.gltf")
    ctx:place(figure, 0.8)
    ctx:setShader(figure, HOLOGRAM, function(material)
        material:setShaderFeature("BLEND_MODE_ALPHA", true)
        material:setShaderParam("tint", Vector4(0.05, 0.2, 0.9, 1.0))
        material:setShaderParam("fill", 0.35)
    end)
end

local function dissolve(ctx)
    ctx.dissolving = {}

    -- noise on the UVs, unlit, floating: its shadow on the ground has the same holes
    local a = sphere(ctx, 0.5)
    ctx:place(a, 1.25)
    ctx:setShader(a, DISSOLVE, function(material)
        material:setShaderTexture("u_noiseTexture", "textures/fx_noise.png")
        material:setShaderTexture("u_baseTexture", "textures/checker.png")
        material:setShaderParam("noiseTiling", Vector4(2, 1, 0, 0))
    end)
    table.insert(ctx.dissolving, { mesh = a, phase = 0.0 })

    -- WORLD_NOISE, lit per pixel: no seam where the UVs wrap, a lit shadow with holes
    local b = sphere(ctx, 0.5)
    ctx:place(b, 1.25)
    ctx:setShader(b, DISSOLVE, function(material)
        material.lighting = MaterialLighting.PIXEL
        material:setShaderFeature("WORLD_NOISE", true)
        material:setShaderTexture("u_noiseTexture", "textures/fx_noise.png")
        material:setShaderParam("baseColor", Vector4(0.2, 0.45, 0.9, 1.0))
        material:setShaderParam("edgeColor", Vector4(0.3, 2.0, 4.0, 1.0))
        material:setShaderParam("worldNoiseScale", 0.8)
    end)
    table.insert(ctx.dissolving, { mesh = b, phase = 1.3 })

    -- vertex colours, lit per pixel, spinning, a held cutoff
    local painted = model(ctx, "models/bumpy_box.gltf")
    ctx:place(painted, 1.1)
    painted.scale = Vector3(0.75, 0.75, 0.75)
    ctx:setShader(painted, DISSOLVE, function(material)
        material.lighting = MaterialLighting.PIXEL
        material:setShaderFeature("VERTEX_COLOR", true)
        material:setShaderTexture("u_noiseTexture", "textures/fx_noise.png")
        material:setShaderParam("cutoff", 0.45)
        material:setShaderParam("edgeWidth", 0.12)
    end)
    ctx.dissolveSpinner = painted
end

local function panner(ctx)
    -- a conveyor belt: opaque, one layer scrolling towards the camera
    local belt = Primitive(ctx.scene)
    belt:createBox(1.0, 0.12, 1.6)
    ctx:place(belt, 0.3)
    ctx:setShader(belt, PANNER, function(material)
        material:setShaderTexture("u_baseTexture", "textures/fx_belt.png")
        material:setShaderParam("tiling", Vector4(1, 2, 0, 0))
        material:setShaderParam("speed", Vector2(0, -0.8))
    end)

    -- clouds: two streak layers crossing, multiplied, alpha blended
    local clouds = card(ctx, 1.5, 1.2)
    ctx:place(clouds, 0.9)
    clouds.castShadows = false
    ctx:setShader(clouds, PANNER, function(material)
        material:setShaderFeature("SECOND_LAYER", true)
        material:setShaderTexture("u_baseTexture", "textures/fx_streaks.png")
        material:setShaderTexture("u_secondTexture", "textures/fx_streaks.png")
        material:setShaderParam("tiling", Vector4(1, 0.5, 0, 0))
        material:setShaderParam("speed", Vector2(0.15, 0))
        material:setShaderParam("secondTiling", Vector4(0.7, 0.3, 0.5, 0))
        material:setShaderParam("secondSpeed", Vector2(-0.05, 0.03))
        material:setShaderParam("tint", Vector4(1.6, 1.6, 1.8, 1.0))
        material.blendMode = MaterialBlendMode.ALPHA
        material.cullMode = MaterialCullMode.OFF
    end)

    -- a laser: a thin cylinder, additive, two streak layers added (COMBINE ADD)
    local laser = Primitive(ctx.scene)
    laser:createCylinder(0.08, 1.6)
    ctx:place(laser, 0.9)
    laser:setRotation(0, 0, 70)
    laser.castShadows = false
    ctx:setShader(laser, PANNER, function(material)
        material:setShaderFeature("SECOND_LAYER", true)
        material:setShaderFeature("COMBINE_ADD", true)
        material:setShaderTexture("u_baseTexture", "textures/fx_streaks.png")
        material:setShaderTexture("u_secondTexture", "textures/fx_streaks.png")
        material:setShaderParam("tint", Vector4(1.5, 0.06, 0.04, 1.0))
        material:setShaderParam("tiling", Vector4(1, 2, 0, 0))
        material:setShaderParam("speed", Vector2(0, -2.0))
        material:setShaderParam("secondTiling", Vector4(2, 1, 0.3, 0))
        material:setShaderParam("secondSpeed", Vector2(0.2, -3.1))
        material.blendMode = MaterialBlendMode.ADDITIVE
    end)

    -- a waterfall: streaks falling, wobbled by the distortion noise, alpha blended
    local fall = card(ctx, 1.1, 1.5)
    ctx:place(fall, 0.85)
    fall.castShadows = false
    ctx:setShader(fall, PANNER, function(material)
        material:setShaderFeature("DISTORTION", true)
        material:setShaderFeature("VERTEX_COLOR", true)
        material:setShaderTexture("u_baseTexture", "textures/fx_streaks.png")
        material:setShaderTexture("u_distortionTexture", "textures/fx_noise.png")
        material:setShaderParam("tint", Vector4(0.45, 0.75, 1.0, 1.0))
        material:setShaderParam("speed", Vector2(0, -1.2))
        material:setShaderParam("distortionStrength", 0.06)
        material.blendMode = MaterialBlendMode.ALPHA
    end)
end

local function flipbook(ctx)
    -- looping at 6 frames per second, alpha blended
    local loop = card(ctx, 1.2)
    ctx:place(loop, 0.85)
    ctx:setShader(loop, FLIPBOOK, function(material)
        material:setShaderTexture("u_sheetTexture", "textures/fx_flipbook.png")
        material:setShaderParam("fps", 6)
    end)

    -- FRAME_BLEND at 2 frames per second: each frame crossfades into the next
    local blend = card(ctx, 1.2)
    ctx:place(blend, 0.85)
    ctx:setShader(blend, FLIPBOOK, function(material)
        material:setShaderFeature("FRAME_BLEND", true)
        material:setShaderTexture("u_sheetTexture", "textures/fx_flipbook.png")
        material:setShaderParam("fps", 2)
    end)

    -- ALPHA_CLIP, opaque, frames 4 to 11 only: casts a clipped shadow on the ground
    local clipped = card(ctx, 1.2)
    ctx:place(clipped, 0.85)
    ctx:setShader(clipped, FLIPBOOK, function(material)
        material:setShaderFeature("ALPHA_CLIP", true)
        material:setShaderTexture("u_sheetTexture", "textures/fx_flipbook.png")
        material:setShaderParam("startFrame", 4)
        material:setShaderParam("frameCount", 8)
        material:setShaderParam("fps", 4)
        material.blendMode = MaterialBlendMode.NONE
        material.cullMode = MaterialCullMode.OFF
    end)

    -- holds the last frame (loop off), restarted every 5 seconds through startTime
    local once = card(ctx, 1.2)
    ctx:place(once, 0.85)
    ctx:setShader(once, FLIPBOOK, function(material)
        material:setShaderTexture("u_sheetTexture", "textures/fx_flipbook.png")
        material:setShaderParam("fps", 8)
        material:setShaderParam("loop", 0)
        material:setShaderParam("startTime", Engine.systemTime)
    end)
    ctx.oneShot = once
    ctx.oneShotTime = 0
end

local function water(ctx)
    local function waterTile(configure)
        local tile = model(ctx, "models/fx_water_grid.gltf")
        ctx:place(tile, 0.25)
        ctx:setShader(tile, WATER, function(material)
            material:setShaderTexture("u_normalTexture", "textures/fx_water_normal.png")
            material:setShaderTexture("u_noiseTexture", "textures/fx_noise.png")
            if configure then
                configure(material)
            end
        end)
        return tile
    end
    -- unlit
    waterTile()
    -- lit per pixel: the glint of the sun
    waterTile(function(material)
        material.lighting = MaterialLighting.PIXEL
    end)
    -- lit per pixel with waves, more foam
    waterTile(function(material)
        material.lighting = MaterialLighting.PIXEL
        material:setShaderFeature("WAVES", true)
        material:setShaderParam("waveHeight", 0.08)
        material:setShaderParam("waveLength", 1.2)
        material:setShaderParam("foamAmount", 0.28)
    end)
end

local function foliage(ctx)
    local function plant(lighting, configure)
        local mesh = model(ctx, "models/fx_plant.gltf")
        ctx:place(mesh, 0.0)
        ctx:setShader(mesh, FOLIAGE, function(material)
            if lighting then
                material.lighting = lighting
            end
            material:setShaderTexture("u_baseTexture", "textures/fx_leaves.png")
            if configure then
                configure(material)
            end
        end)
        return mesh
    end
    -- per pixel (default), the vertex colour's red as the wind mask: sways and casts a
    -- swaying, cut-out shadow
    plant(nil, function(material)
        material:setShaderParam("windStrength", 0.18)
    end)
    -- the height as the mask
    plant(nil, function(material)
        material:setShaderFeature("WIND_MASK_HEIGHT", true)
        material:setShaderParam("heightRange", 1.2)
        material:setShaderParam("windStrength", 0.2)
    end)
    -- per vertex
    plant(MaterialLighting.VERTEX, function(material)
        material:setShaderParam("windStrength", 0.15)
    end)
    -- unlit, tinted
    plant(MaterialLighting.NONE, function(material)
        material:setShaderParam("baseColor", Vector4(0.9, 0.7, 0.3, 1.0))
    end)
    -- an instanced clump: each instance sways in its own phase
    local clump = model(ctx, "models/fx_plant.gltf")
    ctx:place(clump, 0.0)
    clump:createInstancedMesh()
    for i = 0, 3 do
        local angle = i * 1.7
        clump:addInstance(Vector3(math.cos(angle) * 0.45, 0, math.sin(angle) * 0.45), Quaternion(), Vector3(0.6, 0.6, 0.6))
    end
    ctx:setShader(clump, FOLIAGE, function(material)
        material:setShaderTexture("u_baseTexture", "textures/fx_leaves.png")
        material:setShaderParam("windStrength", 0.2)
    end)
end

local function spinBob(ctx)
    -- a row of coins, unlit: each in its own phase
    local first = Primitive(ctx.scene)
    first:createBox(0.42, 0.42, 0.06)
    local position = ctx:place(first, 0.7)
    local coins = { first }
    for i = 1, 2 do
        local coin = Primitive(ctx.scene)
        coin:createBox(0.42, 0.42, 0.06)
        extra(ctx, coin, Vector3(position.x + (i == 1 and -0.55 or 0.55), 0.7, position.z))
        table.insert(coins, coin)
    end
    for _, coin in ipairs(coins) do
        ctx:setShader(coin, SPIN_BOB, function(material)
            material:setShaderTexture("u_baseTexture", "textures/checker.png")
            material:setShaderParam("bobHeight", 0.2)
        end)
    end

    -- lit per pixel, casting a turning, bobbing shadow
    local box = Primitive(ctx.scene)
    box:createBox(0.7, 0.7, 0.2)
    ctx:place(box, 0.9)
    ctx:setShader(box, SPIN_BOB, function(material)
        material.lighting = MaterialLighting.PIXEL
        material:setShaderParam("baseColor", Vector4(0.9, 0.25, 0.2, 1.0))
        material:setShaderParam("spinSpeed", 60)
        material:setShaderParam("bobHeight", 0.25)
    end)

    -- lit per vertex, vertex colours, scaled: the bob stays in world units
    local painted = model(ctx, "models/bumpy_box.gltf")
    ctx:place(painted, 0.9)
    painted.scale = Vector3(0.5, 0.5, 0.5)
    ctx:setShader(painted, SPIN_BOB, function(material)
        material.lighting = MaterialLighting.VERTEX
        material:setShaderFeature("VERTEX_COLOR", true)
        material:setShaderParam("baseColor", Vector4(1, 1, 1, 1))
        material:setShaderParam("spinSpeed", -120)
    end)

    -- instanced gems: every instance spins about its own axis, in its own phase
    local gems = Primitive(ctx.scene)
    gems:createBox(0.25, 0.25, 0.25)
    ctx:place(gems, 0.0)
    gems:createInstancedMesh()
    for i = 0, 3 do
        gems:addInstance(Vector3(-0.6 + i * 0.4, 0.6, 0), Quaternion(), Vector3(1, 1, 1),
            Vector4(0.3 + 0.2 * i, 1.0 - 0.2 * i, 0.6, 1.0))
    end
    ctx:setShader(gems, SPIN_BOB, function(material)
        material.lighting = MaterialLighting.PIXEL
        material:setShaderParam("baseColor", Vector4(1, 1, 1, 1))
        material:setShaderParam("spinSpeed", 180)
    end)
end

local function decals(ctx)
    -- on the ground, coplanar with it: the depth offset keeps it from z-fighting
    local hazard = Primitive(ctx.scene)
    hazard:createPlane(1.5, 1.5)
    ctx:place(hazard, 0.0)
    ctx:setShader(hazard, DECAL, function(material)
        material:setShaderTexture("u_baseTexture", "textures/fx_decal.png")
    end)

    -- dirt multiplied onto the ground (DECAL_BLEND MULTIPLY alone makes it a multiply blend)
    local dirt = Primitive(ctx.scene)
    dirt:createPlane(1.6, 1.6)
    ctx:place(dirt, 0.0)
    ctx:setShader(dirt, DECAL, function(material)
        material:setShaderFeature("DECAL_BLEND_MULTIPLY", true)
        material:setShaderTexture("u_baseTexture", "textures/fx_dirt.png")
    end)

    -- on a wall: a lit box with an alpha decal and a dirt decal on its front face
    local wall = Primitive(ctx.scene)
    wall:createBox(1.6, 1.3, 0.2)
    local position = ctx:place(wall, 0.65)
    wall:setColor(0.8, 0.8, 0.82)
    local sign = Primitive(ctx.scene)
    sign:createWall(0.8, 0.8)
    extra(ctx, sign, Vector3(position.x - 0.3, 0.7, position.z + 0.1))
    ctx:setShader(sign, DECAL, function(material)
        material:setShaderTexture("u_baseTexture", "textures/fx_decal.png")
        material:setShaderParam("tint", Vector4(1.0, 0.6, 0.6, 1.0))
    end)
    local stain = Primitive(ctx.scene)
    stain:createWall(0.9, 0.9)
    extra(ctx, stain, Vector3(position.x + 0.35, 0.55, position.z + 0.1))
    ctx:setShader(stain, DECAL, function(material)
        material:setShaderFeature("DECAL_BLEND_MULTIPLY", true)
        material:setShaderTexture("u_baseTexture", "textures/fx_dirt.png")
    end)
end

function Effects.build(ctx)
    rimAndHologram(ctx)
    dissolve(ctx)
    panner(ctx)
    flipbook(ctx)
    water(ctx)
    foliage(ctx)
    spinBob(ctx)
    decals(ctx)
end

function Effects.update(ctx, dt)
    ctx.time = (ctx.time or 0) + dt

    -- the dissolving spheres burn away and come back
    for _, entry in ipairs(ctx.dissolving) do
        local material = entry.mesh:getMaterial(0)
        material:setShaderParam("cutoff", 0.5 - 0.5 * math.cos(ctx.time * 0.6 + entry.phase))
        entry.mesh:setMaterial(0, material)
    end
    ctx.dissolveSpinner:setRotation(0, ctx.time * 25, 0)

    -- the one-shot flipbook restarts every 5 seconds
    ctx.oneShotTime = ctx.oneShotTime + dt
    if ctx.oneShotTime > 5 then
        ctx.oneShotTime = 0
        local material = ctx.oneShot:getMaterial(0)
        material:setShaderParam("startTime", Engine.systemTime)
        ctx.oneShot:setMaterial(0, material)
    end
end

return Effects
