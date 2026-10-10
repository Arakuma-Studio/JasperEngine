-- The 2D library shaders. library:sprite: each feature alone, an atlas animation and a
-- static atlas frame with OUTLINE (the atlas frames have opaque strips against their
-- neighbours: an outline that bled across frames would draw lines along the frame edges),
-- instanced atlas frames, straight and premultiplied soft sprites, a hue cycle, all
-- features at once and, built alone, a sprite lit by 2D lights with a 2D shadow.
-- library:particle: the alpha, additive, premultiplied and multiply blend modes over a
-- checker backdrop, GLOW, EROSION (alpha 1 -> 0.15 over the six) and untextured points.
-- library:ui_fx, on a UI layer: an Image and a Text plain and with each feature.
--
-- Built alone (SL_GROUPS=2d) the rows stand as a wall facing the camera, and the scene
-- gets the 2D lights (they reach every mesh in a scene, so only then).
--   SMOKE_GAME_ENV="SL_GROUPS=2d" SMOKE_GAME_SECONDS="10 12" tools/smoke-test.sh tools/fixtures/shader-library
local TwoD = {}

local SPRITE = "library:sprite"
local UI_FX = "library:ui_fx"
local PARTICLE = "library:particle"

local SIZE = 1.5       -- world units
local TEXELS = 64      -- sprite sizes are whole units: 64, scaled to SIZE
local ROW_HEIGHT = 1.9

-- the next cell: rows go back along -Z, or up in a wall when built alone
local function put(ctx, object, y)
    local position = ctx:place(object, y)
    if ctx.solo then
        position = Vector3(position.x, y + (ctx.rows - 1) * ROW_HEIGHT, ctx.originZ)
        object.position = position
    end
    return position
end

local function sprite(ctx, texture, configure)
    local s = Sprite(ctx.scene)
    s:setTexture(texture)
    s:setSize(TEXELS, TEXELS)
    s:setScale(SIZE / TEXELS)
    s.pivotPreset = PivotPreset.CENTER
    s.castShadows = false
    s.receiveLights = false -- the 2D-lit one sets it back
    put(ctx, s, 1.0)
    ctx:setShader(s, SPRITE, configure)
    return s
end

-- the atlas frames (2 x 2 of 64 px), normalised
local FRAMES = { Rect(0, 0, 0.5, 0.5), Rect(0.5, 0, 0.5, 0.5), Rect(0, 0.5, 0.5, 0.5), Rect(0.5, 0.5, 0.5, 0.5) }

