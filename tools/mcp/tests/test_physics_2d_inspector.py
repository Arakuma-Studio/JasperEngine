"""The 2D physics Inspector driven like a human (xdotool in the harness's Xvfb, widgets found through the
UI probe, editor/util/UIProbe.h), for the physics-2d review fixes in editor/window/InspectorPhysics2D.cpp:

- each collider's widgets have their own ImGui IDs: Add Point of collider 2 adds a point to collider 2;
- the joint axis Angle drag is one undo step per drag (it ended its step at release);
- a physics material's Friction and Bounciness drags are separate project undo steps.
"""

from __future__ import annotations

import asyncio
import shutil
import time

from harness import Editor
from ui_input import Ui as FrameUi

EDITOR = {"create": True, "name": "Physics 2D Inspector", "xvfb": "harness", "ui_probe": True}
CI = True
TIMEOUT = 900


class Ui(FrameUi):
    async def click(self, x: float, y: float, double: bool = False):
        if double:
            await super().double_click((x, y))
        else:
            await super().click((x, y))

    def state(self):
        probe = self.ed.ui_probe_state()
        return probe.get("items", {}), probe.get("scroll", {}).get("inspector", [0.0, 0.0])

    async def settled(self):
        last, same = None, 0
        for _ in range(60):
            await self.frames(2)
            now = self.state()
            same = same + 1 if now == last else 0
            if same >= 2:
                return now
            last = now
        self.ed.fail(f"the Inspector never settles: {last}")

    async def reveal(self, name: str):
        """Wheel notches down the Inspector (from its top) until `name` is drawn whole"""
        await self.frames()
        panel = await self.ed.ui_rect("inspector")
        self.ed.check(panel, "the inspector is drawn")
        at = ((panel[0] + panel[2]) / 2, (panel[1] + panel[3]) / 2)
        self.xdo("mousemove", str(int(at[0])), str(int(at[1])))
        for _ in range(60):
            self.xdo("click", "4")
        items, scroll = await self.settled()
        for _ in range(200):
            if name in items:
                return items[name]
            if scroll[0] >= scroll[1]:
                break
            before = scroll[0]
            self.xdo("click", "5")
            for _ in range(60):
                await self.frames(2)
                items, scroll = self.state()
                if scroll[0] != before:
                    break
            items, scroll = await self.settled()
        self.ed.fail(f"{name} is never drawn (Inspector scroll {scroll}): {sorted(items)}")

    async def project_file(self, name: str, folders=("assets", "physics")):
        await self.ed.call("focus_window", window="project")
        # by the clock: frames can run far ahead of the window's first listing of its folder
        deadline = time.monotonic() + 30.0
        while time.monotonic() < deadline:
            await self.frames(4)
            await asyncio.sleep(0.1)
            items = self.ed.ui_items()
            rect = items.get("project.file." + name)
            if rect:
                return rect
            for folder in reversed(folders):
                if "project.file." + folder in items:
                    await self.click(*center(items["project.file." + folder]), double=True)
                    break
        self.ed.fail(f"the Project window never lists {name}: {sorted(self.ed.ui_items())}")


def center(rect):
    return (rect[0] + rect[2]) / 2.0, (rect[1] + rect[3]) / 2.0


S: dict = {}


async def test_add_point_acts_on_its_own_collider(ed: Editor):
    if not shutil.which("xdotool"):
        ed.fail("xdotool is needed for the real-input UI tests (apt install xdotool)")
    S["scene"] = (await ed.call("create_scene", name="Inspector2D", type="2D"))["scene"]["id"]
    await ed.call("wait", frames=10)

    ui = Ui(ed)
    sid = S["scene"]
    body = (await ed.call("create_entity", scene=sid, name="Two", type="rigidbody2d"))["entity_id"]
    box = [[-10, -10], [10, -10], [10, 10], [-10, 10]]
    await ed.call("edit_colliders", scene=sid, entity=body, body_type="static",
                  add=[{"shape": "polygon", "points": box}, {"shape": "polygon", "points": [[x + 40, y] for x, y in box]}])
    await ed.call("select_entities", scene=sid, entities=[body])
    await ed.call("focus_window", window="inspector")
    rect = await ui.reveal("physics2d.collider.1.add_point")
    await ui.click(*center(rect))
    await ui.frames(5)
    colliders = (await ed.call("edit_colliders", scene=sid, entity=body))["colliders"]
    ed.check_eq(len(colliders[1]["points"]), 5, f"collider 2's Add Point added to collider 2: {colliders}")
    ed.check_eq(len(colliders[0]["points"]), 4, f"collider 1 untouched: {colliders}")


