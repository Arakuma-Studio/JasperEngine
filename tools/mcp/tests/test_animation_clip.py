"""Animation clips over MCP (animation clips spec 5, 9.R4/R28/R29): edit_animation's clip-file
mode (create, create {from}, tracks by path, root, events + events_mode, wrap, frame_rate, length,
the two-way rejection table, all or nothing, results with bindings / users / the undo scope),
get_animation {clip, times, root} and {entity} with players, preview_animation on an Animation
Player (state, bound transforms), edit_pose_animation bake.clip, the .anim parts of the asset
tools (list_assets type animation, get_asset_info, view_asset, write_file validation,
move_asset / delete_asset), the Animation Player component through the generic tools (folded
from T2a's spot check) and play mode: runtime movement and an event reaching a Lua script method.
Export emits the component. Runs on a scratch copy of tools/fixtures/pose-animation (Hero: Mixamo
names, Robot: Blender names)."""

from __future__ import annotations

import asyncio
import json
import math
import time

import harness
from harness import Editor, REPO

EDITOR = {"project": REPO / "tools" / "fixtures" / "pose-animation"}
TIMEOUT = 1500

BOB = "assets/anim/bob.anim"
WALK_SUB = "assets/models/hero_animated.gltf#Walk"
S: dict = {}


# ---------------------------------------------------------------------------- helpers

async def edit(ed: Editor, **args):
    """edit_animation that must succeed: (structured result, text)"""
    r = await ed.call_raw("edit_animation", args)
    ed.check(not r.is_error, f"edit_animation {json.dumps(args)[:400]} failed: {r.text[:1500]}")
    return r.structured, r.text


async def refused(ed: Editor, tool: str, args: dict, contains: str) -> str:
    r = await ed.call_raw(tool, args)
    ed.check(r.is_error, f"{tool} {json.dumps(args)[:300]} should fail, got: {r.text[:500]}")
    ed.check(contains.lower() in r.text.lower(), f"{tool} error should mention {contains!r}: {r.text[:600]}")
    return r.text


def file_text(ed: Editor, rel: str) -> str:
    return (ed.project / rel).read_text()


def file_json(ed: Editor, rel: str) -> dict:
    return json.loads(file_text(ed, rel))


def approx(a, b, eps=1e-4) -> bool:
    if isinstance(a, (list, tuple)):
        return len(a) == len(b) and all(approx(x, y, eps) for x, y in zip(a, b))
    return abs(a - b) <= eps


def quad_out(t: float) -> float:
    return 1 - (1 - t) * (1 - t)


async def make_cube(ed: Editor, name: str, position=(0, 0, 0)) -> int:
    made = await ed.call("create_entity", type="cube", name=name, position=list(position))
    return made["entity_id"]


async def local_position(ed: Editor, entity) -> list:
    got = await ed.call("get_entity", entity=entity, components=["transform"])
    return harness.find_key(got, "position")


async def wait_job(ed: Editor, job_id: str, timeout: float, poll: float = 3.0) -> dict:
    deadline = time.monotonic() + timeout
    while True:
        job = await ed.call("get_job", job_id=job_id, log_lines=10)
        if job["status"] != "running":
            return job
        if time.monotonic() > deadline:
            ed.fail(f"job {job_id} still running after {timeout}s: {job}")
        await asyncio.sleep(poll)


BOB_TRACKS = [
    {"path": "", "channel": "position", "keys": [{"time": 0, "value": [0, 0, 0]}, {"time": 0.5, "value": [0, 2, 0], "ease": "quad_out"},
                                                  {"time": 1, "value": [0, 0, 0]}]},
    {"path": "", "channel": "rotation", "keys": [{"time": 0, "value": {"euler": [0, 0, 0]}}, {"time": 1, "value": {"euler": [0, 90, 0]}}]},
]


# ---------------------------------------------------------------------------- create, read

async def test_tools_and_schema(ed: Editor):
    ed.require_tools("edit_animation", "get_animation", "preview_animation", "edit_pose_animation", "list_assets")
    props = ed.tool("edit_animation").input_schema["properties"]
    for name in ("clip", "create", "root", "events", "events_mode", "wrap", "frame_rate", "length"):
        ed.check_in(name, props, "edit_animation clip-mode argument")
    ed.check_in("path", props["tracks"]["items"]["properties"], "tracks[].path")
    ed.check("jasper://docs/animation" in ed.tool("edit_animation").description, "the description points to the guide")
    get = ed.tool("get_animation").input_schema
    for name in ("clip", "times", "root", "entity"):
        ed.check_in(name, get["properties"], "get_animation argument")
    ed.check("entity" not in get.get("required", []), "get_animation entity is optional (clip reads a file)")
    ed.check_in("state", ed.tool("preview_animation").input_schema["properties"], "preview_animation state")
    ed.check_in("animation", ed.tool("list_assets").input_schema["properties"]["type"]["enum"], "list_assets type animation")
    bake = ed.tool("edit_pose_animation").input_schema["properties"]["bake"]["properties"]
    ed.check_in("clip", bake, "bake.clip")
    ed.check("Clips (.anim, shared): edit_animation {clip, create, tracks [{path}]}" in ed.instructions, "the instructions bullet")
    ed.check("jasper://docs/animation" in ed.instructions, "the instructions name the guide")


