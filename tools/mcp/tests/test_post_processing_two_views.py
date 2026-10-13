"""Post-processing in two editor views of one frame (shader library phase 4, final pass): with the
Scene view's "Show post-processing" on and the Game view shown beside it, both draw the uber pass and
the bloom chain every frame at their own sizes. Each view keeps its own targets (RenderSystem::
RenderView), so no frame makes an image: checked with JASPERENGINE_FRAME_STATS=all, which logs a line
for the frame (the Scene view in edit mode, the game while playing) and one per extra view
("view N": the Game view while stopped, the Scene view while playing).

The default layout has the Game view and the Scene view as tabs of one dock (one shown at a time),
so the editor starts with a layout that docks the Game view in the bottom panel instead."""

from __future__ import annotations

import os
import re
import time

import harness
from harness import Editor, Scratch

# read at editor start: this file runs in its own process
os.environ["JASPERENGINE_READY_SIGNAL"] = "1"
os.environ["JASPERENGINE_FRAME_STATS"] = "all"

EDITOR = {"create": True, "name": "Post Processing Two Views"}
TIMEOUT = 900

# the harness's default layout (1600x900) with the Game view moved into the bottom panel
LAYOUT = """[Window][Inspector]
Pos=1248,54
Size=352,825
Collapsed=0
DockId=0x00000002,0

[Window][MCP Server]
Collapsed=0
DockId=0x00000002,1

[Window][Terrain]
Collapsed=0
DockId=0x00000002,2

[Window][Project]
Pos=0,615
Size=1246,263
Collapsed=0
DockId=0x00000004,0

[Window][Console]
Pos=0,615
Size=1246,263
Collapsed=0
DockId=0x00000004,1

[Window][Animation]
Pos=0,615
Size=1246,263
Collapsed=0
DockId=0x00000004,2

[Window][Hierarchy]
Pos=0,54
Size=288,559
Collapsed=0
DockId=0x00000005,0

[Window][Game]
Pos=0,615
Size=1246,263
Collapsed=0
DockId=0x00000004,3

[Docking][Data]
DockSpace       ID=0xFB305400 Window=0x1BBC0F80 Pos=0,54 Size=1600,825 Split=X
  DockNode      ID=0x00000001 Parent=0xFB305400 SizeRef=1248,879 Split=Y
    DockNode    ID=0x00000003 Parent=0x00000001 SizeRef=1248,615 Split=X
      DockNode  ID=0x00000005 Parent=0x00000003 SizeRef=288,615 Selected=0xBABDAE5E
      DockNode  ID=0x00000006 Parent=0x00000003 SizeRef=960,615 CentralNode=1
    DockNode    ID=0x00000004 Parent=0x00000001 SizeRef=1248,263 Selected=0xD1EB2482
  DockNode      ID=0x00000002 Parent=0xFB305400 SizeRef=352,879 Selected=0x36DC96AB
"""

_make = Scratch.make.__func__


def _make_with_layout(cls, project=None, create=False, prefix="jasper-mcp-"):
    scratch = _make(cls, project, create, prefix)
    (scratch.root / "editor_layout_v2.ini").write_text(LAYOUT)
    return scratch


harness.Scratch.make = classmethod(_make_with_layout)

LINE = re.compile(r"Frame stats \(frame (\d+)(?:, view (\d+))?\): .*?images (\d+) alive \((\d+) made, (\d+) made last frame\).*?"
                  r"pass list: (.*)$")


def stats_lines(ed: Editor, start: int) -> list[dict]:
    log = ed.scratch.root / "editor.log"
    lines = log.read_text(errors="replace").splitlines()[start:] if log.exists() else []
    out = []
    for line in lines:
        m = LINE.search(line)
        if m:
            uber = re.search(r"uber (\d+x\d+)", m.group(6))
            bloom = re.search(r"bloom prefilter (\d+x\d+)", m.group(6))
            out.append({"frame": int(m.group(1)), "view": int(m.group(2)) if m.group(2) else 0,
                        "alive": int(m.group(3)), "made": int(m.group(4)), "made_last": int(m.group(5)),
                        "uber": uber.group(1) if uber else None, "bloom": bloom.group(1) if bloom else None})
    return out


