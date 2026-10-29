-- S18's test project: 1.5 s in, a knot of the road moves (main.cpp watches the rebuild)
Timer.after(1.5, function()
    local scene = Engine.scene
    local road = GameObject(scene, scene:findEntity("Road")):getSpline()
    road:setKnotPosition(6, Vector3(0, 0, 25))
    Log.print("[gate] knot moved from Lua")
end)
