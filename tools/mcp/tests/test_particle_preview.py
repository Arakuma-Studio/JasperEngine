"""S91 agent side (docs/specs/S91-effect-authoring.md R12, R26-R31, R33; AC10-AC13): preview_particle_effect and
bake_particle_sheet, the small extensions (dry_run, module_fields, with_children) and check --particles.

- the group: both tools hidden until the particles group is enabled;
- determinism (AC11): the same request twice gives the same filmstrip bytes and state_hash; capacity_hit fails on a
  burst over Max Particles (and passes under it), invisible fails on an empty effect, fail_on makes an error result;
- overdraw (AC12): the analytic layers agree with the GPU heat row (overdraw_map) at the cells within 10%;
- non-mutation (AC13): a preview of a scene's subtree leaves the live systems, the scene, the Scene view camera and
  the undo histories as they were;
- the bake (AC10): without output nothing is written; with output the PNG and the .particles are one undo step that
  undo removes; the independent oracle (tools/mcp/particle_sheet_ref.py) finds the cell's straight alpha and colour,
  and the cell composited over the dark background equals the preview's cell at that time;
- jobs: wait false returns a job get_job finishes; save_path writes the image the call returns;
- thresholds: a changed particles.qa.* threshold is listed;
- the CLI: check --particles reports the FAIL in compiler style and exits 1.

Runs in one new empty project; tests run in order and share the editor.
"""

from __future__ import annotations

import subprocess

import harness
import particle_sheet_ref as ref
import pngdiff
from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Particle Preview CI"}
TIMEOUT = 1500

TOOLS = ("preview_particle_effect", "bake_particle_sheet")
CHECK_KEYS = {"id", "kind", "status", "round", "measured", "threshold", "threshold_source", "limited_by", "at", "fix_hint", "method"}


def burst_effect(count: int, max_particles: int, **main) -> dict:
    """A one-shot burst from a sphere, seed 5 (a module the file does not list is absent: no shape emits from a point)."""
    fields = {"startLifetime": 1, "startSpeed": 2, "startSize": 0.2}
    fields.update(main)
    return {"duration": 1, "looping": False, "seed": 5, "maxParticles": max_particles,
            "modules": {"main": fields, "emission": {"rateOverTime": 0, "bursts": [{"time": 0, "count": count}]},
                        "shape": {"shape": "sphere", "radius": 1}}}


async def make_effect(ed: Editor, path: str, effect: dict) -> None:
    """effect with its modules keyed by type, as edit_particle_effect patches them; the file holds them as a list"""
    whole = dict(effect)
    whole["modules"] = [{"type": key, **fields} for key, fields in effect.get("modules", {}).items()]
    await ed.call("create_particle_effect", path=path, effect=whole, overwrite=True)


def check_of(result: dict, check_id: str) -> dict:
    for check in result["checks"]:
        if check["id"] == check_id:
            return check
    raise harness.TestFailure(f"no check {check_id} in {[c['id'] for c in result['checks']]}")


async def test_group(ed: Editor):
    """Hidden by default, listed once the particles group is enabled."""
    ed.require_tools("tool_groups", *TOOLS)
    await ed.call("tool_groups", disable=["particles"])
    default = {t.name for t in await ed.tools(groups=None)}
    ed.check(not default & set(TOOLS), f"hidden by default: {sorted(default & set(TOOLS))}")
    await ed.call("tool_groups", enable=["particles"])
    listed = {t.name for t in await ed.tools(groups=None)}
    ed.check(set(TOOLS) <= listed, f"listed once enabled: {sorted(set(TOOLS) - listed)}")


