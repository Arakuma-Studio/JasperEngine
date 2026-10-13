-- Toast logic: attach to the GameObject that has the UI Document (toasts.html).
-- ToastsUI:show(text, kind, seconds) adds a toast (kind info | success | warning | error,
-- seconds of game time, default 3). It slides in, counts down, slides out and is removed.
-- At most 5 are shown: a new one sends the oldest away. From another script:
--   script("Toasts"):show("Saved", "success")   (eval_lua), or a property of type ToastsUI.
---@class ToastsUI : ScriptInstance
local ToastsUI = {
    properties = {}
}

local MAX_TOASTS = 5
local ICONS = { info = "info", success = "circle-check", warning = "triangle-alert", error = "circle-x" }

---@param doc UIDocument
---@param id string
---@return UIElement
local function byId(doc, id)
    local element = doc:getElementById(id)
    assert(element, "toasts.html has no element #" .. id)
    return element
end

function ToastsUI:init()
    self.doc = UIDocument(self.scene, self.entity)
    self.toasts = {}   -- { element, left (seconds), leaving }, oldest first

    RegisterEngineEvent(self, "onUpdate")
    RegisterEvent(self, self.doc.onLoad, "onDocumentLoad")
    self:onDocumentLoad()

    self:show("New quest: The Sunken Archive", "info", 3)
    self:show("Achievement unlocked: Deep Diver", "success", 3)
    self:show("Low oxygen. Find an air pocket", "warning", 3)
end

-- After the first load and every reload: the design samples go, and a reload has made new
-- elements, so the toasts of before are gone too
function ToastsUI:onDocumentLoad()
    for _, sample in ipairs(self.doc:querySelectorAll("[data-sample]")) do
        sample:remove()
    end
    self.toasts = {}
end

---@param text string
---@param kind? "info"|"success"|"warning"|"error"
---@param seconds? number
function ToastsUI:show(text, kind, seconds)
    kind = ICONS[kind or "info"] and kind or "info"
    seconds = seconds or 3

    local toast = self.doc:createElement("div")
    toast.className = "toast toast-" .. kind
    toast.innerHTML = '<i data-lucide="' .. ICONS[kind] .. '"></i><p class="message"></p><div class="toast-progress"></div>'
    toast:querySelector(".message").textContent = text   -- text, never parsed as HTML
    toast:querySelector(".toast-progress").style.animationDuration = seconds .. "s"
    byId(self.doc, "toasts"):appendChild(toast)

    anime.animate(toast, {
        translateX = { from = "120%", to = "0%" },
        opacity = { from = 0, to = 1 },
        duration = 300,
        ease = "outBack",
    })
    table.insert(self.toasts, { element = toast, left = seconds, leaving = false })

    -- too many: the oldest still showing goes
    local showing = 0
    for _, entry in ipairs(self.toasts) do
        if not entry.leaving then showing = showing + 1 end
    end
    for _, entry in ipairs(self.toasts) do
        if showing <= MAX_TOASTS then break end
        if not entry.leaving then
            self:dismiss(entry)
            showing = showing - 1
        end
    end
end

---@param entry table
function ToastsUI:dismiss(entry)
    entry.leaving = true
    anime.animate(entry.element, {
        translateX = "120%",
        opacity = 0,
        duration = 250,
        ease = "inCubic",
        onComplete = function()
            entry.element:remove()
            for i, other in ipairs(self.toasts) do
                if other == entry then table.remove(self.toasts, i) break end
            end
        end,
    })
end

function ToastsUI:onUpdate()
    local dt = Engine.deltatime   -- game time: toasts wait while the game is paused
    for _, entry in ipairs(self.toasts) do
        if not entry.leaving then
            entry.left = entry.left - dt
            if entry.left <= 0 then self:dismiss(entry) end
        end
    end
end

return ToastsUI
