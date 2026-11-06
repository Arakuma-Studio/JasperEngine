"""The Inspector's animation clip sections (animation clips spec 4.2, 9.R30; editor/window/InspectorWindowClips.cpp),
driven like a human with xdotool in the harness's Xvfb on a scratch copy of tools/fixtures/pose-animation:

- an Animation Player on the animated hero with an .anim clip (one misspelled path, one event), the `#Walk`
  sub-clip with wrap loop and a clip deleted from disk: the section draws, + Add Clip is one scene undo step,
  the row menu's Remove is one step too;
- the Model section: Use Animation Player (T2d's hook) turns the entity animations off;
- the live states while playing;
- the .anim asset section after a Project window click: the Length Auto checkbox writes the file (project undo);
  a programmatic selection change and a click on a GameObject in the Scene view show the selection again (R30);
- a model's clips as an asset (the Animation tab of its Import Settings, S53 R16); the AnimationComponent's
  Convert to Clip button.

Widgets are found through the UI probe (Editor(ui_probe=True), editor/util/UIProbe.h). The Project window's
tree and listing are clicked at the rects editor_status reports (ui.project_window).
Screenshots go to $JASPER_SHOTS_DIR when set (else the scratch directory).
"""

from __future__ import annotations

import asyncio
import json
import os
import subprocess
from pathlib import Path

from harness import Editor, REPO

EDITOR = {"project": REPO / "tools" / "fixtures" / "pose-animation", "xvfb": "harness", "ui_probe": True}
CI = True
TIMEOUT = 900

GUID = "0a1b2c3d4e5f60718293a4b5c6d7e8f9"
HIPS = "Hero/mixamorig:Hips"
SPINE = HIPS + "/mixamorig:Spine"
WAVE = {
    "format": "jasperengine.anim", "version": 1, "guid": GUID, "frame_rate": 30, "wrap": "loop",
    "tracks": [
        {"path": HIPS, "channel": "rotation", "keys": [[0, [0, 0, 0, 1]], [0.5, {"euler": [0, 30, 0]}], [1, [0, 0, 0, 1]]]},
        {"path": SPINE, "channel": "rotation", "keys": [[0, [0, 0, 0, 1]], [1, {"euler": [10, 0, 0]}]]},
        {"path": HIPS + "/mixamorig:Spne", "channel": "position", "keys": [[0, [0, 0, 0]], [1, [0, 0.1, 0]]]},
    ],
    "events": [{"time": 0.5, "function": "onWave", "args": ["left", 1]}],
    "preview": {"model": "models/hero_animated.gltf"},
}
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

    async def wheel(self, at, down: bool, notches: int):
        self.xdo("mousemove", str(int(at[0])), str(int(at[1])))
        await asyncio.sleep(0.2)
        for _ in range(notches):
            self.xdo("click", "5" if down else "4")
        await asyncio.sleep(0.3)

    async def frames(self, n: int = 3):
        await self.ed.call("wait", frames=n)

    async def reveal(self, name: str, top: bool = True) -> list[float]:
        await self.frames()
        panel = await self.ed.ui_rect("inspector")
        self.ed.check(panel, "the inspector is drawn")
        at = ((panel[0] + panel[2]) / 2, (panel[1] + panel[3]) / 2)
        if top:
            await self.wheel(at, False, 80)
        for _ in range(80):
            await self.frames()
            rect = self.ed.ui_items().get(name)
            if rect:
                # bring it near the top of the panel (until it no longer moves)
                for _ in range(40):
                    if rect[1] < panel[1] + 60:
                        break
                    await self.wheel(at, True, 1)
                    await self.frames()
                    moved = self.ed.ui_items().get(name)
                    if not moved or moved[1] == rect[1]:
                        break
                    rect = moved
                return rect
            await self.wheel(at, True, 2)
        self.ed.fail(f"{name} is never drawn: {sorted(self.ed.ui_items())}")


def center(rect):
    return (rect[0] + rect[2]) / 2.0, (rect[1] + rect[3]) / 2.0


async def project_window(ed: Editor) -> dict:
    return (await ed.call("editor_status"))["ui"]["project_window"]


