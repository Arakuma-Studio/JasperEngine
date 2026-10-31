#!/usr/bin/env python3
"""Build the upgrade-from-main sample game with an OLD editor, through its own MCP tools.

    uv run --no-project --with-requirements tools/mcp/requirements.txt \\
        python tools/ci/upgrade_from_main/gen_fixture.py --editor <old jasperengine-editor> --out <dir>

Run by generate.sh (which builds the old editor from an origin/main ref first). Makes, in <out>:
    project/        the game "Upgrade Courier" as a user would build it in that editor
    cpp-project/    a one-scene project with a C++ script (Lua is the fixture's language; this one
                    checks that a C++ script still builds and runs)
    editor/         the old editor's transcripts (input.txt, autopilot.txt), screenshots, problems
    calls.json      every tool call made, in order (the recipe)

Everything is made with the editor's tools (create_project, create_scene, create_entity,
add_component, set_properties, set_transform, edit_colliders, create_prefab,
create_prefab_variant, prefab_mode, instantiate_prefab, set_script_properties, create_script,
attach_script, create_ui_document, create_ui, set_child_scene, edit_animation,
edit_pose_animation, create_particles, create_material, link_material, generate_placeholder_art,
import_files, set_project_settings, edit_build_scenes...); only the Lua, HTML and CSS sources
(game/) and a sound are written as files, as a user writes code in their own editor.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import math
import re
import shutil
import struct
import subprocess
import sys
import wave
from pathlib import Path
from typing import Any

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
GAME = HERE / "game"
sys.path.insert(0, str(REPO / "tools" / "mcp"))
sys.path.insert(0, str(HERE))

import harness  # noqa: E402
import playrun  # noqa: E402

NAME = "Upgrade Courier"
# The gems of Level 1: on the courier's path (its input schedule, lib/transcript.lua) and off it
GEMS = [("Gem A", "prefabs/Gem.prefab", [1.6, 0.8, 0]), ("Gem B", "prefabs/Gem.prefab", [2.6, 0.8, -0.6]),
        ("Gem C", "prefabs/Gem Gold.prefab", [1.8, 0.8, -2.7]), ("Gem D", "prefabs/Gem.prefab", [-2, 0.8, -3]),
        ("Gem E", "prefabs/Gem Gold.prefab", [6, 0.8, 4])]
CALLS: list[dict] = []


class Driver:
    def __init__(self, ed: harness.Editor):
        self.ed = ed

    async def __call__(self, tool: str, **args: Any) -> Any:
        CALLS.append({"tool": tool, "arguments": args})
        try:
            return await self.ed.call(tool, args)
        except harness.ToolError as e:
            raise SystemExit(f"{tool} {json.dumps(args)[:600]} failed: {e}") from None


async def tree(d: Driver, scene: str) -> list[dict]:
    """get_scene_tree's text ("  Name [id] components") as {name, id, parent, components}."""
    r = await d("get_scene_tree", scene=scene)
    nodes: list[dict] = []
    stack: list[tuple[int, str]] = []
    for line in r["tree"].splitlines():
        m = re.match(r"^( *)(.*?) \[(\d+)\](.*)$", line)
        if not m:
            continue
        depth = len(m.group(1))
        while stack and stack[-1][0] >= depth:
            stack.pop()
        nodes.append({"name": m.group(2), "id": m.group(3), "parent": stack[-1][1] if stack else None,
                      "components": m.group(4)})
        stack.append((depth, m.group(2)))
    return nodes


def write_wav(path: Path) -> None:
    """A short two-tone pickup chime (16-bit mono, 22050 Hz)."""
    rate = 22050
    frames = bytearray()
    for i in range(int(rate * 0.25)):
        t = i / rate
        f = 880.0 if t < 0.1 else 1320.0
        v = 0.4 * math.sin(2 * math.pi * f * t) * (1 - t / 0.25)
        frames += struct.pack("<h", int(v * 32767))
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(bytes(frames))


