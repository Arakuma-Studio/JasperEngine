-- S95 AC-17: the counter lives only in the document (the file says 0). After the view comes back
-- (Android without its context, a restored WebGL context) the DOM still holds it and the document
-- loaded once: a view loss keeps the UI.
---@class LifecycleProbe : ScriptInstance
local LifecycleProbe = { properties = {} }

function LifecycleProbe:init()
    self.doc = UIDocument(self.scene, self.entity)
    self.loads = 0
    self.started = false
    self.reportIn = -1
    self.doc.onLoad = function() self.loads = self.loads + 1 end
    self.doc:getElementById("count").textContent = "7"
    RegisterEngineEvent(self, "onViewLoaded")
    RegisterEngineEvent(self, "onViewDestroyed")
    RegisterEngineEvent(self, "onUpdate")
end

-- the view that comes back (the first one was loaded before the scripts started)
function LifecycleProbe:onViewLoaded()
    -- a few frames on: what the first frames after the view did (a reload, onLoad) is in
    self.reportIn = 10
end

function LifecycleProbe:onViewDestroyed()
    Log.print("[lifecycle] view lost")
end

function LifecycleProbe:counter()
    local count = self.doc:getElementById("count")
    return count and count.textContent or "none"
end

function LifecycleProbe:onUpdate()
    if not self.started then
        self.started = true
        Log.print("[lifecycle] start counter " .. self:counter())
    end
    if self.reportIn < 0 then return end
    self.reportIn = self.reportIn - 1
    if self.reportIn > 0 then return end
    self.reportIn = -1
    Log.print(string.format("[lifecycle] view back counter %s loads %d", self:counter(), self.loads))
end

return LifecycleProbe
