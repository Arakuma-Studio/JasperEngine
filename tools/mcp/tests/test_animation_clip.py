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
import os
import math
import re
import shutil
import time

import harness
from harness import Editor, REPO

EDITOR = {"project": REPO / "tools" / "fixtures" / "pose-animation"}
CI = True
TIMEOUT = 1500

BOB = "assets/anim/bob.anim"
WALK_SUB = "assets/models/hero_animated.gltf#Walk"
S: dict = {}


# ---------------------------------------------------------------------------- helpers

def bone_side(path: str):
    """The side a bone name says ("L", "R" or None), as ClipValidation::boneSide: the last segment's
    words (separators and camelCase), a word Left / Right or a lone L / R."""
    leaf = path.rsplit("/", 1)[-1]
    words = re.findall(r"[A-Z]+(?=[A-Z][a-z])|[A-Z]?[a-z]+|[A-Z]+|[0-9]+", leaf)
    sides = {"L" if w.lower() in ("left", "l") else "R" for w in words if w.lower() in ("left", "l", "right", "r")}
    return sides.pop() if len(sides) == 1 else None


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


def schema_words(schema, out: set) -> None:
    if isinstance(schema, dict):
        for key, value in schema.items():
            if key == "properties" and isinstance(value, dict):
                out.update(value.keys())
            if key == "enum" and isinstance(value, list):
                out.update(v for v in value if isinstance(v, str))
            schema_words(value, out)
    elif isinstance(schema, list):
        for item in schema:
            schema_words(item, out)


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
    key_item = props["tracks"]["items"]["properties"]["keys"]["items"]
    ed.check(key_item.get("additionalProperties", True) is not False, f"keys items allow in/out (tangents): {key_item}")
    ed.check("jasper://docs/animation" in ed.tool("edit_animation").description, "the description points to the guide")
    get = ed.tool("get_animation").input_schema
    for name in ("clip", "times", "root", "entity"):
        ed.check_in(name, get["properties"], "get_animation argument")
    ed.check("entity" not in get.get("required", []), "get_animation entity is optional (clip reads a file)")
    ed.check_in("state", ed.tool("preview_animation").input_schema["properties"], "preview_animation state")
    ed.check_in("animation", ed.tool("list_assets").input_schema["properties"]["type"]["enum"], "list_assets type animation")
    bake = ed.tool("edit_pose_animation").input_schema["properties"]["bake"]["properties"]
    ed.check_in("clip", bake, "bake.clip")
    # agents review I5: Unity's legacy Animation is the Animation Player, not the entity Animation
    ed.check("Legacy Animation = AnimationPlayerComponent" in ed.instructions, "the naming rule names the legacy Animation")


