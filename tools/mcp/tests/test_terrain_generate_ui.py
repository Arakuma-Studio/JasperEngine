"""The Terrain window's Generate section driven like a human (T2 R60; AC44), with xdotool in the harness's Xvfb on a
created project: Preview writes nothing, Apply is one undo step, Cancel writes nothing, and the last graph and seed
persist across a reload. Screenshot to $JE_EVIDENCE_DIR when set (else the scratch directory).

The stream-key lint runs here too: tools/check_stream_keys.py knows the two new keys (AC44's "StreamSchema lint green").
"""

from __future__ import annotations

import hashlib
import os
import shutil
import subprocess
import sys
from pathlib import Path

from harness import Editor, REPO
from ui_input import Ui

CI = True
EDITOR = {"create": True, "name": "Terrain Generate UI", "xvfb": "harness", "ui_probe": True}
TIMEOUT = 1200

WINDOW = "Terrain"
SIZE = 200
MAX_HEIGHT = 100


async def save_shot(ed: Editor, name: str) -> None:
    """the editor's whole window, into $JE_EVIDENCE_DIR when set (else the scratch directory)"""
    relative = f"shots/{name}"
    shot = await ed.call_raw("screenshot", {"view": "editor", "save_path": relative})
    ed.check(not shot.is_error, shot.text)
    folder = Path(os.environ.get("JE_EVIDENCE_DIR") or ed.scratch.root)
    folder.mkdir(parents=True, exist_ok=True)
    shutil.copy(ed.project / relative, folder / name)


def map_files(ed: Editor) -> dict:
    out = {}
    for path in ed.project.rglob("*.png"):
        if ".jasperengine" not in str(path):
            out[str(path.relative_to(ed.project))] = hashlib.sha256(path.read_bytes()).hexdigest()
    return out


async def scene_with_terrain(ed: Editor, name: str, resolution: int = 65):
    await ed.call("tool_groups", enable=["terrain"])
    made = await ed.call("create_scene", name=name, type="3D", path=f"scenes/{name}")
    sid = made["scene"] if "scene" in made else made["scene_id"]
    terrain = (await ed.call("create_entity", scene=sid, name="Ground", type="terrain",
                             values={"terrain": {"terrainSize": SIZE, "maxHeight": MAX_HEIGHT, "resolution": 8}}))["entity_id"]
    await ed.call("set_terrain_maps", scene=sid, entity=terrain, heightmap={"generate": "flat", "resolution": resolution, "base_height": 0.25})
    await ed.call("select_entities", scene=sid, entities=[terrain])
    await ed.call("focus_window", window="terrain")
    return sid, terrain


async def open_section(ed: Editor, ui: Ui) -> None:
    """the Generate header open and its Preview button in view: the header is clicked once per attempt, then
    its inside is waited for (a second click before the first draws would close it again)"""
    for _ in range(3):
        for _ in range(30):
            await ui.frames(2)
            items = ed.ui_items()
            if "terrain.generate.preview" in items:
                return
            if "terrain.generate.seed" in items:
                # open, with the buttons below the window's edge
                await ui.reveal("Preview", WINDOW)
                continue
            if "terrain.generate" in items:
                break
        else:
            continue
        if "terrain.generate.seed" not in ed.ui_items():
            await ed.call("focus_window", window="terrain")
            await ui.click(ui.centre(ed.ui_items()["terrain.generate"]))
    ed.check("terrain.generate.preview" in ed.ui_items(), f"the section opens: {sorted(k for k in ed.ui_items() if 'terrain' in k)}")


async def wait_status(ed: Editor, ui: Ui, frames: int = 600) -> str:
    """the section's last status once a run has finished: its progress bar gone"""
    for _ in range(frames):
        await ui.frames(2)
        items = ed.ui_items()
        if "terrain.generate.progress" in items:
            continue
        for key in items:
            if key.startswith("terrain.generate.status: "):
                return key.split(": ", 1)[1]
    ed.fail(f"no status: {sorted(k for k in ed.ui_items() if 'generate' in k)}")