async def inspect_asset(ed: Editor, ui: Ui, folder: str, name: str) -> None:
    """Shows `folder` from the Project window's tree and clicks `name` in its listing, at the rects
    editor_status reports (ui.project_window, window pixels [x, y, w, h]): the tree's rows move with
    what the window shows above them (S25's Favorites), so no fixed positions"""
    await ed.call("focus_window", window="project")

    def node(state, path):
        return next((n for n in state["tree"] if n["path"] == path), None)

    def entry(state):
        return next((e["rect"] for e in state["entries"] if e["name"] == name), None)

    def middle(rect):
        return rect[0] + rect[2] / 2.0, rect[1] + rect[3] / 2.0

    state = await project_window(ed)
    for _ in range(40):
        if state["drawn"] and node(state, "assets"):
            break
        await ui.frames()
        state = await project_window(ed)
    parts = folder.split("/")
    for depth in range(1, len(parts)):
        parent = node(state, "/".join(parts[:depth]))
        ed.check(parent, f"{'/'.join(parts[:depth])} in the tree: {[n['path'] for n in state['tree']]}")
        if not node(state, "/".join(parts[:depth + 1])):
            await ui.click(*parent["arrow"])
            state = await project_window(ed)
    for _ in range(10):
        if state["directory"] == folder and entry(state):
            break
        target = node(state, folder)
        ed.check(target, f"{folder} in the tree: {[n['path'] for n in state['tree']]}")
        await ui.click(*middle(target["rect"]))
        await ui.frames()
        state = await project_window(ed)
    ed.check(entry(state), f"{name} listed in {state['directory']}: {[e['name'] for e in state['entries']]}")
    for _ in range(4):
        await ui.click(*middle(entry(await project_window(ed))))
        await ui.frames(10)
        if "asset.header" in ed.ui_items():
            break


async def shot(ed: Editor, name: str):
    shots = Path(os.environ["JASPER_SHOTS_DIR"]) if os.environ.get("JASPER_SHOTS_DIR") else ed.scratch.root / "shots"
    shots.mkdir(parents=True, exist_ok=True)
    await ed.call("wait", frames=4)
    await ed.screenshot("editor", path=str(shots / f"{name}.png"), max_width=1600)


async def player(ed: Editor) -> dict:
    got = await ed.call("get_entity", entity="Animated", components=["animationplayer"])
    comps = got.get("components", {})
    return comps.get("animationplayer", comps)


async def test_setup(ed: Editor):
    anim = ed.project / "assets" / "anim"
    anim.mkdir(parents=True, exist_ok=True)
    (anim / "hero_wave.anim").write_text(json.dumps(WAVE, indent=2) + "\n")
    gone = dict(WAVE, guid="ffeeddccbbaa99887766554433221100", tracks=WAVE["tracks"][:1], events=[])
    (anim / "gone.anim").write_text(json.dumps(gone, indent=2) + "\n")
    await ed.call("sync_files")

    await ed.call("set_model", path="assets/models/hero_animated.gltf", create={"name": "Animated", "position": [2, 0, 0]})
    await ed.call("add_component", entity="Animated", component="animationplayer", values={"animationplayer": {
        "clips": [{"clip": "assets/anim/hero_wave.anim", "name": "Wave"},
                  {"clip": "assets/models/hero_animated.gltf#Walk", "wrap": "loop"},
                  {"clip": "assets/anim/gone.anim"}],
        "defaultClip": "Walk"}})
    got = await player(ed)
    S["player"] = got
    ed.check("clips" in json.dumps(got), f"player: {json.dumps(got)[:600]}")
    # the third clip's file goes away: the entry keeps its reference (R8)
    (anim / "gone.anim").unlink()
    await ed.call("sync_files")
    await ed.call("select_entities", entities=["Animated"])
    await ed.call("focus_window", window="inspector")


