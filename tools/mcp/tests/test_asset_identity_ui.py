"""The Project window's asset identity UI driven with real mouse and keyboard input
(docs/specs/S04-asset-identity.md AC17): xdotool clicks the rects editor_status reports
(ui.project_window), the assertions read the editor's state back.

- the listing hides the .meta files
- right-click > Find References opens the References panel on the file, listing its referrers
- right-click > Copy GUID puts the file's GUID on the clipboard (pasted into the search box)
- right-click > Rename: the file keeps its GUID and its meta goes along, the thumbnail cache
  gains no file (thumbnails are keyed by GUID), and no referrer is written

Screenshots of each step go to build/test-results/asset-identity-ui/.
"""

from __future__ import annotations

import asyncio
import os
import shutil
import subprocess
from pathlib import Path

import yaml

from harness import Editor, skip

REPO = Path(__file__).resolve().parents[3]
EDITOR = {"project": REPO / "tools" / "fixtures" / "legacy_paths_project", "xvfb": "harness"}
TIMEOUT = 600
CI = True

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


def centre(rect: list) -> tuple[str, str]:
    x, y, w, h = rect
    return str(int(x + w / 2)), str(int(y + h / 2))


async def frames(ed: Editor, count: int = 2) -> None:
    """At least `count` editor frames: each call is answered between two frames (a loaded
    machine draws slowly, and ImGui takes one input event per frame)"""
    for _ in range(count):
        await ed.call("editor_status")


async def click_at(ed: Editor, x: str, y: str, button: str = "1") -> None:
    xdo(ed, "mousemove", "--window", window(ed), x, y)
    await frames(ed)            # the hover is seen before the press
    xdo(ed, "mousedown", button)
    await frames(ed)
    xdo(ed, "mouseup", button)
    await frames(ed)


async def click(ed: Editor, rect: list, button: str = "1") -> None:
    x, y = centre(rect)
    await click_at(ed, x, y, button)


async def ui(ed: Editor) -> dict:
    return (await ed.call("editor_status"))["ui"]


async def wait_ui(ed: Editor, predicate, what: str, timeout: float = 15.0) -> dict:
    state = None
    for _ in range(int(timeout / 0.25)):
        state = await ui(ed)
        if predicate(state):
            return state
        await asyncio.sleep(0.25)
    ed.fail(f"{what}; ui: {state}")


def entry(state: dict, name: str) -> list | None:
    for item in state["project_window"]["entries"]:
        if item["name"] == name:
            return item["rect"]
    return None


def menu_item(state: dict, label: str) -> list | None:
    for item in state["project_window"]["context_menu"]:
        if item["label"] == label:
            return item["rect"]
    return None


def meta_guid(ed: Editor, rel: str) -> str:
    return yaml.safe_load((ed.project / (rel + ".meta")).read_text())["guid"]


SHOTS = REPO / "build" / "test-results" / "asset-identity-ui"


async def shot(ed: Editor, name: str) -> None:
    SHOTS.mkdir(parents=True, exist_ok=True)
    await ed.screenshot("editor", SHOTS / f"{name}.png", focus=False, max_width=0)


def tree_node(state: dict, path: str) -> dict | None:
    for node in state["project_window"]["tree"]:
        if node["path"] == path:
            return node
    return None


async def open_textures(ed: Editor) -> dict:
    """Shows assets/textures from the folder tree: opens assets with its arrow, clicks textures"""
    await ed.call("focus_window", window="project")
    state = await wait_ui(ed, lambda s: s["project_window"]["drawn"] and tree_node(s, "assets"), "the tree shows assets")
    if state["project_window"]["directory"] != "assets/textures":
        if not tree_node(state, "assets/textures"):
            x, y = tree_node(state, "assets")["arrow"]
            await click_at(ed, str(int(x)), str(int(y)))
            state = await wait_ui(ed, lambda s: tree_node(s, "assets/textures"), "assets opened in the tree")
        await click(ed, tree_node(state, "assets/textures")["rect"])
        state = await wait_ui(ed, lambda s: s["project_window"]["directory"] == "assets/textures" and entry(s, "mask.png"),
                              "assets/textures shown")
    return state


async def context_menu(ed: Editor, name: str, label: str) -> None:
    state = await ui(ed)
    await click(ed, entry(state, name), button="3")
    state = await wait_ui(ed, lambda s: menu_item(s, label), f"the context menu of {name} shows {label}")
    STATE["menu"] = state["project_window"]["context_menu"]
    await shot(ed, f"ui-menu-{label.lower().replace(' ', '-')}")
    await click(ed, menu_item(state, label))