async def test_same_request_same_bytes_and_capacity(ed: Editor):
    await make_effect(ed, "assets/fx/over.particles", burst_effect(600, 400))
    args = {"path": "assets/fx/over.particles", "frames": 3}
    first = await ed.call_raw("preview_particle_effect", args)
    second = await ed.call_raw("preview_particle_effect", args)
    ed.check(not first.is_error and first.images, f"preview: {first.text[:400]}")
    ed.check(first.images[0][0] == second.images[0][0], "the same request gives the same filmstrip bytes")
    ed.check_eq(first.value["state_hash"], second.value["state_hash"])
    ed.check_eq(first.value["seeds"][0]["seed"], 5, "the effect's seed")
    width, height, _ = pngdiff.decode(first.images[0][0])
    ed.check_eq(width, 3 * 192 + 2, "three 192 px cells and their separators")
    for check in first.value["checks"]:
        ed.check_eq(set(check), CHECK_KEYS, f"the shared row shape: {check['id']}")
    hit = check_of(first.value, "capacity_hit")
    ed.check_eq(hit["status"], "FAIL", f"capacity_hit: {hit}")
    ed.check_eq(hit["measured"]["never_born"], 200)
    ed.check_in("estimate, not measured on a device", first.text)
    ed.check_eq(first.value["metrics"]["cost"]["method"], "estimate_from_bench_table")
    ed.check_eq(first.value["metrics"]["scene_context"]["world_budget"], "not_applied")
    ed.check("next" in first.value, "the guidance reaches the agent")

    await make_effect(ed, "assets/fx/under.particles", burst_effect(100, 400))
    under = await ed.call("preview_particle_effect", path="assets/fx/under.particles", frames=0)
    ed.check_eq(check_of(under, "capacity_hit")["status"], "PASS")

    reseeded = await ed.call("preview_particle_effect", path="assets/fx/over.particles", frames=0, seed=9)
    ed.check(reseeded["state_hash"] != first.value["state_hash"], "another seed is another run")


async def test_invisible_and_fail_on(ed: Editor):
    await make_effect(ed, "assets/fx/empty.particles", {"duration": 1, "looping": False, "modules": {"emission": {"rateOverTime": 0}}})
    result = await ed.call("preview_particle_effect", path="assets/fx/empty.particles", frames=2)
    invisible = check_of(result, "invisible")
    ed.check_eq(invisible["status"], "FAIL", f"invisible: {invisible}")
    ed.check_eq(result["verdict"], "FAIL")
    r = await ed.call_raw("preview_particle_effect", {"path": "assets/fx/empty.particles", "frames": 0, "fail_on": "fail"})
    ed.check(r.is_error, "fail_on: fail turns a FAIL verdict into an error result")
    ed.check_in("FAIL invisible", r.text)
    err = await ed.call("preview_particle_effect", path="assets/fx/empty.particles", frames=30, expect_error=True)
    ed.check_in("frames is 0 to 24", err)
    err = await ed.call("preview_particle_effect", expect_error=True)
    ed.check_in("Give path", err)


async def test_overdraw_heat_row_agrees(ed: Editor):
    """Twelve still particles stacked at one point: the analytic layers and the heat row's agree within 10%."""
    effect = {"duration": 1, "looping": False, "seed": 3,
              "modules": {"main": {"startLifetime": 2, "startSpeed": 0, "startSize": 1},
                          "emission": {"rateOverTime": 0, "bursts": [{"time": 0, "count": 12}]}, "shape": {"enabled": False}}}
    await make_effect(ed, "assets/fx/stack.particles", effect)
    # (at 0 s nothing is born yet, and the stack has died by the run's end: cells while it lives)
    r = await ed.call_raw("preview_particle_effect", {"path": "assets/fx/stack.particles", "times": [0.5, 1.0], "overdraw_map": True})
    ed.check(not r.is_error, r.text[:400])
    heat = r.value["metrics"]["overdraw"]["heat"]
    ed.check(abs(heat["peak_layers"] - heat["analytic_peak_layers"]) <= 0.1 * heat["analytic_peak_layers"],
             f"heat row {heat['peak_layers']} against analytic {heat['analytic_peak_layers']} layers")
    ed.check(abs(heat["analytic_peak_layers"] - 12) < 0.01, f"the analytic peak is the stack: {heat}")
    width, height, _ = pngdiff.decode(r.images[0][0])
    ed.check(height > 2 * 192, f"the heat row under the cells: {width}x{height}")


