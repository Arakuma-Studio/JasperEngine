"""S07: the ECS tools (get_ecs_stats, get_system_schedule, set_system_options, query_entities).

Runs against the TappyPlane fixture (a 2D Lua game). Tests run in order and share one editor.
The editor runs in the harness's Xvfb, so test_systems_window can click the Systems window's
Enabled checkbox with xdotool (AC17).
"""

import asyncio
import os
import shutil
import subprocess
from pathlib import Path

from harness import Editor, tool_meta
import ui_input

EDITOR = {"xvfb": "harness"}
TIMEOUT = 900

# screenshots of the Systems window kept as evidence (AC17)
EVIDENCE = Path(__file__).resolve().parents[3] / "build" / "evidence"

SYSTEMS = ["ActionSystem", "MeshSystem", "UISystem", "UIDocumentSystem", "RenderSystem", "PhysicsSystem", "ParticleSubsystem", "AudioSystem"]
# every scene system in schedule order: S21's Animator, upstream's pose animator, lattice deformer,
# vertex animator and fluid simulation systems, S18's splines and S19's navigation too, and upstream's
# camera shake / follow (CameraControlSystem), video player and trail renderer systems, and the Water
# Excluders' and Wakes' system (the timing checks stay on SYSTEMS: those have nothing to do in these scenes)
ALL_SYSTEMS = ["ActionSystem", "AnimatorSystem", "PoseAnimatorSystem", "SplineSystem", "MeshSystem", "LatticeSystem", "VertexAnimatorSystem",
               "CameraControlSystem", "UISystem", "UIDocumentSystem", "FluidSystem", "VideoPlayerSystem", "RenderSystem", "TrailSystem",
               "WaterInteractionSystem", "NavigationSystem", "PhysicsSystem", "ParticleSubsystem", "AudioSystem"]


def type_count(stats, name):
    for entry in stats["types"]:
        if entry["name"] == name:
            return entry["count"]
    return 0


def phase(schedule, name):
    for entry in schedule["phases"]:
        if entry["phase"] == name:
            return entry["waves"]
    raise AssertionError(f"no phase {name}: {schedule}")


def system_entry(schedule, phase_name, system):
    for wave in phase(schedule, phase_name):
        for entry in wave:
            if entry["name"] == system:
                return entry
    raise AssertionError(f"no {system} in {phase_name}")


def pointer(ed: Editor):
    """xdotool on the harness's Xvfb, or None (no xdotool, or the editor runs its own Xvfb)."""
    xvfb = getattr(ed, "_xvfb", None)
    if not shutil.which("xdotool") or not xvfb or not xvfb.display:
        return None
    env = dict(os.environ, DISPLAY=xvfb.display)

    def xdo(*args: str) -> str:
        return subprocess.run(["xdotool", *args], env=env, capture_output=True, text=True, timeout=20).stdout

    windows = xdo("search", "--name", ".").split()
    if not windows:
        return None
    xdo("windowfocus", "--sync", windows[-1])
    return xdo


def systems_row(shown, phase_name, system):
    for row in shown.get("rows", []):
        if row["phase"] == phase_name and row["system"] == system:
            return row
    raise AssertionError(f"the Systems window has no {phase_name} {system} row: {shown.get('rows')}")


async def click_enabled_checkbox(ed: Editor, xdo, phase_name: str, system: str):
    """Scrolls the Systems table until the row's Enabled checkbox is in view, then clicks it."""
    for _ in range(20):
        shown = await ed.call("focus_window", window="systems")
        row = systems_row(shown, phase_name, system)
        box = row["enabled_checkbox"]
        ui = ui_input.for_editor(ed)   # frame-paced input (tools/mcp/ui_input.py)
        if row["visible"]:
            await ui.click(ui.centre(box))
            await ed.call("wait", frames=3)
            return
        # wheel over the table (the first row's checkbox column) one notch down
        first = shown["rows"][0]["enabled_checkbox"]
        top = shown["rect"]["y"] + 60
        await ui.wheel(ui.at((first["x"] + first["width"] / 2, max(top, min(first["y"], box["y"] - 40)))), 1)
    raise AssertionError(f"the {phase_name} {system} row never scrolled into view: {row}")


