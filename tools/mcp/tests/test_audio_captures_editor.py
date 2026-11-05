"""Window > Audio > Captures and the Game view's banner, driven like a human (docs/specs/S35-agent-audio.md R30, R31; AC15):
it lists the sessions, shows the lanes and the listen banner, Enter plays a moment (a preview voice; the session list does
not change), Escape stops. Real X input in the harness's Xvfb; screenshots in build/s35-captures-shots.
"""

import asyncio
import shutil

import harness
from audio_refs import capture_sessions, write_tone
from harness import Editor
from ui_input import Ui

EDITOR = {"xvfb": "harness", "create": True, "name": "Captures", "ui_probe": True}
CI = True
TIMEOUT = 900

STATE = {}


def middle(ui: Ui, rect: list) -> tuple[int, int]:
    return ui.at(rect, ((rect[2] - rect[0]) / 2, (rect[3] - rect[1]) / 2))


async def rect_of(ed: Editor, name: str, seconds: float = 60.0):
    deadline = asyncio.get_running_loop().time() + seconds
    while asyncio.get_running_loop().time() < deadline:
        rect = ed.ui_items().get(name)
        if rect:
            return rect
        await ed.call("wait", frames=3)
    await keep_shot(ed, "not-drawn-" + name.replace(".", "-"))
    ed.fail(f"{name} is not drawn: {sorted(k for k in ed.ui_items() if k.startswith('audioCaptures'))}")


async def keep_shot(ed: Editor, name: str) -> None:
    folder = harness.REPO / "build" / "s35-captures-shots"
    folder.mkdir(parents=True, exist_ok=True)
    await ed.screenshot(view="editor", path=folder / f"{name}.png", max_width=0)


async def test_setup(ed: Editor):
    if not shutil.which("xdotool"):
        ed.fail("xdotool is needed for this real-input UI test (the linux image has it)")
    write_tone(ed.project / "sounds/tone.wav", seconds=2.0, frequency=800.0, rate=48000)
    await ed.call("sync_files")
    await ed.call("tool_groups", enable=["audio"])
    scene = (await ed.call("create_scene", name="Tone", type="2D", path="scenes/Tone.scene"))["scene"]["id"]
    await ed.call("create_entity", scene=scene, name="Source", type="audio_source")
    await ed.call("set_properties", scene=scene, entity="Source", values={"audiosource": {"filename": "sounds/tone.wav", "state": "playing"}})
    await ed.call("save_scene", scene=scene)
    STATE["scene"] = scene


async def test_banner_while_capturing(ed: Editor):
    await ed.call("play", action="start", scene=STATE["scene"], fixed_fps=60)
    await ed.call("wait", frames=5)
    await rect_of(ed, "game.capturingAudio")
    await keep_shot(ed, "banner")
    await ed.call("run_command", id="window.audioCaptures")
    await ed.call("wait", frames=60)
    ed.check(any(name.startswith("audioCaptures.session.") for name in ed.ui_items()), f"it opened: {sorted(ed.ui_items())[:20]}")
    await ed.call("play", action="stop")
    await ed.call("wait", frames=5)
    ed.check("game.capturingAudio" not in ed.ui_items(), "the banner goes with the capture")


async def test_lists_the_session_shows_the_lanes_and_plays_a_moment(ed: Editor):
    sessions = capture_sessions(ed.project)
    ed.check_eq(len(sessions), 1, f"{sessions}")
    await ed.call("audio_session", action="check", session=sessions[0].name)
    await rect_of(ed, "audioCaptures.session." + sessions[0].name)
    await rect_of(ed, "audioCaptures.timeline")
    await rect_of(ed, "audioCaptures.verdict")
    await rect_of(ed, "audioCaptures.listenBanner")
    moment = await rect_of(ed, "audioCaptures.moment.0")
    await keep_shot(ed, "captures")

    ui = Ui(ed)
    ui.focus()
    await ui.click(middle(ui, moment))
    await rect_of(ed, "audioCaptures.previewPlaying", 30.0)
    await keep_shot(ed, "moment-playing")
    await ui.key("Escape")
    await ui.frames(10)
    ed.check("audioCaptures.previewPlaying" not in ed.ui_items(), "Escape stops it")
    await ui.key("Down")
    await ui.key("Return")
    await rect_of(ed, "audioCaptures.previewPlaying", 30.0)
    await ui.key("Escape")
    await ui.frames(5)
    ed.check_eq(capture_sessions(ed.project), sessions, "a moment is not a capture")
