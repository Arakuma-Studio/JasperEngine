-- S93 R2's web gate (main.cpp says where it runs): the mouse presses and releases the game gets
local ups = 0

Engine.onMouseDown:add("gate", function(button, x, y, mods)
    Log.print(string.format("[gate] down %d at %d,%d", button, math.floor(x + 0.5), math.floor(y + 0.5)))
end)

Engine.onMouseUp:add("gate", function(button, x, y, mods)
    ups = ups + 1
    Log.print(string.format("[gate] up %d: button %d at %d,%d", ups, button, math.floor(x + 0.5), math.floor(y + 0.5)))
end)
