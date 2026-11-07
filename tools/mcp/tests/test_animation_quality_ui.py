"""S74 R45-R47 (AC18), with real input in an isolated editor: the .anim Inspector's info line holds what
get_animation.stats says; each Root Motion toggle is one undo step; Reduce Keys... previews without
writing, and what it writes passes check_animation compare_to with the number it showed; Animation
Statistics' rows equal the tools'. Screenshots go to JE_EVIDENCE_DIR, else the test's scratch folder.

Runs on a scratch copy of tools/fixtures/pose-animation. Needs xdotool and the harness's Xvfb.
"""

from __future__ import annotations

import hashlib
import json
import os
import shutil
from pathlib import Path

import ui_input
from harness import Editor, REPO, skip

CI = True
EDITOR = {"project": REPO / "tools" / "fixtures" / "pose-animation", "xvfb": "harness", "ui_probe": True}
TIMEOUT = 900

MODEL = "assets/models/hero_animated.gltf"
WALK = f"{MODEL}#Walk"
CLIP = "assets/anim/dense.anim"
OUTPUT = "assets/anim/dense.opt.anim"
DOT = " · "
S: dict = {}


def evidence(ed: Editor) -> Path:
    folder = Path(os.environ.get("JE_EVIDENCE_DIR") or ed.scratch.root)
    folder.mkdir(parents=True, exist_ok=True)
    return folder


async def shot(ed: Editor, name: str) -> None:
    await ed.call("wait", frames=4)
    await ed.screenshot("editor", path=str(evidence(ed) / f"{name}.png"))


def text_of(item: dict) -> str:
    """a drawn value's text: its label, without the ## id and the line breaks the panel wrapped it with"""
    return item["label"].split("##")[0].replace("\n", "")


def thousands(value: int) -> str:
    return f"{value:,}"


def bytes_text(value: int) -> str:
    if value < 1024:
        return f"{thousands(value)} B"
    if value < 1024 * 1024:
        return f"{value / 1024:.0f} KB"
    return f"{value / (1024 * 1024):.1f} MB"


def tree_hash(root: Path) -> dict:
    return {str(p.relative_to(root)): hashlib.sha1(p.read_bytes()).hexdigest()
            for p in sorted(root.rglob("*")) if p.is_file() and ".jasperengine" not in p.parts}


async def project_window(ed: Editor) -> dict:
    return (await ed.call("editor_status"))["ui"]["project_window"]


async def inspect_asset(ed: Editor, ui, folder: str, name: str) -> None:
    """clicks `name` in the Project window's listing of `folder`, where editor_status says they are drawn"""
    await ed.call("focus_window", window="project")

    def node(state, path):
        return next((n for n in state["tree"] if n["path"] == path), None)

    def entry(state):
        return next((e["rect"] for e in state["entries"] if e["name"] == name), None)

    state = await project_window(ed)
    for _ in range(40):
        if state["drawn"] and node(state, "assets"):
            break
        await ed.call("wait", frames=3)
        state = await project_window(ed)
    parts = folder.split("/")
    for depth in range(1, len(parts)):
        parent = node(state, "/".join(parts[:depth]))
        ed.check(parent, f"{'/'.join(parts[:depth])} in the tree")
        if not node(state, "/".join(parts[:depth + 1])):
            await ui.click((int(parent["arrow"][0]), int(parent["arrow"][1])))
            state = await project_window(ed)
    for _ in range(10):
        if state["directory"] == folder and entry(state):
            break
        target = node(state, folder)
        ed.check(target, f"{folder} in the tree: {[n['path'] for n in state['tree']]}")
        rect = target["rect"]
        await ui.click((int(rect[0] + rect[2] / 2), int(rect[1] + rect[3] / 2)))
        await ed.call("wait", frames=3)
        state = await project_window(ed)
    rect = entry(state)
    ed.check(rect, f"{name} listed in {state['directory']}: {[e['name'] for e in state['entries']]}")
    await ui.click((int(rect[0] + rect[2] / 2), int(rect[1] + rect[3] / 2)))
    await ed.call("wait", frames=10)


