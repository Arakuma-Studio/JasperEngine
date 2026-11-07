"""The Particle Effect window driven like a human (docs/specs/S91-effect-authoring.md AC14): xdotool in the harness's
Xvfb, the widgets found through the UI probe (Editor(ui_probe=True), editor/util/UIProbe.h), the window's clock and
eye read from editor_status (ui.particle_effect).

- a .particles opened from the Project window (Assets > Open, what a double-click runs) shows the window, its file
  and its sub-emitter child in the Outline;
- the eye hides a node in the Preview Stage only: the files and the undo history stay as they were;
- an edit made while paused (a drag on Duration in the window's module rows) re-simulates the stage to the same
  time with the same seed, and is one undo step;
- Esc during a drag puts the file back and adds no undo step.

Screenshots go to $JASPER_SHOTS_DIR when set (else the scratch directory).
"""

from __future__ import annotations

import json
import os
from pathlib import Path

from harness import Editor
from ui_input import Ui

CI = True
EDITOR = {"create": True, "name": "Particle Effect Window", "xvfb": "harness", "ui_probe": True}
TIMEOUT = 420

FIRE = "assets/fx/Fire.particles"
SPARK = "assets/fx/Spark.particles"

FIRE_JSON = {
    "version": 1,
    "duration": 2,
    "maxParticles": 300,
    "modules": [
        {"type": "main", "startLifetime": {"mode": "twoConstants", "constant": 0.4, "constantMax": 0.8}, "startSpeed": 3},
        {"type": "emission", "rateOverTime": 40},
        {"type": "subEmitters"},
    ],
    "subEmitters": [{"trigger": "death", "effect": "fx/Spark.particles", "emitCount": 2}],
}
SPARK_JSON = {
    "version": 1,
    "looping": False,
    "duration": 0.5,
    "maxParticles": 600,
    "modules": [{"type": "main", "startLifetime": 0.3, "startSpeed": 2}, {"type": "emission", "rateOverTime": 0}],
}


def centre(ui: Ui, rect: list[float]) -> tuple[int, int]:
    return ui.at(rect, ((rect[2] - rect[0]) / 2, (rect[3] - rect[1]) / 2))


async def shot(ed: Editor, name: str) -> None:
    shots = Path(os.environ["JASPER_SHOTS_DIR"]) if os.environ.get("JASPER_SHOTS_DIR") else ed.scratch.root
    shots.mkdir(parents=True, exist_ok=True)
    await ed.screenshot("editor", path=str(shots / f"particle-effect-window-{name}.png"), max_width=1600)


async def probe_item(ui: Ui, name: str) -> list[float]:
    for _ in range(40):
        await ui.frames(2)
        rect = ui.ed.ui_items().get(name)
        if rect:
            return rect
    ui.ed.fail(f"{name} is never drawn: {sorted(ui.ed.ui_items())}")


async def window_state(ed: Editor) -> dict:
    return (await ed.call("editor_status"))["ui"]["particle_effect"]


async def settled(ui: Ui) -> dict:
    """The window's state once its stage is at its time (a re-simulation runs over frames)"""
    for _ in range(120):
        state = await window_state(ui.ed)
        if not state["resimulating"]:
            return state
        await ui.frames(4)
    ui.ed.fail(f"the stage never finished re-simulating: {state}")


async def undo_steps(ed: Editor) -> int:
    return len((await ed.call("get_undo_history", scope="project", limit=500))["entries"])


def file_bytes(ed: Editor) -> tuple[bytes, bytes]:
    return (ed.project / FIRE).read_bytes(), (ed.project / SPARK).read_bytes()


async def open_fire(ed: Editor, ui: Ui) -> None:
    """Fire.particles selected in the Project window, then Assets > Open (what a double-click runs)"""
    await ed.call("focus_window", window="project")
    for _ in range(12):
        await ui.frames(4)
        items = ed.ui_items()
        if "project.file.Fire.particles" in items:
            await ui.click(centre(ui, items["project.file.Fire.particles"]))
            break
        for folder in ("fx", "assets"):
            if "project.file." + folder in items:
                await ui.double_click(centre(ui, items["project.file." + folder]))
                break
    else:
        ed.fail(f"the Project window never lists Fire.particles: {sorted(ed.ui_items())}")
    # the stage's frames on this software renderer are longer than ImGui's double-click time
    await ed.call("run_command", id="assets.open")
    await ui.frames(6)