async def test_listing_hides_metas(ed: Editor):
    if not shutil.which("xdotool") or not ed._xvfb or not ed._xvfb.display:
        skip("needs xdotool and the harness Xvfb")
    await ed.call("editor_status")
    state = await open_textures(ed)
    names = [item["name"] for item in state["project_window"]["entries"]]
    ed.check({"mask.png", "brick.png", "rock.png"} <= set(names), f"listing: {names}")
    ed.check(not any(n.endswith(".meta") for n in names), f"metas listed: {names}")
    ed.check((ed.project / "assets/textures/mask.png.meta").exists(), "the meta exists on disk")
    await shot(ed, "ui-listing")


async def test_find_references_from_the_menu(ed: Editor):
    await open_textures(ed)
    await context_menu(ed, "mask.png", "Find References")
    labels = [item["label"] for item in STATE["menu"]]
    ed.check({"Copy GUID", "Find References", "Rename"} <= set(labels), f"menu: {labels}")
    state = await wait_ui(ed, lambda s: s["references"]["open"] and s["references"]["results"],
                          "the References panel lists the referrers")
    refs = state["references"]
    ed.check_eq(refs["guid"], meta_guid(ed, "assets/textures/mask.png"))
    ed.check(refs["asset"].endswith("assets/textures/mask.png"), f"{refs['asset']}")
    referrers = {r["referrer"] for r in refs["results"]}
    ed.check({"Closed.scene", "materials/Brick.material", "prefabs/Lamp.prefab"} <= referrers, f"{referrers}")
    await shot(ed, "ui-find-references")


async def test_copy_guid_from_the_menu(ed: Editor):
    await open_textures(ed)
    await context_menu(ed, "rock.png", "Copy GUID")
    # pasted into the Project window's search box: the search text is the GUID
    state = await ui(ed)
    await click(ed, state["project_window"]["search_box"])
    xdo(ed, "key", "ctrl+a")
    await frames(ed)
    xdo(ed, "key", "ctrl+v")
    rock = meta_guid(ed, "assets/textures/rock.png")
    await wait_ui(ed, lambda s: s["project_window"]["search"] == rock, f"the pasted search text is rock.png's GUID {rock}")
    await shot(ed, "ui-copy-guid")
    # cleared again
    state = await ui(ed)
    await click(ed, state["project_window"]["search_box"])
    xdo(ed, "key", "ctrl+a")
    await frames(ed)
    xdo(ed, "key", "BackSpace")
    await wait_ui(ed, lambda s: s["project_window"]["search"] == "", "the search cleared")


async def test_rename_from_the_menu(ed: Editor):
    await open_textures(ed)
    brick = meta_guid(ed, "assets/textures/brick.png")
    thumbs = ed.project / ".jasperengine" / "thumbs"

    def thumb_files() -> set[str]:
        return {p.name for p in thumbs.glob("*")} if thumbs.exists() else set()

    # the listing's thumbnails generated first (they come one at a time)
    before = thumb_files()
    for _ in range(40):
        await asyncio.sleep(0.25)
        now = thumb_files()
        if now == before and now:
            break
        before = now
    referrers = ["Main.scene", "Closed.scene", "prefabs/Lamp.prefab", "materials/Brick.material"]
    mtimes = {r: (ed.project / r).stat().st_mtime_ns for r in referrers}

    await context_menu(ed, "brick.png", "Rename")
    await frames(ed, 4)         # the dialog opens and focuses its field
    # the dialog preselects the name without its extension
    xdo(ed, "type", "--delay", "50", "h2")
    await frames(ed, 3)
    xdo(ed, "key", "Return")
    state = await wait_ui(ed, lambda s: entry(s, "h2.png") and not entry(s, "brick.png"), "h2.png listed instead of brick.png")
    names = [item["name"] for item in state["project_window"]["entries"]]
    ed.check(not any(n.endswith(".meta") for n in names), f"metas listed: {names}")
    ed.check((ed.project / "assets/textures/h2.png.meta").exists(), "the meta went along")
    ed.check(not (ed.project / "assets/textures/brick.png.meta").exists(), "the old meta stayed")
    ed.check_eq(meta_guid(ed, "assets/textures/h2.png"), brick, "the GUID changed")
    await asyncio.sleep(2.0)   # time for a regeneration, if one were asked
    ed.check_eq(thumb_files(), before, "the rename made a thumbnail")
    ed.check_eq({r: (ed.project / r).stat().st_mtime_ns for r in referrers}, mtimes, "a referrer was written")
    resolved = await ed.call("resolve_asset", asset=brick)
    ed.check_eq(resolved["path"], "assets/textures/h2.png")
    await shot(ed, "ui-rename")
    await ed.call("undo", scope="project")
    await wait_ui(ed, lambda s: entry(s, "brick.png"), "the rename undone")