async def drawn(ed: Editor, ui, label: str, window: str | None = None, frames: int = 600) -> dict:
    """the item, scrolled into its window's view (a table row below the fold is drawn, not visible)"""
    return await ui.reveal(label, window, frames=frames)


async def setup(ed: Editor):
    if "ui" in S:
        return S["ui"]
    if not shutil.which("xdotool") or not ed._xvfb or not ed._xvfb.display:
        skip("needs xdotool and the harness's Xvfb")
    info = await ed.call("get_animation", clip=WALK, max_keys=1)
    times = [round(i * info["length"] / 63, 6) for i in range(64)]
    sampled = await ed.call("get_animation", clip=WALK, times=times, max_keys=1)
    by_index = {t["index"]: t for t in info["tracks"]}
    tracks = []
    for row in sampled["samples"]["tracks"]:
        track = by_index[row["index"]]
        if track["channel"] not in ("position", "rotation", "scale"):
            continue
        keys = [[t, v["quat"] if isinstance(v, dict) else v] for t, v in zip(times, row["values"])]
        tracks.append({"path": track["path"], "channel": track["channel"], "keys": keys})
    await ed.call("write_file", path=CLIP, content=json.dumps({
        "format": "jasperengine.anim", "version": 1, "guid": "5a74c0de5a74c0de5a74c0de5a74c0de", "wrap": "loop", "frame_rate": 30,
        "tracks": tracks, "events": [],
        "preview": {"model": "models/hero_animated.gltf"}}))
    S["ui"] = ui_input.for_editor(ed)
    return S["ui"]


async def test_the_info_line_is_what_the_tools_say(ed: Editor):
    ui = await setup(ed)
    await inspect_asset(ed, ui, "assets/anim", "dense.anim")
    stats = (await ed.call("get_animation", clip=CLIP, max_keys=1))["stats"]
    line = text_of(await drawn(ed, ui, "##clip_info_line", "Inspector"))
    parts = line.split(DOT.strip())
    parts = [p.strip() for p in parts]
    ed.check_eq(parts[0], f"{stats['length_s']:.2f} s", f"Length: {line}")
    ed.check_eq(parts[1], "30 Hz", f"{line}")
    ed.check_eq(parts[2], f"{stats['tracks']} tracks", f"{line}")
    ed.check_eq(parts[3], f"{thousands(stats['keys'])} keys", f"{line}")
    ed.check_eq(parts[4], f"Memory {bytes_text(stats['runtime_bytes'])}", f"{line}")
    ed.check_eq(parts[5], f"File {bytes_text(stats['file_bytes'])}", f"{line}")
    await shot(ed, "ac18-1-info-line")


async def test_root_motion_toggles_are_one_undo_step(ed: Editor):
    ui = await setup(ed)
    file = Path(ed.project) / CLIP
    before = file.read_bytes()
    await ui.click_revealed("##clip_asset_root_motion", "Inspector")
    await ed.call("wait", frames=5)
    await shot(ed, "ac18-2-root-motion-open")
    await ui.click_revealed("##clip_root_xz", "Inspector")
    await ed.call("wait", frames=10)
    after = json.loads(file.read_text())
    ed.check_eq(after["root_motion"]["bake_position_xz"], True, "the toggle wrote the block")
    await ed.call("undo", scope="project")
    await ed.call("wait", frames=5)
    ed.check_eq(file.read_bytes(), before, "one undo step restores the bytes")
    await shot(ed, "ac18-3-root-motion-undone")


