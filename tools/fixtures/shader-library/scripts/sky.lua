-- The sky library shaders: library:gradient_sky, procedural_sky (with CLOUDS) and
-- panoramic_sky, on the scene's one Skybox. SL_SKY picks the sky (procedural by default):
-- gradient, procedural, panoramic, or cycle (each in turn for SL_SKY_SECONDS, default 6,
-- counted from when no variant is compiling). Built alone (SL_GROUPS=sky) the camera looks
-- low across the ground towards the sun, the light is lowered so the sun is in view, and
-- the panorama's wrap (straight behind the default camera, +Z) is on screen; a pillar's
-- shadow points away from the sun.
--   SMOKE_GAME_ENV="SL_GROUPS=sky SL_SKY=panoramic" tools/smoke-test.sh tools/fixtures/shader-library
local Sky = {}

local SKIES = { "gradient", "procedural", "panoramic" }

local function setSky(ctx, name)
    local skybox = ctx.skybox
    -- horizonColor is both the gradient's and the procedural sky's: a dusk for the gradient,
    -- the procedural sky's default otherwise
    if name == "gradient" then
        skybox:setShaderParam("horizonColor", Vector4(1.0, 0.45, 0.2, 1.0))
    else
        skybox:removeShaderUniform("horizonColor")
    end
    skybox.customShader = "library:" .. name .. "_sky"
    ctx.current = name
    ctx:log("sky %s at %.1f s", name, ctx.time or 0)
end

function Sky.build(ctx)
    local scene = ctx.scene
    local skybox = Skybox(scene)
    ctx.skybox = skybox

    -- every sky's values up front: a shader only reads its own
    skybox:setShaderParam("topColor", Vector4(0.02, 0.05, 0.3, 1.0))
    skybox:setShaderParam("bottomColor", Vector4(0.06, 0.03, 0.08, 1.0))
    skybox:setShaderParam("topExponent", 0.45)
    skybox:setShaderParam("horizonOffset", 0.03)
    skybox:setShaderFeature("CLOUDS", true)
    skybox:setShaderTexture("u_cloudTexture", "textures/noise.png")
    skybox:setShaderParam("cloudCoverage", 0.45)
    skybox:setShaderTexture("u_panoramaTexture", "textures/panorama.png")

    local choice = os.getenv("SL_SKY") or "procedural"
    ctx.cycle = choice == "cycle"
    ctx.period = tonumber(os.getenv("SL_SKY_SECONDS") or "") or 6
    ctx.time = 0
    if ctx.cycle then
        ctx.index = 1
        setSky(ctx, SKIES[1])
    else
        local known = false
        for _, name in ipairs(SKIES) do
            known = known or name == choice
        end
        if not known then
            ctx:fail("SL_SKY=%s: use gradient, procedural, panoramic or cycle", choice)
            choice = "procedural"
        end
        setSky(ctx, choice)
    end

    if ctx.solo then
        -- a low sun (towards it: +X, up, +Z: 34 degrees from +Z, 27 up); the camera looks
        -- 25 degrees from +Z and 15 up, so the sun is just left of the centre (+X is on
        -- the left looking at +Z) and the panorama's wrap (+Z) right of it
        ctx.light:setDirection(-0.5, -0.45, -0.75)
        ctx.camera.position = Vector3(0, 1.6, -6)
        ctx.camera.target = Vector3(4.08, 4.19, 2.75)

        local pillar = Primitive(scene)
        pillar:createBox(0.5, 2.4, 0.5)
        pillar.position = Vector3(1.4, 1.2, 1.5)
        pillar:setColor(0.8, 0.75, 0.7)
        ctx.pillar = pillar
    end
end

function Sky.update(ctx, dt)
    ctx.time = ctx.time + dt
    if not ctx.cycle then
        return
    end
    -- hold each sky for the period once nothing is compiling (the next one compiles on
    -- its first frame)
    if ShaderPool.getPendingShaderCount() > 0 then
        ctx.held = 0
        return
    end
    ctx.held = (ctx.held or 0) + dt
    if ctx.held >= ctx.period then
        ctx.held = 0
        ctx.index = ctx.index % #SKIES + 1
        setSky(ctx, SKIES[ctx.index])
    end
end

return Sky