async def test_preview_does_not_write(ed: Editor):
    """AC44: Preview shows an image and statistics, changes no file and no height, and adds no undo step."""
    ui = Ui(ed)
    sid, terrain = await scene_with_terrain(ed, "Preview")
    await open_section(ed, ui)
    files = map_files(ed)
    heights = await ed.call("sample_terrain", scene=sid, entity=terrain, grid=8)
    await ui.click(ui.centre(ed.ui_items()["terrain.generate.preview"]))
    status = await wait_status(ed, ui)
    ed.check(status.startswith("Preview"), status)
    await ui.frames(10)
    ed.check("terrain.generate.image" in ed.ui_items(), "the hillshaded image")
    await save_shot(ed, "terrain-generate-section.png")
    ed.check_eq(map_files(ed), files, "no map file changed")
    again = await ed.call("sample_terrain", scene=sid, entity=terrain, grid=8)
    ed.check_eq(again["grid"], heights["grid"], "no height changed")


async def test_apply_is_one_undo_step(ed: Editor):
    """AC44: Apply changes the heights, and one undo puts them back."""
    ui = Ui(ed)
    sid, terrain = await scene_with_terrain(ed, "Apply")
    await open_section(ed, ui)
    before = await ed.call("sample_terrain", scene=sid, entity=terrain, grid=8)
    await ui.click(ui.centre(ed.ui_items()["terrain.generate.apply"]))
    status = await wait_status(ed, ui)
    ed.check(status.startswith("Generate Terrain ("), status)
    applied = await ed.call("sample_terrain", scene=sid, entity=terrain, grid=8)
    ed.check(applied["grid"] != before["grid"], "the heights changed")
    await ed.call("undo", scene=sid)
    restored = await ed.call("sample_terrain", scene=sid, entity=terrain, grid=8)
    ed.check_eq(restored["grid"], before["grid"], "one undo restores the heights")


async def test_cancel_writes_nothing(ed: Editor):
    """AC44: Cancel while it runs leaves files and heights as they were. A big map, so there is time to press it."""
    ui = Ui(ed)
    sid, terrain = await scene_with_terrain(ed, "Cancel", resolution=2049)
    await open_section(ed, ui)
    files = map_files(ed)
    before = await ed.call("sample_terrain", scene=sid, entity=terrain, grid=8)
    await ui.click(ui.centre(ed.ui_items()["terrain.generate.apply"]))
    cancel = await ed.ui_rect("terrain.generate.cancel", timeout=30.0)
    ed.check(cancel, "Cancel shows while it runs")
    await ui.click(ui.centre(cancel))
    status = await wait_status(ed, ui)
    ed.check(status.startswith("Cancelled"), status)
    ed.check_eq(map_files(ed), files, "no map file changed")
    after = await ed.call("sample_terrain", scene=sid, entity=terrain, grid=8)
    ed.check_eq(after["grid"], before["grid"], "no height changed")


async def test_settings_persist_across_a_reload(ed: Editor):
    """AC44, R60: the seed and the graph are in the workspace after a reload."""
    ui = Ui(ed)
    await scene_with_terrain(ed, "Persist")
    await open_section(ed, ui)
    # Pick another preset and set a seed through the fields
    await ui.click(ui.centre(ed.ui_items()["terrain.generate.graph"]))
    item = await ed.ui_rect("terrain.generate.preset.plains", timeout=20.0)
    ed.check(item, "the presets are listed")
    await ui.click(ui.centre(item))
    seed = ed.ui_items()["terrain.generate.seed"]
    await ui.double_click(ui.centre(seed))
    await ui.key("ctrl+a")
    await ui.type("4242")
    await ui.key("Return")
    await ui.frames(6)

    project = str(ed.project)
    await ed.call("open_project", path=project, unsaved="discard")
    await ui.frames(10)
    workspace = list(ed.project.rglob("workspace.yaml")) or list(Path(os.environ.get("XDG_CONFIG_HOME", ed.scratch.root)).rglob("workspace.yaml"))
    text = "\n".join(path.read_text() for path in workspace)
    ed.check("preset:plains" in text, f"generateGraph persisted in {workspace}")
    ed.check("4242" in text, f"generateSeed persisted in {workspace}")


async def test_stream_keys_lint(ed: Editor):
    """AC44: the known-key tables cover the two new keys."""
    result = subprocess.run([sys.executable, str(REPO / "tools" / "check_stream_keys.py")], capture_output=True, text=True)
    ed.check_eq(result.returncode, 0, result.stdout + result.stderr)
