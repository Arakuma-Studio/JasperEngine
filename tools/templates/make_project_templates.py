#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""Makes the editor's built-in project templates (docs/specs/S27-editor-tools.md R19).

Each template is a project the editor itself creates and saves, driven over MCP (tools/mcp/harness.py),
plus template.json and a 480x270 thumbnail of its start scene. The result replaces
editor/resources/templates/<id>/, which CMake embeds in the editor (editor/EditorToolsSources.cmake).
Run it again after a change of the project or scene formats:

    uv run --no-project --with-requirements tools/mcp/requirements.txt python tools/templates/make_project_templates.py

Needs a built editor (build/linux/editor, or JASPER_EDITOR) and runs it headless.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import shutil
import struct
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "mcp"))

import pngdiff  # noqa: E402
from harness import Editor  # noqa: E402

THUMB_W, THUMB_H = 480, 270
# a template takes the project, never the editor's per-user state or build output (TemplateCatalog.cpp),
# nor the files the editor generates into every project it opens or builds (Generator.cpp: the
# editor-integration files, written when missing with this editor's version, and the root
# CMakeLists.txt) and the .gitignore / .gitattributes writer's output (R18 runs it on creation)
EXCLUDED = {".jasperengine", "build", "Recordings", ".trash", ".git", ".clangd", ".vscode", ".luarc.json", "AGENTS.md",
            "CLAUDE.md", "CMakeLists.txt", ".gitignore", ".gitattributes"}

ROTATOR = """---@class Rotator : ScriptInstance
-- Turns its GameObject around the vertical axis.
local Rotator = { properties = {
    { name = "degreesPerSecond", type = "float", default = 45.0 },
} }

function Rotator:init()
    self.go = GameObject(self.scene, self.entity)
    self.angle = 0
    RegisterEngineEvent(self, "onUpdate")
end

function Rotator:onUpdate()
    self.angle = (self.angle + self.degreesPerSecond * Engine.deltatime) % 360
    self.go:setRotation(0, self.angle, 0)
end

return Rotator
"""

PLAYER_MOVE = """---@class PlayerMove : ScriptInstance
-- Moves its sprite with the arrow keys or WASD.
local PlayerMove = { properties = {
    { name = "speed", type = "float", default = 300.0 },
} }

function PlayerMove:init()
    self.go = GameObject(self.scene, self.entity)
    RegisterEngineEvent(self, "onUpdate")
end

local function held(a, b)
    return Input.isKeyPressed(a) or Input.isKeyPressed(b)
end

function PlayerMove:onUpdate()
    local x, y = 0, 0
    if held(Input.KEY_LEFT, Input.KEY_A) then x = x - 1 end
    if held(Input.KEY_RIGHT, Input.KEY_D) then x = x + 1 end
    if held(Input.KEY_UP, Input.KEY_W) then y = y + 1 end
    if held(Input.KEY_DOWN, Input.KEY_S) then y = y - 1 end
    if x ~= 0 or y ~= 0 then
        local step = self.speed * Engine.deltatime
        self.go.position = self.go.position + Vector3(x * step, y * step, 0)
    end
end

return PlayerMove
"""

INFO = {
    "empty": {"name": "Empty", "description": "One 3D scene with only a camera.", "category": "Core",
              "order": 0, "accentColor": "#5A67A7"},
    "3d": {"name": "3D", "description": "A lit 3D scene: sun with shadows, sky, ground and a turning cube.",
           "category": "Core", "order": 1, "accentColor": "#3F8F6B"},
    "2d": {"name": "2D", "description": "A 2D scene with a sprite moved by the arrow keys or WASD, on a 1280x720 letterboxed canvas.",
           "category": "Core", "order": 2, "accentColor": "#C27C2C"},
    "ui": {"name": "UI", "description": "A main menu in HTML and CSS with Play and Quit buttons wired in Lua, on a 1920x1080 canvas.",
           "category": "Core", "order": 3, "accentColor": "#9B4F96"},
}


def nodes_of(tree: dict) -> list[dict]:
    out: list[dict] = []
    stack = list(tree.get("nodes", []))
    while stack:
        node = stack.pop()
        out.append(node)
        stack.extend(node.get("children", []))
    return out


async def main_scene(ed: Editor, type_: str) -> int:
    """A scene "Main" saved as Main.scene, the project's only scene."""
    made = await ed.call("create_scene", name="Main", type=type_, path="Main", close_previous=True, unsaved="discard")
    sid = made["scene"]["id"]
    scenes = await ed.call("list_scenes")
    for scene in scenes.get("scenes", []):
        if scene["id"] != sid:
            await ed.call("close_scene", scene=scene["id"], unsaved="discard")
    await ed.call("edit_build_scenes", action="set_start", scene=sid)
    return sid


async def build_empty(ed: Editor) -> None:
    sid = await main_scene(ed, "3D")
    tree = await ed.call("get_scene_tree", scene=sid, format="json")
    doomed = [n["id"] for n in nodes_of(tree) if n["name"] in ("Directional Light", "Skybox")]
    if doomed:
        await ed.call("delete_entities", scene=sid, entities=doomed)


