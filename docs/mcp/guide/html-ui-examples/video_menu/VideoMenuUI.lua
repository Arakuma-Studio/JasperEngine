-- Video menu logic: attach to the GameObject that has the UI Document (video_menu.html).
-- Clicks log "video_menu: <id>", arrow keys move the focus between the buttons and Enter presses
-- the focused one (a gamepad's d-pad and A do the same). The motion toggle pauses and resumes the
-- background video; its label follows the video's play / pause events, so it stays right
-- whatever paused it. A video that cannot play leaves the gradient: the "no-video" class.
---@class VideoMenuUI : ScriptInstance
local VideoMenuUI = {
    properties = {}
}

local BUTTONS = { "play", "continue", "options", "quit" }

---@param doc UIDocument
---@param id string
---@return UIElement
local function byId(doc, id)
    local element = doc:getElementById(id)
    assert(element, "video_menu.html has no element #" .. id)
    return element
end

function VideoMenuUI:init()
    self.doc = UIDocument(self.scene, self.entity)
    self.focused = 0   -- index in BUTTONS of the focused button, 0 = none

    -- One function per listener, made once: onDocumentLoad adds the same functions again after a
    -- reload (adding a function twice is ignored), so a handler never runs twice.
    self.onButtonClick = function(event)
        Log.print("video_menu: " .. event.currentTarget.id)
    end
    self.onButtonFocus = function(event)
        for i, id in ipairs(BUTTONS) do
            if id == event.currentTarget.id then self.focused = i end
        end
    end
    self.onButtonBlur = function()
        self.focused = 0
    end
    self.onMotionClick = function()
        self:toggleMotion()
    end
    self.onVideoState = function()
        self:showMotionState()
    end
    self.onVideoError = function(event)
        Log.warn("video_menu: the background video does not play: " .. tostring(event.detail.message))
        self.doc.body.classList:add("no-video")
    end
    self.onKeyDown = function(event)
        self:keyDown(event)
    end

    RegisterEvent(self, self.doc.onLoad, "onDocumentLoad")
    self:onDocumentLoad()
end

-- After the first load and every reload (hot reload rebuilds the elements)
function VideoMenuUI:onDocumentLoad()
    local doc = self.doc
    for _, id in ipairs(BUTTONS) do
        local button = byId(doc, id)
        button:addEventListener("click", self.onButtonClick)
        button:addEventListener("focus", self.onButtonFocus)
        button:addEventListener("blur", self.onButtonBlur)
    end
    byId(doc, "motion"):addEventListener("click", self.onMotionClick)
    local video = byId(doc, "backdrop")
    video:addEventListener("play", self.onVideoState)
    video:addEventListener("pause", self.onVideoState)
    video:addEventListener("error", self.onVideoError)
    doc:addEventListener("keydown", self.onKeyDown)
    self:showMotionState()
end

-- Pause the background (fewer distractions, less decoding) or play it again
function VideoMenuUI:toggleMotion()
    local video = byId(self.doc, "backdrop")
    if video.paused then
        video:play()
    else
        video:pause()
    end
    Log.print("video_menu: background " .. (video.paused and "paused" or "playing"))
end

-- The toggle's icon, label and aria-pressed from the video's state
function VideoMenuUI:showMotionState()
    local paused = byId(self.doc, "backdrop").paused
    local motion = byId(self.doc, "motion")
    motion:setAttribute("aria-pressed", paused and "true" or "false")
    byId(self.doc, "motion-label").textContent = paused and "Play background" or "Pause background"
    motion:querySelector("svg"):setAttribute("data-lucide", paused and "play" or "pause")
end

---@param event UIEvent
function VideoMenuUI:keyDown(event)
    if event.key == "ArrowDown" or event.key == "ArrowUp" then
        local step = event.key == "ArrowDown" and 1 or -1
        local index = self.focused == 0 and 1 or (self.focused - 1 + step) % #BUTTONS + 1
        byId(self.doc, BUTTONS[index]):focus()
        event:preventDefault()
    elseif event.key == "Enter" and self.focused == 0 then
        -- a focused button handles Enter itself (a click); with none focused, Enter plays
        byId(self.doc, "play"):click()
        event:preventDefault()
    end
end

return VideoMenuUI