async def test_create_and_read(ed: Editor):
    (ed.project / "assets" / "anim").mkdir(parents=True, exist_ok=True)
    out, text = await edit(ed, clip=BOB, create={}, wrap="loop", frame_rate=24, tracks=BOB_TRACKS,
                           events=[{"time": 0.5, "function": "onBob", "args": ["up", 2, True]}])
    ed.check_eq(out["clip"], BOB, "project-relative clip")
    ed.check(out["created"] and not out["read_only"], f"created: {out}")
    ed.check_eq((out["wrap"], out["frame_rate"], out["length"]), ("loop", 24, 1), "settings")
    ed.check_eq([(t["index"], t["path"], t["channel"]) for t in out["tracks"]], [(0, "", "position"), (1, "", "rotation")], "tracks")
    pos = out["tracks"][0]
    ed.check_eq(pos["key_count"], 3, "keys")
    ed.check_eq(pos["keys"][1], {"time": 0.5, "value": [0, 2, 0], "ease": "quad_out"}, "a key as given")
    ed.check(pos["loop"] is False and pos["relative"] is False, "track flags")
    rot = out["tracks"][1]["keys"][1]["value"]
    ed.check(approx(rot["euler"], [0, 90, 0], 1e-3) and approx(rot["quat"], [0, math.sqrt(0.5), 0, math.sqrt(0.5)], 1e-5),
             f"euler keys are written as quaternions: {rot}")
    ed.check_eq(out["events"], [{"time": 0.5, "function": "onBob", "args": ["up", 2, True]}], "events")
    ed.check_eq(out["users"], 0, "no users yet")
    ed.check("bindings" not in out, "no root, no user: no binding report")
    ed.check(out["changes"] and out["warnings"] == [], f"changes and warnings: {out}")
    ed.check("one project undo step" in text and "undo scope project" in text, f"the text names the undo scope: {text}")
    ed.check("no Animation Player uses it yet" in text, text)
    data = file_json(ed, BOB)
    ed.check_eq((data["format"], data["version"], data["wrap"], data["frame_rate"]), ("jasperengine.anim", 1, "loop", 24), "the file")
    ed.check(len(data.get("guid", "")) == 32, f"a new clip has a GUID: {data.get('guid')}")
    S["bob_guid"] = data["guid"]
    ed.check_eq(data["tracks"][0]["keys"][1], [0.5, [0, 2, 0], "quad_out"], "the file's key form")

    got = await ed.call("get_animation", clip=BOB)
    ed.check_eq((got["clip"], got["guid"], got["read_only"]), (BOB, S["bob_guid"], False), "get_animation reads the file")
    ed.check_eq(got["tracks"], out["tracks"], "the same tracks")
    ed.check_eq(got["users"], [], "users list")

    # a model sub-clip reads too (read-only, no scene needed)
    sub = await ed.call("get_animation", clip=WALK_SUB, max_keys=2)
    ed.check(sub["read_only"] and sub["clip"] == WALK_SUB and len(sub["tracks"]) > 3, f"a model clip: {json.dumps(sub)[:500]}")
    ed.check(all(len(t["keys"]) <= 2 for t in sub["tracks"]), "max_keys caps the keys listed")
    ed.check(any(t["key_count"] > 2 for t in sub["tracks"]), "key_count is the real count")
    S["walk_paths"] = [t["path"] for t in sub["tracks"]]


async def test_times_sampling(ed: Editor):
    got = await ed.call("get_animation", clip=BOB, times=[0, 0.25, 0.5, 0.75, 1.25])
    samples = got["samples"]
    ed.check_eq(samples["times"], [0, 0.25, 0.5, 0.75, 1.25], "the times")
    pos = next(t for t in samples["tracks"] if t["channel"] == "position")
    ys = [v[1] for v in pos["values"]]
    # a key's ease shapes the segment that starts at it: 0 -> 2 linear over [0, 0.5], then back to
    # 0 with quad_out (the key at 0.5); loop: 1.25 samples 0.25
    expected = [0, 1, 2, 2 - 2 * quad_out(0.5), 1]
    ed.check(approx(ys, expected, 1e-4), f"sampled y {ys}, hand-computed {expected}")
    rot = next(t for t in samples["tracks"] if t["channel"] == "rotation")
    ed.check(approx(rot["values"][2]["euler"][1], 45, 0.05), f"rotation at 0.5 s is halfway (slerp): {rot['values'][2]}")
    await refused(ed, "get_animation", {"clip": BOB, "times": "0.5"}, "times must be")
    await refused(ed, "get_animation", {"clip": BOB, "times": [-1]}, ">= 0")
    await refused(ed, "get_animation", {"times": [0]}, "pass clip")
    await refused(ed, "get_animation", {"root": "Hero"}, "pass clip")
    await refused(ed, "get_animation", {}, "Pass entity")
    await refused(ed, "get_animation", {"clip": "assets/anim/nope.anim"}, "does not exist")


# ---------------------------------------------------------------------------- the two-way rejection table