async def test_preview_leaves_the_scene_alone(ed: Editor):
    """A subtree preview: the live systems, the scene, the camera and the undo histories do not change."""
    made = await ed.call("create_scene", name="PreviewLab", type="3D", path="scenes/PreviewLab")
    scene = made["scene"]["id"]
    await ed.call("select_scene", scene=scene)
    root = await ed.call("create_entity", scene=scene, type="empty", name="Effect")
    for name, x in (("Sparks", 0), ("Smoke", 1)):
        made = await ed.call("create_particles", scene=scene, name=name, preset="sparks" if name == "Sparks" else "smoke",
                             position=[x, 0, 0], parent=root["entity_id"])
    await ed.call("control_particles", scene=scene, entity="Effect", action="seek", time=0.7, with_children=True)
    live = await ed.call("get_particle_stats", scene=scene)
    camera = await ed.call("set_scene_camera")
    project_steps = len((await ed.call("get_undo_history", scope="project"))["entries"])
    scene_steps = len((await ed.call("get_undo_history", scene=scene))["entries"])

    result = await ed.call("preview_particle_effect", scene=scene, entity="Effect", frames=2)
    ed.check_eq(len(result["seeds"]), 2, f"the subtree's two systems: {result['seeds']}")

    after = await ed.call("get_particle_stats", scene=scene)
    ed.check_eq([(s["state_hash"], s["time"]) for s in after["systems"]], [(s["state_hash"], s["time"]) for s in live["systems"]],
                "the live systems did not move")
    ed.check_eq(await ed.call("set_scene_camera"), camera, "the Scene view camera is unchanged")
    ed.check_eq(len((await ed.call("get_undo_history", scope="project"))["entries"]), project_steps, "no project undo step")
    ed.check_eq(len((await ed.call("get_undo_history", scene=scene))["entries"]), scene_steps, "no scene undo step")

    # R31: the subtree plays, seeks and captures as one
    sought = await ed.call("control_particles", scene=scene, entity="Effect", action="seek", time=0.4, with_children=True)
    ed.check_eq(len(sought["systems"]), 2, f"seek with_children: {sought}")
    ed.check(all(abs(s["time"] - 0.4) < 0.05 for s in sought["systems"]), f"both at 0.4 s: {sought['systems']}")
    err = await ed.call("control_particles", scene=scene, entity="Effect", action="play", expect_error=True)
    ed.check_in("has no Particle System", err, "without with_children the parent alone is addressed")
    captured = await ed.call_raw("capture_particles", {"scene": scene, "entity": "Effect", "time": 0.5, "with_children": True, "max_width": 256})
    ed.check(not captured.is_error and captured.images, captured.text[:300])
    ed.check_eq(len(captured.value["stats"]["systems"]), 2)


async def test_dry_run_and_module_fields(ed: Editor):
    await make_effect(ed, "assets/fx/dry.particles", burst_effect(10, 100))
    before = (ed.project / "assets/fx/dry.particles").read_bytes()
    steps = len((await ed.call("get_undo_history", scope="project"))["entries"])
    dry = await ed.call("edit_particle_effect", path="assets/fx/dry.particles", patch={"maxParticles": 50}, dry_run=True)
    ed.check_eq(dry["would_change"], True)
    ed.check_eq(dry["effect"]["maxParticles"], 50, "the effect the patch would make")
    ed.check_eq((ed.project / "assets/fx/dry.particles").read_bytes(), before, "nothing written")
    ed.check_eq(len((await ed.call("get_undo_history", scope="project"))["entries"]), steps, "no undo step")
    err = await ed.call("edit_particle_effect", path="assets/fx/dry.particles", patch={"maxParticels": 50}, dry_run=True, expect_error=True)
    ed.check_in("would be refused", err)

    fields = await ed.call("get_particle_effect", module_fields=["emission", "effect"])
    emission = {f["key"]: f for f in fields["module_fields"]["emission"]["fields"]}
    ed.check_eq(emission["rateOverTime"]["default"], 10, "the Inspector table's default")
    effect = {f["key"]: f for f in fields["module_fields"]["effect"]["fields"]}
    ed.check_eq(effect["maxParticles"]["min"], 1)
    err = await ed.call("get_particle_effect", module_fields=["glitter"], expect_error=True)
    ed.check_in("no module", err)


