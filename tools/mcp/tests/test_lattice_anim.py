"""Lattice animation (spec 2026-10-23-lattice-deformer-design.md §7): LatticeTracks keyframes.

- edit_animation's "lattice" channel: keys as {"<index>": [x,y,z]} and [[i,j,k],[x,y,z]] pairs,
  points a key leaves out filled from the keys around it, merge mode, errors, one undo step;
- "current" keys, the MCP path of the Animation window's keying: the moved points plus the
  track's own, the rest pose as every point, new points given the current offsets in the keys
  that exist;
- play mode: the offsets follow the keys at the sampled times and end exactly on the last key,
  points no key names keep their offsets; the Animation window preview evaluates the clip and
  restores the authored offsets when it stops;
- Lua: LatticeTracks built in a script (constructor and setters), two clips animating different
  points (plus one shared point) crossfaded with fadeOut/fadeIn, an out-of-range point ignored;
- the scene file and a second editor: the tracks reload and re-save byte-identical;
- a screenshot mid-animation shows the bound box deformed.

Runs in a new empty project; the tests share one editor and run in order (state in S).
"""

import json
import math
import sys
from pathlib import Path

from harness import Editor

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import pngdiff  # noqa: E402  (tools/mcp/pngdiff.py)

TIMEOUT = 900
EDITOR = {"create": True}
CI = True

S: dict = {}

ZERO = [0.0, 0.0, 0.0]


# -- helpers --------------------------------------------------------------------


def close(a, b, tol=1e-4):
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        return abs(a - b) <= tol
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(close(x, y, tol) for x, y in zip(a, b))
    if isinstance(a, dict) and isinstance(b, dict):
        return a.keys() == b.keys() and all(close(a[k], b[k], tol) for k in a)
    return a == b


def check_close(ed, actual, expected, what="", tol=1e-4):
    ed.check(close(actual, expected, tol), f"{what}: expected {expected!r}, got {actual!r}")


async def setup(ed: Editor) -> dict:
    """Scene LatticeAnim: a box bound to a 2 x 2 x 2 lattice around it, once per editor."""
    if S.get("ed") is ed:
        await ed.call("select_scene", scene=S["scene"])
        return S
    S.clear()
    S["ed"] = ed
    made = await ed.call("create_scene", name="LatticeAnim", type="3D", path="scenes/LatticeAnim")
    S["scene"] = made["scene"]["id"]
    sid = S["scene"]
    box = (await ed.call("create_entity", scene=sid, type="cube", name="Box", position=[0, 1, 0]))["entity_id"]
    cage = (await ed.call("create_entity", scene=sid, type="empty", name="Cage", position=[0, 1, 0],
                          scale=[2, 2, 2]))["entity_id"]
    await ed.call("add_component", scene=sid, entity=cage, component="lattice")
    await ed.call("add_component", scene=sid, entity=box, component="latticemodifier",
                  values={"latticemodifier": {"bindings": [{"lattice": cage}]}})
    plain = (await ed.call("create_entity", scene=sid, type="empty", name="Plain"))["entity_id"]
    S.update(box=box, cage=cage, plain=plain)
    return S


async def offsets(ed, entity=None, scene=None) -> list:
    got = await ed.call("get_entity", scene=scene or S["scene"], entity=entity or S["cage"], components=["lattice"],
                        properties=["offsets"])
    return got["components"]["lattice"]["offsets"]


async def set_offsets(ed, values: dict):
    """{index: [x,y,z]} through set_properties (offsets[i])."""
    await ed.call("set_properties", scene=S["scene"], entity=S["cage"],
                  values={"lattice": {f"offsets[{i}]": v for i, v in values.items()}})


async def tracks_of(ed, action, scene=None) -> dict:
    """The track's arrays (named, so get_entity does not elide the big ones); easings lowercase."""
    async def read(component, names):
        got = await ed.call("get_entity", scene=scene or S["scene"], entity=action, components=[component],
                            properties=names)
        return got["components"][component]
    kf = await read("keyframetracks", ["times", "easings"])
    lt = await read("latticetracks", ["points", "values"])
    easings = [e.lower() for e in kf["easings"]] if kf.get("easings") else kf.get("easings")
    return {"times": kf["times"], "easings": easings, "points": lt["points"], "values": lt["values"]}


def lattice_action(data: dict, index: int = 0) -> dict:
    action = [a for a in data["actions"] if a.get("kind") == "lattice_track"][index]
    return action


