-- Server browser logic: attach to the GameObject that has the UI Document (server_browser.html).
-- The table shows the "browser" data model's servers: ten thousand of them, made up here. A click
-- on a sortable header asks to sort (the sort event): the list is sorted and set again. The chips
-- filter by region, Refresh makes new pings, a selected server fills the details panel, and Join
-- (or Enter, a double click, the gamepad's A on a row) logs "servers: joining <name>".
---@class ServerBrowserUI : ScriptInstance
local ServerBrowserUI = {
    properties = {}
}

local COUNT = 10000
local MAPS = { "Harbor", "Dunes", "Citadel", "Glacier", "Foundry", "Canopy", "Ruins", "Skyline" }
local REGIONS = { "EU", "NA", "Asia" }
local MODES = { "Duel", "Squads", "Capture", "Siege" }
local NONE = { id = 0, name = "", map = "", region = "", players = 0, max = 0, ping = 0, tip = "" }

---@param doc UIDocument
---@param id string
---@return UIElement
local function byId(doc, id)
    local element = doc:getElementById(id)
    assert(element, "server_browser.html has no element #" .. id)
    return element
end

-- A small deterministic generator: the same list on every run
local seed = 1
local function random(n)
    seed = (seed * 1103515245 + 12345) % 2147483648
    return seed % n
end

function ServerBrowserUI:init()
    self.doc = UIDocument(self.scene, self.entity)
    self.all = {}
    for i = 1, COUNT do
        local map, region, mode = MAPS[random(#MAPS) + 1], REGIONS[random(#REGIONS) + 1], MODES[random(#MODES) + 1]
        local max = (random(4) + 2) * 8
        local server = { id = i, name = string.format("%s %s #%d", mode, map, i), map = map, region = region,
                         players = random(max + 1), max = max, ping = 12 + random(220) }
        server.tip = string.format("%s, %s: %d of %d players", server.name, region, server.players, max)
        self.all[i] = server
    end
    self.sortKey, self.descending = "ping", false

    -- data-event-* callbacks: fn(event, arguments...)
    self.model = self.doc:createDataModel("browser", {
        servers = self:visible(""),
        shown = 0,
        region = "",
        selected = NONE,
        filter = function(event, region) self:filter(region) end,
        join = function() self:join(self.model.selected.id) end,
    })
    self.model.shown = #self.model.servers

    self.onSort = function(event) self:sort(event.detail.key, event.detail.direction) end
    self.onSelect = function(event) self:select(event.detail.keys[1]) end
    self.onActivate = function(event) self:join(tonumber(event.detail.key)) end
    self.onRefresh = function() self:refresh() end
    RegisterEvent(self, self.doc.onLoad, "onDocumentLoad")
    self:onDocumentLoad()
end

-- After the first load and every reload
function ServerBrowserUI:onDocumentLoad()
    local table = byId(self.doc, "servers")
    table:addEventListener("sort", self.onSort)
    table:addEventListener("select", self.onSelect)
    table:addEventListener("activate", self.onActivate)
    byId(self.doc, "refresh"):addEventListener("click", self.onRefresh)
end

-- The servers of a region ("" all), in the current order
---@param region string
---@return table[]
function ServerBrowserUI:visible(region)
    local list = {}
    for _, server in ipairs(self.all) do
        if region == "" or server.region == region then list[#list + 1] = server end
    end
    local key, descending = self.sortKey, self.descending
    table.sort(list, function(a, b)
        if a[key] == b[key] then return a.id < b.id end
        if descending then return a[key] > b[key] end
        return a[key] < b[key]
    end)
    return list
end

function ServerBrowserUI:show()
    local list = self:visible(self.model.region)
    self.model.servers = list
    self.model.shown = #list
end

---@param key string
---@param direction string "asc" or "desc"
function ServerBrowserUI:sort(key, direction)
    self.sortKey, self.descending = key, direction == "desc"
    self:show()
end

---@param region string
function ServerBrowserUI:filter(region)
    self.model.region = region
    self:show()
end

---@param id string|nil the selected row's key (the server id)
function ServerBrowserUI:select(id)
    local server = id and self.all[tonumber(id)]
    self.model.selected = server or NONE
end

---@param id integer|nil
function ServerBrowserUI:join(id)
    local server = id and self.all[id]
    if server then print("servers: joining " .. server.name) end
end

-- New pings for every server (the order follows when sorted by ping)
function ServerBrowserUI:refresh()
    for _, server in ipairs(self.all) do
        server.ping = 12 + random(220)
    end
    self:show()
end

return ServerBrowserUI
