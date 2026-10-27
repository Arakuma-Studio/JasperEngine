"""S26 with real input (xdotool in the harness's Xvfb, 1600x900, default layout), checked through
MCP: Ctrl+wheel cycling (AC8), a collapsed Hierarchy group as the pick root (AC8), H / Alt+H / L
(AC15), F with Camera Easing off (AC14), Alt+F, the view gizmo's cube toggling Persp/Iso (AC11),
a prefab dropped on a tilted plane and a drop refused while the surface's BVH builds (AC10), a
click waiting for a BVH with its status text (R33), camera bookmark shortcuts (AC12) and the pane
under the pointer becoming the active one (R20).

Positions come from the editor (the scene screenshot's view_rect_in_window, find_ui_item) so a
layout change fails with the screenshots ($JE_EVIDENCE_DIR, else the scratch directory) to look at.
"""

from __future__ import annotations

import asyncio
import json
import math
import os
import shutil
import subprocess
from pathlib import Path

from harness import Editor, skip

# background mesh BVH builds stay pending 90 s (only meshes of 256+ triangles have one): what a
# click and a drop do meanwhile is seen (R16, R33), also on a loaded host drawing a few frames a second
EDITOR = {"xvfb": "harness", "create": True, "name": "Scene View UI", "env": {"JASPER_PICK_BUILD_DELAY_MS": "90000"}}
TIMEOUT = 1200

S: dict = {}


class Ui:
    """xdotool input, each action followed by editor frames (the editor reads X events at a frame's
    start, so a check after settle() sees the input applied)."""

    def __init__(self, ed: Editor):
        self.ed = ed
        self.env = dict(os.environ, DISPLAY=ed._xvfb.display)
        windows = self.xdo("search", "--name", "JasperEngine").split() or self.xdo("search", "--name", ".").split()
        if not windows:
            skip("no editor window found")
        self.window = windows[-1]
        geometry = self.xdo("getwindowgeometry", self.window)
        x, y = 0, 0
        for line in geometry.splitlines():
            if "Position:" in line:
                x, y = (int(v) for v in line.split("Position:")[1].split("(")[0].strip().split(","))
        self.origin = (x, y)

    def xdo(self, *args: str) -> str:
        return subprocess.run(["xdotool", *args], env=self.env, capture_output=True, text=True, timeout=20).stdout

    def focus(self):
        self.xdo("windowactivate", "--sync", self.window)
        self.xdo("windowfocus", "--sync", self.window)

    def at(self, rect: dict, offset: tuple[float, float]) -> tuple[int, int]:
        return (int(self.origin[0] + rect["x"] + offset[0]), int(self.origin[1] + rect["y"] + offset[1]))

    async def settle(self, seconds: float):
        await asyncio.sleep(seconds)
        await self.ed.call("wait", frames=2)

    async def move(self, point: tuple[int, int]):
        self.xdo("mousemove", str(point[0]), str(point[1]))
        await self.settle(0.25)

    async def click(self, point: tuple[int, int], button: str = "1"):
        await self.move(point)
        self.xdo("mousedown", button)
        await self.settle(0.1)
        self.xdo("mouseup", button)
        await self.settle(0.4)

    async def wheel(self, point: tuple[int, int], down: bool, modifier: str | None = None):
        await self.move(point)
        if modifier:
            self.xdo("keydown", modifier)
            await self.settle(0.1)
        self.xdo("click", "5" if down else "4")
        await self.settle(0.2)
        if modifier:
            self.xdo("keyup", modifier)
            await self.settle(0.2)

    async def drag(self, start: tuple[int, int], end: tuple[int, int], steps: int = 16):
        await self.move(start)
        self.xdo("mousedown", "1")
        await self.settle(0.2)
        for k in range(1, steps + 1):
            self.xdo("mousemove", str(int(start[0] + (end[0] - start[0]) * k / steps)), str(int(start[1] + (end[1] - start[1]) * k / steps)))
            await asyncio.sleep(0.06)
        await self.settle(0.4)
        self.xdo("mouseup", "1")
        await self.settle(0.6)

    async def key(self, *keys: str):
        for key in keys:
            *mods, main = key.split("+")
            for mod in mods:
                self.xdo("keydown", mod)
            if mods:
                await self.settle(0.1)
            self.xdo("key", main)
            if mods:
                await self.settle(0.1)
            for mod in reversed(mods):
                self.xdo("keyup", mod)
            await self.settle(0.3)