local function buildSprites(ctx)
    -- row 1: each feature alone on the character
    sprite(ctx, "textures/character.png")
    sprite(ctx, "textures/character.png", function(m)
        m:setShaderFeature("OUTLINE", true)
        m:setShaderParam("outlineColor", Vector4(1.0, 0.85, 0.1, 1.0))
        m:setShaderParam("outlineWidth", 2.0)
    end)
    sprite(ctx, "textures/character.png", function(m)
        m:setShaderFeature("HUE_SHIFT", true)
        m:setShaderParam("hueShift", 150.0)
    end)
    sprite(ctx, "textures/character.png", function(m)
        m:setShaderFeature("FLASH", true)
        m:setShaderParam("flashColor", Vector4(1.0, 0.1, 0.1, 1.0))
        m:setShaderParam("flashAmount", 0.6)
    end)
    sprite(ctx, "textures/character.png", function(m)
        m:setShaderFeature("GRAYSCALE", true)
    end)
    sprite(ctx, "textures/character.png", function(m)
        m:setShaderFeature("COLOR_REPLACE", true)
        m:setShaderTexture("u_replaceMaskTexture", "textures/character_mask.png")
        m:setShaderParam("replaceColorR", Vector4(0.6, 0.15, 0.9, 1.0))  -- body: purple
        m:setShaderParam("replaceColorG", Vector4(0.1, 0.5, 1.0, 1.0))   -- hat: blue
        m:setShaderParam("replaceColorB", Vector4(1.0, 0.45, 0.0, 1.0))  -- irises: orange
    end)
    sprite(ctx, "textures/character.png", function(m)
        m:setShaderFeature("OVERLAY", true)
        m:setShaderTexture("u_overlayTexture", "textures/stripes.png")
        m:setShaderParam("overlayTiling", Vector4(2, 2, 0, 0))
    end)
    -- a dim sprite whose alpha is a light: the core burns to the HDR colour
    sprite(ctx, "textures/soft.png", function(m)
        m:setShaderFeature("GLOW", true)
        m:setShaderParam("glowColor", Vector4(3.0, 1.0, 0.15, 1.0))
        m:setShaderParam("glowPower", 3.0)
        m.blendMode = MaterialBlendMode.ADDITIVE
    end):setColor(0.15, 0.05, 0.02, 1.0)

    -- row 2: atlas frames with OUTLINE
    local outline = function(width)
        return function(m)
            m:setShaderFeature("OUTLINE", true)
            m:setShaderParam("outlineColor", Vector4(1.0, 0.1, 0.8, 1.0))
            m:setShaderParam("outlineWidth", width)
        end
    end
    local animated = sprite(ctx, "textures/atlas.png", outline(2.0))
    for _, frame in ipairs(FRAMES) do
        animated:addFrame(frame)
    end
    animated:startAnimation(0, 3, 400, true)

    -- the frame with neighbours on two sides and the atlas wrap on the others
    local still = sprite(ctx, "textures/atlas.png", outline(3.0))
    still.textureRect = FRAMES[4]

    -- instanced: each instance a frame (the instance rect inside the sprite's rect)
    local instanced = Sprite(ctx.scene)
    instanced:setTexture("textures/atlas.png")
    instanced:setSize(TEXELS, TEXELS)
    instanced:setScale(SIZE * 0.5 / TEXELS)
    instanced.pivotPreset = PivotPreset.CENTER
    instanced.castShadows = false
    instanced.receiveLights = false
    put(ctx, instanced, 1.0)
    instanced:createInstancedMesh()
    local tints = { Vector4(1, 1, 1, 1), Vector4(1, 0.8, 0.8, 1), Vector4(0.8, 1, 0.8, 1), Vector4(0.8, 0.8, 1, 1) }
    for i, frame in ipairs(FRAMES) do
        local offset = Vector3(((i - 1) % 2 - 0.5) * TEXELS, (0.5 - math.floor((i - 1) / 2)) * TEXELS, 0)
        instanced:addInstance(offset, Quaternion(), Vector3(1, 1, 1), tints[i], frame)
    end
    ctx:setShader(instanced, SPRITE, outline(2.0))

    -- 2D lights (built alone): lit, with a 2D shadow from the small caster sprite
    ctx.lit = sprite(ctx, "textures/character.png")
    ctx.lit.receiveLights = true

    -- straight and premultiplied soft sprites: the same look
    sprite(ctx, "textures/soft.png"):setColor(0.3, 0.6, 1.0, 1.0)
    sprite(ctx, "textures/soft_premultiplied.png", function(m)
        m:setShaderFeature("ALPHA_PREMULTIPLIED", true)
        m.blendMode = MaterialBlendMode.PREMULTIPLIED
    end):setColor(0.3, 0.6, 1.0, 1.0)

    -- hue cycling
    sprite(ctx, "textures/character.png", function(m)
        m:setShaderFeature("HUE_SHIFT", true)
        m:setShaderParam("hueShift", 0.0)
        m:setShaderParam("hueSpeed", 90.0)
    end)

    -- everything at once
    sprite(ctx, "textures/character.png", function(m)
        for _, feature in ipairs({ "OUTLINE", "HUE_SHIFT", "FLASH", "GRAYSCALE", "COLOR_REPLACE", "OVERLAY", "GLOW" }) do
            m:setShaderFeature(feature, true)
        end
        m:setShaderTexture("u_replaceMaskTexture", "textures/character_mask.png")
        m:setShaderParam("grayscaleAmount", 0.3)
        m:setShaderParam("flashAmount", 0.15)
        m:setShaderParam("overlayAmount", 0.4)
        m:setShaderParam("glowColor", Vector4(0.15, 0.1, 0.0, 1.0))
        m:setShaderParam("outlineColor", Vector4(1.0, 1.0, 1.0, 1.0))
    end)
end

-- 2D lights reach every mesh of the scene, so only built alone
local function buildLights2D(ctx)
    local scene = ctx.scene
    scene.ambientLight2DIntensity = 0.25
    local position = ctx.lit.position

    local warm = Light2D(scene)
    warm.position = Vector3(position.x - 0.7, position.y + 0.6, position.z + 0.2)
    warm:setColor(1.0, 0.75, 0.35)
    warm.intensity = 2.5
    warm.range = 2.2
    warm.shadows = true

    local cool = Light2D(scene)
    cool.position = Vector3(position.x + 0.8, position.y - 0.5, position.z + 0.2)
    cool:setColor(0.35, 0.6, 1.0)
    cool.intensity = 1.5
    cool.range = 1.6

    -- a small square between the warm light and the sprite casts a 2D shadow across it
    local caster = Sprite(scene)
    caster:setTexture("textures/checker.png")
    caster:setSize(TEXELS, TEXELS)
    caster:setScale(0.18 / TEXELS)
    caster.pivotPreset = PivotPreset.CENTER
    caster.castShadows = false
    caster.position = Vector3(position.x - 0.42, position.y + 0.36, position.z + 0.1)
    caster:getShadowCaster2D()

    ctx.lights2D = { warm, cool, caster }
end

-- six particles from the sprite sheet; alphas: their alpha (life), default 1
local function points(ctx, configure, textured, alphas)
    local p = Points(ctx.scene)
    if textured ~= false then
        p:setTexture("textures/particles.png")
    end
    local colours = {
        Vector4(1.0, 0.55, 0.15, 1), Vector4(1.0, 0.85, 0.2, 1), Vector4(0.95, 0.3, 0.2, 1),
        Vector4(0.3, 0.7, 1.0, 1), Vector4(0.6, 0.35, 1.0, 1), Vector4(0.3, 0.95, 0.5, 1),
    }
    local offsets = {
        Vector3(-0.42, 0.3, 0), Vector3(0, 0.45, 0.02), Vector3(0.42, 0.3, 0.04),
        Vector3(-0.3, -0.25, 0.06), Vector3(0.3, -0.25, 0.08), Vector3(0, 0, 0.1),
    }
    for i = 1, 6 do
        local colour = colours[i]
        if alphas then
            colour = Vector4(colour.x, colour.y, colour.z, alphas[i])
        end
        p:addPoint(offsets[i], colour, 0.75, (i - 1) * 0.4, FRAMES[(i - 1) % 4 + 1])
    end
    put(ctx, p, 1.0)
    p.customShader = PARTICLE
    if configure then
        configure(p)
    end
    return p
end

local function buildParticles(ctx)
    local first = points(ctx) -- alpha
    points(ctx, function(p)
        p.blendMode = MaterialBlendMode.ADDITIVE
    end)
    points(ctx, function(p)
        p.blendMode = MaterialBlendMode.PREMULTIPLIED
        p:setShaderFeature("OUTPUT_PREMULTIPLY", true)
    end)
    points(ctx, function(p)
        p.blendMode = MaterialBlendMode.MULTIPLY
        p:setShaderFeature("OUTPUT_MULTIPLY", true)
    end)
    points(ctx, function(p)
        p.blendMode = MaterialBlendMode.ADDITIVE
        p:setShaderFeature("GLOW", true)
    end)
    -- the particle's alpha is its life: whole at the top left, almost gone at the bottom right
    points(ctx, function(p)
        p:setShaderFeature("EROSION", true)
        p:setShaderTexture("u_erosionTexture", "textures/noise.png")
        p:setShaderParam("erosionTiling", 0.6)
    end, true, { 1.0, 0.8, 0.6, 0.45, 0.3, 0.15 })
    points(ctx, nil, false) -- untextured

    -- a checker backdrop behind the row, so the blend modes show against dark and light
    local backdrop = Primitive(ctx.scene)
    backdrop:createPlane(15.6, 1.7)
    backdrop:setRotation(90, 0, 0)
    local p = first.position
    backdrop.position = Vector3(0, p.y, p.z - 0.6)
    backdrop.castShadows = false
    ctx:setShader(backdrop, "library:unlit", function(m)
        m:setShaderTexture("u_baseTexture", "textures/checker.png")
        m:setShaderParam("tiling", Vector4(18, 2, 0, 0))
        m:setShaderParam("baseColor", Vector4(0.45, 0.45, 0.5, 1.0))
        m.cullMode = MaterialCullMode.OFF
    end)
    ctx.backdrop = backdrop
end

local function buildUI(ctx)
    -- a layer of its own, drawn over the 3D scene by its default UI camera
    local ui = Scene()
    Engine.addSceneLayer(ui)
    ctx.ui = ui
    ctx.uiObjects = {}

    local features = { false, "GRAYSCALE", "HUE_SHIFT", "TINT_OVERLAY", "SHINE" }
    local labels = { "Plain", "Gray", "Hue", "Tint", "Shine" }
    local configure = function(object, feature)
        object.customShader = UI_FX
        if feature then
            object:setShaderFeature(feature, true)
        end
        if feature == "SHINE" then
            -- slow, so the band is on the element most of the time
            object:setShaderParam("shineSpeed", 60.0)
            object:setShaderParam("shineInterval", 3.0)
            object:setShaderParam("shineWidth", 60.0)
        end
    end
    for i, feature in ipairs(features) do
        local image = Image(ui)
        image:setTexture("textures/button.png")
        image:setSize(160, 56)
        image.position = Vector3(20 + (i - 1) * 176, 16, 0)
        configure(image, feature)
        table.insert(ctx.uiObjects, image)

        local text = Text(ui)
        text.text = labels[i]
        text.fontSize = 40
        text:setColor(1.0, 0.8, 0.25, 1.0)
        text.position = Vector3(28 + (i - 1) * 176, 80, 0)
        configure(text, feature)
        table.insert(ctx.uiObjects, text)
    end
end

function TwoD.build(ctx)
    if ctx.solo then
        -- the wall: three rows of eight cells from y 1 to 4.8, the UI above
        ctx.camera.position = Vector3(0, 3.9, ctx.originZ + 10.8)
        ctx.camera.target = Vector3(0, 3.9, ctx.originZ)
    end
    buildSprites(ctx)
    buildParticles(ctx)
    buildUI(ctx)
    if ctx.solo then
        buildLights2D(ctx)
    end
end

return TwoD
