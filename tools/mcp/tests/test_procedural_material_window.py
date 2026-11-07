"""The Procedural Material window with real input (S90 R26, R27, AC15), on a fresh project, driven by REAL X input
(xdotool on the harness' Xvfb; widgets found through the UI probe, Editor(ui_probe=True)) and checked through the
.procmat file. The window's model (R7's uniform edits and debounce, R24's tier budget, pins, seed, presets, the check
report's rows) is the editor unit test editor/tests/window/ProceduralMaterialWindowTest.cpp.

Covered here, where the App's wiring, the device, focus and real input matter:
- a real double-click on the .procmat in the Project window opens it in the Procedural Material window, whose
  node images and material preview are drawn on the editor's device, with the tier line;
- a wire dragged from Blend's output and dropped on empty canvas opens the node search; typing "invert" and Enter
  adds an Invert wired to it (one undo step);
- a click on Perlin shows its output in the preview, drawn;
- Esc during a node drag puts it back with no undo step; Esc closes the search;
- Variations: a click on a cell makes its seed the graph's, in the file; a wire to a node that is gone is a
  Problems row whose Fix removes it.
Set JASPER_MCP_PROCMAT_SHOTS=<dir> to keep the editor screenshots.
"""

from __future__ import annotations

import asyncio
import json
import os
import shutil
import subprocess
from pathlib import Path

import harness
from harness import Editor

EDITOR = {"create": True, "xvfb": "harness", "ui_probe": True}
CI = True
TIMEOUT = 900

PATH = "assets/Stone.procmat"
SHOTS = os.environ.get("JASPER_MCP_PROCMAT_SHOTS")
GRAPH = {
    "formatVersion": 1,
    "kind": "procedural_material",
    "kindVersion": 1,
    "nextId": 7,
    "nodes": [
        {"id": "n1", "type": "perlin", "position": [0.0, 0.0], "params": {}},
        {"id": "n2", "type": "blend", "position": [300.0, 0.0], "params": {"amount": 0.5}},
        {"id": "n3", "type": "material_output", "position": [620.0, 0.0], "params": {}},
    ],
    "edges": [
        {"id": "e4", "from": {"node": "n1", "port": "output"}, "to": {"node": "n2", "port": "s1"}},
        {"id": "e5", "from": {"node": "n2", "port": "output"}, "to": {"node": "n3", "port": "base_color"}},
        {"id": "e6", "from": {"node": "n1", "port": "output"}, "to": {"node": "n3", "port": "roughness"}},
    ],
}


def xdo(ed: Editor, *args: str) -> str:
    if not getattr(ed, "_xvfb", None) or not ed._xvfb.display:
        harness.skip("needs the harness' Xvfb (xvfb='harness')")
    if not shutil.which("xdotool"):
        harness.skip("xdotool is not installed (apt install xdotool)")
    env = dict(os.environ, DISPLAY=ed._xvfb.display)
    return subprocess.run(["xdotool", *args], env=env, capture_output=True, text=True, timeout=20).stdout


def center(rect):
    return (rect[0] + rect[2]) / 2.0, (rect[1] + rect[3]) / 2.0


async def frames(ed: Editor, n: int = 4):
    await ed.call("wait", frames=n)


async def move(ed: Editor, x: float, y: float):
    xdo(ed, "mousemove", str(int(x)), str(int(y)))
    await asyncio.sleep(0.15)
    await frames(ed, 2)


async def click(ed: Editor, at, button: str = "1"):
    await move(ed, *at)
    xdo(ed, "click", button)
    await frames(ed, 6)


async def rect_of(ed: Editor, name: str, what: str, seconds: float = 10.0):
    rect = await ed.ui_rect(name, timeout=seconds)
    if not rect:
        ed.fail(f"{what} ({name}) is never drawn: {sorted(ed.ui_items())[:120]}")
    return rect


async def focus_x(ed: Editor):
    """the window on the graph (focus_window opens a .procmat in its kind's window), with the X focus"""
    got = await ed.call("focus_window", window="graph_editor", path=PATH)
    ed.check_eq(got.get("graph"), PATH, f"the graph shown: {got}")
    ids = xdo(ed, "search", "--onlyvisible", "--name", "JasperEngine").split()
    ed.check(ids, "the editor's X window")
    xdo(ed, "windowfocus", "--sync", ids[-1])
    await frames(ed, 6)


