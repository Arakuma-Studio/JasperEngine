-- The Lua side of an HTML/CSS UI document: attach it to the GameObject that has the UI Document.
---@class BlankUI : ScriptInstance
local BlankUI = {
    properties = {}
}

function BlankUI:init()
    self.doc = UIDocument(self.scene, self.entity)
    -- onLoad fires after every later load and reload (hot reload included): bind listeners there
    RegisterEvent(self, self.doc.onLoad, "onDocumentLoad")
    self:onDocumentLoad()
    Log.print("BlankUI: document " .. self.doc.filename .. " ready")
end

function BlankUI:onDocumentLoad()
    -- Look elements up here and add listeners, e.g.
    -- self.doc:querySelector("h1"):addEventListener("click", function(event) Log.print("clicked") end)
end

return BlankUI
