"""The Noise Graph window driven like a human (T2 R50, R55-R59, R62; AC38, AC43), with xdotool in the harness's
Xvfb on a created project: it opens from the command catalog and from a .noisegraph's double-click; Project window >
Create > Noise Graph makes a graph with a GUID; find_ui_item finds the canvas, Seed, Preview and Apply; an edit is one
project undo step; an error node exposes its message through the UI probe and the Console stays empty; and closed, the
window keeps no job and no texture. Screenshots go to $JE_EVIDENCE_DIR when set (else the scratch directory).
"""

from __future__ import annotations

import json
import os
import shutil
from pathlib import Path

from harness import Editor
from ui_input import Ui

CI = True
EDITOR = {"create": True, "name": "Noise Graph Window", "xvfb": "harness", "ui_probe": True}
TIMEOUT = 900

WINDOW = "Noise Graph"
# (a folder that sorts first, so it is in view without scrolling, as test_animator_window.py does)
GRAPH = "aaa_graphs/Driven.noisegraph"

SIMPLE = {
    "formatVersion": 1, "kind": "noise", "kindVersion": 1, "nextId": 10,
    "nodes": [
        {"id": "n1", "type": "fbm", "position": [0.0, 0.0], "params": {"octaves": 3, "frequency": 3.0}},
        {"id": "n2", "type": "remap", "position": [260.0, 0.0], "params": {"from_min": -1.0, "from_max": 1.0, "to_min": 0.0, "to_max": 1.0}},
        {"id": "n3", "type": "output_height", "position": [520.0, 0.0], "params": {"unit": "normalized"}},
    ],
    "edges": [
        {"id": "e4", "from": {"node": "n1", "port": "out"}, "to": {"node": "n2", "port": "in"}},
        {"id": "e5", "from": {"node": "n2", "port": "out"}, "to": {"node": "n3", "port": "in"}},
    ],
    "variables": [], "groups": [], "viewport": {"offset": [0.0, 0.0], "zoom": 1.0}, "metadata": {"noise": {"space": "terrain"}},
}



def mid(ui: Ui, rect) -> tuple[int, int]:
    """the screen centre of a UI probe rect: the probe gives [x0, y0, x1, y1], Ui.centre takes [x, y, width, height]"""
    return ui.centre([rect[0], rect[1], rect[2] - rect[0], rect[3] - rect[1]])

async def save_shot(ed: Editor, name: str) -> None:
    """the editor's whole window, into $JE_EVIDENCE_DIR when set (else the scratch directory)"""
    relative = f"shots/{name}"
    shot = await ed.call_raw("screenshot", {"view": "editor", "save_path": relative})
    ed.check(not shot.is_error, shot.text)
    folder = Path(os.environ.get("JE_EVIDENCE_DIR") or ed.scratch.root)
    folder.mkdir(parents=True, exist_ok=True)
    shutil.copy(ed.project / relative, folder / name)


def write_graph(ed: Editor, graph: dict = SIMPLE, path: str = GRAPH) -> Path:
    target = ed.project / "assets" / path
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(graph, indent=2) + "\n")
    return target


async def wait_probe(ed: Editor, ui: Ui, name: str, frames: int = 120, prefix: bool = False):
    """the probe's name and rect once the editor drew it; prefix: an error's name carries its message"""
    for i in range(frames):
        if i % 20 == 19 and name.startswith("project."):
            # another window docked over the Project window takes its tab back each time it opens
            await ed.call("focus_window", window="project")
        for key, rect in ed.ui_items().items():
            if key == name or (prefix and key.startswith(name)):
                return key, rect
        await ui.frames(2)
    ed.fail(f"the UI probe never named {name}: {sorted(ed.ui_items())[:40]}")


async def reveal_in_project_window(ed: Editor, ref: str) -> None:
    """the Project window at ref (a folder, or a file's folder with it pinged), through a bookmark's navigate,
    which needs no scrolling or folder clicks"""
    if not Path(ref).suffix:
        (ed.project / ref).mkdir(parents=True, exist_ok=True)
    await ed.call("sync_files")
    listed = (await ed.call("edit_bookmarks", action="list"))["bookmarks"]
    if not any(str(b.get("path", "")).rstrip("/").endswith(ref.rstrip("/")) for b in listed):
        listed = (await ed.call("edit_bookmarks", action="add", ref=ref))["bookmarks"]
    index = next(b["index"] for b in listed if str(b.get("path", "")).rstrip("/").endswith(ref.rstrip("/")))
    await ed.call("focus_window", window="project")
    await ed.call("edit_bookmarks", action="navigate", index=index)


