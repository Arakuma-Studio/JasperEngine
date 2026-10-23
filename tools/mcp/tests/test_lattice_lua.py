"""Lattice from Lua (spec 2026-10-23-lattice-deformer-design.md §6, §8): the Lattice object and
LatticeSystem driven by a script while playing, results read back through console prints.

Scene LatticeLua (3D): an empty "Driver" with the LatticeProbe script. Its init creates a
lattice around a box, binds the box, sets and reads offsets (index and flat), point positions
(local and world), saves a pose, resamples the resolution, adds a follower on the cage's
upper half and tweens to the pose. Its onUpdate then runs phases: the tween ends exactly on
the pose and the follower has moved up; a poked jiggle moves, settles and sleeps (no
dynamic offsets left); a jump of the cage wakes it through its acceleration and it sleeps
again; back at rest the follower returns to its rest. Each step prints `LATTICE <key> <value>`.

Edit mode (eval_lua with allow_edit_mode): applyPose shows at once, a tween does not advance.
"""

import json

from harness import Editor

EDITOR = {"create": True}
TIMEOUT = 900

PROBE = r"""---@class LatticeProbe : ScriptInstance
local LatticeProbe = { properties = {} }

local function v3(v)
    return string.format("[%.6f,%.6f,%.6f]", v.x, v.y, v.z)
end

local function out(key, value)
    print("LATTICE " .. key .. " " .. tostring(value))
end

function LatticeProbe:init()
    self.time = 0
    self.phase = "tween"
    self.phaseTime = 0
    self.jiggleMax = 0

    local cage = Lattice(self.scene)
    cage.name = "ProbeCage"
    cage.position = Vector3(0, 1, 0)
    cage.scale = Vector3(2, 2, 2)
    cage:updateTransform()
    self.cage = cage

    self.box = Primitive(self.scene)
    self.box.name = "ProbeBox"
    self.box:createBox(1, 1, 1)
    self.box.position = Vector3(0, 1, 0)

    out("bind", cage:bind(self.box))
    out("bindAgain", cage:bind(self.box.entity, 0.5))
    local modifier = cage:getModifierComponent(self.box.entity)
    out("bindings", #modifier.bindings)
    out("bindingWeight", string.format("%.3f", modifier.bindings[1].weight))
    out("bindSelf", cage:bind(cage))

    out("points", cage:getPointCount())
    out("resolution", v3(cage:getResolution()))
    cage:setOffset(1, 2, 1, Vector3(0, 0.25, 0))
    out("offset", v3(cage:getOffset(1, 2, 1)))
    out("index", cage:getPointIndex(1, 2, 1))
    out("offsetAt", v3(cage:getOffsetAt(cage:getPointIndex(1, 2, 1))))
    out("rest", v3(cage:getRestPosition(1, 2, 1)))
    out("pointPosition", v3(cage:getPointPosition(1, 2, 1)))
    out("pointWorld", v3(cage:getPointWorldPosition(1, 2, 1)))
    cage:setPointWorldPosition(0, 0, 0, Vector3(-1, -0.2, -1))
    out("setWorld", v3(cage:getOffset(0, 0, 0)))
    cage:setOffset(0, 0, 0, Vector3(0, 0, 0))
    out("outOfRange", v3(cage:getOffset(3, 0, 0)))

    cage:savePose("up")
    out("hasPose", cage:hasPose("up"))
    cage:applyPose("up", 0.5)
    out("halfPose", v3(cage:getOffset(1, 2, 1)))

    -- resampling keeps the deformation: (1, 2, 1) of 3^3 is (2, 4, 2) of 5^3
    cage:setResolution(5, 5, 5)
    out("resampledPoints", cage:getPointCount())
    out("resampled", v3(cage:getOffset(2, 4, 2)))
    cage:resetOffsets()
    out("reset", v3(cage:getOffset(2, 4, 2)))
    cage:applyPose("up")
    out("poseResampled", v3(cage:getOffset(2, 4, 2)))
    cage:resetOffsets()

    -- the follower sits where the rest (inside the cage's upper half) ends up
    self.rest = GameObject(self.scene)
    self.rest.name = "ProbeRest"
    self.rest.position = Vector3(0.1, 1.5, 0)
    self.follower = GameObject(self.scene)
    self.follower.name = "ProbeFollower"
    out("follow", cage:addFollower(self.follower, self.rest))
    out("followItself", cage:addFollower(self.rest, self.rest))
    out("followerComponent", cage:getFollowerComponent(self.follower.entity) ~= nil)
    out("deformRest", v3(cage:deformPoint(Vector3(0.1, 1.5, 0))))

    out("tweenMissing", cage:tweenToPose("nope", 0.3))
    out("tween", cage:tweenToPose("up", 1.0, EaseType.QUAD_OUT))
    out("tweening", cage:isTweening())

    RegisterEngineEvent(self, "onUpdate")
end

function LatticeProbe:next(phase)
    self.phase = phase
    self.phaseTime = 0
end

function LatticeProbe:onUpdate()
    local dt = Engine.deltatime
    self.time = self.time + dt
    self.phaseTime = self.phaseTime + dt
    local cage = self.cage

    if self.phase == "tween" then
        if cage:isTweening() then
            if self.phaseTime > 0.2 and not self.midTween then
                self.midTween = true
                out("midTween", v3(cage:getOffset(2, 4, 2)))
            end
            if self.phaseTime > 5 then out("tweenTimeout", self.phaseTime); self:next("done") end
            return
        end
        out("tweenDone", v3(cage:getOffset(2, 4, 2)))
        out("tweenTime", string.format("%.3f", self.phaseTime))
        self:next("follow")
    elseif self.phase == "follow" then
        -- the follower moved this frame (followers run once transforms are current)
        if self.phaseTime < 0.05 then return end
        out("follower", v3(self.follower:getWorldPosition()))
        out("deformed", v3(cage:deformPoint(Vector3(0.1, 1.5, 0))))
        cage:setJiggleStiffness(80)
        cage:setJiggleDamping(8)
        cage:setJiggle(true)
        out("sleepingBefore", cage:isJiggleSleeping())
        cage:poke(2, 2, 2, Vector3(0, 2, 0))
        out("awake", not cage:isJiggleSleeping())
        self:next("jiggle")
    elseif self.phase == "jiggle" then
        local j = cage:getJiggleOffset(2, 2, 2):length()
        if j > self.jiggleMax then self.jiggleMax = j end
        if cage:isJiggleSleeping() then
            out("jiggleMax", string.format("%.6f", self.jiggleMax))
            out("slept", string.format("%.3f", self.phaseTime))
            out("dynamicAfterSleep", #cage:getLatticeComponent().dynamicOffsets)
            out("offsetAfterJiggle", v3(cage:getOffset(2, 4, 2)))
            self.jiggleMax = 0
            self:next("settle")
        elseif self.phaseTime > 10 then
            out("jiggleTimeout", string.format("%.6f", j)); self:next("done")
        end
    elseif self.phase == "settle" then
        -- a few still frames, then a jump: the acceleration wakes it
        if self.phaseTime < 0.2 then return end
        out("stillSleeping", cage:isJiggleSleeping())
        cage.position = Vector3(0.3, 1, 0)
        self:next("jump")
    elseif self.phase == "jump" then
        local j = cage:getJiggleOffset(2, 2, 2):length()
        if j > self.jiggleMax then self.jiggleMax = j end
        if not cage:isJiggleSleeping() then self.wokeByJump = true end
        if self.wokeByJump and cage:isJiggleSleeping() then
            out("jumpWoke", true)
            out("jumpMax", string.format("%.6f", self.jiggleMax))
            cage.position = Vector3(0, 1, 0)
            cage:setJiggle(false)
            cage:tweenToRest(0)
            self:next("rest")
        elseif self.phaseTime > 10 then
            out("jumpTimeout", tostring(self.wokeByJump)); self:next("done")
        end
    elseif self.phase == "rest" then
        if self.phaseTime < 0.05 then return end
        out("tweeningAtRest", cage:isTweening())
        out("restOffset", v3(cage:getOffset(2, 4, 2)))
        out("followerAtRest", v3(self.follower:getWorldPosition()))
        out("removeFollower", cage:removeFollower(self.follower))
        out("unbind", cage:unbind(self.box))
        out("unbindAgain", cage:unbind(self.box.entity))
        self:next("done")
    elseif self.phase == "done" then
        if not latticeProbeDone then
            out("done", true)
            latticeProbeDone = true
        end
    end
end

return LatticeProbe
"""

