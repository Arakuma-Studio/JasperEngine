---@class Title3DCheck : ScriptInstance
-- On the Realtime 3D title: once it is ready, its mesh (cooked in a build without the mesher)
local Title3DCheck = { properties = {} }

function Title3DCheck:init()
    self.title = TitleArt(self.scene, self.entity)
    self.done = false
    RegisterEngineEvent(self, "onUpdate")
end

function Title3DCheck:onUpdate()
    if self.done or not self.title.ready then return end
    self.done = true
    local stats = self.title:getMeshStats("Title")
    if stats == nil then
        Log.error("[gate] title3d: ready without a mesh")
        return
    end
    Log.print("[gate] title3d ready: tier=" .. stats.tier .. " cooked=" .. tostring(stats.cooked) .. " triangles=" .. stats.triangles)
    Log.print("[gate] title3d budget=" .. TitleArt.getFrameBudget())
end

return Title3DCheck