async def test_axis_angle_drag_is_one_undo_step(ed: Editor):
    ui = Ui(ed)
    sid = S["scene"]
    for name, position in (("Car", [0, 200, 0]), ("Wheel", [0, 160, 0])):
        entity = (await ed.call("create_entity", scene=sid, name=name, type="rigidbody2d", position=position))["entity_id"]
        await ed.call("edit_colliders", scene=sid, entity=entity, body_type="dynamic", add=[{"shape": "circle", "radius": 10, "fit": False}])
    joint = (await ed.call("set_joint", scene=sid, name="Suspension", type="wheel", body_a="Car", body_b="Wheel", axis=[0, 1]))["joint"]["id"]

    async def axis():
        got = await ed.call("get_entity", scene=sid, entity=joint, components=["joint2d"], properties=["axis"])
        return got["components"]["joint2d"]["axis"]

    await ed.call("select_entities", scene=sid, entities=[joint])
    await ed.call("focus_window", window="inspector")
    rect = await ui.reveal("physics2d.joint.axis_angle")
    start = center(rect)
    await ui.drag(start, (start[0] + 60, start[1]))
    first = await axis()
    ed.check(abs(first[0]) > 0.05, f"the first drag turned the axis: {first}")
    rect = await ui.reveal("physics2d.joint.axis_angle")
    start = center(rect)
    await ui.drag(start, (start[0] + 60, start[1]))
    second = await axis()
    ed.check(abs(second[0] - first[0]) > 0.05, f"the second drag turned it further: {first} -> {second}")
    await ed.call("undo", scene=sid)
    after = await axis()
    ed.check(abs(after[0] - first[0]) < 1e-3 and abs(after[1] - first[1]) < 1e-3,
             f"one undo takes back the second drag only: {after} (first {first}, second {second})")


async def test_material_properties_are_separate_undo_steps(ed: Editor):
    ui = Ui(ed)
    await ed.call("edit_physics_material", path="assets/physics/Rubber.physicsmaterial", create="2d")
    await ed.call("sync_files")
    rect = await ui.project_file("Rubber.physicsmaterial")
    for _ in range(4):
        await ui.click(*center(rect))
        await ui.frames(5)
        if "physics2d.material.friction" in ed.ui_items():
            break
        rect = await ui.project_file("Rubber.physicsmaterial")
    items = ed.ui_items()
    ed.check("physics2d.material.friction" in items, f"the material asset view: {sorted(items)}")

    def values():
        text = (ed.project / "assets" / "physics" / "Rubber.physicsmaterial").read_text()
        import json
        data = json.loads(text)
        return data["friction"], data["bounciness"]

    before = values()
    friction = center(items["physics2d.material.friction"])
    await ui.drag(friction, (friction[0] + 80, friction[1]))
    items = ed.ui_items()
    bounce = items["physics2d.material.bounciness"]
    # a click right of the slider's middle: bounciness above 0.5
    await ui.click(bounce[0] + (bounce[2] - bounce[0]) * 0.8, (bounce[1] + bounce[3]) / 2)
    await ui.frames(5)
    edited = values()
    ed.check(edited[0] != before[0] and edited[1] > 0.5, f"both changed: {before} -> {edited}")
    await ed.call("undo", scope="project")
    await ui.frames(3)
    undone = values()
    ed.check(undone[1] == before[1] and undone[0] == edited[0], f"one undo takes back Bounciness only: {undone} (edited {edited})")