S: dict = {}


async def setup(ed: Editor) -> dict:
    """The scene, once per editor."""
    if S.get("ed") is ed:
        return S
    S.clear()
    S["ed"] = ed
    made = await ed.call("create_scene", name="LatticeLua", type="3D", path="scenes/LatticeLua")
    S["scene"] = made["scene"]["id"]
    ent = await ed.call("create_entity", scene=S["scene"], type="empty", name="Driver")
    await ed.call("create_script", kind="lua", name="LatticeProbe", content=PROBE, scene=S["scene"],
                  entity=ent["entity_id"])
    await ed.call("save_scene", scene=S["scene"])
    await ed.call("save_project")
    return S


def lattice_lines(entries: list[dict]) -> dict:
    """`LATTICE <key> <value>` console lines -> {key: value} (JSON values parsed)."""
    values = {}
    for entry in entries:
        message = entry.get("message", "")
        at = message.find("LATTICE ")
        if at < 0:
            continue
        parts = message[at + len("LATTICE "):].strip().split(" ", 1)
        key, raw = parts[0], (parts[1] if len(parts) > 1 else "")
        try:
            values[key] = json.loads(raw)
        except ValueError:
            values[key] = raw
    return values


def near(a, b, eps=1e-4) -> bool:
    if isinstance(a, list):
        return isinstance(b, list) and len(a) == len(b) and all(abs(x - y) <= eps for x, y in zip(a, b))
    return abs(a - b) <= eps


