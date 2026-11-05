-- Mesh benchmark (docs/specs/S55-draw-submission.md R8), built at runtime from the assets of
-- tools/fixtures/mesh-bench/generate.py: 2,000 Models of 10 meshes on a grid, 200 unique meshes,
-- a terrain under a sun of 4 shadow cascades, a skinned crowd of 50, and 20 copies each of an
-- interleaved and a shared-accessor glTF. The camera and every object hold still (the crowd's
-- bones are posed once), so the counters are the same every frame.
local MeshBench = { properties = {} }

local function log(fmt, ...)
    print("[MB] " .. string.format(fmt, ...))
end

local function fail(fmt, ...)
    print("[ERROR] [MB] " .. string.format(fmt, ...))
end

function MeshBench:init()
    local scene = self.scene
    -- a GameObject made here lives as long as its Lua object: keep them all
    self.objects = {}
    local function keep(object)
        table.insert(self.objects, object)
        return object
    end
    local function model(path, position, color)
        local m = keep(Model(scene))
        if not m:loadModel(path) then
            fail("%s did not load", path)
            return nil
        end
        m.position = position
        if color then
            m:setColor(color.x, color.y, color.z)
        end
        return m
    end

    scene:setGlobalIllumination(0.2, Vector3(0.6, 0.65, 0.8))
    -- detail levels on (the shadow passes take them about one level coarser: S54's rule)
    scene.meshLodEnabled = true

    local camera = keep(Camera(scene))
    camera.type = CameraType.CAMERA_PERSPECTIVE
    camera.position = Vector3(0, 18, 46)
    camera.target = Vector3(0, 0, 0)
    scene:setCamera(camera)

    local sun = keep(Light(scene))
    sun.type = LightType.DIRECTIONAL
    sun:setDirection(-0.45, -1.0, -0.35)
    sun.intensity = 2.5
    sun.shadowType = ShadowType.HARD
    sun.numCascades = 4

    local terrain = keep(Terrain(scene))
    terrain:setHeightMap("textures/height.png")
    terrain.size = 160
    terrain.maxHeight = 3
    terrain.position = Vector3(0, -2, 0)

    -- 2,000 Models of 10 meshes, in 3 colours: 50 x 40 on a grid
    local colors = { Vector3(0.8, 0.3, 0.3), Vector3(0.3, 0.7, 0.4), Vector3(0.3, 0.4, 0.8) }
    for n = 0, 1999 do
        local x = n % 50
        local z = math.floor(n / 50)
        model("meshes/m" .. (n % 10) .. ".gltf", Vector3(-49 + x * 2, 1, -30 + z * 1.5), colors[(n // 10) % 3 + 1])
    end

    -- 200 unique meshes (one file, a node and mesh each)
    model("unique.gltf", Vector3(0, 1, -10))

    -- 20 copies each of the interleaved and the shared-accessor glTF
    for i = 0, 19 do
        model("interleaved.gltf", Vector3(-19 + i * 2, 1, 32))
        model("shared.gltf", Vector3(-19 + i * 2, 1, 35))
    end

    -- a skinned crowd of 50
    for i = 0, 49 do
        model("crowd.gltf", Vector3(-24.5 + (i % 25) * 2, 0, 38 + math.floor(i / 25) * 2))
    end

    log("bench: %d objects", #self.objects)
end

return MeshBench