async def test_bake_sheet(ed: Editor):
    """One alpha-blended square, half transparent red: the oracle reads its alpha and colour back."""
    effect = {"duration": 1, "looping": False, "seed": 2,
              "modules": {"main": {"startLifetime": 2, "startSpeed": 0, "startSize": 1, "startColor": [1, 0, 0, 0.5]},
                          "emission": {"rateOverTime": 0, "bursts": [{"time": 0, "count": 1}]}, "shape": {"enabled": False},
                          "renderer": {"untexturedShape": "square"}}}
    await make_effect(ed, "assets/fx/square.particles", effect)
    args = {"path": "assets/fx/square.particles", "columns": 2, "rows": 1, "cell_size": 192, "seconds": 1}
    preview = await ed.call_raw("bake_particle_sheet", args)
    ed.check(not preview.is_error and preview.images, preview.text[:400])
    ed.check(not (ed.project / "assets/fx/square_sheet.png").exists(), "without output nothing is written")

    steps = len((await ed.call("get_undo_history", scope="project"))["entries"])
    baked = await ed.call_raw("bake_particle_sheet", {**args, "output": "assets/fx/square_sheet"})
    ed.check(not baked.is_error, baked.text[:400])
    ed.check_eq(len((await ed.call("get_undo_history", scope="project"))["entries"]), steps + 1, "one undo step")
    png = (ed.project / "assets/fx/square_sheet.png").read_bytes()
    ed.check_eq(png, baked.images[0][0], "the file is the image the call returned")
    sheet = pngdiff.decode(png)
    ed.check_eq(sheet[:2], (384, 192))
    cell = ref.cell(sheet, 2, 1, 0)
    empty, alphas, colours = ref.coverage(cell)
    ed.check(empty > 0 and alphas, "a particle on a transparent cell")
    inside = [a for a in alphas if abs(a - 128) <= 4]
    ed.check(len(inside) > 0.8 * len(alphas), f"straight alpha 0.5: {sorted(set(alphas))[:12]}")
    ed.check(all(c[0] >= 250 and c[1] <= 4 and c[2] <= 4 for c, a in zip(colours, alphas) if abs(a - 128) <= 4), "straight red")

    # the cell over the dark background is what the preview shows at that time
    # the first frame of 2 over 1 s is the middle of its half: 0.25 s
    shown = await ed.call_raw("preview_particle_effect", {"path": "assets/fx/square.particles", "times": [0.25], "seconds": 1,
                                                          "view": "front", "background": "dark"})
    strip = pngdiff.decode(shown.images[0][0])
    preview_cell = pngdiff.crop(strip, 0, 0, 192, 192)
    composed = ref.over(cell, (31, 31, 33))
    worst = ref.max_difference(composed, (preview_cell[0], preview_cell[1], bytearray(preview_cell[2])))
    ed.check(worst <= 3, f"the sheet over the dark background differs from the preview by {worst}")

    effect_text = (ed.project / "assets/fx/square_sheet.particles").read_text()
    ed.check_in('"tilesX": 2', effect_text)
    await ed.call("undo", scope="project")
    ed.check(not (ed.project / "assets/fx/square_sheet.png").exists(), "undo removes the sheet")
    ed.check(not (ed.project / "assets/fx/square_sheet.particles").exists(), "and its effect")
    err = await ed.call("bake_particle_sheet", path="assets/fx/square.particles", columns=5, rows=5, expect_error=True)
    ed.check_in("at most 24 frames", err)


async def test_job_and_save_path(ed: Editor):
    started = await ed.call("preview_particle_effect", path="assets/fx/under.particles", frames=1, wait=False)
    ed.check("job_id" in started, f"wait false: {started}")
    for _ in range(600):
        job = await ed.call("get_job", job_id=started["job_id"])
        if job["status"] != "running":
            break
        await ed.call("wait", frames=5)
    ed.check_eq(job["status"], "succeeded", f"job: {job}")
    r = await ed.call_raw("preview_particle_effect", {"path": "assets/fx/under.particles", "frames": 1, "save_path": "renders/under.png"})
    ed.check_eq((ed.project / "renders/under.png").read_bytes(), r.images[0][0], "save_path holds the image")


async def test_changed_thresholds_are_listed(ed: Editor):
    await ed.call("set_project_settings", values={"particles.qa.pop_percent": 80})
    try:
        result = await ed.call("preview_particle_effect", path="assets/fx/under.particles", frames=0)
        changed = {c["key"]: c for c in result["thresholds"]["changed"]}
        ed.check("particles.qa.pop_percent" in changed, f"changed: {result['thresholds']['changed']}")
        ed.check_eq(check_of(result, "pop")["threshold_source"], "particles.qa.pop_percent")
    finally:
        await ed.call("set_project_settings", values={"particles.qa.pop_percent": 30})


async def test_check_cli(ed: Editor):
    """check --particles: the FAIL in compiler style, exit 1; --files with an effect without FAIL (its pop is a warning) exits 0."""
    cmd = harness.editor_command() + ["check", "--project", str(ed.project), "--particles"]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    output = r.stdout + r.stderr
    ed.check_eq(r.returncode, 1, f"a FAIL is an error: {output[-2000:]}")
    ed.check_in("assets/fx/over.particles: error: capacity_hit FAIL", output)
    passing = subprocess.run(cmd + ["--files", str(ed.project / "assets/fx/under.particles")], capture_output=True, text=True, timeout=600)
    ed.check_eq(passing.returncode, 0, (passing.stdout + passing.stderr)[-2000:])