def lines(entries: list[dict], tag: str) -> dict:
    """`<tag> <key> <value>` console lines -> {key: value} (JSON values parsed)."""
    values = {}
    for entry in entries:
        message = entry.get("message", "")
        at = message.find(tag + " ")
        if at < 0:
            continue
        parts = message[at + len(tag) + 1:].strip().split(" ", 1)
        key, raw = parts[0], (parts[1] if len(parts) > 1 else "")
        try:
            values[key] = json.loads(raw)
        except ValueError:
            values[key] = raw
    return values


# -- edit_animation lattice channel ------------------------------------------------


async def test_mcp_lattice_track(ed: Editor):
    s = await setup(ed)
    sid, cage = s["scene"], s["cage"]
    # 4 = (1, 1, 0) and 22 = (1, 1, 2) of the 3 x 3 x 3 lattice: the middle of the -z and +z faces
    r = await ed.call("edit_animation", scene=sid, target=cage, name="Squash", loop=False, autoplay=True, tracks=[{
        "channel": "lattice",
        "keys": [{"time": 0, "points": {"4": [0, 0, 0], "22": [0, 0, 0]}},
                 {"time": 1, "points": [[[1, 1, 0], [0, 0.4, 0]], [[1, 1, 2], [0, -0.4, 0]]]}]}])
    ed.check(r["created"] is True, "a new Animation")
    action = lattice_action(r)
    ed.check_eq(action["target"]["id"], cage, "the track animates the lattice")
    ed.check_eq(len(action["keys"]), 2, "two keys")
    check_close(ed, action["keys"][1]["points"], {"4": [0, 0.4, 0], "22": [0, -0.4, 0]}, "keys read back")
    ed.check_eq(action["keys"][0].get("ease"), "linear", "linear to the next key")
    track = action["action"]["id"]
    t = await tracks_of(ed, track)
    ed.check_eq(t["points"], [4, 22], "sparse points, ascending")
    ed.check_eq(t["times"], [0.0, 1.0], "times")
    check_close(ed, t["values"], [[ZERO, ZERO], [[0, 0.4, 0], [0, -0.4, 0]]], "values[key][n]")
    S.update(squash=r["animation"]["id"], squash_track=track)

    # get_animation shows the lattice keys too
    got = await ed.call("get_animation", scene=sid, entity=S["squash"])
    check_close(ed, lattice_action(got)["keys"][1]["points"], {"4": [0, 0.4, 0], "22": [0, -0.4, 0]}, "get_animation")

    # one undo step: the animation and its track go away together, redo brings them back
    await ed.call("undo", scene=sid)
    await ed.check_error("get_animation", scene=sid, entity=cage, contains="no Animation targets it")
    await ed.call("redo", scene=sid)
    got = await ed.call("get_animation", scene=sid, entity=S["squash"])
    ed.check_eq(lattice_action(got)["action"]["id"], track, "redo restores the same track")


