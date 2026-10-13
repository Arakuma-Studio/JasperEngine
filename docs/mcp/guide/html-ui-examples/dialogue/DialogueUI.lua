-- Dialogue logic: attach to the GameObject that has the UI Document (dialogue.html).
-- DialogueUI:say(speaker, text, choices) shows a line; it types at 40 characters per second of
-- game time. A click or Space completes the line, then advances. Choices (a list of strings)
-- appear when the line is complete; picking one logs "dialogue: choice <n>" and follows the
-- conversation table below.
---@class DialogueUI : ScriptInstance
local DialogueUI = {
    properties = {}
}

local CHARS_PER_SECOND = 40

-- The demo conversation: each node is a line, with choices (and where each leads) or a next node
local CONVERSATION = {
    start = { speaker = "Captain Mira",
              text = "The archive doors are sealed and the tide is rising. We have one shot at this. How do we get in?",
              choices = { { "Blow the doors with a charge.", "blast" },
                          { "Look for the old maintenance shaft.", "shaft" },
                          { "Wait for the tide to turn.", "wait" } } },
    blast = { speaker = "Captain Mira", text = "Loud, but it works. Everyone, stand back from the doors!", next = "after" },
    shaft = { speaker = "Captain Mira", text = "Good eye. The grate is rusted through. Follow me, and keep your light low.", next = "after" },
    wait = { speaker = "Captain Mira", text = "Then we wait. But the watch changes at dawn, and we will not be alone.", next = "after" },
    after = { speaker = "Narrator", text = "Somewhere below, something old stirs in the dark." },
}

---@param doc UIDocument
---@param id string
---@return UIElement
local function byId(doc, id)
    local element = doc:getElementById(id)
    assert(element, "dialogue.html has no element #" .. id)
    return element
end

function DialogueUI:init()
    self.doc = UIDocument(self.scene, self.entity)
    self.text = ""          -- the whole line
    self.length = 0         -- in characters (UTF-8)
    self.shown = 0          -- characters typed so far (fractional)
    self.choices = {}
    self.node = nil

    -- Made once, so adding them again after a reload is ignored
    self.onBoxClick = function()
        self:advance()
    end
    self.onKeyDown = function(event)
        if event.key == " " then
            self:advance()
            event:preventDefault()
        elseif tonumber(event.key) and self:typed() then
            self:pick(tonumber(event.key))
        end
    end

    RegisterEngineEvent(self, "onUpdate")
    RegisterEvent(self, self.doc.onLoad, "onDocumentLoad")
    self:onDocumentLoad()
    self:play("start")
end

-- After the first load and every reload: listeners, and the current line again
function DialogueUI:onDocumentLoad()
    byId(self.doc, "dialogue"):addEventListener("click", self.onBoxClick)
    self.doc:addEventListener("keydown", self.onKeyDown)
    if self.node then self:render() end
end

---@param speaker string
---@param text string
---@param choices string[]|nil
function DialogueUI:say(speaker, text, choices)
    self.speaker = speaker
    self.text = text
    self.length = utf8.len(text) or #text
    self.shown = 0
    self.choices = choices or {}
    self:render()
end

---@param id string a key of CONVERSATION
function DialogueUI:play(id)
    self.node = CONVERSATION[id]
    local choices = {}
    for i, choice in ipairs(self.node.choices or {}) do choices[i] = choice[1] end
    self:say(self.node.speaker, self.node.text, choices)
end

---@return boolean
function DialogueUI:typed()
    return self.shown >= self.length
end

-- Click or Space: complete the line, then go on (lines with choices wait for one)
function DialogueUI:advance()
    if not self:typed() then
        self.shown = self.length
        self:render()
    elseif #self.choices == 0 and self.node and self.node.next then
        self:play(self.node.next)
    end
end

---@param index integer 1-based
function DialogueUI:pick(index)
    if not self:typed() or not self.choices[index] then return end
    Log.print("dialogue: choice " .. index)
    if self.node and self.node.choices then
        self:play(self.node.choices[index][2])
    end
end

function DialogueUI:onUpdate()
    if self:typed() then return end
    local before = math.floor(self.shown)
    self.shown = math.min(self.length, self.shown + Engine.deltatime * CHARS_PER_SECOND)
    if math.floor(self.shown) ~= before then self:render() end
end

-- The document from the state: speaker, the typed part of the line, the choices once typed
function DialogueUI:render()
    local doc = self.doc
    byId(doc, "speaker").textContent = self.speaker or ""
    local count = math.floor(self.shown)
    local cut = count >= self.length and #self.text or (utf8.offset(self.text, count + 1) or #self.text + 1) - 1
    byId(doc, "line").textContent = self.text:sub(1, cut)

    local done = self:typed()
    local box = byId(doc, "dialogue")
    box.classList:toggle("waiting", done and #self.choices == 0 and self.node ~= nil and self.node.next ~= nil)

    local list = byId(doc, "choices")
    local shownChoices = done and #self.choices or 0
    if list.childElementCount == shownChoices then return end
    for _, child in ipairs(list.children) do child:remove() end
    for i = 1, shownChoices do
        local button = doc:createElement("button")
        button.className = "choice"
        local key = doc:createElement("span")
        key.className = "key"
        key.textContent = tostring(i)
        button:appendChild(key)
        button:appendChild(doc:createTextNode(self.choices[i]))
        button:addEventListener("click", function(event)
            event:stopPropagation()   -- the box behind would complete the next line at once
            self:pick(i)
        end)
        list:appendChild(button)
    end
end

return DialogueUI
