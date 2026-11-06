"""Timeline audio in the Animation window, driven like a human (docs/specs/S40-timeline-audio.md R9-R11,
R22-R23; AC5, AC11's UI parts) with real X input in the harness's Xvfb: the clip mode's Audio lane draws a
cue as a block with its waveform, a Sound Event dragged from the Sound Browser onto the lane becomes a cue at
the drop time in one undo step, Align Peak to Playhead (Shift+A) puts the tick's analysed peak on the
playhead, and the playing preview starts preview voices (none while Play captures audio); in the entity
mode, Add Property > Sound > Audio Clip adds an audio action and its frame in one undo step, a sound dragged
onto the tracks adds one at the drop time, and Shift+A aligns the selected audio block's peak. The window's
own logic (the cue edits, the alignment math, preview voices) is AnimationWindowClipTest's AudioLane* and
EntityModeAudioActions. Screenshots go to build/s40-audio-editor-shots (fetched as evidence).
"""

import asyncio
import json
import shutil

import harness
from audio_refs import write_wav
from harness import Editor
from ui_input import Ui

EDITOR = {"xvfb": "harness", "create": True, "name": "Timeline Audio", "ui_probe": True}
CI = True
TIMEOUT = 900

CLIP = "anim/walk.anim"
TICK = "sounds/tick.wav"
STEP = "sounds/step.wav"
EVENT = "events/steps.soundevent"
STATE = {}


def burst(peak: float, seconds: float, rate: int = 48000) -> list:
    """A decaying click whose loudest sample is at `peak` seconds."""
    out = [0.0] * int(round(seconds * rate))
    start = int(round(peak * rate))
    for i in range(480):
        if start + i < len(out):
            out[start + i] = (-0.9 if i % 2 else 0.9) * (1.0 - i / 480.0)
    return out


def middle(ui: Ui, rect: list) -> tuple[int, int]:
    return ui.at(rect, ((rect[2] - rect[0]) / 2, (rect[3] - rect[1]) / 2))


async def keep_shot(ed: Editor, name: str) -> None:
    folder = harness.REPO / "build" / "s40-audio-editor-shots"
    folder.mkdir(parents=True, exist_ok=True)
    await ed.screenshot(view="editor", path=folder / f"{name}.png", max_width=0)


async def rect_of(ed: Editor, name: str, seconds: float = 40.0):
    deadline = asyncio.get_running_loop().time() + seconds
    while asyncio.get_running_loop().time() < deadline:
        rect = ed.ui_items().get(name)
        if rect:
            return rect
        await ed.call("wait", frames=3)
    await keep_shot(ed, "not-drawn-" + name.replace(".", "-"))
    ed.fail(f"{name} is not drawn: {sorted(ed.ui_items())[:80]}")


def cues(ed: Editor) -> list:
    return json.loads((ed.project / CLIP).read_text()).get("audio", [])


async def frames_of(ed: Editor, entity: str) -> list:
    got = await ed.call("get_animation", scene=STATE["scene"], entity=entity)
    return got.get("actions", [])


async def test_setup(ed: Editor):
    if not shutil.which("xdotool"):
        ed.fail("xdotool is needed for this real-input UI test (the linux image has it)")
    write_wav(ed.project / TICK, [burst(0.2, 0.5)], 48000)
    write_wav(ed.project / STEP, [burst(0.05, 0.3)], 48000)
    await ed.call("sync_files")
    await ed.call("write_file", path=EVENT, content=json.dumps({
        "format": "jasperengine.soundevent", "version": 1, "mode": "random",
        "clips": [{"clip": {"path": TICK}}, {"clip": {"path": STEP}}]}, indent=2))
    clip = {"format": "jasperengine.anim", "version": 3, "frame_rate": 30, "wrap": "loop", "length": 2,
            "tracks": [{"path": "", "channel": "position", "keys": [[0, [0, 0, 0]], [2, [0, 1, 0]]]}], "events": [],
            "audio": [{"time": 0.1, "sound": {"path": TICK}, "name": "Tick"}]}
    await ed.call("write_file", path=CLIP, content=json.dumps(clip, indent=2) + "\n")
    await ed.call("sync_files")
    made = await ed.call("create_scene", name="Walk", type="3D", path="scenes/Walk.scene")
    STATE["scene"] = made["scene"]["id"]
    await ed.call("create_entity", scene=STATE["scene"], name="Walker", type="cube")
    await ed.call("add_component", scene=STATE["scene"], entity="Walker", component="animationplayer",
                  values={"animationplayer": {"clips": [{"clip": CLIP}]}})
    await ed.call("create_entity", scene=STATE["scene"], name="Sequencer", type="animation")
    await ed.call("save_scene", scene=STATE["scene"])
    await ed.call("run_command", id="window.soundBrowser")
    await ed.call("select_entities", scene=STATE["scene"], entities=["Walker"])
    await ed.call("focus_window", window="animation")