ECS_TOOLS = ("get_ecs_stats", "get_system_schedule", "set_system_options", "query_entities")


async def test_ecs_tools_group(ed: Editor):
    """S31: the ECS tools are the diagnostics group's, hidden until a session enables it."""
    ed.require_tools("tool_groups", *ECS_TOOLS)
    for name in ECS_TOOLS:
        ed.check_eq(tool_meta(ed.tool(name)).get("jasper/group"), "diagnostics", f"{name}'s group")
    default = {t.name for t in await ed.tools(groups=None)}
    ed.check(not default & set(ECS_TOOLS), f"hidden by default: {sorted(default & set(ECS_TOOLS))}")
    await ed.call("tool_groups", enable=["diagnostics"])
    try:
        listed = {t.name for t in await ed.tools(groups=None)}
        ed.check(set(ECS_TOOLS) <= listed, f"listed once diagnostics is enabled: {sorted(set(ECS_TOOLS) - listed)}")
    finally:
        await ed.call("tool_groups", disable=["diagnostics"])


async def test_ecs_stats_counts(ed: Editor):
    ed.require_tools("get_ecs_stats", "create_entity", "delete_entities", "list_component_types")
    before = await ed.call("get_ecs_stats")
    ed.check(before["entities"]["total"] > 0, f"entities: {before['entities']}")
    ed.check_eq(before["entities"]["total"], before["entities"]["user"] + before["entities"]["system"], "user + system")
    ed.check_eq(before["component_types"]["max"], 256, "max component types")
    listed = await ed.call("list_component_types")
    names = listed.get("components") or listed.get("types") or []
    ed.check_eq(before["component_types"]["registered"], len(names), "registered types = list_component_types")
    ed.check(isinstance(before["change_tick"], int) and before["change_tick"] > 0, "change_tick")
    ed.check(before["bytes"]["total"] > 0, "bytes")

    made = await ed.call("create_entity", type="empty", name="EcsProbe")
    after = await ed.call("get_ecs_stats")
    ed.check_eq(after["entities"]["total"], before["entities"]["total"] + 1, "one more entity")
    ed.check_eq(type_count(after, "transform"), type_count(before, "transform") + 1, "one more Transform")

    await ed.call("delete_entities", entity=made["entity_id"])
    gone = await ed.call("get_ecs_stats")
    ed.check_eq(gone["entities"]["total"], before["entities"]["total"], "deleted again")
    ed.check_eq(type_count(gone, "transform"), type_count(before, "transform"), "its Transform is gone")


async def test_changed_since(ed: Editor):
    ed.require_tools("get_ecs_stats", "set_transform", "query_entities")
    made = await ed.call("create_entity", type="empty", name="EcsMoved")
    entity = made["entity_id"]
    other = (await ed.call("create_entity", type="empty", name="EcsStill"))["entity_id"]
    # let the editor's frame settle what creating them changed
    await ed.call("get_ecs_stats")
    tick = (await ed.call("get_ecs_stats"))["change_tick"]

    await ed.call("set_transform", entity=entity, position=[3, 4, 0])
    changed = await ed.call("query_entities", components=["transform"], changed_since_tick=tick)
    ids = [e["id"] for e in changed["entities"]]
    ed.check_in(entity, ids, "the moved GameObject changed since the tick before set_transform")
    later = await ed.call("query_entities", components=["transform"], changed_since_tick=changed["change_tick"])
    ed.check(entity not in [e["id"] for e in later["entities"]], "not changed again since")
    await ed.call("delete_entities", entities=[entity, other])