async def test_file_mode_rejections(ed: Editor):
    before = file_text(ed, BOB)
    for arg, value, contains in (
            ("loop", True, "wrap"), ("duration", 2, "length"), ("tweens", [{"type": "position", "to": [1, 0, 0]}], "scene actions"),
            ("add_actions", [{"action": 1}], "without clip"), ("set_frames", [{"index": 0}], "shift the keys"),
            ("remove_actions", [0], "keys: []"), ("target", "Hero", "tracks[].path"), ("autoplay", True, "playAutomatically"),
            ("speed", 2, "Animation Player"), ("animation", "Hero", "not both"), ("name", "x", "clips[i].name")):
        await refused(ed, "edit_animation", {"clip": BOB, arg: value}, contains)
    track = {"path": "", "channel": "position", "keys": [{"time": 0, "value": [0, 0, 0]}]}
    for change, contains in (
            ({"start_time": 1}, "shift the keys"), ({"name": "x"}, "path and channel"), ({"path": "a//b"}, "malformed"),
            ({"path": 3}, "must be a string"), ({"channel": "colour"}, "Use one of"), ({"channel": "property"}, "reserved"),
            ({"channel": "morph", "relative": True, "keys": [{"time": 0, "value": [1]}]}, "only position, rotation and scale"),
            ({"target": "Hero"}, "give path or target"), ({"path": None, "target": "Hero"}, "turn target into a path"),
            ({"mode": "add"}, "replace or merge"), ({"keys": [{"time": -1, "value": [0, 0, 0]}]}, "time must be"),
            ({"keys": [{"time": 0, "value": [0, 0]}]}, "[x, y, z]"), ({"keys": [{"time": 0, "value": [0, 0, 0], "ease": "wobbly"}]}, "Unknown ease"),
            ({"channel": "lattice", "keys": [{"time": 0, "points": "current"}]}, "pass root"),
            ({"channel": "lattice", "keys": [{"time": 0, "points": [[[1, 0, 0], [0, 1, 0]]]}]}, "grid coordinates"),
            ({"channel": "lattice", "keys": [{"time": 0, "points": "rest"}]}, "rest"),
            ({"path": "Nowhere", "keys": []}, "no Nowhere : position track"), ({"keys": [], "mode": "merge"}, "merge adds keys")):
        t = dict(track)
        t.update(change)
        t = {k: v for k, v in t.items() if v is not None}
        await refused(ed, "edit_animation", {"clip": BOB, "tracks": [t]}, contains)
    for args, contains in (
            ({"wrap": "sideways"}, "once, loop, ping_pong or clamp_forever"), ({"frame_rate": 0}, "1 to 240"),
            ({"length": "long"}, "must be a number"), ({"events": {"time": 0}}, "array"),
            ({"events": [{"time": 0, "function": "on-bob"}]}, "Lua identifier"),
            ({"events": [{"time": 0, "function": "f", "args": list(range(9))}]}, "at most 8"),
            ({"events": [{"time": 0, "function": "f", "args": [[1]]}]}, "number, string or boolean"),
            ({"events": [{"time": 0, "function": "f", "when": 1}]}, "unknown key"),
            ({"events": [{"function": "f"}]}, "time must be"),
            ({"events_mode": "merge"}, "needs events"), ({"events": [], "events_mode": "append"}, "replace"),
            ({"create": {}}, "already exists"), ({"create": {"form": "x"}}, "unknown key"),
            ({}, "Nothing to do"), ({"root": "Nobody"}, "root")):
        await refused(ed, "edit_animation", {"clip": BOB, **args}, contains)
    await refused(ed, "edit_animation", {"clip": "assets/anim/missing.anim", "wrap": "loop"}, "create: {}")
    await refused(ed, "edit_animation", {"clip": "assets/anim/bob.txt", "create": {}}, "must end in .anim")
    await refused(ed, "edit_animation", {"clip": "../outside.anim", "create": {}}, "clip")
    await refused(ed, "edit_animation", {"clip": "assets/nofolder/x.anim", "create": {}}, "create_folder")
    await refused(ed, "edit_animation", {"clip": WALK_SUB, "wrap": "loop"}, "read-only")
    # all or nothing: a valid track and a bad event change nothing
    await refused(ed, "edit_animation", {"clip": BOB, "tracks": [{"path": "Arm", "channel": "scale", "keys": [{"time": 0, "value": [2, 2, 2]}]}],
                                         "events": [{"time": 0, "function": "1bad"}]}, "nothing was changed")
    ed.check_eq(file_text(ed, BOB), before, "a refused call writes nothing")


async def test_scene_mode_rejections(ed: Editor):
    for arg, value in (("wrap", "loop"), ("length", 1), ("frame_rate", 30), ("events", []), ("events_mode", "merge"),
                       ("root", "Hero"), ("create", {})):
        await refused(ed, "edit_animation", {"target": "Hero", arg: value}, "clip-file setting")
    await refused(ed, "edit_animation", {"target": "Hero", "tracks": [{"path": "", "channel": "position",
                                                                         "keys": [{"time": 0, "value": [0, 0, 0]}]}]}, "pass clip")
    await refused(ed, "preview_animation", {"animation": "Robot", "state": "Walk"}, "no Animation Player")


# ---------------------------------------------------------------------------- edits

