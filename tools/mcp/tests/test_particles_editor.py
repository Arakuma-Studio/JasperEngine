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
