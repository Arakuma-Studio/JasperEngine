-- Behaviour transcript of the upgrade fixture (tools/ci/upgrade_from_main).
-- Every scene's Director counts the scene's frames and prints the gameplay state as lines
--   T|<scene>|<frame>|<key>|<value>|...
-- that an old and a new editor (or player) must print identically. Shared by every script of
-- the game through require: one table for the whole play session.
local T = {
    scene = "",
    frame = 0,        -- frames since the scene's Director started (its onPostUpdate counts them)
    total = 0,        -- frames since the game started
    done = false,     -- the last scene ended its run
    state = { score = 0, pickups = 0, sfx = 0, bursts = 0, pulses = 0, contacts = 0 },
}

-- Autopilot: the game plays its own input schedule (exported players, the web), instead of
-- the keys an agent sends through the editor's send_input.
T.autopilot = (os.getenv ~= nil and os.getenv("UPGRADE_AUTOPILOT") == "1") or Engine.platform == Platform.Web
-- The autopilot also fixes the clock: every frame advances exactly 1/60 s whatever the machine's
-- speed (the editor runs do the same through the play tool's fixed_fps)
if T.autopilot then
    Engine.fixedDeltatime = 1 / 60
end
-- Exported desktop players quit after the last scene when asked to
T.exitAtEnd = os.getenv ~= nil and os.getenv("UPGRADE_EXIT_AT_END") == "1"

local function num(v)
    local s = string.format("%.3f", v)
    if s == "-0.000" then s = "0.000" end
    return s
end

function T.fmt(v)
    local t = type(v)
    if t == "number" then
        if math.type(v) == "integer" then return tostring(v) end
        return num(v)
    elseif t == "boolean" then
        return v and "true" or "false"
    elseif t == "nil" then
        return "nil"
    elseif t == "string" then
        return v
    end
    -- Vector2 / Vector3 / Vector4 / Quaternion userdata
    local ok, x = pcall(function() return v.x end)
    if ok and x ~= nil then
        local parts = { num(v.x), num(v.y) }
        local okz, z = pcall(function() return v.z end)
        if okz and z ~= nil then parts[#parts + 1] = num(z) end
        local okw, w = pcall(function() return v.w end)
        if okw and w ~= nil then parts[#parts + 1] = num(w) end
        return "(" .. table.concat(parts, ",") .. ")"
    end
    return tostring(v)
end

function T.log(key, ...)
    if T.done then return end   -- after the end line: a player left running (the web) adds nothing
    local parts = { "T", T.scene, tostring(T.frame), key }
    for i = 1, select("#", ...) do
        parts[#parts + 1] = T.fmt((select(i, ...)))
    end
    print(table.concat(parts, "|"))
end

-- A line on a timeline of its own: frames counted from when something started (a clip, a
-- model that loads asynchronously), so the scene frame it started on stays out of the line
function T.logAt(timeline, frame, key, ...)
    if T.done then return end
    local parts = { "T", T.scene, timeline .. "+" .. tostring(frame), key }
    for i = 1, select("#", ...) do
        parts[#parts + 1] = T.fmt((select(i, ...)))
    end
    print(table.concat(parts, "|"))
end

function T.enter(scene)
    T.scene = scene
    T.frame = 0
    T.log("enter", T.autopilot)
end

-- Called once per frame by the scene's Director, from onPostUpdate (after every onUpdate)
function T.step()
    T.frame = T.frame + 1
    T.total = T.total + 1
end

function T.finish()
    T.log("end", T.total, T.state.score)
    T.done = true
    if T.exitAtEnd then
        io.stdout:flush()
        os.exit(0)
    end
end

-- Input: the real keys, or the autopilot's schedule of the scene ({key, from, to} frames)
-- (public: the editor-driven runs read it to send the same keys through send_input)
T.schedules = {}
function T.schedule(scene, list)
    T.schedules[scene] = list
end

function T.held(key)
    if not T.autopilot then
        return Input.isKeyPressed(key)
    end
    for _, s in ipairs(T.schedules[T.scene] or {}) do
        if s[1] == key and T.frame >= s[2] and T.frame < s[3] then return true end
    end
    return false
end

-- True on the first frame a key is held (scripts keep no per-key state)
local previous = {}
function T.pressed(key)
    local now = T.held(key)
    local id = T.scene .. ":" .. tostring(key)
    local was = previous[id]
    previous[id] = now
    return now and not was
end

return T
