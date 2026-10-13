-- Main menu logic: attach to the GameObject that has the UI Document (main_menu.html).
-- Clicks log "main_menu: <id>", Settings opens the slide-in panel (class "open"), arrow keys
-- move the focus between the buttons and Enter presses the focused one.
---@class MainMenuUI : ScriptInstance
local MainMenuUI = {
    properties = {}
}

local BUTTONS = { "play", "settings", "credits", "quit" }

---@param doc UIDocument
---@param id string
---@return UIElement
local function byId(doc, id)
    local element = doc:getElementById(id)
    assert(element, "main_menu.html has no element #" .. id)
    return element
end

function MainMenuUI:init()
    self.doc = UIDocument(self.scene, self.entity)
    self.focused = 0   -- index in BUTTONS of the focused button, 0 = none

    -- One function per listener, made once: onDocumentLoad adds the same functions again after a
    -- reload (adding a function twice is ignored), so a handler never runs twice.
    self.onButtonClick = function(event)
        self:press(event.currentTarget.id)
    end
    self.onButtonFocus = function(event)
        for i, id in ipairs(BUTTONS) do
            if id == event.currentTarget.id then self.focused = i end
        end
    end
    self.onButtonBlur = function()
        self.focused = 0
    end
    self.onBack = function()
        self:showSettings(false)
    end
    self.onKeyDown = function(event)
        self:keyDown(event)
    end

    RegisterEvent(self, self.doc.onLoad, "onDocumentLoad")
    self:onDocumentLoad()
end

-- After the first load and every reload (hot reload rebuilds the elements)
function MainMenuUI:onDocumentLoad()
    local doc = self.doc
    for _, id in ipairs(BUTTONS) do
        local button = byId(doc, id)
        button:addEventListener("click", self.onButtonClick)
        button:addEventListener("focus", self.onButtonFocus)
        button:addEventListener("blur", self.onButtonBlur)
    end
    byId(doc, "settings-back"):addEventListener("click", self.onBack)
    doc:addEventListener("keydown", self.onKeyDown)
    self:playEntrance()
end

-- The brand drops in, then the buttons rise one after another
function MainMenuUI:playEntrance()
    local timeline = anime.createTimeline({ defaults = { ease = "outCubic" } })
    timeline:add(byId(self.doc, "brand"), { opacity = { from = 0 }, translateY = { from = -16 }, duration = 500 })
    timeline:add(self.doc:querySelectorAll(".menu-button"), {
        opacity = { from = 0 },
        translateY = { from = 24 },
        duration = 600,
        delay = anime.stagger(70),
    }, "-=250")
end

---@param id string
function MainMenuUI:press(id)
    Log.print("main_menu: " .. id)
    if id == "settings" then
        self:showSettings(true)
    end
end

---@param open boolean
function MainMenuUI:showSettings(open)
    local panel = byId(self.doc, "settings-panel")
    if open then
        panel.classList:add("open")
        byId(self.doc, "settings-back"):focus()
    else
        panel.classList:remove("open")
        byId(self.doc, "settings"):focus()
    end
end

---@param event UIEvent
function MainMenuUI:keyDown(event)
    local panelOpen = byId(self.doc, "settings-panel").classList:contains("open")
    if event.key == "Escape" and panelOpen then
        self:showSettings(false)
        event:preventDefault()
    elseif (event.key == "ArrowDown" or event.key == "ArrowUp") and not panelOpen then
        local step = event.key == "ArrowDown" and 1 or -1
        local index = self.focused == 0 and 1 or (self.focused - 1 + step) % #BUTTONS + 1
        byId(self.doc, BUTTONS[index]):focus()
        event:preventDefault()
    elseif event.key == "Enter" and not panelOpen and self.focused == 0 then
        -- a focused button handles Enter itself (a click); with none focused, Enter starts the game
        byId(self.doc, "play"):click()
        event:preventDefault()
    end
end

return MainMenuUI
