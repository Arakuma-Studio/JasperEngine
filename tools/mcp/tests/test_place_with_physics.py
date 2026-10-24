"""place_with_physics (editor/mcp/tools/PlacementTools.cpp over editor/physics/PlacementSession), on
a new project whose scene is built here from engine primitives (cubes, a cylinder):

- the schema (one tool, real constraints, defaults) and a fix-it error for every bad argument;
- a preview changes nothing (get_entity before and after, the scene not modified);
- drop: a cube lands on the floor at the right height, resting on it;
- the same arguments with apply: true commit the stored preview (from_preview, the preview's
  state_hash and poses); after an edit in between, or with other arguments, they simulate again;
- one undo puts every mover back; prefab instances get transform overrides;
- the job: progress through get_job, cancel_job stops a run and nothing changes, a scene edit
  or Play starting during a run fails it;
- refused during Play;
- screenshot: a Scene view image; a preview's poses are put back afterwards, also when a save
  happens meanwhile (the saved file holds the real poses) or the call is cancelled mid-capture;
- scatter: one batch with undo_group (create_entity xN + place_with_physics with jitter and seed)
  is one undo step and gives the same poses and state_hash when run again;
- separate resolves an overlap; move stops at a wall; snap lands on the surface a ray hits (world
  ray and Scene view pixel);
- decompose: always waits for the convex decomposition of a concave mover ("Decomposing k/n").
"""

from __future__ import annotations

import asyncio
import contextlib
import json
from pathlib import Path

import yaml

from harness import Editor, TestFailure
from htmlui_pixels import decode_png

EDITOR = {"create": True, "name": "PhysicsPlacement"}
TIMEOUT = 900

S: dict = {}


def fail(message: str) -> None:
    raise TestFailure(message)


def check(cond, message: str) -> None:
    if not cond:
        fail(message)


def near(a, b, tol: float) -> bool:
    return all(abs(x - y) <= tol for x, y in zip(a, b)) and len(a) == len(b)


# ---------------------------------------------------------------------------- scene

# The floor's top is y = 0; every area is far enough from the others to stay out of their context
# region (the movers' bounds + 2 m)
SCENE = [
    {"name": "Floor", "type": "cube", "position": [0, -0.5, 0], "scale": [60, 1, 60]},
    # drop
    {"name": "Crate", "type": "cube", "position": [-10, 3, 0]},
    {"name": "Crate2", "type": "cube", "position": [-12.5, 4, 0], "rotation": {"euler": [0, 30, 20]}},
    # move: a wall across the lane, its near face at x = 3.75
    {"name": "Slider", "type": "cube", "position": [0, 0.5, 10]},
    {"name": "Wall", "type": "cube", "position": [4, 1.5, 10], "scale": [0.5, 3, 4]},
    # snap: a table top at y = 1
    {"name": "Table", "type": "cube", "position": [-10, 0.5, -10], "scale": [2, 1, 2]},
    # high enough to travel over the table's edge to where the ray hits (snap drives it there)
    {"name": "Snapper", "type": "cube", "position": [-6, 4, -10]},
    # separate: two cubes overlapping by half a metre
    {"name": "OverA", "type": "cube", "position": [10, 0.5, -10]},
    {"name": "OverB", "type": "cube", "position": [10.5, 0.5, -10]},
]


async def world(ed: Editor, ref) -> tuple[list, list]:
    e = await ed.call("get_entity", entity=ref)
    return e["world"]["position"], e["world"]["rotation"]["quat"]


async def local(ed: Editor, ref) -> tuple[list, list]:
    e = await ed.call("get_entity", entity=ref)
    t = e["components"]["transform"]
    return t["position"], t["rotation"]["quat"]


async def poses(ed: Editor, refs) -> dict:
    return {r: await world(ed, r) for r in refs}


async def place(ed: Editor, **args):
    r = await ed.call_raw("place_with_physics", args, timeout=600)
    if r.is_error:
        fail(f"place_with_physics {args} failed: {r.text}")
    return r


def by_path(result: dict, path: str) -> dict:
    for e in result["entities"]:
        if e["path"] == path:
            return e
    fail(f"{path} is not in the result: {[e['path'] for e in result['entities']]}")
    return {}


async def scene_modified(ed: Editor) -> bool:
    scenes = await ed.call("list_scenes")
    return next(s for s in scenes["scenes"] if s["selected"])["modified"]


async def wait_job(ed: Editor, job: str, seconds: float = 300) -> dict:
    for _ in range(int(seconds / 0.1)):
        state = await ed.call("get_job", job_id=job)
        if state["status"] != "running":
            return state
        await asyncio.sleep(0.1)
    fail(f"job {job} still running after {seconds} s")
    return {}