def evidence(ed: Editor) -> Path:
    folder = Path(os.environ.get("JE_EVIDENCE_DIR") or ed.scratch.root)
    folder.mkdir(parents=True, exist_ok=True)
    return folder


async def setup(ed: Editor) -> dict:
    if S.get("ed") is ed:
        S["ui"].xdo("mouseup", "1")
        S["ui"].xdo("keyup", "ctrl")
        S["ui"].xdo("keyup", "shift")
        S["ui"].xdo("keyup", "alt")
        return S
    if not shutil.which("xdotool") or not ed._xvfb or not ed._xvfb.display:
        skip("needs xdotool and the harness's Xvfb")
    S.clear()
    S["ed"] = ed
    S["ui"] = Ui(ed)
    S["out"] = evidence(ed)
    return S


async def new_scene(ed: Editor, name: str) -> int:
    made = await ed.call("create_scene", name=name, type="3D", path=f"scenes/{name}")
    sid = made["scene"]["id"]
    await ed.call("select_scene", scene=sid)
    return sid


async def cube(ed: Editor, sid: int, name: str, position, scale=(1, 1, 1), parent=None, kind="cube") -> int:
    args = {"scene": sid, "type": kind, "name": name, "position": list(position), "scale": list(scale), "select": False}
    if parent is not None:
        args["parent"] = parent
    return (await ed.call("create_entity", **args))["entity_id"]


async def view(ed: Editor, sid: int) -> tuple[dict, dict]:
    """(view_rect_in_window, the screenshot's info) of the scene's (active pane) view"""
    shot = await ed.call_raw("screenshot", {"view": "scene", "scene": sid, "max_width": 0})
    return shot.structured["view_rect_in_window"], shot.structured


def uv(ui: Ui, rect: dict, u: float, v: float) -> tuple[int, int]:
    return ui.at(rect, (rect["width"] * u, rect["height"] * v))


async def selected(ed: Editor, sid: int) -> list[int]:
    return [e["id"] for e in (await ed.call("get_selection", scene=sid)).get("entities", [])]


async def camera(ed: Editor, sid: int) -> dict:
    return (await ed.call("set_scene_camera", scene=sid))["camera"]


def rotate(q, v):
    """v rotated by the quaternion [x, y, z, w]"""
    x, y, z, w = q
    ix = w * v[0] + y * v[2] - z * v[1]
    iy = w * v[1] + z * v[0] - x * v[2]
    iz = w * v[2] + x * v[1] - y * v[0]
    iw = -x * v[0] - y * v[1] - z * v[2]
    return [ix * w + iw * -x + iy * -z - iz * -y, iy * w + iw * -y + iz * -x - ix * -z, iz * w + iw * -z + ix * -y - iy * -x]


def lattice_obj(path: Path, n: int, solid: bool):
    c = n * 0.05
    lines = [f"v {i * 0.1 - c:.3f} {j * 0.1 - c:.3f} 0" for i in range(n + 1) for j in range(n + 1)]
    for i in range(n):
        for j in range(n):
            if solid or i % 4 == 0 or j % 4 == 0:
                a = i * (n + 1) + j + 1
                lines.append(f"f {a} {a + n + 1} {a + 1}")
                lines.append(f"f {a + 1} {a + n + 1} {a + n + 2}")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n")


# ---------------------------------------------------------------------------- AC8


async def robot(ed: Editor) -> dict:
    if "robot" in S:
        await ed.call("select_scene", scene=S["robot"]["scene"])
        return S["robot"]
    sid = await new_scene(ed, "WheelRobot")
    root = (await ed.call("create_entity", scene=sid, type="empty", name="Robot", select=False))["entity_id"]
    arm = await cube(ed, sid, "Arm", (0, 0, 0), parent=root)
    wall = await cube(ed, sid, "Wall", (0, 0, -3), (8, 8, 0.2))
    await ed.call("create_prefab", scene=sid, entity=root, path="prefabs/WheelRobot.prefab")
    await ed.call("set_scene_camera", scene=sid, position=[0, 0, 5], target=[0, 0, 0])
    S["robot"] = {"scene": sid, "robot": root, "arm": arm, "wall": wall}
    return S["robot"]


