"""Lattice engine runtime review (spec 2026-10-23-lattice-deformer-design.md §6-§8): regressions
found by the engine-side review, each driven from Lua while playing.

- Lattice:addFollower with 3 to 5 arguments (the documented optional applyPosition,
  applyRotation, applyScale) found no Lua overload: LuaBridge matches the exact arity.
- ActionSystem::animationApplyEnd (the end-of-clip fix, every track type) sampled a track whose
  frame has an explicit duration shorter than its keys at that frame's end on the clip's last
  update, while every earlier update samples it at the clip time: a TranslateTracks target
  jumped back from its last key to its value at the frame's end.
- The jiggle kept its position history while its lattice was inactive (or the system paused):
  a lattice moved meanwhile read as a huge acceleration on the frame it came back and was
  kicked into motion although it never accelerated while simulated.
"""

from harness import Editor

EDITOR = {"create": True}
CI = True
TIMEOUT = 600

S: dict = {}


async def setup(ed: Editor) -> dict:
    """An empty 3D scene, playing."""
    if S.get("ed") is not ed:
        S.clear()
        S["ed"] = ed
        made = await ed.call("create_scene", name="LatticeReview", type="3D", path="scenes/LatticeReview")
        S["scene"] = made["scene"]["id"]
        await ed.call("save_scene", scene=S["scene"])
        await ed.call("save_project")
    if (await ed.call("get_play_state"))["state"] != "playing":
        started = await ed.call("play", action="start", scene=S["scene"])
        ed.check_eq(started["state"], "playing", "play starts")
        await ed.call("wait", frames=2)
    return S


async def lua(ed: Editor, code: str):
    return (await ed.call("eval_lua", code=code))["values"]


async def test_add_follower_optional_arguments(ed: Editor):
    await setup(ed)
    code = """
        local cage = Lattice(scene); cage.entityOwned = false; cage.name = "ArityCage"
        local follower = GameObject(scene); follower.entityOwned = false
        local rest = GameObject(scene); rest.entityOwned = false
        local function flags()
            local c = cage:getFollowerComponent(follower.entity)
            if not c then return "none" end
            return tostring(c.applyPosition) .. "," .. tostring(c.applyRotation) .. "," .. tostring(c.applyScale)
        end
        local result = {}
        local ok, value = pcall(function() return cage:addFollower(follower, rest, false) end)
        result.three = tostring(ok) .. ":" .. tostring(value) .. ":" .. flags()
        ok, value = pcall(function() return cage:addFollower(follower.entity, rest.entity, true, false) end)
        result.four = tostring(ok) .. ":" .. tostring(value) .. ":" .. flags()
        ok, value = pcall(function() return cage:addFollower(follower, rest, false, true, true) end)
        result.five = tostring(ok) .. ":" .. tostring(value) .. ":" .. flags()
        ok, value = pcall(function() return cage:addFollower(follower.entity, rest.entity, true, true, false, 0.5) end)
        result.six = tostring(ok) .. ":" .. tostring(value) .. ":" .. flags()
        ok, value = pcall(function() return cage:addFollower(follower, rest) end)
        result.two = tostring(ok) .. ":" .. tostring(value) .. ":" .. flags()

        return result
    """
    got = (await lua(ed, code))[0]
    ed.check_eq(got.get("three"), "true:true:false,true,false", f"addFollower(f, r, applyPosition): {got}")
    ed.check_eq(got.get("four"), "true:true:true,false,false", f"addFollower(f, r, position, rotation): {got}")
    ed.check_eq(got.get("five"), "true:true:false,true,true", f"addFollower(f, r, position, rotation, scale): {got}")
    ed.check_eq(got.get("six"), "true:true:true,true,false", f"addFollower with every argument: {got}")
    ed.check_eq(got.get("two"), "true:true:true,true,false", f"addFollower(f, r): {got}")
    await ed.check_alive()


async def test_clip_end_keeps_tracks_of_short_frames(ed: Editor):
    """Two TranslateTracks over 1 s in one clip; the first frame's explicit duration is 0.5 s.
    Every update samples both at the clip time, so both end on their last key (x = 1)."""
    await setup(ed)
    code = """
        local a = GameObject(scene); a.entityOwned = false; a.name = "EndA"
        local b = GameObject(scene); b.entityOwned = false; b.name = "EndB"
        local ta = TranslateTracks(scene, {0, 1}, {Vector3(0, 0, 0), Vector3(1, 0, 0)}); ta.entityOwned = false
        local tb = TranslateTracks(scene, {0, 1}, {Vector3(0, 0, 0), Vector3(1, 0, 0)}); tb.entityOwned = false
        local anim = Animation(scene); anim.entityOwned = false
        anim:addActionFrame(0, 0.5, ta.entity, a.entity) -- explicit, shorter than its keys
        anim:addActionFrame(0, 0, tb.entity, b.entity)   -- auto: its keys' 1 s
        endA = a; endB = b; endAnim = anim
        anim:start()
        return true
    """
    await lua(ed, code)
    waited = await ed.call_raw("wait", {"until": "endAnim:isStopped()", "timeout_seconds": 30})
    ed.check(not waited.is_error, f"the clip ended: {waited.text[:500]}")
    xa, xb = await lua(ed, "return endA.position.x, endB.position.x")
    ed.check(abs(xb - 1.0) < 1e-5, f"the auto frame's track ends on its last key: {xb}")
    ed.check(abs(xa - 1.0) < 1e-5, f"the short frame's track ends where the clip time put it (its last key), "
                                   f"not back at its frame's end (0.5): {xa}")
    await ed.check_alive()


async def test_jiggle_ignores_motion_while_inactive(ed: Editor):
    """A still lattice moved while inactive comes back asleep: no acceleration was simulated."""
    await setup(ed)
    await lua(ed, """
        jcage = Lattice(scene); jcage.entityOwned = false; jcage.name = "JiggleCage"
        jcage:setJiggle(true)
        return true
    """)
    await ed.call("wait", frames=6)
    before = (await lua(ed, "return jcage:isJiggleSleeping()"))[0]
    ed.check_eq(before, True, "a still lattice sleeps")
    await lua(ed, "jcage:setActive(false); return true")
    await ed.call("wait", frames=2)
    # well under the teleport distance (2 lattice sizes)
    await lua(ed, "jcage.position = Vector3(1.5, 0, 0); return true")
    await ed.call("wait", frames=4)
    await lua(ed, "jcage:setActive(true); return true")
    await ed.call("wait", frames=3)
    sleeping, dynamic = await lua(ed, "return jcage:isJiggleSleeping(), #jcage:getLatticeComponent().dynamicOffsets")
    ed.check_eq([sleeping, dynamic], [True, 0], "back active after moving while inactive: still asleep, nothing to pack")
    # it still reacts to motion it does see
    await lua(ed, "jcage.position = Vector3(0, 0, 0); return true")
    await ed.call("wait", frames=3)
    woke = (await lua(ed, "return jcage:isJiggleSleeping()"))[0]
    ed.check_eq(woke, False, "a jump while active wakes it")
    await ed.call("play", action="stop")
    await ed.check_alive()
