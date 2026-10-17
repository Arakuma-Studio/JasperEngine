-- SLX_SCENE=<scene name> in the environment loads that scene instead of this one, so the
-- export check (shader-library-export/check.py) screenshots every scene from one build.
local SceneSelect = { properties = {} }

function SceneSelect:init()
    local name = os.getenv("SLX_SCENE")
    if name and name ~= "" and name ~= "Lit" then
        SceneManager.loadScene(name)
    end
end

return SceneSelect
