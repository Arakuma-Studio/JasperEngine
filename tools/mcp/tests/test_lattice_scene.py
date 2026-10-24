"""Lattice Scene-view editing (docs/superpowers/specs/2026-10-23-lattice-deformer-design.md §9.2, §9.3).

The human path end to end, with real X input on the harness' Xvfb (xdotool) and MCP for setup and
read-back: the selected lattice draws its cage; Ctrl+A selects every point, Alt+A none, Esc clears;
a click picks one point and a gizmo drag moves only it (LatticeEditorOps through LatticePointsCmd),
one undo step that undo/redo restore exactly; a rectangle selects the points inside it (Shift adds);
O turns the proportional falloff on and the same drag then moves the neighbours too, less; the
right-click menu opens over the view; a prefab instance's dragged point is an offsets[i] override;
in a 2D view one click takes the points stacked on one spot.

Points are found on screen by colour: selected points are drawn in the selection orange (1, 0.6, 0),
(255, 202, 0) on screen, and nothing else in this scene is (the lattice has no mesh, so no
selection outline).

Needs xdotool and ImageMagick's import (the harness' Xvfb, EDITOR xvfb="harness").
"""

from __future__ import annotations

import asyncio
import os
import shutil
import subprocess

import harness
import pngdiff
from harness import Editor

TIMEOUT = 900
EDITOR = {"create": True, "xvfb": "harness"}

S: dict = {}

ORANGE = (255, 202, 0)


# -- helpers --------------------------------------------------------------------


def need_tools():
    for tool in ("xdotool", "import"):
        if not shutil.which(tool):
            harness.skip(f"{tool} is not installed")


def run_x(ed: Editor, *args: str, capture: bool = False) -> bytes:
    if not getattr(ed, "_xvfb", None) or not ed._xvfb.display:
        harness.skip("needs the harness' Xvfb (xvfb='harness')")
    env = dict(os.environ, DISPLAY=ed._xvfb.display)
    done = subprocess.run(list(args), env=env, check=True, capture_output=True)
    return done.stdout if capture else b""


def focus_editor(ed: Editor):
    """X keyboard focus on the editor's window (Xvfb has no window manager to give it)."""
    ids = run_x(ed, "xdotool", "search", "--onlyvisible", "--name", "JasperEngine", capture=True).split()
    if not ids:
        ed.fail("the editor's X window was not found")
    run_x(ed, "xdotool", "windowfocus", "--sync", ids[-1].decode())


async def settle(ed: Editor, frames: int = 6):
    await ed.call("wait", frames=frames)


async def key(ed: Editor, combo: str):
    run_x(ed, "xdotool", "key", "--clearmodifiers", combo)
    await settle(ed)


async def click(ed: Editor, x: int, y: int, button: int = 1, modifier: str | None = None):
    run_x(ed, "xdotool", "mousemove", str(x), str(y))
    await settle(ed, 3)
    if modifier:
        run_x(ed, "xdotool", "keydown", modifier)
    run_x(ed, "xdotool", "mousedown", str(button))
    await settle(ed, 3)
    run_x(ed, "xdotool", "mouseup", str(button))
    if modifier:
        run_x(ed, "xdotool", "keyup", modifier)
    await settle(ed)


async def drag(ed: Editor, start: tuple[int, int], end: tuple[int, int], steps: int = 8, modifier: str | None = None):
    run_x(ed, "xdotool", "mousemove", str(start[0]), str(start[1]))
    await settle(ed, 3)
    if modifier:
        run_x(ed, "xdotool", "keydown", modifier)
    run_x(ed, "xdotool", "mousedown", "1")
    await settle(ed, 3)
    for n in range(1, steps + 1):
        x = round(start[0] + (end[0] - start[0]) * n / steps)
        y = round(start[1] + (end[1] - start[1]) * n / steps)
        run_x(ed, "xdotool", "mousemove", str(x), str(y))
        await settle(ed, 2)
    run_x(ed, "xdotool", "mouseup", "1")
    if modifier:
        run_x(ed, "xdotool", "keyup", modifier)
    await settle(ed)


def screen(ed: Editor):
    return pngdiff.decode(run_x(ed, "import", "-window", "root", "png:-", capture=True))


