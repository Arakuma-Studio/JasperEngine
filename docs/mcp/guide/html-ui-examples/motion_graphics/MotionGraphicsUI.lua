-- A deterministic six-second composition. Attach to motion_graphics.html's UI Document.
-- All geometry is a pure function of timeline.currentTime and the seed; seek() can run it
-- forwards or backwards without accumulating error. Profiler measures the procedural callback.
---@class MotionGraphicsUI : ScriptInstance
---@field doc UIDocument
---@field timeline AnimeTimeline
---@field split AnimeTextSplitter
local MotionGraphicsUI = { properties = {} }

---@param doc UIDocument
---@param id string
---@return UIElement
local function byId(doc, id)
    local element = doc:getElementById(id)
    assert(element, "motion_graphics.html has no element #" .. id)
    return element
end

function MotionGraphicsUI:init()
    self.doc = UIDocument(self.scene, self.entity)
    RegisterEvent(self, self.doc.onLoad, "onDocumentLoad")
    self:onDocumentLoad()
end

function MotionGraphicsUI:onDocumentLoad()
    if self.timeline then self.timeline:cancel() end
    if self.split then self.split:revert() end

    local doc = self.doc
    local random = anime.utils.createSeededRandom(2718)
    self.stars = {}
    for i = 1, 30 do
        self.stars[i] = {
            x = random(35, 1045), y = random(45, 1305),
            radius = random(3, 8), phase = random(0, 6283) / 1000,
        }
    end
    self.starPath = byId(doc, "stars")
    self.split = anime.splitText(byId(doc, "headline"), {
        chars = { wrap = "clip" }, words = true, lines = { wrap = "clip" }, accessible = true,
    })

    local logo = byId(doc, "logo")
    local drawable = anime.createDrawable(logo)
    local motion = anime.createMotionPath(byId(doc, "orbit"))
    local timeline = anime.createTimeline({
        defaults = { ease = "outCubic" },
        onRender = function(animation)
            Profiler.sample("motion_graphics.procedural", function()
                self:drawStars(animation.currentTime / 1000)
            end)
        end,
    })
    self.timeline = timeline
    timeline:add(self.split.chars, {
        translateY = { from = "110%", to = "0%" }, duration = 850,
        delay = anime.stagger(38), ease = "cubicBezier(.16,1,.3,1)",
    }, 0)
    timeline:add(byId(doc, "deck"), { opacity = { from = 0, to = 1 }, duration = 700 }, 650)
    timeline:add(drawable, { draw = "0 1", duration = 1400, ease = "inOutSine" }, 1100)
    timeline:add(logo, {
        fill = { from = "rgba(230,255,104,0)", to = "rgba(230,255,104,1)" },
        duration = 700, ease = "linear",
    }, 2600)
    timeline:add(byId(doc, "morph"), {
        d = anime.morphTo(byId(doc, "morph-target")), duration = 1800, ease = "inOutSine",
    }, 1900)
    timeline:add(byId(doc, "follower"), {
        translateX = motion.translateX, translateY = motion.translateY, rotate = motion.rotate,
        duration = 4200, ease = "linear",
    }, 900)
    -- This last child fixes the composition's duration, including its quiet final hold.
    timeline:add({ time = 0 }, { time = 6, duration = 6000, ease = "linear" }, 0)
    self:drawStars(0)
end

---@param seconds number
function MotionGraphicsUI:drawStars(seconds)
    local commands = {}
    for _, star in ipairs(self.stars) do
        local x = star.x + 14 * math.sin(seconds * 0.45 + star.phase)
        local y = star.y + 10 * math.cos(seconds * 0.35 + star.phase)
        local radius = star.radius * (1 + 0.18 * math.sin(seconds * 1.4 + star.phase))
        for point = 0, 7 do
            local angle = point * math.pi / 4 + seconds * 0.08
            local r = point % 2 == 0 and radius or radius * 0.28
            commands[#commands + 1] = string.format("%s%.3f %.3f", point == 0 and "M" or "L",
                x + math.cos(angle) * r, y + math.sin(angle) * r)
        end
        commands[#commands + 1] = "Z"
    end
    self.starPath:setAttribute("d", table.concat(commands, " "))
end

return MotionGraphicsUI
