"""The Vertex Animation parts of the editor (vertex animation spec 2.1, 3.3, 3.6, 5.2-5.4; editor/window/
InspectorWindowVat.cpp, editor/window/ProjectWindow.cpp, editor/vat/), driven like a human with xdotool in the
harness's Xvfb on a new project holding the quad bake of editor/tests/fixtures/vat (two clips, one socket):

- the Project window: the .vat.glb's "generated" badge; a .vat dragged into the Scene view makes one GameObject
  (Model = the .vat.glb, Vertex Animator = the .vat), one scene undo step;
- the Vertex Animator section: Preview (the clock runs in edit mode, stops on another selection), the time
  scrubber, the clip dropdown, a mesh_mismatch with its one-click fix (undoable);
- the .vat asset view after a Project window click: the header, an Interpolate click and a clip rename write the
  file through the project history, the events list adds an event, Rebake / Open in Baker open the Vertex
  Animation Baker on the recipe (a Rebake whose source is missing writes nothing), Create Crowd makes one GameObject with its instances in one scene step;
- the pair through move_asset / delete_asset (the mesh field follows), and an outside edit of a .vat reported in
  external_changes.problems.

Widgets are found through the UI probe (Editor(ui_probe=True), editor/util/UIProbe.h; Project window files are
"project.file.<name>"). The Inspector is scrolled with real wheel notches, each one waited for through the
probe's Inspector scroll ([y, max_y]: a notch arrives some frames after xdotool sends it, so an unmoved
widget never means the end of the scroll) and the layout let settle. Screenshots go to $JASPER_SHOTS_DIR
when set (else the scratch directory).
"""

from __future__ import annotations

import asyncio
import json
import os
import shutil
import subprocess
import time
from pathlib import Path

from harness import Editor, REPO

EDITOR = {"create": True, "name": "VAT Inspector", "xvfb": "harness", "ui_probe": True}
CI = True
TIMEOUT = 900

FIXTURE = REPO / "editor" / "tests" / "fixtures" / "vat"
S: dict = {}