# ---------------------------------------------------------------------------- tests

async def test_setup_and_schema(ed: Editor):
    ed.require_tools("place_with_physics")
    await ed.batch([("create_entity", item) for item in SCENE])
    # what every test leaves as it found it unless it says otherwise
    await ed.call("save_scene", path="scenes/Placement.scene")
    S["start"] = await poses(ed, [e["name"] for e in SCENE])

    tool = ed.tool("place_with_physics")
    schema = tool.input_schema
    props = schema["properties"]
    check(set(props) == {"scene", "entities", "use_selection", "mode", "target", "ray", "axis", "amount", "gravity", "settle",
                         "context", "context_margin", "ignore_layers", "push_neighbours", "colliders", "decompose", "jitter",
                         "seed", "max_seconds", "feel", "apply", "screenshot", "wait", "timeout_seconds"},
          f"the parameters of spec section 8.1: {sorted(props)}")
    check(props["mode"]["enum"] == ["drop", "separate", "move", "snap", "gather", "distribute"] and props["mode"]["default"] == "drop",
          f"mode: {props['mode']}")
    check(set(props["target"]["properties"]) == {"position", "rotation", "delta", "space"}, f"target: {props['target']}")
    check(set(props["ray"]["properties"]) == {"origin", "direction", "screen"}, f"ray: {props['ray']}")
    check(props["colliders"]["enum"] == ["auto", "authored", "generated", "both"] and props["colliders"]["default"] == "auto", "colliders")
    check(props["decompose"]["enum"] == ["never", "cached", "always"] and props["decompose"]["default"] == "cached", "decompose")
    check(props["feel"]["enum"] == ["precise", "balanced", "fast"] and props["feel"]["default"] == "balanced", "feel")
    check(props["seed"]["default"] == 1 and props["max_seconds"]["default"] == 10, "seed and max_seconds defaults")
    check(props["gravity"]["default"] == [0, -9.81, 0], f"gravity default: {props['gravity']}")
    check(props["context_margin"]["default"] == 2 and props["context"]["default"] == "auto", "context defaults")
    check("default" not in props["apply"] and props["apply"]["type"] == "boolean", "apply: a boolean, false when omitted (preview)")
    check(set(props["jitter"]["properties"]) == {"position", "rotation_deg"}, "jitter")
    check("required" not in schema, "entities or use_selection: checked by the handler")
    check("jasper://docs/physics" in tool.description and "apply: true" in tool.description, f"description: {tool.description}")
    annotations = tool.annotations
    check(annotations is None or not annotations.read_only_hint, "edits: not read-only")
    msg = await ed.call("place_with_physics", {"entities": ["Crate"], "bogus": 1}, expect_error=True)
    check("bogus" in msg and "entities" in msg, f"an unknown argument is refused, listing the valid ones: {msg}")


