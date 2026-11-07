"""The Sound Browser and the Sound Event view driven like a human (docs/specs/S37-sound-events.md R16, R18;
AC9): search, arrow and Space audition (a preview voice plays, no game voice line), Enter imports a built-in
clip with its .meta, a drag onto a Button adds a ui_click source in one undo step, Audition x8 shows 8
renditions, Match Levels brings the clips within 1 LU. Real X input in the harness's Xvfb.
"""

import asyncio
import json
import shutil

import harness
from audio_refs import TRACE_ENV, voice_lines, write_tone
from harness import Editor
from ui_input import Ui

EDITOR = {"xvfb": "harness", "create": True, "name": "Sound Browser", "ui_probe": True, "env": TRACE_ENV}
CI = True
TIMEOUT = 900

EVENT = "events/steps.soundevent"
STATE = {}


def middle(ui: Ui, rect: list) -> tuple[int, int]:
    """The screen point at the middle of a UI probe rect ([x0, y0, x1, y1])."""
    return ui.at(rect, ((rect[2] - rect[0]) / 2, (rect[3] - rect[1]) / 2))


async def rect_of(ed: Editor, name: str, seconds: float = 40.0):
    deadline = asyncio.get_running_loop().time() + seconds
    while asyncio.get_running_loop().time() < deadline:
        rect = ed.ui_items().get(name)
        if rect:
            return rect
        await ed.call("wait", frames=3)
    await keep_shot(ed, "not-drawn-" + name.replace(".", "-"))
    ed.fail(f"{name} is not drawn: {sorted(k for k in ed.ui_items() if k.startswith('soundBrowser'))}\n{(await ed.call('read_console', limit=30))}")


async def keep_shot(ed: Editor, name: str) -> None:
    """the editor window, where a run on a build server can fetch it (build/ is not synced back by itself)"""
    folder = harness.REPO / "build" / "s37-browser-shots"
    folder.mkdir(parents=True, exist_ok=True)
    await ed.screenshot(view="editor", path=folder / f"{name}.png", max_width=0)


async def test_setup(ed: Editor):
    if not shutil.which("xdotool"):
        ed.fail("xdotool is needed for this real-input UI test (the linux image has it)")
    write_tone(ed.project / "sounds/step_a.wav", seconds=0.6, amplitude=0.8, frequency=800)
    write_tone(ed.project / "sounds/step_b.wav", seconds=0.6, amplitude=0.05, frequency=500)
    write_tone(ed.project / "sounds/click.wav", seconds=0.1, amplitude=0.3, frequency=2000)
    await ed.call("sync_files")
    await ed.call("tool_groups", enable=["audio"])
    await ed.call("edit_sound_event", path=EVENT, create=True, values={"mode": "shuffle", "clips": ["sounds/step_a.wav", "sounds/step_b.wav"]})
    made = await ed.call("create_scene", name="Menu", type="UI", path="scenes/Menu.scene")
    STATE["scene"] = made["scene"]["id"]
    await ed.call("create_ui", scene=STATE["scene"], root={"type": "button", "name": "Go",
                                                           "layout": {"anchor": "top_left", "position": [100, 100], "size": [200, 100]}})
    await ed.call("save_scene", scene=STATE["scene"])
    await ed.call("run_command", id="window.soundBrowser")
    await rect_of(ed, "soundBrowser.card.step_a")


async def test_search_arrow_and_space_audition(ed: Editor):
    ui = Ui(ed)
    ui.focus()
    await ed.console_since(limit=500)
    search = await rect_of(ed, "soundBrowser.search")
    await ui.click(middle(ui, search))
    await ui.type("step")
    await ui.frames(20)
    items = ed.ui_items()
    ed.check("soundBrowser.card.step_a" in items, f"found: {sorted(items)}")
    ed.check("soundBrowser.card.click" not in items, f"the search leaves it out: {sorted(items)}")

    # out of the search field, then the keys of the window
    await ui.click(middle(ui, await rect_of(ed, "soundBrowser.card.step_a")))
    await ui.key("Down")
    await ui.frames(6)
    await ui.key("space")
    await rect_of(ed, "soundBrowser.previewPlaying", 20.0)
    await keep_shot(ed, "audition")
    # a preview voice is not a voice of the game
    lines = voice_lines(await ed.console_since(limit=500))
    ed.check_eq(lines, [], "no voice line for an audition")
    await ui.key("space")
    await ui.frames(10)
    ed.check("soundBrowser.previewPlaying" not in ed.ui_items(), "Space again stops it")
    # Escape clears the search
    await ui.key("Escape")
    await rect_of(ed, "soundBrowser.card.click")