async def open_in_project_window(ed: Editor, ui: Ui, name: str):
    """double-clicks a file of the Project window, by its probe rect"""
    await reveal_in_project_window(ed, "assets/" + str(Path(GRAPH).parent / name))
    _, rect = await wait_probe(ed, ui, "project.file." + name)
    await ui.double_click(mid(ui, rect))


async def test_opens_from_the_catalog(ed: Editor):
    """AC43: window.noiseGraph is in the catalog and shows the toolbar, also with no graph yet."""
    shortcuts = await ed.call("list_shortcuts")
    ids = json.dumps(shortcuts)
    ed.check("window.noiseGraph" in ids, "window.noiseGraph in the catalog")
    ui = Ui(ed)
    await ed.call("run_command", id="window.noiseGraph")
    ed.check(await ui.find("Seed", WINDOW) is not None, "Seed in the toolbar")


async def test_create_from_the_project_window_has_a_guid(ed: Editor):
    """AC43, R62: Create > Noise Graph > Blank and a preset, each a file with a GUID and one undo step."""
    ui = Ui(ed)
    # into the assets folder: the created files land where the Project window is
    await reveal_in_project_window(ed, "assets")
    await ui.frames(10)

    await ed.call("run_command", id="assets.createNoiseGraph")
    await ui.frames(6)
    await ui.key("Escape")      # the name field the create leaves focused
    blanks = list(ed.project.rglob("New Noise Graph*.noisegraph"))
    ed.check(blanks, "the Blank graph exists")
    blank = json.loads(blanks[0].read_text())
    ed.check_eq(sorted(n["type"] for n in blank["nodes"]), ["constant", "output_height"], "Blank is a Constant into an output_height")
    info = await ed.call("get_asset_info", path=str(blanks[0].relative_to(ed.project)))
    ed.check(info.get("guid"), f"a GUID: {info}")

    # the menu itself
    create = await wait_probe(ed, ui, "project.create")
    await ui.click(mid(ui, create[1]))
    menu = await wait_probe(ed, ui, "project.create.noisegraph")
    await ui.move(mid(ui, menu[1]))
    item = await wait_probe(ed, ui, "project.create.noisegraph.mountains")
    await ui.click(mid(ui, item[1]))
    await ui.frames(6)
    await ui.key("Escape")
    made = list(ed.project.rglob("Mountains*.noisegraph"))
    ed.check(made, "a graph from the preset")
    ed.check_eq(json.loads(made[0].read_text())["metadata"]["preset"]["name"], "Mountains", "it is the preset's copy")
    info = await ed.call("get_asset_info", path=str(made[0].relative_to(ed.project)))
    ed.check(info.get("guid"), f"a GUID: {info}")

    undone = await ed.call("undo", scope="project")
    ed.check(undone is not None, "undo ran")
    ed.check(not made[0].exists(), "one undo takes the created file back")


async def test_opens_from_a_noisegraph_and_finds_the_controls(ed: Editor):
    """AC43: a .noisegraph's double-click; find_ui_item finds noisegraph.canvas, Seed, Preview and Apply; thumbnails come."""
    ui = Ui(ed)
    write_graph(ed)
    await open_in_project_window(ed, ui, Path(GRAPH).name)
    for label in ("noisegraph.canvas", "Seed", "Preview", "Apply"):
        ed.check(await ui.find(label, WINDOW) is not None, f"find_ui_item finds {label}")
    await wait_probe(ed, ui, "noisegraph.node.n1")
    await ui.frames(30)
    await save_shot(ed, "noise-graph-thumbnails.png")


async def test_an_edit_undoes(ed: Editor):
    """AC43: right-click on the background opens the search; a pick adds a node as one project undo step, which undoes."""
    ui = Ui(ed)
    target = write_graph(ed)
    await open_in_project_window(ed, ui, Path(GRAPH).name)
    _, canvas = await wait_probe(ed, ui, "noisegraph.canvas")
    before = target.read_text()
    x, y = canvas[0] + 12, canvas[3] - 12
    await ui.click(ui.at([x, y, x, y]), button="3")
    await wait_probe(ed, ui, "noisegraph.search")
    # R59: the filter field has the focus; a name narrows the list and Enter adds its first row
    await ui.type("simplex")
    await ui.frames(4)
    await save_shot(ed, "noise-graph-search.png")
    await ui.key("Return")
    await ui.frames(6)
    after = json.loads(target.read_text())
    ed.check(any(n["type"] == "simplex" for n in after["nodes"]), "the node was added")
    await ed.call("undo", scope="project")
    await ui.frames(4)
    ed.check_eq(target.read_text(), before, "one undo restores the file byte for byte")


