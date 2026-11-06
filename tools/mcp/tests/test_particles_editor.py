"""S20 particles in the editor (docs/specs/S20-particles.md R27-R29, AC14): module add / remove /
enable and a curve key edited through the Inspector's command path (ParticleEffectEdit, which the
particles group's tools share), undo / redo giving the effect's JSON back exactly, the Scene view's
Particle Effect preview (a selected system plays in Edit mode; a scrub lands on its time), and a
parameter-only edit during the preview keeping the live particles (no restart).

Runs in one new empty project; tests run in order and share the editor.
"""

from __future__ import annotations

import asyncio
import json
import time

from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Particles Editor CI"}
TIMEOUT = 900

S: dict = {}


async def setup(ed: Editor) -> dict:
    if S.get("ed") is ed:
        await ed.call("select_scene", scene=S["scene"])
        return S
    S.clear()
    S["ed"] = ed
    ed.require_tools("create_particles", "tool_groups")
    await ed.call("tool_groups", enable=["particles"])
    made = await ed.call("create_scene", name="ParticleEditor", type="3D", path="scenes/ParticleEditor")
    S["scene"] = made["scene"]["id"]
    await ed.call("select_scene", scene=S["scene"])
    fire = await ed.call("create_particles", scene=S["scene"], name="Sparks", preset="sparks")
    S["system"] = fire["particle_system"]["id"]
    await ed.call("edit_particle_effect", scene=S["scene"], entity=S["system"], patch={"seed": 3})
    return S


async def effect(ed: Editor) -> dict:
    return (await ed.call("get_particle_effect", scene=S["scene"], entity=S["system"]))["effect"]


async def stats(ed: Editor) -> dict:
    return (await ed.call("get_particle_stats", scene=S["scene"], entity=S["system"]))["systems"][0]


async def test_modules_and_curve_key_undo(ed: Editor):
    s = await setup(ed)
    original = await effect(ed)
    await ed.call("add_particle_module", scene=s["scene"], entity=s["system"], module="sizeOverLifetime",
                  fields={"size": {"mode": "curve", "curve": [[0, 1], [1, 0]]}})
    added = await effect(ed)
    ed.check_eq(added["modules"]["sizeOverLifetime"]["size"]["mode"], "curve")
    # a curve key moved (the Inspector's key table edits the same field)
    await ed.call("edit_particle_effect", scene=s["scene"], entity=s["system"],
                  patch={"modules": {"sizeOverLifetime": {"size": {"mode": "curve", "curve": [[0, 1, 0, 0], [0.5, 0.25, 0, 0], [1, 0, 0, 0]]}}}})
    keyed = await effect(ed)
    ed.check_eq(len(keyed["modules"]["sizeOverLifetime"]["size"]["curve"]), 3, "the new key")
    # disabled, then enabled again
    await ed.call("edit_particle_effect", scene=s["scene"], entity=s["system"], patch={"modules": {"sizeOverLifetime": {"enabled": False}}})
    ed.check_eq((await effect(ed))["modules"]["sizeOverLifetime"].get("enabled"), False)
    await ed.call("edit_particle_effect", scene=s["scene"], entity=s["system"], patch={"modules": {"sizeOverLifetime": {"enabled": True}}})
    await ed.call("remove_particle_module", scene=s["scene"], entity=s["system"], module="sizeOverLifetime")
    ed.check("sizeOverLifetime" not in (await effect(ed))["modules"], "removed")

    # five steps back: each restores the JSON exactly
    await ed.call("undo", scene=s["scene"])
    await ed.call("undo", scene=s["scene"])
    await ed.call("undo", scene=s["scene"])
    ed.check_eq(await effect(ed), keyed, "undo back to the curve key edit")
    await ed.call("undo", scene=s["scene"])
    ed.check_eq(await effect(ed), added, "undo back to the added module")
    await ed.call("undo", scene=s["scene"])
    ed.check_eq(await effect(ed), original, "undo back to the original effect")
    await ed.call("redo", scene=s["scene"])
    await ed.call("redo", scene=s["scene"])
    ed.check_eq(await effect(ed), keyed, "redo forward to the curve key edit")