async def test_merge_replace_remove_and_settings(ed: Editor):
    out, text = await edit(ed, clip=BOB, tracks=[{"path": "", "channel": "position", "mode": "merge",
                                                 "keys": [{"time": 0.25, "value": [0, 1, 0]}, {"time": 1, "value": [0, 0.5, 0]}]}])
    keys = out["tracks"][0]["keys"]
    ed.check_eq([k["time"] for k in keys], [0, 0.25, 0.5, 1], "merge adds and replaces by time")
    ed.check_eq(keys[3]["value"], [0, 0.5, 0], "the key at 1 s replaced")
    out, _ = await edit(ed, clip=BOB, tracks=[{"path": "Arm/Hand", "channel": "scale", "loop": True,
                                              "keys": [{"time": 0, "value": [1, 1, 1]}, {"time": 0.2, "value": [2, 2, 2], "ease": "step"}]}])
    hand = next(t for t in out["tracks"] if t["path"] == "Arm/Hand")
    ed.check(hand["loop"] and hand["keys"][1]["ease"] == "step", f"a new track: {hand}")
    ed.check_eq([t["path"] for t in out["tracks"]], ["", "", "Arm/Hand"], "tracks sorted by path")
    out, _ = await edit(ed, clip=BOB, tracks=[{"path": "Arm/Hand", "channel": "scale", "keys": []}])
    ed.check("Arm/Hand" not in [t["path"] for t in out["tracks"]], "keys [] removes the track")
    ed.check(any("removed track Arm/Hand : scale" in c for c in out["changes"]), out["changes"])
    # events: merge replaces the same function at the same time, replace sets the list
    out, _ = await edit(ed, clip=BOB, events_mode="merge", events=[{"time": 0.5, "function": "onBob", "args": ["down"]},
                                                                   {"time": 0.9, "function": "onLand"}])
    ed.check_eq([(e["time"], e["function"], e["args"]) for e in out["events"]], [(0.5, "onBob", ["down"]), (0.9, "onLand", [])], "events merged")
    out, _ = await edit(ed, clip=BOB, events=[{"time": 0.1, "function": "onBob", "args": ["up"]}])
    ed.check_eq(len(out["events"]), 1, "events replaced")
    # settings
    out, _ = await edit(ed, clip=BOB, length=1.5, wrap="ping_pong")
    ed.check_eq((out["length"], out["explicit_length"], out["wrap"]), (1.5, True, "ping_pong"), "explicit length")
    out, _ = await edit(ed, clip=BOB, length=-1, wrap="loop", frame_rate=30)
    ed.check_eq((out["length"], out["explicit_length"], out["frame_rate"]), (1, False, 30), "automatic length again")
    # the same keys again: nothing is written
    same = next(t for t in out["tracks"] if t["channel"] == "position")
    before = file_text(ed, BOB)
    out, text = await edit(ed, clip=BOB, tracks=[{"path": "", "channel": "position", "keys": same["keys"]}])
    ed.check("no change: nothing written" in text, text)
    ed.check_eq(file_text(ed, BOB), before, "unchanged file")


async def test_project_undo_vs_scene_undo(ed: Editor):
    before = await ed.call("get_animation", clip=BOB)
    await edit(ed, clip=BOB, wrap="once")
    ed.check_eq((await ed.call("get_animation", clip=BOB))["wrap"], "once", "edited")
    await ed.call("undo", scope="project")
    ed.check_eq((await ed.call("get_animation", clip=BOB))["wrap"], before["wrap"], "undo scope project restores the clip")
    await ed.call("redo", scope="project")
    ed.check_eq((await ed.call("get_animation", clip=BOB))["wrap"], "once", "redo")
    await ed.call("undo", scope="project")
    # the reference is a scene step
    S["box_a"] = await make_cube(ed, "BoxA", (3, 0, 0))
    await ed.call("add_component", entity=S["box_a"], component="animationplayer",
                  values={"animationplayer": {"clips": [{"clip": BOB}]}})
    await ed.call("undo", scope="scene")
    got = await ed.call("get_entity", entity=S["box_a"])
    ed.check("animationplayer" not in json.dumps(got).lower(), "undo scope scene removed the player")
    ed.check_eq((await ed.call("get_animation", clip=BOB))["wrap"], before["wrap"], "the clip is untouched by the scene undo")
    await ed.call("redo", scope="scene")


# ---------------------------------------------------------------------------- sharing, players

async def test_sharing_one_clip(ed: Editor):
    S["box_b"] = await make_cube(ed, "BoxB", (5, 0, 0))
    await ed.call("add_component", entity=S["box_b"], component="animationplayer",
                  values={"animationplayer": {"clips": [{"clip": BOB, "name": "Bob", "wrap": "loop"}]}})
    # a second scene with a third user
    made = await ed.call("create_scene", name="Second", type="3D", path="Second")
    S["second"] = made["scene"]["id"]
    third = (await ed.call("create_entity", scene=S["second"], type="cube", name="BoxC"))["entity_id"]
    await ed.call("add_component", scene=S["second"], entity=third, component="animationplayer",
                  values={"animationplayer": {"clips": [{"clip": BOB}]}})
    await ed.call("select_scene", scene="Main Scene")
    out, text = await edit(ed, clip=BOB, tracks=[{"path": "", "channel": "scale", "keys": [{"time": 0, "value": [1, 1, 1]},
                                                                                           {"time": 1, "value": [1.5, 1.5, 1.5]}]}])
    ed.check_eq(out["users"], 3, f"three Animation Players share the file: {text}")
    ed.check("shared by 3 components (the edit changes every one)" in text, text)
    ed.check("bindings" in out and out["bindings"]["resolved"] == out["bindings"]["total"] == 3, f"bindings against the first user: {out.get('bindings')}")
    got = await ed.call("get_animation", clip=BOB)
    ed.check_eq(len(got["users"]), 3, f"users listed: {got['users']}")
    ed.check(any(u.get("state") == "Bob" for u in got["users"]), f"the named state: {got['users']}")
    for entity in (S["box_a"], S["box_b"]):
        info = await ed.call("get_animation", entity=entity)
        ed.check_eq(len(info["players"]), 1, f"players: {info}")
        state = info["players"][0]["states"][0]
        ed.check_eq(state["clip"], BOB, "the state's clip")
        ed.check_eq(info["players"][0]["bindings"][0]["resolved"], 3, f"every track bound: {info['players'][0]['bindings']}")
        ed.check_eq(info["animations"], [], "no entity animations")
    bob = (await ed.call("get_animation", entity=S["box_b"]))["players"][0]["states"][0]
    ed.check_eq((bob["name"], bob["wrap"]), ("Bob", "loop"), "state name and wrap")
    await refused(ed, "get_animation", {"entity": "Robot"}, "no Animation Player plays it")


