"""Lattice agent-UX pass (spec 2026-10-23-lattice-deformer-design.md §12 "Agent UX pass"): the fixes
found by using the lattice blind over MCP.

- bend and twist presets keep true world angles on a non-cubic cage (a cage fitted around a tall
  pole bends it the asked angle and keeps its cross-section), and toward "-x" bends the other way;
- a child's own binding to a lattice it also inherits through include_children replaces the
  inherited one (never deformed twice; weight 0 excludes the child), bind says so, unbind of a
  child that only inherits says how to exclude it;
- the follow op (a prop rides a squashed surface), its rest, its refusals and the
  follower_outside warning;
- lattice keys "rest" and pose names in edit_animation, "rest" read back, the hints for the usual
  mistakes (the mesh as the track target, an unknown pose, toward = axis, editing while playing);
- the bake note when the cage deforms nothing any more.

Runs in a new empty project; the tests share one editor and run in order (state in S).
"""

import math
import shutil
import sys
from pathlib import Path

from harness import REPO, Editor

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import pngdiff  # noqa: E402  (tools/mcp/pngdiff.py)

TIMEOUT = 900
EDITOR = {"create": True}
CI = True

S: dict = {}

SPHERE = REPO / "tools/fixtures/shader-library-export/assets/models/uv_sphere.gltf"


def close(a, b, tol=1e-3):
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        return abs(a - b) <= tol
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(close(x, y, tol) for x, y in zip(a, b))
    return a == b


def check_close(ed, actual, expected, what="", tol=1e-3):
    ed.check(close(actual, expected, tol), f"{what}: expected {expected!r}, got {actual!r}")


def idx(res, i, j, k):
    return i + res[0] * (j + res[1] * k)


async def setup(ed: Editor) -> dict:
    if "scene" in S:
        return S
    made = await ed.call("create_scene", name="LatticeUX", type="3D", path="scenes/LatticeUX")
    S["scene"] = made["scene"]["id"]
    await ed.call("create_entity", scene=S["scene"], type="camera", name="Cam", position=[0, 1.5, 9])
    return S


async def el(ed, **args):
    args.setdefault("scene", S["scene"])
    return await ed.call("edit_lattice", args)


async def el_error(ed, contains, **args):
    args.setdefault("scene", S["scene"])
    raw = await ed.call_raw("edit_lattice", args)
    ed.check(raw.is_error, f"edit_lattice should fail: {args} -> {raw.text[:300]}")
    for part in [contains] if isinstance(contains, str) else contains:
        ed.check(part in raw.text, f"error should contain {part!r}: {raw.text}")
    return raw.text


async def create(ed, type_, name, **args):
    return (await ed.call("create_entity", scene=S["scene"], type=type_, name=name, **args))["entity_id"]


def world_of(read: dict, index: int):
    for p in read["points"]:
        if p["index"] == index:
            return p["world"]
    return None


async def shot(ed, name):
    data = await ed.screenshot("game", path=str(ed.scratch.root / f"{name}.png"), max_width=480)
    return pngdiff.decode(data)


async def settle(ed, frames=20):
    # bindings reach the GPU (and a mesh its lattice variant) over a few editor frames
    await ed.call("wait", frames=frames)


# -- presets in the cage's proportions ------------------------------------------------