async def drag_start(ed: Editor, at, to, steps: int = 8):
    """the left button pressed at `at` and moved to `to`, still down"""
    await move(ed, *at)
    xdo(ed, "mousedown", "1")
    await frames(ed, 2)
    for i in range(1, steps + 1):
        xdo(ed, "mousemove", str(int(at[0] + (to[0] - at[0]) * i / steps)), str(int(at[1] + (to[1] - at[1]) * i / steps)))
        await asyncio.sleep(0.05)
        await frames(ed, 1)
    await frames(ed, 2)


async def release(ed: Editor):
    xdo(ed, "mouseup", "1")
    await frames(ed, 6)


async def key(ed: Editor, name: str):
    xdo(ed, "key", name)
    await frames(ed, 6)


def graph_file(ed: Editor) -> dict:
    return json.loads((ed.project / PATH).read_text())


async def undo_name(ed: Editor) -> str:
    status = await ed.call("editor_status")
    return (status.get("undo") or {}).get("undo_name") or ""


async def shot(ed: Editor, name: str):
    if SHOTS:
        Path(SHOTS).mkdir(parents=True, exist_ok=True)
    await frames(ed, 4)
    await ed.screenshot("editor", path=(Path(SHOTS) / f"procmat-{name}.png") if SHOTS else None, max_width=1800)


async def test_setup(ed: Editor):
    ed.require_tools("focus_window", "sync_files", "editor_status", "screenshot")
    (ed.project / PATH).parent.mkdir(parents=True, exist_ok=True)
    (ed.project / PATH).write_text(json.dumps(GRAPH, indent=2) + "\n")
    await ed.call("sync_files")


async def test_double_click_opens_the_material_with_its_images_and_device(ed: Editor):
    got = await ed.call("focus_window", window="project")
    ed.check(got.get("shown"), f"the Project window: {got}")
    ids = xdo(ed, "search", "--onlyvisible", "--name", "JasperEngine").split()
    xdo(ed, "windowfocus", "--sync", ids[-1])
    entry = None
    for _ in range(12):
        await frames(ed, 4)
        items = ed.ui_items()
        entry = items.get("project.file.Stone.procmat")
        if entry:
            break
        if "project.file.assets" in items:
            await move(ed, *center(items["project.file.assets"]))
            xdo(ed, "click", "--repeat", "2", "--delay", "80", "1")
            await frames(ed, 6)
    ed.check(entry, f"the Project window lists the material: {sorted(ed.ui_items())[:80]}")
    await move(ed, *center(entry))
    xdo(ed, "click", "--repeat", "2", "--delay", "80", "1")
    await frames(ed, 10)
    await rect_of(ed, "graph.node.n1", "Perlin on the canvas")
    await rect_of(ed, "procmat.preview.of.material", "the material in the preview")
    # drawn on the editor's own device: the node images and the material, up to date
    await rect_of(ed, "procmat.thumb.n3", "the Material Output's image", 60)
    await rect_of(ed, "procmat.preview.current", "the material's image", 60)
    await rect_of(ed, "procmat.preview.output.base_color", "the Base Color image")
    await rect_of(ed, "procmat.preview.ball.current", "the lit ball of the material, drawn", 60)
    tiers = [name for name in ed.ui_items() if name.startswith("procmat.tier.") and name != "procmat.tier.reason"]
    # the harness' editor draws on llvmpipe: never "gpu"; "unknown" until the tiers are detected, then "software"
    ed.check("procmat.tier.gpu" not in tiers and tiers and tiers[0] in ("procmat.tier.unknown", "procmat.tier.software"),
             f"the tier line on llvmpipe: {tiers}")
    await shot(ed, "opened")


async def test_a_wire_dropped_on_the_canvas_adds_the_node_picked(ed: Editor):
    await focus_x(ed)
    port = center(await rect_of(ed, "graph.port.n2.out.output", "Blend's output port"))
    canvas = await rect_of(ed, "graph.canvas", "the canvas")
    # empty canvas below the Blend
    drop = (port[0] - 40, min(port[1] + 260, canvas[3] - 40))
    before = await undo_name(ed)
    await drag_start(ed, port, drop)
    await release(ed)
    await rect_of(ed, "graph.search.query", "the node search")
    xdo(ed, "type", "--delay", "60", "invert")
    await frames(ed, 6)
    await rect_of(ed, "graph.search.entry.0", "the search's first entry")
    await shot(ed, "search")
    await key(ed, "Return")
    await frames(ed, 4)
    ed.check(not ed.ui_items().get("graph.search.query"), "the search closed")
    got = graph_file(ed)
    added = [n for n in got["nodes"] if n["type"] == "invert"]
    ed.check_eq(len(added), 1, f"an Invert was added: {got['nodes']}")
    wires = [(e["from"]["node"], e["from"]["port"], e["to"]["node"]) for e in got["edges"] if e["to"]["node"] == added[0]["id"]]
    ed.check_eq(wires, [("n2", "output", added[0]["id"])], "wired to Blend's output")
    ed.check("Invert" in await undo_name(ed) and await undo_name(ed) != before, "one undo step names the node")


