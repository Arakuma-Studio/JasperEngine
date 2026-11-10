-- T6's test project: the same dig from Lua works and reports (main.cpp watches the collision swap)
Timer.after(1.0, function()
    local scene = Engine.scene
    local vol = VoxelVolume.fromGameObject(GameObject(scene, scene:findEntity("Ground")))
    local r = vol:dig(VoxelShape.sphere(Vector3(40, 32, 40), 3), {blend = 0.5})
    Log.print("[gate] voxels lua dig " .. tostring(r.ok))
end)