async def test_player_section(ed: Editor):
    ui = Ui(ed)
    rect = await ui.reveal("player.addClip")
    await shot(ed, "01-player-section")
    # the Bindings foldout opens by itself the first time it has problems
    await ui.reveal("player.bindings", top=False)
    await shot(ed, "02-player-bindings")
    await ui.reveal("player.addClip")
    await shot(ed, "02b-player-clips")
    # an Add Clip click is one scene undo step
    before = await player(ed)
    rect = await ui.reveal("player.addClip")
    await ui.click(*center(rect))
    after = await player(ed)
    ed.check_eq(len(after["clips"]), len(before["clips"]) + 1, "Add Clip appends an entry")
    await ed.call("undo")
    undone = await player(ed)
    ed.check_eq(len(undone["clips"]), len(before["clips"]), "undo removes it")

    # the row menu (right click on the clip field): Remove is one step
    rect = await ui.reveal("player.clip.2")
    ui.xdo("mousemove", str(int(center(rect)[0])), str(int(center(rect)[1])))
    await asyncio.sleep(0.3)
    ui.xdo("click", "3")
    await ui.frames(5)
    item = await ed.ui_rect("player.clipMenu.remove")
    ed.check(item, f"the row menu shows Remove: {sorted(ed.ui_items())}")
    await shot(ed, "02c-player-row-menu")
    await ui.click(*center(item))
    removed = await player(ed)
    ed.check_eq(len(removed["clips"]), len(before["clips"]) - 1, "Remove drops the entry")
    await ed.call("undo")
    back = await player(ed)
    ed.check_eq(len(back["clips"]), len(before["clips"]), "undo brings it back")


async def test_model_section(ed: Editor):
    """every sub-clip has its entity animation row (with its clip buttons): no separate Model Clips table"""
    ui = Ui(ed)
    await ui.reveal("model.usePlayer")
    ed.check("model.clips" not in ed.ui_items(), "no Model Clips table while the entity rows show every clip")
    await shot(ed, "03-model-section")


async def test_use_animation_player(ed: Editor):
    """the Model section's Use Animation Player (T2d's hook): one scene step, undone at once"""
    ui = Ui(ed)
    rect = await ui.reveal("model.usePlayer")
    await ui.click(*center(rect))
    await ed.call("wait", frames=10)
    got = await ed.call("get_entity", entity="Animated", components=["model"])
    found = []

    def walk(v):
        if isinstance(v, dict):
            for k, x in v.items():
                if k == "animationEntities":
                    found.append(x)
                walk(x)
        elif isinstance(v, list):
            for x in v:
                walk(x)
    walk(got)
    ed.check(found and found[0] is False, f"animationEntities off: {json.dumps(got)[:600]}")
    await ui.reveal("model.clips")
    await shot(ed, "03b-model-after-use-player")
    await ed.call("undo")
    await ed.call("wait", frames=10)


async def test_live_states(ed: Editor):
    ui = Ui(ed)
    await ed.call("play", action="start")
    await ed.call("wait", seconds=1)
    await ed.call("select_entities", entities=["Animated"])
    await ui.reveal("player.states")
    await shot(ed, "04-player-play-states")
    await ed.call("play", action="stop")


async def test_asset_section(ed: Editor):
    ui = Ui(ed)
    await ed.call("focus_window", window="project")
    await ed.call("wait", frames=5)
    await inspect_asset(ed, ui, "assets/anim", "hero_wave.anim")
    await shot(ed, "06-clip-asset")
    S["asset_items"] = sorted(ed.ui_items())
    ed.check("asset.header" in ed.ui_items(), f"asset section drawn: {S['asset_items']}")
    # an edit in the asset section is a project step through the document
    clip = ed.project / "assets" / "anim" / "hero_wave.anim"
    ed.check('"length"' not in clip.read_text(), "auto length at first")
    rect = ed.ui_items().get("clipAsset.lengthAuto")
    ed.check(rect, "the Auto checkbox is drawn")
    await ui.click(*center(rect))
    await ed.call("wait", frames=5)
    ed.check('"length": 1' in clip.read_text(), f"explicit length written: {clip.read_text()[:300]}")
    await shot(ed, "06b-clip-asset-length")
    await ed.call("undo", scope="project")
    await ed.call("wait", frames=5)
    ed.check('"length"' not in clip.read_text(), "project undo restores the auto length")
    # a programmatic selection change shows the selection again (R30)
    await ed.call("select_entities", entities=["Hero"])
    await ed.call("wait", frames=5)
    ed.check("asset.header" not in ed.ui_items(), "the selection is shown again")
    await shot(ed, "07-selection-again")


