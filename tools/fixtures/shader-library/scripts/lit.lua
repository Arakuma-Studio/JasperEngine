-- The lit library shaders on the scene's lights: library:simple_lit per pixel, mixed and per
-- vertex (normal map, emission, shadows received), library:vertex_lit (vertex colour,
-- emission, instanced), next to the built-in Lit lit per pixel and per vertex, so the same
-- light can be compared across shaders and stages (the lights.glsl units make a light
-- equally bright in all of them). Twelve moving point lights, more than the per-object cap
-- (8), cycle the per-light shading options (both, per pixel, per vertex); the scene's
-- directional light casts the shadows.
local Lit = {}

local SIMPLE = "library:simple_lit"
local VERTEX = "library:vertex_lit"

local COLORS = {
    {1.0, 0.3, 0.25}, {0.25, 0.5, 1.0}, {0.35, 1.0, 0.4}, {1.0, 0.85, 0.25},
    {0.9, 0.35, 1.0}, {0.25, 1.0, 0.9},
}
local SHADINGS = { LightShading.BOTH, LightShading.PIXEL, LightShading.VERTEX }

-- the same dielectric sphere (glTF base colour 0.9, metallic 0) for every comparison
local function sphere(ctx)
    local mesh = Model(ctx.scene)
    if not mesh:loadModel("models/sphere.gltf") then
        ctx:fail("models/sphere.gltf did not load")
    end
    ctx:place(mesh, 0.8)
    return mesh
end