async def test_mcp_fill_merge_and_errors(ed: Editor):
    s = await setup(ed)
    sid, cage = s["scene"], s["cage"]
    # a key that leaves a point out gets it interpolated between the keys naming it (held past
    # the first and last): 4 at t=1 is halfway between 0 and 0.4, 22 at t=0 is held from t=1
    r = await ed.call("edit_animation", scene=sid, target=cage, name="Fill", autoplay=False, tracks=[{
        "channel": "lattice",
        "keys": [{"time": 0, "points": {"4": [0, 0, 0]}},
                 {"time": 1, "points": {"22": [0, -0.2, 0]}, "ease": "quad_in"},
                 {"time": 2, "points": [[4, [0, 0.4, 0]]]}]}])
    fill, track = r["animation"]["id"], lattice_action(r)["action"]["id"]
    t = await tracks_of(ed, track)
    ed.check_eq(t["points"], [4, 22])
    check_close(ed, t["values"], [[ZERO, [0, -0.2, 0]], [[0, 0.2, 0], [0, -0.2, 0]], [[0, 0.4, 0], [0, -0.2, 0]]],
                "filled values")
    ed.check_eq(t["easings"], ["linear", "quad_in"], "per-segment easings")

    # merge: a key at an existing time sets only the points it names; a new time is inserted
    # with the track's own value for the points it leaves out; a new point gets the lattice's
    # current offset (rest here) in the keys that already exist
    await ed.call("edit_animation", scene=sid, animation=fill, tracks=[{
        "channel": "lattice", "mode": "merge",
        "keys": [{"time": 1, "points": {"4": [0, 0.3, 0]}},
                 {"time": 1.5, "points": {"13": [0.1, 0, 0]}}]}])
    t = await tracks_of(ed, track)
    ed.check_eq(t["points"], [4, 22, 13], "a new point column")
    ed.check_eq(t["times"], [0.0, 1.0, 1.5, 2.0], "inserted key")
    check_close(ed, t["values"][1], [[0, 0.3, 0], [0, -0.2, 0], ZERO], "t=1: 4 replaced, 22 kept, 13 current")
    # 4 between 0.3 (t=1) and 0.4 (t=2) through that segment's quad_in: 0.3 + 0.25 * 0.1
    check_close(ed, t["values"][2], [[0, 0.325, 0], [0, -0.2, 0], [0.1, 0, 0]], "t=1.5: 4 sampled from the track")
    check_close(ed, t["values"][0][2], ZERO, "t=0: the new point at its current offset")
    await ed.call("undo", scene=sid)
    t = await tracks_of(ed, track)
    ed.check_eq(t["points"], [4, 22], "undo of the merge")

    # errors name the key and the fix
    await ed.check_error("edit_animation", scene=sid, animation=fill, contains="out of range", tracks=[
        {"channel": "lattice", "keys": [{"time": 0, "points": {"27": [0, 0, 0]}}]}])
    await ed.check_error("edit_animation", scene=sid, animation=fill, contains="outside the 3x3x3", tracks=[
        {"channel": "lattice", "keys": [{"time": 0, "points": [[[3, 0, 0], [0, 0, 0]]]}]}])
    await ed.check_error("edit_animation", scene=sid, animation=fill, contains="keys[1].points", tracks=[
        {"channel": "lattice", "keys": [{"time": 0, "points": {"4": [0, 0, 0]}}, {"time": 1}]}])
    await ed.check_error("edit_animation", scene=sid, animation=fill, contains="flat point indices", tracks=[
        {"channel": "lattice", "keys": [{"time": 0, "points": {"x": [0, 0, 0]}}]}])
    await ed.check_error("edit_animation", scene=sid, target=s["plain"], contains="no Lattice", tracks=[
        {"channel": "lattice", "keys": [{"time": 0, "points": {"4": [0, 0, 0]}}]}])
    t = await tracks_of(ed, track)
    ed.check_eq(t["points"], [4, 22], "refused calls changed nothing")

    # "value" is accepted for the points too
    await ed.call("edit_animation", scene=sid, animation=fill, tracks=[
        {"channel": "lattice", "keys": [{"time": 0, "value": {"5": [0, 0, 0.1]}}]}])
    t = await tracks_of(ed, track)
    ed.check_eq(t["points"], [5], "replace mode with value")
    await ed.call("undo", scene=sid)
    S["fill"] = fill