async def test_preview_plays_selected(ed: Editor):
    """A selected system plays in Edit mode (the Particle Effect panel); time advances."""
    s = await setup(ed)
    await ed.call("select_entities", scene=s["scene"], entity=s["system"])
    await ed.call("control_particles", scene=s["scene"], entity=s["system"], action="restart")
    await ed.call("wait", frames=20)
    first = await stats(ed)
    await ed.call("wait", frames=20)
    second = await stats(ed)
    ed.check(second["time"] > first["time"] > 0, f"the preview advances: {first['time']} -> {second['time']}")
    ed.check(second["particle_count"] > 0, f"particles: {second}")


async def test_scrub(ed: Editor):
    """The preview's scrub (a seek over frames from the nearest checkpoint) lands on the time, forward and back."""
    s = await setup(ed)
    for time in (5.0, 2.0, 0.5):
        got = await ed.call("control_particles", scene=s["scene"], entity=s["system"], action="seek", time=time)
        ed.check(abs(got["stats"]["time"] - time) < 0.05, f"scrub to {time}: {got['stats']}")
        await ed.call("wait", frames=5)
        ed.check(abs((await stats(ed))["time"] - time) < 0.05, f"held at {time}")


async def test_param_edit_keeps_particles(ed: Editor):
    """A parameter-only edit during the preview rebinds without a restart: the particles stay."""
    s = await setup(ed)
    await ed.call("control_particles", scene=s["scene"], entity=s["system"], action="seek", time=1.0)
    before = await stats(ed)
    ed.check(before["particle_count"] > 0, f"particles before: {before}")
    await ed.call("edit_particle_effect", scene=s["scene"], entity=s["system"],
                  patch={"modules": {"main": {"startColor": [1, 0.2, 0.1, 1]}}})
    await ed.call("wait", frames=2)
    after = await stats(ed)
    ed.check_eq(after["particle_count"], before["particle_count"], "the live particles stay")
    ed.check(after["time"] >= before["time"], f"no restart: {before['time']} -> {after['time']}")
    # a structural edit (a module added) restarts
    await ed.call("add_particle_module", scene=s["scene"], entity=s["system"], module="trails")
    await ed.call("wait", frames=2)
    restarted = await stats(ed)
    ed.check(restarted["time"] < before["time"] or restarted["particle_count"] != before["particle_count"],
             f"a structural edit restarts: {before} -> {restarted}")