async def test_argument_errors(ed: Editor):
    base = {"entities": ["Crate"]}
    cases = [
        ({"entities": None}, "entities is required"),
        ({"entities": []}, "non-empty array"),
        ({"entities": ["NoSuchThing"]}, "NoSuchThing"),
        ({"entities": "Crate"}, "non-empty array"),
        ({"use_selection": True}, "not both"),
        ({"mode": "wobble"}, "mode must be"),
        ({"mode": "move"}, "mode move needs target"),
        ({"target": {"delta": [1, 0, 0]}}, "only used by mode: move"),
        ({"mode": "move", "target": {"position": [0, 0, 0], "delta": [1, 0, 0]}}, "not both"),
        ({"mode": "move", "target": {"dleta": [1, 0, 0]}}, "unknown key \"dleta\""),
        ({"mode": "move", "target": {"space": "world"}}, "needs position, delta and/or rotation"),
        ({"mode": "move", "target": {"delta": [1, 0, 0], "space": "parent"}}, "target.space must be"),
        ({"mode": "move", "target": {"delta": [1, 0]}}, "target.delta must be [x,y,z]"),
        ({"mode": "move", "target": {"rotation": "sideways"}}, "target.rotation must be"),
        ({"mode": "move", "target": [1, 0, 0]}, "target must be"),
        ({"mode": "snap"}, "mode snap needs ray"),
        ({"mode": "snap", "ray": {"origin": [0, 1, 0]}}, "needs origin and direction"),
        ({"mode": "snap", "ray": {"origin": [0, 1, 0], "direction": [0, 0, 0]}}, "must not be zero"),
        ({"mode": "snap", "ray": {"screen": [1, 1], "origin": [0, 0, 0]}}, "not both"),
        ({"mode": "snap", "ray": {"screen": [100000, 5]}}, "outside the Scene view"),
        ({"mode": "snap", "ray": {"screen": "middle"}}, "ray.screen must be"),
        ({"mode": "snap", "ray": {"from": [0, 0, 0]}}, "unknown key \"from\""),
        ({"ray": {"origin": [0, 5, 0], "direction": [0, -1, 0]}}, "only used by mode: snap"),
        ({"mode": "distribute"}, "needs axis"),
        ({"mode": "distribute", "axis": "w"}, "axis must be x, y or z"),
        ({"axis": "x"}, "only used by mode: distribute"),
        ({"mode": "gather", "amount": 2}, "amount must be a number from 0 to 1"),
        ({"amount": 0.5}, "only used by mode: gather"),
        ({"gravity": [0, -9.81]}, "gravity must be [x,y,z]"),
        ({"mode": "separate", "gravity": [0, -1, 0]}, "separate runs without gravity"),
        ({"mode": "separate", "settle": True}, "leave settle out"),
        ({"settle": "yes"}, "settle must be true or false"),
        ({"context": "everything"}, "context must be"),
        ({"context": []}, "context must be"),
        ({"context": ["NoSuchFloor"]}, "context: "),
        ({"context_margin": -1}, "context_margin must be a number from 0 to 1000"),
        ({"ignore_layers": ["NoSuchLayer"]}, "is not a layer"),
        ({"ignore_layers": [40]}, "is not a layer"),
        ({"push_neighbours": 1}, "push_neighbours must be true or false"),
        ({"colliders": "magic"}, "colliders must be auto, authored, generated or both"),
        ({"decompose": "sometimes"}, "decompose must be never, cached or always"),
        ({"jitter": 0.5}, "jitter must be"),
        ({"jitter": {"pos": 1}}, "unknown key \"pos\""),
        ({"jitter": {"position": -1}}, "jitter.position must be"),
        ({"jitter": {"rotation_deg": 270}}, "jitter.rotation_deg must be"),
        ({"seed": -1}, "seed must be"),
        ({"seed": 1.5}, "seed must be"),
        ({"seed": 4294967296}, "seed must be"),
        ({"max_seconds": 0}, "max_seconds must be"),
        ({"feel": "slow"}, "feel must be precise, balanced or fast"),
        ({"screenshot": "scene"}, "screenshot must be"),
        ({"screenshot": {"width": 5}}, "unknown key \"width\""),
        ({"screenshot": {"max_width": -3}}, "screenshot.max_width must be"),
        ({"apply": "yes"}, "apply must be true or false"),
        ({"wait": "no"}, "wait must be true or false"),
        ({"timeout_seconds": 0}, "timeout_seconds must be"),
        ({"scene": "NoSuchScene"}, "NoSuchScene"),
    ]
    for extra, needle in cases:
        args = {**base, **extra}
        args = {k: v for k, v in args.items() if v is not None}
        msg = await ed.call("place_with_physics", args, expect_error=True)
        check(needle in msg, f"{extra}: the error says {needle!r}: {msg}")

    # use_selection with nothing selected
    await ed.call("select_entities", entities=[])
    msg = await ed.call("place_with_physics", {"use_selection": True}, expect_error=True)
    check("Nothing is selected" in msg, f"use_selection with an empty selection: {msg}")

    # a 2D scene
    scene_id = (await ed.call("list_scenes"))["selected_scene_id"]
    made = await ed.call("create_scene", name="Flat", type="2D")
    flat = made.get("scene", {}).get("id", made.get("scene_id"))
    await ed.call("create_entity", name="Sprite", type="sprite", scene=flat)
    msg = await ed.call("place_with_physics", {"scene": flat, "entities": ["Sprite"]}, expect_error=True)
    check("3D scenes" in msg, f"2D scenes are refused: {msg}")
    await ed.call("close_scene", scene=flat, unsaved="discard")
    await ed.call("select_scene", scene=scene_id)

    check(await poses(ed, S["start"].keys()) == S["start"], "errors change nothing")