async def test_an_error_shows_on_its_node_and_not_in_the_console(ed: Editor):
    """AC43, R58: an error's message through the UI probe, Apply refused, and nothing in the Console."""
    ui = Ui(ed)
    broken = json.loads(json.dumps(SIMPLE))
    broken["edges"] = [e for e in broken["edges"] if e["id"] != "e4"]     # the remap's input is required
    write_graph(ed, broken, "aaa_graphs/Broken.noisegraph")
    await open_in_project_window(ed, ui, "Broken.noisegraph")
    key, _ = await wait_probe(ed, ui, "noisegraph.error.n2", prefix=True)
    ed.check(": " in key and len(key.split(": ", 1)[1]) > 3, f"the probe carries the message: {key}")
    await ui.frames(10)
    await save_shot(ed, "noise-graph-error.png")
    apply = await ui.find("Apply", WINDOW)
    ed.check(apply is not None and apply.get("enabled") is False, f"Apply is disabled: {apply}")
    console = await ed.call("read_console", levels=["error"])
    entries = console.get("entries", console.get("lines", []))
    noisy = [e for e in entries if "graph" in json.dumps(e).lower() or "required" in json.dumps(e).lower()]
    ed.check(not noisy, f"nothing about the graph in the Console: {noisy}")


async def test_closed_costs_nothing(ed: Editor):
    """AC38, R50: closed, no thumbnail texture is left (its jobs: NoiseGraphDocument.PreviewsAreSkippedWhenClosed)."""
    ui = Ui(ed)
    await ed.call("run_command", id="window.noiseGraph")
    write_graph(ed)
    await open_in_project_window(ed, ui, Path(GRAPH).name)
    await wait_probe(ed, ui, "noisegraph.node.n1")
    await ui.frames(30)
    tab = await ui.find(WINDOW)
    ed.check(tab is not None, "the window's tab")
    tab_rect = tab.get("rect", tab)
    # its tab's close button, as a person closes it (test_undo_ui.py's dock_close_button finds them the same way)
    buttons = [i["rect"] for i in (await ed.call("find_ui_item", label="#CLOSE"))["items"] if i.get("visible", True)]
    on_tab = [b for b in buttons
              if tab_rect["y"] <= b["y"] + b["height"] / 2 <= tab_rect["y"] + tab_rect["height"]
              and tab_rect["x"] <= b["x"] + b["width"] / 2 <= tab_rect["x"] + tab_rect["width"] + b["width"]]
    ed.check(on_tab, f"a close button on the {WINDOW} tab {tab_rect}: {buttons}")
    # GPU texture bytes are one total for the whole editor: read them just around the close, so nothing else the
    # editor loads in between is counted
    opened = await ed.call("get_memory_stats", prefix="Texture", residency="Gpu")
    await ui.click(ui.centre(on_tab[0]))
    closed_items = None
    for _ in range(60):
        await ui.frames(2)
        closed_items = await ui.find("noisegraph.canvas", WINDOW, frames=2)
        if closed_items is None:
            break
    ed.check(closed_items is None, "the window closed")
    await ui.frames(4)
    after = await ed.call("get_memory_stats", prefix="Texture", residency="Gpu")
    before_bytes = sum(r["current"] for r in opened["records"])
    after_bytes = sum(r["current"] for r in after["records"])
    # at least the three thumbnails of 64 x 64 x 4 bytes went
    ed.check(after_bytes <= before_bytes - 3 * 64 * 64 * 4, f"the thumbnails' textures went: {before_bytes} -> {after_bytes}")
    # No job: counted by the window's own preview service, not a whole lane other editor work shares, in
    # NoiseGraphDocument.PreviewsAreSkippedWhenClosed (a closed window submits nothing, and closing cancels the
    # running preview); closed, NoiseGraphWindow::show calls update(false) once and nothing that previews after it