async def test_scene_click_shows_the_selection(ed: Editor):
    """R30: clicking the GameObject that is already selected (no selection change) in the Scene view makes it
    the inspected object again"""
    ui = Ui(ed)
    await inspect_asset(ed, ui, "assets/anim", "hero_wave.anim")
    ed.check("asset.header" in ed.ui_items(), f"the clip asset is inspected: {sorted(ed.ui_items())}")
    selected = await ed.call("get_selection")
    await ed.call("frame_entity", entity="Hero")
    await ed.call("focus_window", window="scene")
    await ed.call("wait", frames=10)
    view = await ed.ui_rect("scene.view")
    ed.check(view, f"the Scene view is drawn: {sorted(ed.ui_items())}")
    await ui.click(*center(view))
    await ed.call("wait", frames=10)
    ed.check("asset.header" not in ed.ui_items(), f"a Scene view click shows the selection (was {selected})")
    await shot(ed, "07b-scene-click")


async def test_animation_component_buttons(ed: Editor):
    ui = Ui(ed)
    found = await ed.call("find_entities", name="Walk")
    ed.check("Walk" in json.dumps(found), f"Walk animation: {json.dumps(found)[:400]}")
    await ed.call("select_entities", entities=["Walk"])
    await ed.call("focus_window", window="inspector")
    await ui.reveal("animation.convert")
    await shot(ed, "09-animation-component")


async def test_pose_animator_bake_menu(ed: Editor):
    """The Pose Animator section's Bake offers what the Pose window's does: Bake to Animation Clip (a shared
    .anim next to the pose animation, listed in the Model's Animation Player) and Bake into Scene (an entity
    Animation); each one scene undo step."""
    ui = Ui(ed)
    walk = "assets/anim/inspector_walk.poseanim"
    await ed.call("edit_pose_animation", path=walk, create={"preview_model": "assets/models/hero_tpose.gltf"}, ops=[
        {"op": "add_pose", "name": "Up", "hold": 0.1, "angles": {"left_upper_arm.raise": 60}},
        {"op": "add_pose", "name": "Down", "hold": 0.1, "angles": {"left_upper_arm.raise": 10}}])
    await ed.call("add_component", entity="Hero", component="poseanimator", values={"poseanimator": {"animation": walk}})
    await ed.call("select_entities", entities=["Hero"])
    await ed.call("focus_window", window="inspector")

    async def bake_menu(item: str):
        rect = await ui.reveal("poseanimator.bake")
        await ui.click(*center(rect))
        await ui.frames(5)
        entry = await ed.ui_rect(item)
        ed.check(entry, f"the Bake menu shows {item}: {sorted(ed.ui_items())}")
        await shot(ed, "10-pose-bake-menu")
        await ui.click(*center(entry))
        await ui.frames(5)

    await bake_menu("poseanimator.bakeMenu.clip")
    clip = ed.project / "assets/anim/inspector_walk.anim"
    ed.check(clip.is_file(), "Bake to Animation Clip writes the .anim next to the pose animation")
    got = json.dumps(await ed.call("get_entity", entity="Hero", components=["animationplayer"]))
    ed.check("inspector_walk.anim" in got, f"listed in the Hero's Animation Player: {got[:600]}")
    await ed.call("undo", scope="scene")
    ed.check(not clip.is_file(), "one scene undo step (the file it wrote goes with it)")

    await bake_menu("poseanimator.bakeMenu.scene")
    found = json.dumps(await ed.call("find_entities", name="inspector_walk"))
    ed.check("inspector_walk" in found, f"Bake into Scene makes the entity Animation: {found[:400]}")
    await ed.call("undo", scope="scene")
    ed.check("inspector_walk" not in json.dumps(await ed.call("find_entities", name="inspector_walk")), "one scene undo step")
    await ed.call("remove_component", entity="Hero", component="poseanimator")