def blobs(img, color=ORANGE, tol: int = 24, min_pixels: int = 6) -> list[tuple[float, float, int]]:
    """Centres (x, y) and sizes of the connected areas of `color` on the screen."""
    width, height, rgba = img
    mask = bytearray(width * height)
    r0, g0, b0 = color
    for i in range(width * height):
        p = i * 4
        if abs(rgba[p] - r0) <= tol and abs(rgba[p + 1] - g0) <= tol and abs(rgba[p + 2] - b0) <= tol:
            mask[i] = 1
    found = []
    for start in range(width * height):
        if mask[start] != 1:
            continue
        stack = [start]
        mask[start] = 2
        sx = sy = n = 0
        while stack:
            i = stack.pop()
            x, y = i % width, i // width
            sx += x
            sy += y
            n += 1
            for j in (i - 1 if x > 0 else -1, i + 1 if x + 1 < width else -1, i - width, i + width):
                if 0 <= j < width * height and mask[j] == 1:
                    mask[j] = 2
                    stack.append(j)
        if n >= min_pixels:
            found.append([sx, sy, n])
    # pieces of one square (a gizmo handle or an edge drawn across it) count once
    merged = True
    while merged:
        merged = False
        for a in range(len(found)):
            for b in range(a + 1, len(found)):
                ax, ay = found[a][0] / found[a][2], found[a][1] / found[a][2]
                bx, by = found[b][0] / found[b][2], found[b][1] / found[b][2]
                if abs(ax - bx) <= 9 and abs(ay - by) <= 9:
                    found[a] = [found[a][0] + found[b][0], found[a][1] + found[b][1], found[a][2] + found[b][2]]
                    del found[b]
                    merged = True
                    break
            if merged:
                break
    return [(sx / n, sy / n, n) for sx, sy, n in found]


async def orange_points(ed: Editor) -> list[tuple[float, float, int]]:
    await settle(ed)
    return blobs(screen(ed))


async def offsets(ed: Editor) -> list[list[float]]:
    comp = (await ed.call("get_entity", scene=S["scene"], entity=S["lattice"], components=["lattice"]))["components"]["lattice"]
    count = comp["resolutionX"] * comp["resolutionY"] * comp["resolutionZ"]
    names = [f"offsets[{i}]" for i in range(count)]
    got = await ed.call("get_entity", scene=S["scene"], entity=S["lattice"], components=["lattice"], properties=names)
    values = got["components"]["lattice"]
    return [values[name] for name in names]


def moved(values: list[list[float]], eps: float = 1e-5) -> list[int]:
    return [i for i, v in enumerate(values) if max(abs(c) for c in v) > eps]


def length(v):
    return sum(c * c for c in v) ** 0.5


# -- tests ----------------------------------------------------------------------


async def test_setup(ed):
    need_tools()
    made = await ed.call("create_scene", name="Lattice View", type="3D", path="scenes/LatticeView")
    S["scene"] = made["scene"]["id"]
    await ed.call("select_scene", scene=S["scene"])
    await ed.call("focus_window", window="scene", scene=S["scene"])
    # a box beside the cage (not inside: points hidden by geometry are drawn faded)
    S["box"] = (await ed.call("create_entity", type="box", scene=S["scene"], name="Box", position=[-4, 0, -3]))["entity_id"]
    lattice = (await ed.call("create_entity", type="empty", scene=S["scene"], name="Cage"))["entity_id"]
    S["lattice"] = lattice
    await ed.call("add_component", scene=S["scene"], entity=lattice, component="LatticeComponent")
    await ed.call("set_transform", scene=S["scene"], entity=lattice, scale=[2, 2, 2])
    # an oblique view: the 27 points of the 3x3x3 cage land apart on screen
    await ed.call("set_scene_camera", scene=S["scene"], position=[3.2, 2.4, 5.0], target=[0, 0, 0])
    await ed.call("select_entities", scene=S["scene"], entities=[lattice])
    focus_editor(ed)
    await settle(ed, 10)
    ed.check_eq(await orange_points(ed), [], "no point is selected yet")


