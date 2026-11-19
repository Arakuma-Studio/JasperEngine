-- SaveGame's test project (main.cpp says where it runs): the first start writes slot1 and, once it
-- is stored, a 256 KiB slot; a later start (a reload, a relaunch after a kill) reads slot1 back
local function writeBig()
    SaveGame.write("big", {blob = string.rep("x", 256 * 1024)}, {onComplete = function(ok, err)
        if ok then Log.print("savegame: big ok") else Log.print("savegame: " .. err.code .. " (" .. err.message .. ")") end
    end})
end

if SaveGame.exists("slot1") then
    local data, info = SaveGame.read("slot1")
    if data and data.pos.z == 2 and data.items[5] == "b" and math.type(data.gold) == "integer" then
        Log.print("savegame: read ok gen " .. info.generation)
    else
        Log.print("savegame: read failed (" .. tostring(info and info.message) .. ")")
    end
else
    local ok, err = SaveGame.write("slot1", {gold = 120, pos = Vector3(4, 0, 2), items = {[1] = "a", [5] = "b"}},
        {meta = {title = "Gate"}, onComplete = function(done, e)
            if done then
                Log.print("savegame: written")
                writeBig()
            else
                Log.print("savegame: " .. e.code .. " (" .. e.message .. ")")
            end
        end})
    if not ok then Log.print("savegame: write refused (" .. err.message .. ")") end
end
