-- capture_contact_sheet's determinism fixture (S48 AC23): an unseeded random start, so two fresh
-- loads of the scene put the sprite in different places (the RNG goes on between them); two draws
-- each way, so two loads agree by chance 1 time in 441, not 1 in 49
local RandomStart = {}

function RandomStart:init()
    local go = GameObject(self.scene, self.entity)
    go:setPosition(math.random(-24, 24), math.random(-4, 4))
end

return RandomStart
