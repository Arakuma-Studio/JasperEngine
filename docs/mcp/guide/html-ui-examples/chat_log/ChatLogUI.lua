-- Chat log logic: attach to the GameObject that has the UI Document (chat_log.html).
-- The list shows the "chat" data model's messages, their text Markdown. A teammate writes every
-- few seconds of game time; Enter or the Send button posts the draft as one's own message and
-- logs "chat: sent <text>". The list keeps to its end while it is there (anchor="end").
---@class ChatLogUI : ScriptInstance
local ChatLogUI = {
    properties = {}
}

local TEAM = {
    { name = "Mira", color = "#f472b6" },
    { name = "Tomas", color = "#60a5fa" },
    { name = "Ines", color = "#fbbf24" },
}
local LINES = {
    "Heading to the **north gate**, cover me",
    "Who has the `vault key`? It opens the archive",
    "Plan:\n1. take the bridge\n2. hold the *tower*\n3. regroup at the mill",
    "> watch the left flank\n\nthey have a sniper up there",
    "Loot so far:\n- 3 potions\n- an *Ember Blade*\n- 1280 gold",
    "Nice shot!",
    "Respawning in 5, wait for me",
    "Map notes are in the [wiki](https://example.com/wiki)",
}
local INTERVAL = 4   -- seconds of game time between teammates' messages

---@param doc UIDocument
---@param id string
---@return UIElement
local function byId(doc, id)
    local element = doc:getElementById(id)
    assert(element, "chat_log.html has no element #" .. id)
    return element
end

---@param author table
---@param text string
---@param mine boolean
local function message(author, text, mine)
    return { author = author.name, initial = string.sub(author.name, 1, 1), color = author.color, text = text, mine = mine }
end

function ChatLogUI:init()
    self.doc = UIDocument(self.scene, self.entity)
    self.clock = 0
    self.next = 0

    local history = {}
    for i = 1, 120 do
        history[i] = message(TEAM[(i % #TEAM) + 1], LINES[(i % #LINES) + 1], i % 5 == 0)
    end
    self.model = self.doc:createDataModel("chat", { messages = history, online = #TEAM + 1 })

    self.onSend = function() self:send() end
    ---@param event UIEvent
    self.onKey = function(event)
        if event.key == "Enter" then
            self:send()
            event:preventDefault()
        end
    end
    RegisterEngineEvent(self, "onUpdate")
    RegisterEvent(self, self.doc.onLoad, "onDocumentLoad")
    self:onDocumentLoad()
end

-- After the first load and every reload
function ChatLogUI:onDocumentLoad()
    byId(self.doc, "send"):addEventListener("click", self.onSend)
    byId(self.doc, "draft"):addEventListener("keydown", self.onKey)
end

-- A teammate's message every INTERVAL seconds of game time
function ChatLogUI:onUpdate()
    self.clock = self.clock + Engine.deltatime
    if self.clock < INTERVAL then return end
    self.clock = self.clock - INTERVAL
    self.next = self.next + 1
    self:post(message(TEAM[(self.next % #TEAM) + 1], LINES[(self.next % #LINES) + 1], false))
end

---@param entry table
function ChatLogUI:post(entry)
    local messages = self.model.messages
    messages[#messages + 1] = entry
end

function ChatLogUI:send()
    local draft = byId(self.doc, "draft")
    local text = draft.value
    if text == "" then return end
    self:post(message({ name = "You", color = "#34d399" }, text, true))
    draft.value = ""
    print("chat: sent " .. text)
end

return ChatLogUI
