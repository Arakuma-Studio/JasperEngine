-- library:unlit in its variants: colour, texture and tiling, vertex colour, alpha clip, the
-- blend modes (alpha, additive, premultiplied, multiply), sort priority, skinned + morph
-- targets, and instanced with per-instance colours; then a project fork with a manifest
local Core = {}

local UNLIT = "library:unlit"

-- a quad standing up, facing the camera, both faces drawn
local function quad(ctx, size)
    local mesh = Primitive(ctx.scene)
    mesh:createPlane(size, size)
    mesh:setRotation(70, 0, 0)
    return mesh
end

function Core.build(ctx)
    local scene = ctx.scene

    -- colour only
    local colour = Primitive(scene)
    colour:createSphere(0.7)
    ctx:place(colour, 0.7)
    ctx:setShader(colour, UNLIT, function(material)
        material:setShaderParam("baseColor", Vector4(0.05, 0.55, 0.6, 1.0))
    end)

    -- base map
    local textured = Primitive(scene)
    textured:createBox(1.1, 1.1, 1.1)
    ctx:place(textured, 0.55)
    textured:setRotation(0, 30, 0)
    ctx:setShader(textured, UNLIT, function(material)
        material:setShaderTexture("u_baseTexture", "textures/checker.png")
    end)

    -- tiling and offset, tinted
    local tiled = Primitive(scene)
    tiled:createBox(1.1, 1.1, 1.1)
    ctx:place(tiled, 0.55)
    tiled:setRotation(0, -20, 0)
    ctx:setShader(tiled, UNLIT, function(material)
        material:setShaderTexture("u_baseTexture", "textures/checker.png")
        material:setShaderParam("tiling", Vector4(3, 3, 0.25, 0))
        material:setShaderParam("baseColor", Vector4(0.5, 0.8, 1.0, 1.0))
    end)

    -- vertex colours (the model has RGB ones)
    local painted = Model(scene)
    if painted:loadModel("models/bumpy_box.gltf") then
        ctx:place(painted, 0.8)
        ctx:setShader(painted, UNLIT, function(material)
            material:setShaderFeature("VERTEX_COLOR", true)
        end)
    else
        ctx:fail("models/bumpy_box.gltf did not load")
    end

    -- alpha clip: the leaf's transparent border is cut, in shadows too (depth program)
    local leaf = quad(ctx, 1.4)
    ctx:place(leaf, 0.8)
    ctx:setShader(leaf, UNLIT, function(material)
        material:setShaderTexture("u_baseTexture", "textures/leaf.png")
        material:setShaderFeature("ALPHA_CLIP", true)
        material:setShaderParam("alphaCutoff", 0.5)
        material.cullMode = MaterialCullMode.OFF
    end)

    -- alpha blend
    local glass = Primitive(scene)
    glass:createSphere(0.7)
    ctx:place(glass, 0.7)
    ctx:setShader(glass, UNLIT, function(material)
        material:setShaderParam("baseColor", Vector4(0.9, 0.2, 0.3, 0.45))
        material.blendMode = MaterialBlendMode.ALPHA
    end)

    -- additive glow over the ground
    local additive = quad(ctx, 1.6)
    ctx:place(additive, 0.8)
    additive.castShadows = false
    ctx:setShader(additive, UNLIT, function(material)
        material:setShaderTexture("u_baseTexture", "textures/soft.png")
        material:setShaderParam("baseColor", Vector4(1.0, 0.55, 0.1, 1.0))
        material.blendMode = MaterialBlendMode.ADDITIVE
        material.cullMode = MaterialCullMode.OFF
    end)

    -- premultiplied: a glow whose colour is already multiplied by its alpha
    local premultiplied = quad(ctx, 1.6)
    ctx:place(premultiplied, 0.8)
    premultiplied.castShadows = false
    ctx:setShader(premultiplied, UNLIT, function(material)
        material:setShaderTexture("u_baseTexture", "textures/soft_premultiplied.png")
        material:setShaderParam("baseColor", Vector4(0.15, 0.4, 0.9, 1.0))
        material.blendMode = MaterialBlendMode.PREMULTIPLIED
        material.cullMode = MaterialCullMode.OFF
    end)

    -- multiply: darkens what is behind it with the checker
    local multiply = quad(ctx, 1.6)
    ctx:place(multiply, 0.8)
    multiply.castShadows = false
    ctx:setShader(multiply, UNLIT, function(material)
        material:setShaderTexture("u_baseTexture", "textures/checker.png")
        material.blendMode = MaterialBlendMode.MULTIPLY
        material.cullMode = MaterialCullMode.OFF
    end)

    -- two overlapping alpha quads: the higher sort priority draws last, on top
    local back = quad(ctx, 1.4)
    local position = ctx:place(back, 0.8)
    ctx:setShader(back, UNLIT, function(material)
        material:setShaderParam("baseColor", Vector4(0.1, 0.9, 0.2, 0.6))
        material.blendMode = MaterialBlendMode.ALPHA
        material.cullMode = MaterialCullMode.OFF
        material.sortPriority = 1
    end)
    local front = quad(ctx, 1.0)
    front.position = Vector3(position.x + 0.3, position.y - 0.2, position.z - 0.3)
    ctx:setShader(front, UNLIT, function(material)
        material:setShaderParam("baseColor", Vector4(0.9, 0.9, 0.1, 0.6))
        material.blendMode = MaterialBlendMode.ALPHA
        material.cullMode = MaterialCullMode.OFF
        material.sortPriority = 0
    end)

    -- skinned + morph targets
    local strip = Model(scene)
    if strip:loadModel("models/skinned_morph.gltf") then
        ctx:place(strip, 0.0)
        strip:setMorphWeight(0, 0.8)
        ctx:setShader(strip, UNLIT, function(material)
            material:setShaderParam("baseColor", Vector4(0.95, 0.75, 0.2, 1.0))
        end)
    else
        ctx:fail("models/skinned_morph.gltf did not load")
    end

    -- instanced, the instance colours through v_color
    local rings = Primitive(scene)
    rings:createTorus(0.35, 0.12)
    ctx:place(rings, 0.0)
    rings:createInstancedMesh()
    local colours = { Vector4(1, 0.2, 0.2, 1), Vector4(0.2, 1, 0.3, 1), Vector4(0.3, 0.5, 1, 1), Vector4(1, 1, 0.2, 1) }
    for i = 1, 4 do
        rings:addInstance(Vector3(0, 0.3 + (i - 1) * 0.35, 0), Quaternion(), Vector3(1, 1, 1), colours[i])
    end
    ctx:setShader(rings, UNLIT, function(material)
        material:setShaderTexture("u_baseTexture", "textures/checker.png")
    end)

    -- a project fork with a manifest on the library vertex path: the frame block (camera,
    -- main light, ambient, time) in both stages, a custom texture left to its default, a
    -- feature, and the displacement hook
    local fork = Primitive(scene)
    fork:createSphere(0.7, 32, 16)
    ctx:place(fork, 0.8)
    ctx:setShader(fork, "assets/shaders/frame_rim", function(material)
        material:setShaderFeature("PULSE", true)
    end)

    ctx.spinning = { textured, tiled }
end

function Core.update(ctx, dt)
    ctx.angle = (ctx.angle or 0) + dt * 30
    for _, mesh in ipairs(ctx.spinning) do
        mesh:setRotation(0, ctx.angle, 0)
    end
end

return Core
