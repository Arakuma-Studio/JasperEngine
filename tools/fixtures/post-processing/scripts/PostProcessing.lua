-- Post-processing fixture (shader library phase 4): one test scene drawn with each effect of
-- PostProcessingSettings, picked by PP_CASE. The scene has what each effect needs to show:
--
--   a gradient sky (banding: dither), a checker floor running away from the camera and thin
--   poles (fine detail and edges: FXAA, sharpen, edge detect), a white wall under a strong
--   point light (values far above 1: exposure and the tone mapping curves), saturated boxes
--   and spheres (grade, saturation, monochrome), a dark corner (lift), a blended pane (blends
--   after the curve), points and a line renderer (the in-shader colour on every shader type),
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
--   PP_SCENE=saved  Saved.scene: the same objects (this script) with post-processing saved in
--                   the scene file (the "mobile" set without grain): YAML, export code, runtime
--   PP_CYCLE=1      every case in turn, one per PP_CYCLE_SECONDS (4): the variant bit and the
--                   uber pass switching at runtime (frame stats: no per-object variant churn)
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

-- the cases: a function editing a PostProcessingSettings copy
local CASES = {}
local ORDER = {}
local function case(name, apply)
    CASES[name] = apply
    table.insert(ORDER, name)
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
    if name == "fixed" or name == "t1_fixed" then
        self.scene.fixedResolutionEnabled = true
        self.scene:setFixedResolutionSize(640, 360)
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
    if self.cycle then
        self.index = 1
        applyCase(self, ORDER[1])
    else
        applyCase(self, os.getenv("PP_CASE") or "off")
    end
end

function PostProcessing:onUpdate()
    self.frames = self.frames + 1
    if self.cycle then
        self.time = self.time + Engine.getDeltatime()
        if self.time >= self.period then
            self.time = 0
            self.index = self.index % #ORDER + 1
            applyCase(self, ORDER[self.index])
        end
    end
end

return PostProcessing