async def test_preview_player(ed: Editor):
    rest = await local_position(ed, S["box_a"])
    r = await ed.call_raw("preview_animation", {"animation": S["box_a"], "action": "seek", "time": 0.5})
    ed.check(not r.is_error, r.text)
    data = r.structured
    ed.check(data["previewing"] and data["state"] == "bob" and data["clip"] == BOB, f"preview: {data}")
    ed.check_eq(len(data["bound"]), 1, f"one bound path (the root): {data['bound']}")
    bound = data["bound"][0]
    sampled = await ed.call("get_animation", clip=BOB, times=[0.5])
    pos = next(t for t in sampled["samples"]["tracks"] if t["channel"] == "position")["values"][0]
    ed.check(approx(bound["position"], pos, 1e-3), f"the preview shows the sampled value: {bound['position']} vs {pos}")
    ed.check_eq(bound["path"], "", "the root's path")
    ed.check(approx(await local_position(ed, S["box_a"]), pos, 1e-3), "the scene shows the preview")
    r = await ed.call_raw("preview_animation", {"animation": S["box_a"], "action": "play", "wait_ms": 300})
    ed.check(not r.is_error and r.structured["playing"], r.text)
    ed.check(r.structured["time"] != 0.5, f"playing advances the preview: {r.structured['time']}")
    r = await ed.call_raw("preview_animation", {"animation": S["box_a"], "action": "stop"})
    ed.check(not r.is_error and not r.structured["previewing"], r.text)
    ed.check(approx(await local_position(ed, S["box_a"]), rest, 1e-4), "stop restores the scene")
    await refused(ed, "preview_animation", {"animation": S["box_b"], "state": "Nope"}, "states: \"Bob\"")
    r = await ed.call_raw("preview_animation", {"animation": S["box_b"], "state": "Bob", "action": "seek", "time": 0.25})
    ed.check(not r.is_error and r.structured["state"] == "Bob", r.text)
    await ed.call("preview_animation", animation=S["box_b"], action="stop")


async def test_bindings_hero_robot(ed: Editor):
    # extract the model's Walk to an editable file
    out, text = await edit(ed, clip="assets/anim/walk.anim", create={"from": WALK_SUB})
    ed.check(out["created"] and len(out["tracks"]) == len(S["walk_paths"]), f"extracted: {text[:500]}")
    ed.check_eq(out["source"], {"kind": "model", "path": "models/hero_animated.gltf", "name": "Walk"}, "source")
    ed.check_eq(out["preview_model"], "assets/models/hero_animated.gltf", "preview model")
    ed.check("one project undo step" in text, text)
    # Hero and Robot play it: one rig matches, the other gets nearest names
    for name in ("Hero", "Robot"):
        await ed.call("add_component", entity=name, component="animationplayer",
                      values={"animationplayer": {"clips": [{"clip": "assets/anim/walk.anim"}]}})
    hero = await ed.call("get_animation", clip="assets/anim/walk.anim", root="Hero")
    b = hero["bindings"]
    ed.check(b["resolved"] == b["total"] == len(S["walk_paths"]) and b["missing"] == [], f"Hero binds every track: {json.dumps(b)[:800]}")
    robot = await ed.call("get_animation", clip="assets/anim/walk.anim", root="Robot")
    b = robot["bindings"]
    ed.check(b["resolved"] < b["total"] and b["missing"], f"Robot misses Mixamo paths: {json.dumps(b)[:800]}")
    miss = b["missing"][0]
    ed.check(miss["status"] == "missing_path" and miss["nearest"], f"a missing path with nearest names: {miss}")
    ed.check_eq(b["root"]["name"], "Robot", "the root")
    # edit_animation with root reports them too, and turns a target into a path
    some = S["walk_paths"][-1]
    leaf = some.split("/")[-1]
    out, text = await edit(ed, clip="assets/anim/walk.anim", root="Hero",
                           tracks=[{"target": leaf, "channel": "scale", "keys": [{"time": 0, "value": [1, 1, 1]}]}])
    ed.check(any(t["path"] == some and t["channel"] == "scale" for t in out["tracks"]), f"target -> path {some}: {[t['path'] for t in out['tracks']]}")
    ed.check(out["bindings"]["root"]["name"] == "Hero" and out["bindings"]["missing"] == [], f"bindings on root: {out['bindings']}")
    await refused(ed, "edit_animation", {"clip": "assets/anim/walk.anim", "root": "Hero",
                                         "tracks": [{"target": "Robot", "channel": "scale", "keys": [{"time": 0, "value": [1, 1, 1]}]}]}, "is not root")
    info = await ed.call("get_animation", entity="Robot")
    ed.check(info["players"][0]["bindings"][0]["missing"], "get_animation {entity} reports the missing paths")
    # get_animation of a bone: the player of its ancestor
    bone = await ed.call("get_animation", entity=leaf)
    ed.check(bone["players"] and bone["players"][0]["entity"]["name"] == "Hero" and bone["players"][0]["plays"]["name"] == leaf,
             f"a bone reports the player that animates it: {json.dumps(bone)[:400]}")
    for name in ("Robot", "Hero"):
        await ed.call("remove_component", entity=name, component="animationplayer")