async def test_ctrl_wheel_cycle(ed: Editor):
    s = await setup(ed)
    ui = s["ui"]
    r = await robot(ed)
    sid = r["scene"]
    await ed.call("select_entities", scene=sid, entities=[])
    rect, _ = await view(ed, sid)
    centre = uv(ui, rect, 0.5, 0.5)
    ui.focus()
    seen = []
    for _ in range(3):
        await ui.wheel(centre, down=True, modifier="ctrl")
        seen.append((await selected(ed, sid) or [None])[0])
    await ed.screenshot("editor", path=str(s["out"] / "s26-ctrl-wheel-pill.png"))
    ed.check_eq(seen, [r["robot"], r["arm"], r["wall"]], "Ctrl+wheel down walks root, child, the wall behind")
    cam_before = await camera(ed, sid)
    ed.check(abs(cam_before["distance"] - 5) < 1e-3, f"Ctrl+wheel does not zoom: {cam_before}")
    await ui.wheel(centre, down=False, modifier="ctrl")
    ed.check_eq(await selected(ed, sid), [r["arm"]], "Ctrl+wheel up goes back")
    # the next plain click at that spot confirms it
    await ui.click(centre)
    ed.check_eq(await selected(ed, sid), [r["arm"]], "the click confirms the wheel's choice")


async def test_collapsed_group_is_root(ed: Editor):
    s = await setup(ed)
    ui = s["ui"]
    sid = await new_scene(ed, "Collapsed")
    group = (await ed.call("create_entity", scene=sid, type="empty", name="Shelf", select=False))["entity_id"]
    book = await cube(ed, sid, "Book", (0, 0, 0), parent=group)
    await ed.call("set_scene_camera", scene=sid, position=[0, 0, 5], target=[0, 0, 0])
    got = await ed.call("pick_at", scene=sid, u=0.5, v=0.5)
    ed.check_eq(got["would_select"]["entity"], book, "an open group: the GameObject under the pointer")
    await ed.call("focus_window", window="hierarchy")
    found = await ed.call("find_ui_item", label="Shelf", window="Hierarchy")
    items = found.get("items", [])
    if not items:
        ed.fail(f"the Shelf row was not found: {found}")
    row = items[0]["rect"]
    # the tree arrow at the row's left (about a font height wide)
    await ui.click(ui.at(row, (row["height"] / 2, row["height"] / 2)))
    await ed.screenshot("editor", path=str(s["out"] / "s26-collapsed-group.png"))
    # the arrow's click selects the row too: from no selection (a selected root drills, R13)
    await ed.call("select_entities", scene=sid, entities=[])
    got = await ed.call("pick_at", scene=sid, u=0.5, v=0.5)
    ed.check_eq(got["would_select"]["entity"], group, "under a collapsed row: the row (R12 root source)")


# ---------------------------------------------------------------------------- AC15


async def test_hide_lock_shortcuts(ed: Editor):
    s = await setup(ed)
    ui = s["ui"]
    sid = await new_scene(ed, "Keys")
    box = await cube(ed, sid, "Box", (0, 0, 0))
    await ed.call("set_scene_camera", scene=sid, position=[0, 0, 5], target=[0, 0, 0])
    await ed.call("select_entities", scene=sid, entities=[box])
    rect, _ = await view(ed, sid)
    await ui.move(uv(ui, rect, 0.2, 0.2))
    await ed.call("focus_window", window="scene", scene=sid)
    await ui.key("h")
    state = await ed.call("set_scene_view_filter", scene=sid)
    ed.check_eq([e["id"] for e in state["hidden"]], [box], "H hides the selection")
    await ed.screenshot("editor", path=str(s["out"] / "s26-hidden-banner.png"))
    await ui.key("alt+h")
    ed.check_eq((await ed.call("set_scene_view_filter", scene=sid))["hidden"], [], "Alt+H shows all")
    await ui.key("l")
    ed.check_eq([e["id"] for e in (await ed.call("set_scene_view_filter", scene=sid))["locked"]], [box], "L locks")
    await ui.key("l")
    ed.check_eq((await ed.call("set_scene_view_filter", scene=sid))["locked"], [], "L again unlocks")


# ---------------------------------------------------------------------------- AC14, AC11