async def test_select_all_none_and_escape(ed):
    await ed.call("focus_window", window="scene", scene=S["scene"])
    await key(ed, "ctrl+a")
    points = await orange_points(ed)
    # all 27 points: the gizmo sits on the centre one (the centroid), the other 26 show
    ed.check_eq(len(points), 26, f"Ctrl+A selects the 27 points of the cage, saw {len(points)} orange squares")
    S["screen_points"] = [(round(x), round(y)) for x, y, _ in points]
    sel = await ed.call("get_selection", scene=S["scene"])
    ed.check_in(S["lattice"], [e.get("id", e) if isinstance(e, dict) else e for e in harness.find_entities(sel)] or [S["lattice"]],
                "the lattice stays the selection")

    await key(ed, "alt+a")
    ed.check_eq(await orange_points(ed), [], "Alt+A selects no point")

    await key(ed, "ctrl+a")
    ed.check_eq(len(await orange_points(ed)), 26, "Ctrl+A again")
    await key(ed, "Escape")
    ed.check_eq(await orange_points(ed), [], "Esc clears the point selection")


async def test_click_and_drag_one_point(ed):
    # the right-most point on screen: a corner, clear of the others
    x, y = max(S["screen_points"], key=lambda p: p[0])
    await click(ed, x, y)
    # the gizmo sits on the picked point now; its centre handle drags in the view plane
    before = await offsets(ed)
    ed.check_eq(moved(before), [], "nothing moved yet")
    await drag(ed, (x, y), (x - 90, y))
    after = await offsets(ed)
    changed = moved(after)
    ed.check_eq(len(changed), 1, f"one drag of one selected point moves that point only, moved {changed}")
    S["dragged"] = changed[0]
    S["dragged_offset"] = after[changed[0]]
    ed.check(length(after[changed[0]]) > 0.05, f"the point moved: {after[changed[0]]}")

    # one drag = one undo step, restored exactly; redo brings it back exactly
    await ed.call("undo", scene=S["scene"])
    ed.check_eq(moved(await offsets(ed)), [], "undo puts every point back")
    await ed.call("redo", scene=S["scene"])
    redone = await offsets(ed)
    ed.check_eq(moved(redone), [S["dragged"]], "redo moves the same point")
    ed.check(all(abs(a - b) < 1e-6 for a, b in zip(redone[S["dragged"]], S["dragged_offset"])),
             f"redo gives the same offset: {redone[S['dragged']]} vs {S['dragged_offset']}")
    await ed.call("undo", scene=S["scene"])


