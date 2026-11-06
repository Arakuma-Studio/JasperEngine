# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""Calls that arrive while a person's scene save runs (agent-authoring): they wait for the save
and then run, saying so (waited_for_save_ms); read-only calls answer at once; past the hold they
fail with the save named. Red before the change: "This tool is not available while a scene is
playing. Stop Play mode first" with nothing playing, and edits refused as "saving before Play".

JASPER_TEST_SCENE_SAVE_DELAY_MS holds every background scene save open (a large scene's save);
JASPER_TEST_SAVE_HOLD_MS shortens the wait for the timeout test."""

import time
from pathlib import Path

from harness import Editor

FIXTURE = Path(__file__).resolve().parents[3] / "tools" / "fixtures" / "stochastic-tiling"
CI = True
# A project under the temp directory is a temporary project, whose Ctrl+S asks where to save it:
# the scratch copy is under /tmp, so the editor's temp directory is elsewhere
NOT_TEMP = {"TMPDIR": "/var/tmp"}
EDITOR = {"project": str(FIXTURE), "env": {"JASPER_TEST_SCENE_SAVE_DELAY_MS": "2500", **NOT_TEMP}}


async def saved_scene(ed: Editor, name: str) -> int:
    """The fixture's scene, selected and changed, so Ctrl+S saves it in the background"""
    sid = (await ed.call("list_scenes"))["scenes"][0]["id"]
    await ed.call("select_scene", scene=sid)
    await ed.call("create_entity", scene=sid, name=name, type="box")
    return sid


async def test_edits_wait_for_a_save(ed: Editor):
    sid = await saved_scene(ed, "Held")
    await ed.call("run_command", id="file.save")
    started = time.monotonic()
    status = await ed.call("editor_status")
    ed.check(time.monotonic() - started < 2.0, "a read-only call answers at once")
    states = {s["id"]: s.get("play_state") for s in status.get("scenes", [])}
    ed.check_eq(states.get(sid), "saving", f"the scene is saving: {status.get('scenes')}")
    made = await ed.call_raw("create_entity", {"scene": sid, "name": "DuringSave", "type": "box"})
    ed.check(not made.is_error, f"create_entity waited instead of refusing: {made.text[:300]}")
    ed.check(made.structured.get("waited_for_save_ms", 0) > 0, f"the result says it waited: {made.structured}")
    ed.check_in("Waited", made.text, "and the text says so")
    ed.check("Stop Play mode" not in made.text, made.text)
    await ed.call("run_command", id="file.save")
    moved = await ed.call_raw("set_properties", {"scene": sid, "entity": "DuringSave", "values": {"transform": {"position": [1, 2, 3]}}})
    ed.check(not moved.is_error, f"set_properties (allowed while playing) waited too: {moved.text[:300]}")
    ed.check(moved.structured.get("waited_for_save_ms", 0) > 0, f"{moved.structured}")


async def test_the_hold_times_out(ed: Editor):
    async with Editor(str(FIXTURE), name="Save Hold Timeout",
                      env={"JASPER_TEST_SCENE_SAVE_DELAY_MS": "6000", "JASPER_TEST_SAVE_HOLD_MS": "1500", **NOT_TEMP}) as short:
        sid = await saved_scene(short, "Slow")
        await short.call("run_command", id="file.save")
        err = await short.call("create_entity", scene=sid, name="TooLate", type="box", expect_error=True)
        short.check_in("scene save started in the editor is still running", err, "the save is named")
        short.check("Stop Play mode" not in err, err)
