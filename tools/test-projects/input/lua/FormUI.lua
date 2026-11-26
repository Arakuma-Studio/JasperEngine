-- text-entry-gamepad-cursor-back-button AC7, AC22 (main.cpp says where it runs): the e-mail field
-- of form.html. At the input scheme's second switch (after the gates' tap and key, or mouse and
-- touch taps) the field takes the focus and the document becomes a menu, so Back reaches it.
---@class FormUI : ScriptInstance
local FormUI = {
    properties = {}
}

function FormUI:init()
    self.doc = UIDocument(self.scene, self.entity)
    self.switches = 0
    -- one function each: a listener added again on a reload with the same function is not doubled
    self.onInput = function() Log.print("[gate] value " .. self.email.value .. " (" .. #self.email.value .. ")") end
    self.onSubmit = function(event)
        event:preventDefault()
        Log.print("[gate] submit")
    end
    self.onBack = function(event)
        event:preventDefault()
        Log.print("[gate] back")
    end
    RegisterEvent(self, self.doc.onLoad, "onDocumentLoad")
    self:onDocumentLoad()
    -- main.lua's onSchemeChanged calls this after logging the switch
    FormUIOnScheme = function() self:onScheme() end
end

function FormUI:onDocumentLoad()
    local email = self.doc:getElementById("email")
    local form = self.doc:getElementById("form")
    if not email or not form then return end
    self.email = email
    email:addEventListener("input", self.onInput)
    form:addEventListener("submit", self.onSubmit)
    self.doc:addEventListener("back", self.onBack)
end

function FormUI:onScheme()
    self.switches = self.switches + 1
    if self.switches ~= 2 or not self.email then return end
    self.doc.isMenu = true
    self.email:focus()
    Log.print("[gate] form focused")
end

return FormUI
