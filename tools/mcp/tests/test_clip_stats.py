"""S74 R5, R7, R53 (the facts of step 1): get_animation and get_asset_info report a clip's `stats`,
a model's get_asset_info gives each animation's keys and runtime_bytes, asking changes nothing the
pool holds, get_frame_times {monitors} has the Animator's monitors, and get_memory_stats lists the
loaded clips.

Runs on a scratch copy of tools/fixtures/pose-animation.
"""

from harness import Editor, REPO

CI = True
EDITOR = {"project": REPO / "tools" / "fixtures" / "pose-animation"}
TIMEOUT = 600

MODEL = "assets/models/hero_animated.gltf"
CLIP = "assets/anim/s74_stats.anim"
CONTROLLER = "assets/anim/s74.controller"
STAT_KEYS = {"runtime_bytes", "file_bytes", "keys", "tracks", "length_s", "constant_tracks"}
MONITORS = ["Animators", "Animators sampled", "Animators culled", "Anim Prepare ms", "Anim Wave ms", "Anim Finalize ms", "Clip memory"]


async def clip_memory(ed: Editor) -> int:
    stats = await ed.call("get_memory_stats", prefix="Animation.Clips")
    records = [record for record in stats["records"] if record["name"] == "Animation.Clips"]
    return records[0]["current"] if records else 0


async def test_anim_stats(ed: Editor):
    await ed.call("edit_animation", clip=CLIP, create={}, wrap="loop", tracks=[
        {"path": "Moves", "channel": "position", "keys": [{"time": 0, "value": [0, 0, 0]}, {"time": 1.25, "value": [1, 0, 0]}]},
        {"path": "Holds", "channel": "position", "keys": [{"time": 0, "value": [1, 2, 3]}, {"time": 1.25, "value": [1, 2, 3]}]},
    ])
    before = await clip_memory(ed)

    got = await ed.call("get_animation", clip=CLIP)
    stats = got["stats"]
    ed.check(set(stats) == STAT_KEYS, f"the field table's keys: {sorted(stats)}")
    ed.check(stats["tracks"] == 2 and stats["keys"] == 4, f"counts: {stats}")
    ed.check(stats["length_s"] == 1.25, f"length: {stats}")
    ed.check(stats["constant_tracks"] == 1, f"Holds: {stats}")
    ed.check(stats["runtime_bytes"] > 0, f"memory: {stats}")
    on_disk = await ed.call("get_asset_info", path=CLIP)
    ed.check(on_disk["clip"]["stats"] == stats, f"the same numbers: {on_disk['clip']['stats']} != {stats}")
    ed.check(stats["file_bytes"] == on_disk["size"], f"the file's size: {stats} {on_disk['size']}")
    summary = await ed.call_raw("get_asset_info", {"path": CLIP})
    ed.check("Memory " in summary.text, f"the summary names the memory: {summary.text[:400]}")
    ed.check(await clip_memory(ed) == before, "asking loads nothing that stays")


async def test_model_stats(ed: Editor):
    before = await clip_memory(ed)
    info = await ed.call("get_asset_info", path=MODEL)
    animations = info["model"]["animations"]
    ed.check(len(animations) > 0, "the fixture's model has animations")
    for animation in animations:
        ed.check(animation["keys"] > 0 and animation["runtime_bytes"] > 0, f"{animation}")
        got = await ed.call("get_animation", clip=f"{MODEL}#{animation['name']}", max_keys=1)
        stats = got["stats"]
        ed.check(stats["runtime_bytes"] == animation["runtime_bytes"], f"{animation['name']}: {stats} {animation}")
        ed.check(stats["keys"] == animation["keys"], f"{animation['name']}: {stats} {animation}")
        ed.check(stats["file_bytes"] is None, f"a model clip has no file of its own: {stats}")
    ed.check(await clip_memory(ed) == before, "asking loads nothing that stays")


async def test_monitors_and_memory(ed: Editor):
    await ed.call("edit_animator_controller", path=CONTROLLER, create=True, ops=[
        {"op": "add_state", "name": "Walk", "clip": f"{MODEL}#Walk", "default": True}])
    await ed.call("add_component", entity="Hero", component="animator", values={"animator": {"controller": CONTROLLER}})
    await ed.call("play", action="start")
    await ed.call("wait", frames=30)
    r = await ed.call("get_frame_times", window=20, monitors=MONITORS)
    memory = await clip_memory(ed)
    await ed.call("play", action="stop")
    for name in MONITORS:
        ed.check(name in r["monitors"], f"{name}: {sorted(r['monitors'])}")
    ed.check(r["monitors"]["Animators"]["latest"] >= 1, f"Animators: {r['monitors']['Animators']}")
    ed.check(r["monitors"]["Animators sampled"]["latest"] >= 1, f"sampled: {r['monitors']['Animators sampled']}")
    ed.check(r["monitors"]["Anim Wave ms"]["unit"] == "ms", f"{r['monitors']['Anim Wave ms']}")
    ed.check(r["monitors"]["Clip memory"]["unit"] == "bytes", f"{r['monitors']['Clip memory']}")
    ed.check(r["monitors"]["Clip memory"]["latest"] > 0, f"{r['monitors']['Clip memory']}")
    ed.check(memory > 0, f"Animation.Clips while playing: {memory}")
