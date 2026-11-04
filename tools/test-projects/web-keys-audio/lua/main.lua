-- the platform-lifecycle web gate (main.cpp says where it runs): every key the engine gets
Engine.onKeyDown:add("gate", function(key, isRepeat, mods)
    Log.print("[gate] key " .. key)
end)