async def build_3d(ed: Editor) -> None:
    sid = await main_scene(ed, "3D")
    ground = (await ed.call("create_entity", scene=sid, type="plane", name="Ground", scale=[2, 1, 2]))["entity_id"]
    await ed.call("create_material", path="materials/Ground", scene=sid, entity=ground,
                  values={"baseColorFactor": [0.42, 0.45, 0.4, 1.0], "roughnessFactor": 0.9})
    cube = (await ed.call("create_entity", scene=sid, type="cube", name="Cube", position=[0, 0.5, 0]))["entity_id"]
    await ed.call("create_script", kind="lua", name="Rotator", directory="scripts", content=ROTATOR, scene=sid, entity=cube)


async def build_2d(ed: Editor) -> None:
    await ed.call("set_project_settings", values={"graphics.canvas_width": 1280, "graphics.canvas_height": 720,
                                                   "graphics.scaling_mode": "letterbox"})
    sid = await main_scene(ed, "2D")
    art = await ed.call("generate_placeholder_art", path="sprites/player.png", width=64, height=64, shape="rounded_rect",
                        corner_radius=12, color="#F2B33D", outline_color="#5C3B0A", outline_width=3)
    player = (await ed.call("create_entity", scene=sid, type="sprite", name="Player", position=[608, 328, 0],
                            values={"sprite": {"width": 64, "height": 64},
                                    "mesh": {"submeshes[0].material.baseColorTexture": art["path"]}}))["entity_id"]
    await ed.call("create_script", kind="lua", name="PlayerMove", directory="scripts", content=PLAYER_MOVE, scene=sid, entity=player)


async def build_ui(ed: Editor) -> None:
    await ed.call("set_project_settings", values={"graphics.canvas_width": 1920, "graphics.canvas_height": 1080})
    sid = await main_scene(ed, "UI")
    await ed.call("create_ui_document", scene=sid, html="ui/main_menu.html", name="Main Menu", template="main_menu", script=True)


BUILDERS = {"empty": build_empty, "3d": build_3d, "2d": build_2d, "ui": build_ui}


def png_size(data: bytes) -> tuple[int, int]:
    return struct.unpack(">II", data[16:24])


def letterbox_png(data: bytes) -> bytes:
    """`data` scaled to fit 480x270 (nearest), centred on black."""
    w, h, rgba = pngdiff.decode(data)
    scale = min(THUMB_W / w, THUMB_H / h)
    sw, sh = max(1, round(w * scale)), max(1, round(h * scale))
    ox, oy = (THUMB_W - sw) // 2, (THUMB_H - sh) // 2
    out = bytearray(THUMB_W * THUMB_H * 4)
    for y in range(THUMB_H):
        for x in range(THUMB_W):
            o = (y * THUMB_W + x) * 4
            sx, sy = x - ox, y - oy
            if 0 <= sx < sw and 0 <= sy < sh:
                i = (min(h - 1, int(sy / scale)) * w + min(w - 1, int(sx / scale))) * 4
                out[o:o + 3] = rgba[i:i + 3]
            out[o + 3] = 255
    return pngdiff.encode(THUMB_W, THUMB_H, bytes(out))


def copy_project(src: Path, dst: Path) -> None:
    if dst.exists():
        shutil.rmtree(dst)
    dst.mkdir(parents=True)
    for entry in sorted(src.iterdir()):
        if entry.name in EXCLUDED:
            continue
        if entry.is_dir():
            shutil.copytree(entry, dst / entry.name)
        else:
            shutil.copy2(entry, dst / entry.name)


async def make(template_id: str, out_dir: Path) -> None:
    info = INFO[template_id]
    async with Editor(create=True, name=info["name"], label=f"template-{template_id}") as ed:
        await BUILDERS[template_id](ed)
        await ed.call("save_project")
        await ed.call("configure_game_view", aspect="16:9")
        await ed.call("wait", frames=30)
        shot = await ed.screenshot(view="game", max_width=THUMB_W)
        thumbnail = shot if png_size(shot) == (THUMB_W, THUMB_H) else letterbox_png(shot)
        target = out_dir / template_id
        copy_project(ed.project, target)
        (target / "thumbnail.png").write_bytes(thumbnail)
        meta = {"schemaVersion": 1, "id": template_id, **info, "thumbnail": "thumbnail.png", "startScene": "Main"}
        (target / "template.json").write_text(json.dumps(meta, indent=2) + "\n")
        files = sorted(str(p.relative_to(target)) for p in target.rglob("*") if p.is_file())
        print(f"[templates] {template_id}: {len(files)} files: {', '.join(files)}")


async def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=Path, default=REPO / "editor" / "resources" / "templates")
    parser.add_argument("ids", nargs="*", default=list(INFO))
    args = parser.parse_args()
    for template_id in args.ids:
        await make(template_id, args.out)
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