async def test_bend_and_twist_true_angles_on_a_tall_cage(ed: Editor):
    await setup(ed)
    res = [3, 5, 3]
    lat = await create(ed, "lattice", "TallCage", position=[0, 2, 0], scale=[1, 4, 1])
    await el(ed, lattice=lat, resolution=res, interpolation="sharp")
    read = await el(ed, lattice=lat, detail="points",
                    ops=[{"op": "preset", "name": "bend", "axis": "y", "toward": "x", "amount": 60}])
    theta, length = math.pi / 3, 4.0
    top = world_of(read, idx(res, 1, 4, 1))
    # the axis is an arc of the cage's world length turning 60 degrees: its end is where a 60
    # degree arc of length 4 ends (before the fix: a local-space arc stretched 4x, about 23 degrees)
    check_close(ed, top, [length / theta * (1 - math.cos(theta)), length / theta * math.sin(theta), 0],
                "the top centre ends a 60 degree arc away", tol=1e-3)
    left, right = world_of(read, idx(res, 0, 4, 1)), world_of(read, idx(res, 2, 4, 1))
    width = math.dist(left, right)
    check_close(ed, width, 1.0, "the cross-section keeps its width (rigid)", tol=1e-3)
    tangent = [right[0] - left[0], right[1] - left[1]]
    check_close(ed, math.degrees(math.atan2(-tangent[1], tangent[0])), 60.0, "the top face turned 60 degrees", tol=0.05)

    # toward "-x" mirrors it (a negative amount)
    read = await el(ed, lattice=lat, detail="points",
                    ops=[{"op": "reset"}, {"op": "preset", "name": "bend", "axis": "y", "toward": "-x", "amount": 60}])
    check_close(ed, world_of(read, idx(res, 1, 4, 1))[0], -length / theta * (1 - math.cos(theta)), "toward -x bends left")
    await el(ed, lattice=lat, ops=[{"op": "reset"}, {"op": "preset", "name": "bend", "axis": "y", "toward": "+x", "amount": 10}])

    # twist of a 2 x 1 cross-section: the corners keep their distance from the axis
    flat = await create(ed, "lattice", "FlatCage", position=[0, 1, 5], scale=[2, 2, 1])
    read = await el(ed, lattice=flat, interpolation="sharp", detail="points",
                    ops=[{"op": "preset", "name": "twist", "axis": "y", "amount": 45}])
    corner = world_of(read, idx([3, 3, 3], 2, 2, 0))
    check_close(ed, math.hypot(corner[0], corner[2] - 5), math.hypot(1.0, 0.5), "a twisted corner stays on its circle")
    S["tall"] = lat


async def test_toward_errors_name_the_fix(ed: Editor):
    await setup(ed)
    lat = S["tall"]
    await el_error(ed, ["ops[0].toward", "use z or x"], lattice=lat,
                   ops=[{"op": "preset", "name": "bend", "axis": "y", "toward": "y", "amount": 30}])
    await el_error(ed, ["ops[0].toward", "-x"], lattice=lat,
                   ops=[{"op": "preset", "name": "bend", "axis": "y", "toward": "sideways", "amount": 30}])


# -- a child's own binding replaces the inherited one -------------------------------------


async def test_child_binding_replaces_inherited(ed: Editor):
    """The coordinator's double binding: a child bound to a lattice its group already hands down was
    deformed twice. Now each lattice deforms a mesh once, the child's own binding wins (weight 0
    excludes it), and bind says so."""
    await setup(ed)
    sid = S["scene"]
    group = await create(ed, "empty", "Group", position=[0, 0, 0])
    a = await create(ed, "cube", "A", parent=group, position=[-1.2, 1, 0])
    b = await create(ed, "cube", "B", parent=group, position=[1.2, 1, 0])
    made = await el(ed, create={"targets": [group], "name": "GroupCage"},
                    ops=[{"op": "preset", "name": "shear", "axis": "y", "toward": "x", "amount": 0.4}])
    cage = made["lattice"]["id"]
    await ed.call("select_entities", scene=sid, entities=[])
    await settle(ed)
    base = await shot(ed, "group-sheared")

    # A bound again with weight 1: the same picture (it was sheared twice before)
    got = await el(ed, lattice=cage, ops=[{"op": "bind", "target": a, "weight": 1}])
    ed.check_eq(got["ops"][0].get("overrides", {}).get("id"), group, f"bind reports the inherited binding it replaces: {got}")
    ed.check(any("replaces" in n for n in got.get("notes", [])), f"and says so: {got.get('notes')}")
    await settle(ed)
    twice = await shot(ed, "group-a-bound-again")
    ok, fraction, _ = pngdiff.compare(base, twice, channel_tolerance=8, max_fraction=0.0005)
    ed.check(ok, f"a child bound again is not deformed twice ({fraction:.4%} of the pixels changed)")

    # B with weight 0: excluded; the same as binding A alone
    got = await el(ed, lattice=cage, ops=[{"op": "bind", "target": b, "weight": 0}])
    ed.check(any("excluded" in n for n in got.get("notes", [])), f"weight 0 is reported as excluded: {got.get('notes')}")
    await settle(ed)
    excluded = await shot(ed, "group-b-excluded")
    ok, _, _ = pngdiff.compare(base, excluded, channel_tolerance=8, max_fraction=0.0005)
    ed.check(not ok, "excluding B changes the picture")
    await el(ed, lattice=cage, ops=[{"op": "unbind", "target": b}, {"op": "unbind", "target": group}])
    await settle(ed)
    alone = await shot(ed, "group-a-alone")
    ok, fraction, _ = pngdiff.compare(excluded, alone, channel_tolerance=8, max_fraction=0.0005)
    ed.check(ok, f"B excluded = only A bound ({fraction:.4%} of the pixels differ)")

    # unbinding a child that only inherits the lattice says how to exclude it
    await el(ed, lattice=cage, ops=[{"op": "unbind", "target": a}, {"op": "bind", "target": group}])
    await el_error(ed, ["has no binding of its own", "weight 0", "Group"], lattice=cage, ops=[{"op": "unbind", "target": b}])
    await ed.call("delete_entities", scene=sid, entities=[group, cage])


