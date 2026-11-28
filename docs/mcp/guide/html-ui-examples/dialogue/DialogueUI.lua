-- Dialogue view: attach to the GameObject that has the UI Document (dialogue.html). It shows the
-- lines of the GameObject's Dialogue Runner (one is added when it has none, playing the demo
-- conversation assets/demo.dialogue, a .dialogue graph: edit it in Window > Dialogue).
-- DialogueUI:say(speaker, text, choices) shows a line; it types at 40 characters per second of
-- game time. A click or Space completes the line, then advances. Choices appear when the line is
-- complete; picking one logs "dialogue: choice <n>" and chooses it on the runner. A line's voice
-- plays through Audio.
---@class DialogueUI : ScriptInstance
local DialogueUI = {
    properties = {}
}

local CHARS_PER_SECOND = 40

-- The demo conversation, beside the document (create_ui_document renames assets/ with the document)
local DEMO_DIALOGUE = "assets/demo.dialogue"

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
    self.line = nil         -- the runner's line on show

    -- Made once, so adding them again after a reload is ignored
    self.onBoxClick = function()
        self:advance()
    end
    self.onKeyDown = function(event)
        if event.key == " " then
            self:advance()
            event:preventDefault()
        else
            local digit = tonumber(event.key)
            if digit and self:typed() then self:pick(math.floor(digit)) end
        end
    end

    RegisterEngineEvent(self, "onUpdate")
    RegisterEvent(self, self.doc.onLoad, "onDocumentLoad")
    self:onDocumentLoad()

    -- the runner: the GameObject's own, else one with the demo conversation
    self.runner = DialogueRunner(self.scene, self.entity)
    if self.runner.dialogue == "" then
        local folder = self.doc.filename:match("^(.*)/") or ""
        self.runner.dialogue = (folder ~= "" and folder .. "/" or "") .. DEMO_DIALOGUE
    end
    RegisterEvent(self, self.runner.onLine, "onLine")
    if not self.runner.isRunning then self.runner:start() end
end

-- After the first load and every reload: listeners, and the current line again
function DialogueUI:onDocumentLoad()
    byId(self.doc, "dialogue"):addEventListener("click", self.onBoxClick)
    self.doc:addEventListener("keydown", self.onKeyDown)
    if self.line then self:render() end
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

-- Each line the runner reaches (also sent again when its text changes: a hot reload, a language)
---@param runner DialogueRunner
---@param line DialogueLine
function DialogueUI:onLine(runner, line)
    self:show(line)
end

---@param line DialogueLine
function DialogueUI:show(line)
    self.line = line
    local choices = {}
    for i, option in ipairs(line.options) do choices[i] = option.text end
    if line.voice and Audio then Audio.play(line.voice) end
    self:say(line.speakerName, line.text, choices)
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
    elseif #self.choices == 0 and self.runner.state == "line" then
        self.runner:advance()
    end
end

---@param index integer 1-based
function DialogueUI:pick(index)
    if not self:typed() or not self.choices[index] then return end
    Log.print("dialogue: choice " .. index)
    self.runner:choose(self.line.options[index].index)
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
    box.classList:toggle("waiting", done and #self.choices == 0 and self.line ~= nil and self.runner.state == "line")

    local list = byId(doc, "choices")
    local shownChoices = done and #self.choices or 0
    if list.childElementCount == shownChoices then return end
    for _, child in ipairs(list.children) do child:remove() end
    for i = 1, shownChoices do
        local button = assert(doc:createElement("button"))
        button.className = "choice"
        local key = assert(doc:createElement("span"))
        key.className = "key"
        key.textContent = tostring(i)
        button:appendChild(key)
        button:appendChild(assert(doc:createTextNode(self.choices[i])))
        button:addEventListener("click", function(event)
            event:stopPropagation()   -- the box behind would complete the next line at once
            self:pick(i)
        end)
        list:appendChild(button)
    end
end

return DialogueUI