async def test_preview_changes_nothing(ed: Editor):
    before = {name: await ed.call("get_entity", entity=name) for name in ("Crate", "Crate2")}
    modified = await scene_modified(ed)
    r = await place(ed, entities=["Crate", "Crate2"])
    v = r.structured
    check(v["applied"] is False and v["mode"] == "drop" and v["seed"] == 1, f"a preview: {v}")
    check(isinstance(v["state_hash"], str) and v["state_hash"], "a state_hash")
    check(v["all_settled"] is True and v["steps"] > 0 and v["sim_seconds"] > 0, f"settled: {v}")
    check({e["path"] for e in v["entities"]} == {"Crate", "Crate2"}, f"the movers: {v['entities']}")
    check("apply: true" in r.text and "Nothing was changed" in r.text, f"the text says how to commit: {r.text}")
    check(isinstance(v["hint"], str) and v["hint"], "a hint")
    for name, entity in before.items():
        after = await ed.call("get_entity", entity=name)
        check(after["world"] == entity["world"] and after["components"]["transform"] == entity["components"]["transform"],
              f"{name} unchanged by a preview")
    check(await scene_modified(ed) == modified, "the scene is not marked modified")
    S["preview"] = v


async def test_drop_lands_on_floor(ed: Editor):
    v = S["preview"]
    crate = by_path(v, "Crate")
    check(near(crate["before"]["position"], [-10, 3, 0], 1e-4), f"before: {crate['before']}")
    pos = crate["after"]["position"]
    check(abs(pos[1] - 0.5) <= 0.001, f"the 1 m cube rests on the floor (top y = 0) with its centre at y = 0.5: {pos}")
    check(abs(pos[0] + 10) < 0.05 and abs(pos[2]) < 0.05, f"it fell straight down: {pos}")
    check(abs(crate["moved_m"] - 2.5) < 0.01, f"moved_m: {crate['moved_m']}")
    check(crate["settled"] is True and crate["settle_step"] > 0, f"settled: {crate}")
    check(any(e["path"] == "Floor" for e in crate["resting_on"]), f"resting on the floor: {crate['resting_on']}")
    check(crate["max_penetration_m"] < 0.01, f"penetration: {crate['max_penetration_m']}")
    check(isinstance(crate["collider"], str) and crate["collider"], f"collider: {crate['collider']}")
    check(near(crate["after"]["local"]["position"], pos, 1e-4), "a root's local pose is its world pose")
    tilted = by_path(v, "Crate2")
    check(tilted["after"]["position"][1] < 1.0 and tilted["rotated_deg"] > 1.0, f"the tilted one tipped over onto a face: {tilted}")
    check(v["context"]["bodies"] >= 1, f"context: {v['context']}")


async def test_apply_matches_preview(ed: Editor):
    preview = S["preview"]
    r = await place(ed, entities=["Crate", "Crate2"], apply=True)
    v = r.structured
    check(v["applied"] is True, f"applied: {v}")
    check(v["from_preview"] is True and preview["from_preview"] is False, f"the stored preview was committed as it is: {v['from_preview']}")
    check(v["state_hash"] == preview["state_hash"], f"the same state_hash as the preview: {v['state_hash']} vs {preview['state_hash']}")
    check(v["steps"] == preview["steps"], "the same steps")
    for name in ("Crate", "Crate2"):
        a, p = by_path(v, name), by_path(preview, name)
        check(a["after"] == p["after"], f"{name}: the committed pose is the previewed one: {a['after']} vs {p['after']}")
        pos, quat = await world(ed, name)
        check(near(pos, a["after"]["position"], 1e-4) and near(quat, a["after"]["rotation"], 1e-4), f"{name} is there now: {pos} {quat}")
    check("one undo step" in r.text, f"text: {r.text}")
    check(await scene_modified(ed), "the scene is modified")


async def test_one_undo_restores(ed: Editor):
    await ed.call("undo")
    for name in ("Crate", "Crate2"):
        pos, quat = await world(ed, name)
        check(near(pos, S["start"][name][0], 1e-5) and near(quat, S["start"][name][1], 1e-5), f"one undo puts {name} back: {pos} {quat}")
    await ed.call("redo")
    check(near((await world(ed, "Crate"))[0], by_path(S["preview"], "Crate")["after"]["position"], 1e-4), "redo commits it again")
    await ed.call("undo")


async def test_apply_after_an_edit_simulates_again(ed: Editor):
    preview = (await place(ed, entities=["Crate"])).structured
    # an edit in between (even one that changes nothing in the end): the scene may read
    # differently now, so the apply simulates again instead of committing the stored preview
    await ed.call("set_transform", entity="Table", position=[-10, 0.5, -10])
    v = (await place(ed, entities=["Crate"], apply=True)).structured
    check(v["applied"] is True and v["from_preview"] is False, f"simulated again: {v['from_preview']}")
    check(v["state_hash"] == preview["state_hash"], "the same scene content: the same result anyway")
    await ed.call("undo")
    # other arguments than the preview's: simulated
    await place(ed, entities=["Crate"], seed=5, jitter={"position": 0.2})
    v = (await place(ed, entities=["Crate"], seed=6, jitter={"position": 0.2}, apply=True)).structured
    check(v["from_preview"] is False, "another seed is another simulation")
    await ed.call("undo")
    # a structure change in between drops the stored preview
    await place(ed, entities=["Crate"])
    await ed.call("create_entity", name="Extra", type="cube", position=[30, 0.5, -30])
    v = (await place(ed, entities=["Crate"], apply=True)).structured
    check(v["from_preview"] is False, "structure changed: simulated again")
    await ed.call("undo")
    await ed.call("delete_entities", entities=["Extra"])
    check(await poses(ed, ["Crate"]) == {"Crate": S["start"]["Crate"]}, "back where it started")