# -- followers ------------------------------------------------------------------------


async def test_follow_op(ed: Editor):
    await setup(ed)
    sid = S["scene"]
    box = await create(ed, "cube", "Box", position=[5, 0.5, 0])
    prop = await create(ed, "sphere", "Prop", position=[5, 1.2, 0], scale=[0.4, 0.4, 0.4])
    made = await el(ed, create={"targets": [box], "name": "BoxCage"}, ops=[{"op": "follow", "target": prop}])
    cage = made["lattice"]["id"]
    rest = made["ops"][1]["rest"]["id"]
    ed.check_eq(made["followers"], [{"entity": made["ops"][1]["target"], "rest": made["ops"][1]["rest"]}], "listed")
    restT = await ed.call("get_entity", scene=sid, entity=rest, components=["transform"])
    check_close(ed, restT["world"]["position"], [5, 1.2, 0], "the rest is made where the prop stands")
    follower = (await ed.call("get_entity", scene=sid, entity=prop, components=["latticefollower"]))["components"]["latticefollower"]
    ed.check_eq((follower["lattice"]["id"], follower["rest"]["id"]), (cage, rest), "lattice and rest assigned")

    # just above the cage, with fade: a warning with the share it gets, and the fix
    got = await el(ed, lattice=cage, ops=[{"op": "preset", "name": "squash", "amount": 0.4}])
    warning = next((w for w in got["warnings"] if w["kind"] == "follower_outside"), None)
    ed.check(warning and 0 < warning["share"] < 0.5 and "extend" in warning["message"], f"follower_outside: {got['warnings']}")
    got = await el(ed, lattice=cage, outside="extend")
    ed.check_eq(got["warnings"], [], "extend reaches it")
    await settle(ed, 3)
    world = (await ed.call("get_entity", scene=sid, entity=prop))["world"]["position"]
    # the top face went from 1.0 to 0.8 (squash 0.4 of a unit box): the prop follows it down
    check_close(ed, world, [5, 1.0, 0], "the prop rides the squashed surface", tol=0.02)

    # following again keeps the rest; refusals
    again = await el(ed, lattice=cage, ops=[{"op": "follow", "target": prop}])
    ed.check_eq(again["ops"][0]["rest"]["id"], rest, "the existing rest is kept")
    await el_error(ed, "is deformed by this lattice", lattice=cage, ops=[{"op": "follow", "target": box}])
    await el_error(ed, "or below it", lattice=cage, ops=[{"op": "follow", "target": prop, "rest": prop}])
    await el_error(ed, ["ops[0]", "Keys:"], lattice=cage, ops=[{"op": "follow", "target": prop, "at": [0, 0, 0]}])
    S.update(box=box, prop=prop, box_cage=cage)