async def make_assets(d: Driver) -> None:
    ed = d.ed
    # Project Settings changed from their defaults, as a game does
    await d("set_project_settings", values={
        "player.product_name": NAME,
        "player.identifier": "com.jasperengine.upgradecourier",
        "player.version": "0.3",
        "player.window_title": NAME,
        "player.window_width": 960,
        "player.window_height": 540,
        "graphics.canvas_width": 960,
        "graphics.canvas_height": 540,
        "quality.vsync": False,
        "tags_and_layers.tags": ["Gem", "Pickup"],
        "tags_and_layers.layers": {"8": "Pickups"},
    })
    # a sound copied into the project folder by hand (import_files would convert it to .webm)
    (ed.project / "assets/audio").mkdir(parents=True, exist_ok=True)
    write_wav(ed.project / "assets/audio/pickup.wav")
    await d("sync_files")
    await d("import_files", sources=[str(REPO / "tools/fixtures/pose-animation/assets/models/robot_apose.gltf")],
            to_dir="assets/models")
    art = [
        ("assets/textures/ground.png", {"shape": "checker", "color": "#5b8c5a", "color2": "#3f6b40", "cells": 8,
                                        "width": 256, "height": 256}),
        ("assets/textures/crate.png", {"shape": "rounded_rect", "color": "#b07a3b", "outline_color": "#5a3a14",
                                       "outline_width": 10, "corner_radius": 12, "width": 128, "height": 128,
                                       "background": "#8a5a24"}),
        ("assets/sprites/ball.png", {"shape": "circle", "color": "#ffd166", "outline_color": "#7a5500",
                                     "outline_width": 3, "width": 64, "height": 64}),
        ("assets/sprites/ground2d.png", {"shape": "rect", "color": "#4a4e69", "width": 64, "height": 16}),
        ("assets/sprites/cloud.png", {"shape": "ellipse", "color": "#f1faee", "width": 128, "height": 64}),
        ("assets/sprites/flag.png", {"shape": "gradient", "color": "#e63946", "color2": "#457b9d",
                                     "direction": "horizontal", "width": 128, "height": 32}),
    ]
    for path, args in art:
        await d("generate_placeholder_art", path=path, **args)
    await d("create_material", path="assets/materials/Ground.material", values={
        "baseColorTexture": "assets/textures/ground.png", "roughnessFactor": 0.9, "metallicFactor": 0.0})
    await d("create_material", path="assets/materials/Crate.material", values={
        "baseColorTexture": "assets/textures/crate.png", "roughnessFactor": 0.7})
    await d("create_material", path="assets/materials/Gem.material", values={
        "baseColorFactor": [0.25, 0.8, 1.0, 1.0], "emissiveFactor": [0.1, 0.4, 0.6], "metallicFactor": 0.3,
        "roughnessFactor": 0.2})
    # code: the Lua scripts and the HTML/CSS, as files a user writes in their own editor
    for src in sorted((GAME / "lua").rglob("*.lua")):
        rel = src.relative_to(GAME / "lua")
        if rel.parts[0] == "lib":
            await d("write_file", path=rel.as_posix(), content=src.read_text())
        else:
            await d("create_script", kind="lua", name=src.stem, content=src.read_text(), overwrite=True)
    for src in sorted((GAME / "ui").iterdir()):
        await d("write_file", path=f"assets/ui/{src.name}", content=src.read_text())
    await d("edit_animation", clip="assets/anim/Hover.anim", create={}, wrap="loop", frame_rate=30, tracks=[
        {"path": "", "channel": "position", "relative": True, "keys": [
            {"time": 0, "value": [0, 0, 0]}, {"time": 0.5, "value": [0, 0.6, 0], "ease": "quad_out"},
            {"time": 1.0, "value": [0, 0, 0], "ease": "quad_in"}]},
        {"path": "", "channel": "rotation", "keys": [
            {"time": 0, "value": {"euler": [0, 0, 0]}}, {"time": 1.0, "value": {"euler": [0, 180, 0]}}]},
    ], events=[{"time": 0.5, "function": "onBeaconPulse", "args": [1, "top"]}])
    await d("edit_pose_animation", path="assets/anim/RobotWave.poseanim",
            create={"template": "Wave", "preview_model": "assets/models/robot_apose.gltf"})


