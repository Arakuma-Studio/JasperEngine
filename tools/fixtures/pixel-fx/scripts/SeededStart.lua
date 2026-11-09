-- the fixed twin of RandomStart: seeded at every start, so every fresh load is the same
local SeededStart = {}

function SeededStart:init()
    math.randomseed(7)
    local go = GameObject(self.scene, self.entity)
    go:setPosition(math.random(-12, 12) / 16, 0)
end

return SeededStart
