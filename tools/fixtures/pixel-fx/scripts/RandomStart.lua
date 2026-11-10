-- capture_contact_sheet's determinism fixture (S48 AC23): an unseeded random start, so two fresh
-- loads of the scene put the sprite in different places. The Lua state and its RNG live through a
-- scene load (LuaBinding makes one state per Play), so each load draws again; a draw equal to the
-- last load's is drawn again, so no two loads in a row can match by chance.
local RandomStart = {}

function RandomStart:init()
    local go = GameObject(self.scene, self.entity)
    local x
    repeat
        x = math.random(-24, 24)
    until x ~= _G.RandomStartLastX
    _G.RandomStartLastX = x
    go:setPosition(x, 0)
end

return RandomStart