async def test_enter_imports_a_built_in_clip_with_its_meta(ed: Editor):
    ui = Ui(ed)
    ui.focus()
    await ui.click(middle(ui, await rect_of(ed, "soundBrowser.source.Essentials")))
    await ui.frames(20)
    cards = []
    for _ in range(60):
        cards = [name for name in ed.ui_items() if name.startswith("soundBrowser.card.")]
        if cards:
            break
        await ed.call("wait", frames=5)
    ed.check(cards, "the essentials are listed")
    name = sorted(cards)[0]
    STATE["builtin"] = name[len("soundBrowser.card."):]
    await ui.click(middle(ui, ed.ui_items()[name]))
    await ui.key("Return")
    await ed.call("sync_files")
    imported = list((ed.project / "sounds/essentials").rglob(STATE["builtin"] + ".ogg"))
    ed.check(imported, f"imported under sounds/essentials: {list((ed.project / 'sounds').rglob('*'))[:20]}")
    ed.check(imported[0].with_name(imported[0].name + ".meta").is_file(), "with its .meta")
    await ui.click(middle(ui, await rect_of(ed, "soundBrowser.source.Project")))
    await rect_of(ed, "soundBrowser.card.click")


async def test_a_drag_onto_a_button_adds_a_ui_click_source_in_one_undo_step(ed: Editor):
    ui = Ui(ed)
    ui.focus()
    start = middle(ui, await rect_of(ed, "soundBrowser.card.click"))
    target = middle(ui, await rect_of(ed, "hierarchy.node.Go"))
    await ui.press(start)
    for i in range(1, 13):
        await ui.move((start[0] + (target[0] - start[0]) * i // 12, start[1] + (target[1] - start[1]) * i // 12), frames=2)
    await ui.release()
    await ui.frames(10)
    got = await ed.call("get_entity", scene=STATE["scene"], entity="Go", components=["audiosource"])
    audio = got["components"]["audiosource"]
    ed.check_eq(audio["playOn"], ["ui_click"], f"{audio}")
    ed.check("click.wav" in audio["filename"], f"{audio}")
    await ed.call("undo")
    got = await ed.call("get_entity", scene=STATE["scene"], entity="Go")
    ed.check("audiosource" not in json.dumps(got["components"]), f"one undo step takes it all back: {list(got['components'])}")


async def test_the_event_view_audition_and_match_levels(ed: Editor):
    ui = Ui(ed)
    ui.focus()
    await ui.click(middle(ui, await rect_of(ed, "soundBrowser.card.steps")))
    await ui.click(middle(ui, await rect_of(ed, "soundEvent.audition")))
    for i in range(8):
        await rect_of(ed, f"soundEvent.rendition.{i}", 60.0)
    await keep_shot(ed, "event-view")

    before = (ed.project / EVENT).read_text()
    await ui.click(middle(ui, await rect_of(ed, "soundEvent.matchLevels")))
    for _ in range(60):
        if (ed.project / EVENT).read_text() != before:
            break
        await ed.call("wait", frames=5)
    body = json.loads((ed.project / EVENT).read_text())
    ed.check("volume_db" in body["clips"][1], f"the quiet clip is raised: {body}")
    result = await ed.call("edit_sound_event", path=EVENT, audition=8)
    levels = {r["picks"][0]["variation"]: r["lufs"] for r in result["renditions"]}
    ed.check_eq(sorted(levels), [0, 1], f"{result['renditions']}")
    ed.check(abs(levels[0] - levels[1]) <= 1.0, f"within 1 LU: {levels}")