async def make_level(d: Driver) -> None:
    s = "scenes/Level1.scene"
    await d("create_scene", path=s, name="Level1", type="3D")
    # the new scene's sun: shadows on
    await d("set_properties", scene=s, entity="Directional Light", values={"light": {"shadows": True}})
    await d("create_entity", scene=s, type="point_light", name="Lamp", position=[-3, 4, 2],
            values={"light": {"shadows": True, "intensity": 3.0, "range": 12.0}})
    await d("create_entity", scene=s, type="cube", name="Floor", position=[0, -0.25, 0], scale=[40, 0.5, 40])
    await d("link_material", scene=s, entity="Floor", material="assets/materials/Ground.material")
    await d("edit_colliders", scene=s, entity="Floor", body_type="static", add=[{"shape": "box"}])
    await d("create_entity", scene=s, type="sphere", name="Player", position=[0, 0.6, 0])
    await d("edit_colliders", scene=s, entity="Player", body_type="dynamic", add=[{"shape": "sphere"}])
    await d("attach_script", scene=s, entity="Player", path="Player.lua")
    await d("set_script_properties", scene=s, entity="Player", values={"acceleration": 3.0})
    for i in range(3):
        await d("create_entity", scene=s, type="cube", name=f"Crate {i + 1}", position=[4, 0.5 + i * 1.01, -1])
        await d("link_material", scene=s, entity=f"Crate {i + 1}", material="assets/materials/Crate.material")
        await d("edit_colliders", scene=s, entity=f"Crate {i + 1}", body_type="dynamic", add=[{"shape": "box"}])
    await d("create_entity", scene=s, type="camera", name="Main Camera", position=[0, 6, 11],
            values={"camera": {"type": "perspective"}})
    await d("set_main_camera", scene=s, entity="Main Camera")
    await d("attach_script", scene=s, entity="Main Camera", path="CameraFollow.lua")
    await d("set_script_properties", scene=s, entity="Main Camera", values={"target": "Player"})

    # prefabs: a Sparkle effect nested in a Gem; a Gem Gold variant
    await d("create_entity", scene=s, type="torus", name="Gem", position=[0, 0.8, 0], scale=[0.4, 0.4, 0.4])
    await d("link_material", scene=s, entity="Gem", material="assets/materials/Gem.material")
    await d("attach_script", scene=s, entity="Gem", path="Gem.lua")
    await d("create_particles", scene=s, preset="magic", name="Sparkle", parent="Gem", position=[0, 0, 0],
            rate=12, max_particles=64, autoplay=True, loop=True)
    await d("create_prefab", scene=s, entity="Gem/Sparkle", path="prefabs/Sparkle.prefab")
    await d("create_prefab", scene=s, entity="Gem", path="prefabs/Gem.prefab")
    await d("rename_entity", scene=s, entity="Gem", name="Gem A")
    await d("set_transform", scene=s, entity="Gem A", position=GEMS[0][2])
    await d("create_prefab_variant", base="prefabs/Gem.prefab", path="prefabs/Gem Gold.prefab")
    # the variant: worth 10 and bigger (edited in Prefab Mode, saved into the variant). Not a
    # colour: a submesh linked to a .material takes the file's values when its scene opens.
    stage = await d("prefab_mode", action="open", prefab="prefabs/Gem Gold.prefab", auto_save=False)
    sid = str(harness.find_key(stage, "scene_id"))
    root = (await tree(d, sid))[0]["id"]
    await d("set_script_properties", scene=sid, entity=root, values={"value": 10})
    await d("set_transform", scene=sid, entity=root, scale=[0.55, 0.55, 0.55])
    await d("prefab_mode", action="save", scene=sid)
    await d("prefab_mode", action="close")
    for name, prefab, pos in GEMS[1:]:
        await d("instantiate_prefab", scene=s, prefab=prefab, name=name, position=pos)
    # overrides: Gem B is worth 3, flatter, its Sparkle off; Gem D spins faster
    await d("set_script_properties", scene=s, entity="Gem B", values={"value": 3})
    await d("set_transform", scene=s, entity="Gem B", scale=[0.5, 0.25, 0.5])
    await d("set_entity_state", scene=s, entity="Gem B/Sparkle", active=False)
    await d("set_script_properties", scene=s, entity="Gem D", values={"spinSpeed": 240.0})

    # the beacon: an Animation Clip on an Animation Player, its event calls Beacon.lua
    await d("create_entity", scene=s, type="cylinder", name="Beacon", position=[-4, 1, 1], scale=[0.5, 1, 0.5])
    await d("add_component", scene=s, entity="Beacon", component="animationplayer",
            values={"animationplayer": {"clips": [{"clip": "assets/anim/Hover.anim"}], "playAutomatically": False}})
    await d("attach_script", scene=s, entity="Beacon", path="Beacon.lua")
    # the robot: a Pose Animation on its Pose Animator
    await d("create_entity", scene=s, type="model", name="Robot", model="assets/models/robot_apose.gltf",
            position=[-1, 0, -6])
    await d("add_component", scene=s, entity="Robot", component="poseanimator",
            values={"poseanimator": {"animation": "assets/anim/RobotWave.poseanim", "autoplay": False}})
    await d("attach_script", scene=s, entity="Robot", path="Robot.lua")
    # pickup feedback: a sound and a burst of sparks
    await d("create_entity", scene=s, type="audio_source", name="PickupSound",
            values={"audiosource": {"filename": "assets/audio/pickup.wav", "volume": 0.6}})
    await d("create_particles", scene=s, preset="sparks", name="PickupFX", position=[0, 1, 0], autoplay=False,
            loop=False, bursts=[{"time": 0, "count": 24}])
    # its Particle System (an action at the scene root, targeting PickupFX) gets a name of its own
    newest = max((n for n in await tree(d, s) if n["name"] == "Particle System"), key=lambda n: int(n["id"]))
    await d("rename_entity", scene=s, entity=newest["id"], name="PickupBurst")
    # the HTML HUD
    await d("create_ui_document", scene=s, html="assets/ui/hud.html", name="HUD", script=False)
    await d("attach_script", scene=s, entity="HUD", path="HudUI.lua")
    # the director
    await d("create_entity", scene=s, type="empty", name="Director")
    await d("attach_script", scene=s, entity="Director", path="Level.lua")
    await d("set_script_properties", scene=s, entity="Director", values={
        "pickupSound": "PickupSound", "hud": "HUD", "burst": "PickupBurst"})
    await d("save_scene", scene=s)


