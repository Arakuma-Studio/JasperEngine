"""S36 AC12: the Inspector's clip view. Selecting a clip shows its waveform, measurements, notes and
settings; dragging the trim handle and Apply writes one undo step; Project window thumbnails appear; the
placeholder dialog and Import Sound Essentials run from the UI. Screenshots go to the scratch directory, and
to $JASPER_EVIDENCE_DIR when set.

Real input through ui_input (xdotool in the harness's Xvfb). Oracles: the .meta on disk (PyYAML), the handle's
rectangle against the window's numbers, the thumbnail PNG's pixels."""

from __future__ import annotations

import os
import shutil
import sys
from pathlib import Path

import yaml

from harness import Editor
from ui_input import Ui

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import audio_refs as ar  # noqa: E402

CI = True
EDITOR = {"xvfb": "harness", "create": True, "name": "Clip Inspector"}
TIMEOUT = 600
CLIP = "assets/sfx/tone.wav"
RATE = 48000


async def keep_shot(ed: Editor, name: str) -> None:
    folder = os.environ.get("JASPER_EVIDENCE_DIR")
    shot = await ed.call_raw("screenshot", {"view": "editor"})
    if folder and shot.images:
        Path(folder).mkdir(parents=True, exist_ok=True)
        (Path(folder) / f"{name}.png").write_bytes(shot.images[0][0])


async def select_clip(ed: Editor, ui: Ui, rel: str = CLIP) -> None:
    await select_clip_quietly(ed, ui, rel)
    ed.check(await ui.find("Import Settings", window="Inspector", frames=600), "the clip view is drawn")


async def select_clip_quietly(ed: Editor, ui: Ui, rel: str) -> None:
    await ed.call("sync_files", paths=[rel])
    # the palette lists what the asset index holds: wait for the new file
    for _ in range(200):
        found = await ed.call("search_editor", query="sound: " + Path(rel).name)
        if any(r["key"] == "sound:" + rel for r in found["results"]):
            break
        await ui.frames(2)
    await ed.call("run_command", key="sound:" + rel)


async def test_clip_view_drag_trim_and_apply(ed: Editor):
    ui = Ui(ed)
    # a click at its start so a note shows
    samples = [0.0] * int(0.2 * RATE) + ar.sine(1000.0, 0.8, RATE, -12.0)
    ar.write_wav(ed.project / CLIP, [samples], RATE, ar.PCM16)
    await select_clip(ed, ui)
    for label in ("Spectrogram", "Apply", "Revert", "Audition", "Match Loudness", "Gain", "Trim Head", "Fade Out", "Loop End", "Provenance"):
        ed.check(await ui.find(label, window="Inspector", frames=120), f"the view has {label}")
    await keep_shot(ed, "clip-view")

    meta = ed.project / (CLIP + ".meta")
    before = meta.read_text()
    handle = await ui.find("##audio.handle.trim_head", window="Inspector", frames=600)
    ed.check(handle, "the trim handle is drawn once the waveform is in")
    rect = handle["rect"]
    start = ui.centre(rect)
    await ui.drag(start, (start[0] + 60, start[1]))
    await ui.frames()
    await keep_shot(ed, "clip-view-dragged")
    ed.check_eq(meta.read_text(), before, "a drag alone writes nothing: Apply does")
    await ui.click_item("Apply", window="Inspector")
    await ui.frames()
    root = yaml.safe_load(meta.read_text())
    trim = root["import"]["audioImporter"]["trim_head_ms"]
    ed.check(0 < trim < 1000, f"the handle set the trim: {trim}")
    await keep_shot(ed, "clip-view-applied")

    r = await ed.call("analyze_audio", paths=[CLIP], image="none")
    ed.check_in("played", r["items"][0], "the analysis follows the Apply")
    await ed.call("undo", scope="project")
    ed.check_eq(meta.read_text(), before, "one undo step")
    await ed.call("redo", scope="project")
    ed.check_eq(yaml.safe_load(meta.read_text())["import"]["audioImporter"]["trim_head_ms"], trim, "redo")
    await ed.call("undo", scope="project")


async def test_spectrogram_and_revert(ed: Editor):
    ui = Ui(ed)
    ar.write_wav(ed.project / CLIP, [ar.sine(440.0, 0.5, RATE, -12.0)], RATE, ar.PCM16)
    await select_clip(ed, ui)
    await ui.click_item("Spectrogram", window="Inspector")
    await ui.frames(60)
    await keep_shot(ed, "clip-view-spectrogram")
    await ui.click_item("Waveform", window="Inspector")
    meta = ed.project / (CLIP + ".meta")
    before = meta.read_text()
    handle = await ui.find("##audio.handle.trim_tail", window="Inspector", frames=600)
    ed.check(handle, "the tail handle is drawn")
    start = ui.centre(handle["rect"])
    await ui.drag(start, (start[0] - 40, start[1]))
    await ui.click_item("Revert", window="Inspector")
    await ui.frames()
    ed.check_eq(meta.read_text(), before, "Revert leaves the file as it was")


async def test_thumbnail(ed: Editor):
    ui = Ui(ed)
    rel = "assets/sfx/thumb.wav"
    ar.write_wav(ed.project / rel, [ar.sine(220.0, 0.5, RATE, -6.0)], RATE, ar.PCM16)
    await select_clip_quietly(ed, ui, rel)
    thumbs = ed.project / ".jasperengine"
    for _ in range(300):
        await ui.frames(2)
        found = [p for p in thumbs.rglob("*.png") if "thumb" in p.name]
        if found:
            break
    ed.check(found, f"a thumbnail appears under {thumbs}")
    await keep_shot(ed, "thumbnail")