class Ui:
    def __init__(self, ed: Editor):
        self.ed = ed
        self.env = dict(os.environ, DISPLAY=ed._xvfb.display)

    def xdo(self, *args: str) -> str:
        return subprocess.run(["xdotool", *args], env=self.env, capture_output=True, text=True, timeout=20).stdout

    async def click(self, x: float, y: float, double: bool = False):
        self.xdo("mousemove", str(int(x)), str(int(y)))
        await asyncio.sleep(0.3)
        self.xdo("click", "--repeat", "2" if double else "1", "--delay", "80", "1")
        await asyncio.sleep(0.6)

    async def drag(self, start, end, steps: int = 12):
        self.xdo("mousemove", str(int(start[0])), str(int(start[1])))
        await asyncio.sleep(0.3)
        self.xdo("mousedown", "1")
        await asyncio.sleep(0.2)
        for i in range(1, steps + 1):
            x = start[0] + (end[0] - start[0]) * i / steps
            y = start[1] + (end[1] - start[1]) * i / steps
            self.xdo("mousemove", str(int(x)), str(int(y)))
            await asyncio.sleep(0.08)
        await asyncio.sleep(0.3)
        self.xdo("mouseup", "1")
        await asyncio.sleep(0.6)

    async def type_text(self, text: str):
        self.xdo("key", "ctrl+a")
        await asyncio.sleep(0.1)
        self.xdo("type", "--delay", "40", text)
        await asyncio.sleep(0.2)
        self.xdo("key", "Return")
        await asyncio.sleep(0.5)

    async def wheel(self, at, down: bool, notches: int):
        self.xdo("mousemove", str(int(at[0])), str(int(at[1])))
        await asyncio.sleep(0.2)
        for _ in range(notches):
            self.xdo("click", "5" if down else "4")
        await asyncio.sleep(0.3)

    async def frames(self, n: int = 3):
        # find_ui_item answers two frames later; wait is refused under Create Crowd, a modal (S69)
        for _ in range((n + 1) // 2):
            await self.ed.call("find_ui_item", label="##frame pacer")

    def state(self) -> tuple[dict, list[float]]:
        """The UI probe's widgets and the Inspector's scroll [y, max_y] of the last frame"""
        probe = self.ed.ui_probe_state()
        return probe.get("items", {}), probe.get("scroll", {}).get("inspector", [0.0, 0.0])

    async def settled(self, limit: float = 30.0) -> tuple[dict, list[float]]:
        """The widgets and scroll once three polls in a row (2 frames apart) agree: the layout is done
        and the last wheel notch applied"""
        last, same = None, 0
        deadline = time.monotonic() + limit
        while time.monotonic() < deadline:
            await self.frames(2)
            now = self.state()
            same = same + 1 if now == last else 0
            if same >= 2:
                return now
            last = now
        self.ed.fail(f"the Inspector never settles: {last}")

    async def notch(self, at, down: bool, scroll: list[float], notches: int = 1, until=None,
                    limit: float = 30.0) -> tuple[dict, list[float]]:
        """Wheel notches, then the state once the Inspector's scroll has moved (or until(scroll) holds).
        xdotool's notches reach the editor some frames later (more under load): waited for, never
        assumed applied."""
        await self.wheel(at, down, notches)
        deadline = time.monotonic() + limit
        while time.monotonic() < deadline:
            await self.frames(2)
            now = self.state()[1]
            if (until(now) if until else now[0] != scroll[0]):
                return await self.settled()
        self.ed.fail(f"{notches} wheel notch(es) {'down' if down else 'up'} never scrolled the Inspector from {scroll}")

    async def reveal(self, name: str, top: bool = True) -> list[float]:
        """Scrolls the Inspector with real wheel notches until `name` is drawn whole near its top, or as
        near as the Inspector scrolls (its scroll at the end), once the layout has settled. top: from
        the Inspector's top when `name` is not drawn."""
        await self.frames()
        panel = await self.ed.ui_rect("inspector")
        self.ed.check(panel, "the inspector is drawn")
        at = ((panel[0] + panel[2]) / 2, (panel[1] + panel[3]) / 2)
        items, scroll = await self.settled()
        if top and name not in items and scroll[0] > 0:
            items, scroll = await self.notch(at, False, scroll, notches=80, until=lambda s: s[0] <= 0)
        seen = False
        for _ in range(200):
            rect = items.get(name)
            at_end = scroll[0] >= scroll[1]
            if rect:
                seen = True
                if rect[1] < panel[1] + 60 or at_end:
                    return rect
            elif seen:
                # the last notch scrolled it past the top: one back draws it whole again
                items, scroll = await self.notch(at, False, scroll)
                rect = items.get(name)
                self.ed.check(rect, f"{name} is drawn again one notch back: {sorted(items)}")
                return rect
            elif at_end:
                break
            items, scroll = await self.notch(at, True, scroll)
        self.ed.fail(f"{name} is never drawn (Inspector scroll {scroll}): {sorted(items)}")

    async def project_file(self, name: str, folders=("assets", "vat")) -> list[float]:
        """The Project window's entry `name` of assets/vat (opening the folders by double-click)"""
        await self.ed.call("focus_window", window="project")
        for _ in range(12):
            await self.frames(4)
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


async def shot(ed: Editor, name: str):
    shots = Path(os.environ["JASPER_SHOTS_DIR"]) if os.environ.get("JASPER_SHOTS_DIR") else ed.scratch.root / "shots"
    shots.mkdir(parents=True, exist_ok=True)
    # find_ui_item answers two frames later; wait is refused under Create Crowd, a modal (S69)
    for _ in range(2):
        await ed.call("find_ui_item", label="##frame pacer")
    await ed.screenshot("editor", path=str(shots / f"{name}.png"), max_width=1600)


async def lua(ed: Editor, code: str):
    return (await ed.call("eval_lua", code=code, allow_edit_mode=True))["values"]


async def animator(ed: Editor, name: str) -> dict:
    got = await ed.call("get_entity", entity=name, components=["vertexanimator", "model"])
    return got.get("components", got)


def vat_text(ed: Editor, name: str = "quad.vat") -> dict:
    return json.loads((ed.project / "assets" / "vat" / name).read_text())


async def test_setup(ed: Editor):
    if not shutil.which("xdotool"):
        ed.fail("xdotool is needed for the real-input UI tests (apt install xdotool)")
    vat = ed.project / "assets" / "vat"
    vat.mkdir(parents=True, exist_ok=True)
    for name in ("quad.vat", "quad.vat.glb"):
        shutil.copyfile(FIXTURE / name, vat / name)
    # a plain model (the same mesh under another name) for the mesh_mismatch case
    shutil.copyfile(FIXTURE / "quad.vat.glb", vat / "plain.glb")
    await ed.call("sync_files")
    await ed.call("create_scene", name="Crowds", type="3d")
    await ed.call("wait", frames=10)


async def test_generated_badge_and_drop_into_the_scene(ed: Editor):
    ui = Ui(ed)
    await ui.project_file("quad.vat.glb")
    badge = await ed.ui_rect("project.generated.quad.vat.glb")
    ed.check(badge, f"the .vat.glb shows the generated badge: {sorted(ed.ui_items())}")
    ed.check("project.generated.plain.glb" not in ed.ui_items(), "a plain model has no badge")
    ed.check("project.generated.quad.vat" not in ed.ui_items(), "the .vat itself is not generated")
    await shot(ed, "01-project-badge")

    # drag the .vat into the Scene view: one GameObject, one scene step
    source = await ui.project_file("quad.vat")
    await ed.call("focus_window", window="scene")
    await ui.frames(5)
    view = await ed.ui_rect("scene.view")
    ed.check(view, f"the Scene view is drawn: {sorted(ed.ui_items())}")
    source = await ui.project_file("quad.vat")
    await ui.drag(center(source), center(view))
    await ui.frames(10)
    got = await animator(ed, "quad")
    ed.check("vertexanimator" in got, f"the dropped .vat made a Vertex Animator: {json.dumps(got)[:600]}")
    ed.check("vat/quad.vat" in json.dumps(got["vertexanimator"]), f"animation: {json.dumps(got)[:600]}")
    ed.check("vat/quad.vat.glb" in json.dumps(got["model"]), f"the Model is the .vat.glb: {json.dumps(got)[:600]}")
    await shot(ed, "02-dropped-into-the-scene")
    await ed.call("undo")
    found = await ed.call("find_entities", name="quad")
    ed.check("quad" not in json.dumps(found.get("entities", found)), f"one undo removes it: {json.dumps(found)[:300]}")
    await ed.call("redo")
    got = await animator(ed, "quad")
    ed.check("vertexanimator" in got, "redo brings it back")


async def test_animator_section_preview_and_scrub(ed: Editor):
    ui = Ui(ed)
    await ed.call("select_entities", entities=["quad"])
    await ed.call("focus_window", window="inspector")
    # the redo of the previous test loads the GameObject's model and animation again: the section's
    # layout is final once they are
    loaded = False
    for _ in range(80):
        if (await lua(ed, 'return object("quad"):getVertexAnimator():isLoaded()')) == [True]:
            loaded = True
            break
        await ui.frames(2)
    ed.check(loaded, "the Vertex Animator loads")
    rect = await ui.reveal("vat.animation")
    print(f"[ui] vat.animation at {rect}, Inspector scroll {ui.state()[1]}")
    for name in ("vat.clip", "vat.preview", "vat.addAttachment"):
        ed.check(name in ed.ui_items(), f"{name} drawn: {sorted(ed.ui_items())}")
    await shot(ed, "03-animator-section")
    values = await lua(ed, 'local va = object("quad"):getVertexAnimator(); return va.preview, va.currentClip')
    ed.check_eq(values[0], False, "no preview at first")

    # Preview: the clock runs in edit mode
    rect = await ui.reveal("vat.preview")
    await ui.click(*center(rect))
    await ui.frames(10)
    first = await lua(ed, 'local va = object("quad"):getVertexAnimator(); return va.preview, va.time')
    ed.check_eq(first[0], True, "Preview on")
    await asyncio.sleep(0.5)
    await ui.frames(10)
    second = await lua(ed, 'return object("quad"):getVertexAnimator().time')
    ed.check(second[0] != first[1], f"the preview plays: {first} then {second}")
    scrub = await ui.reveal("vat.scrub", top=False)
    ed.check(scrub, "the scrubber shows while previewing")
    await shot(ed, "04-preview-scrubber")
    # nearly still (speed 0.001), a drag to the scrubber's right end then its left end sets the time
    await lua(ed, 'object("quad"):getVertexAnimator().speed = 0.001; return true')
    mid = (scrub[1] + scrub[3]) / 2
    await ui.drag(((scrub[0] + scrub[2]) / 2, mid), (scrub[0] + (scrub[2] - scrub[0]) * 0.9, mid), steps=6)
    await shot(ed, "04b-scrub-late")
    late = await lua(ed, 'local va = object("quad"):getVertexAnimator(); return va.time, va.preview, va.currentClip, va.speed')
    ed.check(0.8 < late[0] < 0.97, f"the scrubber set a late time: {late} (scrubber {scrub}, items {ed.ui_items().get('vat.scrub')})")
    await ui.drag(((scrub[0] + scrub[2]) / 2, mid), (scrub[0] + (scrub[2] - scrub[0]) * 0.1, mid), steps=6)
    early = await lua(ed, 'return object("quad"):getVertexAnimator().time')
    ed.check(0.03 < early[0] < 0.2, f"the scrubber set an early time: {early}")
    await lua(ed, 'object("quad"):getVertexAnimator().speed = 1; return true')

    # another selection stops it (the Scene view goes back to where it started)
    await ed.call("select_entities", entities=[])
    await ui.frames(5)
    stopped = await lua(ed, 'local va = object("quad"):getVertexAnimator(); return va.preview, va.time')
    ed.check_eq(stopped[0], False, "the preview stops with another selection")
    await ed.call("select_entities", entities=["quad"])
    rect = await ui.reveal("vat.preview")
    await ui.click(*center(rect))
    await ui.frames(5)
    ed.check_eq((await lua(ed, 'return object("quad"):getVertexAnimator().preview'))[0], True, "on again")
    await ui.click(*center(await ui.reveal("vat.preview")))
    await ui.frames(5)
    ed.check_eq((await lua(ed, 'return object("quad"):getVertexAnimator().preview'))[0], False, "the button stops it")

    # the clip dropdown: Idle as the default clip, one scene step
    rect = await ui.reveal("vat.clip")
    await ui.click(*center(rect))
    option = await ed.ui_rect("vat.clip.option.Idle")
    ed.check(option, f"the dropdown lists Idle: {sorted(ed.ui_items())}")
    await ui.click(*center(option))
    await ui.frames(5)
    got = await animator(ed, "quad")
    ed.check_eq(got["vertexanimator"].get("defaultClip"), "Idle", f"the clip dropdown: {json.dumps(got)[:400]}")
    await ed.call("undo")
    got = await animator(ed, "quad")
    ed.check_eq(got["vertexanimator"].get("defaultClip", ""), "", "undo")



async def test_mesh_mismatch_fix(ed: Editor):
    ui = Ui(ed)
    await ed.call("set_model", path="assets/vat/plain.glb", create={"name": "Mismatch", "position": [2, 0, 0]})
    await ed.call("add_component", entity="Mismatch", component="vertexanimator",
                  values={"vertexanimator": {"animation": "assets/vat/quad.vat"}})
    await ed.call("select_entities", entities=["Mismatch"])
    await ed.call("focus_window", window="inspector")
    await ui.frames(30)
    error = await lua(ed, 'return object("Mismatch"):getVertexAnimator():getLoadError()')
    ed.check("mesh_mismatch" in error[0], f"the animator reports the mismatch: {error}")
    rect = await ui.reveal("vat.meshFix", top=False)
    ed.check("vat.loadError" in ed.ui_items(), "the load error is shown")
    await shot(ed, "05-mesh-mismatch")
    await ui.click(*center(rect))
    await ui.frames(30)
    got = await animator(ed, "Mismatch")
    ed.check("vat/quad.vat.glb" in json.dumps(got["model"]), f"the fix set the Model: {json.dumps(got)[:400]}")
    error = await lua(ed, 'return object("Mismatch"):getVertexAnimator():getLoadError()')
    ed.check_eq(error[0], "", "no mismatch after the fix")
    ed.check("vat.meshFix" not in ed.ui_items(), "the fix button is gone")
    await ed.call("undo")
    await ui.frames(30)
    got = await animator(ed, "Mismatch")
    ed.check("plain.glb" in json.dumps(got["model"]), f"undo puts the plain model back: {json.dumps(got)[:400]}")


async def test_vat_asset_view(ed: Editor):
    ui = Ui(ed)
    rect = await ui.project_file("quad.vat")
    for _ in range(4):
        await ui.click(*center(rect))
        await ui.frames(5)
        if "vatAsset.mode" in ed.ui_items():
            break
        rect = await ui.project_file("quad.vat")
    ed.check("vatAsset.mode" in ed.ui_items(), f"the .vat asset view: {sorted(ed.ui_items())}")
    for name in ("vatAsset.texture", "vatAsset.rebake", "vatAsset.openBaker", "vatAsset.createCrowd",
                 "vatAsset.clip.0.name", "vatAsset.clip.1.interpolate", "vatAsset.socket.0"):
        ed.check(name in ed.ui_items(), f"{name} drawn: {sorted(ed.ui_items())}")
    await shot(ed, "06-vat-asset")

    # Interpolate off: the file changes through the project history
    ed.check_eq(vat_text(ed)["clips"][1]["interpolate"], True, "interpolated at first")
    await ui.click(*center(ed.ui_items()["vatAsset.clip.1.interpolate"]))
    await ui.frames(5)
    ed.check_eq(vat_text(ed)["clips"][1]["interpolate"], False, "the checkbox wrote the file")
    await ed.call("undo", scope="project")
    await ui.frames(5)
    ed.check_eq(vat_text(ed)["clips"][1]["interpolate"], True, "project undo")

    # a clip renamed by typing
    rect = await ui.reveal("vatAsset.clip.1.name")
    await ui.click(*center(rect), double=True)
    await ui.type_text("Rest")
    await ui.frames(5)
    ed.check_eq(vat_text(ed)["clips"][1]["name"], "Rest", f"renamed: {vat_text(ed)['clips']}")

    # the events of Rest: + Add Event
    rect = await ui.reveal("vatAsset.clip.1.events")
    await ui.click(*center(rect))
    rect = await ui.reveal("vatAsset.addEvent", top=False)
    await ui.click(*center(rect))
    await ui.frames(5)
    events = vat_text(ed)["clips"][1].get("events", [])
    ed.check(events and events[0]["function"] == "onEvent", f"an event added: {events}")
    await shot(ed, "07-vat-asset-events")
    await ed.call("undo", scope="project")
    await ed.call("undo", scope="project")
    await ui.frames(5)
    ed.check_eq(vat_text(ed)["clips"][1]["name"], "Idle", "both undone")

    # Rebake runs the recipe in the Vertex Animation Baker: quad.vat's source (models/quad.gltf) is not in
    # this project, so the baker opens on the recipe and writes nothing (it says why)
    before = (ed.project / "assets" / "vat" / "quad.vat").read_text()
    rect = await ui.reveal("vatAsset.rebake")
    await ui.click(*center(rect))
    await ui.frames(10)
    ed.check(await ed.ui_rect("vatbaker.source"), f"Rebake opened the baker: {sorted(ed.ui_items())[:60]}")
    ed.check_eq((ed.project / "assets" / "vat" / "quad.vat").read_text(), before, "a Rebake whose source is missing writes nothing")
    # Open in Baker opens it too (the recipe loaded)
    rect = await ui.project_file("quad.vat")
    await ui.click(*center(rect))
    rect = await ui.reveal("vatAsset.openBaker")
    await ui.click(*center(rect))
    await ui.frames(10)
    ed.check(await ed.ui_rect("vatbaker.source"), f"Open in Baker opened the baker: {sorted(ed.ui_items())[:60]}")


async def test_create_crowd(ed: Editor):
    ui = Ui(ed)
    rect = await ui.project_file("quad.vat")
    await ui.click(*center(rect))
    rect = await ui.reveal("vatAsset.createCrowd")
    await ui.click(*center(rect))
    create = await ed.ui_rect("crowd.create")
    ed.check(create, f"the Create Crowd dialog: {sorted(ed.ui_items())}")
    await shot(ed, "08-create-crowd")
    await ui.click(*center(create))
    await ui.frames(10)
    got = await ed.call("get_entity", entity="quad Crowd", components=["instancedmesh", "vertexanimator"])
    comps = got.get("components", got)
    ed.check("instancedmesh" in comps and "vertexanimator" in comps, f"the crowd GameObject: {json.dumps(got)[:600]}")
    count = (await lua(ed, 'return object("quad Crowd", "Mesh"):getNumInstances()'))[0]
    ed.check(40 <= count <= 100, f"its instances: {count}")
    await shot(ed, "09-crowd")
    await ed.call("undo")
    found = await ed.call("find_entities", name="quad Crowd")
    ed.check("quad Crowd" not in json.dumps(found.get("entities", found)), "one undo removes the crowd")


async def test_move_and_delete_keep_the_pair(ed: Editor):
    await ed.call("move_asset", path="assets/vat/quad.vat", to="assets/vat/walker.vat")
    vat = ed.project / "assets" / "vat"
    ed.check((vat / "walker.vat").exists() and (vat / "walker.vat.glb").exists(), "the pair was renamed together")
    ed.check(not (vat / "quad.vat.glb").exists(), "no mesh left behind")
    ed.check_eq(vat_text(ed, "walker.vat")["mesh"], "walker.vat.glb", "the mesh field follows")
    got = await animator(ed, "quad")
    ed.check("vat/walker.vat" in json.dumps(got["vertexanimator"]), f"the animator follows: {json.dumps(got)[:400]}")
    ed.check("vat/walker.vat.glb" in json.dumps(got["model"]), f"the Model follows: {json.dumps(got)[:400]}")
    await ed.call("undo", scope="project")
    ed.check((vat / "quad.vat").exists() and (vat / "quad.vat.glb").exists(), "undo renames both back")

    await ed.call("delete_asset", paths=["assets/vat/quad.vat"])
    ed.check(not (vat / "quad.vat").exists() and not (vat / "quad.vat.glb").exists(), "one delete for both")
    await ed.call("undo", scope="project")
    ed.check((vat / "quad.vat").exists() and (vat / "quad.vat.glb").exists(), "undo restores both")


async def test_outside_edit_is_reported(ed: Editor):
    path = ed.project / "assets" / "vat" / "quad.vat"
    good = path.read_text()
    time.sleep(0.05)
    path.write_text('{\n  "format": "jasperengine.vat",\n  oops\n}\n')
    r = await ed.call_raw("editor_status")
    ext = (r.structured if isinstance(r.structured, dict) else {}).get("external_changes") or {}
    found = [p for p in ext.get("problems", []) if p.get("path") == "assets/vat/quad.vat"]
    ed.check(found, f"the broken .vat is reported: {ext}")
    ed.check("Not a valid Vertex Animation" in found[0].get("message", ""), f"message: {found[0]}")
    ed.check_eq(found[0].get("line"), 3, f"line: {found[0]}")
    time.sleep(0.05)
    path.write_text(good)
    # the watcher's batch of the broken write may still be on its way (read before the fix was
    # written): the next reports, as soon as they have the fixed file, have no problem for it
    reports = []
    for _ in range(20):
        r = await ed.call_raw("editor_status")
        ext = (r.structured if isinstance(r.structured, dict) else {}).get("external_changes") or {}
        reports.append(ext)
        if not [p for p in ext.get("problems", []) if p.get("path") == "assets/vat/quad.vat"]:
            break
        await asyncio.sleep(0.25)
    ed.check(not [p for p in reports[-1].get("problems", []) if p.get("path") == "assets/vat/quad.vat"], f"fixed: {reports}")
    error = await lua(ed, 'return object("quad"):getVertexAnimator():getLoadError()')
    ed.check_eq(error[0], "", "the animator plays the fixed file")
