-- (c) Eduardo Doria and contributors
-- SPDX-License-Identifier: MIT
--
-- The crowd of the Vertex Animation benchmark (tools/performance/vat_crowd.py): `count` heroes on a
-- grid 1.6 m apart (the T-posed arms span 1.6 m), playing Walk, Idle and Wave in turn, each at its
-- own time of its clip, built when Play starts.
--   mode "skinned": one Model of models/hero.gltf per hero, its Animation Player playing its clip
--                   (the CPU poses every hero every frame, one skinned draw per hero and material)
--   mode "vat":     one Model of `vat`'s .vat.glb with an Instanced Mesh of `count` instances and a
--                   Vertex Animator playing each instance's clip (no CPU pose work, one instanced
--                   draw per material)
-- The Models are kept in `self.objects`: a Model made with Model(scene) owns its entity, and the
-- garbage collector taking it destroys the hero (earlier runs lost most heroes this way).
local Crowd = {
    properties = {
        { name = "mode", displayName = "Mode", type = "string", default = "vat" },
        { name = "count", displayName = "Count", type = "int", default = 2000 },
        { name = "vat", displayName = "VAT", type = "string", default = "vat/hero_vertices.vat" },
    }
}

local CLIPS = { "Walk", "Idle", "Wave" }
local SPACING = 1.6

local function place(i, count)
    local side = math.ceil(math.sqrt(count))
    local x = (i % side) - (side - 1) / 2
    local z = math.floor(i / side) - (side - 1) / 2
    return Vector3(x * SPACING, 0, z * SPACING)
end

-- out of sync: clip i % 3, time spread over the clip by a golden-ratio sequence
local function timeOf(i, length)
    return ((i * 0.6180339887) % 1.0) * length
end

function Crowd:init()
    local count = math.floor(self.count)
    self.objects = {}
    if self.mode == "skinned" then
        local lengths = {}
        for i = 0, count - 1 do
            local hero = Model(self.scene)
            self.objects[#self.objects + 1] = hero
            hero:loadModel("models/hero.gltf")
            hero.position = place(i, count)
            local clip = CLIPS[(i % 3) + 1]
            local player = AnimationPlayer(self.scene, hero.entity)
            player.playAutomatically = false
            player:addClip("models/hero.gltf#" .. clip, clip)
            player:play(clip)
            local state = player:getState(clip)
            state.wrap = "loop"
            lengths[clip] = lengths[clip] or state.length
            -- before the clip loads the length is 0: the call is remembered (spread over 1 s)
            state.time = timeOf(i, (lengths[clip] and lengths[clip] > 0) and lengths[clip] or 1.0)
        end
    else
        local crowd = Model(self.scene)
        self.objects[1] = crowd
        local mesh = string.gsub(self.vat, "%.vat$", ".vat.glb")
        crowd:loadModel(mesh)
        crowd:createInstancedMesh()
        -- culled one by one like the skinned heroes (each by its clips' bounds)
        crowd.cullInstances = true
        for i = 0, count - 1 do
            crowd:addInstance(place(i, count))
        end
        local animator = VertexAnimator(self.scene, crowd.entity)
        animator.playAutomatically = false
        animator.randomStart = false
        animator.animation = self.vat
        for i = 0, count - 1 do
            local clip = CLIPS[(i % 3) + 1]
            animator:playInstance(i, clip)
            local length = animator:getClipLength(clip)
            animator:setInstanceTime(i, timeOf(i, length > 0 and length or 1.0))
        end
    end
    print("[VatCrowd] " .. self.mode .. ": " .. count .. " heroes")
end

return Crowd
