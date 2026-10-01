-- Builds every object at runtime, with shader variants the export cannot predict from
-- the scene: the game has to compile them (ShaderPool / RuntimeShaderCompiler).
--   tools/smoke-test.sh tools/fixtures/runtime-shaders
local RuntimeShaders = { properties = {} }

local function log(fmt, ...)
    print("[RTS] " .. string.format(fmt, ...))
end

local function fail(fmt, ...)
    -- the smoke test fails on an [ERROR] in the game log
    print("[ERROR] [RTS] " .. string.format(fmt, ...))
end

-- the variant the capsule shown after the warm-up draws with: lit, no shadows received,
-- normals, vertex colors (every primitive has them), fog
local CAPSULE_VARIANT = "mesh:Puc,Nor,Vc4,Fog"

function RuntimeShaders:init()
    RegisterEngineEvent(self, "onUpdate")
    local scene = self.scene

    self.time = 0
    self.frames = 0
    self.stage = 0

    log("runtime compilation supported=%s enabled=%s async=%s",
        tostring(ShaderPool.isRuntimeCompilationSupported()), tostring(ShaderPool.runtimeCompilation),
        tostring(ShaderPool.asyncCompilation))
    if not ShaderPool.isRuntimeCompilationSupported() then
        fail("this build has no runtime shader compiler")
    end

    local camera = Camera(scene)
    camera.type = CameraType.CAMERA_PERSPECTIVE
    camera.position = Vector3(0, 4.5, 11)
    camera.target = Vector3(0, 1, 0)
    scene:setCamera(camera)

    local light = Light(scene)
    light.type = LightType.DIRECTIONAL
    light:setDirection(-0.5, -1.0, -0.6)
    light.intensity = 3.0
    light.shadows = true

    -- precompiled (project.yaml shaderAdditions): lit, shadows, normals, vertex colors
    local ground = Primitive(scene)
    ground:createPlane(18, 18)
    ground:setColor(0.55, 0.55, 0.58)

    -- RGB vertex colors + a normal map without a base texture (and no tangents), from a
    -- model no scene of the project uses
    local bumpy = Model(scene)
    if not bumpy:loadModel("bumpy_box.gltf") then
        fail("bumpy_box.gltf did not load")
    end
    bumpy.position = Vector3(-4.5, 0.8, 0)

    -- unlit + vertex colors
    local unlit = Primitive(scene)
    unlit:createSphere(0.9)
    unlit.position = Vector3(-1.8, 0.9, 1.5)
    unlit:setColor(1.0, 0.35, 0.35)
    unlit.receiveLights = false

    -- instanced (and fog, once it comes in)
    local rings = Primitive(scene)
    rings:createTorus(0.45, 0.15)
    rings:setColor(0.95, 0.85, 0.2)
    rings:createInstancedMesh()
    for i = 0, 4 do
        rings:addInstance(Vector3(1.0 + i * 0.8, 0.5 + i * 0.45, -1.5 - i * 0.4))
    end

    -- skinned + morph targets, from a model no scene of the project uses
    local strip = Model(scene)
    if not strip:loadModel("skinned_morph.gltf") then
        fail("skinned_morph.gltf did not load")
    end
    strip.position = Vector3(4.0, 0, 0.5)
    strip:setMorphWeight(0, 0.8)

    -- a forked mesh shader only this script uses (its sources ship with the export)
    local forked = Primitive(scene)
    forked:createCylinder(0.6, 1.6)
    forked.position = Vector3(1.6, 0.8, 2.2)
    forked:setColor(0.9, 0.9, 0.9)
    forked.customShader = "assets/shaders/tinted"

    self.objects = { ground = ground, bumpy = bumpy, unlit = unlit, rings = rings, strip = strip, forked = forked }
end

function RuntimeShaders:onUpdate()
    local scene = self.scene
    self.frames = self.frames + 1
    self.time = self.time + Engine.deltatime
    if self.frames % 100 == 0 then
        log("frame %d (%.2f s): %.0f fps, %d variants compiling", self.frames, self.time, Engine.framerate,
            ShaderPool.getPendingShaderCount())
    end

    -- a property every mesh's variant depends on changes while playing
    if self.stage == 0 and self.time >= 1.0 then
        self.stage = 1
        local fog = Fog(scene)
        fog.type = FogType.LINEAR
        fog:setLinearStartEnd(6, 26)
        fog:setColor(0.16, 0.18, 0.24)
        log("frame %d: fog on, every mesh needs a fog variant now", self.frames)

        -- warm up what the next object draws with, before it exists
        self.collection = ShaderVariantCollection()
        if not self.collection:add(CAPSULE_VARIANT) then
            fail("collection rejected %s", CAPSULE_VARIANT)
        end
        -- and what an object on screen draws with (already requested by its own load)
        self.collection:addObject(self.objects.bumpy)
        log("frame %d: warming up %d variants", self.frames, self.collection.variantCount)

        -- a variant nothing draws with, by spec
        ShaderPool.warmUp("mesh:Ult,Vc3")
    end

    if self.stage == 1 then
        ShaderPool.warmUp("mesh:Ult,Vc3")
        if self.collection:warmUp() then
            self.stage = 2
            log("frame %d: warm-up done: %d ready, %d failed, %s ready=%s",
                self.frames, self.collection.warmedUpVariantCount, self.collection.failedVariantCount,
                CAPSULE_VARIANT, tostring(ShaderPool.isShaderReady(CAPSULE_VARIANT)))
            if self.collection.failedVariantCount > 0 then
                fail("%d warmed-up variants failed", self.collection.failedVariantCount)
            end

            -- its variant is ready: it shows up on its first frame
            local capsule = Primitive(scene)
            capsule:createCapsule(0.5, 1.0)
            capsule.position = Vector3(-0.2, 1.0, 3.2)
            capsule:setColor(0.6, 0.3, 0.95)
            capsule.receiveShadows = false
            capsule.castShadows = false
            self.objects.capsule = capsule
            self.capsuleFrame = self.frames
        end
    end

    -- once the meshes have switched to their fog variants too
    if self.stage == 2 and self.frames >= self.capsuleFrame + 5 and ShaderPool.getPendingShaderCount() == 0
            and ShaderPool.isShaderReady("mesh:Ult,Vc3") then
        self.stage = 3
        log("frame %d (%.2f s): every variant ready, nothing pending (capsule added at frame %s)",
            self.frames, self.time, tostring(self.capsuleFrame))
        if ShaderPool.isShaderFailed("mesh:Ult,Vc3") then
            fail("mesh:Ult,Vc3 failed")
        end
        -- everything the build ships and every variant this device compiled before
        local notReady = ShaderPool.warmUpAllShaders()
        log("warmUpAllShaders: %d not ready yet", notReady)
        self.stage = 4
        self.allFrame = self.frames
    end

    if self.stage == 4 and self.frames >= self.allFrame + 5 then
        self.stage = 5
        log("warmUpAllShaders after 5 frames: %d not ready, %d compiling",
            ShaderPool.warmUpAllShaders(), ShaderPool.getPendingShaderCount())
    end
end

return RuntimeShaders