async def test_query_entities(ed: Editor):
    ed.require_tools("query_entities", "create_entity", "get_entity")
    parent = (await ed.call("create_entity", type="empty", name="EcsParent"))["entity_id"]
    child = (await ed.call("create_entity", type="empty", name="EcsChild", parent=parent))["entity_id"]

    every = await ed.call("query_entities", components=["transform"], limit=10000)
    ids = [e["id"] for e in every["entities"]]
    ed.check_in(parent, ids)
    ed.check_in(child, ids)
    ed.check(ids.index(parent) < ids.index(child), "Transform order: parents before children")
    ed.check(all({"id", "name", "path"} <= set(e) for e in every["entities"]), "id, name, path")
    again = await ed.call("query_entities", components=["transform"], limit=10000)
    ed.check_eq([e["id"] for e in again["entities"]], ids, "deterministic order")

    sprites = await ed.call("query_entities", components=["sprite"], limit=10000)
    sprite_ids = {e["id"] for e in sprites["entities"]}
    ed.check(sprite_ids, "the fixture has sprites")
    plain = await ed.call("query_entities", components=["transform"], exclude=["sprite"], limit=10000)
    plain_ids = {e["id"] for e in plain["entities"]}
    ed.check(not (plain_ids & sprite_ids), "exclude: no sprite in the result")
    ed.check_eq(plain_ids | sprite_ids, set(ids), "with and without a sprite: every GameObject")

    one = await ed.call("query_entities", components=["transform"], limit=1)
    ed.check_eq(len(one["entities"]), 1, "limit")
    ed.check(one["more"], "more after the limit")
    ed.check_eq(one["entities"][0]["id"], ids[0], "the limit keeps the order")

    await ed.check_error("query_entities", components=["nosuchcomponent"])
    await ed.check_error("query_entities", components=[])
    await ed.call("delete_entities", entities=[parent])


async def test_system_schedule_and_options(ed: Editor):
    ed.require_tools("get_system_schedule", "set_system_options", "play", "wait")
    edit = await ed.call("get_system_schedule")
    for name in ("FixedUpdate", "Update", "Draw"):
        waves = phase(edit, name)
        ed.check_eq([[s["name"] for s in wave] for wave in waves], [[s] for s in ALL_SYSTEMS], f"{name}: one wave each, in order")
    ed.check_eq(phase(edit, "PostUpdate"), [], "no PostUpdate systems")
    action = system_entry(edit, "Update", "ActionSystem")
    ed.check_in("exclusive", action["resources"])
    ed.check_in("main_thread", action["resources"])

    await ed.check_error("set_system_options", name="ActionSystem", enabled=False, contains="play")

    await ed.call("play", action="start")
    try:
        await ed.call("wait", seconds=1)
        running = await ed.call("get_system_schedule", runtime=True)
        for system in SYSTEMS:
            entry = system_entry(running, "Update", system)
            ed.check(entry["timing"]["calls"] > 0, f"{system} ran: {entry['timing']}")
            ed.check(entry["timing"]["avg_ms"] >= 0, f"{system} avg")
        ed.check(running["last_trace"], "a wave trace of the last frame")
        ed.check(any(t["system"] == "RenderSystem" for t in running["last_trace"]), "RenderSystem in the trace")

        await ed.check_error("set_system_options", name="NoSuchSystem", enabled=False, contains="ActionSystem")
        paused = await ed.call("set_system_options", name="ActionSystem", enabled=False)
        ed.check_eq(paused["enabled"], False, "disabled")
        calls = system_entry(await ed.call("get_system_schedule", runtime=True), "Update", "ActionSystem")["timing"]["calls"]
        await ed.call("wait", seconds=0.5)
        later = await ed.call("get_system_schedule", runtime=True)
        stopped = system_entry(later, "Update", "ActionSystem")
        ed.check_eq(stopped["timing"]["calls"], calls, "a disabled ActionSystem does not run")
        ed.check_eq(stopped["enabled"], False, "shown disabled")
        ed.check(system_entry(later, "Update", "RenderSystem")["timing"]["calls"] > 0, "the others still run")

        every = await ed.call("set_system_options", name="AudioSystem", every_n_frames=3)
        ed.check_eq(every["every_n_frames"], 3, "every_n_frames")
    finally:
        await ed.call("play", action="stop")

    # debug state: gone with the Play session
    edit = await ed.call("get_system_schedule")
    ed.check(system_entry(edit, "Update", "ActionSystem")["enabled"], "the edit scene was not changed")
    await ed.call("play", action="start")
    try:
        fresh = await ed.call("get_system_schedule", runtime=True)
        ed.check(system_entry(fresh, "Update", "ActionSystem")["enabled"], "re-enabled when Play started again")
        ed.check_eq(system_entry(fresh, "Update", "AudioSystem")["every_n_frames"], 1, "every_n_frames reset")
    finally:
        await ed.call("play", action="stop")


