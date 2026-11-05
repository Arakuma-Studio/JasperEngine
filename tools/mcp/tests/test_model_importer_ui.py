"""The Inspector's Model Import Settings with real input (docs/specs/S53-model-import-cook.md R16-R21,
AC12): clicking a model in the Project window shows its importer with its measured cook; a
multi-selection shows totals and the largest models; a model the loader reports an error in shows
the failure row with its actions; Window > Analysis > Mesh Optimization Audit lists the project's models; an export
writes build-report.json and the Build Report window shows it. xdotool clicks the rects
editor_status and find_ui_item report; the screenshots of each step go to SHOTS (default
build/test-results/model-importer-ui/, JASPER_MODEL_SHOTS overrides it).
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
from pathlib import Path

from harness import Editor, skip
import ui_input
from model_fixture import grid_glb

REPO = Path(__file__).resolve().parents[3]
EDITOR = {"project": REPO / "tools" / "fixtures" / "textures", "xvfb": "harness"}
TIMEOUT = 1200
CI = True
SHOTS = Path(os.environ.get("JASPER_MODEL_SHOTS", REPO / "build" / "test-results" / "model-importer-ui"))
STATE: dict = {}


def xdo(ed: Editor, *args: str) -> str:
    env = dict(os.environ, DISPLAY=ed._xvfb.display)
    return subprocess.run(["xdotool", *args], env=env, capture_output=True, text=True, timeout=20).stdout.strip()


def window(ed: Editor) -> str:
    if "window" not in STATE:
        ids = xdo(ed, "search", "--name", "JasperEngine").split()
        if not ids:
            ed.fail("no editor window on the harness display")
        STATE["window"] = ids[-1]
        xdo(ed, "windowactivate", "--sync", STATE["window"])
    return STATE["window"]


async def click(ed: Editor, rect: list, modifier: str | None = None) -> None:
    window(ed)
    x, y, w, h = rect
    ui = ui_input.for_editor(ed)
    await ui.click(ui.at((x + w / 2, y + h / 2)), modifier=modifier)


async def ui(ed: Editor) -> dict:
    return (await ed.call("editor_status"))["ui"]


async def wait_ui(ed: Editor, predicate, what: str, timeout: float = 30.0) -> dict:
    state = None
    for _ in range(int(timeout / 0.25)):
        state = await ui(ed)
        if predicate(state):
            return state
        await ui_input.frames(ed, 2)
    ed.fail(f"{what}; ui: {state}")


def tree_node(state: dict, path: str) -> dict | None:
    return next((node for node in state["project_window"]["tree"] if node["path"] == path), None)


def entry(state: dict, name: str) -> list | None:
    return next((item["rect"] for item in state["project_window"]["entries"] if item["name"] == name), None)


async def wait_item(ed: Editor, label: str, window_name: str | None = None) -> dict:
    """An item (a control, its ## id, or a table header: plain text is not an item), once drawn,
    scrolled into view for the screenshot that follows."""
    window(ed)
    return await ui_input.for_editor(ed).reveal(label, window_name, frames=480)


async def shot(ed: Editor, name: str) -> None:
    SHOTS.mkdir(parents=True, exist_ok=True)
    await ed.screenshot("editor", SHOTS / f"{name}.png", focus=False, max_width=0)


async def test_model_importer_audit_and_build_report(ed: Editor):
    if not shutil.which("xdotool") or not ed._xvfb or not ed._xvfb.display:
        skip("needs xdotool and the harness Xvfb")
    (ed.project / "assets" / "models" / "grid.glb").write_bytes(grid_glb())
    # a model that requires an extension the engine does not read (S53 R5): an error, export fails
    unread = {"asset": {"version": "2.0"}, "extensionsUsed": ["VENDOR_required_test"],
              "extensionsRequired": ["VENDOR_required_test"], "meshes": []}
    (ed.project / "assets" / "models" / "unread.gltf").write_text(json.dumps(unread))
    await ed.call("sync_files", paths=["assets/models/grid.glb", "assets/models/unread.gltf"])
    await ed.call("tool_groups", enable=["meshes", "editor_ux"])
    # the cooks first, so the importer shows measured numbers
    result = await ed.call("reimport_models", paths=["assets/models/grid.glb", "assets/models/panel.gltf"])
    ed.check(all(m["status"] == "cooked" for m in result["models"]), f"{result['models']}")

    await ed.call("focus_window", window="project")
    state = await wait_ui(ed, lambda s: s["project_window"]["drawn"] and tree_node(s, "assets"), "the tree shows assets")
    if not tree_node(state, "assets/models"):
        x, y = tree_node(state, "assets")["arrow"]
        window(ed)
        ui_obj = ui_input.for_editor(ed)
        await ui_obj.click(ui_obj.at((x, y)))
        state = await wait_ui(ed, lambda s: tree_node(s, "assets/models"), "assets opened in the tree")
    await click(ed, tree_node(state, "assets/models")["rect"])
    state = await wait_ui(ed, lambda s: entry(s, "grid.glb") and entry(s, "panel.gltf"), "assets/models shown")

    # one model: its importer, preview, levels and before-and-after table
    await click(ed, entry(state, "grid.glb"))
    await wait_item(ed, "##meshCompression", "Inspector")
    await wait_item(ed, "Differs by at most", "Inspector")
    await ui_input.frames(ed, 20)
    await shot(ed, "1-model-importer")

    # a multi-selection: mixed values, totals, the largest
    await click(ed, entry(state, "panel.gltf"), modifier="ctrl")
    await wait_item(ed, "##largest0", "Inspector")
    await ui_input.frames(ed, 6)
    await shot(ed, "2-model-importer-multi")

    # a model with a load error: the failure row and its actions
    await ed.call("reimport_models", paths=["assets/models/unread.gltf"])
    state = await ui(ed)
    await click(ed, entry(state, "unread.gltf"))
    await wait_item(ed, "Show in Explorer", "Inspector")
    await ui_input.frames(ed, 6)
    await shot(ed, "3-model-failure-row")

    # Window > Analysis > Mesh Optimization Audit
    await ed.call("run_command", command="window.meshAudit")
    await wait_item(ed, "Duplicate %")
    await ui_input.frames(ed, 10)
    await shot(ed, "4-mesh-audit")

    # an export's Build Report (a model with a load error fails an export: it goes first)
    (ed.project / "assets" / "models" / "unread.gltf").unlink()
    await ed.call("sync_files", paths=["assets/models/unread.gltf"])
    out = ed.scratch.root / "export-source"
    built = await ed.call("build_project", mode="source", output_dir=str(out), platform="linux", timeout_seconds=600)
    report = out / "build-report.json"
    ed.check(report.exists(), f"build-report.json next to the export: {built}")
    data = json.loads(report.read_text())
    ed.check(data["categories"]["models"] > 0, f"the models' bytes: {data['categories']}")
    ed.check(any(f.get("source") == "models/grid.glb" for f in data["largest"]), f"the grid's cook among the largest: {data['largest']}")
    await ed.call("run_command", command="window.buildReport")
    await wait_item(ed, "Source Size")
    await ui_input.frames(ed, 10)
    await shot(ed, "5-build-report")