async def test_reduce_keys_previews_writes_and_proves(ed: Editor):
    ui = await setup(ed)
    root = Path(ed.project)
    before = tree_hash(root)
    await ui.click_revealed("##clip_asset_reduce", "Inspector")
    memory = text_of(await drawn(ed, ui, "##reduce_memory", "Reduce Keys"))
    keys = text_of(await drawn(ed, ui, "##reduce_keys", "Reduce Keys"))
    max_error = text_of(await drawn(ed, ui, "##reduce_max_error", "Reduce Keys"))
    await drawn(ed, ui, "##reduce_mean_error", "Reduce Keys")
    await drawn(ed, ui, "##reduce_worst_joint", "Reduce Keys")
    await shot(ed, "ac18-4-reduce-keys-preview")
    after = tree_hash(root)
    ed.check_eq(sorted(k for k in set(before) | set(after) if before.get(k) != after.get(k)), [], "the preview writes nothing")

    # the same numbers the tool gives for the same settings
    tool = (await ed.call("optimize_animation", clips=[CLIP]))["clips"][0]
    ed.check_eq(memory, f"{bytes_text(tool['bytes_before'])} -> {bytes_text(tool['bytes_after'])}", "Memory")
    ed.check_eq(keys, f"{thousands(tool['keys_before'])} -> {thousands(tool['keys_after'])}", "Keys")
    ed.check_eq(max_error, f"{tool['max_error_mm']:.2f} mm", "Max Error")

    await ui.click_item("##reduce_write", "Reduce Keys")
    for _ in range(100):
        if (root / OUTPUT).exists():
            break
        await ed.call("wait", frames=5)
    ed.check((root / OUTPUT).exists(), f"{OUTPUT} written")
    await ed.call("wait", frames=30)
    await shot(ed, "ac18-5-reduce-keys-written")
    await ui.click_item("##reduce_close", "Reduce Keys")
    await ed.call("wait", frames=5)

    proof = await ed.call("check_animation", clips=[OUTPUT], compare_to=CLIP)
    ed.check(proof["ok"] is True, f"{proof['findings']}")
    compare = proof["clips"][0]["compare"]
    ed.check(compare["within_tolerance"] is True, f"{compare}")
    ed.check_eq(f"{compare['max_error_mm']:.2f} mm", max_error, "the dialog's number, from the file")

    # one undo step takes it back
    await ed.call("undo", scope="project")
    ed.check(not (root / OUTPUT).exists(), "undo removes it")
    await ed.call("redo", scope="project")
    ed.check((root / OUTPUT).exists(), "redo")


async def test_statistics_rows_equal_the_tools(ed: Editor):
    ui = await setup(ed)
    await ed.call("focus_window", window="animation_statistics")
    await ed.call("wait", frames=5)
    await shot(ed, "ac18-6-statistics-empty")
    await ui.click_item("##stat_scan", "Animation Statistics")
    tool = (await ed.call("check_animation", checks=["stats"], top=1000))
    window = "Animation Statistics"
    for clip in (CLIP, WALK):
        memory = text_of(await drawn(ed, ui, f"##stat_memory_{clip}", window))
        entry = next(c for c in tool["clips"] if c["clip"] == clip)
        ed.check_eq(memory, bytes_text(entry["runtime_bytes"]), f"{clip} Memory")
        ed.check_eq(text_of(await drawn(ed, ui, f"##stat_keys_{clip}", window)), thousands(entry["keys"]), f"{clip} Keys")
        ed.check_eq(text_of(await drawn(ed, ui, f"##stat_tracks_{clip}", window)), thousands(entry["tracks"]), f"{clip} Tracks")
        ed.check_eq(text_of(await drawn(ed, ui, f"##stat_length_{clip}", window)), f"{entry['length_s']:.2f} s", f"{clip} Length")
    # the reduced clip shows what its reduction measured; the source has no such number
    record = json.loads((Path(ed.project) / OUTPUT).read_text())["optimize"]
    ed.check_eq(text_of(await drawn(ed, ui, f"##stat_error_{OUTPUT}", window)), f"{record['max_error_mm']:.2f} mm", "Max Error")
    ed.check_eq(text_of(await drawn(ed, ui, f"##stat_error_{CLIP}", window)), "-", "no record, no number")
    totals = text_of(await drawn(ed, ui, "##stat_totals", window))
    ed.check(f"Totals: {thousands(tool['totals']['clips'])} clips" in totals, f"{totals}")
    ed.check(f"Memory {bytes_text(tool['totals']['runtime_bytes'])}" in totals, f"{totals}")
    await shot(ed, "ac18-7-statistics")

    # double-click selects: the Inspector shows the clip's file
    item = await drawn(ed, ui, f"##stat_clip_{CLIP}", window)
    await ui.double_click(ui.centre(item["rect"]))
    await ed.call("wait", frames=20)
    await ed.call("focus_window", window="inspector")
    await drawn(ed, ui, "##clip_info_line", "Inspector")
    await shot(ed, "ac18-8-statistics-selects")