async def test_create_from_entity_animation(ed: Editor):
    box = await make_cube(ed, "Mover", (0, 0, 4))
    made, _ = await edit(ed, target=box, tracks=[{"channel": "position", "keys": [{"time": 0, "value": [0, 0, 4]}, {"time": 2, "value": [0, 3, 4]}]}],
                         tweens=[{"type": "scale", "to": [2, 2, 2], "duration": 1}])
    animation = made["animation"]["id"]
    out, text = await edit(ed, clip="assets/anim/mover.anim", create={"from": animation}, wrap="loop")
    ed.check(out["created"] and out["added_player"] and out["player"]["name"] == "Mover", f"converted: {text}")
    ed.check("one scene undo step" in text and "undo scope scene" in text, text)
    ed.check_eq([(t["path"], t["channel"]) for t in out["tracks"]], [("", "position")], "keyframe tracks only")
    ed.check(any("not converted" in c for c in out["changes"]), f"the tween is listed: {out['changes']}")
    ed.check_eq(out["wrap"], "loop", "this call's edits are in the file")
    sampled = await ed.call("get_animation", clip="assets/anim/mover.anim", times=[1])
    ed.check(approx(sampled["samples"]["tracks"][0]["values"][0], [0, 1.5, 4], 1e-3), f"same values: {sampled['samples']}")
    info = await ed.call("get_animation", entity=box)
    ed.check(info["players"] and info["players"][0]["states"][0]["clip"] == "assets/anim/mover.anim", f"the player: {info['players']}")
    await ed.call("undo", scope="scene")
    ed.check(not (ed.project / "assets/anim/mover.anim").exists(), "the scene undo removes the file it wrote")
    info = await ed.call("get_animation", entity=box)
    ed.check_eq(info["players"], [], "and the player")
    await refused(ed, "edit_animation", {"clip": "assets/anim/x.anim", "create": {"from": box}}, "not an entity Animation")


async def test_bake_to_clip(ed: Editor):
    pose = "assets/anim/wave.poseanim"
    await ed.call("edit_pose_animation", path=pose, create={"preview_model": "assets/models/hero_tpose.gltf"}, ops=[
        {"op": "add_pose", "name": "A", "angles": {"left_upper_arm.raise": 10}},
        {"op": "add_pose", "name": "B", "angles": {"left_upper_arm.raise": 60}}])
    r = await ed.call_raw("edit_pose_animation", {"path": pose, "bake": {"model": "Hero", "clip": True}})
    ed.check(not r.is_error, r.text)
    bake = r.structured["bake"]
    ed.check(bake["clip"].endswith(".anim") and bake["tracks"] > 10 and bake["added_player"], f"baked: {bake}")
    ed.check("one scene undo step" in r.text, r.text)
    clip = await ed.call("get_animation", clip=bake["clip"], root="Hero")
    ed.check(clip["bindings"]["missing"] == [] and clip["source"]["kind"] == "pose", f"bone paths bind on Hero: {json.dumps(clip['bindings'])[:500]}")
    info = await ed.call("get_animation", entity="Hero")
    ed.check(any(s["clip"] == bake["clip"] for s in info["players"][0]["states"]), "the model's player lists it")
    await ed.call("undo", scope="scene")
    ed.check(not (ed.project / bake["clip"]).exists(), "undo removes the baked clip")
    await refused(ed, "edit_pose_animation", {"path": pose, "bake": {"model": "Hero", "clip": "x.txt"}}, "bake.clip")


# ---------------------------------------------------------------------------- asset tools

async def test_asset_tools(ed: Editor):
    listed = await ed.call("list_assets", type="animation")
    paths = [e["path"] for e in listed["entries"]]
    ed.check(BOB in paths and "assets/anim/wave.poseanim" in paths, f"type animation: .anim and .poseanim: {paths}")
    ed.check(all(e["type"] == "animation" for e in listed["entries"]), "typed animation")
    r = await ed.call_raw("get_asset_info", {"path": BOB})
    info = r.structured
    ed.check_eq(info["type"], "animation", "type")
    clip = info["clip"]
    ed.check(clip["version"] == 1 and clip["guid"] == S["bob_guid"] and clip["wrap"] == "loop" and len(clip["tracks"]) == 3, f"clip info: {clip}")
    ed.check_eq(len(clip["users"]), 3, "users")
    ed.check("3 users" in r.text, r.text)
    await refused(ed, "view_asset", {"path": BOB}, "preview_animation")