# -- review round 1 (each failed on the build before its fix) ------------------------------------

async def test_invisible_follows_the_whole_run(ed: Editor):
    """A visible one-shot is not invisible: with frames 0 and overdraw_map (no cell at all), and with two cells."""
    await make_effect(ed, "assets/fx/visible.particles", burst_effect(100, 400))
    heat_only = await ed.call("preview_particle_effect", path="assets/fx/visible.particles", frames=0, overdraw_map=True)
    ed.check_eq(check_of(heat_only, "invisible")["status"], "PASS", f"frames 0 with overdraw_map: {check_of(heat_only, 'invisible')}")
    two = await ed.call("preview_particle_effect", path="assets/fx/visible.particles", frames=2)
    ed.check_eq(check_of(two, "invisible")["status"], "PASS", f"two cells: {check_of(two, 'invisible')}")
    ed.check(all(0 < c["t"] < two["seconds"] for c in two["cells"]), f"cells inside the run: {two['cells']}")


async def test_cut_runs_and_capacity_target(ed: Editor):
    cut = await ed.call("preview_particle_effect", path="assets/fx/visible.particles", frames=0, seconds=0.5)
    ed.check_eq(check_of(cut, "never_ends")["status"], "SKIP", "a run cut short by seconds says nothing about the end")
    # a looping rate held at 50: the fix is the peak it needs (rate 100 x lifetime 2), not the births refused
    rate = {"duration": 1, "looping": True, "seed": 4, "maxParticles": 50,
            "modules": {"main": {"startLifetime": 2, "startSpeed": 1, "startSize": 0.1}, "emission": {"rateOverTime": 100},
                        "shape": {"shape": "sphere", "radius": 1}}}
    await make_effect(ed, "assets/fx/rate.particles", rate)
    held = await ed.call("preview_particle_effect", path="assets/fx/rate.particles", frames=0, seconds=4)
    ed.check_eq(check_of(held, "capacity_hit")["status"], "FAIL")
    target = held["fixes"][0]["patch"]["maxParticles"]
    ed.check(180 <= target <= 220, f"Max Particles to its peak, about 200: {target}")
    variable = dict(rate, tickRate=0, maxParticles=1000)
    await make_effect(ed, "assets/fx/variable.particles", variable)
    costed = await ed.call("preview_particle_effect", path="assets/fx/variable.particles", frames=0)
    ed.check_eq(costed["metrics"]["cost"]["ticks_per_frame"], 1.0, "a variable step ticks once a frame")


async def test_blown_out_only_for_additive(ed: Editor):
    white = {"duration": 1, "looping": False, "seed": 2,
             "modules": {"main": {"startLifetime": 2, "startSpeed": 0, "startSize": 1, "startColor": [1, 1, 1, 1]},
                         "emission": {"rateOverTime": 0, "bursts": [{"time": 0, "count": 5}]}, "shape": {"enabled": False},
                         "renderer": {"untexturedShape": "square"}}}
    await make_effect(ed, "assets/fx/white.particles", white)
    result = await ed.call("preview_particle_effect", path="assets/fx/white.particles", times=[0.5])
    ed.check_eq(check_of(result, "blown_out")["status"], "PASS", f"opaque white is not additive saturation: {check_of(result, 'blown_out')}")