async def test_current_keys_like_editor_keying(ed: Editor):
    """points: "current" keys the lattice as the Animation window does (no Scene-view selection)."""
    s = await setup(ed)
    sid, cage = s["scene"], s["cage"]
    before = await offsets(ed)
    ed.check(all(close(v, ZERO) for v in before), f"the cage is at rest: {before}")

    # at rest with no track: every point (the rest pose)
    r = await ed.call("edit_animation", scene=sid, target=cage, name="Keyed", autoplay=False, tracks=[
        {"channel": "lattice", "keys": [{"time": 0, "points": "current"}]}])
    keyed, track = r["animation"]["id"], lattice_action(r)["action"]["id"]
    t = await tracks_of(ed, track)
    ed.check_eq(t["points"], list(range(27)), "rest pose: every point")
    check_close(ed, t["values"], [[ZERO] * 27], "all zero")

    # moved points plus the track's own: still every point; the moved ones hold their offsets
    await set_offsets(ed, {4: [0, 0.25, 0], 22: [0, -0.25, 0]})
    await ed.call("edit_animation", scene=sid, animation=keyed, tracks=[
        {"channel": "lattice", "mode": "merge", "keys": [{"time": 1, "points": "current"}]}])
    t = await tracks_of(ed, track)
    ed.check_eq(t["times"], [0.0, 1.0])
    check_close(ed, t["values"][0], [ZERO] * 27, "the rest key is kept")
    ed.check(close(t["values"][1][4], [0, 0.25, 0]) and close(t["values"][1][22], [0, -0.25, 0]), "the moved points")

    # a new track keys only the moved points; a later key adds a newly moved point, which the
    # earlier key gets at its current offset (the only value known for it)
    r = await ed.call("edit_animation", scene=sid, target=cage, name="Moved", autoplay=False, tracks=[
        {"channel": "lattice", "keys": [{"time": 0, "points": "current"}]}])
    moved, mtrack = r["animation"]["id"], lattice_action(r)["action"]["id"]
    t = await tracks_of(ed, mtrack)
    ed.check_eq(t["points"], [4, 22], "the moved points only")
    await set_offsets(ed, {13: [0.1, 0, 0]})
    await ed.call("edit_animation", scene=sid, animation=moved, tracks=[
        {"channel": "lattice", "mode": "merge", "keys": [{"time": 0.5, "points": "current"}]}])
    t = await tracks_of(ed, mtrack)
    ed.check_eq(t["points"], [4, 22, 13], "the newly moved point joins")
    check_close(ed, t["values"], [[[0, 0.25, 0], [0, -0.25, 0], [0.1, 0, 0]]] * 2, "both keys")

    # clean up: the offsets back at rest, the keying animations removed
    await set_offsets(ed, {4: ZERO, 22: ZERO, 13: ZERO})
    for anim in (keyed, moved):
        await ed.call("delete_entities", scene=sid, entities=[anim, lattice_action(
            await ed.call("get_animation", scene=sid, entity=anim))["action"]["id"]])


# -- playback --------------------------------------------------------------------


async def test_preview_evaluates_and_restores(ed: Editor):
    """The Animation window preview (preview_animation) evaluates the clip; stop restores the offsets."""
    s = await setup(ed)
    sid = s["scene"]
    await set_offsets(ed, {13: [0, 0, 0.05]})
    # the field of view the deformation's share of the view was measured with (0.75 rad, before
    # S26's 60 degree default made the box smaller in the shot)
    await ed.call("set_scene_camera", scene=sid, target=[0, 1, 0], distance=5, orbit={"yaw": 30, "pitch": -20},
                  camera_settings={"fov": math.degrees(0.75)})
    r = await ed.call("preview_animation", scene=sid, animation=S["squash"], action="seek", time=0)
    ed.check(r["previewing"] is True, f"previewing: {r}")
    rest = await ed.screenshot("scene", path=ed.scratch.root / "anim_rest.png" if ed.scratch else None)
    await ed.call("preview_animation", scene=sid, animation=S["squash"], action="seek", time=0.5)
    off = await offsets(ed)
    # the seek steps 1/60 s at a time: the last step samples up to one step before 0.5
    ed.check(0.4 * (0.5 - 2 / 60) - 1e-4 <= off[4][1] <= 0.2 + 1e-4, f"point 4 at 0.5 s: {off[4]}")
    ed.check(close(off[22], [0, -off[4][1], 0]), f"point 22 mirrors it: {off[22]}")
    check_close(ed, off[13], [0, 0, 0.05], "a point no key names keeps its offset")
    # the GPU deforms the bound box mid-animation: the Scene view changes (once frames have drawn
    # the seeked offsets)
    await ed.call("wait", frames=5)
    mid = await ed.screenshot("scene", path=ed.scratch.root / "anim_mid.png" if ed.scratch else None)
    same, fraction, _ = pngdiff.compare(pngdiff.decode(rest), pngdiff.decode(mid), max_fraction=0.002)
    ed.check(not same, f"the deformed box shows in the Scene view ({fraction:.4f} of the pixels changed)")
    await ed.call("preview_animation", scene=sid, animation=S["squash"], action="stop")
    off = await offsets(ed)
    check_close(ed, off[4], ZERO, "stop restores the authored offsets")
    check_close(ed, off[22], ZERO, "stop restores the authored offsets")
    check_close(ed, off[13], [0, 0, 0.05], "and keeps the authored ones")
    t = await tracks_of(ed, S["squash_track"])
    check_close(ed, t["values"][1], [[0, 0.4, 0], [0, -0.4, 0]], "the preview leaves the keys alone")