async def make_hud(d: Driver) -> None:
    s = "scenes/HUD.scene"
    await d("create_scene", path=s, name="HUD", type="UI")
    await d("create_ui", scene=s, root={
        "type": "container", "name": "HUDRoot", "layout": {"anchor": "top_left", "size": [320, 90], "position": [16, 16]},
        "children": [
            {"type": "text", "name": "ScoreLabel", "text": "Score 0", "font_size": 28,
             "layout": {"anchor": "top_left", "size": [300, 36], "position": [0, 0]}},
            {"type": "progress_bar", "name": "TimeBar", "value": 0.0,
             "layout": {"anchor": "top_left", "size": [300, 20], "position": [0, 48]}},
        ]})
    await d("attach_script", scene=s, entity="HUDRoot", path="LegacyHud.lua")
    await d("set_script_properties", scene=s, entity="HUDRoot",
            values={"label": "HUDRoot/ScoreLabel", "timeBar": "HUDRoot/TimeBar"})
    await d("save_scene", scene=s)
    await d("set_child_scene", scene="scenes/Level1.scene", child=s, start_active=True, loaded_inline=True)
    await d("save_scene", scene="scenes/Level1.scene")


async def make_arena(d: Driver) -> None:
    s = "scenes/Arena2D.scene"
    await d("create_scene", path=s, name="Arena2D", type="2D")
    await d("create_entity", scene=s, type="sprite", name="Ground", position=[30, 20, 0],
            values={"sprite": {"width": 900, "height": 40},
                    "mesh": {"submeshes[0].material.baseColorTexture": "assets/sprites/ground2d.png"}})
    await d("edit_colliders", scene=s, entity="Ground", body_type="static", add=[{"shape": "box"}])
    await d("set_properties", scene=s, entity="Ground", values={"rigidbody2d": {"colliders[0].contactEvents": True}})
    await d("create_entity", scene=s, type="sprite", name="Ball",
            values={"sprite": {"width": 48, "height": 48},
                    "mesh": {"submeshes[0].material.baseColorTexture": "assets/sprites/ball.png"}})
    await d("edit_colliders", scene=s, entity="Ball", body_type="dynamic", add=[{"shape": "circle", "restitution": 0.5}])
    await d("set_properties", scene=s, entity="Ball", values={"rigidbody2d": {"colliders[0].contactEvents": True}})
    await d("create_prefab", scene=s, entity="Ball", path="prefabs/Ball.prefab")
    await d("delete_entities", scene=s, entities=["Ball"])
    for i, (x, y) in enumerate([(300, 300), (480, 420), (640, 360)]):
        await d("instantiate_prefab", scene=s, prefab="prefabs/Ball.prefab", name=f"Ball {i + 1}", position=[x, y, 0])
    await d("set_properties", scene=s, entity="Ball 2", values={"mesh": {"submeshes[0].material.baseColorFactor": [0.6, 1.0, 0.6, 1.0]}})
    await d("create_entity", scene=s, type="sprite", name="Cloud", position=[200, 480, 0],
            values={"sprite": {"width": 128, "height": 64},
                    "mesh": {"submeshes[0].material.baseColorTexture": "assets/sprites/cloud.png"}})
    await d("edit_animation", scene=s, target="Cloud", name="CloudDrift", autoplay=True, tweens=[
        {"type": "position", "from": [200, 480, 0], "to": [760, 480, 0], "duration": 4.0, "loop": True,
         "ping_pong": True, "ease": "sine_in_out"}])
    await d("create_entity", scene=s, type="sprite", name="Flag", position=[860, 120, 0],
            values={"sprite": {"width": 64, "height": 32},
                    "mesh": {"submeshes[0].material.baseColorTexture": "assets/sprites/flag.png"}})
    await d("create_entity", scene=s, type="light_2d", name="Glow", position=[480, 300, 0])
    await d("create_entity", scene=s, type="empty", name="Director")
    await d("attach_script", scene=s, entity="Director", path="Arena.lua")
    await d("set_script_properties", scene=s, entity="Director", values={"cloud": "Cloud"})
    await d("save_scene", scene=s)