# -- animation keys -------------------------------------------------------------------


async def test_lattice_keys_rest_and_poses(ed: Editor):
    await setup(ed)
    sid, box, cage = S["scene"], S["box"], S["box_cage"]
    await el(ed, lattice=cage, ops=[{"op": "reset"}, {"op": "preset", "name": "squash", "amount": 0.4}, {"op": "pose", "save": "squash"},
                                    {"op": "reset"}, {"op": "preset", "name": "stretch", "amount": 0.3}, {"op": "pose", "save": "stretch"},
                                    {"op": "reset"}])
    poses = (await ed.call("get_entity", scene=sid, entity=cage, components=["lattice"], properties=["poses"]))["components"]["lattice"]["poses"]
    squash = next(p for p in poses if p["name"] == "squash")["offsets"]
    got = await ed.call("edit_animation", scene=sid, target=box, name="Bounce", tracks=[
        {"target": cage, "channel": "lattice", "keys": [{"time": 0, "points": "rest"}, {"time": 0.5, "points": "squash"},
                                                        {"time": 0.7, "points": "stretch"}, {"time": 1, "points": "rest"}]}])
    keys = got["actions"][0]["keys"]
    ed.check_eq([k["points"] for k in (keys[0], keys[3])], ["rest", "rest"], "rest keys read back as \"rest\"")
    for index, value in keys[1]["points"].items():
        check_close(ed, value, squash[int(index)], f"the squash key holds the pose (point {index})", tol=1e-5)
    moved = {i for i, o in enumerate(squash) if any(abs(c) > 0 for c in o)}
    ed.check_eq(set(map(int, keys[1]["points"])), moved, "the keyed points are the ones the poses move")

    text = (await ed.call_raw("edit_animation", {"scene": sid, "target": box, "tracks": [
        {"target": cage, "channel": "lattice", "keys": [{"time": 0, "points": "squish"}]}]})).text
    ed.check("poses: squash, stretch" in text, f"an unknown pose lists the poses: {text}")
    text = (await ed.call_raw("edit_animation", {"scene": sid, "target": box, "tracks": [
        {"target": box, "channel": "lattice", "keys": [{"time": 0, "points": "rest"}]}]})).text
    ed.check("not the mesh it deforms" in text and "BoxCage" in text, f"the mesh as target names the cage: {text}")


# -- play and bake ----------------------------------------------------------------------


async def test_play_refusal_names_the_runtime_way(ed: Editor):
    await setup(ed)
    await ed.call("save_scene", scene=S["scene"])
    await ed.call("play", action="start", scene=S["scene"])
    try:
        await el_error(ed, ["is playing; stop play mode before editing it.", "eval_lua", "applyPreset"],
                       lattice=S["box_cage"], ops=[{"op": "reset"}])
    finally:
        await ed.call("play", action="stop")


async def test_bake_says_the_cage_is_unused(ed: Editor):
    await setup(ed)
    if not SPHERE.is_file():
        ed.fail(f"missing fixture model {SPHERE}")
    dest = ed.project / "assets/models"
    dest.mkdir(parents=True, exist_ok=True)
    for f in SPHERE.parent.iterdir():
        if f.name.startswith("uv_sphere"):
            shutil.copy(f, dest / f.name)
    await ed.call("sync_files")
    rock = await create(ed, "model", "Rock", model="assets/models/uv_sphere.gltf", position=[-5, 1, 0])
    made = await el(ed, create={"targets": [rock], "name": "RockCage", "resolution": [4, 4, 4]},
                    ops=[{"op": "preset", "name": "noise", "amount": 0.12, "seed": 3}])
    ed.check_eq(made["warnings"], [], "the guide's rock noise does not fold")
    got = await el(ed, lattice=made["lattice"]["id"], ops=[{"op": "bake"}])
    ed.check("delete it" in got["ops"][0].get("note", ""), f"the bake says the cage is unused now: {got['ops']}")