async def test_misspelled_patch_field_is_refused(ed: Editor):
    """F10: an unknown field in a patch is refused with a did-you-mean, and nothing is written; the
    warnings of a patch that goes through come back."""
    s = await setup(ed)
    path = "assets/fx/typo.particles"
    await ed.call("create_particle_effect", path=path, preset="sparks")
    target = ed.project / path
    before = target.read_bytes()

    r = await ed.call_raw("edit_particle_effect", {"path": path, "patch": {"modules": {"emission": {"rateOverTim": 20}}}})
    ed.check(r.is_error, f"the misspelled field is refused: {r.text[:300]}")
    ed.check("did you mean 'rateOverTime'" in r.text, f"with what it meant: {r.text[:300]}")
    found = [d for d in (r.structured or {}).get("diagnostics", []) if d["path"] == "emission.rateOverTim"]
    ed.check(found and found[0]["severity"] == "error" and "did you mean" in found[0]["message"], f"its diagnostic: {r.structured}")
    ed.check_eq(target.read_bytes(), before, "the file is untouched")

    for patch, fragment in (({"duratoin": 3}, "did you mean 'duration'"),
                            ({"modules": {"emission": {"rateOverTime": {"mode": "twoConstants", "constant": 1, "constantMxa": 2}}}}, "did you mean 'constantMax'"),
                            ({"modules": {"emision": {"rateOverTime": 2}}}, "did you mean 'emission'")):
        r = await ed.call_raw("edit_particle_effect", {"path": path, "patch": patch})
        ed.check(r.is_error and fragment in r.text, f"{patch}: {r.text[:300]}")
        ed.check_eq(target.read_bytes(), before, f"{patch} wrote nothing")

    # the same through an inline effect and through add_particle_module's fields
    r = await ed.call_raw("edit_particle_effect", {"scene": s["scene"], "entity": s["system"], "patch": {"duratoin": 3}})
    ed.check(r.is_error, f"an inline effect's patch: {r.text[:300]}")
    r = await ed.call_raw("add_particle_module", {"path": path, "module": "noise", "fields": {"strenght": 3}})
    ed.check(r.is_error and "did you mean 'strength'" in r.text, f"add_particle_module's fields: {r.text[:300]}")
    ed.check_eq(target.read_bytes(), before, "add_particle_module wrote nothing")

    # a patch that goes through says what it left wrong
    done = await ed.call("edit_particle_effect", path=path, patch={"modules": {"renderer": {"renderMode": "none"}}})
    ed.check([d for d in done["diagnostics"] if d["severity"] == "warning" and d["path"] == "renderer.renderMode"], f"its warnings: {done['diagnostics']}")


async def external(ed: Editor) -> dict:
    r = await ed.call_raw("editor_status")
    return (r.structured if isinstance(r.structured, dict) else {}).get("external_changes") or {}


async def test_particles_file_is_text_and_tracked(ed: Editor):
    """F11: read_file and write_file take a .particles; another program's edit reports its problems."""
    await setup(ed)
    path = "assets/fx/direct.particles"
    await ed.call("create_particle_effect", path=path, preset="sparks")
    target = ed.project / path

    read = await ed.call_raw("read_file", {"path": path})
    ed.check(not read.is_error and '"version"' in read.text, f"read_file: {read.text[:300]}")
    good = target.read_text()

    def with_field(key: str, value) -> str:
        effect = json.loads(good)
        effect[key] = value
        return json.dumps(effect, indent=2) + "\n"

    written = await ed.call_raw("write_file", {"path": path, "content": with_field("duration", 2)})
    ed.check(not written.is_error, f"write_file: {written.text[:300]}")
    ed.check_eq((await ed.call("get_particle_effect", path=path))["effect"].get("duration"), 2, "write_file's effect is the one read")
    warned = await ed.call("write_file", path=path, content=with_field("duratoin", 2))
    ed.check([w for w in warned["warnings"] if "did you mean 'duration'" in w], f"write_file's warnings: {warned}")
    invalid = await ed.call_raw("write_file", {"path": path, "content": '{"version": 1, "duration": -1}'})
    ed.check(invalid.is_error and "Not a valid particle effect" in invalid.text, f"an invalid effect is refused: {invalid.text[:300]}")
    await ed.call("write_file", path=path, content=good)
    await external(ed)

    time.sleep(0.05)
    target.write_text(with_field("duratoin", 2))
    ext = await external(ed)
    found = [p for p in ext.get("problems", []) if p.get("path") == path]
    ed.check(found and "did you mean 'duration'" in found[0]["message"], f"the bad field is reported: {ext}")
    time.sleep(0.05)
    target.write_text("{not json")
    ext = await external(ed)
    found = [p for p in ext.get("problems", []) if p.get("path") == path]
    ed.check(found and "JSON" in found[0]["message"], f"a broken file is reported: {ext}")
    time.sleep(0.05)
    target.write_text(good)
    await external(ed)

    # the tool's own write is not another program's
    await ed.call("edit_particle_effect", path=path, patch={"duration": 4})
    for _ in range(3):
        ext = await external(ed)
        ed.check(path not in ext.get("changed", []), f"edit_particle_effect's own write: {ext}")
        await asyncio.sleep(0.25)


