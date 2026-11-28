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
-- the Performance sampler in WebAssembly (release-performance-and-gpu-quirks AC14): 3 s of gameplay,
-- then the summary; the web has no thread clock
Performance.setSampling(true)
Timer.run(function()
    Timer.wait(3)
    local s = Performance.getSummary()
    while s.seconds < 1 and Engine.gameTime - started < 20 do
        Timer.wait(0.5)
        s = Performance.getSummary()
    end
    local p50 = s.fps and s.fps.p50 or 0
    Log.print(string.format("[gate] perf mainThreadCpu=%s p50%s", tostring(s.mainThreadCpu), p50 > 0 and ">0" or "=0"))
end)