async def test_long_scope_keeps_the_verdict_and_inactive_systems_are_left_out(ed: Editor):
    made = await ed.call("create_scene", name="DeepLab", type="3D", path="scenes/DeepLab")
    scene = made["scene"]["id"]
    await ed.call("select_scene", scene=scene)
    parent = None
    for level in range(5):
        created = await ed.call("create_entity", scene=scene, type="empty", name=f"Level{level}_" + "x" * 40,
                                **({"parent": parent} if parent else {}))
        parent = created["entity_id"]
    await ed.call("create_particles", scene=scene, name="Deep", preset="sparks", parent=parent)
    off = await ed.call("create_particles", scene=scene, name="Off", preset="smoke", parent=parent)
    await ed.call("set_entity_state", scene=scene, entity=off["particle_system"]["id"], active=False)
    r = await ed.call_raw("preview_particle_effect", {"scene": scene, "entity": parent, "frames": 0})
    ed.check_in("Verdict", r.text, "a deep subtree's path does not cut the verdict off")
    ed.check_eq(len(r.value["seeds"]), 1, f"the inactive system is not part of the effect: {r.value['seeds']}")

    camera = await ed.call("create_entity", scene=scene, type="camera", name="PlayCamera")
    await ed.call("set_main_camera", scene=scene, entity=camera["entity_id"])
    await ed.call("play", action="start", scene=scene)
    try:
        await ed.call("wait", frames=10)   # play starts over frames
        real = await ed.call("edit_particle_effect", scene=scene, entity="Deep", patch={"duration": 2}, expect_error=True)
        ed.check_in("playing", real, "the scene plays")
        err = await ed.call("edit_particle_effect", scene=scene, entity="Deep", patch={"duration": 2}, dry_run=True, expect_error=True)
        ed.check_in("playing", err, "a dry run is refused where the edit is")
    finally:
        await ed.call("play", action="stop")


async def test_bake_keeps_the_offset_and_stays_in_the_project(ed: Editor):
    raised = {"duration": 1, "looping": False, "seed": 2,
              "modules": {"main": {"startLifetime": 2, "startSpeed": 0, "startSize": 0.5},
                          "emission": {"rateOverTime": 0, "bursts": [{"time": 0, "count": 1}]},
                          "shape": {"shape": "sphere", "radius": 0, "position": [0, 1, 0]}}}
    await make_effect(ed, "assets/fx/raised.particles", raised)
    await ed.call("bake_particle_sheet", path="assets/fx/raised.particles", columns=2, rows=1, cell_size=64, output="assets/fx/raised_sheet")
    import json
    baked = json.loads((ed.project / "assets/fx/raised_sheet.particles").read_text())
    shape = next(m for m in baked["modules"] if m["type"] == "shape")
    ed.check(abs(shape.get("position", [0, 0, 0])[1] - 1.0) < 0.05, f"the flipbook is born at the framed centre: {shape}")

    outside = ed.scratch.root / "outside"
    outside.mkdir(exist_ok=True)
    link = ed.project / "assets" / "linked"
    if not link.exists():
        link.symlink_to(outside, target_is_directory=True)
    err = await ed.call("bake_particle_sheet", path="assets/fx/raised.particles", columns=2, rows=1, cell_size=64,
                        output="assets/linked/escape", expect_error=True)
    ed.check_in("output", err)
    ed.check_eq(sorted(p.name for p in outside.iterdir()), [], "nothing written outside the project")


async def test_check_cli_skips_sub_emitter_effects(ed: Editor):
    pop = {"duration": 1, "looping": False, "modules": {"main": {"startLifetime": 0.5, "startSpeed": 1}, "emission": {"rateOverTime": 0}}}
    await make_effect(ed, "assets/fx/pop.particles", pop)
    parent = dict(burst_effect(20, 100))
    parent["modules"] = dict(parent["modules"], subEmitters={})
    parent["subEmitters"] = [{"trigger": "death", "effect": "assets/fx/pop.particles"}]
    await make_effect(ed, "assets/fx/explode.particles", parent)
    cmd = harness.editor_command() + ["check", "--project", str(ed.project), "--particles"]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    output = r.stdout + r.stderr
    ed.check("pop.particles: error" not in output, f"a sub-emitter's effect is not checked alone: {output[-1500:]}")
    warn = subprocess.run(cmd + ["--files", str(ed.project / "assets/fx/visible.particles"), "--fail-on", "warn"],
                          capture_output=True, text=True, timeout=600)
    ed.check_eq(warn.returncode, 1, f"--fail-on warn makes its pop WARN an error: {(warn.stdout + warn.stderr)[-1500:]}")


async def test_focus_window_refuses_path_before_opening_anything(ed: Editor):
    """F14: path belongs to graph_editor; a call that fails for it leaves the Particle Effect window as it was"""
    ed.check(not (await ed.call("editor_status"))["ui"]["particle_effect"]["open"], "the window starts closed")
    err = await ed.call("focus_window", window="particle_effect", path="assets/fx/square.particles", expect_error=True)
    ed.check_in("only for window", err)
    await ed.call("wait", frames=3)
    ed.check(not (await ed.call("editor_status"))["ui"]["particle_effect"]["open"], "the failed call opened nothing")

