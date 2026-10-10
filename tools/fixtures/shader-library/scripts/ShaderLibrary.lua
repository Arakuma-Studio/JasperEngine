-- Builds the shader library showcase at runtime (so every variant compiles in the game):
-- a camera, a ground, a directional light, then each group script present. A group is a
-- module scripts/<group>.lua, one per set of library shaders so groups never collide:
--
--   return {
--       build = function(ctx) ... end,       -- places its objects (ctx:place) and shaders
--       update = function(ctx, dt) ... end,  -- optional, every frame
--   }
--
-- Groups are looked up by the names in GROUPS (add yours there); a missing one is skipped.
--   tools/smoke-test.sh tools/fixtures/shader-library
local ShaderLibrary = { properties = {} }

local GROUPS = {
    "core", "lighting", "lit", "stylized", "effects", "nature", "animated",
    "sprites", "ui", "particles", "sky", "decals", "extra",
}

-- each group fills rows of COLUMNS cells, starting on the row after the previous group's
local COLUMNS = 8
local SPACING = 1.9
local ROW_DEPTH = 2.4
local FIRST_ROW_Z = 3.0

local function log(fmt, ...)
    print("[SL] " .. string.format(fmt, ...))
end

local function fail(fmt, ...)
    -- the smoke test fails on an [ERROR] in the game log
    print("[ERROR] [SL] " .. string.format(fmt, ...))
end

-- What a group builds with
local Context = {}
Context.__index = Context

function Context.new(scene, name, originZ)
    return setmetatable({ scene = scene, name = name, originZ = originZ, count = 0, rows = 0, objects = {} }, Context)
end

-- puts an object in the group's next cell, at height y (default 0.6); returns its position
function Context:place(object, y)
    local column = self.count % COLUMNS
    local row = math.floor(self.count / COLUMNS)
    self.count = self.count + 1
    self.rows = row + 1
    local position = Vector3((column - (COLUMNS - 1) / 2) * SPACING, y or 0.6, self.originZ - row * ROW_DEPTH)
    object.position = position
    table.insert(self.objects, object)
    return position
end

-- sets shader on every submesh of a mesh; configure(material, submesh) edits each material
function Context:setShader(mesh, shader, configure)
    for i = 0, mesh:getNumSubmeshes() - 1 do
        local material = mesh:getMaterial(i)
        material.shader = shader
        if configure then
            configure(material, i)
        end
        mesh:setMaterial(i, material)
    end
end

function Context:log(fmt, ...)
    log("[" .. self.name .. "] " .. string.format(fmt, ...))
end

function Context:fail(fmt, ...)
    fail("[" .. self.name .. "] " .. string.format(fmt, ...))
end

function ShaderLibrary:init()
    RegisterEngineEvent(self, "onUpdate")
    local scene = self.scene

    local camera = Camera(scene)
    camera.type = CameraType.CAMERA_PERSPECTIVE
    camera.position = Vector3(0, 7.5, 12.5)
    camera.target = Vector3(0, 0, -2.5)
    scene:setCamera(camera)

    local light = Light(scene)
    light.type = LightType.DIRECTIONAL
    light:setDirection(-0.4, -1.0, -0.5)
    light.intensity = 2.5
    light.shadows = true

    local ground = Primitive(scene)
    ground:createPlane(60, 60)
    ground.position = Vector3(0, 0, -15)
    ground:setColor(0.42, 0.44, 0.48)

    self.groups = {}
    local originZ = FIRST_ROW_Z
    for _, name in ipairs(GROUPS) do
        local ok, group = pcall(require, name)
        if ok and type(group) == "table" and group.build then
            local ctx = Context.new(scene, name, originZ)
            local built, err = pcall(group.build, ctx)
            if not built then
                fail("group %s failed to build: %s", name, tostring(err))
            end
            log("group %s: %d objects in %d rows", name, ctx.count, ctx.rows)
            table.insert(self.groups, { module = group, ctx = ctx })
            originZ = originZ - math.max(ctx.rows, 1) * ROW_DEPTH - 0.6
        elseif ok then
            fail("group %s is not a table with build(ctx)", name)
        elseif not tostring(group):find("no file in assets directory", 1, true) then
            fail("group %s failed to load: %s", name, tostring(group))
        end
    end
    if #self.groups == 0 then
        fail("no group script found")
    end

    self.time = 0
    self.frames = 0
end

function ShaderLibrary:onUpdate()
    local dt = Engine.deltatime
    self.time = self.time + dt
    self.frames = self.frames + 1
    for _, group in ipairs(self.groups) do
        if group.module.update then
            local ok, err = pcall(group.module.update, group.ctx, dt)
            if not ok then
                fail("group %s update: %s", group.ctx.name, tostring(err))
                group.module.update = nil
            end
        end
    end
    if self.frames % 100 == 0 then
        log("frame %d (%.2f s): %.0f fps, %d variants compiling", self.frames, self.time, Engine.framerate,
            ShaderPool.getPendingShaderCount())
    end
end

return ShaderLibrary