async def test_reports_reproduce_a_run(ed: Editor):
    """F13: stats and captures name the seed in use and a hash of the state: the same seed gives the
    same hash, another seed another, and the seed a seedless effect drew reproduces its run."""
    s = await setup(ed)
    async def capture(time_s: float = 0.5) -> dict:
        r = await ed.call_raw("capture_particles", {"scene": s["scene"], "entity": s["system"], "time": time_s, "max_width": 64})
        ed.check(not r.is_error, f"capture: {r.text[:300]}")
        return r.value["stats"]

    await ed.call("edit_particle_effect", scene=s["scene"], entity=s["system"], patch={"seed": 3})
    first = await capture()
    ed.check_eq(first.get("seed"), 3, f"the seed in use: {first}")
    ed.check(isinstance(first.get("state_hash"), str) and len(first["state_hash"]) == 16, f"the state's hash: {first}")
    ed.check(first["particle_count"] > 0, f"particles to hash: {first}")
    again = await capture()
    ed.check_eq(again["state_hash"], first["state_hash"], "the same seed and time give the same state")
    live = await stats(ed)
    ed.check_eq((live["seed"], live["state_hash"]), (3, first["state_hash"]), "get_particle_stats reports them too")

    await ed.call("edit_particle_effect", scene=s["scene"], entity=s["system"], patch={"seed": 4})
    other = await capture()
    ed.check_eq(other["seed"], 4, "the new seed")
    ed.check(other["state_hash"] != first["state_hash"], f"another seed, another state: {first} {other}")

    # a seedless effect draws one per play; the reported one reproduces that run
    await ed.call("edit_particle_effect", scene=s["scene"], entity=s["system"], patch={"seed": 0})
    await ed.call("control_particles", scene=s["scene"], entity=s["system"], action="stop")
    await ed.call("control_particles", scene=s["scene"], entity=s["system"], action="clear")
    await ed.call("control_particles", scene=s["scene"], entity=s["system"], action="play")
    drawn = await capture()
    ed.check(drawn["seed"] not in (0, 3, 4), f"the seed drawn: {drawn}")
    await ed.call("edit_particle_effect", scene=s["scene"], entity=s["system"], patch={"seed": drawn["seed"]})
    pinned = await capture()
    ed.check_eq(pinned["seed"], drawn["seed"], "the pinned seed")
    ed.check_eq(pinned["state_hash"], drawn["state_hash"], "pinning the reported seed reproduces the run")
    await ed.call("edit_particle_effect", scene=s["scene"], entity=s["system"], patch={"seed": 3})


async def test_state_hash_tells_empty_runs_apart(ed: Editor):
    """Fix round 1 of BF23: with nothing alive yet, two seeds still report different runs, and the same
    state at a later tick is not the same run."""
    s = await setup(ed)
    async def capture(time_s: float) -> dict:
        r = await ed.call_raw("capture_particles", {"scene": s["scene"], "entity": s["system"], "time": time_s, "max_width": 64})
        ed.check(not r.is_error, f"capture: {r.text[:300]}")
        return r.value["stats"]

    await ed.call("edit_particle_effect", scene=s["scene"], entity=s["system"], patch={"seed": 3})
    first = await capture(0)
    again = await capture(0)
    ed.check_eq(again["state_hash"], first["state_hash"], "the same seed and time")
    await ed.call("edit_particle_effect", scene=s["scene"], entity=s["system"], patch={"seed": 4})
    other = await capture(0)
    ed.check_eq(other["particle_count"], first["particle_count"], "both as empty")
    ed.check(other["state_hash"] != first["state_hash"], f"another seed, another run: {first} {other}")
    await ed.call("edit_particle_effect", scene=s["scene"], entity=s["system"], patch={"seed": 3})
