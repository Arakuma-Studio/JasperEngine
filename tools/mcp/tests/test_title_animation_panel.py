"""S85 AC17 (R41, R42, R44): the Title Designer's Animation panel opens with a document that has
animations, closed (its header only) with one that has none, and draws its tracks. The screenshots
land in build/shots for a person to look at.
"""

from pathlib import Path

import harness
from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Title Animation Panel"}
TIMEOUT = 600

SHOTS = Path(__file__).resolve().parents[3] / "build" / "shots"


async def shot(ed: Editor, name: str) -> bytes:
    await ed.call("wait", frames=12)
    data = await ed.screenshot("title_designer", max_width=0)
    SHOTS.mkdir(parents=True, exist_ok=True)
    (SHOTS / name).write_bytes(data)
    return data


async def test_the_strip_opens_with_animations(ed: Editor):
    await ed.call("tool_groups", enable=["title_art", "editor_tools", "editor_ux"])
    await ed.call("set_project_settings", values={"title_art.auto_bake": False})
    path = "assets/panel.titleart"
    await ed.call("edit_title", path=path, create={"text": "JACKPOT"}, bake="never")
    r = await ed.call("focus_window", window="title_designer", title_designer={"path": path})
    await shot(ed, "title-animation-panel-empty.png")
    ed.check_eq(r["state"]["panels"]["animation"], False, "closed without animations")

    await ed.call("edit_title", path=path, bake="never", ops=[
        {"op": "add_rim", "layer": "Title"},
        {"op": "add_animation", "name": "Intro", "duration": 1.0},
        {"op": "set_track", "animation": "Intro", "field": "opacity", "keys": [[0, 0, "quad_out"], [0.5, 1]]},
        {"op": "set_track", "animation": "Intro", "field": "transform.scale", "relative": True, "keys": [[0, 2, "back_out"], [0.6, 1]]},
        {"op": "set_track", "animation": "Intro", "layer": "Title", "field": "rim#0.width", "keys": [[0, 2], [0.5, 12], [1, 4]]},
        {"op": "set_track", "animation": "Intro", "layer": "Title", "field": "glyph.opacity",
         "keys": [[0, 0], [0.3, 1]], "stagger": {"each": 0.05, "order": "forward"}},
        {"op": "set_marker", "animation": "Intro", "name": "impact", "time": 0.5},
    ])
    await ed.call("wait", frames=12)
    r = await ed.call("focus_window", window="title_designer", title_designer={"path": path})
    ed.check_eq(r["state"]["panels"]["animation"], True, "the first animation opens it")
    await shot(ed, "title-animation-panel.png")


async def test_the_canvas_shows_the_pose_at_a_time(ed: Editor):
    """R46: at 0 the title is transparent and at 0.5 s it is back: the pose reaches the canvas."""
    path = "assets/panel.titleart"
    designer = {"path": path}
    await ed.call("focus_window", window="title_designer", title_designer=designer)
    await ed.call("wait", frames=4)
    await ed.check_error("focus_window", {"window": "title_designer", "title_designer": {"animation": {"name": "Nope", "time": 0}}}, contains="no animation 'Nope'")
    await ed.call("focus_window", window="title_designer", title_designer={"animation": {"name": "Intro", "time": 0.0}})
    hidden = await shot(ed, "title-animation-pose-0.png")
    await ed.call("focus_window", window="title_designer", title_designer={"animation": {"name": "Intro", "time": 0.5}})
    shown = await shot(ed, "title-animation-pose-050.png")
    ed.check(hidden != shown, "a different time shows a different pose")
    await ed.call("focus_window", window="title_designer", title_designer={"animation": None})
    await shot(ed, "title-animation-pose-stopped.png")