def log_length(ed: Editor) -> int:
    log = ed.scratch.root / "editor.log"
    return len(log.read_text(errors="replace").splitlines()) if log.exists() else 0


async def measure(ed: Editor, what: str, ball: int, moves: int = 30) -> None:
    """Both views draw the uber pass and the bloom chain at their own sizes, and nothing is made."""
    # an idle editor draws nothing: moving the ball redraws both views (the playing game every frame)
    for i in range(6):
        await ed.call("set_transform", entity=ball, position=[0.1 * (i % 2), 0, 0])
        await ed.call("wait", frames=5)  # settle: targets made once per view
    start = log_length(ed)
    for i in range(moves):
        await ed.call("set_transform", entity=ball, position=[0.2 * (i % 3), 0, 0])
        await ed.call("wait", frames=2)
    lines: list[dict] = []
    deadline = time.time() + 30
    while time.time() < deadline:
        lines = stats_lines(ed, start)
        if len([x for x in lines if x["view"] == 0]) >= 20 and len([x for x in lines if x["view"] != 0]) >= 20:
            break
        time.sleep(1)
    frame_lines = [x for x in lines if x["view"] == 0 and x["uber"]]
    view_lines = [x for x in lines if x["view"] != 0 and x["uber"]]
    ed.check(len(frame_lines) >= 20 and len(view_lines) >= 20,
             f"{what}: both views draw the uber pass ({len(frame_lines)} frame lines, {len(view_lines)} view lines)")
    sizes = {x["uber"] for x in frame_lines} | {x["uber"] for x in view_lines}
    ed.check(len({x["uber"] for x in frame_lines}) == 1 and len({x["uber"] for x in view_lines}) == 1 and len(sizes) == 2,
             f"{what}: the two views draw at two different sizes: {sizes}")
    ed.check(all(x["bloom"] for x in frame_lines + view_lines), f"{what}: both views run the bloom chain")
    made = {x["made"] for x in lines}
    ed.check(len(made) == 1 and all(x["made_last"] == 0 for x in lines),
             f"{what}: no image made in {len(lines)} measured commits (images made in total {sorted(made)}, "
             f"made last frame {sorted({x['made_last'] for x in lines})})")
    print(f"{what}: frame uber {frame_lines[0]['uber']} bloom {frame_lines[0]['bloom']}, view {view_lines[0]['view']} "
          f"uber {view_lines[0]['uber']} bloom {view_lines[0]['bloom']}; {len(lines)} commits, images alive "
          f"{lines[-1]['alive']}, made {lines[-1]['made']}")


async def test_two_views_keep_their_targets(ed: Editor):
    ed.require_tools("create_scene", "create_entity", "set_scene_settings", "set_scene_camera", "focus_window", "wait", "play", "set_transform")
    await ed.call("create_scene", name="TwoViews", type="3d")
    ball = (await ed.call("create_entity", name="Ball", type="sphere"))["entity_id"]
    await ed.call("set_scene_settings", settings={"post_processing.bloom_enabled": True, "post_processing.fxaa_enabled": True,
                                                   "post_processing.render_scale": 0.75})
    await ed.call("set_scene_camera", display={"show_post_processing": True})
    await ed.call("focus_window", window="game")
    shown = await ed.call("focus_window", window="scene")
    ed.check(shown["shown"], "the Scene view is shown beside the Game view")

    # stopped: the frame is the Scene view, the Game view a second view (2)
    await measure(ed, "edit mode", ball)

    # playing: the frame is the game (Game view), the Scene view a second view (1)
    await ed.call("play", action="start")
    try:
        await ed.call("focus_window", window="scene")
        await measure(ed, "play mode", ball)
    finally:
        await ed.call("play", action="stop")