async def test_play_follows_keys(ed: Editor):
    s = await setup(ed)
    sid = s["scene"]
    await ed.call("save_scene", scene=sid)
    started = await ed.call("play", action="start", scene=sid)
    ed.check_eq(started["state"], "playing", "play starts")
    try:
        code = f"""
            local track = object({S["squash_track"]}, "Action")
            local cage = object({S["cage"]}, "Lattice")
            local o4, o22, o13 = cage:getOffsetAt(4), cage:getOffsetAt(22), cage:getOffsetAt(13)
            return track:getTimeCount(), Engine.deltatime, o4.x, o4.y, o4.z, o22.y, o13.z
        """
        samples = []
        for _ in range(40):
            values = (await ed.call("eval_lua", code=code))["values"]
            samples.append(values)
            if values[0] >= 1.2:
                break
            await ed.call("wait", frames=1)
        inside = [v for v in samples if 0.05 < v[0] < 0.95]
        ed.check(len(inside) >= 2, f"samples inside the clip: {samples}")
        for tc, dt, x, y, z, y22, z13 in inside:
            # the offsets of a frame are sampled before the action time advances by that frame's dt
            lo, hi = 0.4 * max(0.0, tc - 1.5 * dt) - 1e-4, 0.4 * tc + 1e-4
            ed.check(lo <= y <= hi, f"point 4 at t={tc:.4f} (dt {dt:.4f}): {y} not in [{lo}, {hi}]")
            ed.check(abs(x) < 1e-6 and abs(z) < 1e-6, f"only y is keyed: {x}, {z}")
            ed.check(abs(y22 + y) < 1e-5, f"point 22 mirrors point 4: {y22} vs {y}")
            ed.check(abs(z13 - 0.05) < 1e-6, f"point 13 keeps its offset: {z13}")
        ys = [v[3] for v in samples]
        ed.check(all(b >= a - 1e-6 for a, b in zip(ys, ys[1:])), f"point 4 rises monotonically: {ys}")
        await ed.call("wait", game_seconds=0.5)
        end = (await ed.call("eval_lua", code=code))["values"]
        ed.check(end[0] >= 1.0 or end[0] == 0.0, f"the clip ended: {end}")
        ed.check(abs(end[3] - 0.4) < 1e-6 and abs(end[5] + 0.4) < 1e-6, f"ends exactly on the last key: {end}")
        shot = await ed.screenshot("game", path=None)
        ed.check(len(shot) > 1000, "game view screenshot")
    finally:
        await ed.call("play", action="stop")
    off = await offsets(ed)
    check_close(ed, off[4], ZERO, "stop reverts the played offsets")