async def test_lattice_from_lua_in_play(ed: Editor):
    s = await setup(ed)
    await ed.console_since(None, limit=1)
    started = await ed.call("play", action="start", scene=s["scene"])
    ed.check_eq(started["state"], "playing", "play starts")
    waited = await ed.call_raw("wait", {"until": "latticeProbeDone == true", "timeout_seconds": 60})
    ed.check(not waited.is_error, f"the probe finished: {waited.text[:1500]}")

    entries = await ed.console_since(None, limit=1000)
    v = lattice_lines(entries)
    errors = [e.get("message", "") for e in entries if e.get("type") == "error"]

    # bindings
    ed.check_eq(v.get("bind"), True, f"bind: {v}")
    ed.check_eq(v.get("bindAgain"), True, "bind again updates the binding")
    ed.check_eq(v.get("bindings"), 1, "one binding per lattice")
    ed.check_eq(v.get("bindingWeight"), 0.5, "the second bind set the weight")
    ed.check_eq(v.get("bindSelf"), False, "a lattice does not bind itself")

    # offsets and positions
    ed.check_eq(v.get("points"), 27, "3 x 3 x 3 points")
    ed.check(near(v.get("resolution"), [3, 3, 3]), f"resolution {v.get('resolution')}")
    ed.check(near(v.get("offset"), [0, 0.25, 0]), f"offset {v.get('offset')}")
    ed.check_eq(v.get("index"), 1 + 3 * (2 + 3 * 1), "flat index i + rx * (j + ry * k)")
    ed.check(near(v.get("offsetAt"), [0, 0.25, 0]), "offset by flat index")
    ed.check(near(v.get("rest"), [0, 0.5, 0]), f"rest position {v.get('rest')}")
    ed.check(near(v.get("pointPosition"), [0, 0.75, 0]), f"local point position {v.get('pointPosition')}")
    ed.check(near(v.get("pointWorld"), [0, 2.5, 0]), f"world point position {v.get('pointWorld')}")
    ed.check(near(v.get("setWorld"), [0, -0.1, 0]), f"setPointWorldPosition -> offset {v.get('setWorld')}")
    ed.check(near(v.get("outOfRange"), [0, 0, 0]), "out of range reads zero")
    ed.check(any("outside the resolution" in m for m in errors), f"out of range is logged: {errors}")

    # poses and resampling
    ed.check_eq(v.get("hasPose"), True, "pose saved")
    ed.check(near(v.get("halfPose"), [0, 0.125, 0]), f"applyPose weight 0.5: {v.get('halfPose')}")
    ed.check_eq(v.get("resampledPoints"), 125, "5 x 5 x 5")
    ed.check(near(v.get("resampled"), [0, 0.125, 0]), f"resampled offset {v.get('resampled')}")
    ed.check(near(v.get("reset"), [0, 0, 0]), "resetOffsets")
    ed.check(near(v.get("poseResampled"), [0, 0.25, 0]), f"the pose was resampled with the lattice: {v.get('poseResampled')}")

    # followers
    ed.check_eq(v.get("follow"), True, "addFollower")
    ed.check_eq(v.get("followItself"), False, "a follower cannot be its own rest")
    ed.check_eq(v.get("followerComponent"), True, "getFollowerComponent")
    ed.check(near(v.get("deformRest"), [0.1, 1.5, 0]), f"at rest deformPoint is the identity: {v.get('deformRest')}")

    # tween
    ed.check_eq(v.get("tweenMissing"), False, "tween to a missing pose")
    ed.check_eq(v.get("tween"), True, "tween started")
    ed.check_eq(v.get("tweening"), True, "isTweening")
    mid = v.get("midTween")
    ed.check(isinstance(mid, list) and 0.0 < mid[1] < 0.25, f"mid tween offset between rest and pose: {mid}")
    ed.check_eq(v.get("tweenDone"), [0.0, 0.25, 0.0], "an ended tween leaves the exact target")
    ed.check("tweenTimeout" not in v, "tween ended")
    ed.check(0.8 <= float(v.get("tweenTime", 0)) < 3.0, f"tween took about 1 s: {v.get('tweenTime')}")

    # follower moved up with the cage's top, and agrees with deformPoint
    follower, deformed = v.get("follower"), v.get("deformed")
    ed.check(isinstance(follower, list) and follower[1] > 1.5 + 0.01, f"the follower moved up: {follower}")
    ed.check(near(follower, deformed, 1e-3), f"follower {follower} == deformPoint(rest) {deformed}")

    # jiggle
    ed.check_eq(v.get("sleepingBefore"), True, "jiggle starts asleep")
    ed.check_eq(v.get("awake"), True, "poke wakes it")
    ed.check("jiggleTimeout" not in v, f"jiggle settled: {v.get('jiggleTimeout')}")
    ed.check(float(v.get("jiggleMax", 0)) > 0.01, f"the poked point moved: {v.get('jiggleMax')}")
    ed.check_eq(v.get("dynamicAfterSleep"), 0, "asleep: no dynamic offsets left")
    ed.check_eq(v.get("offsetAfterJiggle"), [0.0, 0.25, 0.0], "jiggle leaves the authored offsets alone")
    ed.check_eq(v.get("stillSleeping"), True, "a still lattice stays asleep")
    ed.check("jumpTimeout" not in v, f"the jump woke it and it slept again: {v.get('jumpTimeout')}")
    ed.check_eq(v.get("jumpWoke"), True, "acceleration wakes the jiggle")
    ed.check(float(v.get("jumpMax", 0)) > 1e-3, f"the jump moved the points: {v.get('jumpMax')}")

    # back at rest
    ed.check_eq(v.get("tweeningAtRest"), False, "tweenToRest(0) is immediate")
    ed.check_eq(v.get("restOffset"), [0.0, 0.0, 0.0], "rest")
    ed.check(near(v.get("followerAtRest"), [0.1, 1.5, 0], 1e-4), f"follower back at its rest: {v.get('followerAtRest')}")
    ed.check_eq(v.get("removeFollower"), True, "removeFollower")
    ed.check_eq(v.get("unbind"), True, "unbind")
    ed.check_eq(v.get("unbindAgain"), False, "nothing left to unbind")
    ed.check_eq(v.get("done"), True, "probe done")

    unexpected = [m for m in errors if "outside the resolution" not in m and "no pose named" not in m
                  and "cannot be its own rest" not in m and "cannot deform itself" not in m]
    ed.check(not unexpected, f"unexpected errors: {unexpected}")

    stopped = await ed.call("play", action="stop")
    ed.check_eq(stopped["state"], "stopped", "stop")
    await ed.check_alive()


