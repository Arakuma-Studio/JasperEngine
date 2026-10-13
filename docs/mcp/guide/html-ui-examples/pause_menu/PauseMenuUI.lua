-- Pause menu logic: attach to the GameObject that has the UI Document (pause_menu.html).
-- Escape opens and closes the menu. While it is open the game is frozen (Engine.timeScale = 0);
-- the menu still animates, because HTML UI runs on the UI clock. Resume closes it, the other
-- buttons log "pause_menu: <id>". On a gamepad Start opens and closes it and B closes it; the
-- d-pad moves between the buttons and A presses one (the document does that by itself).
---@class PauseMenuUI : ScriptInstance
local PauseMenuUI = {
    properties = {}
}

local BUTTONS = { "resume", "options", "restart", "to-menu" }

---@param doc UIDocument
---@param id string
---@return UIElement
local function byId(doc, id)
    local element = doc:getElementById(id)
    assert(element, "pause_menu.html has no element #" .. id)
    return element
end

function PauseMenuUI:init()
    self.doc = UIDocument(self.scene, self.entity)
    self.isOpen = false
    self.timeScale = 1

    -- Made once, so adding them again after a reload is ignored (no double calls)
    self.onButtonClick = function(event)
        local id = event.currentTarget.id
        if id == "resume" then
            self:setOpen(false)
        else
            Log.print("pause_menu: " .. id)
        end
    end

    RegisterEngineEvent(self, "onKeyDown")
    RegisterEngineEvent(self, "onGamepadButtonDown")
    RegisterEvent(self, self.doc.onLoad, "onDocumentLoad")
    self:onDocumentLoad()
end

-- After the first load and every reload: listeners, and the classes the file does not know
function PauseMenuUI:onDocumentLoad()
    for _, id in ipairs(BUTTONS) do
        byId(self.doc, id):addEventListener("click", self.onButtonClick)
    end
    if self.isOpen then
        byId(self.doc, "pause").classList:add("open")
    else
        byId(self.doc, "pause").classList:remove("open")
    end
end

-- Engine key event: Escape is not taken by documents, so the game sees it
function PauseMenuUI:onKeyDown(key, isRepeat, mods)
    if key == Input.KEY_ESCAPE and not isRepeat then
        self:setOpen(not self.isOpen)
    end
end

-- Engine gamepad event: Start and B are not used by the menu's buttons, so the game sees them
function PauseMenuUI:onGamepadButtonDown(gamepad, button)
    if button == Input.GAMEPAD_BUTTON_START then
        self:setOpen(not self.isOpen)
    elseif button == Input.GAMEPAD_BUTTON_B and self.isOpen then
        self:setOpen(false)
    end
end

---@param open boolean
function PauseMenuUI:setOpen(open)
    if open == self.isOpen then return end
    self.isOpen = open
    local pause = byId(self.doc, "pause")
    if open then
        self.timeScale = Engine.timeScale
        Engine.timeScale = 0
        pause.classList:add("open")
        anime.animate(byId(self.doc, "pause-card"), {
            opacity = { from = 0 },
            scale = { from = 0.94 },
            translateY = { from = 12 },
            duration = 250,
            ease = "outCubic",
        })
        byId(self.doc, "resume"):focus()
    else
        Engine.timeScale = self.timeScale
        pause.classList:remove("open")
        byId(self.doc, "resume"):blur()
    end
end

return PauseMenuUI