async def test_frame_keys(ed: Editor):
    s = await setup(ed)
    ui = s["ui"]
    sid = await new_scene(ed, "FrameKeys")
    box = await cube(ed, sid, "Far", (40, 5, -30))
    await cube(ed, sid, "Other", (-20, 0, 10))
    await ed.call("set_scene_camera", scene=sid, position=[0, 2, 10], target=[0, 0, 0],
                  camera_settings={"easing": False})
    await ed.call("select_entities", scene=sid, entities=[box])
    rect, _ = await view(ed, sid)
    await ui.move(uv(ui, rect, 0.3, 0.3))
    await ed.call("focus_window", window="scene", scene=sid)
    await ui.key("f")
    cam = await camera(ed, sid)
    ed.check(all(abs(a - b) < 1e-2 for a, b in zip(cam["target"], [40, 5, -30])), f"with easing off, F frames at once: {cam}")
    await ui.key("alt+f")
    cam = await camera(ed, sid)
    ed.check(abs(cam["target"][0] - 10) < 1.0 and abs(cam["target"][2] + 10) < 1.0, f"Alt+F frames both boxes: {cam['target']}")
    await ed.call("set_scene_camera", scene=sid, camera_settings={"easing": True})


async def test_gizmo_cube_toggles_projection(ed: Editor):
    s = await setup(ed)
    ui = s["ui"]
    sid = await new_scene(ed, "Gizmo")
    await cube(ed, sid, "Box", (0, 0, 0))
    await ed.call("set_scene_camera", scene=sid, position=[4, 3, 6], target=[0, 0, 0])
    rect, info = await view(ed, sid)
    ed.check_eq((await camera(ed, sid))["projection"], "perspective")
    # the view gizmo: VIEWPORT_GIZMO_SIZE (100) times the overlay scale, at the view's top right
    scale = rect["width"] / info["width"]   # window pixels per image pixel
    gizmo = 100 * info.get("overlay_scale", 1.0) * scale
    centre = ui.at(rect, (rect["width"] - gizmo / 2, gizmo / 2))
    await ui.click(centre)
    await ed.screenshot("editor", path=str(s["out"] / "s26-gizmo-iso.png"))
    ed.check_eq((await camera(ed, sid))["projection"], "orthographic", "the cube toggles to Iso")
    await ui.click(centre)
    ed.check_eq((await camera(ed, sid))["projection"], "perspective", "and back to Persp")


# ---------------------------------------------------------------------------- AC10


async def project_window(ed: Editor) -> dict:
    return (await ed.call("editor_status"))["ui"]["project_window"]


async def project_item(ed: Editor, ui: Ui, name: str) -> dict:
    """The Project window entry `name` of prefabs/ (editor_status's clickable rects): the folder
    opened from the tree first"""
    await ed.call("focus_window", window="project")
    state = {}
    for _ in range(40):
        state = await project_window(ed)
        for item in state["entries"]:
            if item["name"] == name and state["directory"] == "prefabs":
                x, y, w, h = item["rect"]
                return {"x": x, "y": y, "width": w, "height": h}
        for node in state["tree"]:
            if node["path"] == "prefabs" and state["directory"] != "prefabs":
                x, y, w, h = node["rect"]
                await ui.click(ui.at({"x": x, "y": y}, (w / 2, h / 2)))
                break
        await asyncio.sleep(0.25)
    ed.fail(f"prefabs/{name} is not shown in the Project window: {state}")


async def scene_view_state(ed: Editor, sid: int) -> dict:
    result = await ed.session.read_resource(f"jasper://scene-view/{sid}")
    return json.loads(result.contents[0].text)


async def status_message(ed: Editor, text: str, seconds: float = 6.0) -> str:
    """The status bar's message once it says `text` (it is set a frame or two after the cause)"""
    message = ""
    for _ in range(int(seconds / 0.1)):
        message = (await ed.call("editor_status")).get("status_message", "")
        if text in message:
            break
        await asyncio.sleep(0.1)
    return message