async def test_box_select(ed):
    await key(ed, "Escape")
    pts = S["screen_points"]
    xs = sorted(p[0] for p in pts)
    split = (xs[len(xs) // 2 - 1] + xs[len(xs) // 2]) // 2
    left = [p for p in pts if p[0] < split]
    top = min(p[1] for p in pts) - 25
    bottom = max(p[1] for p in pts) + 25
    start = (min(p[0] for p in pts) - 25, top)
    await drag(ed, start, (split, bottom))
    got = await orange_points(ed)
    # the gizmo sits at the selection's centre; its handles are not orange
    ed.check_eq(len(got), len(left), f"the rectangle selects the {len(left)} points inside it, saw {len(got)}")
    sel = await ed.call("get_selection", scene=S["scene"])
    ed.check(str(S["lattice"]) in str(sel), "the lattice is still selected after the rectangle")

    # Shift adds the rest
    await drag(ed, (split, top), (max(p[0] for p in pts) + 25, bottom), modifier="shift")
    # every point (the centre one under the gizmo)
    ed.check_eq(len(await orange_points(ed)), len(pts), "Shift + rectangle adds the other points")
    await key(ed, "Escape")


async def test_falloff_drag(ed):
    x, y = max(S["screen_points"], key=lambda p: p[0])
    await click(ed, x, y)
    await key(ed, "o")  # proportional falloff on
    await drag(ed, (x, y), (x - 90, y))
    after = await offsets(ed)
    changed = moved(after)
    ed.check(S["dragged"] in changed, "the picked point moves")
    ed.check(len(changed) > 1, f"with the falloff on its neighbours move too: {changed}")
    main = length(after[S["dragged"]])
    ed.check(all(length(after[i]) <= main + 1e-6 for i in changed), "the neighbours move less than the picked point")
    await ed.call("undo", scene=S["scene"])
    ed.check_eq(moved(await offsets(ed)), [], "the whole falloff drag is one undo step")
    await key(ed, "o")  # off again
    await key(ed, "Escape")


async def test_context_menu(ed):
    pts = S["screen_points"]
    cx = sum(p[0] for p in pts) // len(pts)
    cy = max(p[1] for p in pts) + 60
    before = screen(ed)
    await click(ed, cx, cy, button=3)
    after = screen(ed)
    ok, fraction, _ = pngdiff.compare(before, after, channel_tolerance=24, max_fraction=0.002)
    ed.check(not ok and fraction > 0.002, f"a right click over the view opens the lattice menu (changed {fraction:.4f})")
    await key(ed, "Escape")


def lattice_overrides(info) -> list[str]:
    return [p for e in info.get("overrides", []) if e.get("kind") == "property" and e.get("component") == "lattice"
            for p in e.get("properties", [])]


async def test_prefab_instance_point_override(ed):
    # the lattice becomes a prefab, of which it is an instance; one point dragged on the instance
    await ed.call("create_prefab", scene=S["scene"], entity=S["lattice"], path="prefabs/Cage.prefab")
    info = await ed.call("get_prefab_info", scene=S["scene"], entity=S["lattice"], usage=False)
    ed.check(info.get("prefab"), f"the lattice is an instance of the prefab now: {info}")
    ed.check_eq(lattice_overrides(info), [], "no point override yet")
    await ed.call("select_entities", scene=S["scene"], entities=[S["lattice"]])
    focus_editor(ed)
    await settle(ed, 8)
    x, y = max(S["screen_points"], key=lambda p: p[0])
    await click(ed, x, y)
    await drag(ed, (x, y), (x - 70, y))
    changed = moved(await offsets(ed))
    ed.check_eq(len(changed), 1, f"one point moved on the instance: {changed}")
    info = await ed.call("get_prefab_info", scene=S["scene"], entity=S["lattice"], usage=False)
    ed.check_eq(lattice_overrides(info), [f"offsets[{changed[0]}]"], f"the instance overrides that point only: {info}")
    await ed.call("undo", scene=S["scene"])
    ed.check_eq(moved(await offsets(ed)), [], "undo puts the point back")
    info = await ed.call("get_prefab_info", scene=S["scene"], entity=S["lattice"], usage=False)
    ed.check_eq(lattice_overrides(info), [], f"and drops the override: {info}")


async def test_2d_stacked_points(ed):
    # a 2D view looks down z: the three layers of a 3x3x3 cage around a sprite land on the
    # same spots, and one click takes the whole stack (the sprite between them follows fully)
    made = await ed.call("create_scene", name="Lattice 2D", type="2D", path="scenes/Lattice2D")
    S["scene"] = made["scene"]["id"]
    await ed.call("select_scene", scene=S["scene"])
    await ed.call("focus_window", window="scene", scene=S["scene"])
    await ed.call("create_entity", type="sprite", scene=S["scene"], name="Sprite")
    lattice = (await ed.call("create_entity", type="empty", scene=S["scene"], name="Cage 2D", position=[50, 50, 0]))["entity_id"]
    S["lattice"] = lattice
    await ed.call("add_component", scene=S["scene"], entity=lattice, component="lattice")
    await ed.call("set_transform", scene=S["scene"], entity=lattice, scale=[160, 160, 20])
    await ed.call("set_scene_camera", scene=S["scene"], center=[50, 50], zoom=0.5)
    await ed.call("select_entities", scene=S["scene"], entities=[lattice])
    focus_editor(ed)
    await settle(ed, 10)

    await key(ed, "ctrl+a")
    spots = [(round(x), round(y)) for x, y, _ in await orange_points(ed)]
    ed.check(len(spots) >= 8, f"the 27 points show on 9 spots (the centre one under the gizmo): {spots}")
    await key(ed, "Escape")

    x, y = max(spots, key=lambda p: p[0] - p[1])  # the corner right and up
    await click(ed, x, y)
    await drag(ed, (x, y), (x + 60, y - 40))
    after = await offsets(ed)
    changed = moved(after)
    ed.check_eq(len(changed), 3, f"the click took the three stacked points: {changed}")
    ed.check(all(after[i] == after[changed[0]] for i in changed), f"and they moved together: {[after[i] for i in changed]}")
    ed.check(abs(after[changed[0]][2]) < 1e-6, "a 2D move keeps z")
    await ed.call("undo", scene=S["scene"])
    ed.check_eq(moved(await offsets(ed)), [], "one undo step")
