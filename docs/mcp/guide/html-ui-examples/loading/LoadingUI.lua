-- Loading screen logic: attach to the GameObject that has the UI Document (loading.html).
-- The bar fills over the "duration" property (seconds of game time; a real game sets
-- self.model.progress from its own loading). Tips change every 4 seconds with a cross-fade.
-- At 100% it logs "loading: done" and waits for any key or click: "loading: continue".
---@class LoadingUI : ScriptInstance
local LoadingUI = {
    properties = {
        { name = "duration", displayName = "Duration", type = "float", default = 3.0 },
    }
}

local TIP_SECONDS = 4
local TIPS = {
    "Hold Shift to sprint; your stamina comes back faster while you crouch.",
    "Air pockets glow faintly blue. Follow the bubbles when you are lost.",
    "Rare items have a blue frame and epic ones a purple glow.",
    "You can rebind every key in Settings, under Controls.",
}

---@param doc UIDocument
---@param id string
---@return UIElement
local function byId(doc, id)
    local element = doc:getElementById(id)
    assert(element, "loading.html has no element #" .. id)
    return element
end

function LoadingUI:init()
    self.doc = UIDocument(self.scene, self.entity)
    -- Data model first: the document reloads once to bind it
    self.model = self.doc:createDataModel("loading", { progress = 0 })
    self.elapsed = 0
    self.tipElapsed = 0
    self.tip = 1
    self.done = false
    self.continued = false

    self.onClick = function() self:continue() end
    RegisterEngineEvent(self, "onUpdate")
    RegisterEngineEvent(self, "onKeyDown")
    RegisterEvent(self, self.doc.onLoad, "onDocumentLoad")
    self:onDocumentLoad()
end

-- After the first load and every reload
function LoadingUI:onDocumentLoad()
    byId(self.doc, "loading"):addEventListener("click", self.onClick)
    byId(self.doc, "tip-text").textContent = TIPS[self.tip]
end

function LoadingUI:onUpdate()
    local dt = Engine.deltatime
    if not self.done then
        self.elapsed = self.elapsed + dt
        local progress = math.min(100, math.floor(self.elapsed / math.max(self.duration, 0.01) * 100))
        if progress ~= self.model.progress then self.model.progress = progress end
        if progress >= 100 then
            self.done = true
            Log.print("loading: done")
        end
    end

    self.tipElapsed = self.tipElapsed + dt
    if self.tipElapsed >= TIP_SECONDS then
        self.tipElapsed = 0
        self:nextTip()
    end
end

-- Fade the tip out, change it, fade it back in
function LoadingUI:nextTip()
    self.tip = self.tip % #TIPS + 1
    local tip = byId(self.doc, "tip")
    anime.animate(tip, {
        opacity = 0,
        duration = 250,
        ease = "inQuad",
        onComplete = function()
            byId(self.doc, "tip-text").textContent = TIPS[self.tip]
            anime.animate(tip, { opacity = 1, duration = 250, ease = "outQuad" })
        end,
    })
end

function LoadingUI:onKeyDown(key, isRepeat, mods)
    self:continue()
end

function LoadingUI:continue()
    if self.done and not self.continued then
        self.continued = true
        Log.print("loading: continue")
    end
end

return LoadingUI
