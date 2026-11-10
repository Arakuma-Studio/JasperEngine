-- the fixed twin of RandomStart: seeded at every start, so every fresh load is the same (in view,
-- as RandomStart, so the two loads compare a drawn sprite)
local SeededStart = {}

function SeededStart:init()
    math.randomseed(7)
    local go = GameObject(self.scene, self.entity)
    go:setPosition(math.random(8, 24), 8)
end

return SeededStart