async def test_create_and_read(ed: Editor):
    # agents review M1: the guide's first call in a fresh project makes the clip's folder (as the
    # pose tools do), no create_folder first
    ed.check(not (ed.project / "assets" / "anim").exists(), "the fixture has no assets/anim folder")
    out, text = await edit(ed, clip=BOB, create={}, wrap="loop", frame_rate=24, tracks=BOB_TRACKS,
                           events=[{"time": 0.5, "function": "onBob", "args": ["up", 2, True]}])
    ed.check("made the folder assets/anim" in out["changes"], f"the folder is made: {out['changes']}")
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
    ed.check_eq(out["users"], [], "no users yet")
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
    for sample in samples["tracks"]:
        track = got["tracks"][sample["index"]]
        ed.check_eq((track["path"], track["channel"]), (sample["path"], sample["channel"]), "samples.tracks[k].index is the track's")
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
            ({"path": 3}, "must be a string"), ({"channel": "colour"}, "Use one of"), ({"channel": "property"}, "needs component"),
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
            ({"events": [{"time": 0, "function": "end"}]}, "not a keyword"),
            ({"events": [{"time": 0, "function": "f", "args": list(range(9))}]}, "at most 8"),
            ({"events": [{"time": 0, "function": "f", "args": [[1]]}]}, "number, string or boolean"),
            ({"events": [{"time": 0, "function": "f", "when": 1}]}, "unknown key"),
            ({"events": [{"function": "f"}]}, "time must be"),
            ({"events_mode": "merge"}, "needs events"), ({"events": [], "events_mode": "append"}, "replace"),
            ({"create": {}}, "already exists"), ({"create": {"form": "x"}}, "unknown key"),
            ({}, "Nothing to do"), ({"root": "Nobody"}, "No GameObject \"Nobody\""), ({"length": 0}, "or -1 for automatic"),
            ({"tracks": [dict(track, relativ=True)]}, "unknown key \"relativ\"")):
        await refused(ed, "edit_animation", {"clip": BOB, **args}, contains)
    await refused(ed, "edit_animation", {"clip": "assets/anim/missing.anim", "wrap": "loop"}, "create: {}")
    await refused(ed, "edit_animation", {"clip": "assets/anim/bob.txt", "create": {}}, "must end in .anim")
    await refused(ed, "edit_animation", {"clip": "../outside.anim", "create": {}}, "must stay inside the project")
    for given in ("assets/anim/bob.anim", "assets/models/hero_tpose.gltf"):
        await refused(ed, "edit_animation", {"clip": "assets/anim/copy.anim", "create": {"from": given}}, "move_asset")
    await refused(ed, "edit_animation", {"clip": "assets/anim/copy.anim", "create": {"remove_tracks": True}}, "needs from")
    await refused(ed, "edit_animation", {"clip": "assets/anim/copy.anim", "create": {"from": WALK_SUB, "remove_tracks": True}}, "needs from")
    made, _ = await edit(ed, clip="assets/nofolder/deeper/x.anim", create={})
    ed.check(made["created"] and (ed.project / "assets/nofolder/deeper/x.anim").is_file(), f"nested folders made: {made}")
    shutil.rmtree(ed.project / "assets" / "nofolder")
    (ed.project / "assets" / "nofolder.anim").write_text("")
    await refused(ed, "edit_animation", {"clip": "assets/nofolder.anim/x.anim", "create": {}}, "is a file, not a folder")
    (ed.project / "assets" / "nofolder.anim").unlink()
    # a write that fails (a file name over the file system's 255 bytes) leaves no folder behind
    r = await ed.call_raw("edit_animation", {"clip": "assets/failfolder/deeper/" + "x" * 300 + ".anim", "create": {}})
    ed.check(r.is_error, f"the write fails: {r.text[:300]}")
    ed.check(not (ed.project / "assets" / "failfolder").exists(), f"the folders made for it are removed: {r.text[:300]}")
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
    ed.check_eq(len(out["users"]), 3, f"three Animation Players share the file: {text}")
    ed.check("shared by 3 Animation Player entries in open scenes and prefabs" in text, text)
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
    ed.check_eq(bound["entity"]["id"], S["box_a"], "bound[] items name their GameObject")
    ed.check(approx(await local_position(ed, S["box_a"]), pos, 1e-3), "the scene shows the preview")
    r = await ed.call_raw("preview_animation", {"animation": S["box_a"], "action": "play", "wait_ms": 300})
    ed.check(not r.is_error and r.structured["playing"], r.text)
    t = r.structured["time"]
    ed.check(0.5 < t <= 0.5 + 0.3 + 0.6 or t < 0.6, f"playing advances the preview from 0.5 by about the wait (mod 1 s): {t}")
    ed.check(not approx(await local_position(ed, S["box_a"]), pos, 1e-3), "the scene shows the playing preview")
    # played without wait_ms it keeps advancing (the App drives the preview every frame)
    await asyncio.sleep(0.6)
    r = await ed.call_raw("preview_animation", {"animation": S["box_a"], "action": "pause"})
    ed.check(not r.is_error and not r.structured["playing"] and r.structured["time"] != t,
             f"the preview advanced between calls: {t} -> {r.structured['time']}")
    ed.check_eq(r.structured["bound_total"], 1, "bound_total")
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
    ed.check("mixamorig" not in miss["nearest"][0], f"the nearest names are Robot's: {miss}")
    # the suggestion is a real path on Robot: a clip of it binds there
    await edit(ed, clip="assets/anim/near.anim", create=True,
               tracks=[{"path": miss["nearest"][0], "channel": "rotation", "keys": [{"time": 0, "value": [0, 0, 0, 1]}]}])
    near = await ed.call("get_animation", clip="assets/anim/near.anim", root="Robot")
    ed.check_eq((near["bindings"]["resolved"], near["bindings"]["total"]), (1, 1), f"nearest[0] resolves on Robot: {near['bindings']}")
    # agents review I1: a model's paths start with its glTF root node; the guide's example is real
    ed.check(all(p.startswith("Hero/") for p in S["walk_paths"]), f"Hero's paths include its glTF root node: {S['walk_paths'][:3]}")
    spine = next((m for m in b["missing"] if m["path"] == "Hero/mixamorig:Hips/mixamorig:Spine" and m["channel"] == "rotation"), None)
    ed.check(spine is not None and spine["nearest"][:1] == ["Robot/pelvis/spine"], f"the guide's nearest example: {spine}")
    # final agents review I1: Robot is another rig. Rotation keys are absolute local rotations and its
    # rest orientations differ from Hero's (its thighs rest turned 180 degrees): a renamed path would
    # mangle it, so those paths get no fix but rest_mismatch, rig_mismatch says to make Robot its own
    # clip, and no fix goes to a bone of the other side (Left/Right vs .L/.R)
    ed.check("rig_mismatch" in b and "bake" in b["rig_mismatch"], f"the report says Robot is another rig: {json.dumps(b)[:900]}")
    legs = [m for m in b["missing"] if m["channel"] == "rotation" and m["path"].endswith(("mixamorig:LeftUpLeg", "mixamorig:RightUpLeg"))]
    ed.check_eq(len(legs), 2, f"both legs are missing on Robot: {[m['path'] for m in b['missing']]}")
    ed.check(all("fix" not in m and "rest_mismatch" in m for m in legs), f"no thigh fix (other rest pose): {legs}")
    for m in b["missing"]:
        if "fix" in m:
            ed.check(bone_side(m["path"]) is None or bone_side(m["fix_path"]) in (None, bone_side(m["path"])),
                     f"a fix keeps its side: {m['path']} -> {m['fix_path']}")
    text = (await ed.call_raw("get_animation", {"clip": "assets/anim/walk.anim", "root": "Robot"})).text
    ed.check("no fix: rest pose differs a lot" in text and "another rig" in text and "by hand" in text,
             f"the text says it too: {text[:900]}")

    # path fixes are for renamed or reparented objects of the same rig: Hero's glTF root node renamed,
    # every track misses, each gets a fix (the rig's own rest), and the fixes apply together
    await ed.call("rename_entity", entity="Hero/Hero", name="HeroRig")
    renamed = (await ed.call("get_animation", clip="assets/anim/walk.anim", root="Hero"))["bindings"]
    ed.check_eq(renamed["resolved"], 0, f"no path binds after the rename: {json.dumps(renamed)[:600]}")
    fixes = [m for m in renamed["missing"] if "fix" in m]
    ed.check_eq(len(fixes), len(S["walk_paths"]), f"every missing path carries fix: {json.dumps(renamed)[:900]}")
    ed.check(all(m["fix_path"] == "HeroRig" + m["path"][len("Hero"):] for m in fixes), f"the renamed node's paths: {fixes[:2]}")
    ed.check("rig_mismatch" not in renamed and "fix_with" in renamed, f"the same rig: {renamed.get('rig_mismatch')}")
    spine = next(m for m in fixes if m["path"] == "Hero/mixamorig:Hips/mixamorig:Spine" and m["channel"] == "rotation")
    # agents review I3: the fix is a write_file edit renaming the path, every key kept
    await ed.call("move_asset", path="assets/anim/walk.anim", to="assets/anim/walk_one.anim", copy=True)
    before = await ed.call("get_animation", clip="assets/anim/walk_one.anim", root="Hero", max_keys=100000)
    keys_before = next(t for t in before["tracks"] if t["path"] == spine["path"] and t["channel"] == "rotation")["keys"]
    r = await ed.call_raw("write_file", {"path": "assets/anim/walk_one.anim", "edits": [spine["fix"]]})
    ed.check(not r.is_error, f"the fix applies: {r.text[:500]}")
    after = await ed.call("get_animation", clip="assets/anim/walk_one.anim", root="Hero", max_keys=100000)
    ed.check_eq(after["bindings"]["resolved"], before["bindings"]["resolved"] + 1, "the renamed track binds")
    moved = next(t for t in after["tracks"] if t["path"] == spine["fix_path"] and t["channel"] == "rotation")
    ed.check_eq(moved["keys"], keys_before, "every key kept")
    ed.check(not any(m["path"] == spine["path"] for m in after["bindings"]["missing"]), "no longer missing")
    # FX4 (verify review A): every fix applied together merges no tracks and loses no key
    await ed.call("move_asset", path="assets/anim/walk.anim", to="assets/anim/walk_all.anim", copy=True)
    full = await ed.call("get_animation", clip="assets/anim/walk_all.anim", root="Hero", max_keys=100000)
    fixes = [m for m in full["bindings"]["missing"] if "fix" in m]
    ed.check(len(fixes) > 2, f"several fixes: {len(fixes)}")
    existing = {(t["path"], t["channel"]) for t in full["tracks"]}
    targets = [(m["fix_path"], m["channel"]) for m in fixes]
    ed.check_eq(len(set(targets)), len(targets), f"no two fixes share a target: {targets}")
    ed.check(not set(targets) & existing, f"no fix targets a track the clip has: {set(targets) & existing}")
    ed.check("apply together" in full["bindings"]["fix_with"] and "same rig" in full["bindings"]["fix_with"], full["bindings"]["fix_with"])
    keys_by_path = {(t["path"], t["channel"]): t["keys"] for t in full["tracks"]}
    r = await ed.call_raw("write_file", {"path": "assets/anim/walk_all.anim", "edits": [m["fix"] for m in fixes]})
    ed.check(not r.is_error, f"every fix applies in one write_file: {r.text[:500]}")
    applied = await ed.call("get_animation", clip="assets/anim/walk_all.anim", root="Hero", max_keys=100000)
    ed.check_eq(len(applied["tracks"]), len(full["tracks"]), "no track merged into another")
    ed.check(not any("merged" in w for w in applied["warnings"]), f"no merge warning: {applied['warnings']}")
    after_by_path = {(t["path"], t["channel"]): t["keys"] for t in applied["tracks"]}
    for m in fixes:
        ed.check_eq(after_by_path.get((m["fix_path"], m["channel"])), keys_by_path[(m["path"], m["channel"])],
                    f"{m['path']} -> {m['fix_path']} keeps its keys")
    ed.check_eq(sum(len(k) for k in after_by_path.values()), sum(len(k) for k in keys_by_path.values()), "no key lost")
    ed.check_eq(applied["bindings"]["resolved"], full["bindings"]["resolved"] + len(fixes), "each fixed track binds")
    ed.check_eq(applied["bindings"]["resolved"], applied["bindings"]["total"], "the renamed rig binds the fixed clip whole")
    await ed.call("rename_entity", entity="Hero/HeroRig", name="Hero")
    # a bone's bound transform in the preview is the sampled value
    r = await ed.call_raw("preview_animation", {"animation": "Hero", "action": "seek", "time": 0.3})
    ed.check(not r.is_error and r.structured["bound_total"] >= len(r.structured["bound"]) and 0 < len(r.structured["bound"]) <= 8,
             f"at most 8 bound transforms: {r.text}")
    sampled = await ed.call("get_animation", clip="assets/anim/walk.anim", times=[0.3])
    rotations = {t["path"]: t["values"][0] for t in sampled["samples"]["tracks"] if t["channel"] == "rotation" and not t["relative"]}
    compared = 0
    for bt in r.structured["bound"]:
        if bt["path"] in rotations:
            q, e = bt["rotation"]["quat"], rotations[bt["path"]]["quat"]
            ed.check(approx(q, e, 2e-3) or approx(q, [-x for x in e], 2e-3), f"{bt['path']}: preview {q} vs sampled {e}")
            compared += 1
    ed.check(compared > 0, f"a bone compared: {[b['path'] for b in r.structured['bound']]}")
    await ed.call("preview_animation", animation="Hero", action="stop")
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
    out, text = await edit(ed, clip="assets/anim/mover.anim", create={"from": animation}, wrap="loop", max_keys=1)
    ed.check(out["created"] and out["added_player"] and out["player"]["name"] == "Mover", f"converted: {text}")
    ed.check_eq((len(out["tracks"][0]["keys"]), out["tracks"][0]["key_count"]), (1, 2), "max_keys applies to create {from}")
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
    # agents review M2: a bake-only call says the pose file is unchanged (the clip was written), and
    # the Lua hint runs as written
    ed.check("(pose file unchanged)" in r.text and "nothing written" not in r.text, r.text)
    ed.check(f'GameObject(self.scene, self.entity):getAnimationPlayer():play("{bake["name"]}")' in r.text, r.text)
    clip = await ed.call("get_animation", clip=bake["clip"], root="Hero")
    ed.check(clip["bindings"]["missing"] == [] and clip["source"]["kind"] == "pose", f"bone paths bind on Hero: {json.dumps(clip['bindings'])[:500]}")
    info = await ed.call("get_animation", entity="Hero")
    ed.check(any(s["clip"] == bake["clip"] for s in info["players"][0]["states"]), "the model's player lists it")
    ed.check_eq(bake["source"], pose, "the bake names its pose animation")
    # the clip plays what the entity bake plays: the same bone rotations at the frames
    entity = (await ed.call("edit_pose_animation", path=pose, bake={"model": "Hero"}))["bake"]
    baked = await ed.call("get_animation", entity=entity["animation"]["id"], max_keys=100)
    track = next(a for a in baked["actions"] if a.get("kind") == "rotation_track")
    bone_path = track["target"]["path"]
    clip_path = bone_path.split("/", 1)[1] if "/" in bone_path else ""
    keys = track["keys"]
    times = [keys[1]["time"], keys[len(keys) // 2]["time"]]
    sampled = await ed.call("get_animation", clip=bake["clip"], times=times)
    values = next(t for t in sampled["samples"]["tracks"] if t["path"] == clip_path and t["channel"] == "rotation")["values"]
    for n, key in enumerate((keys[1], keys[len(keys) // 2])):
        a, b = values[n]["quat"], key["value"]["quat"]
        ed.check(approx(a, b, 2e-3) or approx(a, [-x for x in b], 2e-3), f"{clip_path} at {times[n]}: clip {a} vs entity bake {b}")
    await ed.call("undo", scope="scene")
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
    ed.check(all(isinstance(t["key_count"], int) and "keys" not in t for t in clip["tracks"]), f"a count is key_count: {clip['tracks']}")
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
    outside = ed.project / rel
    outside.write_text('{"format": "jasperengine.anim",\n "version": 1,\n "tracks": [}\n')
    # outside the last writing call's timestamp window (S05: what a call writes is its own)
    stamp = outside.stat()
    os.utime(outside, ns=(stamp.st_atime_ns, stamp.st_mtime_ns + 2_000_000_000))
    r = await ed.call_raw("get_animation", {"clip": BOB})
    problems = harness.find_key(r.structured, "problems") or []
    ed.check(any(p.get("path") == rel and p.get("line") == 3 for p in problems), f"external_changes problems: {problems}")


async def test_move_copy_delete(ed: Editor):
    await ed.call("create_folder", path="assets/clips")
    r = await ed.call_raw("move_asset", {"path": BOB, "to": "assets/clips/bob.anim"})
    # S04: every reference follows by GUID (the Animation Players' too), no referrer file rewritten
    ed.check(not r.is_error and "references follow it by GUID" in r.text, r.text)
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


async def test_scene_undo_relinks_a_moved_clip(ed: Editor):
    """R8 after a scene undo / redo step: a player entry the step gives back with the path hint of a clip moved
    since points at the clip by GUID again (the clip timeline resolves after the step: CommandHistory's
    onSceneModified following beforeStep, since the merge of main dropped afterStep)."""
    box = await make_cube(ed, "RelinkBox", (6, 0, 0))
    await edit(ed, clip="assets/anim/relink.anim", create=True,
               tracks=[{"path": "", "channel": "position", "keys": [{"time": 0, "value": [0, 0, 0]}, {"time": 1, "value": [0, 1, 0]}]}])
    await ed.call("add_component", entity=box, component="animationplayer", values={"animationplayer": {"clips": [{"clip": "assets/anim/relink.anim"}]}})
    await ed.call("remove_component", entity=box, component="animationplayer")
    r = await ed.call_raw("move_asset", {"path": "assets/anim/relink.anim", "to": "assets/anim/relink_moved.anim"})
    ed.check(not r.is_error, r.text)
    # the removed player comes back with the old path hint: resolved by its GUID right after the step
    await ed.call("undo", scope="scene")
    state = (await ed.call("get_animation", entity=box))["players"][0]["states"][0]
    ed.check_eq(state["clip"], "assets/anim/relink_moved.anim", f"the undone removal points at the moved clip: {state}")
    ed.check(not state.get("missing"), f"not missing: {state}")
    await ed.call("redo", scope="scene")
    await ed.call("undo", scope="scene")
    state = (await ed.call("get_animation", entity=box))["players"][0]["states"][0]
    ed.check_eq(state["clip"], "assets/anim/relink_moved.anim", "again after a redo and an undo")
    await ed.call("delete_entities", entities=[box])


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


# ---------------------------------------------------------------------------- review round 1

async def test_rotation_key_round_trip(ed: Editor):
    """A key read from a result ({euler, quat}) with its euler changed: euler wins."""
    out, _ = await edit(ed, clip="assets/anim/turn.anim", create=True, tracks=[{"path": "", "channel": "rotation", "keys": [
        {"time": 0, "value": {"euler": [0, 0, 0]}}, {"time": 1, "value": {"euler": [0, 90, 0]}}]}])
    keys = out["tracks"][0]["keys"]
    keys[1]["value"]["euler"] = [0, 45, 0]
    out, _ = await edit(ed, clip="assets/anim/turn.anim", tracks=[{"path": "", "channel": "rotation", "keys": keys}])
    got = out["tracks"][0]["keys"][1]["value"]
    ed.check(approx(got["euler"], [0, 45, 0], 1e-3), f"the edited euler is kept: {got}")
    # an untouched read-back key keeps its value
    out, _ = await edit(ed, clip="assets/anim/turn.anim", tracks=[{"path": "", "channel": "rotation", "keys": out["tracks"][0]["keys"]}])
    ed.check(approx(out["tracks"][0]["keys"][1]["value"]["euler"], [0, 45, 0], 1e-3), "a round trip of the result is a no-change")


async def test_idempotent_settings_and_event_length(ed: Editor):
    out, text = await edit(ed, clip="assets/anim/turn.anim", wrap="once")
    ed.check(any(c == "wrap once (unchanged)" for c in out["changes"]) and "no change: nothing written" in text,
             f"a setting at its current value is a no-change: {out['changes']} {text}")
    out, _ = await edit(ed, clip="assets/anim/turn.anim", events=[{"time": 3, "function": "onLate"}])
    ed.check_eq(out["length"], 3, "an event past the last key lengthens an automatic clip")
    ed.check(any(c.startswith("length now 3 s") for c in out["changes"]), f"said in changes: {out['changes']}")


async def test_relative_tracks(ed: Editor):
    """relative: position adds to the rest value, scale multiplies it; times gives the track's own
    offset / factor, the preview the posed value."""
    rel = await make_cube(ed, "Rel", (7, 0, 0))
    await edit(ed, clip="assets/anim/rel.anim", create=True, tracks=[
        {"path": "", "channel": "position", "relative": True, "keys": [{"time": 0, "value": [0, 1, 0]}, {"time": 1, "value": [0, 1, 0]}]},
        {"path": "", "channel": "scale", "relative": True, "keys": [{"time": 0, "value": [2, 2, 2]}, {"time": 1, "value": [2, 2, 2]}]}])
    await ed.call("add_component", entity=rel, component="animationplayer", values={"animationplayer": {"clips": [{"clip": "assets/anim/rel.anim"}]}})
    sampled = await ed.call("get_animation", clip="assets/anim/rel.anim", times=[0.5])
    by = {t["channel"]: t for t in sampled["samples"]["tracks"]}
    ed.check(by["position"]["relative"] and approx(by["position"]["values"][0], [0, 1, 0]), f"times: the offset: {by['position']}")
    ed.check(approx(by["scale"]["values"][0], [2, 2, 2]), f"times: the factor: {by['scale']}")
    r = await ed.call_raw("preview_animation", {"animation": rel, "action": "seek", "time": 0.5})
    ed.check(not r.is_error, r.text)
    bound = r.structured["bound"][0]
    ed.check(approx(bound["position"], [7, 1, 0], 1e-3) and approx(bound["scale"], [2, 2, 2], 1e-3), f"the preview: rest + offset, rest x factor: {bound}")
    await ed.call("preview_animation", animation=rel, action="stop")
    ed.check(approx(await local_position(ed, rel), [7, 0, 0], 1e-4), "restored")


async def test_lattice_and_morph_tracks(ed: Editor):
    jelly = await make_cube(ed, "Jelly", (0, 0, 8))
    made = await ed.call("edit_lattice", create={"targets": [jelly], "name": "JellyCage"},
                         ops=[{"op": "set", "points": [{"index": 13, "offset": [0, 0.3, 0]}]}, {"op": "pose", "save": "squash"}])
    cage = made["lattice"]["id"]
    out, _ = await edit(ed, clip="assets/anim/jelly.anim", create=True, root=cage, tracks=[{"path": "", "channel": "lattice", "keys": [
        {"time": 0, "points": "rest"}, {"time": 0.5, "points": "current"}, {"time": 1, "points": "squash"},
        {"time": 1.5, "points": [[[1, 0, 0], [0, 0.2, 0]]]}]}])
    track = out["tracks"][0]
    ed.check(13 in track["points"] and 1 in track["points"], f"the points: current/pose keys and grid coordinates: {track['points']}")
    keys = track["keys"]
    ed.check(approx(keys[1]["points"]["13"], [0, 0.3, 0]) and approx(keys[2]["points"]["13"], [0, 0.3, 0]), f"current and the pose: {keys}")
    ed.check(approx(keys[0]["points"]["13"], [0, 0, 0]) and approx(keys[3]["points"]["1"], [0, 0.2, 0]), f"rest and grid: {keys}")
    ed.check_eq(out["bindings"]["missing"], [], "the cage binds")
    sampled = await ed.call("get_animation", clip="assets/anim/jelly.anim", times=[0.25])
    ed.check(approx(sampled["samples"]["tracks"][0]["values"][0]["13"], [0, 0.15, 0], 1e-4), f"lattice samples: {sampled['samples']}")
    await refused(ed, "edit_animation", {"clip": "assets/anim/jelly.anim", "root": jelly, "tracks": [
        {"path": "", "channel": "lattice", "keys": [{"time": 0, "points": "current"}]}]}, "not the mesh")
    await refused(ed, "edit_animation", {"clip": "assets/anim/jelly.anim", "root": cage, "tracks": [
        {"path": "", "channel": "lattice", "keys": [{"time": 0, "points": "flat"}]}]}, "not \"current\", \"rest\" or a pose")
    # morph weights (shorter arrays pad with 0)
    out, _ = await edit(ed, clip="assets/anim/jelly.anim", tracks=[{"path": "", "channel": "morph", "keys": [
        {"time": 0, "value": [0, 0.5]}, {"time": 1, "value": [1]}]}])
    morph = next(t for t in out["tracks"] if t["channel"] == "morph")
    ed.check_eq([k["value"] for k in morph["keys"]], [[0, 0.5], [1, 0]], "morph keys")
    sampled = await ed.call("get_animation", clip="assets/anim/jelly.anim", times=[0.5])
    mv = next(t for t in sampled["samples"]["tracks"] if t["channel"] == "morph")["values"][0]
    ed.check(approx(mv, [0.5, 0.25]), f"morph samples: {mv}")


async def test_binding_report_extras(ed: Editor):
    # missing handlers: BoxB has no script with onBob
    got = await ed.call("get_animation", clip=BOB, root=S["box_b"])
    ed.check("onBob" in got["bindings"]["missing_handlers"], f"missing handlers: {got['bindings']}")
    # ambiguous: two children with one name
    twins = (await ed.call("create_entity", type="empty", name="Twins"))["entity_id"]
    arms = [(await ed.call("create_entity", type="cube", name="Arm", parent=twins))["entity_id"] for _ in range(2)]
    await ed.call("rename_entity", entity=arms[1], name="Arm")   # created names are made unique
    await edit(ed, clip="assets/anim/twins.anim", create=True, tracks=[{"path": "Arm", "channel": "scale", "keys": [{"time": 0, "value": [1, 1, 1]}]},
                                                                      {"path": "Arm#1", "channel": "scale", "keys": [{"time": 0, "value": [2, 2, 2]}]}])
    b = (await ed.call("get_animation", clip="assets/anim/twins.anim", root=twins))["bindings"]
    ed.check(b["resolved"] == 2 and [a["path"] for a in b.get("ambiguous", [])] == ["Arm"], f"Arm is ambiguous, Arm#1 is not: {b}")
    # a legacy-layout model under a player (a paste does not reload it): add_component reloads it
    rig = (await ed.call("create_entity", type="empty", name="Rig"))["entity_id"]
    await ed.call("add_component", entity=rig, component="animationplayer", values={"animationplayer": {"speed": 2}})
    bot = (await ed.call("create_entity", type="model", name="Bot", model="assets/models/robot_apose.gltf"))["entity_id"]
    await ed.call("copy_entities", entities=[bot])
    await ed.call("paste_entities", parent=rig)
    await ed.call("delete_entities", entities=[bot])
    await edit(ed, clip="assets/anim/rig.anim", create=True, tracks=[{"path": "Bot/Robot/pelvis", "channel": "rotation",
                                                                    "keys": [{"time": 0, "value": [0, 0, 0, 1]}]}])
    r = await ed.call_raw("get_animation", {"clip": "assets/anim/rig.anim", "root": rig})
    b = r.structured["bindings"]
    ed.check(b.get("legacy_models") and "add_component" in b["legacy_fix"] and "without their node hierarchy" in r.text, f"legacy: {r.text}")
    r = await ed.call_raw("add_component", {"entity": rig, "component": "animationplayer"})
    ed.check(not r.is_error and r.structured["reloaded"][0]["models"] == 1 and "settings are kept" in r.text, r.text)
    b = (await ed.call("get_animation", clip="assets/anim/rig.anim", root=rig))["bindings"]
    ed.check("legacy_models" not in b and b["resolved"] == 1, f"bound after the reload: {b}")
    ed.check_eq((await ed.call("get_animation", entity=rig))["players"][0]["speed"], 2, "the player's settings are kept")
    await ed.call("undo", scope="scene")
    ed.check((await ed.call("get_animation", clip="assets/anim/rig.anim", root=rig))["bindings"].get("legacy_models"), "one undo step")
    # with another component in the same call (and values) the reload is part of that one step
    r = await ed.call_raw("add_component", {"entity": rig, "component": ["animationplayer", "audiosource"],
                                            "values": {"animationplayer": {"speed": 3}}})
    ed.check(not r.is_error and r.structured["reloaded"][0]["models"] == 1 and "reloaded 1 model" in r.text, r.text)
    ed.check_eq([a["component"] for a in r.structured["added"]], ["audiosource"], "the player is not added twice")
    b = (await ed.call("get_animation", clip="assets/anim/rig.anim", root=rig))["bindings"]
    ed.check("legacy_models" not in b and b["resolved"] == 1, f"bound after the reload with another component: {b}")
    ed.check_eq((await ed.call("get_animation", entity=rig))["players"][0]["speed"], 3, "the values apply")
    await ed.call("undo", scope="scene")
    b = (await ed.call("get_animation", clip="assets/anim/rig.anim", root=rig))["bindings"]
    ed.check(b.get("legacy_models"), f"one undo step: the reload, the component and the values: {b}")
    ed.check_eq((await ed.call("get_animation", entity=rig))["players"][0]["speed"], 2, "speed back")


async def test_wrap_of_the_player_entry(ed: Editor):
    await edit(ed, clip=BOB, wrap="ping_pong")
    plain = await ed.call("get_animation", clip=BOB, times=[1.25])
    entry = await ed.call("get_animation", clip=BOB, times=[1.25], root=S["box_b"])  # its entry wraps loop
    ed.check_eq((plain["samples"]["wrap_used"], entry["samples"]["wrap_used"]), ("ping_pong", "loop"), "wrap_used")
    a = next(t for t in plain["samples"]["tracks"] if t["channel"] == "position")["values"][0]
    b = next(t for t in entry["samples"]["tracks"] if t["channel"] == "position")["values"][0]
    ed.check(not approx(a, b), f"different times sampled (0.75 vs 0.25): {a} {b}")
    await edit(ed, clip=BOB, wrap="loop")


async def test_shared_by_two_prefabs(ed: Editor):
    before = len((await ed.call("get_animation", clip=BOB))["users"])
    for name in ("PrefA", "PrefB"):
        cube = await make_cube(ed, name, (9, 0, 0))
        await ed.call("add_component", entity=cube, component="animationplayer", values={"animationplayer": {"clips": [{"clip": BOB}]}})
        await ed.call("create_prefab", entity=cube, path=f"prefabs/{name}.prefab")
    out, text = await edit(ed, clip=BOB, tracks=[{"path": "", "channel": "scale", "mode": "merge", "keys": [{"time": 0.5, "value": [1.2, 1.2, 1.2]}]}])
    users = json.dumps(out["users"])
    ed.check(len(out["users"]) >= before + 2 and "PrefA" in users and "PrefB" in users, f"both prefabs' players share the clip: {users}")
    for name in ("PrefA", "PrefB"):
        state = (await ed.call("get_animation", entity=name))["players"][0]["states"][0]
        ed.check_eq(state["clip"], BOB, f"{name} plays the clip")
    sampled = await ed.call("get_animation", clip=BOB, times=[0.5])
    scale = next(t for t in sampled["samples"]["tracks"] if t["channel"] == "scale")["values"][0]
    ed.check(approx(scale, [1.2, 1.2, 1.2]), f"one edit for every user: {scale}")


async def test_create_from_entity_remove_tracks(ed: Editor):
    box = await make_cube(ed, "Mover2", (0, 0, 10))
    made, _ = await edit(ed, target=box, autoplay=True, tracks=[{"channel": "position", "keys": [{"time": 0, "value": [0, 0, 10]}, {"time": 1, "value": [0, 1, 10]}]}])
    animation = made["animation"]["id"]
    out, _ = await edit(ed, clip="assets/anim/mover2.anim", create={"from": animation})
    ed.check(any("keeps its 1 converted track" in w and "(it autoplays)" in w for w in out["warnings"]), f"the warning: {out['warnings']}")
    ed.check_eq(out["source"]["kind"], "entity", "source")
    await ed.call("undo", scope="scene")
    out, text = await edit(ed, clip="assets/anim/mover2.anim", create={"from": animation, "remove_tracks": True})
    ed.check(out["removed_tracks"] == 1 and "converted tracks removed" in text, text)
    ed.check(not any("keeps its" in w for w in out["warnings"]), f"no warning: {out['warnings']}")
    kinds = [a.get("kind") for a in (await ed.call("get_animation", entity=animation))["actions"]]
    ed.check("position_track" not in kinds, f"the track left the Animation: {kinds}")
    await ed.call("undo", scope="scene")
    kinds = [a.get("kind") for a in (await ed.call("get_animation", entity=animation))["actions"]]
    ed.check("position_track" in kinds and not (ed.project / "assets/anim/mover2.anim").exists(), f"one undo step: {kinds}")


async def test_extract_with_root_switches_the_player(ed: Editor):
    """agents review I2: an extract with root repoints the root's player entries of the model clip
    to the new file in the same call (the UI's Extract), one scene step; without root the result
    names the players still playing the model clip."""
    await ed.call("add_component", entity="Hero", component="animationplayer", values={"animationplayer": {"clips": [{"clip": WALK_SUB}]}})
    out, text = await edit(ed, clip="assets/anim/walk_alone.anim", create={"from": WALK_SUB})
    ed.check("one project undo step" in text and "still plays" in text, text)
    ed.check(out.get("model_clip_users") and out["model_clip_users"][0]["entity"]["name"] == "Hero", f"model_clip_users: {out.get('model_clip_users')}")
    out, text = await edit(ed, clip="assets/anim/walk_hero.anim", create={"from": WALK_SUB}, root="Hero", wrap="loop")
    ed.check("plays the file now" in text and "one scene undo step" in text, text)
    ed.check("model_clip_users" not in out and out["wrap"] == "loop", f"switched, this call's edits in the file: {json.dumps(out)[:400]}")
    ed.check(any(u["entity"]["name"] == "Hero" for u in out["users"]), f"Hero plays the file: {out['users']}")
    state = (await ed.call("get_animation", entity="Hero"))["players"][0]["states"]
    ed.check_eq([(s["name"], s["clip"]) for s in state], [("Walk", "assets/anim/walk_hero.anim")], "the entry switched, its state name kept")
    await ed.call("undo", scope="scene")
    ed.check(not (ed.project / "assets/anim/walk_hero.anim").exists(), "the scene undo removes the file")
    state = (await ed.call("get_animation", entity="Hero"))["players"][0]["states"]
    ed.check_eq(state[0]["clip"], WALK_SUB, "and the entry plays the model clip again")
    await ed.call("remove_component", entity="Hero", component="animationplayer")


TANGENT_CLIP = {"format": "jasperengine.anim", "version": 1, "tracks": [
    {"path": "", "channel": "position", "keys": [
        {"time": 0, "value": [0, 0, 0], "in": [0, 0, 0], "out": [0, 2, 0]},
        {"time": 1, "value": [0, 1, 0], "in": [0, 2, 0], "out": [0, 1, 0]},
        {"time": 2, "value": [0, 0, 0], "in": [0, -1, 0], "out": [0, 0, 0]}]},
    {"path": "", "channel": "rotation", "keys": [
        {"time": 0, "value": [0, 0, 0, 1], "in": [0, 0, 0, 0], "out": [0, 0.5, 0, 0]},
        {"time": 1, "value": [0, 0.382683, 0, 0.92388], "in": [0, 0.3, 0, -0.1], "out": [0, 0, 0, 0]}]}]}


async def test_fix_conflicts_and_hand_written_files(ed: Editor):
    """FX4 (verify review A, minor 2): a fix never targets a track the clip has (fix_conflict when no
    nearest path is free); a hand-written file (not canonical) gets no fix and fix_with says why."""
    rig = (await ed.call("create_entity", type="empty", name="FixRig", position=[0, 0, 12]))["entity_id"]
    await ed.call("create_entity", type="empty", name="FixLimb", parent=rig)
    keys = [{"time": 0, "value": [0, 0, 0]}, {"time": 1, "value": [0, 1, 0]}]
    out, _ = await edit(ed, clip="assets/anim/fixrig.anim", create={}, root=rig,
                        tracks=[{"path": "FixLimb", "channel": "position", "keys": keys}, {"path": "FixLimbs", "channel": "position", "keys": keys}])
    miss = next(m for m in out["bindings"]["missing"] if m["path"] == "FixLimbs")
    ed.check("FixLimb" in miss["nearest"], f"FixLimb is a nearest path: {miss}")
    ed.check(("fix" in miss and miss["fix_path"] != "FixLimb") or "fix_conflict" in miss,
             f"no fix renames FixLimbs onto the clip's own FixLimb track: {miss}")
    hand = {"format": "jasperengine.anim", "version": 1, "tracks": [{"path": "FixLimbs", "channel": "rotation", "keys": [[0, [0, 0, 0, 1]]]}]}
    r = await ed.call_raw("write_file", {"path": "assets/anim/handwritten.anim", "content": json.dumps(hand, indent=2)})
    ed.check(not r.is_error, r.text)
    got = await ed.call("get_animation", clip="assets/anim/handwritten.anim", root=rig)
    b = got["bindings"]
    ed.check(b["missing"] and not any("fix" in m for m in b["missing"]) and "canonical" in b.get("fix_with", ""),
             f"no fix for a non-canonical file, fix_with says why: {json.dumps(b)[:600]}")
    await ed.call("delete_entities", entities=[rig])


async def test_tangents_survive_edits(ed: Editor):
    """agents review I4: keys sent back with their in/out, or unchanged without them, keep their
    tangents in replace and merge; a changed key without them gets linear tangents."""
    path = "assets/anim/tangents.anim"
    r = await ed.call_raw("write_file", {"path": path, "content": json.dumps(TANGENT_CLIP, indent=2)})
    ed.check(not r.is_error, r.text)
    got = await ed.call("get_animation", clip=path)
    tangents = [[(k["in"], k["out"]) for k in t["keys"]] for t in got["tracks"]]
    ed.check(all("in" in k for t in got["tracks"] for k in t["keys"]), f"read with tangents: {got['tracks']}")
    # read-modify-write of the whole tracks as read (rotations as {quat, euler})
    out, _ = await edit(ed, clip=path, tracks=[{"path": t["path"], "channel": t["channel"], "keys": t["keys"]} for t in got["tracks"]])
    ed.check_eq([[(k.get("in"), k.get("out")) for k in t["keys"]] for t in out["tracks"]], tangents, "replace with the read keys keeps them")
    rot = out["tracks"][1]["keys"][1]["value"]["quat"]
    ed.check(approx(rot, [0, 0.382683, 0, 0.92388], 1e-4), f"the rotation stays in its hemisphere: {rot}")
    # the same keys without in/out: unchanged keys keep their tangents
    bare = [{"time": k["time"], "value": k["value"]} for k in got["tracks"][0]["keys"]]
    out, _ = await edit(ed, clip=path, tracks=[{"path": "", "channel": "position", "keys": bare}])
    ed.check_eq([(k.get("in"), k.get("out")) for k in out["tracks"][0]["keys"]], tangents[0], "unchanged keys keep their tangents")
    # merge: the changed key loses them, the others keep theirs
    out, _ = await edit(ed, clip=path, tracks=[{"path": "", "channel": "position", "mode": "merge", "keys": [{"time": 1, "value": [0, 3, 0]}]}])
    keys = out["tracks"][0]["keys"]
    ed.check(approx(keys[1]["value"], [0, 3, 0]) and keys[1]["in"] == [0, 3, 0] and keys[1]["out"] == [0, -3, 0],
             f"the changed key gets the straight line to its neighbours: {keys[1]}")
    ed.check(any("linear tangents" in w for w in out["warnings"]), f"a warning says so: {out['warnings']}")
    ed.check_eq([(keys[i].get("in"), keys[i].get("out")) for i in (0, 2)], [tangents[0][0], tangents[0][2]], "untouched keys keep theirs")
    file = file_json(ed, path)
    ed.check(isinstance(file["tracks"][0]["keys"][0], dict) and "in" in file["tracks"][0]["keys"][0], f"written with tangents: {file['tracks'][0]}")
    # explicit tangents on a new key; malformed pairs are refused
    out, _ = await edit(ed, clip=path, tracks=[{"path": "", "channel": "position", "mode": "merge",
                                                "keys": [{"time": 1, "value": [0, 3, 0], "in": [0, 1, 0], "out": [0, 1, 0]}]}])
    ed.check_eq((out["tracks"][0]["keys"][1]["in"], out["tracks"][0]["keys"][1]["out"]), ([0, 1, 0], [0, 1, 0]), "given in/out are written")
    await refused(ed, "edit_animation", {"clip": path, "tracks": [{"path": "", "keys": [{"time": 0, "value": [0, 0, 0], "in": [0, 0, 0]}]}]},
                  "in and out go together")
    await refused(ed, "edit_animation", {"clip": path, "tracks": [{"path": "", "keys": [{"time": 0, "value": [0, 0, 0], "in": [0, 0], "out": [0, 0]}]}]},
                  "arrays of 3 numbers")
    # FX4 (verify review minor 7): stored rotations with w < 0 keep their hemisphere and tangents,
    # sent back as read ({quat, euler}: euler -> the other hemisphere, flipped back) or euler only (±q)
    neg = {"format": "jasperengine.anim", "version": 1, "tracks": [{"path": "", "channel": "rotation", "keys": [
        {"time": 0, "value": [0, -0.382683, 0, -0.92388], "in": [0, 0.1, 0, 0.2], "out": [0, -0.3, 0, 0.1]},
        {"time": 1, "value": [0, -0.5, 0, -0.866025], "in": [0, -0.2, 0, 0.1], "out": [0, 0, 0, 0]}]}]}
    r = await ed.call_raw("write_file", {"path": "assets/anim/negw.anim", "content": json.dumps(neg, indent=2)})
    ed.check(not r.is_error, r.text)
    read = (await ed.call("get_animation", clip="assets/anim/negw.anim"))["tracks"][0]["keys"]
    stored = [(k["value"]["quat"], k["in"], k["out"]) for k in read]
    ed.check(stored[0][0][3] < 0, f"read as stored (w < 0): {stored}")
    out, _ = await edit(ed, clip="assets/anim/negw.anim", tracks=[{"path": "", "channel": "rotation", "keys": read}])
    ed.check_eq([(k["value"]["quat"], k["in"], k["out"]) for k in out["tracks"][0]["keys"]], stored,
                "keys sent back as read keep the stored hemisphere and tangents")
    eulers = [{"time": k["time"], "value": {"euler": k["value"]["euler"]}} for k in read]
    out, _ = await edit(ed, clip="assets/anim/negw.anim", tracks=[{"path": "", "channel": "rotation", "keys": eulers}])
    ed.check_eq([(k["value"]["quat"], k["in"], k["out"]) for k in out["tracks"][0]["keys"]], stored,
                "euler-only keys equal to the stored ones (as -q) keep the stored keys")
    # FX4 (final review I1): a changed rotation given as +q next to stored -q keys of a tangent track
    # keeps their sign (replaced: the old key's; inserted: the previous key's): the samples don't flip
    flip = {"format": "jasperengine.anim", "version": 1, "tracks": [{"path": "", "channel": "rotation", "keys": [
        {"time": 0, "value": [0, -0.2, 0, -0.979796], "in": [0, 0, 0, 0], "out": [0, 0, 0, 0]},
        {"time": 1, "value": [0, -0.6, 0, -0.8], "in": [0, -0.1, 0, 0.05], "out": [0, -0.1, 0, 0.05]},
        {"time": 2, "value": [0, -0.8, 0, -0.6], "in": [0, 0, 0, 0], "out": [0, 0, 0, 0]}]}]}
    r = await ed.call_raw("write_file", {"path": "assets/anim/flip.anim", "content": json.dumps(flip, indent=2)})
    ed.check(not r.is_error, r.text)
    times = [0.25, 0.5, 0.75, 1.25, 1.5, 1.75]

    async def samples():
        got = await ed.call("get_animation", clip="assets/anim/flip.anim", times=times)
        return [v["quat"] for v in got["samples"]["tracks"][0]["values"]]

    def same_rotation(a, b, eps):
        return abs(sum(x * y for x, y in zip(a, b))) > 1 - eps

    def file_key(time):
        for k in file_json(ed, "assets/anim/flip.anim")["tracks"][0]["keys"]:
            t, v = (k["time"], k["value"]) if isinstance(k, dict) else (k[0], k[1])
            if abs(t - time) < 1e-6:
                return v
        return None

    before = await samples()
    await edit(ed, clip="assets/anim/flip.anim", tracks=[{"path": "", "channel": "rotation", "mode": "merge",
                                                          "keys": [{"time": 1, "value": [0, 0.6, 0, 0.8]}]}])
    after = await samples()
    ed.check(all(same_rotation(a, b, 1e-6) for a, b in zip(before, after)), f"the same rotation as +q: samples unchanged: {before} vs {after}")
    ed.check(file_key(1)[3] < 0, f"the replaced key keeps its sign: {file_key(1)}")
    # a slightly different rotation as +q (euler): the samples move a little, never through zero
    await edit(ed, clip="assets/anim/flip.anim", tracks=[{"path": "", "channel": "rotation", "mode": "merge",
                                                          "keys": [{"time": 1, "value": {"euler": [0, 75, 0]}}]}])
    moved = await samples()
    ed.check(file_key(1)[3] < 0, f"an euler key keeps the replaced key's sign: {file_key(1)}")
    ed.check(all(same_rotation(a, b, 2e-2) for a, b in zip(before, moved)), f"a small change stays a small change (its tangents become linear): {before} vs {moved}")
    # inserted between -q keys as +q: aligned with the previous key
    await edit(ed, clip="assets/anim/flip.anim", tracks=[{"path": "", "channel": "rotation", "mode": "merge",
                                                          "keys": [{"time": 1.5, "value": [0, 0.707107, 0, 0.707107]}]}])
    ed.check(file_key(1.5)[3] < 0, f"the inserted key takes its previous key's sign: {file_key(1.5)}")


async def test_paste_into_a_player_rebinds_the_preview(ed: Editor):
    """engine review M1: a GameObject pasted as the last child of a player's root (parent written in
    place, no re-sort) bumps the hierarchy revision: the preview binds it."""
    await edit(ed, clip="assets/anim/paste_bind.anim", create={}, tracks=[
        {"path": "Leaf", "channel": "position", "keys": [{"time": 0, "value": [0, 0, 0]}, {"time": 1, "value": [0, 4, 0]}]}])
    leaf = await make_cube(ed, "Leaf", (0, 0, 9))
    rig = (await ed.call("create_entity", type="empty", name="PasteRig", position=[0, 0, 9]))["entity_id"]
    await ed.call("add_component", entity=rig, component="animationplayer", values={"animationplayer": {"clips": [{"clip": "assets/anim/paste_bind.anim"}]}})
    r = await ed.call_raw("preview_animation", {"animation": rig, "action": "seek", "time": 0.5})
    ed.check(not r.is_error and r.structured["unresolved"] == ["Leaf"], f"Leaf is not under PasteRig yet: {r.text}")
    await ed.call("copy_entities", entity=leaf)
    pasted = (await ed.call("paste_entities", parent=rig))["created"][0]
    ed.check_eq(pasted["path"], "PasteRig/Leaf", "pasted as the rig's child")
    r = await ed.call_raw("preview_animation", {"animation": rig, "action": "seek", "time": 0.5})
    ed.check(not r.is_error and r.structured["unresolved"] == [] and [b["path"] for b in r.structured["bound"]] == ["Leaf"],
             f"the preview binds the pasted Leaf: {r.text}")
    await ed.call("preview_animation", animation=rig, action="stop")
    await ed.call("delete_entities", entities=[rig, leaf])


async def test_guide_names_exist(ed: Editor):
    """Every tool, argument and result field the guide names (snake_case, and `tool {` calls) exists."""
    got = await ed.session.read_resource("jasper://docs/animation")
    index = "".join(getattr(c, "text", "") or "" for c in got.contents)
    ed.check(index.startswith("# Animation clips") and "jasper://docs/animation/" in index, f"the guide is served as an index: {index[:200]}")
    text = (REPO / "docs" / "mcp" / "guide" / "animation.md").read_text()
    for uri in re.findall(r"`(jasper://docs/animation/[a-z0-9-]+)`", index):
        part = "".join(getattr(c, "text", "") or "" for c in (await ed.session.read_resource(uri)).contents)
        ed.check(len(part) < 16000, f"{uri} is {len(part)} chars")
    known: set = set(ed.tool_names)
    for t in ed.tool_list:
        schema_words(t.input_schema, known)

    def keys(value):
        if isinstance(value, dict):
            for k, v in value.items():
                known.add(k)
                keys(v)
        elif isinstance(value, list):
            for v in value:
                keys(v)
        elif isinstance(value, str):
            known.add(value)
    for call in (("get_animation", {"clip": BOB, "times": [0.5], "root": S["box_b"]}), ("get_animation", {"entity": S["box_b"]}),
                 ("get_animation", {"clip": "assets/anim/rig.anim", "root": "Rig"}), ("get_animation", {"clip": "assets/anim/twins.anim", "root": "Twins"}),
                 ("edit_animation", {"clip": BOB, "wrap": "loop"}), ("preview_animation", {"animation": S["box_b"], "action": "seek", "time": 0.1}),
                 ("get_asset_info", {"path": BOB})):
        r = await ed.call_raw(*call)
        keys(r.structured)
    await ed.call("preview_animation", animation=S["box_b"], action="stop")
    # binding statuses that need a broken setup, ease names and file-format keys
    known |= {"missing_component", "lattice_index_out_of_range", "morph_index_out_of_range", "legacy_model_layout",
              "unaddressable_name", "duplicate_sibling_name", "missing_path", "quad_out", "in_out", "external_changes", "frame_rate",
              # results of calls this test does not repeat: create {from}, an extract, a missing path with a fix, > 50 users
              "added_player", "removed_tracks", "model_clip_users", "fix_with", "users_total", "fix_path", "fix_conflict",
              "rest_mismatch", "rig_mismatch",
              # layers, masks and controllers (test_animation_layers / test_animator_controller): their results, the
              # mask's humanoid parts and the clip / controller file keys
              "humanoid_rig", "coverage_total", "mask_coverage", "live_weight", "normalized_time", "additive_reference",
              "automate_thresholds", "left_arm", "right_arm", "left_leg", "right_leg", "left_fingers", "right_fingers",
              # the Animator absorption's mask: the hands' body parts, a coverage rule (body_part:<part>) and
              # an upstream version 1 mask's read_as (AnimatorTools, AnimatorAssetIO, AvatarMaskWeights)
              "left_hand", "right_hand", "body_part", "read_as", "upstream_v1",
              # property tracks (test_animation_properties): a status, the tangent key member and
              # preview_animation's count of previewed properties
              "unknown_property", "tangent_mode", "properties_total",
              # humanoid clips (test_animation_retarget): get_animation's realisation and report, the Avatar
              # (get_asset_info, set_humanoid) and the humanoid bone names
              "retarget_report", "scale_ratio", "foot_ik", "missing_on_target", "unmapped_source_bones", "hips_local_position",
              "local_rotation", "set_humanoid", "animation_type", "extra_mapped", "human_scale", "extra_bones",
              "humanoid_position", "left_upper_arm", "left_index_proximal", "left_eye", "right_eye", "right_toes",
              "left_foot", "right_foot"}
    words = set(re.findall(r"(?<![\w./:-])[a-z][a-z0-9]*(?:_[a-z0-9]+)+(?![\w(])", text))
    unknown = sorted(w for w in words if w not in known)
    ed.check(not unknown, f"the guide names unknown things: {unknown}")
    # `name {...}`: a tool call, or an argument shown with its object (`create {from: ...}`)
    calls = set(re.findall(r"`([a-z][a-z0-9_]*) \{", text))
    tools = set(ed.tool_names)
    ed.check(calls & tools and calls <= known, f"calls in the guide that are no tools or arguments: {sorted(calls - known)}")


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
