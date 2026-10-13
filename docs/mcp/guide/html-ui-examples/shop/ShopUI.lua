-- Shop logic: attach to the GameObject that has the UI Document (shop.html).
-- The cards are the "shop" data model's items; a chip sets the filter, Buy opens the dialog,
-- the stepper sets the quantity and Buy in the dialog pays: the purse counts down with anime
-- and "shop: bought <id> x<n>" is logged. Buttons the purse cannot pay for are disabled by
-- the document itself (data-attrif-disabled).
---@class ShopUI : ScriptInstance
local ShopUI = {
    properties = {}
}

-- Pictures are relative to the document
local WARES = {
    { id = "sword", name = "Ember Blade", kind = "weapons", rarity = "Epic", stats = "+12 attack, burns", price = 450, icon = "assets/items/sword.svg" },
    { id = "axe", name = "Woodcutter's Axe", kind = "weapons", rarity = "Common", stats = "+6 attack", price = 120, icon = "assets/items/axe.svg" },
    { id = "wand", name = "Arcane Wand", kind = "weapons", rarity = "Epic", stats = "3 arcane bolts", price = 980, icon = "assets/items/wand.svg" },
    { id = "shield", name = "Tower Shield", kind = "armor", rarity = "Rare", stats = "Blocks 40% damage", price = 300, icon = "assets/items/shield.svg" },
    { id = "helmet", name = "Iron Helm", kind = "armor", rarity = "Common", stats = "+4 armor", price = 160, icon = "assets/items/helmet.svg" },
    { id = "crown", name = "Crown of Ages", kind = "armor", rarity = "Epic", stats = "+20% experience", price = 1500, icon = "assets/items/crown.svg" },
    { id = "potion", name = "Health Potion", kind = "potions", rarity = "Common", stats = "Restores 50 health", price = 35, icon = "assets/items/potion.svg" },
    { id = "apple", name = "Golden Apple", kind = "potions", rarity = "Rare", stats = "Full stamina", price = 60, icon = "assets/items/apple.svg" },
}

---@param doc UIDocument
---@param id string
---@return UIElement
local function byId(doc, id)
    local element = doc:getElementById(id)
    assert(element, "shop.html has no element #" .. id)
    return element
end

function ShopUI:init()
    self.doc = UIDocument(self.scene, self.entity)
    self.gold = 640       -- the purse; the model's gold is what the counter shows
    self.pending = nil    -- the ware the dialog is about

    -- Data model first: the document reloads once to bind it. Functions are the
    -- data-event-* callbacks: fn(event, arguments...), and i is 0-based.
    self.model = self.doc:createDataModel("shop", {
        gold = self.gold,
        filter = "all",
        items = WARES,
        qty = 1,
        pendingName = "",
        pendingIcon = "",
        pendingPrice = 0,
        setFilter = function(event, kind) self.model.filter = kind end,
        buy = function(event, i) self:openConfirm(i + 1) end,
    })

    -- Made once, so adding them again after a reload is ignored
    self.onMinus = function() self:setQuantity(self.model.qty - 1) end
    self.onPlus = function() self:setQuantity(self.model.qty + 1) end
    self.onCancel = function() self:closeConfirm() end
    self.onConfirm = function() self:confirm() end

    RegisterEvent(self, self.doc.onLoad, "onDocumentLoad")
    self:onDocumentLoad()
end

-- After the first load and every reload
function ShopUI:onDocumentLoad()
    local doc = self.doc
    byId(doc, "qty-minus"):addEventListener("click", self.onMinus)
    byId(doc, "qty-plus"):addEventListener("click", self.onPlus)
    byId(doc, "confirm-cancel"):addEventListener("click", self.onCancel)
    byId(doc, "confirm-buy"):addEventListener("click", self.onConfirm)
    byId(doc, "confirm").classList:toggle("open", self.pending ~= nil)
end

---@param index integer 1-based
function ShopUI:openConfirm(index)
    local ware = WARES[index]
    if ware.price > self.gold then return end
    self.pending = ware
    self.model.pendingName = ware.name
    self.model.pendingIcon = ware.icon
    self.model.pendingPrice = ware.price
    self.model.qty = 1
    byId(self.doc, "confirm").classList:add("open")
    byId(self.doc, "confirm-buy"):focus()
end

function ShopUI:closeConfirm()
    self.pending = nil
    byId(self.doc, "confirm").classList:remove("open")
end

---@param qty integer
function ShopUI:setQuantity(qty)
    if not self.pending then return end
    local most = math.floor(self.gold / self.pending.price)
    self.model.qty = math.max(1, math.min(qty, most))
end

function ShopUI:confirm()
    local ware, qty = self.pending, self.model.qty
    if not ware or ware.price * qty > self.gold then return end
    self.gold = self.gold - ware.price * qty
    self:closeConfirm()
    Log.print(string.format("shop: bought %s x%d", ware.id, qty))

    -- count the purse down: anime animates a plain table, the model shows its value
    local purse = { value = self.model.gold }
    anime.animate(purse, {
        value = self.gold,
        duration = 600,
        ease = "outQuad",
        modifier = function(value) return math.floor(value + 0.5) end,   -- whole coins
        onUpdate = function() self.model.gold = purse.value end,
    })
end

return ShopUI
