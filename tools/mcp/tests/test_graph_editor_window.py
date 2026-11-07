"""The Graph Editor window with real input (S90 R26: wire-drop search, Esc cancel), on a fresh project whose editor
registers the test graph kind (JASPER_TEST_GRAPH_KIND=1: speed / label / blend, float -> vector3), driven by REAL X
input (xdotool on the harness' Xvfb; widgets found through the UI probe, Editor(ui_probe=True)) and checked through
the graph file (get_graph). The window's model (search ranking, aliases, enum entries, copy and paste, Fix, undo
reaching the document) is the editor unit test editor/tests/graph/GraphEditorWindowTest.cpp.

Covered here, where the App's wiring, focus and real input matter:
- a real double-click on the graph file in the Project window opens it in the Graph Editor (its nodes' probes drawn);
- a wire dragged from Speed's output and dropped on empty canvas opens the node search; typing "blend" and Enter adds
  a Blend wired to Speed, with its input port under the cursor where the wire was dropped (one undo step);
- Esc during a node drag puts the node back and adds no undo step; Esc during a wire drag joins nothing;
- Esc closes the search a right click opened, adding nothing.
Set JASPER_MCP_GRAPH_SHOTS=<dir> to keep the editor screenshots.
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

EDITOR = {"create": True, "xvfb": "harness", "ui_probe": True, "env": {"JASPER_TEST_GRAPH_KIND": "1"}}
CI = True
TIMEOUT = 600

PATH = "assets/Flow.testgraph"
SHOTS = os.environ.get("JASPER_MCP_GRAPH_SHOTS")
GRAPH = {
    "formatVersion": 1,
    "kind": "test",
    "kindVersion": 1,
    "nextId": 3,
    "nodes": [
        {"id": "n1", "type": "speed", "position": [0.0, 0.0], "params": {"value": 2.0}},
        {"id": "n2", "type": "label", "position": [0.0, 160.0], "params": {}},
    ],
    "edges": [],
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


async def rect_of(ed: Editor, name: str, what: str, seconds: float = 10.0):
    rect = await ed.ui_rect(name, timeout=seconds)
    if not rect:
        ed.fail(f"{what} ({name}) is never drawn: {sorted(ed.ui_items())[:80]}")
    return rect


async def focus_x(ed: Editor, window: str):
    await ed.call("focus_window", window=window)
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
    await ed.screenshot("editor", path=(Path(SHOTS) / f"graph-{name}.png") if SHOTS else None, max_width=1600)


async def test_setup(ed: Editor):
    ed.require_tools("focus_window", "sync_files", "get_graph", "editor_status", "screenshot")
    (ed.project / PATH).parent.mkdir(parents=True, exist_ok=True)
    (ed.project / PATH).write_text(json.dumps(GRAPH, indent=2) + "\n")
    await ed.call("sync_files")


async def test_double_click_opens_the_graph(ed: Editor):
    await focus_x(ed, "project")
    entry = None
    for _ in range(12):
        await frames(ed, 4)
        items = ed.ui_items()
        entry = items.get("project.file.Flow.testgraph")
        if entry:
            break
        if "project.file.assets" in items:
            await move(ed, *center(items["project.file.assets"]))
            xdo(ed, "click", "--repeat", "2", "--delay", "80", "1")
            await frames(ed, 6)
    ed.check(entry, f"the Project window lists the graph file: {sorted(ed.ui_items())[:80]}")
    await move(ed, *center(entry))
    xdo(ed, "click", "--repeat", "2", "--delay", "80", "1")
    await frames(ed, 10)
    await rect_of(ed, "graph.node.n1", "Speed on the canvas")
    await rect_of(ed, "graph.port.n1.out.value", "Speed's output port")
    await shot(ed, "opened")


async def test_a_wire_dropped_on_the_canvas_adds_the_node_picked_with_its_port_there(ed: Editor):
    await focus_x(ed, "graph_editor")
    port = center(await rect_of(ed, "graph.port.n1.out.value", "Speed's output port"))
    canvas = await rect_of(ed, "graph.canvas", "the canvas")
    # empty canvas right of Speed, clear of Label below it
    drop = (min(port[0] + 260, canvas[2] - 40), port[1] + 10)
    before = await undo_name(ed)
    await drag_start(ed, port, drop)
    await release(ed)
    await rect_of(ed, "graph.search.query", "the node search")
    await shot(ed, "search")
    xdo(ed, "type", "--delay", "60", "blend")
    await frames(ed, 6)
    await rect_of(ed, "graph.search.entry.0", "the search's first entry")
    await key(ed, "Return")
    await frames(ed, 4)
    ed.check(not ed.ui_items().get("graph.search.query"), "the search closed")
    got = graph_file(ed)
    added = [n for n in got["nodes"] if n["type"] == "blend"]
    ed.check_eq(len(added), 1, f"a Blend was added: {got['nodes']}")
    blend = added[0]["id"]
    wires = [(e["from"]["node"], e["from"]["port"], e["to"]["node"], e["to"]["port"]) for e in got["edges"]]
    ed.check_eq(wires, [("n1", "value", blend, "a")], "wired to Speed's output")
    ed.check(await undo_name(ed) != before and "Blend" in await undo_name(ed), "one undo step names the node")
    # the joined input sits where the wire was dropped
    joined = center(await rect_of(ed, f"graph.port.{blend}.in.a", "the new Blend's input"))
    ed.check(abs(joined[0] - drop[0]) <= 3 and abs(joined[1] - drop[1]) <= 3, f"the port under the cursor: {joined} vs {drop}")
    await shot(ed, "wired")


async def test_esc_cancels_a_node_drag_and_a_wire_drag(ed: Editor):
    await focus_x(ed, "graph_editor")
    text = (ed.project / PATH).read_text()
    before = await undo_name(ed)
    node = await rect_of(ed, "graph.node.n2", "Label on the canvas")
    # a press on the header (not a port) drags the node
    at = (node[0] + 30, node[1] + 8)
    await drag_start(ed, at, (at[0] + 120, at[1] + 90))
    await key(ed, "Escape")
    moved = await rect_of(ed, "graph.node.n2", "Label on the canvas")
    ed.check(abs(moved[0] - node[0]) < 1 and abs(moved[1] - node[1]) < 1, f"Esc put the node back: {moved} vs {node}")
    await release(ed)
    ed.check_eq((ed.project / PATH).read_text(), text, "no write")
    ed.check_eq(await undo_name(ed), before, "no undo step")

    # a wire from Speed toward the Blend's free input, Esc before the release: nothing joined
    got = graph_file(ed)
    blend = next(n["id"] for n in got["nodes"] if n["type"] == "blend")
    port = center(await rect_of(ed, "graph.port.n1.out.value", "Speed's output port"))
    target = center(await rect_of(ed, f"graph.port.{blend}.in.b", "the Blend's input B"))
    await drag_start(ed, port, target)
    await key(ed, "Escape")
    await release(ed)
    ed.check_eq((ed.project / PATH).read_text(), text, "Esc during a wire drag joins nothing")
    ed.check(not ed.ui_items().get("graph.search.query"), "and opens no search")


async def test_esc_closes_the_search_a_right_click_opened(ed: Editor):
    await focus_x(ed, "graph_editor")
    text = (ed.project / PATH).read_text()
    canvas = await rect_of(ed, "graph.canvas", "the canvas")
    await move(ed, canvas[0] + 60, canvas[3] - 60)
    xdo(ed, "click", "3")
    await frames(ed, 6)
    await rect_of(ed, "graph.search.query", "the node search")
    await key(ed, "Escape")
    ed.check(not ed.ui_items().get("graph.search.query"), "Esc closed the search")
    ed.check_eq((ed.project / PATH).read_text(), text, "nothing added")


async def test_focus_window_opens_a_graph_by_path(ed: Editor):
    """review I6: an agent shows a human a given graph, and reads which one is shown"""
    got = await ed.call("focus_window", window="graph_editor", path=PATH)
    ed.check_eq(got.get("graph"), PATH, f"the graph shown: {got}")
    bad = await ed.call_raw("focus_window", {"window": "graph_editor", "path": "assets/missing.testgraph"})
    ed.check(bad.is_error and "path" in bad.text, f"a missing graph is refused: {bad.text[:200]}")
    other = await ed.call_raw("focus_window", {"window": "console", "path": PATH})
    ed.check(other.is_error and "graph_editor" in other.text, f"path belongs to graph_editor: {other.text[:200]}")
