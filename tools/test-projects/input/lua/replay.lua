-- The Replay part of the input test project (docs/specs/input-recorder-and-input-scripts.md AC12,
-- AC13): a menu (assets/ui/menu.html) whose #play button logs "[gate] clicked", and a math.random
-- spawner a replay's seed repeats. assets/tests/Input/menu.input.json taps through it:
--   web:     ?input-script=tests/Input/menu.input.json&quit-after-input (GATE=input-script), or the
--            same file fetched as a URL (GATE=input-script-url, R30)
--   Android: debug.jasperengine.args=--input-script tests/Input/menu.input.json --quit-after-input
-- Both expect "[gate] clicked", "[input] done" and exit code 0.
-- made at the first update: main.lua runs before the game's init sets a scene (Engine.getScene() is nil)
local menu = nil

MenuReady = false
Clicked = false
local spawned = 0

Engine.onUpdate:add("replay", function()
    if not menu then
        menu = UIDocument(Engine.getScene())
        menu.filename = "ui/menu.html"
    end
    if not MenuReady and menu.loaded then
        local play = menu:getElementById("play")
        if play then
            play:addEventListener("click", function()
                if not Clicked then Log.print("[gate] clicked") end
                Clicked = true
            end)
            MenuReady = true
            Log.print("[gate] menu ready")
        end
    end
    -- the spawner: what it rolls is the seed's
    if math.random(1, 100) == 1 then spawned = spawned + 1 end
    InputRecording.checkpoint("spawned", spawned)
end)