async def make_title(d: Driver) -> None:
    s = "scenes/Title.scene"
    await d("create_scene", path=s, name="Title", type="2D")
    await d("create_ui_document", scene=s, html="assets/ui/title.html", name="TitleMenu", script=False)
    await d("attach_script", scene=s, entity="TitleMenu", path="Title.lua")
    await d("save_scene", scene=s)


async def build_game(ed: harness.Editor) -> None:
    d = Driver(ed)
    await make_assets(d)
    await make_title(d)
    # the new project's untitled scene is not part of the game: closed unsaved, it leaves the project
    await d("close_scene", scene="New Scene", unsaved="discard")
    await make_level(d)
    await make_hud(d)
    await make_arena(d)
    # Scenes in Build: the title is the start scene
    await d("edit_build_scenes", action="set_start", scene="scenes/Title.scene")
    await d("save_scene", all=True)
    await d("save_project")


async def make_cpp(ed: harness.Editor) -> None:
    d = Driver(ed)
    await d("set_project_settings", values={"player.product_name": "Upgrade Cpp"})
    s = "scenes/Main.scene"
    await d("create_scene", path=s, name="Main", type="3D")
    await d("close_scene", scene="New Scene", unsaved="discard")
    await d("create_entity", scene=s, type="cube", name="Target", position=[0, 0.5, 0])
    await d("create_entity", scene=s, type="empty", name="Spinner")
    await d("create_script", kind="cpp_script", name="Spinner", directory="scripts",
            header_content=(GAME / "cpp/Spinner.h").read_text(), source_content=(GAME / "cpp/Spinner.cpp").read_text(),
            overwrite=True)
    await d("attach_script", scene=s, entity="Spinner", path="scripts/Spinner.h")
    await d("set_script_properties", scene=s, entity="Spinner", values={"target": "Target", "speed": 120.0})
    await d("edit_build_scenes", action="set_start", scene=s)
    await d("save_scene", all=True)
    await d("save_project")