async def test_drop_on_surface(ed: Editor):
    s = await setup(ed)
    ui = s["ui"]
    sid = await new_scene(ed, "Drop")
    plane = await cube(ed, sid, "Slope", (0, 0, 0), (10, 1, 10), kind="plane")
    await ed.call("set_transform", scene=sid, entity=plane, rotation={"euler": [30, 0, 0]})
    crate = await cube(ed, sid, "Crate", (50, 0, 0))
    await ed.call("create_prefab", scene=sid, entity=crate, path="prefabs/Crate.prefab")
    await ed.call("delete_entities", scene=sid, entities=[crate])
    await ed.call("set_scene_camera", scene=sid, position=[0, 8, 8], target=[0, 0, 0])
    got = await ed.call("get_entity", scene=sid, entity=plane, components=["transform"], properties=["rotation"])
    q = got["components"]["transform"]["rotation"]["quat"]
    normal = rotate(q, [0, 1, 0])

    for align in (False, True):
        await ed.call("select_scene", scene=sid)
        settings_align = "align" if align else "plain"
        rect, _ = await view(ed, sid)
        item = await project_item(ed, ui, "Crate.prefab")
        start = ui.at(item, (item["width"] / 2, item["height"] / 2))
        if align:
            ui.xdo("keydown", "alt")
        await ui.drag(start, uv(ui, rect, 0.5, 0.5))
        if align:
            ui.xdo("keyup", "alt")
        await ed.screenshot("editor", path=str(s["out"] / f"s26-drop-{settings_align}.png"))
        found = await ed.call("find_entities", scene=sid, name="Crate")
        crates = [e for e in found["entities"] if e["id"] != crate]
        ed.check(len(crates) >= 1, f"the drop made an instance: {found}")
        inst = crates[-1]["id"]
        t = (await ed.call("get_entity", scene=sid, entity=inst, components=["transform"]))["components"]["transform"]
        pos, rq = t["position"], t["rotation"]["quat"]
        # every corner on or above the plane, the lowest on it
        dists = []
        for corner in range(8):
            c = [0.5 if corner >> k & 1 else -0.5 for k in range(3)]
            w = rotate(rq, c)
            p = [pos[i] + w[i] for i in range(3)]
            dists.append(sum(p[i] * normal[i] for i in range(3)))
        ed.check(abs(min(dists)) <= 1e-3, f"{settings_align}: the crate rests on the plane: {dists}")
        if align:
            up = rotate(rq, [0, 1, 0])
            angle = math.degrees(math.acos(max(-1.0, min(1.0, sum(up[i] * normal[i] for i in range(3))))))
            ed.check(angle <= 0.5, f"Align to Surface: the up axis is the normal ({angle:.3f} degrees)")
            bottom = sorted(dists)[:4]
            ed.check(max(bottom) <= 1e-3, f"its bottom face lies on the plane: {dists}")
        await ed.call("delete_entities", scene=sid, entities=[inst])


async def test_drop_refused_while_pending(ed: Editor):
    s = await setup(ed)
    ui = s["ui"]
    sid = await new_scene(ed, "DropPending")
    lattice_obj(ed.project / "assets" / "models" / "slab.obj", 300, True)
    if not (ed.project / "prefabs" / "Crate.prefab").exists():
        crate = await cube(ed, sid, "Crate", (50, 0, 0))
        await ed.call("create_prefab", scene=sid, entity=crate, path="prefabs/Crate.prefab")
        await ed.call("delete_entities", scene=sid, entities=[crate])
    await ed.call("sync_files")
    await ed.call("set_scene_camera", scene=sid, position=[0, 0, 6], target=[0, 0, 0])
    # the Project window shows the prefab first: the slab's tree is pending from its load on
    item = await project_item(ed, ui, "Crate.prefab")
    await ed.call("select_scene", scene=sid)
    await ed.call("create_entity", scene=sid, type="model", name="Slab", model="assets/models/slab.obj", select=False)
    rect, _ = await view(ed, sid)      # loaded and drawn; its BVH is held pending
    state = await scene_view_state(ed, sid)
    ed.check(state["bvh_cache"]["pending"] >= 1, f"the slab's tree is still building: {state['bvh_cache']}")
    await ui.drag(ui.at(item, (item["width"] / 2, item["height"] / 2)), uv(ui, rect, 0.5, 0.5))
    message = await status_message(ed, "Preparing surface")
    await ed.screenshot("editor", path=str(s["out"] / "s26-drop-refused.png"))
    found = await ed.call("find_entities", scene=sid, name="Crate")
    after = (await scene_view_state(ed, sid))["bvh_cache"]
    ed.check(not found["entities"], f"no drop onto a surface whose BVH builds: {found}; the cache before {state['bvh_cache']}, after {after}")
    ed.check("Preparing surface" in message, f"the status bar says so: {message!r}")


