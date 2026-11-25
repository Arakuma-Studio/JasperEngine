-- tools/test-projects/game-tests: what a player's test run checks (AC12, AC13)
Test.config{ tags = {"smoke"} }

Test.case("The engine runs game frames", function(t)
    _G.LeftByTheFirstCases = true
    Test.waitFrames(10)
    Test.check(t.frame >= 10, "ten game frames")
end)

Test.case("Game time follows the fixed frame rate", function(t)
    Test.waitGameSeconds(0.5)
    Test.near(t.gameTime, 0.5, 0.02, "half a game second")
end)

Test.case("A case starts from a fresh Lua state", function(t)
    Test.check(rawget(_G, "LeftByTheFirstCases") == nil, "a global of an earlier case")
end)

Test.case("Skipped on purpose", {skip = "a skipped case is never a pass"}, function(t)
    Test.fail("never runs")
end)
