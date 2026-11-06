"""S39 AC8: the music parts of the editor. The clip view's Detect draws the beat grid over the waveform and
writes nothing; Use writes the detected block as one undo step, "Set Tempo from Detection". Screenshots go to
$JASPER_EVIDENCE_DIR when set.

Real input through ui_input (xdotool in the harness's Xvfb). Oracles: the .meta on disk (PyYAML), the click
track's parameters, the grid's colour inside the waveform in the editor's screenshot."""

from __future__ import annotations

import json
import os
import sys
from pathlib import Path

import yaml

import harness
from harness import Editor
from htmlui_pixels import decode_png
from ui_input import Ui

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import audio_refs as ar  # noqa: E402

CI = True
EDITOR = {"xvfb": "harness", "create": True, "name": "Music Editor"}
TIMEOUT = 600
RATE = 48000
# InspectorAudioClip.cpp's beat grid
GRID_RGB = (70, 190, 255)


async def editor_shot(ed: Editor, name: str) -> bytes:
    # unscaled, so window coordinates are pixels and a 1 px tick keeps its colour
    shot = await ed.call_raw("screenshot", {"view": "editor", "max_width": 8192})
    ed.check(shot.images, "the editor screenshot")
    png = shot.images[0][0]
    folder = os.environ.get("JASPER_EVIDENCE_DIR")
    if folder:
        Path(folder).mkdir(parents=True, exist_ok=True)
        (Path(folder) / f"{name}.png").write_bytes(png)
    return png


def grid_pixels(png: bytes, band: dict) -> int:
    """pixels of the grid's blue inside the waveform's rows, from its left edge rightwards"""
    left, top, tall = (band["x"], band["y"], band["height"]) if isinstance(band, dict) else (band[0], band[1], band[3])
    width, height, channels, rows = decode_png(png)
    count = 0
    for y in range(max(0, int(top)), min(height, int(top + tall))):
        row = rows[y]
        for x in range(max(0, int(left)), width):
            r, g, b = row[x * channels], row[x * channels + 1], row[x * channels + 2]
            if abs(r - GRID_RGB[0]) <= 24 and abs(g - GRID_RGB[1]) <= 24 and abs(b - GRID_RGB[2]) <= 24:
                count += 1
    return count


def music_block(ed: Editor, path: str):
    root = yaml.safe_load((ed.project / (path + ".meta")).read_text()) or {}
    return ((root.get("import") or {}).get("audioImporter") or {}).get("music")


async def select_clip(ed: Editor, ui: Ui, rel: str) -> None:
    await ed.call("sync_files", paths=[rel])
    # the palette lists what the asset index holds: wait for the new file
    for _ in range(200):
        found = await ed.call("search_editor", query="sound: " + Path(rel).name)
        if any(r["key"] == "sound:" + rel for r in found["results"]):
            break
        await ui.frames(2)
    await ed.call("run_command", key="sound:" + rel)
    ed.check(await ui.find("Import Settings", window="Inspector", frames=600), "the clip view is drawn")


async def test_clip_view_detect_and_use(ed: Editor):
    ui = Ui(ed)
    rel = "assets/music/detect.wav"
    ar.write_wav(ed.project / rel, [ar.click_track(120.0, 0.25, 4, 8.0, RATE)], RATE, ar.PCM16)
    await ed.call("sync_files", paths=[rel])
    await ed.call("set_audio_import_settings", paths=[rel], settings={"type": "music"})
    await select_clip(ed, ui, rel)
    for label in ("BPM", "Beats per Bar", "First Beat", "Detect"):
        ed.check(await ui.find(label, window="Inspector", frames=120), f"the view has {label}")
    handle = await ui.find("##audio.handle.trim_head", window="Inspector", frames=600)
    ed.check(handle, "the waveform is in")
    band = handle["rect"]
    await ui.frames()
    before_shot = await editor_shot(ed, "clip-view-before-detect")
    meta = ed.project / (rel + ".meta")
    before = meta.read_text()

    await ui.click_item("Detect", window="Inspector")
    ed.check(await ui.find("Use", window="Inspector", frames=900), "Detect finishes and offers Use")
    await ui.frames()
    detected_shot = await editor_shot(ed, "clip-view-detected")
    ed.check_eq(meta.read_text(), before, "Detect writes nothing")
    before_grid, detected_grid = grid_pixels(before_shot, band), grid_pixels(detected_shot, band)
    # 4 bars of 2 px over the waveform's height alone are hundreds of pixels
    ed.check(before_grid < 20 and detected_grid >= 300, f"the beat grid is drawn over the waveform ({before_grid} -> {detected_grid} pixels)")

    await ui.click_item("Use", window="Inspector")
    await ui.frames()
    music = music_block(ed, rel)
    ed.check(music is not None, "Use wrote the block")
    ed.check(abs(music["bpm"] - 120.0) <= 0.1 and music["beats_per_bar"] == 4 and abs(music["first_beat_ms"] - 250.0) <= 5.0,
             f"the detected grid: {music}")
    analysed = (await ed.call("analyze_audio", paths=[rel], image="none"))["items"][0]["music"]
    ed.check_eq(music, analysed["fix"]["music"], "Use writes what analyze_audio's fix says")
    top = (await ed.call("get_undo_history", scope="project", limit=1))["entries"][0]
    ed.check(top["name"].startswith("Set Tempo from Detection"), f"the step's name: {top['name']}")
    ed.check_eq(top["type"], "SetAssetMetaCmd", "one SetAssetMetaCmd")
    await editor_shot(ed, "clip-view-used")

    await ed.call("undo", scope="project")
    ed.check_eq(meta.read_text(), before, "one undo step restores the .meta")
    # the command runs on the inspected clip (run_command refuses an unknown or disabled one)
    await ed.call("run_command", id="audioClip.detectTempo")


