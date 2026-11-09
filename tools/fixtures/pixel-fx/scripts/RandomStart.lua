-- capture_contact_sheet's determinism fixture (S48 AC23): an unseeded random start, so two fresh
-- loads of the scene put the sprite in different places (the RNG goes on between them)
local RandomStart = {}

function RandomStart:init()
    local go = GameObject(self.scene, self.entity)
    go:setPosition(math.random(-24, 24), 0)
end

return RandomStart