async def test_prefab_overrides_recorded(ed: Editor):
    # a prefab with a board and a box above it; in an instance, the box is a member (not the root,
    # whose position is the instance's own, Unity's default override): moving it is an override
    await ed.call("create_entity", name="Shelf", type="empty", position=[-10, 0, 30])
    await ed.call("create_entity", name="Board", type="cube", parent="Shelf", position=[0, 0.25, 0], scale=[3, 0.5, 3])
    await ed.call("create_entity", name="Box", type="cube", parent="Shelf", position=[0, 3, 0])
    await ed.call("create_prefab", entity="Shelf", path="prefabs/Shelf.prefab")
    await ed.call("instantiate_prefab", prefab="prefabs/Shelf.prefab", name="Shelf2", position=[-4, 0, 30])
    info = await ed.call("get_prefab_info", entity="Shelf2/Box")
    check(info["override_count"] == 0, f"no override yet: {info['overrides']}")
    r = await place(ed, entities=["Shelf2/Box"], apply=True)
    box = by_path(r.structured, "Shelf2/Box")
    check(abs(box["after"]["position"][1] - 1.0) < 0.002, f"the box lands on the board (top y 0.5): {box['after']}")
    check(near(box["after"]["local"]["position"], [box["after"]["position"][0] + 4, box["after"]["position"][1], box["after"]["position"][2] - 30], 1e-3),
          f"committed in the parent's space: {box['after']}")
    lpos, _ = await local(ed, "Shelf2/Box")
    check(near(lpos, box["after"]["local"]["position"], 1e-4), f"the local position is the committed one: {lpos}")
    info = await ed.call("get_prefab_info", entity="Shelf2/Box")
    overrides = json.dumps(info["overrides"])
    check(info["override_count"] >= 1 and "position" in overrides, f"the instance records the transform override: {overrides}")
    original = await ed.call("get_prefab_info", entity="Shelf/Box")
    check(original["override_count"] == 0, "the other instance is untouched")
    await ed.call("undo")
    info = await ed.call("get_prefab_info", entity="Shelf2/Box")
    check(info["override_count"] == 0, f"undo removes the override: {info['overrides']}")
    await ed.call("delete_entities", entities=["Shelf", "Shelf2"])


