"""MCP tool conventions (S31 R20, docs/mcp/conventions.md), checked on the live tools/list of
every group:

- tool names are snake_case, unique, and a new tool's name starts with a verb (verb_noun); the
  names that predate the rule are listed below and keep their names;
- parameter keys are snake_case at every level;
- schemas use only the keyword subset the McpSchema builders emit and the CLI validates (type,
  enum, required, properties, additionalProperties, items, minItems, maxItems, minimum, maximum,
  anyOf, plus description and default);
- an object with declared properties says additionalProperties false (the top level is closed by
  the registry, which leaves it out of the listing), or true for a free-form object whose
  properties only name the common keys (schema::anyObject);
- `type` names JSON Schema types; enums are non-empty lists of distinct strings or numbers;
- every tool has a description, annotations, and _meta {"jasper/group", "jasper/module"} with a
  group of the group table (tool_groups).

The description rule (at most 400 characters, shared conventions in the instructions) is a review
rule, not checked here.
"""

from __future__ import annotations

CI = True
EDITOR = {"create": True, "name": "Tool Conventions CI"}

import re

from harness import Editor, tool_meta

ALLOWED_KEYWORDS = {"type", "enum", "required", "properties", "additionalProperties", "items", "minItems",
                    "maxItems", "minimum", "maximum", "anyOf", "description", "default"}
JSON_TYPES = {"object", "array", "string", "integer", "number", "boolean", "null"}
SNAKE = re.compile(r"[a-z][a-z0-9]*(?:_[a-z0-9]+)*")

# A new tool's first word (R20: get_/list_/set_/create_/delete_, edit_<thing> for multi-op tools,
# and the other verbs today's tools use)
VERBS = {
    "analyze", "get", "list", "set", "create", "delete", "edit", "add", "remove", "find", "open", "close", "save", "run",
    "start", "stop", "apply", "import", "export", "read", "write", "check", "compile", "generate", "select",
    "move", "rename", "copy", "paste", "duplicate", "update", "reset", "inspect", "assert", "send", "sample",
    "sculpt", "bake", "capture", "record", "play", "preview", "sync", "search", "view", "focus", "frame", "link",
    "attach", "detach", "fork", "replace", "instantiate", "unpack", "clear", "cancel", "respond", "configure",
    "build", "eval", "measure", "query", "toggle", "commit", "diff", "lock", "unlock", "revert", "publish",
    "verify", "enable", "disable", "validate", "convert", "cook", "paint", "place", "fill", "flatten", "profile",
    "test", "load", "reload", "simulate", "step", "pick", "show", "hide", "group", "ungroup", "align", "snap",
    "discard", "wait", "resolve", "retarget", "raycast", "reimport", "render", "control",
    # S28: dismiss_crash_notice (marks a report handled, not a close), debug_crash (crashes on purpose)
    "dismiss", "debug",
    # S19: visualize_navigation (the spec's name: overlay flags and a Scene view image)
    "visualize",
    # S27: evaluate_curve (the spec's name: a stored curve evaluated at times, as the runtime does)
    "evaluate",
    # S30: install_player_runtime (the spec's name: a download or a local archive into the runtimes)
    "install",
}

# Names registered before the verb_noun rule (S31): they keep their names
PREDATING_NAMES = {
    "add_component", "add_to_prefab", "apply_material_preset", "apply_post_processing_preset", "assert_ui",
    "attach_script", "batch", "build_project", "cancel_job", "check_script", "check_shader",
    "clear_console", "close_scene", "code_editor", "compile_scripts", "configure_game_view",
    "copy_component", "copy_entities", "create_entity", "create_folder", "create_material",
    "create_particles", "create_prefab", "create_prefab_variant", "create_project", "create_scene",
    "create_script", "create_ui", "create_ui_document", "delete_asset", "delete_entities", "delete_scene",
    "detach_script", "duplicate_entities", "edit_animation", "edit_build_scenes", "edit_colliders",
    "edit_project_build_file", "edit_tilemap", "editor_status", "eval_lua", "find_entities", "focus_window",
    "fork_shader", "frame_entity", "generate_placeholder_art", "generate_shaders", "get_animation",
    "get_asset_info", "get_build_info", "get_entity", "get_job", "get_model_info", "get_play_state",
    "get_prefab_info", "get_project_settings", "get_project_summary", "get_runtime_entity",
    "get_scene_settings", "get_scene_tree", "get_selection", "get_shader_uniforms", "get_tilemap",
    "import_files", "inspect_ui", "instantiate_prefab", "link_material", "list_assets",
    "list_component_types", "list_material_presets", "list_post_processing_presets", "list_prefabs",
    "list_runtime_entities", "list_scenes", "list_scripts", "list_shaders", "list_ui_effects", "move_asset",
    "move_entity", "open_project", "open_scene", "paste_component", "paste_entities", "play", "prefab_mode",
    "prefab_overrides", "preview_animation", "read_console", "read_engine_source", "read_file", "redo",
    "remove_component", "remove_from_prefab", "rename_entity", "rename_scene", "replace_prefab",
    "reset_component", "respond_dialog", "run_build", "sample_terrain", "save_project", "save_scene",
    "screenshot", "sculpt_terrain", "search_engine_api", "synthesize_texture", "search_engine_source", "search_files",
    "select_entities", "select_scene", "send_input", "set_child_scene", "set_entity_state", "set_joint",
    "set_main_camera", "set_mesh_geometry", "set_model", "set_project_settings", "set_properties",
    "set_rect_layout", "set_runtime_property", "set_scene_camera", "set_scene_settings",
    "set_script_properties", "set_shader_uniform", "set_shape_points", "set_sprite_animation",
    "set_sprite_frames", "set_standalone_prefab", "set_terrain_maps", "set_texture_settings",
    "set_transform", "set_ui_effect_time", "sync_files", "tool_groups", "undo", "unpack_prefab",
    "update_material", "update_script_entry", "view_asset", "wait", "write_file",
}