# ---------------------------------------------------------------------------- R33


async def test_click_waits_for_bvh(ed: Editor):
    s = await setup(ed)
    ui = s["ui"]
    sid = await new_scene(ed, "Waiting")
    lattice_obj(ed.project / "assets" / "models" / "grid.obj", 320, True)
    await ed.call("sync_files")
    await ed.call("set_scene_camera", scene=sid, position=[0, 0, 6], target=[0, 0, 0])
    made = await ed.call("create_entity", scene=sid, type="model", name="Grid", model="assets/models/grid.obj", select=False)
    grid = made["entity_id"]
    rect, _ = await view(ed, sid)
    await ed.call("select_entities", scene=sid, entities=[])
    state = await scene_view_state(ed, sid)
    ed.check(state["bvh_cache"]["pending"] >= 1, f"the grid's tree is still building: {state['bvh_cache']}")
    await ui.click(uv(ui, rect, 0.5, 0.5))
    waiting = None
    for _ in range(20):
        waiting = (await scene_view_state(ed, sid)).get("pending_click")
        if waiting:
            break
        await asyncio.sleep(0.1)
    message = await status_message(ed, "Preparing picking for Grid")
    await ed.screenshot("editor", path=str(s["out"] / "s26-click-waiting.png"))
    ed.check(waiting is not None and "Grid" in waiting, f"the click waits for the grid's tree: {waiting!r}, selected {await selected(ed, sid)}")
    ed.check("Preparing picking for Grid" in message, f"the status bar after 100 ms: {message!r}")
    ed.check_eq(await selected(ed, sid), [], "nothing selected through the mesh's bounds meanwhile")
    picked = []
    for _ in range(480):
        await asyncio.sleep(0.25)
        picked = await selected(ed, sid)
        if picked:
            break
    ed.check_eq(picked, [grid], "the click selects once the tree is built")


# ---------------------------------------------------------------------------- AC12, R20


async def test_bookmark_shortcuts(ed: Editor):
    s = await setup(ed)
    ui = s["ui"]
    sid = await new_scene(ed, "MarkKeys")
    await cube(ed, sid, "Box", (0, 0, 0))
    await ed.call("set_scene_camera", scene=sid, position=[2, 3, 9], target=[0, 0, 0])
    rect, _ = await view(ed, sid)
    await ui.move(uv(ui, rect, 0.3, 0.3))
    await ed.call("focus_window", window="scene", scene=sid)
    await ui.key("ctrl+shift+2")
    listed = await ed.call("edit_scene_view_bookmarks", scene=sid)
    ed.check([b for b in listed["bookmarks"] if b["slot"] == 2][0]["defined"], "Ctrl+Shift+2 stores slot 2")
    await ed.call("set_scene_camera", scene=sid, position=[-8, 1, -3], target=[1, 1, 1])
    await ui.key("shift+2")
    await asyncio.sleep(0.5)    # Camera Easing: 0.25 s
    cam = await camera(ed, sid)
    ed.check(all(abs(a - b) < 1e-3 for a, b in zip(cam["position"], [2, 3, 9])), f"Shift+2 recalls it: {cam}")


async def test_pane_under_pointer_is_active(ed: Editor):
    s = await setup(ed)
    ui = s["ui"]
    sid = await new_scene(ed, "Panes")
    await cube(ed, sid, "Box", (0, 0, 0))
    rect, _ = await view(ed, sid)        # the whole canvas while Single
    await ed.call("set_scene_view_layout", scene=sid, layout="quad")
    await ed.call("wait", frames=4)
    # Quad: Top (pane 1) top left, Front (2) top right, Side (3) bottom left, Persp (0) bottom right
    for (u, v), pane in (((0.25, 0.3), 1), ((0.75, 0.3), 2), ((0.25, 0.8), 3), ((0.75, 0.8), 0)):
        await ui.move(uv(ui, rect, u, v))
        await ed.call("wait", frames=3)
        layout = await ed.call("set_scene_view_layout", scene=sid)
        ed.check_eq(layout["active_pane"], pane, f"the pane under ({u}, {v})")
    await ed.screenshot("editor", path=str(s["out"] / "s26-quad.png"))
    await ed.call("set_scene_view_layout", scene=sid, layout="single")
