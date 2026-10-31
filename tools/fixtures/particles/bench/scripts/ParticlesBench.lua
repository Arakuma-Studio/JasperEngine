-- Particle System benchmark (particles spec 8): four version 2 systems of 500 alive particles
-- each (2000 in all) with six modules on (colour, size and rotation over lifetime, velocity
-- over lifetime, limit velocity, noise), drawn as billboards in front of a perspective camera.
--
--   tools/smoke-test.sh tools/fixtures/particles/bench   (SMOKE_BACKEND=gles3 / vulkan too)
--   SMOKE_GAME_ENV="JASPERENGINE_FRAME_STATS=1"   the engine logs particles_sim_ms (the CPU time of
--                                                 the simulation that frame) with its frame stats
-- The script logs the alive count and the frame time every 100 frames.
local ParticlesBench = { properties = {} }

local SYSTEMS = 4
local RATE = 250        -- particles a second each
local LIFETIME = 2.0    -- seconds: RATE x LIFETIME alive each

local function log(fmt, ...)
    print("[PARTICLES] " .. string.format(fmt, ...))
end

local function makeSystem(scene, x)
    local ps = ParticleSystem(scene, true)
    GameObject(scene, ps.entity).position = Vector3(x, -1.5, 0)
    local main = ps.main
    main.maxParticles = 1000
    main.startLifetime = LIFETIME
    main.startSpeed = MinMaxCurve(1.5, 2.5)
    main.startSize = MinMaxCurve(0.15, 0.3)
    main.startColor = MinMaxGradient(Vector4(1, 0.6, 0.2, 1), Vector4(1, 0.9, 0.4, 1))
    ps.emission.rateOverTime = RATE
    ps.shape.shapeType = ParticleSystemShapeType.Cone
    ps.shape.angle = 0.35
    ps.shape.radius = 0.3

    ps.colorOverLifetime.enabled = true
    local fade = Gradient()
    fade:setKeys({ {0, Vector3(1, 1, 1)}, {1, Vector3(1, 0.3, 0.1)} }, { {0, 1}, {1, 0} })
    ps.colorOverLifetime.color = MinMaxGradient(fade)
    ps.sizeOverLifetime.enabled = true
    ps.sizeOverLifetime.size = MinMaxCurve(1.0, AnimationCurve.linear(0, 0.5, 1, 1.5))
    ps.rotationOverLifetime.enabled = true
    ps.rotationOverLifetime.z = MinMaxCurve(-1.5, 1.5)
    ps.velocityOverLifetime.enabled = true
    ps.velocityOverLifetime.x = MinMaxCurve(-0.3, 0.3)
    ps.limitVelocityOverLifetime.enabled = true
    ps.limitVelocityOverLifetime.limit = 2.0
    ps.limitVelocityOverLifetime.dampen = 0.2
    ps.noise.enabled = true
    ps.noise.strength = 0.4
    ps.noise.frequency = 0.8
    ps:play()
    return ps
end

function ParticlesBench:init()
    RegisterEngineEvent(self, "onUpdate")
    local scene = self.scene
    self.frames = 0
    self.systems = {}

    local camera = Camera(scene)
    camera.type = CameraType.CAMERA_PERSPECTIVE
    camera.position = Vector3(0, 1, 12)
    camera.target = Vector3(0, 1, 0)
    scene:setCamera(camera)
    self.camera = camera

    local ok, err = pcall(function()
        for i = 1, SYSTEMS do
            self.systems[#self.systems + 1] = makeSystem(scene, -6 + (i - 1) * 4)
        end
    end)
    if ok then
        log("built %d systems (%d particles a second each, %.1f s lifetime)", SYSTEMS, RATE, LIFETIME)
    else
        print("[ERROR] [PARTICLES] build failed: " .. tostring(err))
    end
end

function ParticlesBench:onUpdate()
    self.frames = self.frames + 1
    if self.frames % 100 == 0 then
        local alive = 0
        for _, ps in ipairs(self.systems) do alive = alive + ps.particleCount end
        log("frame %d: %d alive, %.2f ms", self.frames, alive, Engine.deltatime * 1000)
    end
end

return ParticlesBench