async def test_write_file_validation(ed: Editor):
    rel = "assets/anim/hand.anim"
    for content, contains in (
            ('{"format": "jasperengine.anim", "version": 1,\n  "tracks": [\n}', "Not a valid animation clip: 3:"),
            ('{"format": "jasperengine.pose", "version": 1}', "Not a valid animation clip"),
            ('{"format": "jasperengine.anim", "version": 99}', "Not a valid animation clip"),
            ("%YAML 1.1\n--- !u!74 &7400000\nAnimationClip:\n  m_Name: Walk\n", "Unity .anim")):
        await refused(ed, "write_file", {"path": rel, "content": content}, contains)
    ed.check(not (ed.project / rel).exists(), "refused writes write nothing")
    content = json.dumps({"format": "jasperengine.anim", "version": 1, "wrap": "sideways",
                          "tracks": [{"path": "", "channel": "position", "keys": [[0, [0, 0, 0]], [1, [1, 0, 0]]]}]}, indent=2)
    r = await ed.call_raw("write_file", {"path": rel, "content": content})
    ed.check(not r.is_error, r.text)
    ed.check(r.structured["warnings"] and r.structured["warnings"][0].startswith("wrap:"), f"warnings: {r.structured}")
    ed.check("1 warning(s)" in r.text, r.text)
    got = await ed.call("get_animation", clip=rel)
    ed.check_eq(got["wrap"], "once", "the bad wrap was dropped")
    # the asset database gives a clip written without a GUID one
    await ed.call("sync_files")
    ed.check(len(file_json(ed, rel).get("guid", "")) == 32, "a GUID was inserted")
    # an outside edit: auto-sync reports the problem with its line
    (ed.project / rel).write_text('{"format": "jasperengine.anim",\n "version": 1,\n "tracks": [}\n')
    r = await ed.call_raw("get_animation", {"clip": BOB})
    problems = harness.find_key(r.structured, "problems") or []
    ed.check(any(p.get("path") == rel and p.get("line") == 3 for p in problems), f"external_changes problems: {problems}")


async def test_move_copy_delete(ed: Editor):
    await ed.call("create_folder", path="assets/clips")
    r = await ed.call_raw("move_asset", {"path": BOB, "to": "assets/clips/bob.anim"})
    ed.check(not r.is_error and "Animation Players follow it" in r.text, r.text)
    state = (await ed.call("get_animation", entity=S["box_a"]))["players"][0]["states"][0]
    ed.check_eq(state["clip"], "assets/clips/bob.anim", "the player follows the move")
    await ed.call("undo", scope="project")
    state = (await ed.call("get_animation", entity=S["box_a"]))["players"][0]["states"][0]
    ed.check_eq(state["clip"], BOB, "undo moves it back")
    r = await ed.call_raw("move_asset", {"path": BOB, "to": "assets/clips/bob2.anim", "copy": True})
    ed.check(not r.is_error and "new clip with its own GUID" in r.text, r.text)
    copy_guid = file_json(ed, "assets/clips/bob2.anim")["guid"]
    ed.check(copy_guid and copy_guid != S["bob_guid"], f"the copy has its own GUID: {copy_guid}")
    await ed.call("undo", scope="project")
    ed.check(not (ed.project / "assets/clips/bob2.anim").exists(), "undo removes the copy")
    await ed.call("redo", scope="project")
    ed.check_eq(file_json(ed, "assets/clips/bob2.anim")["guid"], copy_guid, "redo gives the same GUID")
    r = await ed.call_raw("delete_asset", {"paths": ["assets/clips/bob2.anim"]})
    ed.check(not r.is_error and "clip_users" not in r.structured, r.text)
    r = await ed.call_raw("delete_asset", {"paths": [BOB]})
    ed.check(not r.is_error and r.structured.get("clip_users") == 3 and "keep the reference" in r.text, r.text)
    state = (await ed.call("get_animation", entity=S["box_a"]))["players"][0]["states"][0]
    ed.check(state.get("missing") and state["clip"] == BOB, f"a Missing reference: {state}")
    await ed.call("undo", scope="project")
    state = (await ed.call("get_animation", entity=S["box_a"]))["players"][0]["states"][0]
    ed.check(not state.get("missing"), f"undo relinks: {state}")


# ---------------------------------------------------------------------------- component plumbing (T2a)