async def test_setup(ed: Editor):
    await ed.call("create_scene", name="Effects", type="3D", path="scenes/Effects")
    (ed.project / "assets" / "fx").mkdir(parents=True, exist_ok=True)
    (ed.project / SPARK).write_text(json.dumps(SPARK_JSON, indent=2) + "\n")
    (ed.project / FIRE).write_text(json.dumps(FIRE_JSON, indent=2) + "\n")
    await ed.call("sync_files")
    ui = Ui(ed)
    await open_fire(ed, ui)
    focused = (await ed.call("editor_status"))["windows"]["focused"]
    ed.check_eq(focused, "Particle Effect", "Assets > Open on a .particles shows the Particle Effect window")
    state = await settled(ui)
    ed.check_eq(state["effect"], "fx/Fire.particles", "the window shows the file")
    ed.check_eq(state["nodes"], ["Fire", "Spark"], "the Outline: the file and its sub-emitter child")
    await probe_item(ui, "particle_effect.stage")
    await shot(ed, "01-opened")


async def test_the_eye_never_changes_the_file(ed: Editor):
    ui = Ui(ed)
    await ui.release_all()
    before = file_bytes(ed)
    steps = await undo_steps(ed)
    for node in ("particle_effect.eye.1", "particle_effect.eye.0"):
        await ui.click(centre(ui, await probe_item(ui, node)))
    state = await settled(ui)
    ed.check_eq(sorted(state["hidden"]), ["Fire", "Spark"], "both nodes are hidden in the preview")
    await shot(ed, "02-hidden")
    ed.check_eq(file_bytes(ed), before, "the eye writes no file")
    ed.check_eq(await undo_steps(ed), steps, "the eye is no undo step")
    await ed.call("sync_files")
    ed.check_eq(file_bytes(ed), before, "and nothing waits to be written")
    for node in ("particle_effect.eye.0", "particle_effect.eye.1"):
        await ui.click(centre(ui, await probe_item(ui, node)))
    state = await settled(ui)
    ed.check_eq(state["hidden"], [], "shown again")
    ed.check_eq(file_bytes(ed), before, "still the same files")


async def pause(ed: Editor, ui: Ui) -> dict:
    state = await settled(ui)
    if state["playing"]:
        await ui.click(centre(ui, await probe_item(ui, "particle_effect.play")))
    state = await settled(ui)
    ed.check(not state["playing"], f"the window is paused: {state}")
    return state


async def test_a_paused_edit_holds_the_time_and_the_seed(ed: Editor):
    ui = Ui(ed)
    await ui.release_all()
    await ui.frames(20)
    before = await pause(ed, ui)
    ed.check(before["time"] > 0.0, f"the clock ran before the pause: {before}")
    steps = await undo_steps(ed)
    text = (ed.project / FIRE).read_text()

    duration = await probe_item(ui, "particle_effect.effect.duration")
    grab = centre(ui, duration)
    await ui.drag(grab, (grab[0] + 40, grab[1]))
    await ui.frames(4)
    after = await settled(ui)
    await shot(ed, "03-edited-paused")
    ed.check(json.loads((ed.project / FIRE).read_text())["duration"] != json.loads(text)["duration"], "the drag changed Duration in the file")
    ed.check_eq(after["time"], before["time"], "the edit holds the time")
    ed.check_eq(after["seed"], before["seed"], "the edit holds the seed")
    ed.check(not after["playing"], "and the window stays paused")
    ed.check_eq(await undo_steps(ed), steps + 1, "the drag is one undo step")
    await ed.call("undo", scope="project")
    await ui.frames(4)
    ed.check_eq((ed.project / FIRE).read_text(), text, "undo puts the file back")
    undone = await settled(ui)
    ed.check_eq(undone["time"], before["time"], "an undo is an edit too: the same time")
    ed.check_eq(undone["seed"], before["seed"], "and the same seed")


async def test_esc_cancels_a_drag_with_no_undo_step(ed: Editor):
    ui = Ui(ed)
    await ui.release_all()
    before = await pause(ed, ui)
    steps = await undo_steps(ed)
    text = (ed.project / FIRE).read_text()
    duration = await probe_item(ui, "particle_effect.effect.duration")
    grab = centre(ui, duration)
    ui.focus()
    await ui.drag(grab, (grab[0] + 60, grab[1]), release=False)
    await shot(ed, "04-dragging")
    await ui.key("Escape")
    await ui.frames(2)
    await ui.release()
    await ui.frames(6)
    after = await settled(ui)
    await shot(ed, "05-esc")
    ed.check_eq((ed.project / FIRE).read_text(), text, "Esc puts the file back")
    ed.check_eq(await undo_steps(ed), steps, "and adds no undo step")
    ed.check_eq((after["time"], after["seed"]), (before["time"], before["seed"]), "the clock and the seed are as before")
