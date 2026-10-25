-- Quest log logic: attach to the GameObject that has the UI Document (quest_log.html).
-- The tree shows the "quests" data model's chapters and their quests; Enter (a double click, the
-- gamepad's A) on a quest marks it done and logs "quests: finished <title>". Selecting a quest
-- shows its notes in the journal (Markdown); selecting a chapter shows the journal's first page.
---@class QuestLogUI : ScriptInstance
local QuestLogUI = {
    properties = {}
}

local CHAPTERS = {
    { id = "main", title = "The Burned Mill", children = {
        { id = "map", title = "The old map", reward = "120 gold", notes = "## The old map\n\nThe miller kept a **map** of the valley.\n\n1. Find the miller's daughter\n2. Ask about the *red circle*\n3. Follow the river north" },
        { id = "key", title = "The vault key", reward = "Vault key", notes = "## The vault key\n\nThe key opens the archive beneath the chapel.\n\n> Only at night, they say." },
        { id = "fire", title = "Who lit the fire?", reward = "300 gold", notes = "## Who lit the fire?\n\n| Suspect | Seen |\n|---|---|\n| The smith | at the inn |\n| The guard | at the gate |\n| *Nobody* | at the mill |" },
    } },
    { id = "side", title = "Favours", children = {
        { id = "apples", title = "Golden apples", reward = "3 apples", notes = "## Golden apples\n\nBring three **golden apples** to the orchard keeper." },
        { id = "wolves", title = "Wolves at the farm", reward = "80 gold", notes = "## Wolves at the farm\n\n- two wolves by the barn\n- one more in the *woods*" },
    } },
    { id = "done", title = "Finished", children = {
        { id = "bridge", title = "Mend the bridge", reward = "Done", done = true, notes = "## Mend the bridge\n\nDone: the cart crosses again." },
    } },
}

---@param doc UIDocument
---@param id string
---@return UIElement
local function byId(doc, id)
    local element = doc:getElementById(id)
    assert(element, "quest_log.html has no element #" .. id)
    return element
end

-- Every quest by id, a plain table with done = false where absent
local function index(chapters)
    local quests, total, done = {}, 0, 0
    for _, chapter in ipairs(chapters) do
        chapter.done = false
        chapter.reward = ""
        for _, quest in ipairs(chapter.children) do
            quest.done = quest.done or false
            quest.children = {}
            quests[quest.id] = quest
            total = total + 1
            if quest.done then done = done + 1 end
        end
    end
    return quests, total, done
end

function QuestLogUI:init()
    self.doc = UIDocument(self.scene, self.entity)
    local total, done
    self.quests, total, done = index(CHAPTERS)
    self.model = self.doc:createDataModel("quests", { chapters = CHAPTERS, total = total, done = done })

    self.onSelect = function(event) self:select(event.detail.keys[1]) end
    self.onActivate = function(event) self:finish(event.detail.key) end
    RegisterEvent(self, self.doc.onLoad, "onDocumentLoad")
    self:onDocumentLoad()
end

-- After the first load and every reload: the chapters open, the journal's first page
function QuestLogUI:onDocumentLoad()
    local tree = byId(self.doc, "tree")
    tree:addEventListener("select", self.onSelect)
    tree:addEventListener("activate", self.onActivate)
    local view = tree.itemsView
    view:expand("main")
    view:expand("side")
end

---@param key string|nil a quest's or a chapter's id
function QuestLogUI:select(key)
    local quest = key and self.quests[key]
    local journal = byId(self.doc, "journal")
    if quest then
        journal:setMarkdown(quest.notes)
    else
        -- the journal's first page again: a new src loads it
        journal:removeAttribute("src")
        journal:setAttribute("src", "assets/quests/journal.md")
    end
end

---@param key string a quest's id
function QuestLogUI:finish(key)
    local quest = self.quests[key]
    if not quest or quest.done then return end
    quest.done = true
    for c, chapter in ipairs(self.model.chapters) do
        for q, entry in ipairs(chapter.children) do
            if entry.id == key then self.model.chapters[c].children[q].done = true end
        end
    end
    self.model.done = self.model.done + 1
    print("quests: finished " .. quest.title)
end

return QuestLogUI
