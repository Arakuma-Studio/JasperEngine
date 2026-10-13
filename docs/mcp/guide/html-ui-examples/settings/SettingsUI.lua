-- Settings logic: attach to the GameObject that has the UI Document (settings.html).
-- The controls are bound two ways to the "settings" data model: reading self.model.music gives
-- what the slider shows. Tabs switch the "active" class, a key-binding button waits for the
-- next key, Reset restores the defaults and Apply logs "settings: apply" with the values.
---@class SettingsUI : ScriptInstance
local SettingsUI = {
    properties = {}
}

local TABS = { "tab-graphics", "tab-audio", "tab-controls", "tab-gameplay" }
local BINDINGS = { "bind-forward", "bind-jump", "bind-interact" }
local DEFAULTS = {
    resolution = "1920x1080", quality = "high", fov = 90, vsync = true, bloom = true,
    master = 80, music = 60, sfx = 75, subtitles = true,
    sensitivity = 8, invertY = false,
    difficulty = "normal", shake = 50, damageNumbers = true,
}
local DEFAULT_KEYS = { forward = "W", jump = "Space", interact = "E" }

---@param doc UIDocument
---@param id string
---@return UIElement
local function byId(doc, id)
    local element = doc:getElementById(id)
    assert(element, "settings.html has no element #" .. id)
    return element
end

-- "a" -> "A", " " -> "Space", "ArrowUp" stays
---@param key string
---@return string
local function keyLabel(key)
    if key == " " then return "Space" end
    if #key == 1 then return key:upper() end
    return key
end

function SettingsUI:init()
    self.doc = UIDocument(self.scene, self.entity)

    -- Data model first: the document reloads once to bind it
    local values = { capturing = "", keys = {} }
    for name, value in pairs(DEFAULTS) do values[name] = value end
    for action, key in pairs(DEFAULT_KEYS) do values.keys[action] = key end
    self.model = self.doc:createDataModel("settings", values)

    -- Made once, so adding them again after a reload is ignored
    self.onTab = function(event) self:selectTab(event.currentTarget.id) end
    self.onBind = function(event) self.model.capturing = event.currentTarget.dataset.action end
    self.onKeyDown = function(event) self:captureKey(event) end
    self.onReset = function() self:reset() end
    self.onApply = function() self:apply() end
    self.onBack = function() Log.print("settings: back") end

    RegisterEvent(self, self.doc.onLoad, "onDocumentLoad")
    self:onDocumentLoad()
end

-- After the first load and every reload
function SettingsUI:onDocumentLoad()
    local doc = self.doc
    for _, id in ipairs(TABS) do byId(doc, id):addEventListener("click", self.onTab) end
    for _, id in ipairs(BINDINGS) do byId(doc, id):addEventListener("click", self.onBind) end
    doc:addEventListener("keydown", self.onKeyDown)
    byId(doc, "reset"):addEventListener("click", self.onReset)
    byId(doc, "apply"):addEventListener("click", self.onApply)
    byId(doc, "back"):addEventListener("click", self.onBack)
    self:selectTab(self.tab or "tab-graphics")
end

---@param tabId string
function SettingsUI:selectTab(tabId)
    self.tab = tabId
    for _, id in ipairs(TABS) do
        local tab = byId(self.doc, id)
        local active = id == tabId
        tab.classList:toggle("active", active)
        byId(self.doc, tab.dataset.panel).classList:toggle("active", active)
    end
end

---@param event UIEvent
function SettingsUI:captureKey(event)
    local action = self.model.capturing
    if action == "" then return end
    if event.key ~= "Escape" then
        self.model.keys[action] = keyLabel(event.key)
    end
    self.model.capturing = ""
    -- the key is the binding: it must not press the focused button too
    event:preventDefault()
    event:stopPropagation()
end

function SettingsUI:reset()
    for name, value in pairs(DEFAULTS) do self.model[name] = value end
    for action, key in pairs(DEFAULT_KEYS) do self.model.keys[action] = key end
    self.model.capturing = ""
end

function SettingsUI:apply()
    local parts = {}
    for name in pairs(DEFAULTS) do parts[#parts + 1] = name .. "=" .. tostring(self.model[name]) end
    for action in pairs(DEFAULT_KEYS) do parts[#parts + 1] = action .. "=" .. self.model.keys[action] end
    table.sort(parts)
    Log.print("settings: apply {" .. table.concat(parts, ", ") .. "}")
end

return SettingsUI