def middle(ui: Ui, rect) -> tuple[int, int]:
    return ui.at(rect, ((rect[2] - rect[0]) / 2, (rect[3] - rect[1]) / 2))


async def probe(ed: Editor, ui: Ui, name: str, frames: int = 600):
    for _ in range(frames // 3):
        rect = ed.ui_items().get(name)
        if rect:
            return rect
        await ed.call("wait", frames=3)
    await ed.screenshot(view="editor", path=harness.REPO / "build" / "s39-shots" / f"not-drawn-{name}.png", max_width=0)
    ed.fail(f"{name} is not drawn: {sorted(k for k in ed.ui_items() if k.startswith('soundEvent'))}")


async def edit_mode_music(ed: Editor) -> str:
    result = await ed.call("eval_lua", code="return Audio.getMusic()", allow_edit_mode=True)
    return result["values"][0]


async def test_event_view_loops_and_play_as_music(ed: Editor):
    ui = Ui(ed)
    for i in range(2):
        ar.write_tone(ed.project / f"music/part{i}.wav", seconds=1.0, frequency=440.0 + 110.0 * i, rate=RATE)
    await ed.call("sync_files")
    await ed.call("tool_groups", enable=["audio"])
    event = "music/theme.soundevent"
    await ed.call("edit_sound_event", path=event, create=True, values={
        "mode": "sequence", "type": "music", "loop": True,
        "clips": [{"clip": "music/part0.wav", "loops": 2}, "music/part1.wav"]})
    ui.focus()
    # open it in the Inspector through the palette, as a person types its name
    key = None
    for _ in range(200):
        found = await ed.call("search_editor", query="asset: theme")
        key = next((r["key"] for r in found["results"] if r["key"].startswith("asset:") and "theme.soundevent" in json.dumps(r)), None)
        if key:
            break
        await ui.frames(2)
    ed.check(key, "the event is in the asset index")
    # the palette reveals it in the Project window; a click there inspects it
    await ed.call("run_command", key=key)
    tile = None
    for _ in range(100):
        items = ed.ui_items()
        tile = next((items[k] for k in ("project.file.theme.soundevent", "project.file.theme") if k in items), None)
        if tile:
            break
        await ed.call("wait", frames=3)
    ed.check(tile, f"the Project window shows the event: {sorted(k for k in ed.ui_items() if k.startswith('project.file'))}")
    await ui.click(middle(ui, tile))
    await probe(ed, ui, "soundEvent.audition")
    # the Loops column of a music event, one per clip
    await probe(ed, ui, "soundEvent.loops.0")
    await probe(ed, ui, "soundEvent.loops.1")
    await editor_shot(ed, "event-view-loops")

    await ui.click(middle(ui, await probe(ed, ui, "soundEvent.playAsMusic")))
    playing = ""
    for _ in range(60):
        playing = await edit_mode_music(ed)
        if playing:
            break
        await ui.frames(3)
    ed.check_eq(playing, "music/theme.soundevent", "Play as Music plays the event through the music system")
    await ui.frames(30)
    await editor_shot(ed, "event-view-playing-as-music")
    await ui.click(middle(ui, await probe(ed, ui, "soundEvent.playAsMusic")))
    await ui.frames(3)
    ed.check_eq(await edit_mode_music(ed), "", "the button stops it")
    # the command too, on the inspected event
    await ed.call("run_command", id="soundEvent.playAsMusic")
    await ui.frames(3)
    ed.check_eq(await edit_mode_music(ed), "music/theme.soundevent", "soundEvent.playAsMusic")
    await ed.call("run_command", id="soundEvent.playAsMusic")