PROBE = r"""---@class LatticeAnimProbe : ScriptInstance
local LatticeAnimProbe = { properties = {} }

local function v3(v)
    return string.format("[%.6f,%.6f,%.6f]", v.x, v.y, v.z)
end

local function out(key, value)
    print("LATANIM " .. key .. " " .. tostring(value))
end

function LatticeAnimProbe:init()
    self.phase = "a"
    self.phaseTime = 0

    local cage = Lattice(self.scene)
    cage.name = "ProbeCage"
    cage:setOffsetAt(13, Vector3(0, 0, 0.05)) -- no track names the centre point
    self.cage = cage

    -- clip A: point 4 rises over 0.5 s; the shared point 10 holds x 0.2; 99 is out of range (ignored)
    self.trackA = LatticeTracks(self.scene, {0, 0.5, 10}, {4, 10, 99}, {
        {Vector3(0, 0, 0), Vector3(0.2, 0, 0), Vector3(1, 1, 1)},
        {Vector3(0, 0.3, 0), Vector3(0.2, 0, 0), Vector3(1, 1, 1)},
        {Vector3(0, 0.3, 0), Vector3(0.2, 0, 0), Vector3(1, 1, 1)}})
    -- clip B through the setters: point 22 sinks; the shared point 10 holds x 0.6
    self.trackB = LatticeTracks(self.scene)
    self.trackB:setTimes({0, 0.5, 10})
    self.trackB:setPoints({22, 10})
    self.trackB:setValues({
        {Vector3(0, 0, 0), Vector3(0.6, 0, 0)},
        {Vector3(0, -0.3, 0), Vector3(0.6, 0, 0)},
        {Vector3(0, -0.3, 0), Vector3(0.6, 0, 0)}})
    self.trackB:setEasing(0, EaseType.QUAD_IN)

    self.animA = Animation(self.scene)
    self.animA:addActionFrame(0, self.trackA.entity, cage.entity)
    self.animB = Animation(self.scene)
    self.animB:addActionFrame(0, self.trackB.entity, cage.entity)
    out("targetA", self.trackA.target == cage.entity)

    self.animA:start()
    RegisterEngineEvent(self, "onUpdate")
end

function LatticeAnimProbe:next(phase)
    self.phase = phase
    self.phaseTime = 0
    self.settled = 0
end

-- The phases wait on the animations' own clocks, not on the summed frame deltas: an animation
-- advances once it runs (its start takes a frame or two, each up to Engine's 0.25 s cap on a
-- loaded machine), and the offsets lag their clock by up to two frames. A phase's state held for three updates
-- in a row is reached (20 s of frames: given up, its values go out as they are).
function LatticeAnimProbe:reached(condition)
    self.settled = condition and (self.settled or 0) + 1 or 0
    return self.settled >= 3 or self.phaseTime >= 20
end

function LatticeAnimProbe:onUpdate()
    local dt = Engine.deltatime
    self.phaseTime = self.phaseTime + dt
    local cage = self.cage

    if self.phase == "a" then
        local tc = self.trackA:getTimeCount()
        if not self.sampled and tc > 0.3 and tc < 0.5 then
            self.sampled = true
            out("sampleA", string.format("[%.6f,%.6f,%.6f]", tc, dt, cage:getOffsetAt(4).y))
        end
        if self:reached(tc >= 0.8) then
            out("aHeld4", v3(cage:getOffsetAt(4)))
            out("aShared", v3(cage:getOffsetAt(10)))
            out("aRest22", v3(cage:getOffsetAt(22)))
            out("aCentre", v3(cage:getOffsetAt(13)))
            self.animA:fadeOut(1.0)
            self.animB:fadeIn(1.0)
            self:next("fade")
        end
    elseif self.phase == "fade" then
        if not self.mid and self.animB.blendWeight >= 0.3 and self.animA.blendWeight >= 0.3 then
            self.mid = true
            out("midShared", v3(cage:getOffsetAt(10)))
            out("midHeld4", v3(cage:getOffsetAt(4)))
            out("midWeights", string.format("[%.6f,%.6f]", self.animA.blendWeight, self.animB.blendWeight))
        end
        if self:reached(self.mid and self.animA:isStopped() and self.trackB:getTimeCount() >= 0.8) then
            out("end4", v3(cage:getOffsetAt(4)))
            out("end22", v3(cage:getOffsetAt(22)))
            out("endShared", v3(cage:getOffsetAt(10)))
            out("endCentre", v3(cage:getOffsetAt(13)))
            out("aStopped", self.animA:isStopped())
            out("bRunning", self.animB:isRunning())
            self:next("done")
        end
    elseif self.phase == "done" then
        if not latticeAnimDone then
            out("done", true)
            latticeAnimDone = true
        end
    end
end

return LatticeAnimProbe
"""


