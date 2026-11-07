# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""Record what a model sees of the editor's MCP server, for the eval and for measuring it.

    tools/build.sh exec linux python3 tools/mcp/eval/dump_surface.py --out build/eval-surface
    (or with the harness's packages on the host: uv run --no-project --with-requirements
     tools/mcp/requirements.txt python tools/mcp/eval/dump_surface.py --out DIR)

Starts the built editor (the harness: a new empty project, headless) and writes DIR/:
  surface.json  every tool as tools/list sends it (all groups, with _meta group), the groups
                a new session lists, the instructions (initialize), the resources (with the text
                of jasper://guide and jasper://docs/...), and real
                results of the calls in RECORD (cases use them for mocks in the server's shapes)
  project/      the new project's files (no generated .jasperengine/), copied by every sample
  sizes.txt     the model-visible size per tool and per listing (measure.py)
"""

from __future__ import annotations

import argparse
import asyncio
import json
import shutil
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE))

from harness import Editor  # noqa: E402
import measure  # noqa: E402

# Calls whose real results the cases reuse: key -> (tool, arguments)
RECORD = {
    "editor_status": ("editor_status", {}),
    "list_scenes": ("list_scenes", {}),
    "get_scene_tree": ("get_scene_tree", {}),
    "read_console": ("read_console", {}),
    "get_play_state": ("get_play_state", {}),
    "tool_groups": ("tool_groups", {}),
    "create_entity_box": ("create_entity", {"type": "box", "name": "Crate"}),
    "get_entity_crate": ("get_entity", {"entity": "Crate"}),
    "set_properties_crate": ("set_properties", {"entity": "Crate", "component": "transform",
                                                "values": {"position": [0, 2, 0]}}),
    "save_scene": ("save_scene", {}),
    "list_assets": ("list_assets", {}),
    "list_component_types": ("list_component_types", {}),
    "unknown_argument": ("create_entity", {"type": "box", "colour": "red"}),
    "create_camera_duplicate": ("create_entity", {"type": "camera"}),
    "set_properties_layer": ("set_properties", {"entity": "Crate", "component": "transform", "values": {"layer": 12}}),
    "get_scene_settings": ("get_scene_settings", {}),
    "project_tags_layers": ("get_project_settings", {"category": "tags_and_layers", "describe": True}),
    "project_physics": ("get_project_settings", {"category": "physics", "describe": True}),
    "configure_game_view_read": ("configure_game_view", {}),
    "configure_game_view_bad_aspect": ("configure_game_view", {"aspect": "20:9"}),
    "batch_args_key": ("batch", {"calls": [{"tool": "get_entity", "args": {"entity": "Crate"}}]}),
    "play_start": ("play", {"action": "start"}),
    "play_state_playing": ("get_play_state", {}),
    "read_console_playing": ("read_console", {}),
    "touch_down": ("send_input", {"events": [{"type": "touch", "action": "down", "x": 10, "y": 10}]}),
    "play_stop": ("play", {"action": "stop"}),
    "unknown_tool_hint": ("delete_entity", {"entity": "Crate"}),
    "project_settings_all": ("get_project_settings", {}),
    "search_api_rotate": ("search_engine_api", {"query": "GameObject rotate rotation"}),
    "search_api_events": ("search_engine_api", {"query": "RegisterEngineEvent onUpdate deltatime"}),
    "search_api_profiling": ("search_engine_api", {"query": "profiler frame time draw calls stats"}),
    "search_source_drawcalls": ("search_engine_source", {"query": "drawCalls"}),
    "search_api_game_view": ("search_engine_api", {"query": "game view resolution aspect device"}),
    "search_api_tags_layers": ("search_engine_api", {"query": "tags layers collision matrix"}),
    "settings_search_game_view": ("get_project_settings", {"search": "game view resolution"}),
}
# edit-mode results first (the play calls come last)
RECORD = {"eval_lua_edit_mode": ("eval_lua", {"code": "return 1"}), **RECORD}


async def build_world(ed) -> None:
    """The scenes world.py describes, made and saved by the editor, so a sample's files match what
    the mocks report (Main with its GameObjects as the start scene, Hud, Level2; ids differ)."""
    await ed.call("create_scene", name="Main", path="scenes/Main.scene", close_previous=True, unsaved="discard")
    for args in ({"type": "camera", "name": "Main Camera", "position": [0, 5, -10]},
                 {"type": "plane", "name": "Ground", "scale": [20, 1, 20]},
                 {"type": "sprite", "name": "Player"},
                 {"type": "sprite", "name": "Enemy"},
                 {"type": "box", "name": "Crate", "position": [0, 3, 0], "scale": [2, 2, 2]}):
        await ed.call("create_entity", **args)
    for child in ("Gun", "Shield"):
        await ed.call("create_entity", type="sprite", name=child, parent="Player")
    await ed.call("save_scene", scene="Main")
    await ed.call("create_scene", name="Hud", path="scenes/Hud.scene", type="2D")
    await ed.call("create_scene", name="Level2", path="scenes/Level2.scene")
    await ed.call("edit_build_scenes", action="set_start", scene="scenes/Main.scene")
    await ed.call("save_project")


def dump_tool(tool) -> dict:
    return tool.model_dump(mode="json", by_alias=True, exclude_none=True)


async def record(out: Path) -> dict:
    async with Editor(create=True, name="EvalGame") as ed:
        tools = [dump_tool(t) for t in await ed.tools(groups="all")]
        default_names = {t.name for t in await ed.tools(groups=None)}
        groups = {str((t.get("_meta") or {}).get("jasper/group") or "core") for t in tools}
        default_groups = sorted(g for g in groups if all(
            t["name"] in default_names for t in tools if str((t.get("_meta") or {}).get("jasper/group") or "core") == g))
        resources = await ed.session.list_resources()
        templates = await ed.session.list_resource_templates()
        recorded = {}
        for key, (tool, args) in RECORD.items():
            try:
                r = await ed.call_raw(tool, args)
            except Exception as exc:  # a JSON-RPC error (an unknown tool): its message is the result
                recorded[key] = {"tool": tool, "args": args, "text": str(exc), "structured": None, "is_error": True}
                continue
            recorded[key] = {"tool": tool, "args": args, "text": r.text,
                             "structured": r.structured, "is_error": r.is_error}
        await build_world(ed)
        contents = {}
        for res in resources.resources:
            uri = str(res.uri)
            if not uri.startswith(("jasper://guide", "jasper://docs")):
                continue
            read = await ed.session.read_resource(res.uri)
            contents[uri] = "\n".join(getattr(c, "text", "") or "" for c in read.contents)
        project_dst = out / "project"
        if project_dst.exists():
            shutil.rmtree(project_dst)
        shutil.copytree(ed.project, project_dst, ignore=shutil.ignore_patterns(".jasperengine", "build", "*.log"))
        return {"instructions": ed.instructions, "tools": tools, "default_groups": default_groups,
                "resources": [r.model_dump(mode="json", by_alias=True, exclude_none=True) for r in resources.resources],
                "resource_templates": [r.model_dump(mode="json", by_alias=True, exclude_none=True)
                                       for r in templates.resource_templates],
                "resource_contents": contents, "recorded": recorded}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True, type=Path)
    a = ap.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    surface = asyncio.run(record(a.out))
    agents_md = a.out / "project" / "AGENTS.md"
    surface["project_agents_md_bytes"] = agents_md.stat().st_size if agents_md.exists() else 0
    (a.out / "surface.json").write_text(json.dumps(surface, indent=1, ensure_ascii=False), encoding="utf-8")
    (a.out / "sizes.txt").write_text(measure.report(surface), encoding="utf-8")
    print(measure.report(surface, top=10))
    return 0


if __name__ == "__main__":
    sys.exit(main())
