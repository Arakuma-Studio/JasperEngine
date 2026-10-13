-- HUD logic: attach to the GameObject that has the UI Document (hud.html).
-- The document shows the "hud" data model: change a field and the HUD updates on the next frame.
-- Game API (call it from your scripts, or eval_lua script("HUD"):damage(25)):
--   HudUI:damage(n)   the shield takes it first, then health; the screen flashes red
--   HudUI:fire()      one round less (false when the magazine is empty)
--   HudUI:reload()    refills the magazine from the reserve
-- Set the "demo" property to have it fire, reload and take damage by itself.
---@class HudUI : ScriptInstance
---@field demo boolean
local HudUI = {
    properties = {
        { name = "demo", displayName = "Demo", type = "bool", default = false },
    }
}

local MAGAZINE = 30
local COOLDOWNS = { 8, 12, 5 }   -- seconds per ability (Q, E, R)

-- 12450 -> "12,450"
---@param n integer
---@return string
local function thousands(n)
    local text = tostring(math.floor(n))
    local formatted = text:reverse():gsub("(%d%d%d)", "%1,"):reverse()
    return (formatted:gsub("^,", ""))
end

---@param doc UIDocument
---@param id string
---@return UIElement
local function byId(doc, id)
    local element = doc:getElementById(id)
    assert(element, "hud.html has no element #" .. id)
    return element
end

function HudUI:init()
    self.doc = UIDocument(self.scene, self.entity)
    -- Data model first: the document reloads once to bind it
    self.model = self.doc:createDataModel("hud", {
        health = 100,
        shield = 50,
        score = thousands(12450),   -- text: the number is self.score
        timer = "00:00",
        weapon = "Pulse Rifle",
        ammo = MAGAZINE,
        reserve = 90,
        abilities = { { cooldown = 0 }, { cooldown = 65 }, { cooldown = 0 } },
    })
    self.score = 12450
    self.elapsed = 0          -- game seconds since the start (pauses with the game)
    self.shownSecond = -1
    self.demoClock = 0
    self.cooldownLeft = { 0, COOLDOWNS[2] * 0.65, 0 }

    RegisterEngineEvent(self, "onUpdate")
    RegisterEvent(self, self.doc.onLoad, "onDocumentLoad")
    self:onDocumentLoad()
end

-- After the first load and every reload: the vignette starts hidden
function HudUI:onDocumentLoad()
    byId(self.doc, "damage-vignette").style.opacity = "0"
end

---@param amount number
function HudUI:damage(amount)
    local model = self.model
    local absorbed = math.min(model.shield, amount)
    model.shield = model.shield - absorbed
    model.health = math.max(0, model.health - (amount - absorbed))
    anime.animate(byId(self.doc, "damage-vignette"), {
        opacity = { 0, 0.6, 0 },   -- keyframes: in, then out
        duration = 400,
        ease = "outQuad",
    })
end

---@return boolean fired
function HudUI:fire()
    if self.model.ammo == 0 then return false end
    self.model.ammo = self.model.ammo - 1
    self.score = self.score + 10
    self.model.score = thousands(self.score)
    return true
end

function HudUI:reload()
    local model = self.model
    local taken = math.min(MAGAZINE - model.ammo, model.reserve)
    model.ammo = model.ammo + taken
    model.reserve = model.reserve - taken
end

---@param index integer 1 = Q, 2 = E, 3 = R
function HudUI:useAbility(index)
    self.cooldownLeft[index] = COOLDOWNS[index]
end

function HudUI:onUpdate()
    local dt = Engine.deltatime   -- game time: stops with Engine.timeScale = 0
    self.elapsed = self.elapsed + dt

    -- the timer text changes once a second, so the model is written once a second
    local second = math.floor(self.elapsed)
    if second ~= self.shownSecond then
        self.shownSecond = second
        self.model.timer = string.format("%02d:%02d", second // 60, second % 60)
    end

    for i, left in ipairs(self.cooldownLeft) do
        if left > 0 then
            self.cooldownLeft[i] = math.max(0, left - dt)
            self.model.abilities[i].cooldown = math.ceil(self.cooldownLeft[i] / COOLDOWNS[i] * 100)
        end
    end

    if self.demo then
        self.demoClock = self.demoClock + dt
        if self.demoClock >= 0.5 then
            self.demoClock = 0
            if not self:fire() then self:reload() end
            if math.random() < 0.2 then self:damage(8) end
            if math.random() < 0.1 then self:useAbility(math.random(1, 3)) end
        end
    end
end

return HudUI