# Upstream's tools (origin/main, absorbed by SYNC#4) keep upstream's names: its scripts, docs and
# tests call them so (audio-mixer, native-bridge, PlayerPrefs)
UPSTREAM_NAMES = {"audio_mixer", "native_plugins", "user_settings"}


def schema_problems(schema, path: str, top: bool = False) -> list[str]:
    out: list[str] = []
    if not isinstance(schema, dict):
        return [f"{path}: a schema must be an object"]
    for key in schema:
        if key not in ALLOWED_KEYWORDS:
            out.append(f"{path}: keyword {key!r} is outside the subset")
    t = schema.get("type")
    types = t if isinstance(t, list) else [t] if t is not None else []
    for item in types:
        if item not in JSON_TYPES:
            out.append(f"{path}: type {item!r}")
    if "enum" in schema:
        values = schema["enum"]
        if not isinstance(values, list) or not values or len({repr(v) for v in values}) != len(values) or \
                not all(isinstance(v, (str, int, float)) for v in values):
            out.append(f"{path}: enum must be a non-empty list of distinct strings or numbers")
    props = schema.get("properties")
    if props is not None:
        if not isinstance(props, dict):
            out.append(f"{path}: properties must be an object")
            props = {}
        for key, sub in props.items():
            if not SNAKE.fullmatch(key):
                out.append(f"{path}: parameter key {key!r} is not snake_case")
            out += schema_problems(sub, f"{path}.{key}")
        if not top and props and "additionalProperties" not in schema:
            out.append(f"{path}: an object with declared properties needs additionalProperties false "
                       "(or true for a free-form object)")
        for key in schema.get("required", []):
            if key not in props:
                out.append(f"{path}: required {key!r} is not a property")
    if "additionalProperties" in schema and not isinstance(schema["additionalProperties"], bool):
        out.append(f"{path}: additionalProperties must be true or false (no schemas)")
    if "items" in schema:
        out += schema_problems(schema["items"], f"{path}[]")
    for i, sub in enumerate(schema.get("anyOf", [])):
        out += schema_problems(sub, f"{path}|{i}")
    return out


def tool_problems(tool, known_groups: set[str]) -> list[str]:
    name = tool.name
    out: list[str] = []
    if not SNAKE.fullmatch(name):
        out.append(f"{name}: tool names are snake_case")
    elif name not in PREDATING_NAMES and name not in UPSTREAM_NAMES and name.split("_")[0] not in VERBS:
        out.append(f"{name}: a new tool's name is verb_noun (get_, list_, set_, create_, delete_, edit_...)")
    if not (tool.description or "").strip():
        out.append(f"{name}: no description")
    if getattr(tool, "annotations", None) is None:
        out.append(f"{name}: no annotations")
    schema = tool.input_schema or {}
    if schema.get("type") != "object":
        out.append(f"{name}: inputSchema must be an object schema")
    if schema.get("additionalProperties") is False:
        out.append(f"{name}: the listing leaves out the top level's additionalProperties false (the registry enforces it)")
    out += [f"{name}: {p}" for p in schema_problems(schema, "inputSchema", top=True)]
    meta = tool_meta(tool)
    if meta.get("jasper/group") not in known_groups:
        out.append(f"{name}: _meta group {meta.get('jasper/group')!r} is not in the group table")
    if not meta.get("jasper/module"):
        out.append(f"{name}: _meta has no jasper/module")
    return out


async def test_conventions(ed: Editor):
    tools = await ed.tools(groups="all")
    groups = {g["name"] for g in (await ed.call("tool_groups"))["groups"]}
    names = [t.name for t in tools]
    problems = [f"{n}: registered twice" for n in sorted({n for n in names if names.count(n) > 1})]
    for t in tools:
        problems += tool_problems(t, groups)
    ed.check(not problems, f"{len(problems)} convention violation(s):\n" + "\n".join(problems))
    print(f"     [conventions] {len(tools)} tools in {len(groups)} groups")


async def test_red_controls():
    """The checker itself: each rule fails on a schema that breaks it."""
    class Fake:
        def __init__(self, name, schema, group="core", description="x"):
            self.name, self.input_schema, self.description = name, schema, description
            self.annotations = object()
            self.meta = {"jasper/group": group, "jasper/module": "Test"}

    groups = {"core", "profiling"}
    ok = Fake("get_thing", {"type": "object", "properties": {"thing_id": {"type": "string"}}})
    assert tool_problems(ok, groups) == [], tool_problems(ok, groups)
    cases = {
        "camelCase key": Fake("get_thing", {"type": "object", "properties": {"captureId": {"type": "string"}}}),
        "keyword outside the subset": Fake("get_thing", {"type": "object", "properties": {
            "a": {"oneOf": [{"type": "string"}]}}}),
        "open nested object": Fake("get_thing", {"type": "object", "properties": {
            "a": {"type": "object", "properties": {"b": {"type": "string"}}}}}),
        "unknown group": Fake("get_thing", {"type": "object"}, group="nope"),
        "not verb_noun": Fake("thing_getter", {"type": "object"}),
        "camelCase name": Fake("getThing", {"type": "object"}),
        "required without property": Fake("get_thing", {"type": "object", "properties": {}, "required": ["x"]}),
    }
    for label, tool in cases.items():
        assert tool_problems(tool, groups), f"red control {label!r} passed the checker"