def copy_project(src: Path, dst: Path) -> None:
    """What a user commits: the project's files less what its own .gitignore leaves out (the
    editor writes one: .jasperengine/, the generated CMakeLists.txt, build output...)."""
    if dst.exists():
        shutil.rmtree(dst)
    subprocess.run(["git", "init", "-q", str(src)], check=True)
    listed = subprocess.run(["git", "-C", str(src), "ls-files", "-co", "--exclude-standard", "-z"], check=True,
                            capture_output=True).stdout.decode().split("\0")
    shutil.rmtree(src / ".git")
    for rel in filter(None, listed):
        (dst / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src / rel, dst / rel)


async def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--editor", required=True, help="the old editor executable")
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--skip-cpp", action="store_true")
    args = ap.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    cmd = [str(Path(args.editor).resolve())]

    async with harness.Editor(create=True, name=NAME, command=cmd, call_timeout=900) as ed:
        print("scratch:", ed.scratch.root, flush=True)
        await build_game(ed)
        copy_project(ed.project, out / "project")
        try:
            input_run = await playrun.play(ed, out / "editor", "input", "scenes/Title.scene")
        except Exception:
            print(ed.stderr_tail(80))
            raise
        print(f"input run: {len(input_run['lines'])} transcript lines, {len(input_run['problems'])} problems")
    (out / "calls.json").write_text(json.dumps(CALLS, indent=1) + "\n")

    async with harness.Editor(project=out / "project", command=cmd, env={"UPGRADE_AUTOPILOT": "1"},
                              call_timeout=900) as ed:
        auto = await playrun.play(ed, out / "editor", "autopilot", "scenes/Title.scene")
        print(f"autopilot run: {len(auto['lines'])} transcript lines, {len(auto['problems'])} problems")

    if not args.skip_cpp:
        CALLS.clear()
        async with harness.Editor(create=True, name="Upgrade Cpp", command=cmd, call_timeout=1800) as ed:
            await make_cpp(ed)
            copy_project(ed.project, out / "cpp-project")
            before = await ed.call("read_console", limit=1)
            await ed.call("play", action="start", scene="scenes/Main.scene", fixed_fps=60, auto_pause=True,
                          timeout_seconds=1800)
            await ed.call("wait", frames=130)
            await ed.call("play", action="stop", timeout_seconds=600)
            entries, _ = await playrun.console_all(ed, before.get("next_cursor"))
            (out / "editor" / "cpp.txt").write_text("\n".join(playrun.transcript_lines(entries)) + "\n")
        (out / "cpp-calls.json").write_text(json.dumps(CALLS, indent=1) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
