-- Inventory logic: attach to the GameObject that has the UI Document (inventory.html).
-- The grid shows the "inventory" data model's 24 entries. Drag a slot onto another to swap
-- them, hover a filled slot for its tooltip, close with the X; I shows and hides the window.
-- A click (or the gamepad's A) on a filled slot picks its item up and one on another slot puts
-- it down there; Escape (B) cancels that, or closes the window. The gamepad's View button shows
-- and hides it; the d-pad moves between the slots (they have tabindex="0").
---@class InventoryUI : ScriptInstance
local InventoryUI = {
    properties = {}
}

local SLOTS = 24
local RARITY = { common = "Common", rare = "Rare", epic = "Epic" }

-- Slot index -> item (the other slots are empty). Pictures are relative to the document.
local START = {
    [1] = { id = "sword", name = "Ember Blade", icon = "assets/items/sword.svg", count = 1, rarity = "epic", weight = 6,
            desc = "Forged in dragonfire. +12 attack; hits burn for 3 seconds." },
    [2] = { id = "shield", name = "Tower Shield", icon = "assets/items/shield.svg", count = 1, rarity = "rare", weight = 9,
            desc = "Blocks 40% of the damage from the front." },
    [3] = { id = "potion", name = "Health Potion", icon = "assets/items/potion.svg", count = 5, rarity = "common", weight = 1,
            desc = "Restores 50 health over 3 seconds." },
    [4] = { id = "gem", name = "Starlight Gem", icon = "assets/items/gem.svg", count = 2, rarity = "epic", weight = 0,
            desc = "Socket it into a weapon: +5% critical chance." },
    [5] = { id = "apple", name = "Golden Apple", icon = "assets/items/apple.svg", count = 3, rarity = "rare", weight = 1,
            desc = "Fully restores stamina." },
    [8] = { id = "key", name = "Vault Key", icon = "assets/items/key.svg", count = 1, rarity = "rare", weight = 0,
            desc = "Opens the sealed vault beneath the archive." },
    [9] = { id = "scroll", name = "Old Map", icon = "assets/items/scroll.svg", count = 1, rarity = "common", weight = 0,
            desc = "A faded map. Something is circled in red." },
    [10] = { id = "axe", name = "Woodcutter's Axe", icon = "assets/items/axe.svg", count = 1, rarity = "common", weight = 7,
             desc = "Chops trees. And other things." },
    [13] = { id = "wand", name = "Arcane Wand", icon = "assets/items/wand.svg", count = 1, rarity = "epic", weight = 2,
             desc = "Channels three bolts of arcane force." },
    [16] = { id = "bomb", name = "Bomb", icon = "assets/items/bomb.svg", count = 4, rarity = "common", weight = 2,
             desc = "Breaks cracked walls. Keep away from fire." },
}

---@param doc UIDocument
---@param id string
---@return UIElement
local function byId(doc, id)
    local element = doc:getElementById(id)
    assert(element, "inventory.html has no element #" .. id)
    return element
end

-- A plain copy of an entry (model entries read through the proxy are proxies themselves)
local function entry(item)
    if not item or item.id == "" then
        return { id = "", name = "", icon = "", count = 0, rarity = "", weight = 0, desc = "" }
    end
    return { id = item.id, name = item.name, icon = item.icon, count = item.count, rarity = item.rarity,
             weight = item.weight, desc = item.desc }
end

function InventoryUI:init()
    self.doc = UIDocument(self.scene, self.entity)
    self.dragFrom = nil

    local items = {}
    for i = 1, SLOTS do items[i] = entry(START[i]) end

    -- Data model first: the document reloads once to bind it. Functions are the
    -- data-event-* callbacks: fn(event, arguments...), and i is 0-based.
    self.model = self.doc:createDataModel("inventory", {
        gold = 1280,
        weight = 0,
        maxWeight = 60,
        items = items,
        picked = 0,   -- 1-based slot of the item picked up with a click or A, 0 = none
        hover = function(event, i) self:showTooltip(event.currentTarget, i + 1) end,
        select = function(event, i) self:select(i + 1) end,
        unhover = function() self:hideTooltip() end,
        pick = function(event, i) self.dragFrom = i + 1 end,
        drop = function(event, i) self:swap(self.dragFrom, i + 1) end,
    })
    self:updateWeight()

    self.onClose = function() self.doc:hide() end
    ---@param event UIEvent
    self.onKey = function(event) self:keyDown(event) end
    RegisterEngineEvent(self, "onKeyDown")
    RegisterEngineEvent(self, "onGamepadButtonDown")
    RegisterEvent(self, self.doc.onLoad, "onDocumentLoad")
    self:onDocumentLoad()
end

-- After the first load and every reload
function InventoryUI:onDocumentLoad()
    byId(self.doc, "close"):addEventListener("click", self.onClose)
    self.doc:addEventListener("keydown", self.onKey)
end

---@param event UIEvent
function InventoryUI:keyDown(event)
    if event.key == "Escape" then
        if self.model.picked ~= 0 then self.model.picked = 0 else self.doc:hide() end
        event:preventDefault()
    elseif event.gamepad >= 0 and not self.doc:querySelector(".slot:focus, .close:focus") then
        -- the first gamepad key focuses the first slot: the grid takes the d-pad from then on
        local first = self.doc:querySelector(".grid > .slot")
        if first then first:focus() end
        event:preventDefault()
    end
end

---@param index integer 1-based
function InventoryUI:select(index)
    local picked = self.model.picked
    if picked ~= 0 then
        self.model.picked = 0
        self:swap(picked, index)
    elseif self.model.items[index].id ~= "" then
        self.model.picked = index
    end
end

function InventoryUI:onGamepadButtonDown(gamepad, button)
    if button == Input.GAMEPAD_BUTTON_BACK then
        if self.doc.visible then
            self.doc:hide()
        else
            self.doc:show()
            local first = self.doc:querySelector(".grid > .slot")
            if first then first:focus() end   -- the gamepad goes on in the grid
        end
    end
end

function InventoryUI:onKeyDown(key, isRepeat, mods)
    if key == Input.KEY_I and not isRepeat then
        if self.doc.visible then self.doc:hide() else self.doc:show() end
    end
end

---@param from integer|nil 1-based slot
---@param to integer 1-based slot
function InventoryUI:swap(from, to)
    self.dragFrom = nil
    local items = self.model.items
    if not from or from == to or items[from].id == "" then return end
    local moved, other = entry(items[from]), entry(items[to])
    items[from] = other   -- assigning through the proxy updates the slots
    items[to] = moved
    self:hideTooltip()
end

function InventoryUI:updateWeight()
    local total = 0
    for i = 1, SLOTS do
        local item = self.model.items[i]
        total = total + item.weight * item.count
    end
    self.model.weight = total
end

---@param slot UIElement
---@param index integer 1-based
function InventoryUI:showTooltip(slot, index)
    local item = self.model.items[index]
    if item.id == "" then
        self:hideTooltip()
        return
    end
    local tooltip = byId(self.doc, "tooltip")
    byId(self.doc, "tooltip-title").textContent = item.name
    byId(self.doc, "tooltip-rarity").textContent = RARITY[item.rarity] or ""
    byId(self.doc, "tooltip-desc").textContent = item.desc
    tooltip.classList:remove("rare", "epic")
    if item.rarity ~= "common" then tooltip.classList:add(item.rarity) end

    -- next to the slot, on its left when there is no room on its right
    local rect = slot:getBoundingClientRect()
    local x = rect.right + 12
    if x + tooltip.offsetWidth > self.doc.body.clientWidth then
        x = rect.left - 12 - tooltip.offsetWidth
    end
    tooltip.style.left = string.format("%dpx", math.floor(x))
    tooltip.style.top = string.format("%dpx", math.floor(rect.top))
    tooltip.classList:add("show")
end

function InventoryUI:hideTooltip()
    byId(self.doc, "tooltip").classList:remove("show")
end

return InventoryUI