async def evidence_shot(ed: Editor, name: str):
    """an editor screenshot into build/evidence: save_path is project-relative (the screenshot
    tool writes inside the project only), the file is copied out"""
    relative = f"screenshots/ecs/{name}"
    shot = await ed.call_raw("screenshot", {"view": "editor", "save_path": relative})
    if not shot.is_error and (ed.project / relative).is_file():
        shutil.copyfile(ed.project / relative, EVIDENCE / name)
    return shot


async def test_systems_window(ed: Editor):
    """AC17 scripted: Window > Analysis > Systems in Play: its rows, a real click on AudioSystem's Enabled
    checkbox, screenshots, and what it shows checked through the tools."""
    ed.require_tools("focus_window", "screenshot", "get_system_schedule", "set_system_options", "get_ecs_stats", "play", "wait")
    EVIDENCE.mkdir(parents=True, exist_ok=True)
    await ed.call("play", action="start")
    try:
        await ed.call("wait", seconds=1)
        shown = await ed.call("focus_window", window="systems")
        ed.check(shown["shown"], f"the Systems window is shown: {shown}")
        ed.check_eq(sorted(row["system"] for row in shown["rows"] if row["phase"] == "Update"), sorted(ALL_SYSTEMS),
                    "the table's Update rows")
        shot = await evidence_shot(ed, "systems-window.png")
        ed.check(not shot.is_error and shot.images, f"editor screenshot: {shot.text[:300]}")

        # the table: the eight systems with their timings
        schedule = await ed.call("get_system_schedule", runtime=True)
        for system in SYSTEMS:
            timing = system_entry(schedule, "Update", system)["timing"]
            ed.check(timing["calls"] > 0 and timing["avg_ms"] > 0, f"{system}: non-zero average: {timing}")
        # the footer: component types registered, as list_component_types lists them
        stats = await ed.call("get_ecs_stats", runtime=True)
        listed = await ed.call("list_component_types")
        ed.check_eq(stats["component_types"]["registered"], len(listed["components"]), "component types in the footer")

        # unchecking AudioSystem: it stops (and pauses its sounds) until Play stops; with the
        # pointer, the click on its Enabled checkbox in the window, else through the tool
        xdo = pointer(ed)
        if xdo:
            await click_enabled_checkbox(ed, xdo, "Update", "AudioSystem")
        else:
            print("no xdotool on the harness's Xvfb: AudioSystem is unchecked through set_system_options")
            await ed.call("set_system_options", name="AudioSystem", enabled=False)
        calls = system_entry(await ed.call("get_system_schedule", runtime=True), "Update", "AudioSystem")["timing"]["calls"]
        await ed.call("wait", seconds=0.5)
        after = system_entry(await ed.call("get_system_schedule", runtime=True), "Update", "AudioSystem")
        ed.check(not after["enabled"] and after["timing"]["calls"] == calls, f"AudioSystem disabled: {after}")
        others = [system_entry(await ed.call("get_system_schedule", runtime=True), "Update", name) for name in SYSTEMS if name != "AudioSystem"]
        ed.check(all(entry["enabled"] for entry in others), f"only AudioSystem was unchecked: {others}")
        await evidence_shot(ed, "systems-window-audio-off.png")
    finally:
        await ed.call("play", action="stop")
    await ed.call("play", action="start")
    try:
        fresh = await ed.call("get_system_schedule", runtime=True)
        ed.check(system_entry(fresh, "Update", "AudioSystem")["enabled"], "AudioSystem enabled again after Play stopped")
    finally:
        await ed.call("play", action="stop")