async def test_the_lane_draws_the_cue_as_a_waveform_block(ed: Editor):
    block = await rect_of(ed, "clip.audio.cue.0")
    lane = await rect_of(ed, "clip.audio.lane")
    ed.check(block[1] >= lane[1] and block[3] <= lane[3], f"the block lies in the lane: {block} in {lane}")
    # the block is as long as the tick (0.5 s), once S36's analysis is in: wider than a marker
    for _ in range(60):
        block = ed.ui_items().get("clip.audio.cue.0", block)
        if block[2] - block[0] > 30:
            break
        await ed.call("wait", frames=5)
    ed.check(block[2] - block[0] > 30, f"a block as long as its sound: {block}")
    await keep_shot(ed, "clip-lane-waveform")


async def test_a_sound_event_dropped_on_the_lane_is_a_cue_in_one_undo_step(ed: Editor):
    ui = Ui(ed)
    ui.focus()
    await ui.click(middle(ui, await rect_of(ed, "soundBrowser.source.Project")))
    start = middle(ui, await rect_of(ed, "soundBrowser.card.steps"))
    lane = await rect_of(ed, "clip.audio.lane")
    target = (int((lane[0] + lane[2]) / 2), middle(ui, lane)[1])
    await ui.press(start)
    for i in range(1, 13):
        await ui.move((start[0] + (target[0] - start[0]) * i // 12, start[1] + (target[1] - start[1]) * i // 12), frames=2)
    await ui.release()
    await ui.frames(10)
    got = cues(ed)
    ed.check_eq(len(got), 2, f"the drop added a cue: {got}")
    dropped = [c for c in got if c["sound"]["path"] == EVENT]
    ed.check(dropped and dropped[0]["time"] > 0.1, f"a cue of the event at the drop time: {got}")
    await keep_shot(ed, "clip-lane-dropped-event")
    await ed.call("undo", scope="project")
    ed.check_eq(len(cues(ed)), 1, "one undo step takes the drop back")


async def test_align_peak_puts_the_analysed_peak_on_the_playhead(ed: Editor):
    ui = Ui(ed)
    ui.focus()
    await ed.call("preview_animation", scene=STATE["scene"], animation="Walker", action="seek", time=1.0)
    await ed.call("preview_animation", scene=STATE["scene"], animation="Walker", action="stop")
    await ed.call("focus_window", window="animation")
    ui.focus()
    block = await rect_of(ed, "clip.audio.cue.0")
    await ui.click(middle(ui, block))
    await ui.key("Escape")   # the cue's popover opened on the click
    await ui.frames(4)
    await ui.key("shift+a")
    await ui.frames(10)
    got = cues(ed)
    ed.check(abs(got[0]["time"] - 0.8) < 0.003, f"the tick's peak (0.2 s) on 1.0 s: {got}")
    await keep_shot(ed, "clip-lane-aligned-peak")
    await ed.call("undo", scope="project")
    ed.check(abs(cues(ed)[0]["time"] - 0.1) < 1e-4, f"one undo step: {cues(ed)}")


async def test_the_playing_preview_starts_preview_voices_but_not_in_a_capture(ed: Editor):
    played = await ed.call("preview_animation", scene=STATE["scene"], animation="Walker", action="play", time=0.0)
    for _ in range(40):
        await ed.call("wait", frames=10)
        played = await ed.call("preview_animation", scene=STATE["scene"], animation="Walker", action="pause")
        if played.get("audio_preview"):
            break
        await ed.call("preview_animation", scene=STATE["scene"], animation="Walker", action="play")
    ed.check(played.get("audio_preview", {}).get("started", 0) >= 1, f"the cue played as a preview voice: {played}")
    started = played["audio_preview"]["started"]
    await ed.call("preview_animation", scene=STATE["scene"], animation="Walker", action="stop")
    # a Play here captures (no device): the preview is off while it does (its mix is the game's)
    await ed.call("play", action="start", scene=STATE["scene"], fixed_fps=60)
    try:
        await ed.call("wait", frames=10)
        state = await ed.call("audio_session", action="status")
        ed.check(state.get("capturing"), f"a capture runs: {state}")
    finally:
        await ed.call("play", action="stop")
    played = await ed.call("preview_animation", scene=STATE["scene"], animation="Walker", action="stop")
    ed.check(played.get("audio_preview", {"started": started})["started"] == started, f"no preview voice since: {played}")


async def test_entity_mode_add_property_sound_adds_an_audio_action_in_one_undo_step(ed: Editor):
    ui = Ui(ed)
    await ed.call("select_entities", scene=STATE["scene"], entities=["Sequencer"])
    await ed.call("focus_window", window="animation")
    ui.focus()
    before = len(await frames_of(ed, "Sequencer"))
    await ui.click_item("Add Property", "Animation")
    await ui.frames(4)
    await ui.click_item("Sound")
    await ui.frames(4)
    await ui.click_item("Audio Clip")
    await ui.frames(4)
    await ui.click_item(TICK)
    await ui.frames(10)
    actions = await frames_of(ed, "Sequencer")
    if len(actions) != before + 1:
        await keep_shot(ed, "entity-audio-action-missing")
        seen = {label: (await ed.call("find_ui_item", label=label)).get("items", [])[:2] for label in ("Add Property", "Sound", "Audio Clip", TICK)}
        ed.fail(f"an audio action's frame: {actions}; items: {json.dumps(seen)[:1500]}")
    await keep_shot(ed, "entity-audio-action")
    await ed.call("undo", scene=STATE["scene"])
    ed.check_eq(len(await frames_of(ed, "Sequencer")), before, "the action and its frame are one undo step")
    await ed.call("redo", scene=STATE["scene"])


async def test_entity_mode_drop_and_align_peak_on_an_audio_block(ed: Editor):
    ui = Ui(ed)
    ui.focus()
    await ui.key("Escape")   # no menu left open over the tracks
    await ui.frames(4)
    before = await frames_of(ed, "Sequencer")
    start = middle(ui, await rect_of(ed, "soundBrowser.card.tick"))
    tracks = await rect_of(ed, "entity.tracks")
    target = (int(tracks[0] + (tracks[2] - tracks[0]) * 0.4), int(tracks[1] + 12))
    await ui.press(start)
    for i in range(1, 13):
        await ui.move((start[0] + (target[0] - start[0]) * i // 12, start[1] + (target[1] - start[1]) * i // 12), frames=2)
    await ui.release()
    await ui.frames(10)
    after = await frames_of(ed, "Sequencer")
    ed.check_eq(len(after), len(before) + 1, f"the drop added an audio action: {after}")
    ed.check(after[-1]["start_time"] > 0.0, f"at the drop time: {after[-1]}")
    await keep_shot(ed, "entity-audio-dropped")
    # the playhead at 1.0 s by a click on the ruler (the drop gave the track area's seconds per pixel),
    # then the first audio block and Shift+A
    pixels_per_second = (target[0] - tracks[0]) / after[-1]["start_time"]
    await ui.click((int(tracks[0] + pixels_per_second * 1.0), int(tracks[1] - 8)))
    await ui.frames(4)
    index = len(before) - 1 if before else 0
    await ui.click(middle(ui, await rect_of(ed, f"entity.frame.{index}")))
    await ui.frames(4)
    await ui.key("shift+a")
    await ui.frames(10)
    aligned = await frames_of(ed, "Sequencer")
    await keep_shot(ed, "entity-audio-aligned")
    # (the ruler click snaps to the frame grid: within a frame of 1.0 s)
    ed.check(abs(aligned[index]["start_time"] - 0.8) < 0.035,
             f"the tick's peak on the playhead at 1.0 s: {aligned[index]}; all: {[(a['index'], a['start_time']) for a in aligned]}; "
             f"tracks {tracks} drop {target} -> {after[-1]['start_time']}")
