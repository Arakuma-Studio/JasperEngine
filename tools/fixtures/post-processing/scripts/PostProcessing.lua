-- Post-processing fixture (shader library phase 4): one test scene drawn with each effect of
-- PostProcessingSettings, picked by PP_CASE. The scene has what each effect needs to show:
--
--   a gradient sky (banding: dither), a checker floor running away from the camera and thin
--   poles (fine detail and edges: FXAA, sharpen, edge detect), a white wall under a strong
--   point light (values far above 1: exposure and the tone mapping curves), saturated boxes
--   and spheres (grade, saturation, monochrome), a dark corner (lift), a blended pane (blends
--   after the curve), points and a line renderer (the in-shader colour on every shader type),
--   additive glows (built-in points, mesh and lines) and a multiply quad on a black backdrop,
--   a white unlit cube,
--   a render-to-texture camera on a screen at the top right (its image takes no
--   post-processing of its own: the screen in the main camera's image is graded once), and a
--   HUD in a layer scene (white text over a grey bar: never post-processed, native resolution).
--
--   tools/smoke-test.sh tools/fixtures/post-processing
--   PP_CASE=off (default)  nothing: what the engine drew before phase 4
--   T0, in the shaders (no pass): exposure, aces, neutral, reinhard, agx, grade, lut (the
--     cross-process strip textures/lut_cross.png), mono, sepia, vignette, dither, tail (aces,
--     grade, vignette and dither together)
--   T1, the uber pass: fxaa, ca, pixelate, crt, edge, grain, sharpen, uber_grade (the grade,
--     vignette and dither moved into the uber pass by FXAA), mobile (aces, grade, vignette,
--     dither, FXAA, chromatic aberration, grain), scale (render scale 0.5 with FXAA),
--     fixed (fixed resolution 640x360 with CRT: the uber pass is its blit), uber_vignette (the
--     vignette case's vignette drawn by the uber pass, with FXAA: the same image), t1 (FXAA,
--     chromatic aberration, grain: the pass budget of T1), t1_fixed (the same at a fixed
--     resolution: no extra pass)
--   T2, the bloom and blur chain (quarter resolution and below, composited by the uber pass):
--     bloom (the lamp and the lit wall), bloom_tint (orange, low threshold, tight scatter),
--     bloom_wide (scatter 1), blur (the whole screen), blur_half (radius 1, half mixed in),
--     frosted (blur intensity 0: only the blurred texture, which the HUD's frosted panel shows;
--     in every other case the panel shows its tint), mobile_full (the section 1 budget set:
--     aces, grade, vignette, dither, FXAA, chromatic aberration, bloom, grain)
--   MSAA (the scene camera's multisampled target): msaa2, msaa4 (no other effect: resolved into
--     a target blitted to the swapchain), msaa4_bloom (resolved into the uber pass's source),
--     msaa4_fixed (resolved into the fixed-resolution target, 640x360)
--   T3, user post-process passes (scene.postProcessPasses): lib_crt, lib_pixelate (pass scale
--     0.5), lib_posterize, lib_edge_neon, lib_gameboy, lib_blur (the library post shaders),
--     user_pass (library pixelate at scale 0.5, then the project's shaders/pp_overlay: its own
--     texture and the original scene left of the split), user_pass_bloom (the same with bloom:
--     the chain runs before the uber pass)
--   review fixes: render_scale (render scale 0.5 alone: the plain upscale pass), console (the
--     editor's Console preset), console_still (the same without its per-frame grain) and ca_fxaa
--     (FXAA under a strong chromatic aberration: probe a pole's edge, every channel smoothed),
--     blend and blend_uber (a lifted, contrasted grade under a purple vignette, in the shaders
--     and in the uber pass: the additive glows on the black backdrop show no square, the
--     multiply quad stays a plain factor), white16 and white32 (a grade with tone mapping NONE
--     at both LUT sides: the white cube is 255), fixed_nearest (FXAA and sharpen at a NEAREST
--     fixed resolution: per fixed-resolution pixel, upscaled nearest)
--   S13 post FX: vhs (the VHS tape), film (Lens grain, film damage, the Academy gate mask),
--     filmic (the Filmic grain), halation (its own two passes), halation_bloom (on bloom's
--     levels), cube_display (luts/look_display.cube, a display-referred .cube), cube_log
--     (luts/look_acescct.cube, an ACEScct .cube applied before the tone mapping, with ACES),
--     adaptive (dynamic resolution on: the uber pass at any scale), s13_script (from frame 30
--     the script turns VHS and the Lens grain on and logs scene:getDynamicResolution())
--   PP_SCENE=saved  Saved.scene: the same objects (this script) with post-processing saved in
--                   the scene file (the "mobile" set without grain): YAML, export code, runtime
--   PP_CYCLE=1      every case in turn, one per PP_CYCLE_SECONDS (4): the variant bit and the
--                   uber pass switching at runtime (frame stats: no per-object variant churn);
--                   PP_CYCLE_CASES="off,bloom,..." cycles those only
--
-- The log says which case runs ("[PP] case ..."); JASPERENGINE_FRAME_STATS=1 adds the pass list.
local PostProcessing = { properties = {} }

local function log(fmt, ...)
    print("[PP] " .. string.format(fmt, ...))
end

local function fail(fmt, ...)
    print("[ERROR] [PP] " .. string.format(fmt, ...))
end

local function configure(mesh, apply)
    for i = 0, mesh:getNumSubmeshes() - 1 do
        local material = mesh:getMaterial(i)
        apply(material)
        mesh:setMaterial(i, material)
    end
end

-- the cases: a function editing a PostProcessingSettings copy, and the user post-process
-- passes of the T3 cases (a function returning them)
local CASES = {}
local ORDER = {}
local PASSES = {}
local function case(name, apply, passes)
    CASES[name] = apply
    PASSES[name] = passes
    table.insert(ORDER, name)
end

local function libraryPass(name, scale, params)
    local pass = PostProcessPass()
    pass.shader = "library:" .. name
    pass.scale = scale or 1.0
    for member, value in pairs(params or {}) do
        pass:setUniform(member, value)
    end
    return pass
end

local function userPasses()
    local overlay = PostProcessPass()
    overlay.shader = "shaders/pp_overlay"
    overlay:setTexture("u_overlayTexture", "textures/checker.png")
    overlay:setUniform("split", 0.45)
    return { libraryPass("pixelate", 0.5, { pixelSize = 4.0 }), overlay }
end

local function grade(p)
    p.colorGradingEnabled = true
    p.temperature = 0.35
    p.tint = -0.1
    p.contrast = 1.2
    p.saturation = 1.35
    p.lift = Vector3(0.03, 0.0, 0.05)
    p.gamma = Vector3(1.0, 1.05, 1.0)
    p.gain = Vector3(1.05, 1.0, 0.95)
end

local function vignette(p)
    p.vignetteEnabled = true
    p.vignetteIntensity = 0.6
    p.vignetteSmoothness = 0.45
    p.vignetteRoundness = 1.0
end

case("off", function(p) end)
case("exposure", function(p) p.exposure = 1.0 end)
case("aces", function(p) p.tonemap = TonemapMode.ACES end)
case("neutral", function(p) p.tonemap = TonemapMode.NEUTRAL end)
case("reinhard", function(p) p.tonemap = TonemapMode.REINHARD end)
case("agx", function(p) p.tonemap = TonemapMode.AGX end)
case("grade", grade)
case("lut", function(p)
    p.colorGradingEnabled = true
    p.lutTexture = "textures/lut_cross.png"
    p.lutContribution = 1.0
end)
case("mono", function(p) p.monochromeEnabled = true end)
case("sepia", function(p)
    p.monochromeEnabled = true
    p.monochromeMode = MonochromeMode.SEPIA
end)
case("vignette", vignette)
case("dither", function(p) p.ditherEnabled = true end)
case("tail", function(p)
    p.tonemap = TonemapMode.ACES
    grade(p)
    vignette(p)
    p.ditherEnabled = true
end)
case("fxaa", function(p) p.fxaaEnabled = true end)
case("ca", function(p)
    p.chromaticAberrationEnabled = true
    p.chromaticAberrationIntensity = 1.0
end)
case("pixelate", function(p)
    p.pixelateEnabled = true
    p.pixelateSize = 6
end)
case("crt", function(p)
    p.crtEnabled = true
    p.crtScanlines = 0.6
    p.crtMask = 0.4
    p.crtCurvature = 0.4
    p.crtVignette = 0.5
end)
case("edge", function(p)
    p.edgeDetectEnabled = true
    p.edgeDetectThreshold = 0.08
    p.edgeDetectIntensity = 1.0
    p.edgeDetectColor = Vector3(0.05, 0.02, 0.1)
end)
case("grain", function(p)
    p.filmGrainEnabled = true
    p.filmGrainIntensity = 0.6
end)
case("sharpen", function(p)
    p.sharpenEnabled = true
    p.sharpenIntensity = 1.0
end)
case("uber_grade", function(p)
    grade(p)
    vignette(p)
    p.ditherEnabled = true
    p.fxaaEnabled = true
end)
case("mobile", function(p)
    p.tonemap = TonemapMode.ACES
    grade(p)
    vignette(p)
    p.ditherEnabled = true
    p.fxaaEnabled = true
    p.chromaticAberrationEnabled = true
    p.chromaticAberrationIntensity = 0.3
    p.filmGrainEnabled = true
    p.filmGrainIntensity = 0.15
end)
case("scale", function(p)
    p.renderScale = 0.5
    p.fxaaEnabled = true
end)
case("fixed", function(p)
    p.crtEnabled = true
end)
case("uber_vignette", function(p)
    vignette(p)
    p.fxaaEnabled = true
end)
-- the pass budget of each tier (JASPERENGINE_FRAME_STATS=1): T1 alone, and as the blit of a fixed resolution
case("t1", function(p)
    p.fxaaEnabled = true
    p.chromaticAberrationEnabled = true
    p.chromaticAberrationIntensity = 0.3
    p.filmGrainEnabled = true
    p.filmGrainIntensity = 0.15
end)
case("t1_fixed", function(p)
    CASES.t1(p)
end)
-- T2: bloom and blur
case("bloom", function(p)
    p.bloomEnabled = true
    p.bloomThreshold = 0.75
    p.bloomIntensity = 1.5
end)
case("bloom_tint", function(p)
    p.bloomEnabled = true
    p.bloomThreshold = 0.5
    p.bloomKnee = 0.8
    p.bloomIntensity = 2.0
    p.bloomTint = Vector3(1.0, 0.55, 0.25)
    p.bloomScatter = 0.35
end)
case("bloom_wide", function(p)
    p.bloomEnabled = true
    p.bloomThreshold = 0.6
    p.bloomIntensity = 1.2
    p.bloomScatter = 1.0
end)
case("blur", function(p)
    p.blurEnabled = true
    p.blurRadius = 2.5
end)
case("blur_half", function(p)
    p.blurEnabled = true
    p.blurRadius = 1.0
    p.blurIntensity = 0.5
end)
case("frosted", function(p)
    p.blurEnabled = true
    p.blurRadius = 3.0
    p.blurIntensity = 0.0
end)
-- the pass budget of section 1 of the phase 4 spec (tonemap, grade, vignette, chromatic
-- aberration, bloom, grain, FXAA; dither too)
case("mobile_full", function(p)
    CASES.mobile(p)
    p.bloomEnabled = true
    p.bloomThreshold = 0.75
end)
-- review fixes: render scale alone (a plain upscale pass), the Console preset (and without
-- its per-frame grain, for pixel probes of FXAA under chromatic aberration), blends over black
-- under a lifted, contrasted grade with a coloured vignette (T0, and the same in the uber pass),
-- whites at both LUT sizes with tone mapping NONE, FXAA and sharpen on a NEAREST fixed resolution
case("render_scale", function(p) p.renderScale = 0.5 end)
local function console(p)
    p.tonemap = TonemapMode.ACES
    p.colorGradingEnabled = true
    p.temperature = 0.05
    p.contrast = 1.1
    p.saturation = 1.05
    p.vignetteEnabled = true
    p.vignetteIntensity = 0.3
    p.ditherEnabled = true
    p.fxaaEnabled = true
    p.bloomEnabled = true
    p.chromaticAberrationEnabled = true
    p.chromaticAberrationIntensity = 0.15
    p.filmGrainEnabled = true
    p.filmGrainIntensity = 0.12
end
case("console", console)
case("console_still", function(p)
    console(p)
    p.filmGrainEnabled = false
end)
case("ca_fxaa", function(p)
    p.fxaaEnabled = true
    p.chromaticAberrationEnabled = true
    p.chromaticAberrationIntensity = 1.0
end)
local function lifted(p)
    p.colorGradingEnabled = true
    p.lift = Vector3(0.15, 0.12, 0.18)
    p.contrast = 1.3
    p.vignetteEnabled = true
    p.vignetteIntensity = 0.8
    p.vignetteColor = Vector3(0.35, 0.1, 0.45)
end
case("blend", lifted)
case("blend_uber", function(p)
    lifted(p)
    p.fxaaEnabled = true
end)
local function whites(p, side)
    p.colorGradingEnabled = true
    p.saturation = 1.2
    p.lutSize = side
end
case("white16", function(p) whites(p, 16) end)
case("white32", function(p) whites(p, 32) end)
case("fixed_nearest", function(p)
    p.fxaaEnabled = true
    p.sharpenEnabled = true
    p.sharpenIntensity = 0.6
end)

-- MSAA
case("msaa2", function(p) p.msaaSamples = 2 end)
case("msaa4", function(p) p.msaaSamples = 4 end)
case("msaa4_bloom", function(p)
    p.msaaSamples = 4
    CASES.bloom(p)
end)
case("msaa4_fixed", function(p) p.msaaSamples = 4 end)
-- T3: user post-process passes
case("lib_crt", function(p) end, function() return { libraryPass("crt") } end)
case("lib_pixelate", function(p) end, function() return { libraryPass("pixelate", 0.5, { pixelSize = 6.0, roundCorners = 0.6 }) } end)
case("lib_posterize", function(p) end, function() return { libraryPass("posterize") } end)
case("lib_edge_neon", function(p) end, function() return { libraryPass("edge_neon", 1.0, { hueSpeed = 0.1 }) } end)
case("lib_gameboy", function(p) end, function() return { libraryPass("gameboy") } end)
case("lib_blur", function(p) end, function() return { libraryPass("blur", 1.0, { radius = 4.0 }) } end)
case("user_pass", function(p) end, userPasses)
case("user_pass_bloom", function(p) CASES.bloom(p) end, userPasses)

-- S13: VHS, film simulation, halation, .cube LUTs, dynamic resolution
case("vhs", function(p) p.vhsEnabled = true end)
case("film", function(p)
    p.filmGrainEnabled = true
    p.filmGrainType = FilmGrainType.LENS
    p.filmGrainIntensity = 0.35
    p.filmDamageEnabled = true
    p.filmDustAmount = 6.0
    p.filmScratchAmount = 2.0
    p.filmHairAmount = 1.0
    p.filmGateMask = FilmGateMask.ACADEMY_137
end)
case("filmic", function(p)
    p.filmGrainEnabled = true
    p.filmGrainType = FilmGrainType.FILMIC
    p.filmGrainIntensity = 0.5
    p.filmGrainColored = true
end)
case("halation", function(p)
    p.halationEnabled = true
    p.halationIntensity = 1.5
end)
case("halation_bloom", function(p)
    CASES.bloom(p)
    p.halationEnabled = true
    p.halationIntensity = 1.5
end)
case("cube_display", function(p)
    p.colorGradingEnabled = true
    p.lutTexture = "luts/look_display.cube"
end)
case("cube_log", function(p)
    p.tonemap = TonemapMode.ACES
    p.colorGradingEnabled = true
    p.lutTexture = "luts/look_acescct.cube"
    p.lutInputEncoding = LutInputEncoding.AUTO
end)
case("adaptive", function(p)
    p.dynamicResolutionEnabled = true
    p.dynamicResolutionMinScale = 0.6
end)
case("s13_script", function(p) p.dynamicResolutionEnabled = true end)

local function buildScene(self, scene, keep)
    -- the sky: a smooth gradient (8-bit banding shows in it without dither)
    local sky = keep(Skybox(scene))
    sky.customShader = "library:gradient_sky"
    sky:setShaderParam("topColor", Vector4(0.05, 0.12, 0.35, 1.0))
    sky:setShaderParam("horizonColor", Vector4(0.55, 0.6, 0.7, 1.0))
    sky:setShaderParam("bottomColor", Vector4(0.1, 0.09, 0.08, 1.0))

    local sun = keep(Light(scene))
    sun.type = LightType.DIRECTIONAL
    sun:setDirection(-0.35, -1.0, -0.5)
    sun.intensity = 1.6
    sun.shadows = true

    -- the lamp itself: a small emissive bulb (bloom)
    local bulb = keep(Primitive(scene))
    bulb:createSphere(0.22)
    bulb.position = Vector3(-4.5, 1.6, -2.2)
    bulb.castShadows = false
    bulb:setColor(1.0, 0.9, 0.7)
    configure(bulb, function(m)
        m.emissiveFactor = Vector3(3.0, 2.6, 1.8)
    end)

    -- a strong light close to a white wall: far above 1 in the middle of the wall
    local lamp = keep(Light(scene))
    lamp.type = LightType.POINT
    lamp.position = Vector3(-4.5, 1.6, -2.4)
    lamp:setColor(1.0, 0.85, 0.6)
    lamp.intensity = 60
    lamp.range = 8

    local wall = keep(Primitive(scene))
    wall:createBox(6.0, 3.6, 0.3)
    wall.position = Vector3(-4.5, 1.8, -3.6)
    wall:setColor(0.95, 0.95, 0.95)

    -- the floor: a checker running away from the camera (fine detail, aliasing)
    local floor = keep(Primitive(scene))
    floor:createPlane(30, 40)
    floor.position = Vector3(0, 0, -8)
    configure(floor, function(m)
        m.shader = "library:simple_lit"
        m:setShaderTexture("u_baseTexture", "textures/checker.png")
        m:setShaderParam("tiling", Vector4(30, 40, 0, 0))
    end)

    -- saturated shapes
    local colors = { {0.9, 0.15, 0.1}, {0.95, 0.75, 0.1}, {0.15, 0.75, 0.25}, {0.15, 0.4, 0.95}, {0.7, 0.2, 0.85} }
    for i, c in ipairs(colors) do
        local shape = keep(Primitive(scene))
        if i % 2 == 1 then
            shape:createBox(1.1, 1.1, 1.1)
            shape:setRotation(0, i * 17, 0)
        else
            shape:createSphere(0.6)
        end
        shape.position = Vector3(-1.2 + (i - 1) * 1.5, 0.6, 0.2)
        shape:setColor(c[1], c[2], c[3])
    end

    -- thin poles against the sky: edges and stair-stepping
    for i = 0, 6 do
        local pole = keep(Primitive(scene))
        pole:createBox(0.06, 4.0, 0.06)
        pole.position = Vector3(2.4 + i * 0.7, 2.0, -3.0)
        pole:setRotation(0, 0, 8 + i * 6)
        pole:setColor(0.12, 0.12, 0.14)
    end

    -- the dark corner: a dark box in its own shadow
    local corner = keep(Primitive(scene))
    corner:createBox(2.0, 1.4, 2.0)
    corner.position = Vector3(7.2, 0.7, -1.0)
    corner:setColor(0.08, 0.08, 0.1)

    -- a blended pane (the transparent queue blends after the curve)
    local pane = keep(Primitive(scene))
    pane:createBox(2.2, 1.6, 0.05)
    pane.position = Vector3(0.6, 1.4, 2.4)
    pane.castShadows = false
    configure(pane, function(m)
        m.shader = "library:unlit"
        m:setShaderParam("baseColor", Vector4(0.3, 0.8, 1.0, 0.4))
        m.blendMode = MaterialBlendMode.ALPHA
    end)

    -- points and a line renderer: the colour on those shader types too
    local points = keep(Points(scene))
    points.position = Vector3(-3.6, 0.3, 4.2)
    for i = 0, 15 do
        points:addPoint(Vector3((i % 4) * 0.5, math.floor(i / 4) * 0.5, 0.0), Vector4(1.0, 0.4 + (i % 3) * 0.2, 0.1, 1.0), 0.25, 0.0)
    end
    local lines = keep(LineRenderer(scene))
    lines.position = Vector3(-3.8, 1.6, 4.2)
    for i = 0, 10 do
        lines:addLine(Vector3(i * 0.25, 0.0, 0.0), Vector3(i * 0.25 + 0.4, 1.0, 0.0), Vector4(0.2, 1.0, 0.6, 1.0))
    end

    -- blends over black (review fix): a black backdrop with additive built-in points, an
    -- additive built-in mesh and additive lines (their black adds nothing: no lifted square
    -- around a glow), and a multiply quad (a plain factor, not graded)
    local backdrop = keep(Primitive(scene))
    backdrop:createBox(2.6, 1.7, 0.05)
    backdrop.position = Vector3(3.6, 1.3, 3.4)
    backdrop.receiveLights = false
    backdrop.castShadows = false
    backdrop:setColor(0.0, 0.0, 0.0)
    local glowPoints = keep(Points(scene))
    glowPoints.position = Vector3(2.7, 1.7, 3.5)
    glowPoints:setTexture("textures/glow.png")
    glowPoints.blendMode = MaterialBlendMode.ADDITIVE
    for i = 0, 3 do
        glowPoints:addPoint(Vector3(i * 0.45, 0.0, 0.0), Vector4(1.0, 0.5 + i * 0.15, 0.2, 1.0), 0.6, 0.0)
    end
    local glowQuad = keep(Primitive(scene))
    glowQuad:createBox(0.8, 0.8, 0.01)
    glowQuad.position = Vector3(3.0, 0.95, 3.5)
    glowQuad.receiveLights = false
    glowQuad.castShadows = false
    glowQuad:setTexture("textures/glow.png")
    glowQuad:setColor(0.3, 0.7, 1.0)
    configure(glowQuad, function(m) m.blendMode = MaterialBlendMode.ADDITIVE end)
    local glowLines = keep(LineRenderer(scene))
    glowLines.position = Vector3(3.6, 0.6, 3.5)
    glowLines.blendMode = MaterialBlendMode.ADDITIVE
    for i = 0, 4 do
        glowLines:addLine(Vector3(i * 0.15, 0.0, 0.0), Vector3(i * 0.15, 0.6, 0.0), Vector4(0.02 + i * 0.1, 0.02, 0.02, 1.0))
    end
    local filter = keep(Primitive(scene))
    filter:createBox(0.7, 0.7, 0.01)
    filter.position = Vector3(1.75, 0.75, 3.6)
    filter.receiveLights = false
    filter.castShadows = false
    filter:setColor(0.5, 0.5, 0.5)
    configure(filter, function(m) m.blendMode = MaterialBlendMode.MULTIPLY end)

    -- a white unlit cube (review fix: whites reach 255 with tone mapping NONE at every LUT size)
    local white = keep(Primitive(scene))
    white:createBox(0.5, 0.5, 0.5)
    white.position = Vector3(1.6, 2.7, 0.5)
    white.receiveLights = false
    white.castShadows = false
    white:setColor(1.0, 1.0, 1.0)

    -- a render-to-texture camera on a screen at the top right
    local rtt = keep(Camera(scene))
    rtt.type = CameraType.CAMERA_PERSPECTIVE
    rtt.position = Vector3(6.0, 3.0, 6.0)
    rtt.target = Vector3(0.0, 0.8, -1.0)
    rtt.renderToTexture = true
    rtt:setFramebufferSize(480, 270)
    local screen = keep(Primitive(scene))
    screen:createBox(4.0, 2.25, 0.05)
    screen.position = Vector3(5.2, 3.9, -3.0)
    screen.receiveLights = false
    screen.castShadows = false
    screen:setTexture(rtt:getFramebuffer())

    -- the HUD: a layer scene, drawn after the main camera's post-processing
    local hud = Scene()
    Engine.addSceneLayer(hud)
    self.hud = hud
    local bar = Polygon(hud)
    bar:addVertex(0, 0)
    bar:addVertex(420, 0)
    bar:addVertex(0, 44)
    bar:addVertex(420, 44)
    bar:setColor(0.5, 0.5, 0.5, 1.0)
    bar.position = Vector3(16, 16, 0)
    self.hudBar = bar
    local text = Text(hud)
    text.text = "HUD: no post-processing"
    text.fontSize = 28
    text:setColor(1.0, 1.0, 1.0, 1.0)
    text.position = Vector3(26, 22, 0)
    self.hudText = text

    -- a frosted panel over the bright wall: the blurred scene behind it when the scene has
    -- blur on (PP_CASE=frosted, blur), its tint otherwise
    local panel = Polygon(hud)
    panel:addVertex(0, 0)
    panel:addVertex(360, 0)
    panel:addVertex(0, 220)
    panel:addVertex(360, 220)
    panel:setColor(1.0, 1.0, 1.0, 1.0)
    panel.position = Vector3(40, 300, 0)
    panel.customShader = "library:frosted_panel"
    self.hudPanel = panel
    local label = Text(hud)
    label.text = "frosted panel"
    label.fontSize = 22
    label:setColor(0.1, 0.1, 0.15, 1.0)
    label.position = Vector3(56, 312, 0)
end

local function applyCase(self, name)
    local apply = CASES[name]
    if not apply then
        fail("PP_CASE=%s: unknown case", name)
        return
    end
    local p = PostProcessingSettings()
    apply(p)
    self.scene.postProcessing = p
    self.scene.postProcessPasses = PASSES[name] and PASSES[name]() or {}
    if name == "fixed" or name == "t1_fixed" or name == "msaa4_fixed" or name == "fixed_nearest" then
        self.scene.fixedResolutionEnabled = true
        self.scene:setFixedResolutionSize(640, 360)
        -- (NEAREST, the default: the effects run per fixed-resolution pixel, upscaled nearest)
        self.scene.fixedResolutionFilter = TextureFilter.NEAREST
    else
        self.scene.fixedResolutionEnabled = false
    end
    self.current = name
    log("case %s", name)
end

function PostProcessing:init()
    RegisterEngineEvent(self, "onUpdate")
    local scene = self.scene
    self.objects = {}
    self.frames = 0
    self.time = 0
    local function keep(object)
        table.insert(self.objects, object)
        return object
    end

    local camera = keep(Camera(scene))
    camera.type = CameraType.CAMERA_PERSPECTIVE
    camera.position = Vector3(0.0, 3.2, 9.0)
    camera.target = Vector3(0.6, 1.2, -1.0)
    scene:setCamera(camera)
    self.camera = camera

    local ok, err = pcall(buildScene, self, scene, keep)
    if not ok then
        fail("scene failed to build: %s", tostring(err))
    end

    -- the saved scene keeps the post-processing its file holds
    if PP_SAVED_LOADED then
        local p = scene.postProcessing
        log("saved scene: tonemap %d, grading %s, fxaa %s, chromatic aberration %s", p.tonemap,
            tostring(p.colorGradingEnabled), tostring(p.fxaaEnabled), tostring(p.chromaticAberrationEnabled))
        return
    end
    if os.getenv("PP_SCENE") == "saved" then
        log("loading the saved scene")
        PP_SAVED_LOADED = true
        SceneManager.loadScene("Saved")
        return
    end

    self.cycle = os.getenv("PP_CYCLE") == "1"
    self.period = tonumber(os.getenv("PP_CYCLE_SECONDS") or "") or 4
    local only = os.getenv("PP_CYCLE_CASES")
    if only and only ~= "" then
        ORDER = {}
        for name in string.gmatch(only, "[^,]+") do
            table.insert(ORDER, name)
        end
    end
    if self.cycle then
        self.index = 1
        applyCase(self, ORDER[1])
    else
        applyCase(self, os.getenv("PP_CASE") or "off")
    end
end

function PostProcessing:onUpdate()
    self.frames = self.frames + 1
    -- S13: a script switching the new effects at runtime, and reading dynamic resolution
    if self.current == "s13_script" and self.frames == 30 then
        local p = self.scene.postProcessing
        p.vhsEnabled = true
        p.filmGrainEnabled = true
        p.filmGrainType = FilmGrainType.LENS
        self.scene.postProcessing = p
        local applied = self.scene.postProcessing
        local dynamic = self.scene:getDynamicResolution()
        log("s13 script: vhs %s, grain type %d, dynamic resolution enabled %s running %s scale %.2f source %s",
            tostring(applied.vhsEnabled), applied.filmGrainType, tostring(dynamic.enabled), tostring(dynamic.running),
            dynamic.scale, dynamic.source)
        if not applied.vhsEnabled or applied.filmGrainType ~= FilmGrainType.LENS or not dynamic.enabled or dynamic.scale <= 0 then
            fail("s13 script: the settings did not apply")
        end
    end
    if self.cycle then
        self.time = self.time + Engine.deltatime
        if self.time >= self.period then
            self.time = 0
            self.index = self.index % #ORDER + 1
            applyCase(self, ORDER[self.index])
        end
    end
end

return PostProcessing
