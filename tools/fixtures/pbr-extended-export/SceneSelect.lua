-- PBX_SCENE=<scene name> in the environment loads that scene instead of this one, so one
-- exported build shows every scene of the fixture (tools/smoke-test.sh SMOKE_GAME_RUNS).
local SceneSelect = { properties = {} }

function SceneSelect:init()
    local name = os.getenv("PBX_SCENE")
    if name and name ~= "" and name ~= "Lobes" then
        SceneManager.loadScene(name)
    end
end

return SceneSelect
