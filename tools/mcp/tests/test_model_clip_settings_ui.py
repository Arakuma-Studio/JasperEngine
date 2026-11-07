"""The Inspector's model clip settings with real input (S74 R30, R31, R48, AC14): clicking a model in
the Project window and its Animation tab shows its clips' rows, whose values are get_asset_info's;
the loop suggestion shows and changes nothing until Use, and Use changes only the working copy
until Apply; a multi-selection shows a mixed Wrap Mode, and one Apply over it is one undo step.
xdotool clicks the rects editor_status and find_ui_item report; the screenshots of each step go to
SHOTS (default build/test-results/model-clip-settings-ui/, JASPER_MODEL_CLIP_SHOTS overrides it).
Runs on a scratch copy of tools/fixtures/pose-animation.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
from pathlib import Path

from harness import Editor, REPO, skip
import ui_input

EDITOR = {"project": REPO / "tools" / "fixtures" / "pose-animation", "xvfb": "harness"}
TIMEOUT = 900
CI = True
SHOTS = Path(os.environ.get("JASPER_MODEL_CLIP_SHOTS", REPO / "build" / "test-results" / "model-clip-settings-ui"))

MODEL = "assets/models/hero_animated.gltf"
SIDECAR = MODEL + ".clips"
WRAP_LABELS = {"once": "Once", "loop": "Loop", "ping_pong": "Ping Pong", "clamp_forever": "Clamp Forever"}
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


async def click(ed: Editor, rect, modifier: str | None = None) -> None:
    window(ed)
    ui = ui_input.for_editor(ed)
    await ui.click(ui.centre(rect), modifier=modifier)


async def wait_ui(ed: Editor, predicate, what: str) -> dict:
    state = None
    for _ in range(120):
        state = (await ed.call("editor_status"))["ui"]
        if predicate(state):
            return state
        await ui_input.frames(ed, 2)
    ed.fail(f"{what}; ui: {state}")


def tree_node(state: dict, path: str) -> dict | None:
    return next((node for node in state["project_window"]["tree"] if node["path"] == path), None)


def entry(state: dict, name: str) -> list | None:
    return next((item["rect"] for item in state["project_window"]["entries"] if item["name"] == name), None)


async def item(ed: Editor, label: str) -> dict:
    window(ed)
    return await ui_input.for_editor(ed).reveal(label, "Inspector", frames=480)


async def text_of(ed: Editor, label: str) -> str:
    """The text a cell draws (its label before the ## id)"""
    return (await item(ed, label))["label"].split("##")[0]


async def expect_text(ed: Editor, label: str, expected: str, what: str) -> None:
    """The cell's text once it is `expected` (rows are measured a few a frame, and read again after a write)"""
    text = ""
    for _ in range(60):
        text = await text_of(ed, label)
        if text == expected:
            return
        await ui_input.frames(ed, 2)
    ed.check_eq(text, expected, what)


async def shot(ed: Editor, name: str) -> None:
    SHOTS.mkdir(parents=True, exist_ok=True)
    await ed.screenshot("editor", SHOTS / f"{name}.png", focus=False, max_width=0)


def thousands(value: int) -> str:
    return f"{value:,}"


def memory(value: int) -> str:
    if value < 1024:
        return f"{value:,} B"
    if value < 1024 * 1024:
        return f"{value / 1024:.0f} KB"
    return f"{value / (1024 * 1024):.1f} MB"


def root_motion(value: dict | None) -> str:
    value = value or {"bone": "", "bake_rotation": False, "bake_position_y": True, "bake_position_xz": False}
    text = value["bone"].rsplit("/", 1)[-1] if value["bone"] else "Auto"
    baked = [name for key, name in (("bake_rotation", "Rotation"), ("bake_position_y", "Y"), ("bake_position_xz", "XZ")) if value[key]]
    return text + (", Bake " + " ".join(baked) if baked else "")


async def rows_equal_the_tool(ed: Editor) -> None:
    animations = (await ed.call("get_asset_info", path=MODEL))["model"]["animations"]
    for index, animation in enumerate(animations):
        name = animation["name"]
        await expect_text(ed, f"##model_clip.{index}", name, "Clip")
        await expect_text(ed, f"##model_clip.wrap.{index}", WRAP_LABELS[animation["wrap"]], f"Wrap Mode of {name}")
        await expect_text(ed, f"##model_clip.root_motion.{index}", root_motion(animation["root_motion"]), f"Root Motion of {name}")
        await expect_text(ed, f"##model_clip.memory.{index}", memory(animation["runtime_bytes"]), f"Memory of {name}")
        await expect_text(ed, f"##model_clip.keys.{index}", thousands(animation["keys"]), f"Keys of {name}")


async def test_model_clip_settings_ui(ed: Editor):
    if not shutil.which("xdotool") or not ed._xvfb or not ed._xvfb.display:
        skip("needs xdotool and the harness Xvfb")
    await ed.call("tool_groups", enable=["editor_ux"])
    sidecar = ed.project / SIDECAR

    await ed.call("focus_window", window="project")
    state = await wait_ui(ed, lambda s: s["project_window"]["drawn"] and tree_node(s, "assets"), "the tree shows assets")
    if not tree_node(state, "assets/models"):
        x, y = tree_node(state, "assets")["arrow"]
        window(ed)
        ui = ui_input.for_editor(ed)
        await ui.click(ui.at((x, y)))
        state = await wait_ui(ed, lambda s: tree_node(s, "assets/models"), "assets opened in the tree")
    await click(ed, tree_node(state, "assets/models")["rect"])
    state = await wait_ui(ed, lambda s: entry(s, "hero_animated.gltf"), "assets/models shown")
    await click(ed, entry(state, "hero_animated.gltf"))
    await click(ed, (await item(ed, "Animation"))["rect"])
    await item(ed, "##model_clip.0")

    # the rows print get_asset_info's numbers (and the measuring has reached every row)
    await rows_equal_the_tool(ed)
    loop_match = await text_of(ed, "##model_clip.loop_match.0")
    ed.check(loop_match.endswith(" mm") and float(loop_match.split()[0]) < 10.0, f"Walk's ends match: {loop_match}")

    # Walk is selected first: a cycle's name, its ends match, no setting -> the suggestion, and nothing written
    await item(ed, "Use##model_clip_suggestion")
    ed.check(not sidecar.exists(), "the suggestion writes nothing")
    await ui_input.frames(ed, 6)
    await shot(ed, "1-suggestion")
    await click(ed, (await item(ed, "Use##model_clip_suggestion"))["rect"])
    await expect_text(ed, "##model_clip.wrap.0", "Loop", "Use sets the working copy")
    await ui_input.frames(ed, 6)
    ed.check(not sidecar.exists(), "Use only edits the working copy")
    await shot(ed, "2-use-pending")
    await click(ed, (await item(ed, "Apply##model_clip_settings"))["rect"])
    await ui_input.frames(ed, 6)
    ed.check_eq(json.loads(sidecar.read_text())["clips"], {"Walk": {"wrap": "loop"}}, "Apply writes the sidecar")
    await rows_equal_the_tool(ed)

    # Walk and Idle: Wrap Mode mixed, then Ping Pong for both with one Apply
    await click(ed, (await item(ed, "##model_clip.1"))["rect"], modifier="ctrl")
    await ui_input.frames(ed, 6)
    await shot(ed, "3-mixed")
    await click(ed, (await item(ed, "##model_clip_wrap"))["rect"])
    await click(ed, (await ui_input.for_editor(ed).reveal("Ping Pong", None, frames=120))["rect"])
    await click(ed, (await item(ed, "Apply##model_clip_settings"))["rect"])
    await ui_input.frames(ed, 6)
    ed.check_eq(json.loads(sidecar.read_text())["clips"], {"Walk": {"wrap": "ping_pong"}, "Idle": {"wrap": "ping_pong"}}, "both clips")
    await rows_equal_the_tool(ed)
    await shot(ed, "4-bulk-applied")

    # one undo step for the bulk Apply, one for the first
    await ed.call("undo", scope="project")
    ed.check_eq(json.loads(sidecar.read_text())["clips"], {"Walk": {"wrap": "loop"}}, "one undo: the bulk Apply goes whole")
    await ui_input.frames(ed, 6)
    await expect_text(ed, "##model_clip.wrap.1", "Once", "the rows follow the undo")
    await ed.call("undo", scope="project")
    ed.check(not sidecar.exists(), "the second undo: no sidecar")
    await rows_equal_the_tool(ed)
