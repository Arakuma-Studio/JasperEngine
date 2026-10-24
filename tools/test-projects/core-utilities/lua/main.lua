-- S09's test project: Lua's log, a Timer on the game clock and a coroutine (main.cpp says where it runs)
Log.warn("[gate] lua warning 1")
local started = Engine.gameTime
local timerFired = false
Timer.after(0.1, function()
    timerFired = true
    Log.warn(string.format("[gate] timer fired after %.2f s", Engine.gameTime - started))
end)
-- resumes once the timer ran: the order of the two lines does not depend on frame times
Timer.run(function()
    Timer.waitFrames(2)
    Timer.waitUntil(function() return timerFired end)
    Log.print("[gate] coroutine resumed")
end)
