"""S07 AC10: a Lua contact callback destroys the other body (GameObject.destroyImmediate) and adds
or removes a component on its own GameObject, for 1000 frames, without an error.

tools/fixtures/ecs-collision: boxes fall on a floor every third frame; each beginContact2D destroys
the box and toggles a Rigidbody2D on the script's GameObject. The destroys are deferred to the end
of the step's contact callbacks (StructuralDeferScope). Run it with an ASan/UBSan editor
(SANITIZE=address,undefined tools/build.sh linux, JASPER_EDITOR=<its jasperengine-editor>,
ASAN_OPTIONS=detect_leaks=0:alloc_dealloc_mismatch=0): the editor's stderr must hold no report.
"""

from pathlib import Path

from harness import Editor

TIMEOUT = 1800

EDITOR = {"project": str(Path(__file__).resolve().parents[2] / "fixtures" / "ecs-collision")}

SANITIZER_REPORTS = ("ERROR: AddressSanitizer", "runtime error:", "ERROR: LeakSanitizer", "SUMMARY: UndefinedBehaviorSanitizer")


async def test_contact_callbacks_destroy_for_1000_frames(ed: Editor):
    ed.require_tools("play", "wait", "eval_lua", "read_console")
    cursor = (await ed.call("read_console", limit=1))["next_cursor"]
    started = await ed.call("play", action="start")
    ed.check_eq(started["state"], "playing", f"play start: {started}")
    try:
        done = await ed.call_raw("wait", {"until": "script('Spawner').frames >= 1000", "timeout_seconds": 900})
        ed.check(not done.is_error and done.structured.get("reason") == "condition", f"1000 frames: {done.text[:500]}")
        values = (await ed.call("eval_lua", code="local s = script('Spawner') return s.frames, s.hits, s.spawned, s.toggles"))["values"]
        frames, hits, spawned, toggles = values
        print(f"[ecs-collision] {frames} frames, {hits} contacts handled, {spawned} boxes spawned, {toggles} Rigidbody2D toggles", flush=True)
        ed.check(frames >= 1000, f"frames {frames}")
        ed.check(hits >= 100 and toggles == hits, f"contacts handled: {hits} hits, {toggles} toggles, {spawned} spawned")
        errors = await ed.call("read_console", since=cursor, types=["error"], limit=50)
        ed.check(not errors["entries"], f"errors logged: {[e['message'] for e in errors['entries']]}")
    finally:
        await ed.call("play", action="stop")
    stderr = ed.stderr_text()
    reports = [line for line in stderr.splitlines() if any(marker in line for marker in SANITIZER_REPORTS)]
    ed.check(not reports, "sanitizer reports:\n" + "\n".join(reports[:20]))