-- a library shader with a lighting mode (nil keeps the shader's default)
local function libraryShader(ctx, mesh, shader, lighting, configure)
    ctx:setShader(mesh, shader, function(material, i)
        if lighting then
            material.lighting = lighting
        end
        if configure then
            configure(material, i)
        end
    end)
end

-- the built-in Lit on the sphere's own dielectric material, to compare the library
-- shaders' diffuse with
local function litReference(ctx, mesh, lighting)
    ctx:setShader(mesh, "", function(material)
        material.lighting = lighting
    end)
end

function Lit.build(ctx)
    local scene = ctx.scene
    -- the sphere model's base colour (linear), for the library shaders
    local grey = Vector4(0.9, 0.9, 0.9, 1.0)

    -- the same light per pixel, mixed and per vertex, in simple_lit, vertex_lit and Lit
    libraryShader(ctx, sphere(ctx), SIMPLE, MaterialLighting.PIXEL, function(material)
        material:setShaderParam("baseColor", grey)
    end)
    libraryShader(ctx, sphere(ctx), SIMPLE, MaterialLighting.MIXED, function(material)
        material:setShaderParam("baseColor", grey)
    end)
    libraryShader(ctx, sphere(ctx), SIMPLE, MaterialLighting.VERTEX, function(material)
        material:setShaderParam("baseColor", grey)
    end)
    libraryShader(ctx, sphere(ctx), VERTEX, nil, function(material)
        material:setShaderParam("baseColor", grey)
    end)
    litReference(ctx, sphere(ctx), MaterialLighting.PIXEL)
    litReference(ctx, sphere(ctx), MaterialLighting.VERTEX)

    -- normal map (no tangents: the frame comes from derivatives), textured, glossy
    local bumpy = Model(scene)
    if bumpy:loadModel("models/bumpy_box.gltf") then
        ctx:place(bumpy, 0.8)
        libraryShader(ctx, bumpy, SIMPLE, MaterialLighting.PIXEL, function(material)
            material:setShaderFeature("NORMAL_MAP", true)
            material:setShaderTexture("u_normalTexture", "models/normal.png")
            material:setShaderTexture("u_baseTexture", "textures/checker.png")
            material:setShaderParam("specularColor", Vector4(0.5, 0.5, 0.5, 1.0))
            material:setShaderParam("gloss", 0.7)
        end)
    else
        ctx:fail("models/bumpy_box.gltf did not load")
    end

    -- emission over mixed lighting
    local glowing = Primitive(scene)
    glowing:createBox(1.1, 1.1, 1.1)
    ctx:place(glowing, 0.55)
    glowing:setRotation(0, 30, 0)
    libraryShader(ctx, glowing, SIMPLE, MaterialLighting.MIXED, function(material)
        material:setShaderParam("baseColor", Vector4(0.3, 0.3, 0.35, 1.0))
        material:setShaderFeature("EMISSION", true)
        material:setShaderTexture("u_emissionTexture", "textures/checker.png")
        material:setShaderParam("emissionColor", Vector4(0.1, 0.6, 1.0, 1.0))
    end)

    -- vertex colours (the model has RGB ones), per vertex
    local painted = Model(scene)
    if painted:loadModel("models/bumpy_box.gltf") then
        ctx:place(painted, 0.8)
        libraryShader(ctx, painted, VERTEX, nil, function(material)
            material:setShaderFeature("VERTEX_COLOR", true)
        end)
    else
        ctx:fail("models/bumpy_box.gltf did not load")
    end

    -- instanced: the batch is one object for its lights
    local rings = Primitive(scene)
    rings:createTorus(0.35, 0.12)
    ctx:place(rings, 0.0)
    rings:createInstancedMesh()
    local tints = { Vector4(1, 0.4, 0.4, 1), Vector4(0.4, 1, 0.5, 1), Vector4(0.5, 0.6, 1, 1), Vector4(1, 1, 0.4, 1) }
    for i = 1, 4 do
        rings:addInstance(Vector3(0, 0.3 + (i - 1) * 0.35, 0), Quaternion(), Vector3(1, 1, 1), tints[i])
    end
    libraryShader(ctx, rings, VERTEX, nil, function(material)
        material:setShaderFeature("EMISSION", true)
        material:setShaderParam("emissionColor", Vector4(0.05, 0.05, 0.05, 1.0))
    end)

    -- shadows received per pixel: a tile under a floating box, both simple_lit
    local tile = Primitive(scene)
    tile:createPlane(1.7, 1.7)
    local tilePosition = ctx:place(tile, 0.02)
    libraryShader(ctx, tile, SIMPLE, MaterialLighting.PIXEL, function(material)
        material:setShaderParam("baseColor", Vector4(0.9, 0.9, 0.85, 1.0))
    end)
    local caster = Primitive(scene)
    caster:createBox(0.6, 0.15, 0.6)
    caster.position = Vector3(tilePosition.x + 0.25, 1.1, tilePosition.z + 0.35)
    table.insert(ctx.objects, caster)
    libraryShader(ctx, caster, SIMPLE, MaterialLighting.PIXEL, function(material)
        material:setShaderParam("baseColor", Vector4(0.9, 0.5, 0.2, 1.0))
    end)

    -- a mode the shader does not list (vertex_lit is vertex only): drawn in its default,
    -- with one warning in the log
    libraryShader(ctx, sphere(ctx), VERTEX, MaterialLighting.PIXEL, function(material)
        material:setShaderParam("baseColor", Vector4(0.9, 0.75, 0.6, 1.0))
    end)

    -- skinned + morph targets, mixed
    local strip = Model(scene)
    if strip:loadModel("models/skinned_morph.gltf") then
        ctx:place(strip, 0.0)
        strip:setMorphWeight(0, 0.8)
        libraryShader(ctx, strip, SIMPLE, MaterialLighting.MIXED, function(material)
            material:setShaderParam("baseColor", Vector4(0.95, 0.75, 0.2, 1.0))
        end)
    else
        ctx:fail("models/skinned_morph.gltf did not load")
    end

    -- the group's bounds, for the moving lights
    local minX, maxX, minZ, maxZ = math.huge, -math.huge, math.huge, -math.huge
    for _, object in ipairs(ctx.objects) do
        local p = object.position
        minX, maxX = math.min(minX, p.x), math.max(maxX, p.x)
        minZ, maxZ = math.min(minZ, p.z), math.max(maxZ, p.z)
    end
    ctx.center = Vector3((minX + maxX) / 2, 0, (minZ + maxZ) / 2)
    ctx.halfWidth = (maxX - minX) / 2 + 0.5
    ctx.halfDepth = (maxZ - minZ) / 2 + 0.6

    -- twelve moving point lights (more than the cap), each shading option in turn
    ctx.lights = {}
    for i = 1, 12 do
        local light = Light(scene)
        light.type = LightType.POINT
        local c = COLORS[(i - 1) % #COLORS + 1]
        light:setColor(c[1], c[2], c[3])
        light.intensity = 5
        light.range = 3.5
        light.shading = SHADINGS[(i - 1) % 3 + 1]
        table.insert(ctx.lights, { light = light, phase = i * 0.52, speed = 0.35 + (i % 4) * 0.1, height = 1.4 + (i % 3) * 0.3 })
    end
    Lit.update(ctx, 0)
end

function Lit.update(ctx, dt)
    ctx.time = (ctx.time or 0) + dt
    for _, entry in ipairs(ctx.lights) do
        local a = ctx.time * entry.speed + entry.phase
        entry.light.position = Vector3(ctx.center.x + math.cos(a) * ctx.halfWidth,
                                       entry.height,
                                       ctx.center.z + math.sin(a * 1.3) * ctx.halfDepth)
    end
end

return Lit