async def test_a_selected_node_shows_its_preview(ed: Editor):
    await focus_x(ed)
    node = await rect_of(ed, "graph.node.n1", "Perlin on the canvas")
    await click(ed, (node[0] + 30, node[1] + 8))
    await rect_of(ed, "procmat.preview.of.n1", "Perlin's output in the preview")
    if not await ed.ui_rect("procmat.preview.current", timeout=60):
        await shot(ed, "selected-failed")
        ed.fail(f"Perlin's image is never drawn: {sorted(n for n in ed.ui_items() if n.startswith(('procmat.', 'graph.problem')))}")
    await rect_of(ed, "procmat.thumb.n1", "Perlin's own image", 60)
    await shot(ed, "selected")


async def test_esc_cancels_a_node_drag_and_closes_the_search(ed: Editor):
    await focus_x(ed)
    text = (ed.project / PATH).read_text()
    before = await undo_name(ed)
    node = await rect_of(ed, "graph.node.n2", "Blend on the canvas")
    at = (node[0] + 30, node[1] + 8)
    await drag_start(ed, at, (at[0] + 120, at[1] + 90))
    await key(ed, "Escape")
    moved = await rect_of(ed, "graph.node.n2", "Blend on the canvas")
    ed.check(abs(moved[0] - node[0]) < 1 and abs(moved[1] - node[1]) < 1, f"Esc put the node back: {moved} vs {node}")
    await release(ed)
    ed.check_eq((ed.project / PATH).read_text(), text, "no write")
    ed.check_eq(await undo_name(ed), before, "no undo step")

    canvas = await rect_of(ed, "graph.canvas", "the canvas")
    await move(ed, canvas[0] + 60, canvas[3] - 60)
    xdo(ed, "click", "3")
    await frames(ed, 6)
    await rect_of(ed, "graph.search.query", "the node search")
    await key(ed, "Escape")
    ed.check(not ed.ui_items().get("graph.search.query"), "Esc closed the search")
    ed.check_eq((ed.project / PATH).read_text(), text, "nothing added")


async def test_a_variation_click_adopts_its_seed(ed: Editor):
    await focus_x(ed)
    before = graph_file(ed).get("metadata", {}).get("seed", 0)
    await click(ed, center(await rect_of(ed, "procmat.variations", "the Variations button")))
    cell = await rect_of(ed, "procmat.variation.3", "the fourth variation")
    await rect_of(ed, "procmat.variation.3.current", "the fourth variation's image, of its seed", 60)
    await shot(ed, "variations")
    await click(ed, center(cell))
    seed = graph_file(ed).get("metadata", {}).get("seed")
    ed.check(isinstance(seed, int) and seed != before, f"the cell's seed is the graph's: {seed} (was {before})")
    ed.check_eq(await undo_name(ed), "Use Variation Seed", "one undo step")


async def test_fix_removes_a_wire_to_a_node_that_is_gone(ed: Editor):
    got = graph_file(ed)
    got["edges"].append({"id": "e99", "from": {"node": "n404", "port": "output"}, "to": {"node": "n2", "port": "s2"}})
    (ed.project / PATH).write_text(json.dumps(got, indent=2) + "\n")
    await ed.call("sync_files")
    await focus_x(ed)
    header = await rect_of(ed, "graph.problems", "the Problems strip")
    await click(ed, (header[0] + 40, center(header)[1]))
    fix = None
    for i in range(6):
        fix = ed.ui_items().get(f"graph.problem.{i}.fix")
        if fix:
            break
    ed.check(fix, f"a Fix on the dangling wire's row: {sorted(n for n in ed.ui_items() if n.startswith('graph.problem'))}")
    await shot(ed, "problems")
    await click(ed, center(fix))
    edges = [e["id"] for e in graph_file(ed)["edges"]]
    ed.check("e99" not in edges, f"Fix removed the wire: {edges}")
    ed.check_eq(await undo_name(ed), "Remove Dangling Wire", "one undo step")