async def test_lattice_edit_mode(ed: Editor):
    """Edit mode: offsets and poses show at once (followers too); tweens wait for Play."""
    s = await setup(ed)
    await ed.call("open_scene", scene=s["scene"])
    code = """
        local cage = Lattice(scene)
        cage.entityOwned = false
        cage.name = "EditCage"
        cage:setOffset(2, 2, 2, Vector3(0.1, 0, 0))
        cage:savePose("p")
        cage:resetOffsets()
        cage:applyPose("p")
        local applied = cage:getOffset(2, 2, 2).x
        cage:resetOffsets()
        cage:tweenToPose("p", 0.2)
        editCage = cage
        return applied, cage:isTweening()
    """
    applied, tweening = (await ed.call("eval_lua", code=code, allow_edit_mode=True))["values"]
    ed.check(abs(applied - 0.1) < 1e-6, f"applyPose in edit mode: {applied}")
    ed.check_eq(tweening, True, "tween started")
    # editor frames pass: the tween does not run in edit mode
    await ed.call("editor_status")
    await ed.call("screenshot", view="scene")
    after = (await ed.call("eval_lua", code="return editCage:isTweening(), editCage:getOffset(2, 2, 2).x",
                           allow_edit_mode=True))["values"]
    ed.check_eq(after, [True, 0.0], "the tween waits for Play")
    await ed.call("eval_lua", code="GameObject.destroyImmediate(editCage); editCage = nil; return true",
                  allow_edit_mode=True)
    await ed.check_alive()