async def test_job_progress_and_cancel(ed: Editor):
    # a slow rain of cubes onto an empty corner of the floor: long enough to watch and cancel
    rain = [{"name": f"Rain{i}", "type": "cube", "position": [15 + (i % 6) * 1.5, 4 + i * 1.2, 12 + (i // 6) * 1.5]} for i in range(60)]
    await ed.batch([("create_entity", item) for item in rain])
    names = [item["name"] for item in rain]
    before = await poses(ed, names[:5])
    args = {"entities": names, "gravity": [0, -0.4, 0], "max_seconds": 120}
    r = await ed.call_raw("place_with_physics", {**args, "wait": False})
    check(not r.is_error, r.text)
    v = r.structured
    check(v.get("status") == "running" and v.get("job_id"), f"wait: false returns a job: {v}")
    progress = []
    while True:
        state = await ed.call("get_job", job_id=v["job_id"])
        if state["status"] != "running":
            break
        check(state["kind"] == "place_with_physics" and "preview" in state["title"], f"the job: {state}")
        progress.append((state["progress"], state["message"]))
        await asyncio.sleep(0.05)
    check(state["status"] == "succeeded" and state["result"]["state_hash"], f"the job finishes with the result: {state}")
    stepping = [(p, m) for p, m in progress if m.startswith("step ")]
    check(len(stepping) >= 3, f"progress while it ran (\"step k/n, a of b asleep\"): {progress[:5]}")
    check(all(0.0 <= p <= 1.0 for p, _ in stepping) and stepping[-1][0] >= stepping[0][0], f"progress grows: {stepping}")
    check(any("asleep" in m for _, m in stepping), f"{stepping[:3]}")
    # wait with a short timeout_seconds: the call returns the running job
    r = await ed.call_raw("place_with_physics", {**args, "seed": 4, "timeout_seconds": 1})
    check(r.structured.get("status") == "running" and r.structured.get("job_id"), f"a run longer than the timeout: {r.structured}")
    await wait_job(ed, r.structured["job_id"], 600)

    # another seed than the preview's: not its stored result, a run to cancel
    r = await ed.call_raw("place_with_physics", {**args, "seed": 2, "apply": True, "wait": False})
    job = r.structured["job_id"]
    cancel = await ed.call("cancel_job", job_id=job)
    check("ancel" in json.dumps(cancel), f"cancel_job: {cancel}")
    state = await wait_job(ed, job, 60)
    check(state["status"] == "cancelled" and "nothing was changed" in state["message"], f"cancelled: {state}")
    check(await poses(ed, names[:5]) == before, "a cancelled apply changes nothing")

    # a structural edit while it runs: the run is abandoned (its world no longer matches the scene)
    r = await ed.call_raw("place_with_physics", {**args, "seed": 3, "apply": True, "wait": False})
    job = r.structured["job_id"]
    await ed.call("create_entity", name="Intruder", type="cube", position=[40, 0.5, 40])
    state = await wait_job(ed, job, 60)
    check(state["status"] == "failed" and "scene changed" in state["message"], f"the scene changed during the run: {state}")
    check(await poses(ed, names[:5]) == before, "nothing committed")
    await ed.call("delete_entities", entities=["Intruder"])

    # Play starting during a run fails it
    r = await ed.call_raw("place_with_physics", {**args, "seed": 5, "apply": True, "wait": False})
    job = r.structured["job_id"]
    await ed.call("play", action="start")
    try:
        state = await wait_job(ed, job, 60)
    finally:
        await ed.call("play", action="stop")
    check(state["status"] == "failed" and "Play mode started" in state["message"], f"Play started during the run: {state}")
    check(await poses(ed, names[:5]) == before, "nothing committed")
    await ed.call("delete_entities", entities=names)


async def test_refused_during_play(ed: Editor):
    await ed.call("play", action="start")
    try:
        msg = await ed.call("place_with_physics", {"entities": ["Crate"]}, expect_error=True)
        check("Play mode" in msg or "playing" in msg, f"refused while playing: {msg}")
    finally:
        await ed.call("play", action="stop")
    check(await poses(ed, ["Crate"]) == {"Crate": S["start"]["Crate"]}, "nothing changed")


async def test_screenshot(ed: Editor):
    before = await ed.call("get_entity", entity="Crate")
    r = await place(ed, entities=["Crate"], screenshot=True)
    check(len(r.images) == 1, f"one image: {r.text}")
    w, h, _, _ = decode_png(r.images[0][0])
    check(w > 100 and h > 100, f"an image of the Scene view: {w}x{h}")
    check(r.structured["screenshot"]["shows"] == "the previewed poses", f"{r.structured['screenshot']}")
    check("previewed poses" in r.text, r.text)
    after = await ed.call("get_entity", entity="Crate")
    check(after["world"] == before["world"] and after["components"]["transform"] == before["components"]["transform"],
          "the preview poses were put back after the capture")
    r = await place(ed, entities=["Crate"], screenshot={"max_width": 320, "frame": False})
    w, _, _, _ = decode_png(r.images[0][0])
    check(w <= 320, f"max_width: {w}")

    # a save while the preview poses are shown: the file gets the real poses
    scene_file = ed.project / "scenes" / "Placement.scene"
    shot = asyncio.ensure_future(ed.call_raw("place_with_physics", {"entities": ["Crate"], "screenshot": True}, timeout=600))
    saves = 0
    while not shot.done():
        await ed.call("save_scene")
        saves += 1
        check(saved_position(scene_file, "Crate") == [-10, 3, 0], f"save {saves}: the file holds the Crate's own pose")
        await asyncio.sleep(0.01)
    r = await shot
    check(not r.is_error, r.text)
    check(saves >= 2, f"saved while the call ran: {saves}")
    check(near((await world(ed, "Crate"))[0], [-10, 3, 0], 1e-6), "the Crate is where it was")

    # the call cancelled while the image is taken: the job still ends and the poses go back
    seen = False
    for attempt in range(40):
        call = asyncio.ensure_future(ed.call_raw("place_with_physics", {"entities": ["Crate"], "screenshot": True, "max_seconds": 30}, timeout=600))
        job = None
        for _ in range(500):
            jobs = await ed.call("get_job")
            running = [j for j in jobs["jobs"] if j["kind"] == "place_with_physics" and j["status"] == "running"]
            if running and "screenshot" in running[0].get("message", ""):
                job = running[0]["job_id"]
                call.cancel()
                break
            if call.done():
                break
            await asyncio.sleep(0.005)
        with contextlib.suppress(BaseException):
            await call
        if job:
            state = await wait_job(ed, job, 60)
            if state["status"] == "succeeded" and "error" in state["result"].get("screenshot", {}):
                check("call ended" in state["result"]["screenshot"]["error"], f"{state['result']['screenshot']}")
                seen = True
        after = await ed.call("get_entity", entity="Crate")
        check(after["world"] == before["world"], f"attempt {attempt}: the preview poses went back")
        if seen:
            break
    check(seen, "a call cancelled during the capture ended its job without an image")

    # apply with a screenshot shows the committed poses; wait: false has no image to return
    r = await place(ed, entities=["Crate"], apply=True, screenshot=True)
    check(len(r.images) == 1 and r.structured["screenshot"]["shows"] == "the committed poses", f"{r.structured.get('screenshot')}")
    await ed.call("undo")
    r = await ed.call_raw("place_with_physics", {"entities": ["Crate"], "screenshot": True, "wait": False})
    state = await wait_job(ed, r.structured["job_id"], 60)
    check("skipped" in state["result"]["screenshot"]["error"], f"wait: false: {state['result']}")


def saved_position(scene_file: Path, name: str) -> list:
    """The Transform position of the top-level GameObject `name` in a scene file."""
    data = yaml.safe_load(scene_file.read_text())
    for entity in data["entities"]:
        if entity.get("name") == name:
            return entity["components"]["transform"]["position"]
    fail(f"{name} is not in {scene_file}")
    return []


async def test_scatter_batch_deterministic(ed: Editor):
    pebbles = [{"name": f"Pebble{i}", "type": "sphere", "position": [-20 + (i % 5) * 0.8, 2 + (i // 5) * 0.6, -20], "scale": [0.4, 0.4, 0.4]}
               for i in range(15)]
    paths = [f"Pebble{i}" for i in range(15)]

    async def scatter() -> tuple[dict, dict]:
        calls = [("create_entity", p) for p in pebbles]
        calls.append(("place_with_physics", {"entities": paths, "jitter": {"position": 0.3, "rotation_deg": 45}, "seed": 7, "apply": True}))
        result = await ed.batch(calls, undo_group=True)
        check(result["failed"] == 0, f"the batch: {result}")
        placed = result["results"][-1]["structured"]
        check(placed["applied"] and placed["seed"] == 7, f"placed: {placed}")
        return placed, await poses(ed, paths)

    first, first_poses = await scatter()
    check(all(p[0][1] < 1.0 for p in first_poses.values()), f"every pebble came down: {first_poses}")
    # one undo removes the pebbles and the placement together
    await ed.call("undo")
    tree = await ed.call("get_scene_tree")
    check("Pebble" not in tree["tree"], "one undo step for the whole batch")
    second, second_poses = await scatter()
    # new GameObjects (new ids, which state_hash includes), the same arrangement to the bit
    check(second_poses == first_poses, f"the same scatter again: the same poses ({first_poses} vs {second_poses})")
    check([e["after"] for e in second["entities"]] == [e["after"] for e in first["entities"]], "the same result poses")
    third = (await place(ed, entities=paths, jitter={"position": 0.3, "rotation_deg": 45}, seed=8)).structured
    check(third["state_hash"] != second["state_hash"], "another seed, another arrangement")
    await ed.call("undo")


async def test_separate_resolves_overlap(ed: Editor):
    r = await place(ed, entities=["OverA", "OverB"], mode="separate", apply=True)
    v = r.structured
    a, b = by_path(v, "OverA")["after"]["position"], by_path(v, "OverB")["after"]["position"]
    gap = max(abs(a[i] - b[i]) for i in range(3))
    check(gap >= 0.999, f"no longer overlapping (1 m cubes, centre distance on some axis >= 1 m - 1 mm): {a} {b}")
    check(v["all_settled"] is True, f"resolved: {v}")
    check(a[1] > 0.45 and b[1] > 0.45, f"no gravity: they stay up on the floor: {a} {b}")
    await ed.call("undo")


async def test_move_stops_at_wall(ed: Editor):
    r = await place(ed, entities=["Slider"], mode="move", target={"delta": [4.5, 0, 0]})
    pos = by_path(r.structured, "Slider")["after"]["position"]
    check(2.5 < pos[0] <= 3.25 + 0.01, f"it slid toward the target and stopped at the wall's face (x 3.75 - 0.5): {pos}")
    check(abs(pos[2] - 10) < 0.1 and abs(pos[1] - 0.5) < 0.01, f"along its lane, on the floor: {pos}")
    # a target far behind the wall: batch move never teleports, the wall still stops it
    r = await place(ed, entities=["Slider"], mode="move", target={"delta": [30, 0, 0]})
    far = by_path(r.structured, "Slider")
    check(2.5 < far["after"]["position"][0] <= 3.25 + 0.01, f"a long move stops at the wall too: {far}")
    # a free path: it gets there
    r = await place(ed, entities=["Slider"], mode="move", target={"position": [0, 0.5, 13]})
    pos = by_path(r.structured, "Slider")["after"]["position"]
    check(near(pos, [0, 0.5, 13], 0.02), f"no obstacle: it reaches the target: {pos}")


async def test_snap_lands_on_surface(ed: Editor):
    r = await place(ed, entities=["Snapper"], mode="snap", ray={"origin": [-10, 10, -10], "direction": [0, -1, 0]})
    v = r.structured
    pos = by_path(v, "Snapper")["after"]["position"]
    check(abs(pos[1] - 1.5) < 0.01 and abs(pos[0] + 10) < 0.1 and abs(pos[2] + 10) < 0.1, f"on the table top (y 1), where the ray hits: {pos}")
    check(any(e["path"] == "Table" for e in by_path(v, "Snapper")["resting_on"]), "resting on the table")
    check(near(v["ray"]["direction"], [0, -1000, 0], 1e-3), f"the ray used: {v.get('ray')}")
    # a Scene view pixel: the centre of a view looking straight down at the table
    await ed.call("set_scene_camera", position=[-10, 25, -9.999], target=[-10, 0, -10])
    shot = await ed.screenshot("scene", max_width=0)
    w, h, _, _ = decode_png(shot)
    r = await place(ed, entities=["Snapper"], mode="snap", ray={"screen": [w / 2, h / 2]}, screenshot=True)
    v = r.structured
    check(v["screenshot"]["shows"] == "the previewed poses", "a screen ray keeps the camera (no framing)")
    pos = by_path(v, "Snapper")["after"]["position"]
    check(abs(pos[0] + 10) < 1.0 and abs(pos[2] + 10) < 1.0 and pos[1] > 1.2, f"onto the table under the view's centre: {pos}")
    again = (await place(ed, entities=["Snapper"], mode="snap", ray={"screen": [w / 2, h / 2]}, apply=True)).structured
    check(again["state_hash"] == v["state_hash"], "the same pixel, the same ray: apply matches the preview")
    await ed.call("undo")


async def test_decompose_always(ed: Editor):
    await ed.call("create_entity", name="Ring", type="torus", position=[20, 3, -20])
    messages = []
    r = await ed.call_raw("place_with_physics", {"entities": ["Ring"], "decompose": "always", "wait": False})
    job = r.structured["job_id"]
    for _ in range(3000):
        state = await ed.call("get_job", job_id=job)
        messages.append(state.get("message", ""))
        if state["status"] != "running":
            break
        await asyncio.sleep(0.01)
    check(state["status"] == "succeeded", f"{state}")
    ring = by_path(state["result"], "Ring")
    check(ring["collider"].startswith("decomposed"), f"the torus uses its decomposition: {ring['collider']}")
    check(any(m.startswith("Decomposing") for m in messages), f"progress showed the decomposition: {sorted(set(messages))[:10]}")
    # cached now: the same arguments give the same result without waiting, and so does the
    # default (decompose: cached)
    again = (await place(ed, entities=["Ring"], decompose="always")).structured
    check(again["state_hash"] == state["result"]["state_hash"], "the cached decomposition gives the same result")
    cached = (await place(ed, entities=["Ring"])).structured
    check(cached["state_hash"] == state["result"]["state_hash"] and not any("decompose: always" in w for w in cached["warnings"]),
          f"decompose: cached uses it: {cached['warnings']}")

    # another concave mesh, not decomposed yet: decompose: cached uses a hull and warns that an
    # apply could differ once the decomposition is in
    await ed.call("create_entity", name="Ring2", type="torus", position=[24, 3, -20])
    await ed.call("set_mesh_geometry", entity="Ring2", shape="torus", radius=1.0, ring_radius=0.15, sides=40, rings=24)
    fresh = (await place(ed, entities=["Ring2"])).structured
    ring2 = by_path(fresh, "Ring2")
    check(not ring2["collider"].startswith("decomposed"), f"not decomposed yet: a hull meanwhile: {ring2['collider']}")
    check(any("decompose: always" in w for w in fresh["warnings"]), f"the preview warns: {fresh['warnings']}")
    await ed.call("delete_entities", entities=["Ring", "Ring2"])