async def test_component_values(ed: Editor):
    got = await ed.call("get_entity", entity=S["box_b"], components=["animationplayer"], detail="full")
    wrap = harness.find_key(got, "clips[0].wrap")
    ed.check(isinstance(wrap, dict) and wrap.get("type") == "enum" and "ping_pong" in wrap.get("options", []),
             f"clips[i].wrap is an enum with its names: {wrap}")
    await ed.call("set_properties", entity=S["box_b"], values={"animationplayer": {"clips[0].wrap": "ping_pong", "speed": 1.5}})
    state = (await ed.call("get_animation", entity=S["box_b"]))["players"][0]
    ed.check_eq((state["states"][0]["wrap"], state["speed"]), ("ping_pong", 1.5), "set_properties on the player")
    for values, why in (
            ({"clips[0].clip": "assets/anim/missing.anim"}, "does not exist"),
            ({"clips[0].clip": "assets/models/hero_tpose.gltf"}, ".anim file or <model>#<animation>"),
            ({"clips[0].wrap": "sideways"}, "wrap mode"),
            ({"clips": [{"clip": BOB, "loop": True}]}, "unknown key")):
        await refused(ed, "set_properties", {"entity": S["box_b"], "values": {"animationplayer": values}}, why)
    await ed.call("set_properties", entity=S["box_b"], values={"animationplayer": {"clips[0].wrap": "loop", "speed": 1}})

    # folded from T2a's spot check: the stored (asset-relative) form in, project-relative out, the
    # GUID follows the path and is read-only, an inserted entry, a model clip has no GUID
    await ed.call("set_properties", entity=S["box_b"], values={"animationplayer": {"clips[0].clip": "anim/walk.anim"}})
    got = await ed.call("get_entity", entity=S["box_b"], components=["animationplayer"])
    clips = harness.find_key(got, "clips")
    walk_guid = file_json(ed, "assets/anim/walk.anim")["guid"]
    ed.check_eq((clips[0]["clip"], clips[0].get("guid")), ("assets/anim/walk.anim", walk_guid), "stored form in, the guid follows")
    await ed.call("set_properties", entity=S["box_b"], values=[
        {"component": "animationplayer", "property": "clips", "insert": {"clip": WALK_SUB, "name": "Model Walk"}}])
    clips = harness.find_key(await ed.call("get_entity", entity=S["box_b"], components=["animationplayer"]), "clips")
    ed.check(len(clips) == 2 and clips[1]["clip"] == WALK_SUB and "guid" not in clips[1], f"an inserted model clip: {clips}")
    await refused(ed, "set_properties", {"entity": S["box_b"], "values": {"animationplayer": {"clips[0].guid": S["bob_guid"]}}}, "read-only")
    await ed.call("set_properties", entity=S["box_b"], values={"animationplayer": {"clips": [{"clip": BOB, "name": "Bob", "wrap": "loop"}]}})
    # undo of a component removal brings the clips back
    await ed.call("remove_component", entity=S["box_b"], component="animationplayer")
    await ed.call("undo", scope="scene")
    clips = harness.find_key(await ed.call("get_entity", entity=S["box_b"], components=["animationplayer"]), "clips")
    ed.check_eq([c["clip"] for c in clips], [BOB], "undo of remove_component")


# ---------------------------------------------------------------------------- play mode

SCRIPT = """local Bobber = {}

function Bobber:init()
    self.hits = 0
    local player = GameObject(self.scene, self.entity):getAnimationPlayer()
    player.onEvent = function(event)
        Log.print("onEvent " .. event.functionName .. " " .. tostring(event.args[1]))
    end
end

function Bobber:onBob(word, count)
    self.hits = self.hits + 1
    Log.print("bobbed " .. tostring(word) .. " " .. tostring(count))
end

return Bobber
"""


async def test_play_runtime_and_event(ed: Editor):
    await edit(ed, clip=BOB, events=[{"time": 0.25, "function": "onBob", "args": ["up", 2]}])
    await ed.call("create_script", kind="lua", name="Bobber", content=SCRIPT, entity=S["box_a"])
    rest = await local_position(ed, S["box_a"])
    await ed.console_since(None)
    await ed.call("play", action="start")
    try:
        await ed.call("wait", game_seconds=0.6)
        got = await ed.call("get_runtime_entity", entity=S["box_a"])
        playing = harness.find_key(got, "position")
        ed.check(playing is not None and not approx(playing, rest, 1e-3), f"the clip moves BoxA at runtime: {rest} -> {playing}")
        console = " ".join(str(e.get("message")) for e in await ed.console_since("last"))
        ed.check("bobbed up 2" in console, f"the event reached the script method with its args: {console[-800:]}")
        ed.check("onEvent onBob up" in console, f"onEvent got one AnimationEvent: {console[-800:]}")
    finally:
        await ed.call("play", action="stop")
    ed.check(approx(await local_position(ed, S["box_a"]), rest, 1e-4), "stopping play restores the scene")


async def test_export_emits_the_component(ed: Editor):
    await ed.call("save_scene", scene="Main Scene")
    text = (ed.project / "Main Scene.scene").read_text()
    ed.check("animationplayer:" in text and "anim/bob.anim" in text and "assets/anim/bob.anim" not in text,
             "saved asset-relative")
    out = ed.scratch.root / "export-clips"
    raw = await ed.call_raw("build_project", {"mode": "source", "output_dir": str(out), "platform": "linux",
                                              "overwrite": True, "timeout_seconds": 900})
    r = raw.value
    if isinstance(r, dict) and r.get("status") == "running":
        r = await wait_job(ed, r["job_id"], 900)
    ed.check_eq(r["status"], "succeeded", raw.text[:2000])
    sources = [p for p in out.rglob("*.cpp") if "AnimationPlayerComponent" in p.read_text(errors="ignore")]
    ed.check(sources, "no AnimationPlayerComponent in the generated scene code")
    code = "\n".join(p.read_text(errors="ignore") for p in sources)
    ed.check('.clip = "anim/bob.anim";' in code, "the generated code sets the clip")
    ed.check(S["bob_guid"] not in code, "no GUID in the generated code")
    ed.check('resources.clips.push_back("anim/bob.anim");' in code, "the scene resources preload the clip")
    ed.check((out / "project" / "assets" / "anim" / "bob.anim").is_file(), "the .anim is exported")