async def test_lua_tracks_and_crossfade(ed: Editor):
    s = await setup(ed)
    sid = s["scene"]
    driver = (await ed.call("create_entity", scene=sid, type="empty", name="Driver"))["entity_id"]
    await ed.call("create_script", kind="lua", name="LatticeAnimProbe", content=PROBE, scene=sid, entity=driver)
    await ed.call("save_scene", scene=sid)
    await ed.console_since(None, limit=1)
    started = await ed.call("play", action="start", scene=sid)
    ed.check_eq(started["state"], "playing", "play starts")
    try:
        waited = await ed.call_raw("wait", {"until": "latticeAnimDone == true", "timeout_seconds": 60})
        ed.check(not waited.is_error, f"the probe finished: {waited.text[:1500]}")
        entries = await ed.console_since(None, limit=1000)
    finally:
        await ed.call("play", action="stop")
    v = lines(entries, "LATANIM")
    errors = [e.get("message", "") for e in entries if e.get("type") == "error"]
    ed.check(not errors, f"no errors (the out-of-range point is ignored silently): {errors}")

    ed.check_eq(v.get("targetA"), True, "addActionFrame set the track's target")
    # onUpdate may run before the actions of its frame: the offsets lag the time by up to two frames
    tc, dt, y = v.get("sampleA", [0, 0, -1])
    ed.check(0.6 * max(0.0, tc - 2.5 * dt) - 1e-4 <= y <= 0.6 * tc + 1e-4 and y > 0, f"clip A mid-segment: {v.get('sampleA')}")
    check_close(ed, v.get("aHeld4"), [0, 0.3, 0], "clip A holds point 4 at its key")
    check_close(ed, v.get("aShared"), [0.2, 0, 0], "clip A holds the shared point")
    check_close(ed, v.get("aRest22"), ZERO, "point 22 untouched before clip B")
    check_close(ed, v.get("aCentre"), [0, 0, 0.05], "the centre point keeps its offset")

    wa, wb = v.get("midWeights", [0, 0])
    ed.check(0.05 < wa < 0.95 and 0.05 < wb < 0.95, f"mid crossfade weights: {v.get('midWeights')}")
    mid = v.get("midShared", [0, 0, 0])
    ed.check(0.2 + 0.02 < mid[0] < 0.6 - 0.02, f"the shared point blends between the clips: {mid}")
    expected = (0.2 * wa + 0.6 * wb) / (wa + wb)
    ed.check(abs(mid[0] - expected) < 0.05, f"weighted by the fade weights: {mid[0]} vs {expected}")
    check_close(ed, v.get("midHeld4"), [0, 0.3, 0], "point 4 is clip A's alone: not pulled to rest while fading")

    ed.check_eq(v.get("aStopped"), True, "clip A faded out and stopped")
    ed.check_eq(v.get("bRunning"), True, "clip B runs")
    check_close(ed, v.get("end4"), [0, 0.3, 0], "point 4 keeps its last value once no track animates it")
    check_close(ed, v.get("end22"), [0, -0.3, 0], "clip B moved point 22")
    check_close(ed, v.get("endShared"), [0.6, 0, 0], "the shared point is clip B's")
    check_close(ed, v.get("endCentre"), [0, 0, 0.05], "the centre point never moved")
    await ed.call("delete_entities", scene=sid, entities=[driver])
    await ed.check_alive()


# -- files -----------------------------------------------------------------------


async def test_save_reload_round_trip(ed: Editor):
    s = await setup(ed)
    sid = s["scene"]
    await ed.call("save_scene", scene=sid)
    text = (ed.project / "scenes/LatticeAnim.scene").read_text()
    ed.check("latticetracks" in text, "the track is in the scene file")
    expected = await tracks_of(ed, S["squash_track"])
    fill = await tracks_of(ed, lattice_action(await ed.call("get_animation", scene=sid, entity=S["fill"]))["action"]["id"])

    async with Editor(project=ed.project, label="lattice-anim-reload") as ed2:
        opened = await ed2.call("open_scene", scene="scenes/LatticeAnim.scene")
        sid2 = opened["scene"]["id"]
        got = await tracks_of(ed2, S["squash_track"], scene=sid2)
        ed2.check_eq(got["points"], expected["points"], "points reloaded")
        ed2.check_eq(got["times"], expected["times"], "times reloaded")
        check_close(ed2, got["values"], expected["values"], "values reloaded")
        anim = await ed2.call("get_animation", scene=sid2, entity=S["fill"])
        got = await tracks_of(ed2, lattice_action(anim)["action"]["id"], scene=sid2)
        ed2.check_eq(got["easings"], fill["easings"], "easings reloaded")
        check_close(ed2, got["values"], fill["values"], "filled values reloaded")
        await ed2.call("set_properties", scene=sid2, entity=S["cage"], values={"lattice": {"fadeDistance": 0.5}})
        await ed2.call("set_properties", scene=sid2, entity=S["cage"], values={"lattice": {"fadeDistance": 0.25}})
        # Verify an unmodified re-save before preview deliberately changes the
        # keyframe evaluation progress (which is part of the legacy scene format).
        scene_file = ed2.project / "scenes/LatticeAnim.scene"
        scene_file.write_text("")
        await ed2.call("save_scene", scene=sid2)
        resaved = scene_file.read_text()
        if resaved != text:
            import difflib
            diff = "".join(list(difflib.unified_diff(text.splitlines(True), resaved.splitlines(True)))[:60])
            ed2.fail("re-save is not byte-identical:\n" + diff)

        # Separately, the reloaded clip must still evaluate its authored keys.
        await ed2.call("preview_animation", scene=sid2, animation=S["squash"], action="seek", time=1.0)
        off = await offsets(ed2, S["cage"], scene=sid2)
        ed2.check(off[4][1] > 0.35, f"the reloaded clip evaluates: {off[4]}")
        await ed2.call("preview_animation", scene=sid2, animation=S["squash"], action="stop")
