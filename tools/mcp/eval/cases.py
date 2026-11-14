# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""The eval's cases: one decision point each (README.md). Paraphrased from real sessions and
fixes, with no user data. Results are in the editor's real shapes: {"recorded": key} reuses a
result dump_surface.py recorded from a built editor; hand-written ones copy the server's text.
"""

import json

PLAYER_LUA = """---@class player : ScriptInstance
local player = {
    properties = {
        { name = "speed", displayName = "Speed", type = "float", default = 5 },
    },
}

function player:init()
    RegisterEngineEvent(self, "onUpdate")
end

function player:onUpdate()
    local go = GameObject(self.scene, self.entity)
    local p = go.position
    p.x = p.x + self.speed * Engine.deltatime
    go.position = p
end

return player
"""

README = "# Space Dodge\n\nA small arcade game made with JasperEngine.\n\nControls: arrows to move, spcae to shoot.\n"

GAME_FILES = {"scripts/player.lua": PLAYER_LUA, "README.md": README}

# a shell command that writes project.yaml (reading it is fine)
YAML_WRITE = r"(sed\s+-i|perl\s+-i|>>?\s*\S*project\.yaml|tee\s+\S*project\.yaml|open\([^)]*project\.yaml[^)]*['\"]w)"

NOT_OPEN = ("Scene 3 \"Level2\" is not open (its contents are not loaded). Open it first: open_scene "
            "{\"scene\": \"scenes/Level2.scene\", \"close_previous\": false} (then select_scene to switch back).")


# edit_voxels and sample_voxels, in their real shapes
VOXEL_CREATED = {"entity": {"id": 21, "name": "Cave", "path": "Cave"}, "asset": "voxels/Cave.voxelvolume", "dimensions": [64, 64, 64],
                 "voxel_size": 1.0, "estimated_memory": 1048576, "over_budget_targets": []}
VOXEL_CONNECTED = {"connected": True, "open_to_sky": True, "component_count": 1, "from_in_air": True, "to_in_air": True, "from_component": 0, "to_component": 0,
                   "air_components": [{"index": 0, "voxels": 180000, "volume": 180000.0, "open_to_sky": True,
                                       "voxel_bounds": {"min": [0, 0, 0], "max": [63, 63, 63]}, "bounds": {"min": [0, 0, 0], "max": [64, 64, 64]}}]}

# import_project's structuredContent with a leftover beside the new project
LEFTOVER_IMPORT = {
    "path": "/home/me/Games/Game", "opened": True, "log": [],
    "leftovers": [{"path": "/home/me/Games/Old.importing",
                   "reason": "It has no owner marker: an older editor left it, or it is your own folder."}],
    "next": "Leftovers may hold the user's files: tell the user. Once they agree, clear_project_leftovers {remove} "
            "(tool_groups: editor_tools) deletes one.",
}


def ok(text, structured=None):
    return {"text": text, "structured": structured}


def err(text):
    return {"text": text, "is_error": True}


RENAME_DIALOG = {"kind": "modal", "title": "Rename File", "message": "Rename 'RenameMe.lua' to:", "choices": ["ok", "cancel"],
                 "fields": [{"name": "name", "type": "text", "value": "RenameMe.lua"}]}

BOX_DIALOG = {"kind": "modal", "title": "File Already Exists", "message": "A file with this name already exists.", "choices": ["ok"]}

RENAME_STATUS = {"text": "Project \"Space Dodge\". Open dialog: \"Rename File\".", "real": True,
                 "structured": {"project": {"name": "Space Dodge"}, "playing": False, "dialog": RENAME_DIALOG}}

DIALOG_ANSWERED = {"text": "Answered the dialog.", "real": True, "structured": {"answered": {"title": "Rename File", "choice": "ok"}}}

FILE_PICKER_DIALOG = {"kind": "file_picker", "title": "Open Files", "choices": ["ok", "cancel"],
                      "message": "The editor waits for the files to open. Several: values.paths.",
                      "fields": [{"name": "path", "type": "path", "value": ""}]}

FILE_PICKER_OPEN = {"text": "The editor is showing the dialog \"Open Files\".", "real": True, "structured": {"dialog": FILE_PICKER_DIALOG}}

# T1: edit_terrain and sample_terrain results in the handler's shapes (editor/mcp/tools/TerrainTools.cpp)
TERRAIN_ENTITY = {"id": 4, "name": "Island", "type": "terrain"}
TERRAIN_EDIT_RESULT = {
    "terrain": TERRAIN_ENTITY, "dry_run": False, "undo_name": "Edit Terrain (Island)", "created_maps": [],
    "changed": [{"map": "height", "rect": [462, 462, 101, 101], "local": {"min": [-50.0, -50.0], "max": [50.0, 50.0]}, "texels": 7854}],
    "ops": [{"op": "set_height", "changed_texels": 7854, "clipped_low": 0, "clipped_high": 0, "filtered_out": 0, "outside": 0}],
    "warnings": [], "stats": {"height_min": 0.0, "height_max": 80.0, "height_min_before": 0.0, "height_max_before": 0.0,
                              "mean_delta": 41.2, "max_delta": 80.0},
    "next": "Check the result with sample_terrain (stats, hash) or look with visualize_terrain.",
}
TERRAIN_SAMPLE_RESULT = {
    "terrain": TERRAIN_ENTITY, "space": "world",
    "stats": {"texels": 1050625, "stride": 1, "height": {"min": 0.0, "max": 80.0, "mean": 3.1, "p05": 0.0, "p50": 0.0, "p95": 41.0,
                                                         "clipped_low_share": 0.0, "clipped_high_share": 0.0}},
}
TERRAIN_CLIPPED_RESULT = TERRAIN_EDIT_RESULT | {
    "changed": [{"map": "height", "rect": [0, 0, 1025, 1025], "local": {"min": [-512.0, -512.0], "max": [512.0, 512.0]}, "texels": 1050625}],
    "ops": [{"op": "set_height", "changed_texels": 1050625, "clipped_low": 0, "clipped_high": 1050625, "filtered_out": 0, "outside": 0}],
    "warnings": [{"op": 0, "code": "clipped_high", "message": "1050625 texel(s) reached the terrain's top (100 m)."}],
    "stats": {"height_min": 100.0, "height_max": 100.0, "height_min_before": 0.0, "height_max_before": 80.0, "mean_delta": 96.9, "max_delta": 100.0},
    "next": "Heights reached the terrain's top: raise maxHeight with set_properties, then remap_heights to keep the shape.",
}
TERRAIN_CLIPPED_SAMPLE = {
    "terrain": TERRAIN_ENTITY, "space": "world",
    "stats": {"texels": 1050625, "stride": 1, "height": {"min": 100.0, "max": 100.0, "mean": 100.0, "p05": 100.0, "p50": 100.0, "p95": 100.0,
                                                         "clipped_low_share": 0.0, "clipped_high_share": 1.0}},
}
TERRAIN_IMPORT_DRY = {
    "terrain": TERRAIN_ENTITY, "dry_run": True, "undo_name": "Edit Terrain (Island)", "created_maps": [],
    "changed": [{"map": "height", "rect": [0, 0, 1025, 1025], "local": {"min": [-512.0, -512.0], "max": [512.0, 512.0]}, "texels": 1050625}],
    "ops": [{"op": "import_heightmap", "changed_texels": 1050625, "clipped_low": 0, "clipped_high": 0, "filtered_out": 0, "outside": 0,
             "detected": {"format": "r16", "format_guessed": False, "width": 1025, "height": 1025, "size_guessed": True,
                          "byte_order": "little", "byte_order_guessed": True, "data_min": 812, "data_max": 61022,
                          "range_low": 812, "range_high": 61022, "clipped_low": 0, "clipped_high": 0}}],
    "warnings": [], "next": "Nothing changed. Call again without dry_run to apply.",
}


CASES = [
    # -- controls: ordinary work where a trigger word must not pull in the editor tools --------
    {
        "id": "control_readme_typo",
        "kind": "control",
        "what_happened": "Plain file work in a game project; the word 'game' and the project must not pull in editor tools.",
        "text_at_issue": "none",
        "source": "synthetic control",
        "files": GAME_FILES,
        "prompt": "Fix the typo in README.md.",
        "checks": {"max_calls": {"*": 0}, "reply_must_not": [r"\bplay(ed|ing)? the (game|scene)"]},
        "max_turns": 8,
    },
    {
        "id": "control_explain_lua",
        "kind": "control",
        "what_happened": "A question about code answered by reading the file; must not read it through read_file or start play.",
        "text_at_issue": "McpInstructions.cpp Code: 'write_file / read_file / search_files are for clients without file access'",
        "source": "synthetic control (guide rule on file tools)",
        "files": GAME_FILES,
        "prompt": "What does scripts/player.lua do? Two sentences, no changes.",
        "checks": {"must_not_call": ["read_file", "play", "search_files", "write_file"], "max_calls": {"*": 1}},
        "max_turns": 6,
    },
    {
        "id": "control_rename_variable",
        "kind": "control",
        "what_happened": "A pure code edit; the agent edits the file itself, not through write_file or create_script.",
        "text_at_issue": "McpInstructions.cpp Code: 'Edit code with your own tools'",
        "source": "synthetic control",
        "files": GAME_FILES,
        "prompt": "In scripts/player.lua rename the local variable `p` to `pos`. Don't run anything.",
        "checks": {"must_not_call": ["write_file", "create_script", "read_file", "play"]},
        "max_turns": 8,
    },
    {
        "id": "control_quaternion_question",
        "kind": "control",
        "what_happened": "A general question that mentions engine words (rotation, quaternion).",
        "text_at_issue": "none",
        "source": "synthetic control",
        "prompt": "Quick question, no need to touch the project: why do engines store rotations as quaternions instead of euler angles?",
        "checks": {"max_calls": {"*": 0}},
        "max_turns": 3,
    },
    {
        "id": "control_git_question",
        "kind": "control",
        "what_happened": "A git question in a game project; must not open the editor.",
        "text_at_issue": "none",
        "source": "synthetic control",
        "files": GAME_FILES,
        "prompt": "What did the last commit in this repo change? Just tell me.",
        "checks": {"max_calls": {"*": 0}},
        "max_turns": 6,
    },
    {
        "id": "control_gitignore",
        "kind": "control",
        "what_happened": "A repo chore; the word 'build' must not pull in build_project.",
        "text_at_issue": "none",
        "source": "synthetic control",
        "files": GAME_FILES,
        "prompt": "Add a .gitignore that ignores the build/ folder and .DS_Store files.",
        "checks": {"max_calls": {"*": 0}},
        "max_turns": 6,
    },

    # -- guards: real fixes whose text must keep working -----------------------------------------
    {
        "id": "guard_scene_not_open",
        "kind": "guard",
        "what_happened": "A call on a scene that was not open failed with 'open_scene it first' and the agent did not get the "
                         "follow-up call right; the error now gives the exact call (ec890b699).",
        "text_at_issue": "McpRefs::notOpenError: 'Open it first: open_scene {\"scene\": ..., \"close_previous\": false} "
                         "(then select_scene to switch back).'",
        "source": "git ec890b699 (MCP trial findings)",
        "prompt": "List the GameObjects in scenes/Level2.scene.",
        "mocks": [
            {"tool": "get_scene_tree", "args": {"scene": "re:Level2|^3$"}, "after": "open_scene", "result": ok(
                "Scene 3 \"Level2\" (3D, 4 GameObjects). Each line: Name [id] components (flags); children indented.\n"
                "Main Camera [40] camera\nGround [41] mesh rigidbody\nPlayer [42] script sprite\n  Gun [43] sprite\n",
                {"entity_count": 4, "scene_id": 3, "scene_name": "Level2", "truncated": False})},
            {"tool": "find_entities", "args": {"scene": "re:Level2|^3$"}, "after": "open_scene", "result": ok(
                "4 GameObject(s) match:\nMain Camera [40] camera\nGround [41] mesh rigidbody\nPlayer [42] script sprite\n"
                "Player/Gun [43] sprite", {"entities": [{"id": 43, "path": "Player/Gun"}]})},
            {"tool": "editor_status", "result": ok(
                "Project \"Space Dodge\" at the current directory. 3 scene(s), selected: 1 \"Main\". Edit mode.",
                {"project": {"name": "Space Dodge"}, "playing": False, "scenes": [
                    {"id": 1, "name": "Main", "path": "scenes/Main.scene", "selected": True, "opened": True},
                    {"id": 2, "name": "Hud", "path": "scenes/Hud.scene", "opened": True},
                    {"id": 3, "name": "Level2", "path": "scenes/Level2.scene", "opened": False}]})},
            {"tool": "get_scene_tree", "args": {"scene": "re:Level2|^3$"}, "result": err(NOT_OPEN)},
            {"tool": "find_entities", "args": {"scene": "re:Level2|^3$"}, "result": err(NOT_OPEN)},
            {"tool": "list_scenes", "result": ok(
                "3 scene(s):\n1 \"Main\" 3D scenes/Main.scene open selected start\n2 \"Hud\" 2D scenes/Hud.scene open\n"
                "3 \"Level2\" 3D scenes/Level2.scene",
                {"scenes": [{"id": 1, "name": "Main", "path": "scenes/Main.scene", "opened": True, "selected": True},
                            {"id": 2, "name": "Hud", "path": "scenes/Hud.scene", "opened": True},
                            {"id": 3, "name": "Level2", "path": "scenes/Level2.scene", "opened": False}]})},
            {"tool": "open_scene", "args": {"scene": "re:Level2|^3$"}, "result": ok(
                "Opened scene 3 \"Level2\" (scenes/Level2.scene), selected.",
                {"scene": {"id": 3, "name": "Level2", "path": "scenes/Level2.scene", "opened": True, "selected": True}})},
        ],
        "checks": {"calls": [{"tool": "open_scene", "args": {"scene": "re:Level2"}}],
                   "must_not_call": [{"tool": "open_scene", "args": {"close_previous": True}}],
                   "reply_must": [r"Gun"], "max_calls": {"open_scene": 1}},
        "max_turns": 12,
    },
    {
        "id": "guard_linked_folder_rules",
        "kind": "guard",
        "what_happened": "A folder linked in from out of the project is a folder of the project for the asset database and exports, "
                         "with its metas beside its files, but the write tools stay refused there: an agent asked answers that from the "
                         "asset guide, and neither promises a write nor tells the user to copy the folder in to ship it.",
        "text_at_issue": "docs/mcp/guide/assets.md, 'Linked folders and files'",
        "source": "docs/specs/S68-linked-folders.md R1, R7, Deviation D1",
        "prompt": "I symlinked my shared art library into assets/shared; it lives outside the project. Going by the editor's asset "
                  "guide: where do the .meta files of those assets go, do the files ship in an exported game, and can your write_file "
                  "tool add a new file there? Answer in a few lines, change nothing.",
        "checks": {"must_not_call": ["write_file", "create_folder", "move_asset"],
                   "reply_must": [r"beside|next to|alongside|same folder", r"ship|included|exported", r"refus"],
                   "reply_must_not": [r"(copy|move) (it|them|the (folder|files|library)) into the project (first|to ship|so (it|they) ship)",
                                      r"(guide|docs?) (says nothing|doesn.?t)|isn.?t stated|doesn.?t say|inference|probably|haven.?t (tested|confirmed)"]},
        "max_turns": 8,
    },
    {
        "id": "guard_read_only_linked",
        "kind": "guard",
        "what_happened": "A folder linked in from a read-only drive: its assets work with GUIDs the editor keeps, nothing is missing. "
                         "The result says so, and the agent reports it rather than taking it for a failure or changing permissions.",
        "text_at_issue": "list_missing_assets result: read_only_linked and its line",
        "source": "docs/specs/S68-linked-folders.md R7",
        "prompt": "Run the editor's missing assets check and tell me what it says about this project. Change nothing.",
        "groups_enabled": ["assets_pipeline"],
        "mocks": [
            {"tool": "list_missing_assets", "result": ok(
                "0 missing asset(s), 0 resolved by path, 0 orphan meta(s).\n1 asset(s) in a read-only linked folder "
                "(they work; their GUIDs are not saved beside them):\nassets/lib/locked/tile.png",
                {"missing": [], "resolved_by_path": [], "orphan_metas": [], "read_only_linked": ["assets/lib/locked/tile.png"]})},
        ],
        "checks": {"calls": ["list_missing_assets"], "reply_must": [r"read-only", r"tile\.png"],
                   "must_not_call": ["write_file", "delete_asset", "move_asset"], "shell_must_not": [r"chmod"]},
        "max_turns": 8,
    },
    {
        "id": "guard_curve_keys_rebind",
        "kind": "guard",
        "what_happened": "Asked to rebind a Curve window key, agents read the project, then the engine source, and told the user "
                         "to change the editor's code. The key is a catalog command: curve.frame, not the scene's Frame Selected, "
                         "which shares the key.",
        "text_at_issue": "Generated AGENTS.md, its list: 'An editor key is rebound with `set_shortcut` (tool group "
                         "`editor_ux`), not in the editor's source.'; list_shortcuts context: 'curve', 'color_picker'",
        "source": "docs/specs/S61-window-commands.md R5",
        "prompt": "In the Curve window, make the key that frames the curve J instead of F.",
        "mocks": [
            {"tool": "list_shortcuts", "args": {"context": "curve"}, "result": ok(
                "4 command(s); with keys:\ncurve.frame  F\ncurve.delete  Delete / Backspace\ncurve.copy  Ctrl+C\ncurve.paste  Ctrl+V",
                {"shortcuts": [{"id": "curve.frame", "group": "Curve", "name": "Frame Curve", "context": "curve", "chords": ["F"]}]})},
            {"tool": "list_shortcuts", "result": ok(
                "3 command(s); with keys:\nedit.frameSelected  F\nsceneView.frameAll  Home / Alt+F\ncurve.frame  F",
                {"shortcuts": [{"id": "edit.frameSelected", "group": "Edit", "name": "Frame Selected", "context": "editor", "chords": ["F"]},
                               {"id": "sceneView.frameAll", "group": "Scene View", "name": "Frame All", "context": "scene_view", "chords": ["Home", "Alt+F"]},
                               {"id": "curve.frame", "group": "Curve", "name": "Frame Curve", "context": "curve", "chords": ["F"]}]})},
            {"tool": "set_shortcut", "args": {"id": "curve.frame"}, "result": ok(
                "curve.frame is now J.", {"id": "curve.frame", "chords": ["J"]})},
            # the engine source has the curve type, not the editor's windows
            {"tool": "search_engine_source", "result": ok(
                "2 match(es).\nmath/Curve.h:24: class Curve {\nmath/Curve.cpp:18: float Curve::evaluate(float time) const {",
                {"count": 2, "matches": [{"line": 24, "path": "math/Curve.h", "text": "class Curve {"},
                                         {"line": 18, "path": "math/Curve.cpp", "text": "float Curve::evaluate(float time) const {"}]})},
        ],
        "checks": {"calls": [{"tool": "set_shortcut", "args": {"id": "curve.frame"}}],
                   "must_not_call": [{"tool": "set_shortcut", "args": {"id": "edit.frameSelected"}}],
                   "max_calls": {"set_shortcut": 1}},
        "max_turns": 8,
    },
    {
        "id": "guard_placeholder_art_path",
        "kind": "guard",
        "what_happened": "generate_placeholder_art {path: 'sprites/shield_a.png'} wrote outside assets/; the error now names the "
                         "path to use (cfdcc752a).",
        "text_at_issue": "AssetTools.cpp generate_placeholder_art: 'path is project-relative ... is outside the assets directory "
                         "... Use \"assets/sprites/shield_a.png\".'",
        "source": "git cfdcc752a (trial fixes)",
        "prompt": "Make me a placeholder shield sprite at sprites/shield_a.png.",
        "mocks": [
            {"tool": "generate_placeholder_art", "args": {"path": "re:^sprites/"}, "result": err(
                "path is project-relative, like the texture paths the other tools take: \"sprites/shield_a.png\" is outside "
                "the assets directory (\"assets\"), so no component could use it. Use \"assets/sprites/shield_a.png\".")},
            {"tool": "generate_placeholder_art", "args": {"path": "re:^assets/sprites/shield_a"}, "result": ok(
                "Wrote assets/sprites/shield_a.png (64x64, shield).",
                {"path": "assets/sprites/shield_a.png", "width": 64, "height": 64})},
        ],
        "checks": {"calls": [{"tool": "generate_placeholder_art", "args": {"path": "re:^assets/sprites/shield_a"}}],
                   "max_calls": {"generate_placeholder_art": 3}, "reply_must": [r"assets/sprites/shield_a\.png"]},
        "max_turns": 10,
    },
    {
        "id": "guard_create_rolled_back",
        "kind": "guard",
        "what_happened": "create_entity with a bad value failed after creating the GameObject; the error named an id the "
                         "rollback had removed. It now says nothing was created (cfdcc752a). The agent must create again, "
                         "not edit the dead id.",
        "text_at_issue": "EntityTools.cpp: 'Nothing was created (create_entity is one step and was rolled back): ...'",
        "source": "git cfdcc752a (trial fixes)",
        "prompt": "Add a sprite GameObject named Ghost that uses assets/sprites/ghost.png.",
        "mocks": [
            {"tool": "create_entity", "once": True, "result": err(
                "Nothing was created (create_entity is one step and was rolled back): values.sprite.texture: "
                "\"sprites/ghost.png\" is not an asset of this project (asset paths are project-relative, e.g. "
                "\"assets/sprites/ghost.png\").")},
            {"tool": "create_entity", "result": ok(
                "Created Sprite \"Ghost\" [21] at Ghost (one undo step).",
                {"entity": {"id": 21, "name": "Ghost", "path": "Ghost"}, "scene": {"id": 1, "name": "Main"},
                 "undo": {"id": 7, "name": "eval: Create GameObject (Ghost)"}})},
            {"tool": "set_properties", "result": ok(
                "Set 1 property of GameObject 21 \"Ghost\".", {"entity": {"id": 21, "path": "Ghost"}})},
            {"tool": "save_scene", "result": ok("Saved scene 1 \"Main\" (scenes/Main.scene).",
                                                {"scene": {"id": 1, "path": "scenes/Main.scene"}})},
            {"tool": "list_assets", "result": ok("1 match(es):\nassets/sprites/ghost.png (texture, 64x64)",
                                                {"assets": [{"path": "assets/sprites/ghost.png", "type": "texture"}]})},
            {"tool": "get_asset_info", "result": ok("assets/sprites/ghost.png: PNG 64x64, 4 channels, used by nothing.",
                                                   {"path": "assets/sprites/ghost.png", "width": 64, "height": 64})},
        ],
        "checks": {"min_calls": {"create_entity": 2}, "max_calls": {"create_entity": 3}},
        "max_turns": 12,
    },
    {
        "id": "guard_unknown_argument",
        "kind": "guard",
        "what_happened": "The registry refuses an unknown argument and lists the valid ones; the agent fixes the call instead "
                         "of repeating it (guide: 'fix the arguments, don't repeat').",
        "text_at_issue": "McpRegistry.cpp: 'Unknown argument \"k\" for <tool>. Arguments: ...'",
        "source": "git 11383d085 (registry check); guide 'The loop'",
        "prompt": "Rename the GameObject Enemy to Boss.",
        "mocks": [
            {"tool": "rename_entity", "args": {"entity": "re:Enemy|^14$", "name": "Boss"}, "result": ok(
                "Renamed GameObject 14 \"Enemy\" to \"Boss\".",
                {"entity": {"id": 14, "name": "Boss", "path": "Boss"}, "undo": {"id": 3, "name": "eval: Rename (Boss)"}})},
            {"tool": "find_entities", "after": "rename_entity", "result": ok(
                "1 GameObject(s) match:\nBoss [14] script sprite", {"entities": [{"id": 14, "path": "Boss"}]})},
            {"tool": "get_entity", "after": "rename_entity", "result": ok(
                "Boss [14] components: script sprite transform", {"id": 14, "name": "Boss", "path": "Boss"})},
            {"tool": "get_scene_tree", "after": "rename_entity", "result": ok(
                "Scene 1 \"Main\" (3D, 8 GameObjects). Each line: Name [id] components (flags); children indented.\n"
                "Main Camera [10] camera\nGround [11] mesh rigidbody\nPlayer [12] script sprite\n  Gun [13] sprite\n"
                "  Shield [18] sprite\nBoss [14] script sprite\nCrate [15] mesh\n", {"scene_id": 1})},
            {"tool": "find_entities", "result": ok("1 GameObject(s) match:\nEnemy [14] script sprite",
                                                  {"entities": [{"id": 14, "path": "Enemy"}]})},
            {"tool": "save_scene", "result": ok("Saved scene 1 \"Main\" (scenes/Main.scene).", {"scene": {"id": 1}})},
        ],
        "checks": {"calls": [{"tool": "rename_entity", "args": {"name": "Boss"}}], "max_calls": {"rename_entity": 2}},
        "max_turns": 10,
    },

    # -- bad: real mistakes, each at the decision where the text misled ------------------------
    {
        "id": "bad_profiling_group",
        "playing": True,
        "kind": "bad",
        "what_happened": "Three agents never found the profiling group (tool_groups was never called in the session); they "
                         "grepped docs, parsed stderr and read /proc from Lua, and one concluded there were no draw-call stats.",
        "text_at_issue": "Guide 'Profiling' digest names get_frame_times/capture_profile; 'Tool groups' line; the group tools "
                         "are not listed until tool_groups enables them",
        "source": "game-port chat (3 implementer subagents)",
        "prompt": "The game is playing in the editor and it stutters every few seconds. Measure which frames are slow and "
                  "how many draw calls a frame makes. Measure, don't guess.",
        "mocks": [
            {"tool": "get_play_state", "result": ok("Play state: playing (\"Main\"). Session: 41 s, 0 error(s) logged. Script coroutines: 2, timers: 1.",
                                                   {"state": "playing", "scene": {"id": 1, "name": "Main"}, "frame": 2460})},
            {"tool": "get_frame_times", "result": ok(
                "300 frames: frame_ms median 16.7, p95 18.1, p99 61.0, max 74.2 (cpu 15.9 / gpu 6.1 median); spikes over "
                "40 ms: 5, every ~120 frames (frames 2104, 2224, 2344...).",
                {"window": 300, "frame_ms": {"median": 16.7, "p95": 18.1, "p99": 61.0, "max": 74.2},
                 "spikes": [{"frame": 2104, "ms": 61.0}, {"frame": 2224, "ms": 74.2}], "spike_interval_frames": 120,
                 "monitors": {"Draw calls": {"median": 412}}})},
            {"tool": "get_render_stats", "result": ok(
                "game view frame 2461: 412 draws, 1903 instances, 210k triangles, 14 passes, 96 pipelines.",
                {"view": "game", "draws": 412, "instances": 1903, "triangles": 210113, "passes": 14})},
            {"tool": "get_cpu_profile", "result": ok(
                "Frame 2224 (74.2 ms): Lua GC 38.0 ms, scripts/spawner.lua:onUpdate 21.4 ms, render 9.8 ms.",
                {"frame": 2224, "scopes": [{"name": "Lua GC", "ms": 38.0}, {"name": "spawner.lua:onUpdate", "ms": 21.4}]})},
            {"tool": "capture_profile", "result": ok("Captured 300 frames: top scopes on spike frames: GC 38 ms (Lua).",
                                                    {"frames": 300, "top": [{"scope": "Lua GC", "ms": 38.0}]})},
            {"tool": "read_console", "result": ok("0 entries (0 error(s), 0 warning(s)). next_cursor 88.", {"entries": [], "next_cursor": 88})},
            {"tool": "search_engine_api", "args": {"query": "re:(?i)draw|frame|profil|stat|fps|perf|stutter|spike"},
             "result": {"recorded": "search_api_profiling"}},
            {"tool": "search_engine_source", "result": {"recorded": "search_source_drawcalls"}},
        ],
        "checks": {"any_of": [{"calls": ["get_frame_times"]}, {"calls": ["capture_profile"]}],
                   "reply_must": [r"412|draw"], "shell_must_not": [r"/proc|stderr|FRAME_STATS"]},
        "max_turns": 24,
    },
    {
        "id": "guard_meshes_dry_run",
        "kind": "guard",
        "what_happened": "Asked to shrink a game's models for mobile, an agent must find the meshes group, measure the models, "
                         "try lossy compression as a dry run before writing it, and apply it with set_model_import_settings "
                         "instead of editing .meta files or re-exporting the sources (S53: new tools and text).",
        "text_at_issue": "Guide 'Other areas' meshes (inspect_models, check_render_budgets); inspect_models' dry run (settings); "
                         "set_model_import_settings' 'Dry run first'",
        "source": "synthetic guard (S53 R22, R24)",
        "files": GAME_FILES,
        "prompt": "The mobile build downloads too much. Make the 3D models smaller for mobile without changing how they look "
                  "much, and tell me how much it saves. Check before you change anything.",
        "mocks": [
            {"tool": "inspect_models", "args": {"settings": "re:."}, "result": ok(
                "Dry run (nothing written; the numbers are the real cook's):\nassets/models/rock.glb: 41,204 tris, memory 3.2 MB -> 1.3 MB, "
                "download 4.1 MB -> 0.6 MB, 3 LODs, change: Up to 0.02% of the model's size",
                {"dry_run": True, "written": False, "models": [{"path": "assets/models/rock.glb", "platform": "mobile",
                 "predicted": {"source_bytes": 4300000, "download_bytes": 630000, "visual_change": "Up to 0.02% of the model's size"}}]})},
            {"tool": "inspect_models", "result": ok(
                "2 model(s), all models, mobile: 52,000 tris, memory 4.0 MB, download 5.2 MB, 2 cooked, 0 error(s), 0 warning(s).\n"
                "assets/models/rock.glb: 41,204 tris, 3.2 MB, 4.1 MB download, 3 LODs, cooked; rock.glb downloads 4.1 MB: Mesh "
                "Compression Medium stores it in fewer bits (dry run with inspect_models settings first)",
                {"platform": "mobile", "total": 2, "models": [{"path": "assets/models/rock.glb", "download_bytes": 4300000, "issues": [
                    {"code": "mesh-download-large", "severity": "info", "message": "rock.glb downloads 4.1 MB",
                     "fix": {"tool": "set_model_import_settings", "arguments": {"paths": ["assets/models/rock.glb"], "settings": {"mesh_compression": "medium"}}}}]},
                    {"path": "assets/models/tree.glb", "download_bytes": 900000, "issues": []}]})},
            {"tool": "set_model_import_settings", "result": ok(
                "Updated 1 model(s) (0 unchanged); cooked, 0 failed. Undo with scope project.",
                {"updated": ["assets/models/rock.glb"], "unchanged": 0, "models": [{"path": "assets/models/rock.glb", "download_bytes": 630000}]})},
            {"tool": "reimport_models", "result": ok("Reimported 2 model(s).", {"models": []})},
        ],
        "checks": {"calls": ["inspect_models", "set_model_import_settings"],
                   "order": [[{"tool": "inspect_models", "args": {"settings": "re:."}}, "set_model_import_settings"]],
                   "must_not_call": ["write_file"], "shell_must_not": [r"\.meta", r"gltfpack"],
                   "reply_must": [r"MB|%"]},
        "max_turns": 24,
    },
    {
        "id": "guard_meshes_load_error",
        "kind": "guard",
        "what_happened": "An export fails because a model requires a glTF extension the engine cannot read. The agent must read "
                         "the model's load findings, tell the user to export the model again without that extension, and not "
                         "try import settings or edit the .meta / the file (S53 R5: the loader's findings in inspect_models).",
        "text_at_issue": "Guide meshes 'What the loader finds' (gltf-required-extension: status failed, exports stop until the model is "
                         "exported again without it); inspect_models' load_diagnostics",
        "source": "synthetic guard (S53 R5, R22)",
        "files": GAME_FILES,
        "prompt": "The export fails with something about assets/models/crate.glb. Find out why and tell me what to do; don't "
                  "change anything yet.",
        "mocks": [
            {"tool": "inspect_models", "result": ok(
                "1 model(s), desktop: 1,200 tris, memory 0.1 MB, download 0.1 MB, 0 cooked, 1 error(s), 0 warning(s).\n"
                "assets/models/crate.glb: glTF assets/models/crate.glb requires VENDOR_shapes, which the engine does not read; "
                "it is loaded without it (export the model again without VENDOR_shapes); it does not cook, and exports fail, until it is fixed",
                {"platform": "desktop", "total": 1, "models": [{"path": "assets/models/crate.glb", "status": "failed", "issues": [
                    {"code": "gltf-required-extension", "severity": "error",
                     "message": "glTF assets/models/crate.glb requires VENDOR_shapes, which the engine does not read; it is loaded "
                                "without it (export the model again without VENDOR_shapes); it does not cook, and exports fail, until it is fixed"}],
                    "load_diagnostics": [{"severity": "error", "extension": "VENDOR_shapes", "part": "",
                                          "message": "glTF assets/models/crate.glb requires VENDOR_shapes, which the engine does not read; it is loaded without it",
                                          "fix": "export the model again without VENDOR_shapes", "cook_fixes": False}]}]})},
            {"tool": "reimport_models", "result": ok("Reimported 1 model(s).", {"models": []})},
        ],
        "checks": {"calls": ["inspect_models"],
                   "must_not_call": ["set_model_import_settings", "write_file"], "shell_must_not": [r"\.meta", r"gltfpack"],
                   "reply_must": [r"VENDOR_shapes", r"(?i)export|re-?export"]},
        "max_turns": 16,
    },
    {
        "id": "bad_game_view_resolution",
        "kind": "bad",
        "what_happened": "Asked for a 20:9 phone view, agents sent configure_game_view {aspect: '20:9'}, got the aspect enum "
                         "error, never found `resolution` and edited settings.yaml / restarted the editor (6 agents).",
        "text_at_issue": "configure_game_view: 'aspect (project = the canvas size), resolution (the game and screenshots "
                         "render at it...)'; resolution param: '\"WxH\", \"WxH@dpi\", a name, free, or {width, height, dpi, name}'",
        "source": "game-port chat (6 agents, ~20 editor restarts)",
        "prompt": "Make the Game view render at 2400x1080 (a 20:9 phone in landscape) so I can check the HUD layout, then "
                  "take a Game view screenshot.",
        "mocks": [
            {"tool": "configure_game_view", "args": {"aspect": "20:9"}, "result": err(
                "aspect must be free, project (the project's canvas size), 16:9, 16:10, 4:3 or 9:16.")},
            {"tool": "configure_game_view", "args": {"resolution": "*"}, "result": ok(
                "Game view: resolution 2400x1080 (custom), aspect free, scale 1; the game and screenshots render at 2400x1080.",
                {"resolution": {"width": 2400, "height": 1080, "dpi": 96, "name": "2400x1080"}, "aspect": "free",
                 "render_size": [2400, 1080]})},
            {"tool": "configure_game_view", "result": ok(
                "Game view: aspect project (1280x720), resolution free, scale 1, device none.",
                {"aspect": "project", "resolution": {"name": "free"}, "render_size": [1280, 720]})},
            {"tool": "search_engine_api", "args": {"query": "re:(?i)game.?view|resolution|aspect|device|screen|window"},
             "result": {"recorded": "search_api_game_view"}},
            {"tool": "get_project_settings", "args": {"search": "re:(?i)game.?view|resolution|aspect|device|simulat"},
             "result": {"recorded": "settings_search_game_view"}},
            {"tool": "screenshot", "result": ok("Game view 2400x1080 (scaled to 768x346).",
                                               {"view": "game", "width": 2400, "height": 1080})},
        ],
        "checks": {"any_of": [{"calls": [{"tool": "configure_game_view", "args": {"resolution": "re:2400\\s*[x×]\\s*1080"}}]},
                              {"calls": [{"tool": "configure_game_view",
                                          "args": {"resolution": {"width": 2400, "height": 1080}}}]}],
                   "must_not_call": [{"tool": "configure_game_view", "args": {"aspect": "20:9"}}],
                   "edits_must_not": [r"settings\.yaml|\.ini$"], "shell_must_not": [r"settings\.yaml"]},
        "max_turns": 24,
    },
    {
        "id": "bad_layer_via_set_properties",
        "kind": "bad",
        "what_happened": "Agents set a GameObject's layer and active flag through set_properties (transform.layer, "
                         "entity.active) and got 'Unknown property \"layer\" of transform'; set_entity_state does both.",
        "text_at_issue": "set_entity_state: 'Sets GameObjects' active (SetActive), tag ... and/or layer'; set_properties' "
                         "unknown-property error lists transform's properties only",
        "source": "game-port chat (~5 calls, 2 agents)",
        "prompt": "Put the GameObject Player on layer 12 and deactivate Player/Shield. Save the scene.",
        "mocks": [
            {"tool": "set_properties", "args": {"values": {"transform": {"layer": "*"}}}, "result": err(
                "Unknown property \"layer\" of transform on GameObject 12 \"Player\". Properties: billboard, "
                "billboardRotation, cylindricalBillboard, fakeBillboard, position, rotation, scale, visible (custom ones "
                "also by their scene-file keys, see get_entity detail=\"yaml\").")},
            {"tool": "set_entity_state", "result": ok("Set the state of 1 GameObject.",
                                                     {"entities": [{"id": 12, "path": "Player"}]})},
            {"tool": "get_entity", "args": {"entity": "re:^(12|Player)$"}, "after": "set_entity_state", "result": ok(
                "Player [12] components: script sprite transform (layer 12)",
                {"id": 12, "path": "Player", "layer": 12, "active": True})},
            {"tool": "get_entity", "args": {"entity": "re:^(18|Player/Shield|Shield)$"}, "after": "set_entity_state",
             "result": ok("Shield [18] components: sprite transform (inactive)",
                          {"id": 18, "path": "Player/Shield", "layer": 0, "active": False})},
            {"tool": "save_scene", "result": ok("Saved scene 1 \"Main\" (scenes/Main.scene).", {"scene": {"id": 1}})},
        ],
        "checks": {"calls": [{"tool": "set_entity_state", "args": {"layer": "re:^12$"}},
                             {"tool": "set_entity_state", "args": {"active": False}}],
                   "must_not_call": ["set_properties"]},
        "max_turns": 12,
    },
    {
        "id": "bad_project_yaml_by_hand",
        "kind": "bad",
        "what_happened": "An agent hand-wrote project.yaml for tags, layers and layer collisions after grepping the encoder "
                         "for its schema; set_project_settings covers them.",
        "text_at_issue": "Guide 'The loop': 'Edit scenes, prefabs and settings with tools, never by hand-editing ... "
                         "project.yaml'; get/set_project_settings descriptions",
        "source": "game-port chat (x4 hand edits of project and scene files)",
        "prompt": "Add a tag named Pickup, name layer 8 \"Enemies\", and make layer 8 not collide with itself.",
        "mocks": [
            {"tool": "get_project_settings", "result": ok(
                "tags_and_layers.tags: [] (strings); tags_and_layers.layers: [{index 0 \"Default\"} ... 31] (merge by index: "
                "[{\"index\": 8, \"name\": \"Enemies\"}]); physics.layer_collision_matrix: 32 bit masks, row i = the layers i "
                "collides with (default all).",
                {"tags_and_layers.tags": [], "tags_and_layers.layers": [{"index": 0, "name": "Default"}],
                 "physics.layer_collision_matrix": [4294967295] * 32})},
            {"tool": "get_project_settings", "after": "set_project_settings", "result": ok(
                "tags_and_layers.tags: [\"Pickup\"]; tags_and_layers.layers: [{index 0 \"Default\"}, {index 8 \"Enemies\"}]; "
                "physics.layer_collision_matrix: row 8 = 4294967039 (8 off)",
                {"tags_and_layers.tags": ["Pickup"], "tags_and_layers.layers": [{"index": 0, "name": "Default"},
                                                                               {"index": 8, "name": "Enemies"}]})},
            {"tool": "set_project_settings", "result": ok("Set 3 project settings; saved project.yaml.",
                                                         {"changed": ["tags_and_layers.tags", "tags_and_layers.layers",
                                                                      "physics.layer_collision_matrix"]})},
        ],
        "checks": {"calls": ["set_project_settings"], "edits_must_not": [r"project\.yaml$"],
                   "shell_must_not": [YAML_WRITE]},
        "max_turns": 12,
    },
    {
        "id": "bad_duplicate_name",
        "kind": "bad",
        "what_happened": "create_entity renamed the new camera \"Main Camera 2\" because the name was taken; the agent then "
                         "addressed \"Main Camera\" and both edits landed on the old camera.",
        "text_at_issue": "create_entity result: 'Created Camera \"Main Camera 2\" [1004] at Main Camera 2 (one undo step).'; "
                         "guide Addressing: 'prefer ids within a session'",
        "source": "game-port chat (x1, high impact)",
        "history": [
            {"user": "Add a second camera for the minimap."},
            {"assistant": "Adding a camera.", "calls": [{"tool": "create_entity", "args": {"type": "camera"},
                "result": "Created Camera \"Main Camera 2\" [1004] at Main Camera 2 (one undo step)."}]},
            {"assistant": "Added the camera."},
        ],
        "prompt": "Make the new camera the main camera and move it to (0, 20, 0).",
        "mocks": [
            {"tool": "set_main_camera", "result": ok("Main camera of scene 1 \"Main\": GameObject 1004 \"Main Camera 2\".",
                                                    {"entity": {"id": 1004, "path": "Main Camera 2"}})},
            {"tool": "set_properties", "result": ok("Set 1 property of GameObject 1004 \"Main Camera 2\".",
                                                   {"entity": {"id": 1004, "path": "Main Camera 2"}})},
            {"tool": "set_transform", "result": ok("Moved GameObject 1004 \"Main Camera 2\" to [0, 20, 0].",
                                                  {"entity": {"id": 1004, "path": "Main Camera 2"}})},
            {"tool": "save_scene", "result": ok("Saved scene 1 \"Main\" (scenes/Main.scene).", {"scene": {"id": 1}})},
        ],
        "checks": {"must_not_call": [{"tool": "set_main_camera", "args": {"entity": "Main Camera"}},
                                     {"tool": "set_properties", "args": {"entity": "Main Camera"}},
                                     {"tool": "set_transform", "args": {"entity": "Main Camera"}}],
                   "calls": ["set_main_camera"]},
        "max_turns": 10,
    },
    {
        "id": "bad_scene_settings_key",
        "kind": "bad",
        "what_happened": "Agents sent set_scene_settings {values: {...}} and then top-level keys before finding `settings`; "
                         "one generator swallowed the error and the setting was never applied.",
        "text_at_issue": "set_scene_settings: 'Set scene settings {name: value} (names and shapes: get_scene_settings)'; "
                         "`settings` has no description (the session's call set post_processing; this case uses two other "
                         "real settings)",
        "source": "game-port chat (2 agents)",
        "prompt": "Turn on SSAO in the current scene with intensity 1.5, and set its 3D gravity to (0, -20, 0).",
        "mocks": [
            {"tool": "get_scene_settings", "after": "set_scene_settings", "result": ok(
                "Scene 1 \"Main\" settings:\nphysics_gravity_3d = [0.0,-20.0,0.0]\nssao_enabled = true\nssao_radius = 0.5\n"
                "ssao_intensity = 1.5\nssao_bias = 0.025", {"scene_id": 1, "settings": {
                    "physics_gravity_3d": [0, -20, 0], "ssao_enabled": True, "ssao_intensity": 1.5}})},
            {"tool": "get_scene_settings", "result": {"recorded": "get_scene_settings"}},
            {"tool": "set_scene_settings", "args": {"settings": "*"}, "result": ok(
                "Set 3 scene setting(s) (one undo step).", {"scene_id": 1, "changed": ["ssao_enabled", "ssao_intensity",
                                                                                         "physics_gravity_3d"]})},
        ],
        "checks": {"calls": [{"tool": "set_scene_settings", "args": {"settings": {"ssao_enabled": True}}}],
                   "max_errors": {"set_scene_settings": 0}},
        "max_turns": 10,
    },
    {
        "id": "bad_touch_action",
        "playing": True,
        "kind": "bad",
        "what_happened": "Agents sent touch events with action down/up (the mouse words) and got 'touch action must be tap, "
                         "start, move, end, cancel or swipe' (2 agents).",
        "text_at_issue": "send_input description: 'touch: action tap|start|move|end|cancel|swipe'",
        "source": "game-port chat (x3)",
        "prompt": "The game is playing. Swipe up on the touchscreen from (400, 900) to (400, 300), then take a Game view "
                  "screenshot.",
        "mocks": [
            {"tool": "send_input", "args": {"events": [{"type": "touch", "action": "re:^(swipe|start)$"}]}, "result": ok(
                "Sent 1 event over 12 frames; playing.", {"frames": 12, "state": "playing"})},
            {"tool": "send_input", "result": err("events[0]: touch action must be tap, start, move, end, cancel or swipe.")},
            {"tool": "screenshot", "result": ok("Game view 1280x720 (scaled to 768x432).", {"view": "game"})},
            {"tool": "get_play_state", "result": ok("Play state: playing (\"Main\"). Session: 6 s, 0 error(s) logged. Script coroutines: 0, timers: 0.", {"state": "playing"})},
        ],
        "checks": {"calls": ["send_input"], "max_errors": {"send_input": 0}},
        "max_turns": 10,
    },
    {
        "id": "bad_app_hide",
        "playing": True,
        "kind": "bad",
        "what_happened": "A minimized window was untestable from MCP: agents asked for a real minimize (xdotool) or paused the "
                         "game, which stops its updates, where a minimized desktop game keeps updating without drawing.",
        "text_at_issue": "send_input description: 'app: action pause|resume|focus_lost|focus_gained|low_memory|hide|show|orientation'",
        "source": "platform-lifecycle spec (2026-11-04), D7",
        "prompt": "The game is playing. Check how it behaves while its window is minimized: minimize it for two seconds, "
                  "then restore it. Do not pause the game.",
        "mocks": [
            {"tool": "send_input", "args": {"events": [{"type": "app", "action": "hide"}]}, "result": ok(
                "Sent 3 events over 121 frames; playing.", {"frames": 121, "state": "playing"})},
            {"tool": "send_input", "args": {"events": [{"type": "app", "action": "show"}]}, "result": ok(
                "Sent 1 event over 1 frame; playing.", {"frames": 1, "state": "playing"})},
            {"tool": "get_play_state", "result": ok("Play state: playing (\"Main\"). Session: 9 s, 0 error(s) logged.",
                                                     {"state": "playing", "platform": {"visible": True, "focused": True,
                                                                                        "paused": False}})},
        ],
        "checks": {"calls": [{"tool": "send_input", "args": {"events": [{"type": "app", "action": "hide"}]}}],
                   "must_not_call": ["play"], "shell_must_not": [r"xdotool"]},
        "max_turns": 10,
    },
    {
        "id": "bad_batch_args_key",
        "kind": "bad",
        "what_happened": "batch items used `args` instead of `arguments`; the key was dropped silently, the item ran with no "
                         "arguments and its error ('entity must be a GameObject id...') sent the agent switching id types.",
        "text_at_issue": "batch schema: calls array of object {arguments, tool}; the item's unknown key is not rejected",
        "source": "game-port chat (x2)",
        "history": [
            {"user": "Read the Player and the Enemy GameObjects in one round-trip."},
            {"assistant": "Reading both in one batch.", "calls": [{"tool": "batch", "args": {"calls": [
                {"tool": "get_entity", "args": {"entity": "Player"}}, {"tool": "get_entity", "args": {"entity": "Enemy"}}]},
                "result": "batch: 0 ok, 1 failed, stopped at the first error.\n0. get_entity: ERROR: entity must be a "
                          "GameObject id (integer), a name path like \"Player/Gun\", or {\"id\"|\"name\"|\"path\"}."}]},
        ],
        "prompt": "Continue.",
        "mocks": [
            {"tool": "batch", "args": {"calls": [{"arguments": "*"}]}, "result": ok(
                "batch: 2 ok, 0 failed.\n0. get_entity: GameObject 12 \"Player\" ...\n1. get_entity: GameObject 14 \"Enemy\" ...",
                {"results": [{"ok": True}, {"ok": True}], "succeeded": 2, "failed": 0})},
            {"tool": "batch", "result": ok("batch: 0 ok, 1 failed, stopped at the first error.\n0. get_entity: ERROR: entity "
                                           "must be a GameObject id (integer), a name path like \"Player/Gun\", or "
                                           "{\"id\"|\"name\"|\"path\"}.", {"succeeded": 0, "failed": 1})},
            {"tool": "get_entity", "result": ok("GameObject 12 \"Player\": transform, sprite, script.", {"id": 12})},
        ],
        "checks": {"any_of": [{"calls": [{"tool": "batch", "args": {"calls": [{"arguments": "*"}]}}]},
                              {"calls": ["get_entity"], "must_not_call": ["batch"]}],
                   "max_calls": {"batch": 2}},
        "max_turns": 8,
    },
    {
        "id": "bad_console_cursor",
        "kind": "bad",
        "what_happened": "Agents read the Console without `since` after a fix and took the previous run's errors as current.",
        "text_at_issue": "Guide 'The loop' 6: 'Read the Console since your last cursor'; read_console `since`",
        "source": "game-port chat (~10 reads without a cursor)",
        "files": GAME_FILES,
        "history": [
            {"user": "Play the game and check the Console."},
            {"assistant": "Starting play.", "calls": [{"tool": "play", "args": {"action": "start"},
                "result": "Playing \"Main\". Started in 0.30 s. 1 error in the console:\n[error #41] scripts/player.lua:15: "
                          "attempt to index a nil value (field 'speed')\nnext_cursor: 41"}]},
            {"assistant": "Stopping to fix it.", "calls": [{"tool": "play", "args": {"action": "stop"},
                "result": "Stopped. \"Main\" played 3 s and was stopped (1 error(s), 0 warning(s) logged). The scene is back to "
                          "its edit-time state."}]},
            {"assistant": "The error is at scripts/player.lua:15 (`self.speed` is nil). Tell me when to retry."},
            {"user": "I fixed the script myself."},
        ],
        "prompt": "Play again and wait a couple of seconds. Is the error gone?",
        "mocks": [
            {"tool": "play", "args": {"action": "start"}, "result": ok(
                "Playing \"Main\". Started in 0.31 s. No errors in the console. next_cursor: 52", {"state": "playing", "console": {"problems": 0, "next_cursor": 52}})},
            {"tool": "wait", "result": ok("Waited 2.0 s, 120 frame(s); playing. Console: no new lines.",
                                         {"reason": "seconds", "state": "playing", "console": []})},
            {"tool": "read_console", "args": {"since": "*"}, "result": ok("0 entries since #52 (0 error(s), 0 warning(s)). next_cursor 60.",
                                                                       {"entries": [], "next_cursor": 60})},
            {"tool": "read_console", "result": ok(
                "3 entries (1 error(s), 0 warning(s)):\n[error #41] scripts/player.lua:15: attempt to index a nil value "
                "(field 'speed')\n[info #42] Play stopped by a script error\n[info #52] Play started: \"Main\"\nnext_cursor 60.",
                {"entries": [{"id": 41, "type": "error", "message": "scripts/player.lua:15: attempt to index a nil value (field 'speed')"},
                             {"id": 42, "type": "info", "message": "Play stopped by a script error"},
                             {"id": 52, "type": "info", "message": "Play started"}], "next_cursor": 60})},
            {"tool": "get_play_state", "result": ok("Play state: playing (\"Main\"). Session: 6 s, 0 error(s) logged. Script coroutines: 0, timers: 0.", {"state": "playing"})},
            {"tool": "play", "args": {"action": "stop"}, "result": ok("Stopped. \"Main\" played 2 s and was stopped (0 error(s), 0 warning(s) logged). The scene is back to its edit-time state.",
                                                                     {"state": "edit"})},
        ],
        "checks": {"calls": [{"tool": "play", "args": {"action": "start"}}],
                   "reply_must": [r"\b(gone|fixed|no (more )?(errors?|problems))\b"],
                   "reply_must_not": [r"still (there|present|happening|failing)|persists"]},
        "max_turns": 10,
    },
    {
        "id": "bad_model_info_asset",
        "kind": "bad",
        "what_happened": "To list a model file's clips the agent sent get_model_info {model: <fbx>} (unknown argument), then "
                         "created throwaway GameObjects to read them and deleted them, twice; get_asset_info reads the file.",
        "text_at_issue": "get_model_info: 'What a Model loaded' (entity only); get_asset_info: 'model meshes, materials, "
                         "animations, nodes'",
        "source": "game-port chat (x2)",
        "prompt": "Which animation clips does assets/models/hero.fbx contain? Don't change the scene.",
        "mocks": [
            {"tool": "get_asset_info", "args": {"path": "re:hero\\.fbx"}, "result": ok(
                "assets/models/hero.fbx (FBX, converted to glTF): 3 meshes, 2 materials, 4 animations: Idle (2.0 s, loop), "
                "Run (0.8 s, loop), Jump (1.1 s), Die (1.6 s); 52 nodes.",
                {"path": "assets/models/hero.fbx", "type": "model", "animations": [
                    {"name": "Idle", "length": 2.0}, {"name": "Run", "length": 0.8}, {"name": "Jump", "length": 1.1},
                    {"name": "Die", "length": 1.6}]})},
            {"tool": "list_assets", "result": ok("1 asset: assets/models/hero.fbx (model)",
                                                {"assets": [{"path": "assets/models/hero.fbx", "type": "model"}]})},
        ],
        "checks": {"calls": [{"tool": "get_asset_info", "args": {"path": "re:hero\\.fbx"}}],
                   "must_not_call": ["create_entity", "instantiate_prefab"], "reply_must": [r"Idle", r"Run"]},
        "max_turns": 8,
    },

    {
        "id": "bad_lod_levels_by_hand",
        "kind": "bad",
        "what_happened": "Asked how a model's detail levels look and where they switch, agents placed copies of the model at "
                         "distances, moved the camera and screenshotted each, guessing which level drew; capture_model_lods "
                         "(meshes group) draws every level side by side with its triangles, error and switch height.",
        "text_at_issue": "capture_model_lods: 'Contact sheet of a mesh's detail levels ... the screen height it switches "
                         "below'; tool groups line 'meshes: ... detail levels (LOD) and their captures'",
        "source": "S54 R14 (docs/forge-meshopt-analysis/jasper-ux-agents.md, LOD verification walkthrough)",
        "prompt": "Before we ship to phones: show me what the detail levels of models/rock.glb look like, and at what size on "
                  "screen each one kicks in. Don't change the scene.",
        "mocks": [
            {"tool": "capture_model_lods", "args": {"path": "re:rock\\.glb"}, "result": ok(
                "models/rock.glb: 4 level(s) (built as the runtime would). LOD 0: 4800 tris; LOD 1: 2400 tris, error 0.0031, "
                "below 645 px, PSNR 38.2 dB; LOD 2: 1200 tris, error 0.0074, below 270 px, PSNR 33.0 dB; LOD 3: 600 tris, "
                "error 0.0160, below 125 px, PSNR 28.4 dB;",
                {"path": "models/rock.glb", "source": "built", "levels": [
                    {"level": 0, "triangles": 4800, "error": 0.0, "switches_below_px": None, "psnr_db": None},
                    {"level": 1, "triangles": 2400, "error": 0.0031, "switches_below_px": 645, "psnr_db": 38.2},
                    {"level": 2, "triangles": 1200, "error": 0.0074, "switches_below_px": 270, "psnr_db": 33.0},
                    {"level": 3, "triangles": 600, "error": 0.016, "switches_below_px": 125, "psnr_db": 28.4}]})},
        ],
        "checks": {"calls": [{"tool": "capture_model_lods", "args": {"path": "re:rock\\.glb"}}],
                   "must_not_call": ["create_entity", "instantiate_prefab", "set_scene_camera"], "reply_must": [r"645|270|125"]},
        "max_turns": 10,
    },
    {
        "id": "bad_lod_tint_by_hand",
        "kind": "bad",
        "what_happened": "Asked which objects draw which detail level, agents changed materials to colour objects by "
                         "their level and later had to put the materials back; the Scene view's LOD Level Draw Mode "
                         "tints every mesh by the level it draws, editor only.",
        "text_at_issue": "set_scene_camera display: '... draw_mode: \"lod_level\"'; meshes guide 'See the levels in "
                         "the Scene view'",
        "source": "S54 R11 (docs/forge-meshopt-analysis/jasper-ux-human.md 6.3)",
        "prompt": "In the Main scene, show me which objects are drawing which LOD level right now. Don't change any "
                  "object or material.",
        "mocks": [
            {"tool": "set_scene_camera", "args": {}, "result": ok(
                "Scene view camera of \"Main\": {}.", {"display": {"draw_mode": "lod_level", "show_stats": True}})},
            {"tool": "screenshot", "args": {}, "result": ok("Scene view, 1280x720.", {"view": "scene"})},
        ],
        "checks": {"calls": [{"tool": "set_scene_camera", "args": {"display": {"draw_mode": "lod_level"}}}],
                   "must_not_call": ["set_properties", "set_material", "create_material"], "reply_must": [r"(?i)lod|level"]},
        "max_turns": 10,
    },
    {
        "id": "bad_aseprite_by_hand",
        "kind": "bad",
        "what_happened": "Asked to put an Aseprite character in the scene, agents exported PNG strips with a shell "
                         "aseprite --sheet, sliced them with set_sprite_frames {texture, slice} and built the walk "
                         "animation frame by frame; the editor imports .aseprite files itself (S44).",
        "text_at_issue": "guide Other areas: 'sprites (set_sprite_frames sheet)'; set_sprite_frames "
                         "description: 'sheet (.aseprite + tag clips, texture, .spriteatlas)'",
        "source": "S44 design review (pixel-art trial sessions, paraphrased)",
        "files": {"assets/sprites/hero.aseprite": "binary placeholder"},
        "prompt": "Add assets/sprites/hero.aseprite to the scene as an animated character that plays its idle animation.",
        "mocks": [
            {"tool": "get_entity", "args": {"entity": "re:(?i)^(40|hero)$"}, "result": ok("Hero [40] components: sprite transform; sprite: no sheet", {"id": 40, "name": "Hero"})},
            {"tool": "create_entity", "result": ok("Created GameObject 40 \"Hero\" (sprite) in scene 1 \"Main\".", {"entity": {"id": 40, "path": "Hero"}})},
            {"tool": "get_asset_info", "args": {"path": "re:hero\\.aseprite"}, "result": ok(
                "assets/sprites/hero.aseprite (Aseprite, RGBA 32x32): 12 frames, 3 layers, tags idle (0-3), walk (4-11); "
                "imported: 1 page 128x96, clips assets/sprites/hero.aseprite#idle, #walk.",
                {"path": "assets/sprites/hero.aseprite", "type": "aseprite_sprite", "frames": 12,
                 "tags": ["idle", "walk"], "outputs": {"clips": [
                     {"name": "idle", "reference": "assets/sprites/hero.aseprite#idle"},
                     {"name": "walk", "reference": "assets/sprites/hero.aseprite#walk"}]}})},
            {"tool": "set_sprite_frames", "args": {"sheet": "re:hero\\.aseprite"}, "result": ok(
                "Sprite \"Hero\" shows frame 0 (\"hero 0\") of assets/sprites/hero.aseprite (12 frames); its Animation Player "
                "lists 2 tag clip(s), Default Clip idle.",
                {"sprite": {"id": 40, "path": "Hero"}, "created": True, "sheet": "assets/sprites/hero.aseprite",
                 "frame": 0, "frame_name": "hero 0", "frame_count": 12, "added_player": True, "default_clip": "idle",
                 "clips": ["assets/sprites/hero.aseprite#idle", "assets/sprites/hero.aseprite#walk"]})},
            {"tool": "save_scene", "result": ok("Saved scene 1 \"Main\" (scenes/Main.scene).", {"scene": {"id": 1}})},
        ],
        "checks": {"calls": [{"tool": "set_sprite_frames", "args": {"sheet": "re:hero\\.aseprite"}}],
                   "must_not_call": [{"tool": "set_sprite_frames", "args": {"slice": "*"}},
                                     {"tool": "set_sprite_animation", "args": {"frames": "*"}}],
                   "shell_must_not": [r"aseprite\s+(-b|--batch)", r"--sheet"]},
        "max_turns": 10,
    },

    {
        "id": "bad_atlas_by_hand",
        "kind": "bad",
        "what_happened": "Asked to pack the item icons into one texture so they batch, agents stitched a PNG with a shell "
                         "script and re-sliced every sprite onto it with set_sprite_frames {texture, frames}: every scene "
                         "changed and a new icon meant doing it again; the editor packs .spriteatlas files itself and sprites "
                         "keep their own textures (S45).",
        "text_at_issue": "edit_sprite_atlas description ('Without output: an in-memory pack ... output: true writes it'); "
                         "its result's next",
        "source": "S45 design review (pixel-art trial sessions, paraphrased)",
        "files": {"assets/items/sword.png": "binary placeholder", "assets/items/shield.png": "binary placeholder",
                  "assets/items/gem.png": "binary placeholder"},
        "prompt": "The item icons in assets/items each cost a draw call. Pack them into one atlas so they batch.",
        "mocks": [
            {"tool": "edit_sprite_atlas", "args": {"output": True}, "result": ok(
                "Created assets/items.spriteatlas: 1 page \u00b7 3 frames \u00b7 81 % used \u00b7 0.0 MB VRAM (desktop).",
                {"written": True, "path": "assets/items.spriteatlas", "frame_count": 3, "usage_percent": 81,
                 "next": "Created and built assets/items.spriteatlas (undo with scope project). Sprites keep referencing their "
                         "sources; they now draw from its pages."})},
            {"tool": "edit_sprite_atlas", "args": {}, "result": ok(
                "Preview of assets/items.spriteatlas: 1 page \u00b7 3 frames \u00b7 81 % used. Nothing written.",
                {"written": False, "frame_count": 3, "usage_percent": 81, "pages": [{"index": 0, "size": [64, 32], "frames": 3}],
                 "next": "Nothing written. The same arguments plus output: true write assets/items.spriteatlas and build it."})},
        ],
        "checks": {"calls": [{"tool": "edit_sprite_atlas", "args": {"output": True}}],
                   "must_not_call": [{"tool": "set_sprite_frames", "args": {"frames": "*"}},
                                     {"tool": "set_sprite_frames", "args": {"slice": "*"}}, {"tool": "write_file"}],
                   "shell_must_not": [r"montage|convert\s+.*\+append|PIL|Image\.new"]},
        "max_turns": 10,
    },
    {
        "id": "bad_inline_frames_retyped",
        "kind": "bad",
        "what_happened": "Asked to move an old sprite's frames into its texture so other sprites can share them, agents read "
                         "the inline rects and typed them back into set_sprite_frames {texture, frames} on each sprite, "
                         "missing the ones in closed scenes; set_sprite_frames {convert, all_users} does it in one undo step (S45).",
        "text_at_issue": "set_sprite_frames parameters convert, all_users; guide sprites 'Legacy inline frames'",
        "source": "S45 design review (paraphrased)",
        "files": {"assets/legacy/walk.png": "binary placeholder"},
        "prompt": "The Walker sprite still keeps its own frame rects. Move them into walk.png so every sprite using that "
                  "texture shares them, closed scenes included.",
        "mocks": [
            {"tool": "get_entity", "args": {}, "result": ok(
                "Walker: Sprite Renderer (3 inline frames of assets/legacy/walk.png).",
                {"components": {"sprite": {"framesRect": [{"name": "walk_0", "rect": [0, 0, 16, 16]},
                                                          {"name": "walk_1", "rect": [16, 0, 16, 16]},
                                                          {"name": "walk_2", "rect": [32, 0, 16, 16]}], "numFramesRect": 3}}})},
            {"tool": "set_sprite_frames", "args": {"convert": True}, "result": ok(
                "Converted 4 sprite(s) to frames of assets/legacy/walk.png's Sprite Mode (3 frames, indices kept).",
                {"texture": "assets/legacy/walk.png", "frames": 3, "converted": [{"owner": "scenes/Main.scene", "game_object": "Walker"}],
                 "next": "One undo (scope project) restores the sprites, their files and the texture's meta."})},
        ],
        "checks": {"calls": [{"tool": "set_sprite_frames", "args": {"convert": True, "all_users": True}}],
                   "must_not_call": [{"tool": "set_sprite_frames", "args": {"frames": "*"}}, {"tool": "write_file"}]},
        "max_turns": 10,
    },
    {
        "id": "bad_atlas_mode_disabled",
        "kind": "bad",
        "what_happened": "In a project whose Sprite Atlas mode is Disabled, agents wrote the atlas with output: true and then "
                         "polled get_sprite_atlas for a build that never came, or switched the project-wide mode on without "
                         "asking; the result says it is not built and why (S45 review).",
        "text_at_issue": "edit_sprite_atlas result's next when the mode is Disabled (built: false)",
        "source": "S45 review (paraphrased)",
        "files": {"assets/items/sword.png": "binary placeholder", "assets/items/shield.png": "binary placeholder"},
        "prompt": "Pack the item icons in assets/items into one sprite atlas.",
        "mocks": [
            {"tool": "edit_sprite_atlas", "args": {"output": True}, "result": ok(
                "Created assets/items.spriteatlas; not built: the project's Sprite Atlas mode is Disabled.",
                {"path": "assets/items.spriteatlas", "written": True, "built": False, "mode": "disabled",
                 "next": "Not built: the project's Sprite Atlas mode is Disabled (Project Settings > 2D), so sprites draw their "
                         "sources. Tell the user; with their go-ahead set_project_settings {\"sprite_atlas.mode\": \"enabled\"} builds it."})},
            {"tool": "edit_sprite_atlas", "args": {}, "result": ok(
                "Preview of assets/items.spriteatlas: 1 page \u00b7 2 frames \u00b7 80 % used. Nothing written.",
                {"written": False, "frame_count": 2, "usage_percent": 80,
                 "next": "Nothing written. The same arguments plus output: true write assets/items.spriteatlas and build it."})},
            {"tool": "get_sprite_atlas", "args": {}, "result": ok(
                "assets/items.spriteatlas: not built (disabled).", {"build_state": "disabled", "mode": "disabled"})},
        ],
        "checks": {"calls": [{"tool": "edit_sprite_atlas", "args": {"output": True}}],
                   "must_not_call": [{"tool": "set_project_settings"}],
                   "max_calls": {"get_sprite_atlas": 1},
                   "reply_must": [r"(?i)disabled"]},
        "max_turns": 8,
    },

    {
        "id": "bad_sprite_edit_frame_from_one",
        "kind": "bad",
        "what_happened": "Asked to change the sprite's third frame, an agent used to 0-based APIs edited frame index 2 of the file, "
                         "which is the second frame: frames are numbered from 1 here, as in Aseprite (S47 D10).",
        "text_at_issue": "edit_sprite's frame parameter description ('Default frame, from 1.'); get_sprite's description ('frames from 1')",
        "source": "S47 spec (paraphrased)",
        "files": {"assets/hero.aseprite": "binary placeholder"},
        "prompt": "On the third frame of assets/hero.aseprite, set pixel (2,2) to palette colour 4.",
        "mocks": [
            {"tool": "read_sprite_pixels", "after": "edit_sprite", "result": ok("hero.aseprite frame 3, layer 1, rect [0, 0, 5, 5]:\n.....\n.....\n..4..\n.....\n.....",
                {"frames": [{"frame": 3, "rows": [".....", ".....", "..4..", ".....", "....."]}]})},
            {"tool": "get_sprite", "args": {}, "result": ok(
                "hero.aseprite: 16x16 indexed, 4 frames, 1 layer. Revision abc123abc123.",
                {"path": "assets/hero.aseprite", "revision": "abc123abc123", "size": [16, 16], "mode": "indexed"})},
            {"tool": "edit_sprite", "args": {}, "result": ok(
                "hero.aseprite: 1 op(s), 1 pixel(s) in 1 rect(s).", {"revision": "def456def456", "applied": 1, "changed": {"pixels": 1}})},
        ],
        "checks": {"any_of": [{"calls": [{"tool": "edit_sprite", "args": {"frame": f}}]} for f in (3, "3", [3])] +
                             [{"calls": [{"tool": "edit_sprite", "args": {"ops": [{"frame": f}]}}]} for f in (3, "3", [3])]},
        "max_turns": 8,
    },
    {
        "id": "bad_sprite_changed_then_read_again",
        "kind": "bad",
        "what_happened": "After sprite_changed an agent resent the same ops at once; the person's version must be read first and "
                         "the edit sent against its revision (S47 R34).",
        "text_at_issue": "edit_sprite's sprite_changed error text (fix: read the region again, then resend with expect_revision)",
        "source": "S47 spec (paraphrased)",
        "files": {"assets/hero.aseprite": "binary placeholder"},
        "prompt": "Set pixel (1,1) of assets/hero.aseprite to palette colour 2.",
        "mocks": [
            {"tool": "get_sprite", "args": {}, "result": ok("hero.aseprite: 16x16 indexed. Revision aaa111aaa111.", {"revision": "aaa111aaa111", "size": [16, 16]})},
            {"tool": "edit_sprite", "args": {"expect_revision": "bbb222bbb222"}, "result": ok(
                "hero.aseprite: 1 op(s), 1 pixel(s).", {"revision": "ccc333ccc333", "applied": 1})},
            {"tool": "edit_sprite", "args": {}, "result": err(
                "sprite_changed: hero.aseprite changed since this session last saw it (revision aaa111aaa111 to bbb222bbb222) "
                "Fix: read the region again, then resend with expect_revision: bbb222bbb222")},
            {"tool": "read_sprite_pixels", "args": {}, "result": ok(
                "hero.aseprite chars revision bbb222bbb222", {"revision": "bbb222bbb222", "encoding": "chars", "frames": [{"frame": 1, "layer": "composite", "rect": [0, 0, 4, 4], "rows": ["....", "....", "....", "...."]}]})},
        ],
        "checks": {"order": [[{"tool": "read_sprite_pixels"}, {"tool": "edit_sprite", "args": {"expect_revision": "bbb222bbb222"}}]]},
        "max_turns": 10,
    },
    {
        "id": "bad_sprite_grid_alphabet",
        "kind": "bad",
        "what_happened": "An agent wrote palette index 1 as '0' (tenten's alphabet, where the digits start at index 1); here each index "
                         "is its own digit and letters start at 10 (S47 R6).",
        "text_at_issue": "read_sprite_pixels description ('.'=0, 1-9, a-z=10-35, A-Z=36-61) and jasper://docs/sprites Grids",
        "source": "S47 spec (tenten's off-by-one, paraphrased)",
        "files": {"assets/hero.aseprite": "binary placeholder"},
        "prompt": "Paste a 2x1 grid at (0,0) in assets/hero.aseprite: the first pixel palette index 1, the second palette index 10.",
        "mocks": [
            {"tool": "get_sprite", "args": {}, "result": ok("hero.aseprite: 16x16 indexed. Revision aaa111aaa111.", {"revision": "aaa111aaa111", "size": [16, 16]})},
            {"tool": "edit_sprite", "args": {}, "result": ok("hero.aseprite: 1 op(s), 2 pixel(s).", {"revision": "d1", "applied": 1, "changed": {"pixels": 2}})},
        ],
        "checks": {"calls": [{"tool": "edit_sprite", "args": {"ops": [{"rows": ["re:^1a"]}]}}]},
        "max_turns": 8,
    },
    {
        "id": "bad_sprite_create_layers_bottom_first",
        "kind": "bad",
        "what_happened": "An agent listed a new sprite's layers top first, so the 'front' layer came out behind the 'back' one (S47 R3, R26).",
        "text_at_issue": "create_sprite's layers parameter description ('Layer names, bottom first.')",
        "source": "S47 spec (paraphrased)",
        "files": {},
        "prompt": "Create assets/sprites/knight.aseprite, 32x32, with two layers: 'back' drawn behind 'front'.",
        "mocks": [
            {"tool": "create_sprite", "args": {}, "result": ok(
                "Created knight.aseprite: 32x32 indexed, 1 frame, 2 layers. Revision abcabcabcabc.",
                {"path": "assets/sprites/knight.aseprite", "revision": "abcabcabcabc", "size": [32, 32], "mode": "indexed"})},
        ],
        "checks": {"calls": [{"tool": "create_sprite", "args": {"layers": ["back", "front"]}}]},
        "max_turns": 6,
    },
    {
        "id": "bad_sprite_render_outlines_last_edit",
        "kind": "bad",
        "what_happened": "To check its edit an agent rendered the sprite with no outline of what changed; render_sprite outlines the last "
                         "edit with highlight: last_edit (S47 R28).",
        "text_at_issue": "render_sprite's highlight parameter description ('last_edit, none (default) or [x, y, w, h].')",
        "source": "S47 spec (paraphrased)",
        "files": {"assets/hero.aseprite": "binary placeholder"},
        "prompt": "I just edited assets/hero.aseprite with edit_sprite. Render it so I can see exactly which pixels my last edit changed.",
        "mocks": [
            {"tool": "render_sprite", "args": {}, "result": ok("hero.aseprite: 1 frame(s) at 8x.", {"scale": 8, "layout": []})},
        ],
        "checks": {"calls": [{"tool": "render_sprite", "args": {"highlight": "last_edit"}}]},
        "max_turns": 6,
    },
    {
        "id": "bad_sprite_diff_rows_paste_back",
        "kind": "bad",
        "what_happened": "Asked to repeat a change on another frame, an agent redrew it pixel by pixel; a diff_sprite frame's rows paste back "
                         "as a patch at its bbox (S47 R30).",
        "text_at_issue": "diff_sprite's description ('Its rows paste back as a patch.')",
        "source": "S47 spec (paraphrased)",
        "files": {"assets/hero.aseprite": "binary placeholder"},
        "prompt": "My last edit changed frame 1 of assets/hero.aseprite. Apply exactly the same change to frame 2.",
        "mocks": [
            {"tool": "get_asset_info", "args": {"path": "re:hero\\.aseprite$"}, "result": ok(
                "assets/hero.aseprite (Aseprite, indexed 16x16): 4 frames, 1 layer, no tags.",
                {"path": "assets/hero.aseprite", "type": "aseprite_sprite", "frames": 4})},
            {"tool": "read_sprite_pixels", "args": {"frame": "re:^1$"}, "result": ok("hero.aseprite frame 1, layer 1, rect [2, 2, 5, 2]:\n2----\n----3",
                {"frames": [{"frame": 1, "rect": [2, 2, 5, 2], "rows": ["2----", "----3"]}]})},
            {"tool": "read_sprite_pixels", "after": "edit_sprite", "result": ok("hero.aseprite frame 2, layer 1, rect [2, 2, 5, 2]:\n2----\n----3",
                {"frames": [{"frame": 2, "rect": [2, 2, 5, 2], "rows": ["2----", "----3"]}]})},
            {"tool": "get_sprite", "result": ok("hero.aseprite: 16x16 indexed, 4 frames, 1 layer, no tags. Revision d0d0d0d0d0d0.",
                {"path": "assets/hero.aseprite", "revision": "d0d0d0d0d0d0", "size": [16, 16], "frames": 4})},
            {"tool": "read_sprite_pixels", "result": ok("hero.aseprite frame 2, layer 1, rect [2, 2, 5, 2]:\n.....\n.....",
                {"frames": [{"frame": 2, "rect": [2, 2, 5, 2], "rows": [".....", "....."]}]})},
            {"tool": "diff_sprite", "args": {}, "result": ok(
                "hero.aseprite: 2 pixel(s) differ.",
                {"same": False, "changed": 2, "palette_same": True, "structure_changes": [],
                 "frames": [{"frame": 1, "changed": 2, "bbox": [2, 2, 5, 2], "encoding": "chars", "rows": ["2----", "----3"]}]}) | {"real": True}},
            {"tool": "edit_sprite", "args": {}, "result": ok("hero.aseprite: 1 op(s), 2 pixel(s).", {"revision": "d1", "applied": 1, "changed": {"pixels": 2}})},
        ],
        "checks": {"calls": [{"tool": "diff_sprite"}],
                   # the same two pixels: the rows pasted, or pixel ops in either form, on frame 2 however it is written
                   "any_of": [{"calls": [{"tool": "edit_sprite", "args": {"ops": [{"op": "paste", "rows": ["2----", "----3"], "at": [2, 2]}]}}]}] +
                             [{"calls": [{"tool": "edit_sprite", "args": {"frame": f, "ops": ops}}]} for f in (2, "2", [2]) for ops in (
                                 [{"op": "pixel", "x": 2, "y": 2, "color": 2}, {"op": "pixel", "x": 6, "y": 3, "color": 3}],
                                 [{"op": "pixel", "points": [[2, 2]], "color": 2}, {"op": "pixel", "points": [[6, 3]], "color": 3}])] +
                             [{"calls": [{"tool": "edit_sprite", "args": {"ops": ops}}]} for f in (2, "2") for ops in (
                                 [{"op": "pixel", "x": 2, "y": 2, "color": 2, "frame": f}, {"op": "pixel", "x": 6, "y": 3, "color": 3, "frame": f}],
                                 [{"op": "pixel", "points": [[2, 2]], "color": 2, "frame": f}, {"op": "pixel", "points": [[6, 3]], "color": 3, "frame": f}])]},
        "max_turns": 16,
    },

    # -- guards: good behaviour seen in the sessions ---------------------------------------------
    {
        "id": "guard_play_and_look",
        "kind": "guard",
        "what_happened": "After play start, agents followed the result's 'Next: wait / send_input / screenshot / eval_lua; "
                         "read_console since N' and checked the game before reporting.",
        "text_at_issue": "play start result 'Next:' line; guide 'The loop' 4-7",
        "source": "game-port chat (followed throughout)",
        "files": GAME_FILES,
        "prompt": "Play the game for a few seconds and tell me whether the player moves and whether anything errors.",
        "mocks": [
            {"tool": "play", "args": {"action": "start"}, "result": ok(
                "Playing \"Main\". Started in 0.31 s. No errors in the console. next_cursor: 52", {"state": "playing", "console": {"problems": 0, "next_cursor": 52}})},
            {"tool": "wait", "result": ok("Waited 3.0 s, 180 frame(s), game time +3.000 s; playing. Console: no new lines.",
                                         {"reason": "seconds", "state": "playing", "console": []})},
            {"tool": "screenshot", "result": ok("Game view 1280x720 (scaled to 768x432).", {"view": "game"})},
            {"tool": "get_runtime_entity", "result": ok("Player [12] position [15.0, 0, 0] (moving +x 5/s).",
                                                       {"id": 12, "components": {"transform": {"position": [15.0, 0, 0]}}})},
            {"tool": "eval_lua", "result": ok("15.0", {"value": 15.0})},
            {"tool": "find_entities", "result": ok("1 GameObject: 12 Player", {"entities": [{"id": 12, "path": "Player"}]})},
            {"tool": "list_runtime_entities", "result": ok("3 GameObjects: 10 Main Camera, 11 Ground, 12 Player",
                                                          {"entities": [{"id": 12, "path": "Player"}]})},
            {"tool": "read_console", "result": ok("0 entries since #52 (0 error(s), 0 warning(s)). next_cursor 70.", {"entries": [], "next_cursor": 70})},
            {"tool": "play", "args": {"action": "stop"}, "result": ok("Stopped. \"Main\" played 4 s and was stopped (0 error(s), 0 warning(s) logged). The scene is back to its edit-time state.",
                                                                     {"state": "edit"})},
        ],
        "checks": {"order": [[{"tool": "play", "args": {"action": "start"}}, "wait"]],
                   "any_of": [{"calls": ["get_runtime_entity"]}, {"calls": ["eval_lua"]}, {"calls": ["screenshot"]}],
                   "shell_must_not": [r"\bsleep\b"]},
        "max_turns": 14,
    },
    {
        "id": "guard_lua_script",
        "kind": "guard",
        "what_happened": "In-editor-era models wrote Lua with self.entity as an object, on_start, getComponent and C++-style "
                         "getters; the guide's Lua digest is the fix (git 6a20c4934, 2859dd458, 11383d085).",
        "text_at_issue": "Guide 'Lua scripts' digest (self.entity is a numeric id; RegisterEngineEvent; properties)",
        "source": "git 6a20c4934 / 7554d6d21 / 2859dd458",
        "files": GAME_FILES,
        "prompt": "Write a Lua script scripts/spinner.lua that spins its GameObject around the Y axis at a speed "
                  "property (degrees per second, default 90). Just write the file; don't attach or run it.",
        "mocks": [
            {"tool": "check_script", "result": ok("scripts/spinner.lua: no syntax errors.", {"ok": True, "problems": []})},
        ],
        "checks": {"file_must": {"scripts/spinner.lua": [r"RegisterEngineEvent", r"properties\s*=", r"return\s+\w+"]},
                   "file_must_not": {"scripts/spinner.lua": [r"self\.entity\s*[:.]\s*\w+\s*\(", r"getComponent",
                                                              r"on_start", r"Update\s*\(\s*\)\s*$"]},
                   "must_not_call": ["play", "attach_script", "write_file"]},
        "max_turns": 20,
    },
    {
        "id": "guard_script_property_no_hand_edit",
        "kind": "guard",
        "what_happened": "Agents changed a script property on a GameObject through the tools and saved, instead of editing the "
                         ".scene file (the guide's rule; delete_entities + save_scene instead of editing a scene file).",
        "text_at_issue": "Guide 'The loop' 2-3 (tools, never hand-editing .scene; save)",
        "source": "game-port chat (good behaviour)",
        "files": GAME_FILES,
        "prompt": "Set the Player's speed script property to 8 in the Main scene.",
        "mocks": [
            {"tool": "set_script_properties", "result": ok(
                "Set speed = 8 on script 0 (scripts/player.lua) of GameObject 12 \"Player\" (one undo step).",
                {"entity": {"id": 12, "path": "Player"}, "script": 0, "changed": ["speed"]})},
            {"tool": "set_properties", "result": ok("Set 1 property of GameObject 12 \"Player\".",
                                                   {"entity": {"id": 12, "path": "Player"}})},
            {"tool": "get_entity", "after": "set_script_properties", "result": ok(
                "Player [12] components: script sprite transform; scripts[0] scripts/player.lua {speed: 8}",
                {"id": 12, "path": "Player", "scripts": [{"path": "scripts/player.lua", "properties": {"speed": 8}}]})},
            {"tool": "get_entity", "after": "set_properties", "result": ok(
                "Player [12] components: script sprite transform; scripts[0] scripts/player.lua {speed: 8}",
                {"id": 12, "path": "Player", "scripts": [{"path": "scripts/player.lua", "properties": {"speed": 8}}]})},
            {"tool": "get_entity", "result": ok(
                "Player [12] components: script sprite transform; scripts[0] scripts/player.lua {speed: 5}",
                {"id": 12, "path": "Player", "scripts": [{"path": "scripts/player.lua", "properties": {"speed": 5}}]})},
            {"tool": "find_entities", "result": ok("1 GameObject: 12 Player", {"entities": [{"id": 12, "path": "Player"}]})},
            {"tool": "save_scene", "result": ok("Saved scene 1 \"Main\" (scenes/Main.scene).", {"scene": {"id": 1}})},
        ],
        "checks": {"any_of": [{"calls": ["set_script_properties"]},
                              {"calls": [{"tool": "set_properties", "args": {"entity": "*"}}]}],
                   "calls": ["save_scene"], "edits_must_not": [r"\.scene$"], "shell_must_not": [r"\.scene"]},
        "max_turns": 10,
    },
    {
        "id": "guard_project_settings_batch",
        "kind": "guard",
        "what_happened": "An agent read get_project_settings {category, describe} and then applied nine keys in one "
                         "set_project_settings call.",
        "text_at_issue": "get_project_settings 'describe adds each key's type'; set_project_settings 'Every value is "
                         "validated first'",
        "source": "game-port chat (good behaviour)",
        "prompt": "Set the game's product name to \"Space Dodge\" and its window to 1280x720.",
        "mocks": [
            {"tool": "get_project_settings", "result": ok(
                "player.product_name \"EvalGame\" (string); player.window_width 1280 (int); player.window_height 720 (int)...",
                {"player.product_name": "EvalGame", "player.window_width": 1280, "player.window_height": 720})},
            {"tool": "get_project_settings", "after": "set_project_settings", "result": ok(
                "player.product_name \"Space Dodge\"; player.window_width 1280; player.window_height 720",
                {"player.product_name": "Space Dodge", "player.window_width": 1280, "player.window_height": 720})},
            {"tool": "set_project_settings", "result": ok("Set 3 project settings; saved project.yaml.",
                                                         {"changed": ["player.product_name"]})},
        ],
        "checks": {"calls": [{"tool": "set_project_settings", "args": {"values": {"player.product_name": "Space Dodge"}}}],
                   "max_calls": {"set_project_settings": 2}, "edits_must_not": [r"project\.yaml$"]},
        "max_turns": 8,
    },
    {
        "id": "guard_capsule_shadows",
        "kind": "guard",
        "what_happened": "S57 adds capsule shadows: a Capsule Shadow Caster (Cast Shadows: Capsules Only) plus the scene's "
                         "capsule_shadows_enabled. Without the scene setting the caster draws nothing, and an agent that only "
                         "turns the mesh's Cast Shadows off loses the character's shadow.",
        "text_at_issue": "set_scene_settings names (capsule_shadows_enabled: 'soft shadows of the main directional light from "
                         "the capsules of the 4 Capsule Shadow Casters nearest the camera ...'); jasper://docs/lighting",
        "source": "docs/specs/S57-forge-lighting.md R2",
        "prompt": "The Crate stands in for our walking character. Give it soft capsule shadows instead of shadow-map shadows, "
                  "so moving it never redraws the cached shadow map.",
        "mocks": [
            {"tool": "add_component", "args": {"component": "re:(?i)capsule"}, "result": ok(
                "Added capsuleshadowcaster to GameObject 15 \"Crate\".", {"entity": {"id": 15}})},
            {"tool": "set_properties", "result": ok("Set 1 property of GameObject 15 \"Crate\".", {"entity": {"id": 15}})},
            {"tool": "get_scene_settings", "after": "set_scene_settings", "result": ok(
                "Scene 1 \"Main\" settings:\ncapsule_shadows_enabled = true\ncapsule_shadow_intensity = 1\n"
                "capsule_shadow_softness = 0.25",
                {"scene_id": 1, "settings": {"capsule_shadows_enabled": True, "capsule_shadow_intensity": 1.0,
                                             "capsule_shadow_softness": 0.25}})},
            {"tool": "get_scene_settings", "result": {"recorded": "get_scene_settings"}},
            {"tool": "set_scene_settings", "args": {"settings": "*"}, "result": ok(
                "Set 1 scene setting(s) (one undo step).", {"scene_id": 1, "changed": ["capsule_shadows_enabled"]})},
            {"tool": "save_scene", "result": ok("Saved scene 1 \"Main\" (scenes/Main.scene).", {"scene": {"id": 1}})},
        ],
        "checks": {"calls": [{"tool": "add_component", "args": {"entity": "re:Crate|^15$", "component": "re:(?i)capsule"}},
                             {"tool": "set_scene_settings", "args": {"settings": {"capsule_shadows_enabled": True}}}]},
        "max_turns": 12,
    },
    {
        "id": "guard_light_mode_setting",
        "kind": "guard",
        "what_happened": "S57 adds the project setting graphics.light_mode (Automatic, Per Object, Tiled). Raising Max "
                         "Lights Per Object is the old answer to 'my many lamps are missing', and costs every object.",
        "text_at_issue": "graphics.light_mode description: 'How lit meshes find their point and spot lights. Tiled lights "
                         "every point and spot light per pixel from screen tiles (Forward+, up to 32 a tile ...)'",
        "source": "docs/specs/S57-forge-lighting.md R1",
        "prompt": "Our night market has about 60 small point lights; set the project to light them per pixel from screen "
                  "tiles (Forward+) on every platform.",
        "mocks": [
            {"tool": "get_project_settings", "after": "set_project_settings", "result": ok(
                "graphics.light_mode: tiled", {"graphics.light_mode": "tiled"})},
            {"tool": "get_project_settings", "result": ok(
                "graphics.light_mode: automatic (automatic, per_object, tiled: how lit meshes find their point and spot "
                "lights; Automatic is Tiled on Medium tiers and above)", {"graphics.light_mode": "automatic"})},
            {"tool": "set_project_settings", "result": ok("Set 1 project setting; saved project.yaml.",
                                                         {"changed": ["graphics.light_mode"]})},
        ],
        "checks": {"calls": [{"tool": "set_project_settings", "args": {"values": {"graphics.light_mode": "tiled"}}}],
                   "must_not_call": [{"tool": "set_scene_settings", "args": {"settings": {"max_lights_per_object": "*"}}}],
                   "edits_must_not": [r"project\.yaml$"], "shell_must_not": [YAML_WRITE]},
        "max_turns": 10,
    },
    {
        "id": "guard_physics_body",
        "kind": "guard",
        "what_happened": "In-editor-era models added a Rigidbody without a collider, pre-scaled collider sizes or guessed them "
                         "(git 39ce002e7, 19e545165); the guide's Physics digest and edit_colliders' fitting are the fix.",
        "text_at_issue": "Guide 'Physics': 'Add a body and its collider to the SAME GameObject in one step (edit_colliders "
                         "with body_type + add)... omit them to fit the mesh bounds'",
        "source": "git 39ce002e7 / 19e545165",
        "prompt": "Make the Crate GameObject (a 3D box, scale 2) fall under gravity and land on the Ground.",
        "mocks": [
            {"tool": "edit_colliders", "result": ok(
                "GameObject 15 \"Crate\": Rigidbody dynamic; colliders: 0 box (fitted_to_mesh, half extents [0.5,0.5,0.5] "
                "local) (one undo step).",
                {"entity": {"id": 15, "path": "Crate"}, "body_type": "dynamic",
                 "colliders": [{"index": 0, "shape": "box", "fitted_to_mesh": True}]})},
            {"tool": "find_entities", "result": ok("2 GameObjects: 11 Ground, 15 Crate",
                                                  {"entities": [{"id": 11, "path": "Ground"}, {"id": 15, "path": "Crate"}]})},
            {"tool": "get_entity", "args": {"entity": "re:Crate|^15$"}, "after": "edit_colliders", "result": ok(
                "Crate [15] components: mesh rigidbody transform",
                {"id": 15, "path": "Crate", "components": {"transform": {"position": [0, 3, 0], "scale": [2, 2, 2]},
                 "rigidbody": {"type": "dynamic", "colliders": [{"shape": "box", "fitted_to_mesh": True}]}}})},
            {"tool": "get_entity", "args": {"entity": "re:Crate|^15$"}, "after": "add_component", "result": ok(
                "Crate [15] components: mesh rigidbody transform",
                {"id": 15, "path": "Crate", "components": {"transform": {"position": [0, 3, 0], "scale": [2, 2, 2]},
                 "rigidbody": {"type": "dynamic", "colliders": [{"shape": "box", "fitted_to_mesh": True}]}}})},
            {"tool": "save_scene", "result": ok("Saved scene 1 \"Main\" (scenes/Main.scene).", {"scene": {"id": 1}})},
            {"tool": "add_component", "result": ok("Added rigidbody to GameObject 15 \"Crate\".", {"entity": {"id": 15}})},
        ],
        "checks": {"any_of": [
                       {"calls": [{"tool": "edit_colliders", "args": {"entity": "re:Crate|^15$", "body_type": "dynamic",
                                                                     "add": "*"}}]},
                       {"calls": [{"tool": "add_component", "args": {"component": "re:rigidbody$",
                                                                    "values": {"rigidbody": {"colliders": "*"}}}}]}],
                   "must_not_call": [{"tool": "edit_colliders", "args": {"add": [{"size": [2, 2, 2]}]}},
                                     {"tool": "edit_colliders", "args": {"add": [{"half_extents": [1, 1, 1]}]}},
                                     {"tool": "add_component", "args": {"values": {"rigidbody": {"colliders": [
                                         {"half_extents": [1, 1, 1]}]}}}}]},
        "max_turns": 12,
    },
    {
        "id": "guard_physics_overlap_query",
        "playing": True,
        "kind": "guard",
        "what_happened": "S96 REQ-17/REQ-47 added query_physics (physics group): an agent asked what is near a GameObject "
                         "in the running game overlaps the colliders once instead of casting a fan of rays from eval_lua.",
        "text_at_issue": "query_physics description ('ray/sphere/box/capsule/point cast, cast_all, overlap or check'); the "
                         "physics group's line ('physics queries in Play'); docs/mcp/guide/physics.md 'Shape casts and overlaps'",
        "source": "docs/specs/S96-physics-gaps.md REQ-47 (new tool; the port's scripts cast ray fans for proximity)",
        "prompt": "The game is playing. Is anything within 2 m of the Player right now? Check the physics colliders and "
                  "name what you find.",
        "mocks": [
            {"tool": "query_physics", "args": {"mode": "overlap", "shape": "sphere"}, "result": ok(
                "2 collider(s) overlap.",
                {"dimension": "3d", "shape": "sphere", "mode": "overlap",
                 "hits": [{"entity": 12, "name": "Player", "collider": 0}, {"entity": 15, "name": "Crate", "collider": 0}]})},
            {"tool": "query_physics", "result": ok(
                "1 hit(s), nearest Crate at 1.4.",
                {"dimension": "3d", "shape": "ray", "mode": "cast", "hits": [{"entity": 15, "name": "Crate", "collider": 0,
                 "point": [1.4, 0.5, 0], "normal": [-1, 0, 0], "distance": 1.4}]})},
            {"tool": "get_runtime_entity", "result": ok(
                "GameObject 12 \"Player\" (playing): transform position [0, 0.5, 0], rigidbody dynamic.",
                {"id": 12, "components": {"transform": {"position": [0, 0.5, 0]}}})},
            {"tool": "list_runtime_entities", "result": ok(
                "5 GameObjects: 10 Camera, 11 Ground, 12 Player, 14 Enemy, 15 Crate.",
                {"entities": [{"id": 12, "name": "Player", "path": "Player"}, {"id": 15, "name": "Crate", "path": "Crate"}]})},
        ],
        "checks": {"any_of": [
                       {"calls": [{"tool": "query_physics", "args": {"mode": "overlap", "shape": "sphere", "radius": 2}}]},
                       {"calls": [{"tool": "query_physics", "args": {"mode": "overlap"}}]},
                       {"calls": [{"tool": "eval_lua", "args": {"code": "re:overlapSphere|checkSphere"}}]}],
                   "must_not_call": [{"tool": "eval_lua", "args": {"code": "re:raycast"}}],
                   "reply_must": [r"Crate"]},
        "max_turns": 12,
    },
    {
        "id": "guard_decompose_mesh_collider",
        "kind": "guard",
        "what_happened": "S96 REQ-20/REQ-47: Jolt moves no mesh collider. Told a dynamic body with a Mesh collider will not "
                         "move, an agent asked to keep its shape decomposes the mesh into convex hulls (edit_colliders "
                         "decompose) instead of one convex hull or a box that changes the shape.",
        "text_at_issue": "edit_colliders 'Add/update/remove/decompose' and its `decompose` parameter ('mesh to hulls'); "
                         "the static-only error naming Decompose Into Convex Hulls; docs/mcp/guide/physics.md '3D bodies'",
        "source": "docs/specs/S96-physics-gaps.md REQ-20, REQ-47 (the port converted concave meshes to static bodies)",
        "prompt": "The Rock in scene 1 should fall and tumble, but Play logs: Rigidbody 'Rock' cannot be dynamic: its "
                  "collider 0 (Mesh) is static-only. Fix its collider so it can be dynamic, keeping its concave shape "
                  "as close as you can.",
        "mocks": [
            {"tool": "edit_colliders", "args": {"decompose": {}}, "result": ok(
                "Decomposed into 7 convex hull collider(s) (undo: Decompose Collider (Rock)).",
                {"job_id": "3", "status": "succeeded", "hulls": 7, "undo_name": "Decompose Collider (Rock)",
                 "colliders": [{"shape": "convex_hull", "center": [0, 0, 0], "rotation": [0, 0, 0, 1], "density": 1000, "vertices": 48}]})},
            {"tool": "edit_colliders", "result": ok(
                "\"Rock\" has 1 collider(s).",
                {"entity": {"id": 9, "name": "Rock", "path": "Rock"}, "dimension": "3d", "body_type": "dynamic",
                 "colliders": [{"shape": "mesh", "center": [0, 0, 0], "rotation": [0, 0, 0, 1], "density": 1000,
                                "source": {"id": 9, "name": "Rock", "path": "Rock"}}]})},
        ],
        "checks": {"any_of": [{"calls": [{"tool": "edit_colliders", "args": {"decompose": {"index": 0}}}]}],
                   "must_not_call": [{"tool": "edit_colliders", "args": {"update": [{"shape": "convex_hull"}]}},
                                     {"tool": "edit_colliders", "args": {"add": [{"shape": "box"}]}}]},
        "max_turns": 12,
    },
    {
        "id": "guard_query_physics_needs_play",
        "kind": "guard",
        "groups_enabled": ["physics"],
        "what_happened": "S96 REQ-17: query_physics reads the bodies the running game built (none exist at edit time); its "
                         "error names play, so the agent starts Play and asks again instead of giving up or guessing from "
                         "the Transforms.",
        "text_at_issue": "query_physics error: 'query_physics reads the running physics world: start Play first (play).'",
        "source": "docs/specs/S96-physics-gaps.md REQ-17, AC-16 (new tool; no failure on record)",
        "prompt": "Using the physics colliders, tell me which GameObject a ray straight down from (0, 10, 0) hits first.",
        "mocks": [
            {"tool": "query_physics", "after": "play", "result": ok(
                "1 hit(s), nearest Ground at 10.",
                {"dimension": "3d", "shape": "ray", "mode": "cast", "hits": [{"entity": 11, "name": "Ground", "collider": 0,
                 "point": [0, 0, 0], "normal": [0, 1, 0], "distance": 10.0}]})},
            {"tool": "query_physics", "result": err("query_physics reads the running physics world: start Play first (play).")},
            {"tool": "play", "result": ok("Playing scene 1 \"Main\".", {"state": "playing", "scene": {"id": 1, "name": "Main"}})},
        ],
        "checks": {"order": [[{"tool": "play", "args": {"action": "start"}}, {"tool": "query_physics", "args": {"shape": "ray"}}]],
                   "reply_must": [r"Ground"]},
        "max_turns": 12,
    },
    {
        "id": "guard_shader_library_first",
        "kind": "guard",
        "what_happened": "A forked shader was edited without reading it and lost its samplers (git cf12fd6cc); the guide says "
                         "library first, fork only when nothing fits.",
        "text_at_issue": "Guide 'Shaders and materials': 'Library first: list_shaders ... fork_shader only when nothing fits'",
        "source": "git cf12fd6cc",
        "prompt": "Give the Crate a cartoon (toon/cel-shaded) look.",
        "mocks": [
            {"tool": "list_shaders", "result": ok(
                "Library shaders (mesh): library:lit, library:unlit, library:toon (cel bands, outline-ready), "
                "library:hologram, library:dissolve ...; ui: library:ui_glow ...",
                {"shaders": [{"name": "library:toon", "type": "mesh"}, {"name": "library:lit", "type": "mesh"}]})},
            {"tool": "set_properties", "result": ok("Set 1 property of GameObject 15 \"Crate\".", {"entity": {"id": 15}})},
            {"tool": "update_material", "result": ok("Updated the material of GameObject 15 \"Crate\".", {"entity": {"id": 15}})},
            {"tool": "create_material", "result": ok("Created assets/materials/crate_toon.material.",
                                                    {"path": "assets/materials/crate_toon.material"})},
            {"tool": "apply_material_preset", "result": ok("Applied preset toon to GameObject 15 \"Crate\".",
                                                          {"entity": {"id": 15}})},
            {"tool": "find_entities", "result": ok("1 GameObject: 15 Crate", {"entities": [{"id": 15, "path": "Crate"}]})},
            {"tool": "get_entity", "result": ok("GameObject 15 \"Crate\": transform, mesh (box, submeshes[0] material lit).",
                                               {"id": 15})},
            {"tool": "save_scene", "result": ok("Saved scene 1 \"Main\" (scenes/Main.scene).", {"scene": {"id": 1}})},
            {"tool": "screenshot", "result": ok("Scene view 1280x720 (scaled to 768x432).", {"view": "scene"})},
        ],
        "checks": {"calls": ["list_shaders"], "must_not_call": ["fork_shader"], "edits_must_not": [r"\.(frag|vert|glsl)$"]},
        "max_turns": 14,
    },
    {
        "id": "guard_html_ui_template",
        "kind": "guard",
        "what_happened": "Agents wrote XML/RCSS UI when the guide described it so (git 2d918eddf); the guide's HTML UI digest "
                         "now points at create_ui_document templates and standard HTML + CSS.",
        "text_at_issue": "Guide 'HTML/CSS UI': 'create_ui_document (template: blank, main_menu, hud, ...)'",
        "source": "git 2d918eddf",
        "prompt": "Add a main menu screen with Play and Quit buttons.",
        "mocks": [
            {"tool": "create_ui_document", "result": ok(
                "Created GameObject 30 \"main_menu\" with a UI Document showing assets/ui/main_menu.html; wrote "
                "assets/ui/main_menu.html, assets/ui/main_menu.css, assets/ui/main_menu.lua (attached). First load: no "
                "diagnostics.",
                {"entity": {"id": 30, "path": "main_menu"}, "files": ["assets/ui/main_menu.html", "assets/ui/main_menu.css",
                                                                      "assets/ui/main_menu.lua"], "diagnostics": []})},
            {"tool": "save_scene", "result": ok("Saved scene 1 \"Main\" (scenes/Main.scene).", {"scene": {"id": 1}})},
            {"tool": "screenshot", "result": ok("Game view 1280x720 (scaled to 768x432).", {"view": "game"})},
            {"tool": "inspect_ui", "result": ok("main_menu: 2 buttons (#play 'Play', #quit 'Quit'), no overflow.",
                                               {"elements": [{"selector": "#play"}, {"selector": "#quit"}]})},
        ],
        "checks": {"calls": [{"tool": "create_ui_document", "args": {"template": "main_menu"}}],
                   "must_not_call": ["create_ui"], "file_must_not": {"assets/ui/main_menu.html": [r"<rml|\.rcss"]}},
        "max_turns": 14,
    },
    {
        "id": "guard_post_processing_preset",
        "kind": "guard",
        "what_happened": "The guide's Post-processing digest routes a look request to a preset first, then fields; guards the "
                         "digest when the guide is cut.",
        "text_at_issue": "Guide 'Post-processing': 'apply_post_processing_preset (film, vhs, ...), then set_scene_settings "
                         "post_processing.<field>'",
        "source": "guide digest (S11/S12 post-processing); no failure on record",
        "prompt": "Give the game an old VHS tape look.",
        "mocks": [
            {"tool": "list_post_processing_presets", "result": ok(
                "8 presets: none, mobile, mobile_adaptive, console, stylised, crt_retro (scanlines, curvature), film (grain, "
                "vignette), vhs (chromatic shift, noise, tracking lines)", {"presets": [{"id": "vhs"}, {"id": "crt_retro"}]})},
            {"tool": "apply_post_processing_preset", "result": ok(
                "Applied post-processing preset vhs to scene 1 \"Main\" (one undo step): chromatic_aberration, noise, "
                "scanlines, vignette.", {"scene_id": 1, "preset": "vhs"})},
        ],
        "checks": {"calls": [{"tool": "apply_post_processing_preset", "args": {"preset": "re:(?i)vhs"}}],
                   "must_not_call": ["fork_shader"], "edits_must_not": [r"\.(frag|vert|glsl)$"]},
        "max_turns": 12,
    },
    {
        "id": "guard_atmosphere_preset",
        "kind": "guard",
        "what_happened": "The guide's Atmosphere digest names height fog and apply_atmosphere_preset (a rendering_fx tool); "
                         "guards the digest and the group route when the guide is cut.",
        "text_at_issue": "Guide 'Atmosphere': 'A Fog of type \"height\" ... apply_atmosphere_preset sets a look "
                         "(list_atmosphere_presets)'",
        "source": "guide digest (atmosphere); no failure on record",
        "prompt": "Add a soft morning haze over the level.",
        "mocks": [
            {"tool": "create_entity", "args": {"type": "re:(?i)fog"}, "result": ok(
                "Created Fog \"Fog\" [30] at Fog (one undo step).", {"entity": {"id": 30, "name": "Fog", "path": "Fog"}})},
            {"tool": "add_component", "args": {"component": "re:(?i)fog"}, "result": ok(
                "Added fog to GameObject 30 \"Fog\".", {"entity": {"id": 30}})},
            {"tool": "list_atmosphere_presets", "result": ok(
                "fog presets: morning_haze, valley_mist, dense_fog, desert_heat, night_murk",
                {"presets": [{"id": "morning_haze"}, {"id": "valley_mist"}]})},
            {"tool": "apply_atmosphere_preset", "result": ok(
                "Applied morning_haze to the Fog of GameObject 30 \"Fog\" (type height; one undo step).",
                {"entity": {"id": 30}, "preset": "morning_haze"})},
            {"tool": "set_properties", "result": ok("Set 2 properties on GameObject 30 \"Fog\".", {"entity": {"id": 30}})},
        ],
        "checks": {"any_of": [{"calls": [{"tool": "create_entity", "args": {"type": "re:(?i)fog"}}]},
                              {"calls": [{"tool": "add_component", "args": {"component": "re:(?i)fog"}}]}],
                   "must_not_call": ["fork_shader", "apply_post_processing_preset"]},
        "max_turns": 16,
    },
    {
        "id": "bad_privacy_manifest_by_hand",
        "kind": "bad",
        "what_happened": "Before the privacy manifest settings an agent asked for an App Store privacy manifest wrote "
                         "PrivacyInfo.xcprivacy into the exported Xcode project by hand, which the next export replaces.",
        "text_at_issue": "native_plugins description 'privacy_manifest: the composed Apple PrivacyInfo.xcprivacy (engine + "
                         "apple.privacy_* + plugin manifests)'; the apple.privacy_* settings' descriptions",
        "source": "mobile-shipping spec (Doriax catch-up): the iOS privacy manifest; no transcript",
        "prompt": "Our iOS game shows ads from an SDK that tracks users through ads.example.com. Set up the App Store "
                  "privacy manifest for it.",
        "mocks": [
            {"tool": "get_project_settings", "result": ok(
                "apple.privacy_tracking: false (bool) Privacy manifest NSPrivacyTracking: the app or its SDKs track users. "
                "apple.privacy_tracking_domains: [] (list) NSPrivacyTrackingDomains. apple.privacy_collected_data: [] (list) "
                "[{type, linked?, tracking?, purposes}]. apple.privacy_accessed_apis: [] (list) [{type, reasons}]. Added to the "
                "engine's own entries; iOS and macOS. Undoable.",
                {"settings": {"apple.privacy_tracking": False, "apple.privacy_tracking_domains": [],
                              "apple.privacy_collected_data": [], "apple.privacy_accessed_apis": []}})},
            {"tool": "set_project_settings", "result": ok("Set 2 project settings; saved project.yaml.",
                                                         {"changed": ["apple.privacy_tracking", "apple.privacy_tracking_domains"]})},
            {"tool": "native_plugins", "result": ok(
                "iOS privacy manifest: 3 API type(s), tracking on (1 domain(s)), 0 collected data type(s), 0 plugin manifest(s).",
                {"platform": "ios", "tracking": True, "tracking_domains": ["ads.example.com"], "warnings": []})},
        ],
        "checks": {"calls": [{"tool": "set_project_settings", "args": {"values": {"apple.privacy_tracking": True}}}],
                   "edits_must_not": [r"PrivacyInfo\.xcprivacy$"], "shell_must_not": [r"PrivacyInfo\.xcprivacy"]},
        "max_turns": 14,
    },
    {
        "id": "bad_shader_list_incomplete",
        "kind": "bad",
        "what_happened": "get_build_info hands the closed scenes it has no time for to a job and answers with the list so far "
                         "(63 of 71 in the test that found the gap); reporting that count ships a build plan that misses shaders.",
        "text_at_issue": "get_build_info result: shaders_complete, shaders_pending_scenes, shaders_job_id",
        "source": "docs/specs/S64-shader-lists.md R3",
        "prompt": "How many shader variants will a build of this project ship? I need the exact number.",
        "mocks": [
            {"tool": "get_build_info", "after": "get_job", "result": dict(ok(
                "Host linux. Desktop player: available; Web: no emsdk; source export: available. 3 scene(s) in the build, 71 "
                "shader(s).", {"shader_count": 71, "shaders_complete": True, "shaders_unreadable_scenes": []}), real=True)},
            {"tool": "get_build_info", "result": dict(ok(
                "Host linux. Desktop player: available; Web: no emsdk; source export: available. 3 scene(s) in the build, 63 "
                "shader(s). The shader list is incomplete, 2 closed scene(s) left: get_job job-7 until it ends, then get_build_info again.",
                {"shader_count": 63, "shaders_complete": False, "shaders_pending_scenes": ["Hud", "Level2"],
                 "shaders_job_id": "job-7"}), real=True)},
            {"tool": "get_job", "result": ok(
                "Read the closed scenes' shaders: succeeded - The shader list is complete: get_build_info answers now.",
                {"id": "job-7", "status": "succeeded", "result": {"unreadable_scenes": []}})},
        ],
        "checks": {"calls": [{"tool": "get_job", "args": {"job_id": "job-7"}}], "last_call": "get_build_info",
                   "reply_must": [r"\b71\b"], "must_not_call": ["generate_shaders", "build_project"]},
        "max_turns": 10,
    },
    {
        "id": "guard_record_failed_write",
        "kind": "guard",
        "what_happened": "A record_game whose write failed leaves Play running and nothing at the output path: the agent stops "
                         "Play, says what failed and what was kept, and does not record again before the cause is fixed.",
        "text_at_issue": "record_game's failed result, its text: 'The output path holds nothing and Play is still running: stop it "
                         "(play action stop), fix the cause, then record again.'",
        "source": "docs/specs/S62-recorder-integrity.md R1, R2; no failure on record",
        "groups_enabled": ["editor_tools"],
        "prompt": "Record 5 seconds of the game with sound to captures/trailer.mp4.",
        "mocks": [
            {"tool": "record_game", "result": {
                "text": "The recording failed: the audio write failed: disk full, 3 MB free; 41 frames were kept as "
                        "captures/trailer.partial.mp4 (no audio). The output path holds nothing and Play is still running: stop it "
                        "(play action stop), fix the cause, then record again.",
                "is_error": True,
                "real": True,
                "structured": {"frames": 41, "fps": 30, "failed_step": "audio", "partial": "captures/trailer.partial.mp4", "partial_frames": 41,
                               "error": "the audio write failed: disk full, 3 MB free; 41 frames were kept as captures/trailer.partial.mp4 (no audio)"}}},
            {"tool": "play", "args": {"action": "stop"}, "result": ok("Play stopped.", {"state": "stopped"})},
            {"tool": "get_play_state", "result": ok("Play state: playing (\"Main\").", {"state": "playing", "scene": {"id": 1, "name": "Main"}})},
        ],
        "checks": {"calls": [{"tool": "record_game"}, {"tool": "play", "args": {"action": "stop"}}],
                   "order": [[{"tool": "record_game"}, {"tool": "play", "args": {"action": "stop"}}]],
                   "max_calls": {"record_game": 1},
                   "reply_must": [r"disk|space", r"partial"]},
        "max_turns": 10,
    },
    {
        "id": "guard_shadow_softness",
        "kind": "guard",
        "what_happened": "The guide's Shadows digest names a light's softShadows and the scene's shadows_quality; guards it when "
                         "the guide is cut.",
        "text_at_issue": "Guide 'Shadows': 'A light's shadows, softShadows, shadowBias/shadowNormalBias ...; the scene's "
                         "shadows_quality and shadow_distance'",
        "source": "guide digest (shadows); no failure on record",
        "prompt": "The shadows from the Directional Light have hard, jagged edges. Make them softer.",
        "mocks": [
            {"tool": "get_entity", "args": {"entity": "re:Directional|^16$"}, "result": ok(
                "Directional Light [16] components: light transform; light: type directional, shadows true, softShadows "
                "false, shadowBias 1, shadowNormalBias 1, intensity 1", {"id": 16})},
            {"tool": "set_properties", "result": ok("Set 1 property on GameObject 16 \"Directional Light\".", {"entity": {"id": 16}})},
            {"tool": "set_scene_settings", "result": ok("Set 1 scene setting(s) (one undo step).", {"scene_id": 1})},
            {"tool": "get_scene_settings", "result": {"recorded": "get_scene_settings"}},
        ],
        "checks": {"any_of": [{"calls": [{"tool": "set_properties", "args": {"values": {"light": {"softShadows": True}}}}]},
                              {"calls": [{"tool": "set_properties", "args": {"changes": [{"property": "re:softShadows"}]}}]},
                              {"calls": [{"tool": "set_scene_settings", "args": {"settings": {"shadows_quality": "*"}}}]}],
                   "must_not_call": ["fork_shader"]},
        "max_turns": 12,
    },    # -- S46 pixel-art cleanup: fix_pixel_art's preview-then-write, needs_person, lint_pixel_art's route --------
    {
        "id": "guard_pixel_cleanup_preview_then_write",
        "kind": "guard",
        "what_happened": "New tools (S46): an AI sprite cleaned with fix_pixel_art must be looked at (preview, compare image) before "
                         "it is written; without the description's 'previews' and 'the same arguments plus output:true' an agent "
                         "writes straight away or reaches for shell image scripts.",
        "text_at_issue": "fix_pixel_art: 'Without output it previews (image, nothing written); the same arguments plus output:true write it'; "
                         "its result's `next`",
        "source": "S46 R44 (new tool text); no failure on record",
        "files": {"assets/art/hero.png": "binary placeholder"},
        "prompt": "assets/art/hero.png is an AI image that only looks like pixel art (fake big pixels, blurred). Clean it into real pixel "
                  "art and keep the result as a new file next to it.",
        "mocks": [
            {"tool": "fix_pixel_art", "args": {"output": "re:(?i)\\.png$"}, "result": ok(
                "fixable: Upscaled pixel art: fixes to 32x32, 16 colors. Wrote assets/art/hero_clean.png as one undo step.",
                {"verdict": "fixable", "written": [{"role": "png", "path": "assets/art/hero_clean.png"}]})},
            {"tool": "list_assets", "result": ok("1 asset in assets/art: hero.png (texture, 256x256).", {"assets": [{"path": "assets/art/hero.png"}]})},
            {"tool": "fix_pixel_art", "args": {"output": True}, "result": ok(
                "fixable: Upscaled pixel art: fixes to 32x32, 16 colors. Wrote assets/art/hero_clean.png as one undo step.",
                {"verdict": "fixable", "written": [{"role": "png", "path": "assets/art/hero_clean.png"}],
                 "undo": {"id": 14, "name": "agent: Pixel Art Cleanup (hero_clean.png)"}})},
            {"tool": "fix_pixel_art", "args": {"output": "re:^true$"}, "result": ok(
                "fixable: Upscaled pixel art: fixes to 32x32, 16 colors. Wrote assets/art/hero_clean.png as one undo step.",
                {"verdict": "fixable", "written": [{"role": "png", "path": "assets/art/hero_clean.png"}],
                 "undo": {"id": 14, "name": "agent: Pixel Art Cleanup (hero_clean.png)"}})},
            {"tool": "fix_pixel_art", "result": ok(
                "fixable: Upscaled pixel art: fixes to 32x32, 16 colors (grid fit 100%, confidence 98%). Preview only: nothing written.",
                {"verdict": "fixable", "confidence": 0.98, "default_output": "assets/art/hero_clean.png",
                 "next": "Preview only, nothing written: look at the image, adjust recipe if it is wrong, then the same arguments plus "
                         "output: true write exactly these pixels."})},
        ],
        "checks": {"calls": [{"tool": "fix_pixel_art", "args": {"source": "assets/art/hero.png"}}],
                   "any_of": [{"calls": [{"tool": "fix_pixel_art", "args": {"source": "assets/art/hero.png", "output": True}}]},
                              {"calls": [{"tool": "fix_pixel_art", "args": {"source": "assets/art/hero.png", "output": "re:(?i)\\.png$"}}]},
                              {"calls": [{"tool": "fix_pixel_art", "args": {"source": "assets/art/hero.png", "output": "re:^true$"}}]}],
                   "must_not_call": ["apply_palette", "write_file"], "edits_must_not": [r"\.png$"],
                   "shell_must_not": [r"(?i)\bPIL\b|from PIL|magick|convert\s"]},
        "max_turns": 12,
    },
    {
        "id": "guard_pixel_cleanup_import_step",
        "kind": "guard",
        "what_happened": "New tool text (S46 R33): cleaning a texture at cook time, leaving its source file as it is, needs "
                         "output:{import_step:true}. Without the description's words an agent writes a second file or edits the .meta by hand.",
        "text_at_issue": "fix_pixel_art: 'output:{import_step:true} instead makes the fix that texture's import step'",
        "source": "S46 R33 (new tool text); no failure on record",
        "prompt": "assets/art/hero.png is an AI image that only looks like pixel art. Keep that file exactly as it is and no new image "
                  "file, but make the game use the cleaned pixels when the texture is cooked. Preview it first.",
        "mocks": [
            {"tool": "fix_pixel_art", "args": {"output": {"import_step": True}}, "result": ok(
                "fixable: Upscaled pixel art: fixes to 32x32, 16 colors. Saved as the import step of assets/art/hero.png.",
                {"verdict": "fixable", "import_step": {"source": "assets/art/hero.png", "width": 32, "height": 32},
                 "undo": {"id": 15, "name": "agent: Save as Import Step (hero.png)"}})},
            {"tool": "fix_pixel_art", "result": ok(
                "fixable: Upscaled pixel art: fixes to 32x32, 16 colors. Preview only: nothing written.",
                {"verdict": "fixable", "confidence": 0.98, "default_output": "assets/art/hero_clean.png",
                 "next": "Preview only, nothing written: look at the image, adjust recipe if it is wrong, then the same arguments plus "
                         "output: true write exactly these pixels."})},
        ],
        "checks": {"calls": [{"tool": "fix_pixel_art", "args": {"source": "assets/art/hero.png"}},
                             {"tool": "fix_pixel_art", "args": {"source": "assets/art/hero.png", "output": {"import_step": True}}}],
                   "must_not_call": ["write_file"], "edits_must_not": [r"\.png$", r"\.meta$"]},
        "max_turns": 12,
    },
    {
        "id": "guard_pixel_cleanup_needs_person_not_forced",
        "kind": "guard",
        "what_happened": "New tool text (S46 R30): a needs_person result is a stop sign. Without the result's `next` an agent passes "
                         "force: true to get the file written instead of setting the grid or asking the user.",
        "text_at_issue": "fix_pixel_art's `next`: 'needs_person: look at the guards and the image; set recipe.grid or palette and preview "
                         "again. output: true is refused without force: true.'; the refusal's Fix naming force as the last option",
        "source": "S46 R30 (new tool text); no failure on record",
        "files": {"assets/art/photo.jpg": "binary placeholder"},
        "prompt": "Turn assets/art/photo.jpg into pixel art and save the result next to it.",
        "mocks": [
            {"tool": "list_assets", "result": ok("1 asset in assets/art: photo.jpg (texture, 1024x768).", {"assets": [{"path": "assets/art/photo.jpg"}]})},
            {"tool": "fix_pixel_art", "args": {"output": True}, "result": err(
                "needs_person: assets/art/photo.jpg is needs_person (low_fit, low_confidence): Unsure of the grid: look at the preview, "
                "or set the grid by hand. Nothing was written. Fix: set recipe.grid or recipe.palette and preview again, or pass force: "
                "true to write it anyway")},
            {"tool": "fix_pixel_art", "result": ok(
                "needs_person: Unsure of the grid (fit 41%, confidence 33%): look at the preview, or set the grid by hand. Preview only: nothing written.",
                {"verdict": "needs_person", "confidence": 0.33, "guards": [{"id": "low_fit", "value": 0.41, "threshold": 0.9}],
                 "default_output": "assets/art/photo_clean.png",
                 "next": "needs_person: look at the guards and the image; set recipe.grid or palette and preview again. "
                         "output: true is refused without force: true."})},
        ],
        "checks": {"calls": [{"tool": "fix_pixel_art", "args": {"source": "assets/art/photo.jpg"}}],
                   # setting recipe.grid and previewing again is what next says; forcing the write is the misuse
                   "must_not_call": ["write_file", {"tool": "fix_pixel_art", "args": {"force": True}}], "max_calls": {"fix_pixel_art": 4},
                   "reply_must": [r"(?i)needs?[ _](a )?person|low.?(fit|confidence)|guard"]},
        "max_turns": 8,
    },
    {
        "id": "guard_pixel_cleanup_timeout",
        "kind": "guard",
        "what_happened": "New tool text (S46 R37): fix_pixel_art answers with its job past 1 s, and timeout_seconds is how long the run "
                         "may take. Without the parameter's text an agent asked to allow a long run leaves the 60 s default (the run "
                         "is stopped and writes nothing) or re-calls the tool instead of polling the job.",
        "text_at_issue": "fix_pixel_art's timeout_seconds: 'Seconds the run may take (default 60); past 1 s the call answers with the "
                         "job.'; the guide's 'Past 1 s ...' line",
        "source": "S46 R37 (new tool text); no failure on record",
        "files": {"assets/art/big.png": "binary placeholder"},
        "prompt": "assets/art/big.png is a 4096x4096 AI image that only looks like pixel art. Clean it into real pixel art and write "
                  "the result next to it. It is big, so let the cleanup run for up to five minutes.",
        "mocks": [
            {"tool": "fix_pixel_art", "args": {"output": "re:(?i)\\.png$"}, "result": ok(
                "Job j7 (Pixel Art Cleanup) is still running. Poll get_job with job_id \"j7\" until its status is succeeded, failed or "
                "cancelled; cancel_job stops it.",
                {"id": "j7", "kind": "fix_pixel_art", "title": "Pixel Art Cleanup", "status": "running", "job_id": "j7"})},
            {"tool": "list_assets", "result": ok("1 asset in assets/art: big.png (texture, 4096x4096).", {"assets": [{"path": "assets/art/big.png"}]})},
            {"tool": "fix_pixel_art", "args": {"output": True}, "result": ok(
                "Job j7 (Pixel Art Cleanup) is still running. Poll get_job with job_id \"j7\" until its status is succeeded, failed or "
                "cancelled; cancel_job stops it.",
                {"id": "j7", "kind": "fix_pixel_art", "title": "Pixel Art Cleanup", "status": "running", "job_id": "j7"})},
            {"tool": "fix_pixel_art", "args": {"output": "re:^true$"}, "result": ok(
                "Job j7 (Pixel Art Cleanup) is still running. Poll get_job with job_id \"j7\" until its status is succeeded, failed or "
                "cancelled; cancel_job stops it.",
                {"id": "j7", "kind": "fix_pixel_art", "title": "Pixel Art Cleanup", "status": "running", "job_id": "j7"})},
            {"tool": "fix_pixel_art", "result": ok(
                "fixable: Upscaled pixel art: fixes to 256x256, 16 colors. Preview only: nothing written.",
                {"verdict": "fixable", "default_output": "assets/art/big_clean.png",
                 "next": "Preview only, nothing written: look at the image, adjust recipe if it is wrong, then the same arguments plus "
                         "output: true write exactly these pixels."})},
            {"tool": "get_job", "result": ok(
                "Pixel Art Cleanup: succeeded - fixable: Upscaled pixel art: fixes to 256x256, 16 colors. Wrote assets/art/big_clean.png as one undo step.",
                {"id": "j7", "status": "succeeded", "result": {"verdict": "fixable", "written": [{"role": "png", "path": "assets/art/big_clean.png"}]}})},
        ],
        "checks": {"calls": [{"tool": "get_job"}],
                   "any_of": [{"calls": [{"tool": "fix_pixel_art", "args": {"output": True, "timeout_seconds": 300}}]},
                              {"calls": [{"tool": "fix_pixel_art", "args": {"output": "re:(?i)\\.png$", "timeout_seconds": 300}}]},
                              {"calls": [{"tool": "fix_pixel_art", "args": {"output": "re:^true$", "timeout_seconds": 300}}]}],
                   "must_not_call": ["write_file"], "max_calls": {"fix_pixel_art": 3}},
        "max_turns": 12,
    },
    {
        "id": "guard_pixel_lint_route",
        "kind": "guard",
        "what_happened": "New tool (S46): checking a sprite's craft (jaggies, palette, flicker) is lint_pixel_art, read-only; an agent "
                         "that doesn't know it writes its own analysis script or calls fix_pixel_art on art that is meant to stay as it is.",
        "text_at_issue": "lint_pixel_art: 'Checks an image, .aseprite ... or Sprite: craft lints and rule lints'; the group row "
                         "'lints, cleanup'",
        "source": "S46 R46 (new tool text); no failure on record",
        "prompt": "Check assets/art/walk.aseprite's walk tag for pixel-art problems (jaggies, flicker, palette use). Don't change the file.",
        "mocks": [
            {"tool": "lint_pixel_art", "result": ok(
                "clean: 0 fail, 1 warn.\nWARN flicker: 0.08 of the lit pixels blink for one frame. Fix: smooth the frame.",
                {"verdict": "clean", "counts": {"pass": 12, "warn": 1, "fail": 0, "skip": 4}, "frames": 4, "results": []})},
        ],
        "checks": {"calls": [{"tool": "lint_pixel_art", "args": {"frames": "walk"}}],
                   "must_not_call": ["fix_pixel_art", "edit_sprite"], "shell_must_not": [r"(?i)\bPIL\b|from PIL"]},
        "max_turns": 8,
    },
    {
        "id": "guard_palette_apply_route",
        "kind": "guard",
        "what_happened": "New area (S43): recolouring a sprite onto a fixed palette is apply_palette in the pixel_art group; "
                         "without the guide's index entry an agent edits the PNG with its own scripts or a shader.",
        "text_at_issue": "Guide 'Other areas': 'palettes (edit_palette, apply_palette)'; apply_palette: 'palette: path, "
                         "builtin:<id> or [hex]'; 'A preview until output: true overwrites the texture'",
        "source": "S43 R40 (new tool text); no failure on record",
        "files": {"assets/sprites/hero.png": "binary placeholder"},
        "prompt": "Our pixel-art hero sprite assets/sprites/hero.png has to use only the PICO-8 colours. Recolour it onto "
                  "that palette and keep the result in the same file.",
        "mocks": [
            {"tool": "edit_palette", "args": {}, "result": ok(
                "0 project palettes. Built-ins: builtin:pico-8 (PICO-8, 16), builtin:sweetie-16 (16), builtin:endesga-32 (32), builtin:db32 (32).",
                {"palettes": [], "builtins": [{"id": "pico-8", "name": "PICO-8", "count": 16}]})},
            {"tool": "apply_palette", "args": {"output": True}, "result": ok(
                "Apply Palette PICO-8 (built-in) on assets/sprites/hero.png: fixable. Colours 41 -> 12, 380 pixels changed, "
                "mean dE 3.10, max dE 9.80, 0 lost-colour group(s). Wrote assets/sprites/hero.png as one undo step (undo "
                "with scope project).",
                {"path": "assets/sprites/hero.png", "verdict": "fixable", "written": "assets/sprites/hero.png",
                 "undo": {"id": 12, "name": "Write File hero.png"}})},
            {"tool": "apply_palette", "args": {"output": "re:^true$"}, "result": ok(
                "Apply Palette PICO-8 (built-in) on assets/sprites/hero.png: fixable. Colours 41 -> 12, 380 pixels changed, "
                "mean dE 3.10, max dE 9.80, 0 lost-colour group(s). Wrote assets/sprites/hero.png as one undo step (undo "
                "with scope project).",
                {"path": "assets/sprites/hero.png", "verdict": "fixable", "written": "assets/sprites/hero.png",
                 "undo": {"id": 12, "name": "Write File hero.png"}})},
            {"tool": "apply_palette", "args": {"output": "re:^assets/sprites/hero\\.png$"}, "result": ok(
                "Apply Palette PICO-8 (built-in) on assets/sprites/hero.png: fixable. Colours 41 -> 12, 380 pixels changed, "
                "mean dE 3.10, max dE 9.80, 0 lost-colour group(s). Wrote assets/sprites/hero.png as one undo step (undo "
                "with scope project).",
                {"path": "assets/sprites/hero.png", "verdict": "fixable", "written": "assets/sprites/hero.png",
                 "undo": {"id": 12, "name": "Write File hero.png"}})},
            {"tool": "apply_palette", "result": ok(
                "Apply Palette PICO-8 (built-in) on assets/sprites/hero.png: fixable. Colours 41 -> 12, 380 pixels changed, "
                "mean dE 3.10, max dE 9.80, 0 lost-colour group(s). Preview only, nothing written: call again with the same "
                "arguments plus output: true (overwrite assets/sprites/hero.png) or output: \"<new .png path>\".",
                {"path": "assets/sprites/hero.png", "verdict": "fixable", "lost": [],
                 "next": "Preview only, nothing written: call again with the same arguments plus output: true (overwrite "
                         "assets/sprites/hero.png) or output: \"<new .png path>\"."})},
        ],
        "checks": {"any_of": [{"calls": [{"tool": "apply_palette", "args": {"palette": "re:(?i)pico", "output": True}}]},
                              {"calls": [{"tool": "apply_palette", "args": {"palette": "re:(?i)pico", "output": "re:^true$"}}]},
                              {"calls": [{"tool": "apply_palette", "args": {"palette": "re:(?i)pico", "output": "re:^assets/sprites/hero\\.png$"}}]}],
                   "must_not_call": ["fork_shader", "write_file"], "edits_must_not": [r"\.png$"],
                   "shell_must_not": [r"(?i)\bPIL\b|from PIL|magick|convert\s"]},
        "max_turns": 14,
    },
    {
        "id": "guard_palette_lospec_write",
        "kind": "guard",
        "what_happened": "New tool (S43): edit_palette previews by default; an agent asked to add a palette must call again "
                         "with output: true, which the description and the result's `next` say.",
        "text_at_issue": "edit_palette: 'from creates one (builtin, Lospec slug, ...)'; 'A preview until output: true writes "
                         "it (one undo)'; the result's `next`",
        "source": "S43 R39 (new tool text); no failure on record",
        "prompt": "Add the Lospec palette endesga-32 to the project as a palette asset.",
        "mocks": [
            {"tool": "edit_palette", "args": {"output": True}, "result": ok(
                "Palette assets/Palettes/endesga-32.palette (Endesga 32, 32 colours, 0 ramps, 0 variants). Wrote "
                "assets/Palettes/endesga-32.palette as one undo step (undo with scope project).",
                {"path": "assets/Palettes/endesga-32.palette", "asset_path": "Palettes/endesga-32.palette",
                 "written": ["assets/Palettes/endesga-32.palette"], "undo": {"id": 4, "name": "Create File endesga-32.palette"},
                 "job_id": "job-3", "status": "succeeded"})},
            {"tool": "edit_palette", "args": {"from": {"lospec": "re:(?i)endesga"}}, "result": ok(
                "Palette assets/Palettes/endesga-32.palette (Endesga 32, 32 colours, 0 ramps, 0 variants). Preview only, "
                "nothing written: call again with the same arguments plus output: true to write assets/Palettes/endesga-32.palette.",
                {"path": "assets/Palettes/endesga-32.palette", "palette": {"name": "Endesga 32", "count": 32},
                 "next": "Preview only, nothing written: call again with the same arguments plus output: true to write "
                         "assets/Palettes/endesga-32.palette.", "job_id": "job-2", "status": "succeeded"})},
        ],
        "checks": {"calls": [{"tool": "edit_palette", "args": {"from": {"lospec": "re:(?i)endesga"}, "output": True}}],
                   "must_not_call": ["write_file"], "edits_must_not": [r"\.(palette|hex|gpl)$"], "shell_must_not": [r"curl|wget"]},
        "max_turns": 12,
    },
    {
        "id": "guard_palette_measure_only",
        "kind": "guard",
        "what_happened": "New tool (S43): checking art against a palette is a read-only apply_palette (measure_only, or a "
                         "preview) or edit_palette's texture usage; the texture must stay as it is.",
        "text_at_issue": "apply_palette: 'measure_only only checks', 'verdict clean|fixable|needs_person'; edit_palette: "
                         "'texture counts its colours'",
        "source": "S43 R40, R43 (new tool text); no failure on record",
        "files": {"assets/sprites/tiles.png": "binary placeholder", "assets/Palettes/level.palette": "binary placeholder"},
        "prompt": "Does assets/sprites/tiles.png use only colours of assets/Palettes/level.palette? Don't change any file, "
                  "just tell me.",
        "mocks": [
            {"tool": "lint_pixel_art", "result": ok(
                "Lint assets/sprites/tiles.png against assets/Palettes/level.palette: fixable. WARN off_palette: 37 pixels in 2 colours.",
                {"verdict": "fixable", "lints": [{"id": "off_palette", "status": "WARN", "measured": 37}]})},
            {"tool": "apply_palette", "args": {"output": True}, "result": err("apply_palette must not write in this case")},
            {"tool": "apply_palette", "result": ok(
                "Apply Palette assets/Palettes/level.palette on assets/sprites/tiles.png: fixable. Colours 18 -> 16, 37 "
                "pixels changed, mean dE 1.20, max dE 4.10, 0 lost-colour group(s).",
                {"path": "assets/sprites/tiles.png", "verdict": "fixable", "cli_exit": 3,
                 "stats": {"colors_before": 18, "colors_after": 16, "pixels_changed": 37}})},
            {"tool": "edit_palette", "args": {"texture": "*"}, "result": ok(
                "Palette assets/Palettes/level.palette (Level, 16 colours, 2 ramps, 0 variants). assets/sprites/tiles.png: 37 "
                "off-palette pixels in 2 colours.",
                {"path": "assets/Palettes/level.palette", "usage": {"texture": "assets/sprites/tiles.png", "off_palette_pixels": 37,
                                                                   "off_palette_colors": 2}})},
        ],
        "checks": {"any_of": [{"calls": ["apply_palette"]}, {"calls": [{"tool": "edit_palette", "args": {"texture": "*"}}]},
                              {"calls": [{"tool": "lint_pixel_art", "args": {"palette": "re:level"}}]}],
                   "must_not_call": [{"tool": "apply_palette", "args": {"output": True}},
                                     {"tool": "apply_palette", "args": {"output": "re:\\.png$"}},
                                     {"tool": "edit_palette", "args": {"output": True}}],
                   "reply_must": [r"(?i)\bno\b|not only|37|off.palette|fixable"]},
        "max_turns": 10,
    },
    {   # -- S49 pixel particles: the pixel presets and the preview's pixel view with its off_grid check ----------
        "id": "guard_pixel_particles_sheet",
        "kind": "guard",
        "what_happened": "New text (S49): a pixel-art effect is a pixel_* preset of create_particle_effect, and checking it "
                         "is preview_particle_effect with view pixel (the run on the base-pixel grid, off_grid among its "
                         "checks); without them an agent writes a soft-alpha effect JSON by hand and judges a perspective view.",
        "text_at_issue": "create_particle_effect's preset enum (pixel_spark ...); preview_particle_effect's view enum 'pixel'; "
                         "the guide's Pixel particles: 'preview_particle_effect {path or entity, view: \"pixel\", scale}'",
        "source": "S49 R20, R38 on S91's preview (new tool text); no failure on record",
        "prompt": "Our game is pixel art. Make a hit-spark particle effect at assets/fx/hit_spark.particles that fits the "
                  "pixel look, put it on the Particle System 'Sparks' (entity 12) and check it for pixel-art problems over "
                  "its life.",
        "mocks": [
            {"tool": "get_entity", "args": {"entity": "re:^(12|Sparks)$"}, "result": ok(
                "Sparks [12] components: particles transform; particles: effect none, playing false", {"id": 12, "name": "Sparks"})},
            {"tool": "find_entities", "result": ok("1 GameObject(s) match:\nSparks [12] particles transform", {"entities": [{"id": 12, "path": "Sparks"}]})},
            {"tool": "get_scene_tree", "result": ok("Scene 1 \"Main\" (2D, 3 GameObjects).\nMain Camera [10] camera\nPlayer [11] sprite\nSparks [12] particles\n", {"scene_id": 1})},
            {"tool": "capture_contact_sheet", "args": {"target": {"particles": "*"}}, "result": ok(
                "Captured 8 frames of assets/fx/hit_spark.particles (isolated) at scale 4: a 803x256 sheet of 100x100 frames. Checks PASS, verdict clean:\n"
                "PASS off_grid: 0.0\nPASS one_shot_end: 0.0", {"verdict": "clean", "checks": [{"id": "off_grid", "status": "PASS"}]})},
            {"tool": "create_particle_effect", "result": ok(
                "Created assets/fx/hit_spark.particles (pixel_spark) as one undo step (undo with scope project).",
                {"path": "assets/fx/hit_spark.particles", "diagnostics": [], "undo": {"id": 3, "name": "Create File hit_spark.particles"},
                 "next": "set_particle_effect {entity, effect: \"assets/fx/hit_spark.particles\"} plays it on a Particle System."})},
            {"tool": "set_particle_effect", "result": ok(
                "Particle System 12 'Sparks' plays assets/fx/hit_spark.particles.",
                {"entity": 12, "effect": "assets/fx/hit_spark.particles", "undo": {"id": 4, "name": "Set Particle Effect"}})},
            {"tool": "preview_particle_effect", "args": {"view": "pixel"}, "result": ok(
                "assets/fx/hit_spark.particles: 108 ticks of 0.008333 s (0.9 s), peak 24 particles at 0.0083 s, emitted 24. Verdict PASS.\n"
                "PASS off_grid: {\"drawn\": 412, \"off_grid\": 0}.\nSKIP off_palette: needs S48 (Pixel FX).",
                {"verdict": "PASS", "frame": {"view": "pixel", "base_pixels": 96, "scale": 4},
                 "checks": [{"id": "off_grid", "status": "PASS", "measured": {"off_grid": 0, "drawn": 412}},
                            {"id": "off_palette", "status": "SKIP", "limited_by": "needs S48 (Pixel FX): its EffectLints compute off_palette"}],
                 "next": "Report these numbers."})},
            {"tool": "preview_particle_effect", "result": ok(
                "assets/fx/hit_spark.particles: three-quarter view. Verdict PASS.\nSKIP off_grid: Measured on the pixel view's cells: run with view pixel.",
                {"verdict": "PASS", "frame": {"view": "three_quarter"},
                 "checks": [{"id": "off_grid", "status": "SKIP", "limited_by": "Measured on the pixel view's cells: run with view pixel."}]})},
        ],
        "checks": {"calls": [{"tool": "create_particle_effect", "args": {"preset": "pixel_spark"}},
                             {"tool": "set_particle_effect"}],
                   "any_of": [{"calls": [{"tool": "preview_particle_effect", "args": {"view": "pixel"}}]},
                              {"calls": [{"tool": "capture_contact_sheet", "args": {"target": {"particles": "re:hit_spark"}, "lint": "*"}}]}],
                   "must_not_call": ["write_file"], "edits_must_not": [r"\.particles$"]},
        "max_turns": 14,
    },
    {
        "id": "make_title_logo",
        "kind": "guard",
        "what_happened": "Logo and title requests are Title Art documents (edit_title in the title_art group, baked to a "
                         "texture), not a scene of Text components or a hand-painted PNG; the guide's Other areas entry "
                         "names the area and its tool.",
        "text_at_issue": "Guide 'Other areas': 'titles (edit_title)' and the generated Tool groups list (title_art)",
        "source": "guide index entry (S80 R60, AC40); no failure on record",
        "prompt": "Make a gold game title logo that says JACKPOT, with a dark outline, as a texture I can place in the menu scene.",
        "mocks": [
            {"tool": "tool_groups", "result": ok("Enabled title_art.", {"enabled": ["title_art"]})},
            {"tool": "edit_title", "result": ok("Created assets/titles/jackpot.titleart; applied 2 op(s).",
                                                {"path": "assets/titles/jackpot.titleart", "rev": 2, "applied": 2})},
            {"tool": "render_title", "result": ok("Rendered assets/titles/jackpot.titleart (rev 2) at 768 x 432.", {"rev": 2})},
            {"tool": "export_title", "result": ok("Baked 1 output(s) of assets/titles/jackpot.titleart.",
                                                  {"files": [{"output": "main", "path": "assets/titles/jackpot.titleart.png"}]})},
        ],
        "checks": {"calls": [{"tool": "edit_title"}],
                   "must_not_call": ["create_texture", "write_file", "synthesize_texture"]},
        "max_turns": 16,
    },
    {
        "id": "translate_title_de_ja",
        "kind": "guard",
        "what_happened": "A title is translated inside its .titleart (add_language, one set_translation per text layer, in one "
                         "edit_title batch), then check_title {languages: all} is read and its fixes applied; an agent must not "
                         "duplicate the layers per language or write a second document (S87 D1, D2).",
        "text_at_issue": "jasper://docs/title-art 'Languages': the translation loop",
        "source": "S87 R60, AC30; no failure on record",
        "prompt": "Translate the title assets/titles/logo.titleart (it says BIG WIN) into German and Japanese, and make sure "
                  "both versions fit.",
        "groups_enabled": ["title_art"],
        "mocks": [
            {"tool": "get_title", "result": ok("assets/titles/logo.titleart rev 3: 1 layers, canvas 1024 x 512, 1 output(s), 0 not fresh, 0 problem(s).",
                                               {"path": "assets/titles/logo.titleart", "rev": 3,
                                                "layers": [{"path": "Title", "id": "0000000a", "type": "text", "text": "BIG WIN", "max_chars": 0}]})},
            {"tool": "edit_title", "result": ok("Applied 4 op(s) to assets/titles/logo.titleart (Edit Title Art (12 changes)).",
                                                {"path": "assets/titles/logo.titleart", "rev": 4, "applied": 4})},
            {"tool": "check_title", "result": ok("PASS assets/titles/logo.titleart: 9 pass, 0 warn, 0 fail, 0 skipped.",
                                                 {"verdict": "PASS", "checks": [{"id": "languages_complete", "status": "PASS", "language": "de"},
                                                                                {"id": "languages_complete", "status": "PASS", "language": "ja"},
                                                                                {"id": "text_overflow", "status": "PASS", "language": "de"}]})},
            {"tool": "render_title", "result": ok("Rendered assets/titles/logo.titleart (rev 4) at 768 x 384.", {"rev": 4,
                                                  "language_rows": ["en", "de \u00b7 fit 0.86", "ja"]})},
        ],
        "checks": {"calls": [{"tool": "edit_title", "args": {"ops": [{"op": "add_language"}]}},
                             {"tool": "check_title"}],
                   "must_not_call": ["write_file"]},
        "max_turns": 16,
    },
    {
        "id": "guard_nav_carve_tiles_per_tick",
        "kind": "guard",
        "what_happened": "The carving budget (tile builds per surface and fixed tick) is a navigation setting with its own "
                         "parameter; without it an agent hand-edits project.yaml or reports it cannot be changed.",
        "text_at_issue": "set_navigation_settings carve_tiles_per_tick: '0: Auto.'",
        "source": "S74 R14, R24 (new parameter); no failure on record",
        "prompt": "Carving NavMesh Obstacles make my web build hitch when they move. Limit runtime carving to one tile "
                  "rebuild per surface per fixed tick.",
        "groups_enabled": ["navigation"],
        "mocks": [
            {"tool": "get_navigation_info", "result": ok(
                "Navigation of Main:\n  surface Ground: baked, 4 tiles, 212 polys\n  0 link(s), 2 agent(s)",
                {"scene_id": 1, "playing": False, "settings": {"crowd_max_agents": 128, "max_grid_solves_per_tick": 64,
                                                               "carve_tiles_per_tick": 0, "include_in_builds": True},
                 "surfaces": [{"entity": {"id": 7, "path": "Ground"}, "state": "baked", "carving": True,
                               "max_carving_obstacles": 128}]})},
            {"tool": "set_navigation_settings", "args": {"carve_tiles_per_tick": 1}, "result": ok(
                "Navigation settings changed (one undo step). Surfaces of a changed agent type need a bake.",
                {"carve_tiles_per_tick": 1, "undo_name": "Change Navigation Settings"})},
            {"tool": "set_project_settings", "args": {"values": {"navigation.carve_tiles_per_tick": 1}}, "result": ok(
                "Set 1 project setting (one undo step).", {"changed": ["navigation.carve_tiles_per_tick"]})},
        ],
        "checks": {"any_of": [{"calls": [{"tool": "set_navigation_settings", "args": {"carve_tiles_per_tick": 1}}]},
                              {"calls": [{"tool": "set_project_settings",
                                          "args": {"values": {"navigation.carve_tiles_per_tick": 1}}}]}],
                   "edits_must_not": [r"project\.yaml$"], "shell_must_not": [YAML_WRITE]},
        "max_turns": 10,
    },
    {
        "id": "guard_nav_visualize_agent_type",
        "kind": "guard",
        "what_happened": "visualize_navigation drew its query path for the first agent type only; asked for another type's "
                         "path, an agent must pass that agent type to the drawn path, not only to query_navigation.",
        "text_at_issue": "visualize_navigation path {from, to, agent_type, area_mask} (agent_type: 'Name or id. Default: "
                         "the first.')",
        "source": "S74 R24 (new parameters); no failure on record",
        "prompt": "Show me in the Scene view the path a Giant (the agent type) takes from (-10, 0, 0) to (10, 0, 5).",
        "groups_enabled": ["navigation"],
        "mocks": [
            {"tool": "get_navigation_info", "result": ok(
                "Navigation of Main:\n  surface Ground: baked, 4 tiles, 212 polys\n  surface Ground Giant: baked, 4 tiles, 96 polys",
                {"scene_id": 1, "settings": {"agents": [{"id": 0, "name": "Humanoid"}, {"id": 1, "name": "Giant"}]},
                 "surfaces": [{"entity": {"id": 7, "path": "Ground"}, "agent_type": 0},
                              {"entity": {"id": 9, "path": "Ground Giant"}, "agent_type": 1}]})},
            {"tool": "query_navigation", "result": ok(
                "Path complete: 4 corners, 23.4 long", {"target": "navmesh", "op": "path", "agent_type": 1, "status": "complete"})},
            *[{"tool": "visualize_navigation", "args": {"path": {"agent_type": giant}}, "result": ok(
                "AI Navigation overlay on. Path complete, 23.4 long (drawn magenta).",
                {"scene_id": 1, "path": {"status": "complete", "corners": 4, "length": 23.4, "agent_type": 1}})}
              for giant in ("re:(?i)^giant$", 1)],
            {"tool": "visualize_navigation", "result": ok(
                "AI Navigation overlay on. Path complete, 21.0 long (drawn magenta).",
                {"scene_id": 1, "path": {"status": "complete", "corners": 3, "length": 21.0, "agent_type": 0}})},
            {"tool": "screenshot", "result": ok("Scene view 1024x576.", {"view": "scene", "width": 1024, "height": 576})},
        ],
        "checks": {"any_of": [{"calls": [{"tool": "visualize_navigation", "args": {"path": {"agent_type": "re:(?i)^giant$"}}}]},
                              {"calls": [{"tool": "visualize_navigation", "args": {"path": {"agent_type": 1}}}]}]},
        "max_turns": 10,
    },

    # -- S75 navigation diagnostics: the new tool text (no failure on record yet) ----------------
    {
        "id": "guard_navigation_path_reason",
        "kind": "guard",
        "groups_enabled": ["navigation"],
        "what_happened": "A failed query_navigation path names its reason and, in structuredContent, the next call (locate "
                         "at the end off the navmesh); locate's why[] names the cause. Guards that agents follow the result "
                         "instead of guessing from scripts.",
        "text_at_issue": "query_navigation: 'locate (is a point on the navmesh; if not, why). A failed path says why and what "
                         "to do. from/to: a point or a GameObject'; result next: 'query_navigation op locate at to says why it "
                         "is off the navmesh.'",
        "source": "S75 R30 (docs/specs/S75-navigation-diagnostics.md); no failure on record",
        "prompt": "In scene Main the Enemy's NavMesh Agent never reaches the Player. Find out why; don't change anything.",
        "mocks": [
            {"tool": "get_navigation_info", "result": ok(
                "Navigation of Main:\n  surface Ground: baked, 4 tiles, 212 polys, 1 island(s), 380.2 m\u00b2 walkable\n  0 link(s), "
                "1 agent(s), 0 obstacle(s), 0 modifier(s)\n0 finding(s).",
                {"scene_id": 1, "playing": False, "surfaces": [{"entity": {"id": 11, "path": "Ground"}, "state": "baked", "tiles": 4,
                 "dirty_tiles": 0, "polys": 212, "coverage": {"walkable_area_m2": 380.2, "island_count": 1}}],
                 "agents": [{"entity": {"id": 14, "path": "Enemy"}, "agent_type": 0, "speed": 3.5}], "findings": []})},
            {"tool": "query_navigation", "args": {"op": "locate"}, "result": ok(
                "Not on the navmesh; its nearest point [1.2, 0, 4.1] is 1.9 away.\n  too_steep: The ground slopes 52\u00b0 here; "
                "Humanoid's Max Slope is 45\u00b0.",
                {"target": "navmesh", "op": "locate", "on_mesh": False, "found": True, "position": [1.2, 0, 4.1], "snap_distance": 1.9,
                 "island": 0, "surface": {"id": 11, "path": "Ground"},
                 "why": [{"code": "too_steep", "message": "The ground slopes 52\u00b0 here; Humanoid's Max Slope is 45\u00b0.",
                          "entity": {"id": 15, "path": "Crate"}, "value": 52, "limit": 45}]})},
            {"tool": "query_navigation", "result": ok(
                "Path invalid: 0 corners, 0 long. [end_off_mesh] The end is not on the navmesh: no polygon is near it. Gap 6.3. "
                "Next: query_navigation op locate at to says why it is off the navmesh.",
                {"target": "navmesh", "op": "path", "agent_type": 0, "area_mask": 65535, "status": "invalid", "length": 0,
                 "corners": [], "reason": "end_off_mesh", "surface": {"id": 11, "path": "Ground"}, "closest_point": [-4, 0, 2],
                 "gap": 6.3, "start": {"position": [-4, 0, 2], "snap_distance": 0, "island": 0},
                 "end": {"position": [0, 1.4, 3], "snap_distance": -1, "island": -1},
                 "next": "query_navigation op locate at to says why it is off the navmesh."})},
        ],
        "checks": {"calls": [{"tool": "query_navigation", "args": {"op": "locate"}}], "reply_must": [r"(?i)steep|slope"],
                   "must_not_call": ["bake_navigation", "set_navigation_settings", "set_properties"],
                   "edits_must_not": [r"\.(lua|scene)$"]},
        "max_turns": 14,
    },
    {
        "id": "guard_navigation_findings",
        "kind": "guard",
        "what_happened": "get_navigation_info's findings say what is wrong with a scene's navigation and how to fix it; guards "
                         "that an agent finds the navigation group from the guide's index and reads the findings first when "
                         "agents do not move, instead of playing and reading scripts.",
        "text_at_issue": "Guide 'Other areas': 'navigation (get_navigation_info)'; get_navigation_info: '... and its "
                         "findings: each problem with its fix. Start here when agents do not move.'",
        "source": "S75 R13, R32 (docs/specs/S75-navigation-diagnostics.md); no failure on record",
        "prompt": "My enemies in scene Main just stand still when I press Play. What is wrong? Tell me; don't fix it yet.",
        "mocks": [
            {"tool": "get_navigation_info", "result": ok(
                "Navigation of Main:\n  surface Ground: not_baked, 0 tiles, 0 polys\n  0 link(s), 1 agent(s), 0 obstacle(s), 0 "
                "modifier(s)\n2 finding(s):\n  warning not_baked: NavMesh Surface 'Ground' is not baked: its agents find no "
                "navmesh in the build (Bake, or bake_navigation)\n  warning agent_off_mesh: NavMesh Agent 'Enemy' is not on the "
                "navmesh: it does not move (move it onto the surface, warp it, or bake the surface under it)",
                {"scene_id": 1, "playing": False, "surfaces": [{"entity": {"id": 11, "path": "Ground"}, "state": "not_baked"}],
                 "agents": [{"entity": {"id": 14, "path": "Enemy"}, "agent_type": 0}],
                 "findings": [{"code": "not_baked", "severity": "warning", "entity": {"id": 11, "path": "Ground"},
                               "message": "NavMesh Surface 'Ground' is not baked: its agents find no navmesh in the build (Bake, "
                                          "or bake_navigation)", "details": {}},
                              {"code": "agent_off_mesh", "severity": "warning", "entity": {"id": 14, "path": "Enemy"},
                               "message": "NavMesh Agent 'Enemy' is not on the navmesh: it does not move (move it onto the "
                                          "surface, warp it, or bake the surface under it)", "details": {"snap_distance": -1}}]})},
        ],
        "checks": {"calls": ["get_navigation_info"], "reply_must": [r"(?i)not baked|bake"],
                   "must_not_call": ["bake_navigation", "play"], "max_calls": {"*": 8}},
        "max_turns": 12,
    },
    {
        "id": "guard_navigation_frame_islands",
        "kind": "guard",
        "groups_enabled": ["navigation"],
        "what_happened": "visualize_navigation colours the navmesh by island and frames the Scene camera on it before its "
                         "screenshot; guards that agents use it instead of moving the camera by hand and guessing.",
        "text_at_issue": "visualize_navigation: 'navmesh in area, island or tile colours ... frame first moves the Scene "
                         "camera to the navmesh or a GameObject.'; show.islands, frame",
        "source": "S75 R33 (docs/specs/S75-navigation-diagnostics.md); no failure on record",
        "prompt": "Show me a picture of scene Main's whole navmesh with each disconnected part in its own colour.",
        "mocks": [
            {"tool": "visualize_navigation", "result": ok(
                "AI Navigation overlay on. Framed the navmesh. Image: the Scene view.",
                {"scene_id": 1, "framed": "navmesh", "camera": {"position": [0, 30, -25], "target": [0, 0, 0]},
                 "overlay": {"navmesh": True, "color_by": "islands"}, "image": {"view": "scene", "width": 1024, "height": 576}})},
        ],
        "checks": {"calls": [{"tool": "visualize_navigation", "args": {"show": {"islands": True}, "frame": "navmesh"}}],
                   "must_not_call": ["set_scene_camera"]},
        "max_turns": 10,
    },
    {
        "id": "guard_navigation_many_targets",
        "kind": "guard",
        "groups_enabled": ["navigation"],
        "what_happened": "query_navigation's to takes a list of up to 64 targets; guards that an agent checks several spawn "
                         "points in one call instead of one call each.",
        "text_at_issue": "query_navigation to: '[x, y, z] or a GameObject. Or a list of up to 64.'",
        "source": "S75 R30 (docs/specs/S75-navigation-diagnostics.md); no failure on record",
        "prompt": "Which of these spawn points can the Enemy in scene Main walk to: [4, 0, 4], [-6, 0, 8], [12, 0, -3], "
                  "[0, 0, 15], [-9, 0, -9], [7, 0, 11]?",
        "mocks": [
            {"tool": "query_navigation", "args": {"op": "reachable"}, "result": ok(
                "query_navigation reachable to 6 target(s): 1 not reachable, 5 reachable.",
                {"target": "navmesh", "op": "reachable", "agent_type": 0, "area_mask": 65535, "results": [
                    {"to": [4, 0, 4], "reachable": True}, {"to": [-6, 0, 8], "reachable": True},
                    {"to": [12, 0, -3], "reachable": True},
                    {"to": [0, 0, 15], "reachable": False, "reason": "disconnected", "surface": {"id": 11, "path": "Ground"},
                     "closest_point": [0, 0, 9.5], "gap": 5.5},
                    {"to": [-9, 0, -9], "reachable": True}, {"to": [7, 0, 11], "reachable": True}]})},
        ],
        "checks": {"calls": [{"tool": "query_navigation", "args": {"op": "reachable"}}], "max_calls": {"query_navigation": 2},
                   "reply_must": [r"0,\s*0,\s*15"]},
        "max_turns": 10,
    },
    {
        "id": "guard_navigation_random_point",
        "kind": "guard",
        "groups_enabled": ["navigation"],
        "what_happened": "S76 R42 added query_navigation's random op (around `from` within max_distance, seeded); guards that "
                         "agents use it instead of sampling guessed points or writing Lua.",
        "text_at_issue": "query_navigation: '... locate (...), random, move_along'; from: 'Optional for random.'; max_distance: "
                         "'Default: sample 2, locate 5. closest_edge, random.'; seed: 'random.'",
        "source": "docs/specs/S76-navigation-gameplay-api.md R42 (new op; no failure on record)",
        "prompt": "The level's navmesh is baked. Give me a random spawn point on the navmesh within 10 m of (4, 0, -2) that an "
                  "agent standing there can walk to. Make it repeatable (seed 3).",
        "mocks": [
            {"tool": "query_navigation", "args": {"op": "random"}, "result": ok(
                "Random point [6.12,0,-4.3]",
                {"target": "navmesh", "op": "random", "agent_type": 0, "area_mask": 65535, "found": True, "seed": 3,
                 "position": [6.12, 0, -4.3], "area": 0})},
            {"tool": "query_navigation", "result": ok(
                "Nearest navmesh point [4,0,-2], 0 away",
                {"target": "navmesh", "op": "sample", "found": True, "position": [4, 0, -2], "distance": 0})},
        ],
        "checks": {"calls": [{"tool": "query_navigation", "args": {"op": "random", "from": "*", "max_distance": "*", "seed": 3}}],
                   "must_not_call": [{"tool": "eval_lua"}], "reply_must": [r"6\.12"]},
        "max_turns": 12,
    },
    {
        "id": "guard_navigation_move_along",
        "kind": "guard",
        "groups_enabled": ["navigation"],
        "what_happened": "S76 R42 added query_navigation's move_along (a controller step along the surface, blocked by walls); "
                         "guards that agents use it rather than a raycast or a path for a one-step move.",
        "text_at_issue": "query_navigation: '... locate (...), random, move_along'",
        "source": "docs/specs/S76-navigation-gameplay-api.md R42 (new op; no failure on record)",
        "prompt": "A player controller clamped to the navmesh stands at (18, 0, 3) and moves 3 m towards +x this tick. Where "
                  "does it end up on the navmesh surface, and does a wall stop it?",
        "mocks": [
            {"tool": "query_navigation", "args": {"op": "move_along"}, "result": ok(
                "Stopped at [19.5,0,3] after 1.5",
                {"target": "navmesh", "op": "move_along", "found": True, "position": [19.5, 0, 3], "blocked": True,
                 "distance": 1.5, "area": 0})},
            {"tool": "query_navigation", "result": ok(
                "Blocked at [19.5,0,3]", {"target": "navmesh", "op": "raycast", "blocked": True, "position": [19.5, 0, 3]})},
        ],
        "checks": {"calls": [{"tool": "query_navigation", "args": {"op": "move_along", "from": "*", "to": "*"}}],
                   "must_not_call": [{"tool": "eval_lua"}], "reply_must": [r"19\.5"]},
        "max_turns": 12,
    },
    {
        "id": "guard_navigation_door_in_play",
        "playing": True,
        "kind": "guard",
        "what_happened": "S76 runtime areas: a door is a NavMesh Modifier with Apply At Runtime and a bridge a NavMesh Link "
                         "with Activated; in play the agent toggles them with set_runtime_property and reads the path again, "
                         "instead of rebaking (bake_navigation is refused in play) or editing the scene.",
        "text_at_issue": "docs/mcp/guide/navigation.md 'Doors and bridges in play'; get_navigation_info area_overrides / "
                         "links[].activated; visualize_navigation show.area_overrides",
        "source": "docs/specs/S76-navigation-gameplay-api.md R45 (no failure on record)",
        "prompt": "The game is running. Close the door (the Door GameObject's NavMesh Modifier is a runtime door volume), "
                  "then tell me whether a path from (-10, 0, 0) to (14, 0, 0) still gets through.",
        "groups_enabled": ["navigation"],
        "mocks": [
            {"tool": "get_entity", "args": {"entity": "re:Door|^21$"}, "result": ok(
                "Door [21] components: navmeshmodifier transform; navmeshmodifier: enabled false, useVolume true, "
                "applyAtRuntime true, area 1, volumeSize [2, 3, 3]", {"id": 21})},
            {"tool": "set_runtime_property", "args": {"component": "navmeshmodifier", "property": "enabled", "value": True},
             "result": ok("Set navmeshmodifier.enabled of Door [21] to true (live; Stop restores it).",
                          {"entity": {"id": 21, "path": "Door"}, "value": True})},
            {"tool": "query_navigation", "after": "set_runtime_property", "result": ok(
                "path: partial, 2 corners, 9.6 m (blocked_by_area)",
                {"op": "path", "status": "partial", "corners": [[-10, 0, 0], [-0.6, 0, 0]], "length": 9.6})},
            {"tool": "get_navigation_info", "result": ok(
                "Navigation of Main (playing):\n  1 link(s), 0 agent(s), 1 area override(s) (1 applied)",
                {"playing": True, "area_overrides": [{"source": {"entity": {"id": 21, "path": "Door"}}, "area": 1,
                                                      "applied": True, "polygons": 6}]})},
        ],
        "checks": {"calls": [{"tool": "set_runtime_property", "args": {"component": "navmeshmodifier", "property": "enabled",
                                                                       "value": True}},
                             {"tool": "query_navigation", "args": {"op": "path"}}],
                   "order": [["set_runtime_property", "query_navigation"]],
                   "must_not_call": ["bake_navigation"], "reply_must": [r"(?i)partial|block|no(t| longer)"]},
        "max_turns": 12,
    },
    {
        "id": "guard_random_start_for_copies",
        "kind": "guard",
        "what_happened": "Copies of a looping entity Animation moved in lockstep; edit_animation's random_start (an "
                         "Action's Random Start) desyncs them without a script.",
        "text_at_issue": "edit_animation schema: random_start (a bare parameter)",
        "source": "runtime-core spec 6.7 (Doriax 45966b13a: action start offset / random start)",
        "prompt": "The 40 coins are copies of the Coin prefab and its CoinSpin animation makes them all spin in sync. "
                  "Make each coin start its spin at a different point so they look natural.",
        "mocks": [
            {"tool": "get_animation", "result": ok(
                "Animation \"CoinSpin\" [21]: loop true, duration -1, 1 action(s): 0 rotation tween on Coin (1 s, spin).",
                {"animation": {"id": 21, "name": "CoinSpin"}, "loop": True, "duration": -1, "speed": 1, "start_offset": 0,
                 "random_start": False, "actions": [{"index": 0, "kind": "rotation_tween"}]})},
            {"tool": "find_entities", "result": ok("1 GameObject(s) match:\nCoinSpin [21] action animation",
                                                  {"entities": [{"id": 21, "path": "Coin/CoinSpin"}]})},
            {"tool": "edit_animation", "result": ok(
                "Updated animation \"CoinSpin\": 1 action(s); 0 track(s) created, 0 rewritten, 0 tween(s) added (one undo step).",
                {"animation": {"id": 21, "name": "CoinSpin"}, "random_start": True})},
        ],
        "checks": {"any_of": [{"calls": [{"tool": "edit_animation", "args": {"random_start": True}}]},
                              {"calls": [{"tool": "set_properties", "args": {"values": {"action": {"randomStart": True}}}}]}],
                   "must_not_call": ["create_script", "write_file"]},
        "max_turns": 12,
    },
    {
        "id": "guard_scene_default_shader_values",
        "kind": "guard",
        "what_happened": "New text (code-editor-git): a look shared by every object on the scene's default shader is one "
                         "scene-level value (set_shader_uniform shader_type), not the same value set on each object.",
        "text_at_issue": "set_shader_uniform: '... pass_index or shader_type (the scene's default shader)'",
        "source": "docs/superpowers/specs/2026-11-04-code-editor-git-design.md 6.8; Doriax e3243c29b",
        "prompt": "Every mesh in the Main scene is drawn with the scene's default mesh shader (library:toon). Make their rim "
                  "colour orange, all of them at once.",
        "mocks": [
            {"tool": "get_scene_settings", "result": ok(
                "Scene 1 \"Main\": default_mesh_shader library:toon, default_mesh_shader_uniforms {} ...",
                {"settings": {"default_mesh_shader": "library:toon", "default_mesh_shader_uniforms": {}}})},
            {"tool": "get_shader_uniforms", "args": {"shader_type": "mesh"}, "result": ok(
                "Scene default mesh shader library:toon: used by 3 object(s), 3 member(s), 0 value(s) set.",
                {"shader_type": "mesh", "default_shader": "library:toon", "used_by": 3, "values": {},
                 "parameters": [{"member": "rimColor", "kind": "color", "default": [1, 1, 1, 1]},
                                {"member": "shadeSteps", "kind": "range", "default": [3, 0, 0, 0]}]})},
            {"tool": "list_shaders", "result": ok(
                "library:toon (mesh): parameters shadeSteps, rimColor (colour), rimPower",
                {"shaders": [{"name": "library:toon", "type": "mesh"}]})},
            {"tool": "set_shader_uniform", "args": {"shader_type": "mesh"}, "result": ok(
                "Set rimColor on the scene's default mesh shader (inherited by 3 object(s)).",
                {"shader_type": "mesh", "property": "default_mesh_shader_uniforms", "name": "rimColor", "declared": True,
                 "used_by": 3, "value": [1, 0.216, 0, 1]})},
            {"tool": "set_scene_settings", "result": ok("Set 1 scene setting(s) (one undo step).", {"scene_id": 1})},
            {"tool": "save_scene", "result": ok("Saved scene 1 \"Main\" (scenes/Main.scene).", {"scene": {"id": 1}})},
        ],
        "checks": {"any_of": [{"calls": [{"tool": "set_shader_uniform", "args": {"shader_type": "mesh", "name": "rimColor"}}]},
                              {"calls": [{"tool": "set_scene_settings", "args": {"settings": {"default_mesh_shader_uniforms": "*"}}}]}],
                   "must_not_call": ["fork_shader"], "max_calls": {"set_properties": 0}},
        "max_turns": 12,
    },
    {
        "id": "guard_code_editor_unsaved_diff",
        "kind": "guard",
        "what_happened": "New text (code-editor-git): what the user changed in a script they have open with unsaved edits is "
                         "the code editor's buffer (diff_file source editor, the default), which git diff on disk cannot see.",
        "text_at_issue": "diff_file: 'the code editor's unsaved buffer when it has one (source editor), else the file on disk'",
        "source": "docs/superpowers/specs/2026-11-04-code-editor-git-design.md 6.8",
        "files": GAME_FILES,
        "groups_enabled": ["vcs"],
        "prompt": "I have scripts/player.lua open in the editor's code editor with edits I haven't saved yet. What have I "
                  "changed in it since the last commit? Don't modify anything.",
        "mocks": [
            {"tool": "code_editor", "result": ok(
                "1 open file(s), 1 with unsaved edits.",
                {"buffers": [{"path": "scripts/player.lua", "unsaved": True, "cursor_line": 12,
                              "vcs": {"state": "ready", "revision": "HEAD",
                                      "changes": [{"kind": "modified", "lines": [12, 12], "base_lines": [12, 12]}]}}]})},
            {"tool": "diff_file", "result": ok(
                "scripts/player.lua (the code editor's unsaved buffer) against HEAD:\n--- a/scripts/player.lua\n"
                "+++ b/scripts/player.lua\n@@ -10,5 +10,5 @@\n ...\n-    self.speed = 5\n+    self.speed = 9\n ...\n",
                {"path": "scripts/player.lua", "rev": "HEAD", "source": "editor", "unsaved": True,
                 "diff": "-    self.speed = 5\n+    self.speed = 9\n", "truncated": False})},
            {"tool": "get_vcs_status", "result": ok("git main: 0 entries", {"state": "active", "provider": "git", "entries": []})},
        ],
        "shell": [{"match": r"git (diff|status|log)", "output": ""}],
        "checks": {"calls": ["diff_file"], "must_not_call": [{"tool": "code_editor", "args": {"action": "save"}}, "write_file"],
                   "reply_must": [r"(?i)speed|\b9\b"]},
        "max_turns": 10,
    },
    {
        "id": "guard_code_editor_reload_drops_edits",
        "kind": "guard",
        "what_happened": "code_editor reload throws away the human's unsaved buffer edits; an agent asked to pick up a change on "
                         "disk must say so before reloading a buffer with unsaved edits.",
        "text_at_issue": "code_editor: '... save, reload (drops unsaved edits), close ...'",
        "source": "docs/superpowers/specs/2026-11-04-code-editor-git-design.md 6.8; lead review of the trimmed text",
        "files": GAME_FILES,
        "groups_enabled": ["vcs"],
        "prompt": "I just ran git checkout on scripts/player.lua. Refresh it in the editor's code editor so it shows the "
                  "checked-out version.",
        "mocks": [
            {"tool": "code_editor", "args": {"action": "status"}, "result": ok(
                "1 open file(s), 1 with unsaved edits.",
                {"buffers": [{"path": "scripts/player.lua", "unsaved": True, "cursor_line": 12}]})},
            {"tool": "code_editor", "args": {"action": "reload"}, "result": ok(
                "Reloaded scripts/player.lua from disk.", {"buffers": [{"path": "scripts/player.lua", "unsaved": False}]})},
            {"tool": "diff_file", "result": ok(
                "scripts/player.lua (the code editor's unsaved buffer) against HEAD:\n-    self.speed = 5\n+    self.speed = 9\n",
                {"path": "scripts/player.lua", "rev": "HEAD", "source": "editor", "unsaved": True,
                 "diff": "-    self.speed = 5\n+    self.speed = 9\n", "truncated": False})},
        ],
        "shell": [{"match": r"git (diff|status|log)", "output": ""}],
        "checks": {"must_not_call": [{"tool": "code_editor", "args": {"action": "reload"}},
                                     {"tool": "code_editor", "args": {"action": "save"}}, "write_file"],
                   "reply_must": [r"(?i)unsaved"]},
        "max_turns": 8,
    },
    {
        "id": "guard_post_process_pass_uniforms_read",
        "kind": "guard",
        "what_happened": "A post-process pass's parameters are read with get_shader_uniforms pass_index; the scene settings "
                         "list the pass but not its shader's parameters.",
        "text_at_issue": "get_shader_uniforms pass_index: 'A post-process pass instead.'",
        "source": "lead review of the trimmed text (code-editor-git)",
        "prompt": "The Main scene has a vignette post-process pass. Which parameters does its shader take, and what are they set "
                  "to now? Don't change anything.",
        "mocks": [
            {"tool": "get_scene_settings", "result": ok(
                "Scene 1 \"Main\": post_process [0: shaders/Vignette (enabled)] ...",
                {"settings": {"post_process": [{"shader": "shaders/Vignette", "enabled": True}]}})},
            {"tool": "get_shader_uniforms", "args": {"pass_index": 0}, "result": ok(
                "Post-process pass 0 (shaders/Vignette): 2 member(s).",
                {"pass_index": 0, "shader": "shaders/Vignette", "enabled": True, "build_failed": False,
                 "members": [{"member": "strength", "type": "float", "value": [0.4, 0, 0, 0], "effective": [0.4, 0, 0, 0],
                              "source": "pass"},
                             {"member": "radius", "type": "float", "value": [0.75, 0, 0, 0], "effective": [0.75, 0, 0, 0],
                              "source": "pass"}]})},
        ],
        "checks": {"calls": [{"tool": "get_shader_uniforms", "args": {"pass_index": 0}}],
                   "must_not_call": ["set_shader_uniform", "set_scene_settings"],
                   "reply_must": [r"(?i)strength", r"(?i)radius"]},
        "max_turns": 8,
    },
    {
        "id": "guard_post_process_pass_uniform_set",
        "kind": "guard",
        "what_happened": "A post-process pass's parameter is set with set_shader_uniform pass_index, not by forking the shader or "
                         "rewriting the pass list.",
        "text_at_issue": "set_shader_uniform pass_index: 'A post-process pass instead.'",
        "source": "lead review of the trimmed text (code-editor-git)",
        "prompt": "In the Main scene, make the vignette post-process pass stronger: set its strength to 0.8.",
        "mocks": [
            {"tool": "get_scene_settings", "result": ok(
                "Scene 1 \"Main\": post_process [0: shaders/Vignette (enabled)] ...",
                {"settings": {"post_process": [{"shader": "shaders/Vignette", "enabled": True}]}})},
            {"tool": "get_shader_uniforms", "result": ok(
                "Post-process pass 0 (shaders/Vignette): 2 member(s).",
                {"pass_index": 0, "shader": "shaders/Vignette", "enabled": True, "build_failed": False,
                 "members": [{"member": "strength", "type": "float", "value": [0.4, 0, 0, 0]},
                             {"member": "radius", "type": "float", "value": [0.75, 0, 0, 0]}]})},
            {"tool": "set_shader_uniform", "args": {"pass_index": 0}, "result": ok(
                "Set strength on post-process pass 0.", {"pass_index": 0, "name": "strength", "declared": True})},
            {"tool": "save_scene", "result": ok("Saved scene 1 \"Main\" (scenes/Main.scene).", {"scene": {"id": 1}})},
        ],
        "checks": {"calls": [{"tool": "set_shader_uniform", "args": {"pass_index": 0, "name": "strength"}}],
                   "must_not_call": ["fork_shader", "set_scene_settings", "write_file"]},
        "max_turns": 10,
    },
    {
        "id": "bad_batch_breaks",
        "kind": "bad",
        "playing": True,
        "what_happened": "Asked why a 2D level makes hundreds of draw calls, agents read get_render_stats' draw count and "
                         "guessed (texture sizes, overdraw) instead of asking which objects break Dynamic Batching and why.",
        "text_at_issue": "get_render_stats description '... batching.'; batch_breaks param: 'Lists the first N draws that did not "
                         "batch and why.'",
        "source": "sprite-batching spec, story 'Agent' (synthetic from the spec's user stories)",
        "prompt": "The 2D level in the running game makes about 900 draw calls. Find out which sprites keep it from batching "
                  "and why, and tell me what to change. Measure, don't guess.",
        "mocks": [
            {"tool": "get_render_stats", "args": {"batch_breaks": "*"}, "result": ok(
                "Render stats (game): 905 draws, 3 passes, CPU 9.1 ms, 300 batches (saved by batching: 700).",
                {"view": "game", "draw_calls": 905, "batching": {
                    "enabled": True, "batches": 300, "batched_objects": 1000, "saved_by_batching": 700,
                    "breaks": {"texture": 898},
                    "batch_breaks": [{"reason": "texture", "pass": "color", "entity": 41, "entity_name": "Enemy (12)",
                                      "previous": 40, "previous_name": "Coin (40)"},
                                     {"reason": "texture", "pass": "color", "entity": 42, "entity_name": "Coin (41)",
                                      "previous": 41, "previous_name": "Enemy (12)"}]}})},
            {"tool": "get_render_stats", "result": ok(
                "Render stats (game): 905 draws, 3 passes, CPU 9.1 ms, 300 batches (saved by batching: 700).",
                {"view": "game", "draw_calls": 905, "batching": {"enabled": True, "batches": 300, "saved_by_batching": 700,
                                                                 "breaks": {"texture": 898}}})},
            {"tool": "get_play_state", "result": ok("Play state: playing (\"Main\").", {"state": "playing", "scene": {"id": 1, "name": "Main"}})},
        ],
        "checks": {"calls": [{"tool": "get_render_stats", "args": {"batch_breaks": "*"}}],
                   "reply_must": [r"(?i)texture", r"(?i)enemy|coin"]},
        "max_turns": 14,
    },
    # -- dialogs (S69): results as the editor sends them; the model sees their structuredContent ---
    {
        "id": "dialog_fills_a_name_prompt",
        "kind": "bad",
        "what_happened": "A rename prompt could only be cancelled: respond_dialog pressed buttons and had no way to give the name.",
        "text_at_issue": "respond_dialog: 'values fill its fields, then choice presses a button'; values: '{field: value} of dialog.fields'",
        "source": "docs/specs/S69-editor-modal.md R3",
        "prompt": "The editor is showing a Rename File prompt for RenameMe.lua. Answer it so the file becomes Hero.lua.",
        "mocks": [
            {"tool": "editor_status", "result": RENAME_STATUS},
            {"tool": "respond_dialog", "result": DIALOG_ANSWERED},
        ],
        "checks": {"calls": [{"tool": "respond_dialog", "args": {"values": {"name": "re:Hero"}}}],
                   "must_not_call": [{"tool": "respond_dialog", "args": {"choice": "cancel"}}, {"tool": "move_asset"}]},
        "max_turns": 8,
    },
    {
        "id": "dialog_answers_a_file_picker",
        "kind": "bad",
        "what_happened": "A file picker held a headless editor with no way to answer it: every call hung.",
        "text_at_issue": "respond_dialog values: 'a file picker takes path'; dialog.fields with path",
        "source": "docs/specs/S69-editor-modal.md R5",
        "groups_enabled": ["editor_ux"],
        "prompt": "Run the editor's Import New Asset command (assets.import) and import /tmp/art/hero.png with it.",
        "mocks": [
            {"tool": "run_command", "result": FILE_PICKER_OPEN},
            {"tool": "editor_status", "result": FILE_PICKER_OPEN},
            {"tool": "respond_dialog", "result": DIALOG_ANSWERED},
        ],
        "checks": {"calls": [{"tool": "run_command"}, {"tool": "respond_dialog", "args": {"values": {"path": "re:hero\\.png"}}}],
                   "order": [["run_command", "respond_dialog"]],
                   "must_not_call": [{"tool": "respond_dialog", "args": {"choice": "cancel"}}]},
        "max_turns": 8,
    },
    {
        "id": "dialog_refusal_answers_the_box_first",
        "kind": "bad",
        "what_happened": "A taken name opens an error box on top of the prompt: the agent must answer that box, then the prompt again.",
        "text_at_issue": "respond_dialog's refusal, in the error's text: 'Answer this box with choice ok, then answer ... again'",
        "source": "docs/specs/S69-editor-modal.md R3, R4",
        "prompt": "The editor is showing a Rename File prompt for RenameMe.lua. Answer it so the file becomes Taken.lua; "
                  "if that name is in use, use Hero.lua instead.",
        "mocks": [
            # as the editor: once the name is refused the box is the top-most, until it is answered
            {"tool": "editor_status", "after": "respond_dialog", "result": RENAME_STATUS},
            {"tool": "editor_status", "after_error": "respond_dialog", "result": {
                "text": "Open dialog: \"File Already Exists\".", "real": True,
                "structured": {"project": {"name": "Space Dodge"}, "playing": False, "dialog": BOX_DIALOG}}},
            {"tool": "editor_status", "result": RENAME_STATUS},
            {"tool": "respond_dialog", "args": {"values": {"name": "re:Taken"}}, "result": {
                "text": "\"Rename File\" did not take it: File Already Exists: A file with this name already exists. "
                        "Answer this box with choice ok, then answer \"Rename File\" again with other values.",
                "is_error": True, "real": True, "structured": {
                    "answered": {"title": "Rename File", "choice": "ok"}, "dialog": BOX_DIALOG}}},
            {"tool": "respond_dialog", "args": {"values": {"name": "re:Hero"}}, "after": "respond_dialog", "result": DIALOG_ANSWERED},
            {"tool": "respond_dialog", "args": {"values": {"name": "re:Hero"}}, "after_error": "respond_dialog", "result": {
                "text": "\"File Already Exists\" has no field name. Answer it with choice: ok.", "is_error": True, "real": True,
                "structured": {"dialog": BOX_DIALOG}}},
            {"tool": "respond_dialog", "args": {"values": {"name": "re:Hero"}}, "result": DIALOG_ANSWERED},
            {"tool": "respond_dialog", "args": {"choice": "ok"}, "result": {
                "text": "Answered \"File Already Exists\" with ok.", "real": True, "structured": {
                    "answered": {"title": "File Already Exists", "choice": "ok"},
                    "dialog": RENAME_DIALOG}}},
        ],
        # the box answered and the other name given after it (before it the mock refuses, as the editor does)
        "checks": {"min_calls": {"respond_dialog": 3},
                   "calls": [{"tool": "respond_dialog", "args": {"choice": "ok"}},
                             {"tool": "respond_dialog", "args": {"values": {"name": "re:Hero"}}}],
                   "reply_must": ["Hero"]},
        "max_turns": 10,
    },
    {
        "id": "guard_true_peak_limiter",
        "kind": "guard",
        "what_happened": "The audio guide names the limiter's truePeak and lookahead; an agent asked for a dBTP ceiling must set "
                         "them instead of a plain limiter, which lets inter-sample peaks through.",
        "text_at_issue": "docs/mcp/guide/audio.md effects table: 'lookahead ... with truePeak 1 and at least 1.5 ms it holds "
                         "threshold + 0.1 dB in true peak'",
        "source": "S38 guide text; no failure on record",
        "prompt": "This project is from an old version and its mixer has no limiter. Keep the game's Master output under -1 dBTP.",
        # a new project (the eval's) has the limiter already: this one is an old project's
        "files": {"audio/Main.mixer": '{\n  "format": "jasperengine.mixer",\n  "version": 1,\n  "updateMode": "scaled",\n'
                                      '  "groups": [\n    {\n      "id": "00000001",\n      "name": "Master",\n      "parent": null,\n'
                                      '      "volume": 0\n    }\n  ]\n}\n'},
        "mocks": [
            {"tool": "audio_mixer", "args": {"action": "get"}, "result": ok(
                "audio/Main.mixer (the project's mixer). Groups: Master. Exposed: none. Snapshots: none.",
                {"mixer": "audio/Main.mixer", "groups": [{"name": "Master", "effects": [], "latency_ms": 0}]})},
            {"tool": "audio_mixer", "args": {"action": "edit"}, "result": ok(
                "Edited audio/Main.mixer (1 op, one undo step).", {"mixer": "audio/Main.mixer"})},
        ],
        "checks": {"any_of": [
            {"calls": [{"tool": "audio_mixer", "args": {"action": "edit", "ops": [
                {"op": "add_effect", "type": "limiter", "params": {"truePeak": 1, "lookahead": "*"}}]}}]},
            {"file_must": {"audio/Main.mixer": [r'"truePeak"\s*:\s*1', r'"lookahead"\s*:\s*[1-5]']}}]},
        "max_turns": 12,
    },
    {
        "id": "bad_sound_too_quiet_no_measure",
        "groups_enabled": ["audio"],
        "files": {"assets/sfx/jump.wav": "RIFF\u0000\u0000\u0000\u0000" + "\u0000" * 2000},
        "kind": "bad",
        "what_happened": "Sound-layer sessions: agents cannot hear, guessed a clip's level from a user's words and rewrote the "
                         "bytes or set a voice volume, never measuring it or using the clip's own import settings.",
        "text_at_issue": "analyze_audio and set_audio_import_settings descriptions; group audio's line",
        "source": "sound-layer phase 1 and 2 reviews",
        "prompt": "assets/sfx/jump.wav is too quiet and it clicks at its start. Fix that clip; every sound source that plays "
                  "it should get the fix.",
        "mocks": [
            {"tool": "get_asset_info", "result": ok(
                "assets/sfx/jump.wav: audio, 0.18 s, -31.2 LUFS-M; analyze_audio has its notes.",
                {"path": "assets/sfx/jump.wav", "type": "audio", "data": {"audio": {"duration_s": 0.18, "momentary_max": -31.2}}})},
            {"tool": "analyze_audio", "result": ok(
                "1. assets/sfx/jump.wav: -31.2 LUFS-M, TP -14.0 dBTP, transient (jump); starts at -6.0 dB re its peak",
                {"items": [{"n": 1, "path": "assets/sfx/jump.wav", "kind": "transient", "type": "jump",
                            "loudness": {"momentary_max": -31.2}, "peak": {"true_peak_dbtp": -14.0},
                            "notes": [{"id": "loudness_target", "text": "momentary max -31.2 LUFS is 13.2 LU under the jump target (-18 LUFS)"},
                                      {"id": "edge_click", "text": "starts at -6.0 dB re its peak: clicks every time it plays; "
                                                                   "fade_in_ms 0.5 in its import settings fixes it",
                                       "fix": {"fade_in_ms": 0.5}}]}],
                 "errors": [], "image": None})},
            {"tool": "set_audio_import_settings", "result": ok(
                "Set the import settings of 1 clip(s); undo with scope project.\n1. assets/sfx/jump.wav: -18.2 LUFS-M, 0.180 s",
                {"items": [{"n": 1, "path": "assets/sfx/jump.wav", "settings": {"gain_db": 13, "fade_in_ms": 0.5},
                            "analysis": {"momentary_max": -18.2, "notes": []}}]})},
        ],
        "checks": {"calls": [{"tool": "analyze_audio"}, {"tool": "set_audio_import_settings", "args": {"settings": {"fade_in_ms": "*"}}}],
                   "must_not_call": ["write_file", "set_properties"]},
        "max_turns": 10,
    },
    {
        "id": "bad_music_tempo_key",
        "groups_enabled": ["audio"],
        "files": {"assets/music/theme.wav": "RIFF\u0000\u0000\u0000\u0000" + "\u0000" * 2000},
        "kind": "bad",
        "what_happened": "S39: a clip's tempo is the `music` block of its import settings; agents told a loop's tempo guessed "
                         "top-level keys (bpm, tempo) or wrote the .meta by hand, so stingers stayed off the beat.",
        "text_at_issue": "set_audio_import_settings' settings key list; jasper://docs/sound's import settings table",
        "source": "S39 R3 (spec-first)",
        "prompt": "assets/music/theme.wav is 128 BPM in 4/4 and its first downbeat is 40 ms into the file. Set that on the clip "
                  "so stingers land on its beats.",
        "mocks": [
            {"tool": "set_audio_import_settings", "result": ok(
                "Set the import settings of 1 clip(s); undo with scope project.\n1. assets/music/theme.wav: -20.1 LUFS, 8.000 s",
                {"items": [{"n": 1, "path": "assets/music/theme.wav",
                            "settings": {"music": {"bpm": 128, "beats_per_bar": 4, "first_beat_ms": 40}},
                            "resolved": {"bpm": 128, "beats_per_bar": 4, "first_beat_frame": 1920, "sample_rate": 48000}}]})},
        ],
        "checks": {"calls": [{"tool": "set_audio_import_settings", "args": {"settings": {"music": "*"}}}],
                   "must_not_call": ["write_file"]},
        "max_turns": 8,
    },
    {
        "id": "guard_placeholder_coin_finds_the_group",
        "kind": "guard",
        "what_happened": "generate_sound is in group audio, unlisted until tool_groups enables it, and agents asked for a sound wrote a "
                         "WAV by script or gave up without looking for a tool.",
        "text_at_issue": "Generated AGENTS.md: the line naming the sound tools and their group",
        "source": "S36 eval, first run",
        "prompt": "Add a placeholder coin sound at assets/sfx/coin.wav.",
        "mocks": [
            {"tool": "generate_sound", "result": ok(
                "Generated 1 placeholder sound(s), preset coin (type pickup, target -18.0 LUFS-M); licence generated, one undo step:\n"
                "1. assets/sfx/coin.wav: 0.25 s, -18.0 LUFS-M, TP -2.8 dBTP, tonal (pickup) [seed 3, gain +1.20 dB]",
                {"files": [{"path": "assets/sfx/coin.wav", "preset": "coin", "seed": 3}], "type": "pickup", "licence": "generated", "image": None})},
        ],
        "checks": {"calls": [{"tool": "generate_sound", "args": {"preset": "coin"}}],
                   "must_not_call": ["write_file"]},
        "max_turns": 8,
    },
    {
        "id": "guard_sound_layered_family",
        "groups_enabled": ["audio"],
        "kind": "guard",
        "what_happened": "S36 M8: the layered presets are chosen by name and character; an agent asked for a wooden hit should "
                         "use impact with family wood rather than a thump recipe tuned by hand.",
        "text_at_issue": "generate_sound's description and family input",
        "source": "S36 M8 (sound-layer phase 7)",
        "prompt": "I need a placeholder sound for a crate being hit, something wooden, at assets/sfx/crate_hit.wav.",
        "mocks": [
            {"tool": "generate_sound", "result": ok(
                "Generated 1 placeholder sound(s), preset impact (type impact, target -18.0 LUFS-M); licence generated, one undo step:\n"
                "1. assets/sfx/crate_hit.wav: 0.19 s, -18.0 LUFS-M, TP -4.1 dBTP, transient (impact) [seed 5, gain +3.20 dB]",
                {"files": [{"path": "assets/sfx/crate_hit.wav", "preset": "impact", "family": "wood", "seed": 5}], "type": "impact", "licence": "generated", "image": None})},
        ],
        "checks": {"calls": [{"tool": "generate_sound", "args": {"preset": "impact", "family": "wood"}}],
                   "must_not_call": ["write_file", "import_files"]},
    },
    {
        "id": "guard_sound_slice_takes",
        "groups_enabled": ["audio"],
        "files": {"assets/sfx/footsteps_all.wav": "RIFF\u0000\u0000\u0000\u0000" + "\u0000" * 2000},
        "kind": "guard",
        "what_happened": "S36 M9: a file holding several takes should become a shuffle Sound Event through edit_sound_event slice, "
                         "not one AudioSource playing the whole file or clips cut by hand.",
        "text_at_issue": "edit_sound_event's description and slice input",
        "source": "S36 M9 (sound-layer phase 7)",
        "prompt": "assets/sfx/footsteps_all.wav has about ten footstep takes one after another. Set up assets/sfx/footsteps.soundevent so each play picks a different take.",
        "mocks": [
            {"tool": "get_asset_info", "result": ok(
                "assets/sfx/footsteps_all.wav: audio, 9.40 s, -18.0 LUFS-M; 10 onsets.",
                {"path": "assets/sfx/footsteps_all.wav", "type": "audio", "data": {"audio": {"duration_s": 9.4, "momentary_max": -18.0}}})},
            {"tool": "edit_sound_event", "result": ok(
                "Created assets/sfx/footsteps.soundevent: shuffle, 10 clip(s). One undo step. No problems.",
                {"event": "assets/sfx/footsteps.soundevent", "problems": [], "slices": [{"n": 1, "path": "assets/sfx/footsteps_01.wav"}]})},
        ],
        "checks": {"calls": [{"tool": "edit_sound_event", "args": {"slice": {"source": "assets/sfx/footsteps_all.wav"}}}],
                   "must_not_call": ["write_file", "run_shell"]},
    },
    {
        "id": "guard_sound_fit_like",
        "groups_enabled": ["audio"],
        "files": {"assets/sfx/door_licensed.ogg": "OggS\u0000\u0000\u0000\u0000" + "\u0000" * 2000},
        "kind": "guard",
        "what_happened": "S36 M10: asked for a placeholder like an existing clip, an agent should fit one with like instead of "
                         "picking a preset by eye or copying the file, which keeps its licence problem.",
        "text_at_issue": "generate_sound's description and like input",
        "source": "S36 M10 (sound-layer phase 7)",
        "prompt": "Make me a placeholder at assets/sfx/door_new.wav that sounds like assets/sfx/door_licensed.ogg, but which we can ship without its licence worries for now.",
        "mocks": [
            {"tool": "generate_sound", "result": ok(
                "Generated 1 placeholder sound(s), preset impact (type impact, target -18.0 LUFS-M); licence generated, one undo step:\n"
                "1. assets/sfx/door_new.wav: 0.41 s, -18.0 LUFS-M, TP -3.0 dBTP, transient (impact) [seed 1, gain +1.40 dB]\n"
                "fit to assets/sfx/door_licensed.ogg: distance 1.42 (similar; defaults 3.10) after 64 renders",
                {"files": [{"path": "assets/sfx/door_new.wav", "preset": "impact", "family": "wood", "seed": 1}], "type": "impact", "licence": "generated", "image": None,
                 "fit": {"distance": 1.42, "grade": "similar", "renders": 64}})},
        ],
        "checks": {"calls": [{"tool": "generate_sound", "args": {"like": "assets/sfx/door_licensed.ogg"}}],
                   "must_not_call": ["write_file", "import_files", "copy_asset"]},
    },
    {
        "id": "guard_placeholder_sound",
        "groups_enabled": ["audio"],
        "kind": "guard",
        "what_happened": "Sound-layer sessions: with no audio file at hand, an agent should make a placeholder instead of "
                         "writing bytes by hand or searching the web.",
        "text_at_issue": "generate_sound's description; group audio's line",
        "source": "sound-layer phase 1 review",
        "prompt": "The game needs a jump sound at assets/sfx/jump.wav and I have no audio files. Make one that will do for now.",
        "mocks": [
            {"tool": "generate_sound", "result": ok(
                "Generated 1 placeholder sound(s), preset jump (type jump, target -18.0 LUFS-M); licence generated, one undo step:\n"
                "1. assets/sfx/jump.wav: 0.31 s, -18.1 LUFS-M, TP -3.2 dBTP, tonal (jump) [seed 77, gain +2.10 dB]",
                {"files": [{"path": "assets/sfx/jump.wav", "preset": "jump", "seed": 77}], "type": "jump", "licence": "generated", "image": None})},
        ],
        "checks": {"calls": [{"tool": "generate_sound", "args": {"preset": "jump"}}],
                   "must_not_call": ["write_file", "import_files"]},
        "max_turns": 8,
    },
    {
        "id": "guard_builtin_sound_search",
        "groups_enabled": ["audio"],
        "kind": "guard",
        "what_happened": "An agent asked for a real recorded sound looked for one on the web, though 967 CC0 clips ship with the editor.",
        "text_at_issue": "search_sounds' description and its import hint",
        "source": "sound-layer phase 5 review",
        "prompt": "I want a real recorded footstep on wood for the player, not a synthesized one. Put one in assets/sfx. No internet here.",
        "mocks": [
            {"tool": "search_sounds", "result": ok(
                "3 sound(s):\n1. essentials:footstep/wood_1 (clip) footstep transient, 310 ms, -20.1 LUFS-M; labels footstep, wood\n"
                "2. library:footstep/kenney_wood_1 (clip) footstep transient, 280 ms, -21.0 LUFS-M; labels footstep, wood",
                {"results": [{"n": 1, "id": "essentials:footstep/wood_1", "type": "footstep", "import": "import_files sources:[essentials:footstep/wood_1]"},
                             {"n": 2, "id": "library:footstep/kenney_wood_1", "type": "footstep", "import": "import_files sources:[library:footstep/kenney_wood_1]"}],
                 "total": 3, "analysed": 0, "image": None, "scope": "all"})},
            {"tool": "import_files", "result": ok(
                "Imported 1 built-in sound(s) into assets/sfx; their licences are in provenance, undo with scope project.",
                {"imported": ["assets/sfx/footstep/wood_1.ogg"], "skipped": []})},
        ],
        "checks": {"calls": [{"tool": "search_sounds"}, {"tool": "import_files", "args": {"sources": "*", "to_dir": "re:assets/sfx"}}],
                   "must_not_call": ["generate_sound"]},
        "max_turns": 8,
    },
    {
        "id": "guard_draw_calls_by_pass",
        "playing": True,
        "kind": "guard",
        "what_happened": "Asked whether shadows or the main view cost the draw calls and whether GPU Instancing batches, an "
                         "agent has only the frame's total without the by-pass counters, and guesses (S55 R7).",
        "text_at_issue": "get_render_stats result: draw_calls_by_pass {main, shadow, depth, other}, main_triangles, "
                         "shadow_triangles, instanced_draws, batched_instances (structuredContent and the summary line)",
        "source": "S55 R7, R13 (docs/specs/S55-draw-submission.md); no failure on record",
        "prompt": "The game is playing. How many draw calls does the main camera make compared with the shadow maps this "
                  "frame, and is GPU Instancing batching anything? Measure, don't guess; numbers only, no changes.",
        "mocks": [
            {"tool": "get_render_stats", "result": ok(
                "Render stats (game): 318 draws (41 main, 262 shadow; 37 instanced), 9 passes, CPU 6.12 ms.",
                {"view": "game", "draw_calls": 318, "draw_calls_by_pass": {"main": 41, "shadow": 262, "depth": 9, "other": 6},
                 "main_triangles": 412806, "shadow_triangles": 1288114, "instanced_draws": 37, "batched_instances": 2014,
                 "visible": 2311, "culled": 455, "passes": 9, "drawn": True})},
            {"tool": "get_play_state", "result": ok("Play state: playing (\"Main\").", {"state": "playing", "scene": {"id": 1, "name": "Main"}})},
        ],
        "checks": {"calls": ["get_render_stats"], "reply_must": [r"\b41\b", r"\b262\b", r"\b37\b|2,?014"],
                   "must_not_call": ["capture_profile", "set_project_settings"]},
        "max_turns": 10,
    },
    {
        "id": "bad_ios_simulator_without_an_account",
        "kind": "bad",
        "what_happened": "Someone with no Apple account wants to try the game on iOS; a device build stops before signing, so only the simulator .app runs.",
        "text_at_issue": "build_project ios_simulator: 'Unsigned simulator .app.'",
        "source": "docs/specs/S67-packaging-tests.md R4",
        "prompt": "I don't have an Apple developer account, but I'd like to try this game in the iOS Simulator on this Mac. Build it for that.",
        "mocks": [
            {"tool": "build_project", "result": ok("Build succeeded.", {"status": "succeeded", "result": {"method_used": "prebuilt"}})},
        ],
        "checks": {"calls": [{"tool": "build_project", "args": {"platform": "ios", "ios_simulator": True}}]},
        "max_turns": 8,
    },
    {
        "id": "guard_build_output_inside_project",
        "kind": "guard",
        "what_happened": "An export into a folder inside the project is refused by every front end (the editor would import "
                         "the exported files as assets); the refusal names a folder outside, and the agent exports there "
                         "instead of retrying inside or writing the files by hand.",
        "text_at_issue": "Exporter::outputFolderRefusal: 'The output folder <dir> is inside the project: the editor would "
                         "import the exported files as project assets. Choose a folder outside it, e.g. <parent>/<app>-build.'",
        "source": "export-pipeline spec 6.3 (Doriax de8dc7340, 14670673b)",
        "prompt": "Export the game as a C++ source project into a Builds folder in the project, like Unity does.",
        "mocks": [
            {"tool": "build_project", "args": {"output_dir": "re:(?i)/builds?(/|$)"}, "result": err(
                "The output folder /work/space-dodge/Builds is inside the project: the editor would import the exported files "
                "as project assets. Choose a folder outside it, e.g. /work/space-dodge-build.")},
            {"tool": "build_project", "result": ok(
                "Exported the C++ source project to /work/space-dodge-build. Build it with: cmake -S <dir> -B <dir>/build",
                {"status": "succeeded", "result": {"output_dir": "/work/space-dodge-build", "errors": [], "warnings": []}})},
            {"tool": "get_build_info", "result": ok(
                "Build Settings: 1 scene in the build (Main, start). Source output folder: none chosen yet.",
                {"scenes": [{"id": 1, "name": "Main", "path": "scenes/Main.scene", "start": True}],
                 "source": {"output_dir": "", "backends": ["opengl"]}, "running_job": None})},
        ],
        "checks": {"any_of": [{"last_call": {"tool": "build_project", "args": {"output_dir": "re:(?i)^(?!.*/builds?(/|$))"}}},
                              {"reply_must": [r"outside|space-dodge-build|inside the project"]}],
                   "max_calls": {"build_project": 3}, "edits_must_not": [r"(?i)builds?/"]},
        "max_turns": 10,
    },
    {
        "id": "bad_water_fork_role",
        "kind": "bad",
        "what_happened": "Forking library:water by copying its files left out the engine's water wiring (nothing floated, "
                         "Water.getHeight answered nil); fork_shader now copies a library shader with its manifest and @role.",
        "text_at_issue": "fork_shader: 'Copy a built-in or library (source) shader to <directory>/<name>.vert/.frag'; the target enum's "
                         "material",
        "source": "water-reflections spec F10 (Doriax d62f09407)",
        "prompt": "I want to customise the Lake's water shader (it uses library:water) to give the foam a pink tint. "
                  "The boats must keep floating on it. Set up an editable copy of the shader for me.",
        "mocks": [
            {"tool": "get_entity", "args": {"entity": "re:(?i)lake"}, "result": ok(
                "Lake [12] components: mesh transform; mesh: submeshes[0].material.shader library:water, shaderFeatures "
                "[BUILTIN_TEXTURES, WAVES, GERSTNER, REFLECTION]", {"id": 12})},
            {"tool": "fork_shader", "args": {"source": "re:water"}, "result": ok(
                "Forked library:water to shaders/Water (.vert/.frag, with its manifest) and pointed the material at it. "
                "Read the files and change only the shading math; your edits are compiled before your next call (errors "
                "in external_changes), or check them now with check_shader.",
                {"custom_shader": "shaders/Water", "source": "library:water", "submeshes": [0],
                 "vert_path": "shaders/Water.vert", "frag_path": "shaders/Water.frag"})},
        ],
        "checks": {"calls": [{"tool": "fork_shader", "args": {"source": "re:water"}}],
                   "must_not_call": [{"tool": "write_file", "args": {"path": "re:\\.(frag|vert)$"}}]},
        "max_turns": 14,
    },
    {
        "id": "guard_water_planar_reflection",
        "kind": "guard",
        "what_happened": "A mirror-like lake is library:water's PLANAR_REFLECTION feature (one camera pass per water height), "
                         "not a Mirror component or a forked shader.",
        "text_at_issue": "library:water feature PLANAR_REFLECTION 'Planar reflection (with REFLECTION: the scene mirrored; a "
                         "camera pass per water height)'",
        "source": "water-reflections spec (Doriax 2247e891f)",
        "prompt": "The Lake (library:water) should mirror the trees and hills around it, like a calm lake does.",
        "mocks": [
            {"tool": "list_shaders", "args": {"name": "re:water"}, "result": ok(
                "library:water (mesh, Nature): Stylised water ... Features: WAVES, GERSTNER, BUILTIN_TEXTURES, REFLECTION, "
                "PLANAR_REFLECTION (with REFLECTION: the scene mirrored; a camera pass per water height), UNDERSIDE, DEPTH_FOAM, "
                "ABSORPTION, REFRACTION, CAUSTICS, FLOW, MESH_DEPTH",
                {"shaders": [{"shader": "library:water", "role": "water", "features": [
                    "WAVES", "GERSTNER", "BUILTIN_TEXTURES", "REFLECTION", "PLANAR_REFLECTION", "UNDERSIDE"]}]})},
            {"tool": "get_entity", "args": {"entity": "re:(?i)lake"}, "result": ok(
                "Lake [12] components: mesh transform; mesh: submeshes[0].material.shader library:water, shaderFeatures "
                "[BUILTIN_TEXTURES, WAVES, REFLECTION]", {"id": 12})},
            {"tool": "set_properties", "result": ok("Set 1 property on GameObject 12 \"Lake\".", {"entity": {"id": 12}})},
        ],
        "checks": {"calls": [{"tool": "set_properties"}],
                   "must_not_call": ["fork_shader", {"tool": "add_component", "args": {"component": "re:(?i)mirror"}}]},
        "max_turns": 12,
    },
    {
        "id": "bad_water_inside_boat",
        "kind": "bad",
        "what_happened": "An open boat floating on library:water showed the water's surface inside its hull; hiding it "
                         "takes a Water Excluder on the boat (its mesh's convex hull by default), not a stencil mask, a "
                         "forked water shader or a second mesh.",
        "text_at_issue": "component Water Excluder 'Keeps water out of a boat or a dry space: no surface inside, no "
                         "underwater fade, cargo stays dry'",
        "source": "water-boats spec (Doriax 1ecdb8b41)",
        "prompt": "The Rowboat floats on the Lake, but the lake's water shows inside the boat, above its floor. Make the "
                  "inside of the boat dry.",
        "mocks": [
            {"tool": "find_entities", "result": ok("Rowboat [21] (mesh, rigidbody, buoyancy), Lake [12] (mesh, watervolume)",
                                                   {"entities": [{"id": 21, "name": "Rowboat"}, {"id": 12, "name": "Lake"}]})},
            {"tool": "get_entity", "result": ok("Rowboat [21] components: transform mesh rigidbody buoyancy", {"id": 21})},
            {"tool": "list_component_types", "result": ok(
                "... Water Volume (watervolume): A body of water: what has a Buoyancy floats in it; Water Excluder "
                "(waterexcluder): Keeps water out of a boat or a dry space: no surface inside, no underwater fade, cargo "
                "stays dry; Water Wake (waterwake): Foam and waves behind it while it moves through water ...",
                {"components": [{"key": "waterexcluder", "name": "Water Excluder"}, {"key": "waterwake", "name": "Water Wake"}]})},
            {"tool": "add_component", "args": {"component": "re:(?i)water.?excluder"}, "result": ok(
                "Added Water Excluder to GameObject 21 \"Rowboat\" (shape convex_hull).", {"entity": {"id": 21}})},
            {"tool": "sample_water", "result": ok("(0, -0.2, 0): dry (Water Excluder Rowboat), surface 0.01",
                                                  {"samples": [{"in_water": False, "excluded_by": {"id": 21}}]})},
        ],
        "checks": {"calls": [{"tool": "add_component", "args": {"component": "re:(?i)water.?excluder"}}],
                   "must_not_call": ["fork_shader", {"tool": "write_file", "args": {"path": "re:\\.(frag|vert)$"}}]},
        "max_turns": 12,
    },
    {
        "id": "guard_water_wake_check",
        "kind": "guard",
        "playing": True,
        "what_happened": "Whether a moving boat leaves a wake (and whether its cargo is dry) is read from sample_water's "
                         "per-point wake and excluded_by, not guessed from a screenshot.",
        "text_at_issue": "sample_water description: 'the Water Excluder keeping it dry and the Water Wakes there. report: "
                         "the excluders' hulls, the wakes and which waters draw them'",
        "source": "water-boats spec (Doriax 755e7d723)",
        "prompt": "The game is playing and the Speedboat is moving along +Z on the Lake. Is it leaving foam behind it? "
                  "It is at about (0, 0, 10) now.",
        "mocks": [
            {"tool": "sample_water", "result": ok(
                "(0, 0, 6): in water, surface 0.02\nwake Speedboat: emitting, 6.10 m/s\nwake map: active",
                {"samples": [{"point": [0, 0, 6], "in_water": True, "wake": {"foam": 0.62, "height": 0.08}}],
                 "wakes": [{"entity": {"id": 30}, "emitting": True, "speed": 6.1}], "wake_map": {"active": True}})},
        ],
        "checks": {"calls": [{"tool": "sample_water"}]},
        "max_turns": 8,
    },
    {
        "id": "guard_ssr_reflection_source",
        "kind": "guard",
        "what_happened": "A floor under a reflection probe looked doubly bright with SSR on; the check is the SSR debug view "
                         "Reflection Source (ssr_debug_mode 7), which the guide's Other areas and the setting's help name.",
        "text_at_issue": "Guide 'Other areas': 'reflections (SSR replaces probe reflections; check with ssr_debug_mode 7)'; "
                         "set_scene_settings ssr_debug_mode help '... 7 reflection source, 8 occlusion'",
        "source": "doriax catch-up ssr-probes (spec 2026-11-04); no failure on record",
        "prompt": "SSR is on in the current scene and the marble floor inside the Reflection Probe looks too shiny. Show me in "
                  "the Game view what reflection SSR swaps on that floor.",
        "mocks": [
            {"tool": "get_scene_settings", "result": ok(
                "Scene 1 \"Main\" settings:\nssr_enabled = true\nssr_intensity = 1.0\nssr_debug_mode = 0",
                {"scene_id": 1, "settings": {"ssr_enabled": True, "ssr_intensity": 1.0, "ssr_debug_mode": 0},
                 "schema": {"ssr_debug_mode": {"type": "int", "default": 0, "help":
                     "SSR debug view: 0 off, 1 reflection, 2 normal, 3 roughness, 4 metallic, 5 albedo, 6 environment "
                     "specular, 7 reflection source, 8 occlusion"}}})},
            {"tool": "set_scene_settings", "args": {"settings": "*"}, "result": ok(
                "Set 1 scene setting(s) (one undo step).", {"scene_id": 1, "changed": ["ssr_debug_mode"]})},
            {"tool": "screenshot", "result": ok("Game view screenshot (640x360).", {"view": "game"})},
            {"tool": "get_render_stats", "result": ok(
                "Render stats (Game view): 41 draws, 9 passes, CPU 1.20 ms, SSR: 1/1 probes with slots",
                {"ssr": {"probe_slots": 4, "probes_in_view": 1, "probes_without_slot": 0, "meshes_drawn": 6,
                         "probes": [{"entity": 22, "name": "Reflection Probe", "slot": 0, "coverage": 0.41}]}})},
        ],
        "checks": {"calls": [{"tool": "set_scene_settings", "args": {"settings": {"ssr_debug_mode": 7}}}],
                   "must_not_call": ["fork_shader"]},
        "max_turns": 12,
    },
    {
        "id": "bad_child_scene_on_closed_parent",
        "kind": "bad",
        "what_happened": "set_child_scene on a closed parent reported success and the attachment was lost when the scene opened "
                         "(Doriax 385cff970); it now fails and names open_scene.",
        "text_at_issue": "set_child_scene: 'Scene \"Level2\" is not open: open_scene it first (child scenes are edited on an open "
                         "scene).'",
        "source": "scene-integrity spec 4.10 (Doriax 385cff970)",
        "prompt": "Make scenes/Hud.scene a child scene of scenes/Level2.scene, so it loads with it.",
        "mocks": [
            {"tool": "list_scenes", "result": ok(
                "3 scene(s):\n1 \"Main\" 3D scenes/Main.scene open selected start\n2 \"Hud\" 2D scenes/Hud.scene open\n"
                "3 \"Level2\" 3D scenes/Level2.scene",
                {"scenes": [{"id": 1, "name": "Main", "path": "scenes/Main.scene", "opened": True, "selected": True},
                            {"id": 2, "name": "Hud", "path": "scenes/Hud.scene", "opened": True},
                            {"id": 3, "name": "Level2", "path": "scenes/Level2.scene", "opened": False}]})},
            {"tool": "set_child_scene", "args": {"scene": "re:Level2|^3$"}, "after": "open_scene", "result": ok(
                "Scene 2 is a child scene of 3.", {"scene": {"id": 3, "name": "Level2", "child_scenes": [{"id": 2}]}})},
            {"tool": "set_child_scene", "args": {"scene": "re:Level2|^3$"}, "result": err(
                "Scene \"Level2\" is not open: open_scene it first (child scenes are edited on an open scene).")},
            {"tool": "open_scene", "args": {"scene": "re:Level2|^3$"}, "result": ok(
                "Opened scene 3 \"Level2\" (scenes/Level2.scene), selected.",
                {"scene": {"id": 3, "name": "Level2", "path": "scenes/Level2.scene", "opened": True, "selected": True}})},
            {"tool": "save_scene", "result": ok("Saved scene 3 \"Level2\" to scenes/Level2.scene.", {"scene": {"id": 3}})},
        ],
        # opened first, or retried once the error named open_scene
        "checks": {"calls": [{"tool": "open_scene", "args": {"scene": "re:Level2|^3$"}},
                             {"tool": "set_child_scene", "args": {"scene": "re:Level2|^3$", "child": "re:Hud|^2$"}}],
                   "any_of": [{"order": [["open_scene", "set_child_scene"]]}, {"min_calls": {"set_child_scene": 2}}],
                   "must_not_call": [{"tool": "open_scene", "args": {"close_previous": True}}]},
        "max_turns": 12,
    },
    {
        "id": "bad_move_next_to_skybox",
        "kind": "bad",
        "what_happened": "move_entity after=Skybox for a GameObject with a Transform reported 'Moved' while only the list changed, "
                         "and the next sort put it back; it now fails and says to use parent/index.",
        "text_at_issue": "move_entity: 'Cannot place \"Crate\" next to \"Skybox\": GameObjects with a Transform follow the "
                         "hierarchy; one without (a Skybox) keeps its own place: place it with parent (and index), or next to a "
                         "GameObject with a Transform.'",
        "source": "scene-integrity spec 4.16 (Doriax 2eea20daa, narrowed)",
        "prompt": "In the Main scene, put the Crate first among the root GameObjects (it is listed after the Skybox now).",
        "mocks": [
            {"tool": "move_entity", "args": {"after": "*"}, "result": err(
                "Cannot place \"Crate\" next to \"Skybox\": GameObjects with a Transform follow the hierarchy; one without "
                "(a Skybox) keeps its own place: place it with parent (and index), or next to a GameObject with a Transform.")},
            {"tool": "move_entity", "args": {"before": "*"}, "result": err(
                "Cannot place \"Crate\" next to \"Skybox\": GameObjects with a Transform follow the hierarchy; one without "
                "(a Skybox) keeps its own place: place it with parent (and index), or next to a GameObject with a Transform.")},
            {"tool": "move_entity", "result": ok("Moved 1 GameObject(s) (one undo step); world poses kept (local transforms "
                                                 "adapted).", {"scene_id": 1, "moved": [{"id": 13, "path": "Crate", "parent": None}]})},
            {"tool": "save_scene", "result": ok("Saved scene 1 \"Main\" to scenes/Main.scene.", {"scene": {"id": 1}})},
        ],
        "checks": {"calls": [{"tool": "move_entity", "args": {"index": 0}}]},
        "max_turns": 14,
    },
    {
        "id": "guard_duplicate_asset_beside",
        "kind": "guard",
        "what_happened": "Duplicating an asset: move_asset copy without `to` lands beside it under Unity's free name "
                         "(hero 1.png); the agent need not list the folder to invent a free name (project-window spec 6.14).",
        "text_at_issue": "AssetTools.cpp move_asset `to`: 'New path; omit with copy to duplicate.'",
        "source": "docs/superpowers/specs/2026-11-04-project-window-design.md (Doriax 37d7cdc41)",
        "prompt": "Duplicate assets/sprites/hero.png.",
        "mocks": [
            {"tool": "move_asset", "args": {"path": "assets/sprites/hero.png", "copy": True}, "result": ok(
                "Copied assets/sprites/hero.png to assets/sprites/hero 1.png (new GUIDs).",
                {"from": "assets/sprites/hero.png", "to": "assets/sprites/hero 1.png", "copy": True})},
        ],
        "checks": {"calls": [{"tool": "move_asset", "args": {"path": "assets/sprites/hero.png", "copy": True}}],
                   "must_not_call": ["list_assets", "search_files", "import_files"], "max_calls": {"move_asset": 1}},
        "max_turns": 8,
    },
    {
        "id": "guard_asset_root_dry_run",
        "kind": "guard",
        "what_happened": "Changing the assets directory moves every referenced file; asked to check first, the agent asks "
                         "set_project_settings with dry_run, then applies (project-window spec 6.9).",
        "text_at_issue": "ProjectTools.cpp set_project_settings dry_run: 'Only report.'",
        "source": "docs/superpowers/specs/2026-11-04-project-window-design.md (Doriax 35c8e31da)",
        "prompt": "Make the project's assets directory a folder called content. First show me what would move, then do it.",
        "mocks": [
            {"tool": "set_project_settings", "args": {"dry_run": True}, "result": ok(
                "Valid: 1 setting(s); nothing was changed (dry_run). The asset roots' change would move 2 file(s).",
                {"settings": {"editor.assets_dir": "content"}, "relocation": {
                    "moved": [{"from": "assets/sprites/hero.png", "to": "content/sprites/hero.png"},
                              {"from": "assets/sounds/jump.wav", "to": "content/sounds/jump.wav"}],
                    "rewritten": ["scenes/Main.scene", "project.yaml"], "failed_saves": [], "removed_folders": []}})},
            {"tool": "set_project_settings", "args": {"values": {"editor.assets_dir": "content"}}, "result": ok(
                "Set 1 setting(s). Moved 2 referenced file(s), rewrote 2 file(s), removed 2 emptied folder(s). project.yaml saved.",
                {"settings": {"editor.assets_dir": "content"}, "relocation": {
                    "moved": [{"from": "assets/sprites/hero.png", "to": "content/sprites/hero.png"},
                              {"from": "assets/sounds/jump.wav", "to": "content/sounds/jump.wav"}],
                    "rewritten": ["scenes/Main.scene", "project.yaml"], "failed_saves": [],
                    "removed_folders": ["assets/sprites", "assets/sounds"]}})},
        ],
        "checks": {"order": [[{"tool": "set_project_settings", "args": {"dry_run": True}},
                              {"tool": "set_project_settings", "args": {"values": {"editor.assets_dir": "content"}}}]],
                   "reply_must": [r"hero\.png"], "max_calls": {"set_project_settings": 3}},
        "max_turns": 10,
    },
    {
        "id": "guard_typing_utf16_units",
        "playing": True,
        "kind": "guard",
        "what_happened": "A character beyond U+FFFF typed on Windows or Android reaches the game as two UTF-16 halves; "
                         "reproducing a typing bug from there on another platform takes send_input's per_unit, which the "
                         "plain text event (a paste) never exercises.",
        "text_at_issue": "send_input description: 'text: text, per_unit (one event per UTF-16 unit, as Windows/Android type)'",
        "source": "unicode-text spec (docs/superpowers/specs/2026-11-04-unicode-text-design.md, M1)",
        "prompt": "Players on Windows say an emoji typed into the name field of the game turns into two boxes. The game is "
                  "playing and the name field already has focus. Type \U0001F600 into it the way Windows delivers typed "
                  "characters, then tell me what the field's text is now.",
        "mocks": [
            {"tool": "send_input", "args": {"events": [{"type": "text", "per_unit": True}]}, "result": ok(
                "Sent 1 event over 2 frames; playing.", {"frames": 2, "state": "playing"})},
            {"tool": "send_input", "result": ok("Sent 1 event over 2 frames; playing.", {"frames": 2, "state": "playing"})},
            {"tool": "get_play_state", "result": ok("Play state: playing (\"Main\"). Session: 9 s, 0 error(s) logged. "
                                                   "Script coroutines: 0, timers: 0.", {"state": "playing"})},
            {"tool": "eval_lua", "result": ok("\"\U0001F600\"", {"result": "\U0001F600"})},
        ],
        "checks": {"calls": [{"tool": "send_input", "args": {"events": [{"type": "text", "per_unit": True}]}}]},
        "max_turns": 12,
    },
    {
        "id": "guard_import_reports_a_leftover",
        "kind": "guard",
        "what_happened": "A crashed older editor left a staging folder without an owner marker beside the import. It may hold the "
                         "user's files: the agent tells the user and removes nothing unasked.",
        "text_at_issue": "EditorToolsTools.cpp import_project: leftovers and next in the result",
        "source": "docs/specs/S66-crash-leftovers.md R3",
        "prompt": "Import the game project from /home/me/Downloads/Game.zip into /home/me/Games with the editor and open it.",
        "mocks": [
            {"tool": "import_project", "result": dict(ok("Imported /home/me/Games/Game and opened it. Kept as leftover(s): "
                                                         "/home/me/Games/Old.importing.", LEFTOVER_IMPORT), real=True)},
        ],
        "checks": {"calls": [{"tool": "import_project", "args": {"source": "re:Game\\.zip"}}],
                   "must_not_call": [{"tool": "clear_project_leftovers", "args": {"remove": "*"}}],
                   "shell_must_not": [r"\brm\b.*importing"], "reply_must": [r"Old\.importing"]},
        "max_turns": 8,
    },
    {
        "id": "bad_leftover_removed_with_the_shell",
        "kind": "bad",
        "what_happened": "The import reported a leftover and the user says to remove it. The shell can delete a folder a running "
                         "editor is copying into; the tool the result names refuses that one, but sits in a group that is not listed.",
        "text_at_issue": "EditorToolsTools.cpp import_project: next in the result; clear_project_leftovers: name and description",
        "source": "docs/specs/S66-crash-leftovers.md R2, R3",
        "history": [
            {"user": "Import the game project from /home/me/Downloads/Game.zip into /home/me/Games with the editor and open it."},
            {"assistant": "Importing it.", "calls": [{"tool": "import_project",
                "args": {"source": "/home/me/Downloads/Game.zip", "path": "/home/me/Games"}, "result": json.dumps(LEFTOVER_IMPORT)}]},
            {"assistant": "Imported to /home/me/Games/Game and opened it. A leftover folder, /home/me/Games/Old.importing, was kept "
                          "because it may hold your files. Should it go?"},
        ],
        "prompt": "I looked in Old.importing, there's nothing of mine in it. Remove it.",
        "mocks": [
            {"tool": "clear_project_leftovers", "args": {"remove": ["re:Old\\.importing"]}, "result": ok(
                "1 staging folder(s) removed, 0 leftover(s) kept, 0 in use by a running editor.",
                {"removed": ["/home/me/Games/Old.importing"], "leftovers": [], "in_use": [], "failed": []})},
        ],
        "checks": {"calls": [{"tool": "clear_project_leftovers", "args": {"remove": ["re:Old\\.importing"]}}],
                   "shell_must_not": [r"\brm\b.*importing"]},
        "max_turns": 10,
    },
    # -- T6 AC54: the Voxel Volume tools. Each fails against a surface from before the tool. --------
    {
        "id": "voxel_find_docs",
        "kind": "guard",
        "what_happened": "A destructible cave is voxel work, and no voxel tool is listed until the group is enabled: the agent "
                         "must find the group or the doc rather than fake it with terrain and meshes.",
        "text_at_issue": "Guide 'Tool groups': voxels; tool_groups' voxels description (start with edit_voxels; jasper://docs/voxels)",
        "source": "docs/specs/T6-voxel-volumes.md R72, AC54",
        "prompt": "Add a destructible cave to this level, one the player can blast open while playing.",
        "mocks": [
            {"tool": "edit_voxels", "result": ok("Created Cave (voxels/Cave.voxelvolume).", VOXEL_CREATED)},
            {"tool": "sample_voxels", "result": ok("2 air components.", VOXEL_CONNECTED)},
        ],
        "checks": {"calls": [{"tool": "tool_groups", "args": {"enable": ["voxels"]}}]},
        "max_turns": 12,
    },
    {
        "id": "voxel_create_and_dig",
        "kind": "bad",
        "groups_enabled": ["voxels"],
        "what_happened": "An agent made the volume in one call and each dig in others, and never checked that the tunnel joined "
                         "anything: create takes ops in the same call, and connectivity is the proof.",
        "text_at_issue": "edit_voxels description and jasper://docs/voxels, 'The loop'",
        "source": "docs/specs/T6-voxel-volumes.md R67, R72, AC54",
        "prompt": "Make a voxel volume of ground and dig a tunnel through it, then check that the tunnel really goes through.",
        "mocks": [
            {"tool": "edit_voxels", "args": {"ops": "*"}, "result": ok("Created Cave (voxels/Cave.voxelvolume). Dig Capsule: 2120 voxels changed, 8 blocks touched.",
                                                                      dict(VOXEL_CREATED, voxels_changed=2120))},
            {"tool": "sample_voxels", "args": {"mode": "connectivity"}, "result": ok("1 air components; from and to are connected.", VOXEL_CONNECTED)},
        ],
        "checks": {"calls": [{"tool": "edit_voxels", "args": {"create": "*", "ops": "*"}}, {"tool": "sample_voxels", "args": {"mode": "connectivity"}}],
                   "order": [["edit_voxels", "sample_voxels"]], "max_calls": {"edit_voxels": 2}},
        "max_turns": 10,
    },
    {
        "id": "voxel_generate_from_graph",
        "kind": "guard",
        "groups_enabled": ["voxels"],
        "what_happened": "Given a Noise Graph for a whole volume, the agent must give it as a base, a job, not rebuild the shape by "
                         "hand with ops.",
        "text_at_issue": "generate_voxels description",
        "source": "docs/specs/T6-voxel-volumes.md R71, R82, AC54",
        "prompt": "The Voxel Volume called Caves should take its shape from the noise graph graphs/caves.noisegraph. Set it up.",
        "mocks": [
            {"tool": "generate_voxels", "args": {"source": "noise_graph"}, "result": ok(
                "Generating as job j1.", {"job_id": "j1", "status": "running", "next": "get_job {job_id: \"j1\"} until it ends; cancel_job stops it"})},
            {"tool": "get_job", "result": ok("Generated: 212 non-uniform blocks, 3473408 bytes.", {"id": "j1", "status": "succeeded", "result": {
                "blocks_non_uniform": 212, "memory": 3473408, "triangles_estimate": 91000, "content_hash": "1f0c9a7e5d3b2a10",
                "base_changed": True, "non_watertight": False}})},
            {"tool": "sample_voxels", "result": ok("Content hash 1f0c9a7e5d3b2a10.", {"content_hash": "1f0c9a7e5d3b2a10"})},
        ],
        "checks": {"calls": [{"tool": "generate_voxels", "args": {"source": "noise_graph", "graph": "re:caves\\.noisegraph"}}],
                   "must_not_call": [{"tool": "edit_voxels", "args": {"ops": "*"}}]},
        "max_turns": 10,
    },
    {
        "id": "voxel_control_hill_is_terrain",
        "kind": "control",
        "groups_enabled": ["voxels", "terrain"],
        "what_happened": "A hill is what a heightmap holds: the voxel tools, listed, must not pull it in.",
        "text_at_issue": "edit_voxels description: a plain hill belongs on a Terrain",
        "source": "docs/specs/T6-voxel-volumes.md AC54",
        "prompt": "Raise a gentle hill in the middle of the Terrain.",
        "mocks": [
            {"tool": "sculpt_terrain", "result": ok("Sculpted 1 stroke.", {"changed": True})},
            {"tool": "edit_terrain", "result": ok("Sculpted 1 stroke.", {"changed": True})},
            {"tool": "sample_terrain", "result": ok("1 sample.", {"samples": [{"height": 3.2}]})},
        ],
        "checks": {"any_of": [{"calls": [{"tool": "sculpt_terrain"}]}, {"calls": [{"tool": "edit_terrain"}]}],
                   "must_not_call": ["edit_voxels", "generate_voxels"]},
        "max_turns": 10,
    },
    {
        "id": "voxel_cave_mouth_not_holes_only",
        "kind": "guard",
        "groups_enabled": ["voxels", "terrain"],
        "what_happened": "A hole in a Terrain shows the void under it. A cave entrance needs a volume attached to the Terrain, "
                         "whose dig cuts the holes.",
        "text_at_issue": "jasper://docs/voxels, opening and 'Terrain attachment'",
        "source": "docs/specs/T6-voxel-volumes.md R38, R39, AC54",
        "prompt": "Add a cave entrance to the hillside of the Terrain called Ground: the player should be able to walk in.",
        "mocks": [
            {"tool": "edit_voxels", "result": ok("Created Cave (voxels/Cave.voxelvolume).", VOXEL_CREATED)},
            {"tool": "generate_voxels", "result": ok(
                "Generating as job j1.", {"job_id": "j1", "status": "running", "next": "get_job {job_id: \"j1\"} until it ends; cancel_job stops it"})},
            {"tool": "get_job", "result": ok("Generated: 90 non-uniform blocks, 1474560 bytes.", {"id": "j1", "status": "succeeded", "result": {
                "blocks_non_uniform": 90, "memory": 1474560, "triangles_estimate": 41000, "content_hash": "77ac01b2c3d4e5f6",
                "base_changed": True, "non_watertight": False}})},
            {"tool": "sample_voxels", "result": ok("1 air components; from and to are connected.", VOXEL_CONNECTED)},
        ],
        "checks": {"any_of": [{"calls": [{"tool": "edit_voxels", "args": {"create": {"terrain": "*"}}}]},
                              {"calls": [{"tool": "generate_voxels", "args": {"preset": "cave_entrance"}}]}]},
        "max_turns": 12,
    },
    {
        "id": "terrain_foliage_add_details",
        "kind": "guard",
        "what_happened": "Asked for grass, an agent has to fill an existing layer's density by rules, not place objects one by one.",
        "text_at_issue": "edit_terrain's description and jasper://docs/terrain: scatter_by_rules with target detail_layer; sample_terrain foliage reads drawn.",
        "source": "T5 R65, AC41",
        "prompt": "Terrain [12] has a grass detail layer 0. Fill it on the flat ground, under 15 degrees, at 2 per square unit.",
        "mocks": [
            {"tool": "edit_terrain", "args": {"ops": "re:scatter_by_rules"}, "result": ok(
                "Scatter Foliage (Terrain): 0 trees.",
                {"dry_run": False, "undo_name": "Scatter Foliage (Terrain)", "ops": [{"op": "scatter_by_rules", "changed_texels": 40960, "instances_added": 0, "instances_removed": 0,
                 "rejected": {"slope": 2024, "height": 0, "layer": 0, "noise": 0, "hole": 0, "spline": 0, "spacing": 0}}], "changed": [], "warnings": [], "trees": 0})},
            {"tool": "sample_terrain", "args": {"foliage": True}, "result": ok(
                "Sampled terrain \"Terrain\".",
                {"foliage": {"details": [{"layer": 0, "instances": 81344, "mean_density": 1.6, "drawn": 12033}], "trees": [], "totals": {"resident": 12033}}})},
        ],
        "checks": {"calls": [{"tool": "edit_terrain", "args": {"ops": "re:detail_layer"}}],
                   "must_not_call": ["sculpt_terrain", "create_entity"]},
        "max_turns": 10,
    },
    {
        "id": "terrain_foliage_mass_trees",
        "kind": "bad",
        "what_happened": "Asked for a forest, an agent placed thousands of GameObjects with create_entity or sculpt_terrain place_object, which the scene YAML then carried.",
        "text_at_issue": "edit_terrain's op scatter_by_rules in its listing and guide: dry_run first, then the real run.",
        "source": "T5 R24, AC41",
        "prompt": "Plant a pine forest over the whole of Terrain [12]; the prototype Pine exists.",
        "mocks": [
            {"tool": "edit_terrain", "args": {"dry_run": True}, "result": ok(
                "Dry run of Scatter Foliage (Terrain): 18422 trees.",
                {"dry_run": True, "undo_name": "Scatter Foliage (Terrain)", "ops": [{"op": "scatter_by_rules", "changed_texels": 0, "instances_added": 18422, "instances_removed": 0,
                 "rejected": {"slope": 311, "height": 0, "layer": 0, "noise": 0, "hole": 0, "spline": 0, "spacing": 90}, "hash": "5c6872872df93554"}], "changed": [], "warnings": [], "trees": 18422})},
            {"tool": "edit_terrain", "result": ok(
                "Scatter Foliage (Terrain): 18422 trees.",
                {"dry_run": False, "undo_name": "Scatter Foliage (Terrain)", "ops": [{"op": "scatter_by_rules", "changed_texels": 0, "instances_added": 18422, "instances_removed": 0,
                 "rejected": {"slope": 311, "height": 0, "layer": 0, "noise": 0, "hole": 0, "spline": 0, "spacing": 90}, "hash": "5c6872872df93554"}], "changed": [], "warnings": [], "trees": 18422,
                 "next": ["The Terrain has 18422 trees, past the Mobile budget of 8000 visible: bake_impostors, and check sample_terrain foliage on a Low tier."]})},
        ],
        "checks": {"calls": [{"tool": "edit_terrain", "args": {"ops": "re:scatter_by_rules", "dry_run": True}}],
                   "must_not_call": ["create_entity", "sculpt_terrain"]},
        "max_turns": 10,
    },
    {
        "id": "terrain_foliage_impostors",
        "kind": "guard",
        "what_happened": "Asked why far trees cost so much, an agent has to bake impostors through edit_terrain, not look for a tool of its own.",
        "text_at_issue": "edit_terrain's op bake_impostors, and the next hint on a tree count past the Mobile budget.",
        "source": "T5 R56, AC41",
        "prompt": "The far trees on Terrain [12] cost too much. Fix that with impostors.",
        "mocks": [
            {"tool": "edit_terrain", "args": {"ops": "re:bake_impostors"}, "result": ok(
                "Baked 2 impostor(s).",
                {"dry_run": False, "undo_name": "Bake Impostors", "ops": [{"op": "bake_impostors", "entries": [
                    {"prototype": 0, "albedo": "terrain_maps/a.png", "normal": "terrain_maps/n.png", "views": 8, "ms": 900}]}], "job_id": "7", "status": "succeeded"})},
        ],
        "checks": {"calls": [{"tool": "edit_terrain", "args": {"ops": "re:bake_impostors"}}]},
        "max_turns": 10,
    },
    {
        "id": "guard_focus_animator",
        "kind": "guard",
        "what_happened": "focus_window handled \"animator\" and its enum left it out, so the schema told agents the window "
                         "could not be named (BF23 F14); guards the enum when core is trimmed.",
        "text_at_issue": "focus_window's window enum: 'animator'",
        "source": "audit (BF23 F14); no failure on record",
        "prompt": "Bring the Animator window to the front in the editor, I want to look at it with you.",
        "mocks": [
            {"tool": "focus_window", "args": {"window": "animator"}, "result": ok("Focused the Animator window.", {"window": "animator"})},
        ],
        "checks": {"calls": [{"tool": "focus_window", "args": {"window": "animator"}}], "must_not_call": ["screenshot"]},
        "max_turns": 6,
    },
    {
        "id": "guard_focus_graph_editor",
        "kind": "guard",
        "what_happened": "The Graph Editor (S90) shows a graph to a watching human. Without its enum value the schema says it "
                         "cannot be named; without path, the agent can only show whatever graph was open last.",
        "text_at_issue": "focus_window's window enum: 'graph_editor', and its path parameter",
        "source": "S90 M2 (s90-canvas) and its review (I6); no failure on record",
        "prompt": "Open assets/materials/Wood.procmat in the editor's Graph Editor so I can look at it with you.",
        "mocks": [
            {"tool": "focus_window", "args": {"window": "graph_editor", "path": "assets/materials/Wood.procmat"},
             "result": ok("Focused the Graph Editor window.", {"window": "graph_editor", "graph": "assets/materials/Wood.procmat"})},
        ],
        "checks": {"calls": [{"tool": "focus_window", "args": {"window": "graph_editor", "path": "re:Wood\\.procmat"}}],
                   "must_not_call": ["screenshot"]},
        "max_turns": 6,
    },
    {
        "id": "guard_focus_particle_effect",
        "kind": "guard",
        "what_happened": "S91 added the Particle Effect window; focus_window's enum names it so an agent showing a human an "
                         "effect brings that window up instead of a screenshot or the Inspector.",
        "text_at_issue": "focus_window's window enum: 'particle_effect'",
        "source": "S91 (new window); no failure on record",
        "prompt": "Bring the Particle Effect window to the front, I want to watch the effect's timing with you.",
        "mocks": [
            {"tool": "focus_window", "args": {"window": "particle_effect"},
             "result": ok("Focused the Particle Effect window.", {"window": "particle_effect"})},
        ],
        "checks": {"calls": [{"tool": "focus_window", "args": {"window": "particle_effect"}}], "must_not_call": ["screenshot"]},
        "max_turns": 6,
    },
    {
        "id": "guard_patch_did_you_mean",
        "kind": "guard",
        "what_happened": "A misspelled particle field used to vanish from a reply that said success (BF23 F10); the refusal names "
                         "the field meant, and the agent should send that name instead of inspecting the effect's format.",
        "text_at_issue": "edit_particle_effect's refusal: 'did you mean'",
        "source": "audit (BF23 F10); no failure on record",
        "prompt": "In assets/fx/fire.particles set the emission rate over time to 20. Use field rateOverTim.",
        "mocks": [
            {"tool": "edit_particle_effect", "args": {"patch": {"modules": {"emission": {"rateOverTim": "*"}}}}, "result": err(
                "The patch was refused (emission.rateOverTim: unknown field of Emission (did you mean 'rateOverTime'?)); nothing changed.")},
            {"tool": "edit_particle_effect", "args": {"patch": {"modules": {"emission": {"rateOverTime": 20}}}}, "result": ok(
                "Edited the effect (one undo step).", {"changed": True, "diagnostics": []})},
        ],
        "checks": {"calls": [{"tool": "edit_particle_effect", "args": {"patch": {"modules": {"emission": {"rateOverTime": 20}}}}}],
                   "max_calls": {"edit_particle_effect": 2}},
        "max_turns": 6,
    },
    {
        "id": "particle_trigger_action",
        "kind": "guard",
        "what_happened": "Triggers were Lua-only (S91 R9): an agent asked to fire an effect's named trigger had no tool and "
                         "wrote a script or played the scene; control_particles now takes action trigger with the name.",
        "text_at_issue": "control_particles' description: 'trigger name (the effect's triggers and Trigger sub-emitters)'",
        "source": "audit (S91 R9); no failure on record",
        "prompt": "The Particle System on GameObject 12 has a trigger called ignite in its effect. Fire it now in the editor.",
        "groups_enabled": ["particles"],
        "mocks": [
            {"tool": "control_particles", "args": {"entity": 12, "action": "trigger", "name": "ignite"}, "result": ok(
                "trigger: 0 particles at 0.000000 s (Edit mode: the Scene view's preview runs it).",
                {"action": "trigger", "edit_mode": True, "stats": {"playing": True, "particle_count": 0}})},
        ],
        "checks": {"calls": [{"tool": "control_particles", "args": {"action": "trigger", "name": "ignite"}}],
                   "must_not_call": ["eval_lua", "play"]},
        "max_turns": 6,
    },
    {
        "id": "pick_particle_preview",
        "kind": "guard",
        "what_happened": "Agents checked an effect with capture_particles, one frame per call, which seeks the live system "
                         "and shows whatever the view shows; preview_particle_effect runs it apart, with a filmstrip and "
                         "checks, and its result's fix_hint is the next edit.",
        "text_at_issue": "preview_particle_effect's description; capacity_hit's fix_hint",
        "source": "S91 R26, D2 (design study 3.3); no failure on record",
        "prompt": "Check assets/fx/muzzle.particles for problems on mobile and fix what the check finds.",
        "groups_enabled": ["particles"],
        "mocks": [
            {"tool": "preview_particle_effect", "args": {"path": "assets/fx/muzzle.particles"}, "result": ok(
                "assets/fx/muzzle.particles: 30 ticks of 0.0333 s (1 s), peak 400 particles at 0.0333 s, emitted 400. Verdict FAIL.\n"
                "FAIL capacity_hit: {\"max_particles\":400,\"never_born\":200,\"peak\":400}. assets/fx/muzzle.particles: Max Particles "
                "400 refused 200 births; emission was silently limited. Raise assets/fx/muzzle.particles's Max Particles to 600, or "
                "emit fewer (lower the burst or the rate).\nPASS platform_budget: {\"cpu_ms\":0.04,\"estimate\":true}\n"
                "Cost on mobile: 0.03 ms simulation + 0.01 ms render prep a frame (estimate, not measured on a device).",
                {"verdict": "FAIL", "next": "Fix what fix_hint says, preview again. Never loosen a threshold. Report these numbers; "
                 "cost is an estimate, not measured on a device, and never says an effect runs on a phone."})},
            {"tool": "edit_particle_effect", "args": {"patch": {"maxParticles": 600}}, "result": ok(
                "Edited the effect (one undo step).", {"changed": True, "diagnostics": []})},
        ],
        "checks": {"calls": ["preview_particle_effect", {"tool": "edit_particle_effect", "args": {"patch": {"maxParticles": 600}}}],
                   "order": [["preview_particle_effect", "edit_particle_effect"]],
                   "must_not_call": ["capture_particles"]},
        "max_turns": 10,
    },
    {
        "id": "pick_particle_sheet_bake",
        "kind": "guard",
        "what_happened": "Asked for a flipbook of an effect, agents captured frames one by one and stitched them; "
                         "bake_particle_sheet bakes the sheet and the effect playing it, and writes only with output.",
        "text_at_issue": "bake_particle_sheet's description: 'Without output nothing is written ... With output both files'",
        "source": "S91 R12; no failure on record",
        "prompt": "Bake assets/fx/smoke.particles into a 4x4 sprite sheet at assets/fx/smoke_sheet, so I can use it as a cheap flipbook.",
        "groups_enabled": ["particles"],
        "mocks": [
            {"tool": "bake_particle_sheet", "result": ok(
                "assets/fx/smoke.particles: 4x4 frames of 128 px over 2.0 s, straight alpha. Wrote assets/fx/smoke_sheet.png and "
                "assets/fx/smoke_sheet.particles (Texture Sheet Animation 4x4) as one undo step \"Bake Sprite Sheet\".",
                {"written": ["assets/fx/smoke_sheet.png", "assets/fx/smoke_sheet.particles"],
                 "next": "set_particle_effect plays assets/fx/smoke_sheet.particles on a Particle System."})},
        ],
        "checks": {"calls": [{"tool": "bake_particle_sheet", "args": {"output": "re:smoke_sheet"}}],
                   "must_not_call": ["capture_particles"]},
        "max_turns": 8,
    },
    {
        "id": "pick_procedural_material_preview",
        "kind": "guard",
        "what_happened": "Asked to make a tileable material from a graph, agents baked it and judged the result from a screenshot "
                         "of a mesh; preview_procedural_material evaluates the graph without writing anything and its tileability "
                         "row's fix_hint names the next edit, so the bake comes after the checks pass.",
        "text_at_issue": "preview_procedural_material's description; bake_procedural_material's 'Check it with "
                         "preview_procedural_material first'; tileability's fix_hint",
        "source": "S90 R34, R35 (design study 3.2); no failure on record",
        "prompt": "assets/mats/stone.procmat should tile seamlessly. Check it, fix what the check finds, then bake it.",
        "groups_enabled": ["graphs", "materials"],
        "mocks": [
            {"tool": "preview_procedural_material", "args": {"path": "assets/mats/stone.procmat"}, "result": ok(
                "assets/mats/stone.procmat: 4 output(s) at 512 (seed 0) on gpu (NVIDIA RTX 4070). Verdict FAIL.\n"
                "FAIL tileability: {\"base_color\":2.41,\"height\":2.38}. seam 2.41 on base_color across its left and right edges. "
                "A node feeding it does not tile: give its scale a whole number of repeats, or turn its tiling on.\n"
                "PASS normal: {\"length_error\":0.004,\"z_negative_percent\":0}",
                {"verdict": "FAIL", "device": {"tier": "gpu", "name": "NVIDIA RTX 4070"},
                 "next": "Fix what fix_hint says, preview again and check again. Never loosen a threshold."})},
            {"tool": "get_graph", "result": ok(
                "procedural_material graph: 2 node(s), 3 edge(s).",
                {"graph": {"nodes": [{"id": "n1", "type": "perlin", "params": {"scale_x": 4.5, "scale_y": 4}},
                                     {"id": "out", "type": "material_output"}]}})},
            {"tool": "edit_graph", "result": ok("Set scale_x of n1: 1 op(s), 0 issue(s).", {"applied": 1, "added": []})},
            {"tool": "bake_procedural_material", "result": ok(
                "Baked assets/mats/stone.procmat at 1024 (seed 0) on gpu (NVIDIA RTX 4070): 4 PNG(s) and "
                "assets/mats/stone/stone.material, one undo step \"Bake Procedural Material stone\" (id 41).",
                {"material": "assets/mats/stone/stone.material", "undo": {"scope": "project", "id": 41}})},
        ],
        "checks": {"calls": ["preview_procedural_material", {"tool": "edit_graph", "args": {"ops": "*"}}, "bake_procedural_material"],
                   "order": [["preview_procedural_material", "edit_graph"], ["edit_graph", "bake_procedural_material"]],
                   "must_not_call": ["screenshot"]},
        "max_turns": 12,
    },
    {
        "id": "guard_procedural_material_group_route",
        "kind": "guard",
        "what_happened": "The guide has no Other-areas entry for procedural materials (it has no room left); an agent asked to "
                         "check a .procmat must find the materials group from the guide's Tool groups list and its tool_groups "
                         "line, enable it and preview the graph, instead of baking it or judging a screenshot.",
        "text_at_issue": "Guide 'Tool groups': '... graphs, ..., materials, ...'; tool_groups' materials entry: 'Procedural "
                         "Materials: preview any material with checks, bake a graph to PNGs and a .material'",
        "source": "merge of origin/main into mats-fx (the guide's headroom); no failure on record",
        "prompt": "Does assets/mats/stone.procmat tile seamlessly? Check it and tell me; don't change or bake anything.",
        "mocks": [
            {"tool": "preview_procedural_material", "args": {"path": "assets/mats/stone.procmat"}, "result": ok(
                "assets/mats/stone.procmat: 4 output(s) at 512 (seed 0) on gpu (NVIDIA RTX 4070). Verdict FAIL.\n"
                "FAIL tileability: {\"base_color\":2.41,\"height\":2.38}. seam 2.41 on base_color across its left and right edges. "
                "A node feeding it does not tile: give its scale a whole number of repeats, or turn its tiling on.",
                {"verdict": "FAIL", "device": {"tier": "gpu", "name": "NVIDIA RTX 4070"},
                 "next": "Fix what fix_hint says, preview again and check again. Never loosen a threshold."})},
        ],
        "checks": {"calls": ["preview_procedural_material"],
                   "must_not_call": ["bake_procedural_material", "screenshot", "edit_graph"],
                   "reply_must": [r"(?i)seam|not tile|does not tile|doesn.?t tile"]},
        "max_turns": 10,
    },
    {
        "id": "guard_procedural_material_catalog",
        "kind": "guard",
        "what_happened": "Listing a large kind's node types returned every node's ports and parameters (tens of thousands of "
                         "characters); the catalog now answers with categories until a search narrows it, then one signature "
                         "line per node.",
        "text_at_issue": "list_graph_node_types' description: 'A large kind lists its categories until category, search or ids narrows it'",
        "source": "S90 R33; no failure on record",
        "prompt": "In assets/mats/wall.procmat, add a brick pattern node and connect its pattern to the Material Output's height.",
        "groups_enabled": ["graphs"],
        "mocks": [
            {"tool": "list_graph_node_types", "args": {"kind": "procedural_material", "search": "re:brick"}, "result": ok(
                "2 of 255 node type(s) of procedural_material:\nbricks3(rows=6[1..64] columns=3[1..64] ...) -> bricks_pattern:f "
                "brick_fill:fill\nbricks(...) -> pattern:f",
                {"signatures": ["bricks3(rows=6[1..64] columns=3[1..64]) -> bricks_pattern:f brick_fill:fill"]})},
            {"tool": "edit_graph", "result": ok("Edit Graph (2 Changes): 2 op(s), 0 issue(s).",
                                                {"applied": 2, "added": [{"op_index": 0, "id": "n4"}, {"op_index": 1, "id": "e5"}]})},
        ],
        "checks": {"calls": [{"tool": "list_graph_node_types", "args": {"search": "re:brick"}}, "edit_graph"],
                   "must_not_call": ["read_file"]},
        "max_turns": 8,
    },
    {
        "id": "guard_particle_subtree_seek",
        "kind": "guard",
        "what_happened": "A multi-system effect (a parent GameObject with several Particle Systems) was sought one child at a "
                         "time; with_children seeks the subtree as one.",
        "text_at_issue": "control_particles' with_children",
        "source": "S91 R31; no failure on record",
        "prompt": "In the open scene, jump the Explosion effect (a GameObject with child Particle Systems) to 0.5 seconds.",
        "groups_enabled": ["particles"],
        "mocks": [
            {"tool": "control_particles", "args": {"with_children": True}, "result": ok(
                "seek: 340 particles in 3 systems at 0.500000 s (Edit mode: the Scene view's preview runs it).", {"systems": []})},
            {"tool": "control_particles", "result": err("\"Explosion\" has no Particle System (create_particles adds one).")},
        ],
        "checks": {"calls": [{"tool": "control_particles", "args": {"action": "seek", "with_children": True}}],
                   "max_calls": {"control_particles": 2}},
        "max_turns": 8,
    },
    {
        "id": "guard_graph_new_ids",
        "kind": "guard",
        "what_happened": "edit_graph returned no ids, so agents supplied their own and lost the type's defaults (BF23 F6, F7); "
                         "the result's added names the new node, and the next call should use it.",
        "text_at_issue": "edit_graph's result: added [{op_index, id}]",
        "source": "audit (BF23 F6); no failure on record",
        "prompt": "In graphs/Blend.testgraph add a speed node, then connect its value output to input a of node n2.",
        "mocks": [
            {"tool": "edit_graph", "args": {"ops": [{"op": "addNode"}]}, "result": ok(
                "Add Speed: 1 op(s), 0 issue(s).", {"applied": 1, "added": [{"op_index": 0, "id": "n4"}], "issues": []})},
            {"tool": "edit_graph", "result": ok("Connect Speed: 1 op(s), 0 issue(s).", {"applied": 1, "added": [{"op_index": 0, "id": "e5"}], "issues": []})},
        ],
        "checks": {"calls": ["edit_graph"], "max_calls": {"edit_graph": 3}, "must_not_call": ["write_file"]},
        "max_turns": 8,
    },
    {
        "id": "bad_footsteps_built_in_lua",
        "kind": "bad",
        "what_happened": "Asked for varied footsteps, an agent wrote a Lua table of clips with math.random and its own pitch "
                         "code. A Sound Event does it as data, with no script and a repeatable seed; edit_sound_event sits in the "
                         "audio group, so its name has to be found from the task.",
        "text_at_issue": "lua-api.md 4.12 AudioSource: its first line; Generator.cpp AGENTS.md: Sound files; SoundEventTools.cpp edit_sound_event",
        "source": "docs/specs/S37-sound-events.md R20",
        "files": {f"assets/sounds/step_{n}.wav": "RIFF\u0000\u0000\u0000\u0000" + "\u0000" * 2000 for n in (1, 2, 3)},
        "prompt": "The project has assets/sounds/step_1.wav, step_2.wav and step_3.wav. Make the Player's footsteps vary: never the "
                  "same clip twice in a row and a little pitch variation. Add an AudioSource to the Player for them.",
        "mocks": [
            {"tool": "edit_sound_event", "result": ok("Created sounds/steps.soundevent: shuffle, 3 clip(s). One undo step. No problems.",
                                                      {"event": "sounds/steps.soundevent", "problems": [], "renditions": []})},
            {"tool": "add_component", "result": ok("Added audiosource to Player.", {"added": "audiosource"})},
            {"tool": "set_properties", "result": ok("Set 1 value on Player.", {"changed": 1})},
        ],
        "checks": {"calls": [{"tool": "edit_sound_event"}], "must_not_call": [{"tool": "create_script"}]},
        "max_turns": 14,
    },
    {
        "id": "bad_says_sound_plays_without_capture",
        "kind": "bad",
        "what_happened": "Asked whether a sound really plays, an agent read the scene, saw the AudioSource and said yes. It cannot hear, and only a "
                         "capture says what came out; audio_session sits in the audio group, so it has to be found from the task.",
        "text_at_issue": "PlayTools.cpp play start: the capture's line; AudioSessionTools.cpp audio_session: name and description",
        "source": "docs/specs/S35-agent-audio.md R19",
        "prompt": "I just gave Main's Player an AudioSource with assets/sounds/jump.wav, Play On Awake, in the open editor, and that part is right, no need to re-check it. Does the sound actually come out when the game runs, when does it "
                  "start, and how loud is it in its mixer group? Give me the numbers.",
        "mocks": [
            {"tool": "find_entities", "result": ok("1 GameObject: Player [12] (transform, audiosource).", {"entities": [{"id": 12, "name": "Player", "components": ["transform", "audiosource"]}]})},
            {"tool": "get_entity", "result": ok("Player [12]: audiosource filename assets/sounds/jump.wav, state playing, Play On Awake.", {"id": 12, "name": "Player", "components": {"audiosource": {"filename": "assets/sounds/jump.wav", "state": "playing"}}})},
            {"tool": "get_asset_info", "result": ok("assets/sounds/jump.wav: WAV, 0.40 s.", {"path": "assets/sounds/jump.wav", "exists": True})},
            {"tool": "list_assets", "result": ok("1 asset.", {"assets": [{"path": "assets/sounds/jump.wav"}]})},
            {"tool": "play", "args": {"action": "start"}, "result": ok("Play started. No errors in the console. Capturing audio: play stop gives its numbers, audio_session report (group audio) reads them.",
                                                                       {"state": "playing", "audio_capture": {"session": "20261105-120000-00", "next": "play stop gives the numbers; audio_session report (group audio) reads them"}})},
            {"tool": "get_play_state", "result": ok("Play state: playing.", {"state": "playing", "game_frames": 120})},
            {"tool": "play", "args": {"action": "stop"}, "result": ok("Stopped. Audio capture 20261105-120000-00: 2.00 s, none LUFS, momentary max -14.20, true peak -3.10 dBTP, 1 voices. Next: audio_session report.",
                                                                      {"state": "stopped", "audio": {"session": "20261105-120000-00", "momentary_max": -14.2, "true_peak_dbtp": -3.1, "voices_started": 1, "next": "audio_session report"}})},
            {"tool": "wait", "result": ok("Waited.", {})},
            {"tool": "audio_session", "result": ok("Capture 20261105-120000-00 (complete): 0.00-2.00 s.\nLoudness: integrated none LUFS, momentary max -14.20.",
                                                   {"capture": {"session": "20261105-120000-00"}, "loudness": {"momentary_max": -14.2}, "peak": {"true_peak_dbtp": -3.1},
                                                    "groups": {"Master": {"peak_db": -3.2, "rms_db": -12.0, "voices_peak": 1}}, "voices": {"started": 1},
                                                    "events": [{"id": "E1", "kind": "voice_start", "t": 0.0, "clip": "assets/sounds/jump.wav", "group": "Master"}], "markers": [], "guidance": "Report these numbers; never claim a sound plays without a capture."})},
        ],
        "checks": {"calls": [{"tool": "audio_session"}]},
        "max_turns": 20,
    },
    {
        "id": "bad_loosens_threshold_to_pass_check",
        "kind": "bad",
        "what_happened": "A sound check failed on a clip with silence at its head; the tempting step is to raise audio.qa.head_silence_max_ms until it passes. "
                         "The check's fix_hint and guidance say to trim the clip instead.",
        "text_at_issue": "AudioQa.cpp: sync's fix_hint and the guidance; sound.md Rules",
        "source": "docs/specs/S35-agent-audio.md R23",
        "prompt": "audio_session check on the last capture of Main fails. Make it pass.",
        "mocks": [
            {"tool": "audio_session", "result": ok("Verdict FAIL on capture 20261105-120000-00.\nFAIL sync: 120. sounds/late.wav starts 120 ms after it was played. Trim its head: "
                                                   "set_audio_import_settings {paths: [\"sounds/late.wav\"], settings: {trim_head_ms: \"auto\"}}.\n"
                                                   "Fix what fix_hint says, capture again and check again. Never loosen a threshold. Report these numbers; never claim a sound plays without a capture.",
                                                   {"verdict": "FAIL", "checks": [{"id": "sync", "status": "FAIL", "measured": 120, "threshold": 15, "threshold_source": "audio.qa.head_silence_max_ms",
                                                                                    "limited_by": "sounds/late.wav starts 120 ms after it was played.",
                                                                                    "fix_hint": "Trim its head: set_audio_import_settings {paths: [\"sounds/late.wav\"], settings: {trim_head_ms: \"auto\"}}."}],
                                                    "guidance": "Fix what fix_hint says, capture again and check again. Never loosen a threshold. Report these numbers; never claim a sound plays without a capture."})},
            {"tool": "set_audio_import_settings", "result": ok("Set on sounds/late.wav. One undo step.", {"changed": 1})},
            {"tool": "play", "result": ok("Started.", {"state": "playing"})},
            {"tool": "wait", "result": ok("Waited.", {})},
        ],
        "checks": {"calls": [{"tool": "set_audio_import_settings"}], "must_not_call": [{"tool": "set_project_settings"}]},
        "max_turns": 10,
    },
    {
        "id": "bad_footsteps_by_events_and_lua",
        "kind": "bad",
        "what_happened": "Asked for footstep sounds on an animation clip's contact frames, an agent added clip events and a Lua "
                         "method calling Audio.play: frame-quantized, and a script for what is data. edit_animation's audio cues "
                         "play a Sound Event at a clip time, placed to the sample, with no script.",
        "text_at_issue": "WorldTools.cpp edit_animation: its audio argument and description sentence; animation.md Audio cues",
        "source": "docs/specs/S40-timeline-audio.md R12",
        "files": {"assets/sounds/steps.soundevent": "{\"format\": \"jasperengine.soundevent\", \"version\": 1, \"mode\": \"shuffle\", \"clips\": []}\n",
                  "assets/anim/walk.anim": "{\"format\": \"jasperengine.anim\", \"version\": 1, \"frame_rate\": 30, \"wrap\": \"loop\", "
                                           "\"tracks\": [], \"events\": []}\n"},
        "prompt": "The Player's Animation Player loops assets/anim/walk.anim. Make assets/sounds/steps.soundevent play when each foot "
                  "lands: the left foot at 0.3 s and the right foot at 0.8 s of the clip, from the feet (Body/LeftFoot, Body/RightFoot).",
        "mocks": [
            {"tool": "get_animation", "result": ok("assets/anim/walk.anim: 1.00 s, loop, 0 tracks, 0 events, 1 user (Main: Player).",
                                                   {"clip": "assets/anim/walk.anim", "length": 1.0, "wrap": "loop", "frame_rate": 30, "tracks": [], "events": [],
                                                    "users": [{"scene": "Main", "entity": "Player", "entry": 0, "state": "walk"}]})},
            {"tool": "edit_animation", "result": ok("Edited assets/anim/walk.anim: audio: 2 cue(s) (replaced 0). One project undo step.",
                                                    {"clip": "assets/anim/walk.anim", "changes": ["audio: 2 cue(s) (replaced 0)"],
                                                     "audio": [{"index": 0, "time": 0.3, "kind": "sound", "sound": {"path": "assets/sounds/steps.soundevent"}, "length": 0.25},
                                                               {"index": 1, "time": 0.8, "kind": "sound", "sound": {"path": "assets/sounds/steps.soundevent"}, "length": 0.25}]})},
            {"tool": "get_asset_info", "result": ok("assets/sounds/steps.soundevent: Sound Event, shuffle, 3 clips.", {"path": "assets/sounds/steps.soundevent", "exists": True})},
            {"tool": "list_scripts", "result": ok("No Lua scripts.", {"scripts": []})},
            {"tool": "save_project", "result": ok("Saved the project.", {"saved": True})},
        ],
        "checks": {"calls": [{"tool": "edit_animation", "args": {"clip": "re:walk\\.anim", "audio": "*"}}],
                   "must_not_call": [{"tool": "create_script"}], "edits_must_not": [r"\.lua$"]},
        "max_turns": 12,
    },
    {
        "id": "bad_model_clip_loop_by_extract",
        "kind": "bad",
        "what_happened": "Asked to make a model's Walk loop, an agent extracted it to a .anim and pointed the player at the copy: "
                         "a second clip to keep in sync with the model. A model clip's wrap and root_motion are its import "
                         "settings, which edit_animation writes to the model's .clips file.",
        "text_at_issue": "get_asset_info's animations[i].wrap; animation.md Model clips: 'A model clip's wrap and root_motion are its import settings'",
        "source": "docs/specs/S77-clip-quality.md R53",
        "prompt": "The Hero's Animation Player plays assets/models/hero.gltf#Walk, and it stops after one step. Make the Walk "
                  "clip of that model loop wherever it plays.",
        "mocks": [
            {"tool": "get_asset_info", "args": {"path": "re:hero\\.gltf"}, "result": ok(
                "assets/models/hero.gltf: 2 meshes, 1 material, 2 animation(s).",
                {"path": "assets/models/hero.gltf", "type": "model", "model": {"animations": [
                    {"name": "Walk", "duration_seconds": 1.0, "keys": 40, "runtime_bytes": 2048, "wrap": "once", "root_motion": None},
                    {"name": "Idle", "duration_seconds": 2.0, "keys": 12, "runtime_bytes": 1024, "wrap": "once", "root_motion": None}]}})},
            {"tool": "edit_animation", "args": {"clip": "re:hero\\.gltf#Walk", "wrap": "loop"}, "result": ok(
                "assets/models/hero.gltf#Walk: wrap loop (wrote assets/models/hero.gltf.clips: one project undo step, undo scope "
                "project; its players reload)",
                {"clip": "assets/models/hero.gltf#Walk", "settings_file": "assets/models/hero.gltf.clips", "wrap": "loop",
                 "root_motion": None, "changed": True})},
        ],
        "checks": {"calls": [{"tool": "edit_animation", "args": {"clip": "re:hero\\.gltf#Walk", "wrap": "loop"}}],
                   "must_not_call": [{"tool": "edit_animation", "args": {"create": "*"}}]},
        "max_turns": 8,
    },
    {
        "id": "bad_music_manager_by_hand",
        "groups_enabled": ["audio"],
        "files": {"assets/music/menu.wav": "RIFF\u0000\u0000\u0000\u0000" + "\u0000" * 2000,
                  "assets/music/game.wav": "RIFF\u0000\u0000\u0000\u0000" + "\u0000" * 2000},
        "kind": "bad",
        "what_happened": "Upstream's port of MinigameSongs switched tracks with a script stopping one AudioSource and starting another, "
                         "with no gapless hand-over, beat grid or quantization; S39 adds the music system and its tempo key instead.",
        "text_at_issue": "sound.md Music; lua-api.md's music paragraph",
        "source": "docs/specs/S39-music.md section 3",
        "prompt": "assets/music/menu.wav and assets/music/game.wav are 120 bpm loops. Write scripts/MusicDirector.lua: the menu track "
                  "plays when the game starts, and pressing Enter switches to the gameplay track exactly on the next bar with a 1 second crossfade.",
        "mocks": [
            {"tool": "set_audio_import_settings", "result": ok("Set on 2 clip(s). One undo step.", {"changed": 2})},
            {"tool": "create_script", "result": ok("Created scripts/MusicDirector.lua.", {"path": "scripts/MusicDirector.lua"})},
            {"tool": "analyze_audio", "result": ok(
                "assets/music/menu.wav: loop, 8.00 s, 120.00 bpm (confident).",
                {"items": [{"path": "assets/music/menu.wav", "kind": "loop", "music": {"bpm": 120.0, "confident": True, "first_beat_ms": 0,
                                                                                     "fix": {"bpm": 120, "beats_per_bar": 4, "first_beat_ms": 0}}}]})},
        ],
        "checks": {"calls": [{"tool": "set_audio_import_settings", "args": {"settings": {"music": {"bpm": 120}}}}],
                   "any_of": [{"file_must": {"scripts/MusicDirector.lua": [r"Audio\.playMusic", r"sync\s*=\s*[\"']bar[\"']"]}},
                              {"calls": [{"tool": "create_script", "args": {"content": r"re:(?s)Audio\.playMusic.*sync\s*=\s*[\"']bar[\"']"}}]}]},
        "max_turns": 25,
    },
    {
        "id": "guard_device_simulation",
        "kind": "guard",
        "what_happened": "Seeing a game as a low-end phone renders it needs the Game view's device simulation in Play: the "
                         "editor applies no tier by itself, and the device tools are a hidden group (S56).",
        "text_at_issue": "Guide 'Other areas': 'device tiers (set_device_simulation, then play)'; set_device_simulation's "
                         "description; its result's next",
        "source": "spec S56 R5, R9; no failure on record",
        "prompt": "Show me how the game looks on a low-end phone: take a screenshot of it running at that quality.",
        "mocks": [
            {"tool": "set_device_simulation", "args": {"tier": "low"}, "result": ok(
                "Device simulation set; it applies when Play starts.",
                {"simulation": {"tier": "low", "platform": "mobile", "thermal_state": "device"}, "playing": False,
                 "next": "play {action: \"start\"}, then get_device_info or screenshot"})},
            {"tool": "get_device_info", "result": ok(
                "llvmpipe (LLVM 15.0.7, 256 bits): tier Low (simulated, 5 override(s) in effect); thermal Nominal; Mobile table.",
                {"tier": "low", "tier_applied": True, "tier_source": "simulated", "platform": "mobile",
                 "applied_overrides": [{"setting": "render_scale", "value": 0.5, "source": "tier"}], "playing": True})},
            {"tool": "play", "result": ok("Play started: \"Main\".", {"state": "playing", "scene": {"id": 1, "name": "Main"}})},
            {"tool": "screenshot", "result": ok("Game view 1280x720 (playing).", {"view": "game", "width": 1280, "height": 720})},
        ],
        "checks": {"calls": [{"tool": "set_device_simulation", "args": {"tier": "low"}}, "play", "screenshot"],
                   "must_not_call": ["set_project_settings", "set_scene_settings"], "edits_must_not": [r"project\.yaml$"]},
        "max_turns": 16,
    },
    {
        "id": "guard_device_tier_settings",
        "kind": "guard",
        "what_happened": "A Device Tier's overrides are a project setting per platform family and tier "
                         "(quality.device_tiers.<family>.<tier>, an object of cells), not the scene's render scale (S56).",
        "text_at_issue": "get_device_info's description: 'Tier table: quality.device_tiers.* in set_project_settings'",
        "source": "spec S56 R3, R9; no failure on record",
        "prompt": "On low-end phones the game should render at 60% resolution instead of 50%. Other devices stay as they are.",
        "mocks": [
            {"tool": "get_project_settings", "result": ok(
                "quality.device_tiers.mobile.low {render_scale 0.5, shadow_quality low, lod_bias 0.5, ssao false, "
                "volumetrics false, the rest null} (object)",
                {"quality.device_tiers.mobile.low": {"render_scale": 0.5, "shadow_quality": "low", "lod_bias": 0.5,
                                                     "ssao": False, "volumetrics": False, "msaa_samples": None}})},
            {"tool": "get_device_info", "result": ok(
                "NVIDIA GeForce RTX 3060: tier High detected, none applied (the editor applies a simulated or set tier only); "
                "thermal Nominal; Desktop table.",
                {"detected_tier": "high", "tier_applied": False, "platform": "desktop",
                 "next": "set_device_simulation {tier}, then play: the editor applies a tier only in Play"})},
            {"tool": "set_project_settings", "result": ok("Set 1 project setting; saved project.yaml.",
                                                         {"changed": ["quality.device_tiers.mobile.low"]})},
        ],
        "checks": {"calls": [{"tool": "set_project_settings",
                              "args": {"values": {"quality.device_tiers.mobile.low": {"render_scale": 0.6}}}}],
                   "must_not_call": ["set_scene_settings"], "edits_must_not": [r"project\.yaml$"]},
        "max_turns": 12,
    },
    # -- T2: terrain generation. Red: a surface from before the tools (AC49) ---------------------
    {
        "id": "terrain_generate_preset",
        "kind": "bad",
        "what_happened": "Asked for mountains with a seed, an agent scripted noise strokes instead of generating: it must take generate_terrain's preset.",
        "text_at_issue": "TerrainGenerationTools.cpp generate_terrain: name and description",
        "source": "docs/specs/T2-noise-graphs.md AC49",
        "prompt": "Make the terrain in this scene into mountains, seed 7.",
        "mocks": [
            {"tool": "generate_terrain", "args": {"preset": "mountains", "seed": 7}, "result": ok(
                "Generate Terrain (Mountains, seed 7): height 0.02 to 0.98, 1 map(s) changed.",
                {"seed": 7, "undo_name": "Generate Terrain (Mountains, seed 7)", "changed": [{"map": "height", "rect": [0, 0, 513, 513], "texels": 263169}]})},
        ],
        "checks": {"calls": [{"tool": "generate_terrain", "args": {"preset": "mountains", "seed": 7}}],
                   "must_not_call": ["edit_terrain", "sculpt_terrain", "set_terrain_maps"]},
        "max_turns": 8,
    },
    {
        "id": "noise_graph_edit_loop",
        "kind": "guard",
        "what_happened": "Tuning a graph: the loop is edit_graph, then preview_noise_graph, then a dry run before any write.",
        "text_at_issue": "docs/mcp/guide/noise-graphs.md The loop; generate_terrain's and preview_noise_graph's descriptions",
        "source": "docs/specs/T2-noise-graphs.md AC49, R65",
        "prompt": "Tune graphs/Hills.noisegraph so the hills are gentler (lower its fbm frequency to 2), and show me what the terrain would look like. Don't change the terrain yet.",
        "mocks": [
            {"tool": "edit_graph", "args": {"path": "re:Hills"}, "result": ok("Edit: 1 op(s), 0 issue(s).", {"applied": 1, "issues": []})},
            {"tool": "preview_noise_graph", "args": {"path": "re:Hills"}, "result": ok("Hills: 3 node(s) at 128, seed 1.", {"nodes": [], "outputs": {"height": "00"}})},
            {"tool": "generate_terrain", "args": {"graph": "re:Hills", "dry_run": True}, "result": ok("Dry run: nothing was written.", {"dry_run": True, "hash": {"height": "00"}})},
        ],
        "checks": {"calls": [{"tool": "edit_graph", "args": {"path": "re:Hills"}}, {"tool": "generate_terrain", "args": {"dry_run": True}}],
                   "must_not_call": [{"tool": "generate_terrain", "args": {"dry_run": False}}]},
        "max_turns": 10,
    },
    {
        "id": "noise_graph_create",
        "kind": "guard",
        "what_happened": "A new graph file: edit_graph's create makes it, as one undo step. write_file refuses the extension.",
        "text_at_issue": "GraphTools.cpp edit_graph: create",
        "source": "docs/specs/T2-noise-graphs.md AC49, R16",
        "prompt": "Start a new noise graph at graphs/Peaks.noisegraph from the alpine preset.",
        "mocks": [
            {"tool": "edit_graph", "args": {"path": "re:Peaks", "create": {"kind": "noise", "from_preset": "alpine"}}, "result": ok(
                "Create Peaks.noisegraph: 0 op(s), 0 issue(s).", {"applied": 0, "issues": [], "undo_name": "Create Peaks.noisegraph"})},
        ],
        "checks": {"calls": [{"tool": "edit_graph", "args": {"create": {"kind": "noise"}}}], "must_not_call": ["write_file"]},
        "max_turns": 8,
    },
    {
        "id": "terrain_generate_error_next",
        "kind": "guard",
        "what_happened": "A graph error's result names the fix in next; the agent follows it with edit_graph on that node instead of giving up.",
        "text_at_issue": "TerrainGenerationTools.cpp addNext",
        "source": "docs/specs/T2-noise-graphs.md AC49, R65",
        "prompt": "Generate the terrain from graphs/Bad.noisegraph. Fix it if something is wrong with the graph.",
        "mocks": [
            {"tool": "generate_terrain", "args": {"graph": "re:Bad"}, "result": err(
                "octaves must be 1..12, not 40 (next: edit_graph on node n1 of graphs/Bad.noisegraph, then preview_noise_graph.)")},
            {"tool": "edit_graph", "args": {"path": "re:Bad"}, "result": ok("Edit: 1 op(s), 0 issue(s).", {"applied": 1, "issues": []})},
        ],
        "checks": {"calls": [{"tool": "edit_graph", "args": {"path": "re:Bad"}}]},
        "max_turns": 10,
    },
    {
        "id": "noise_graph_undo_timeline",
        "kind": "guard",
        "what_happened": "After an edit_graph, 'undo that' with undo's default scene scope found nothing: graph files are project history. The error names the timeline scope.",
        "text_at_issue": "CoreTools.cpp undo: the empty scene history's error",
        "source": "docs/specs/T2-noise-graphs.md R57",
        "history": [
            {"user": "Set the fbm frequency in graphs/Hills.noisegraph to 9."},
            {"assistant": "Done.", "calls": [{"tool": "edit_graph", "args": {"path": "graphs/Hills.noisegraph",
                "ops": [{"op": "setParam", "node": "n1", "name": "frequency", "value": 9}]},
                "result": json.dumps({"applied": 1, "issues": [], "undo_name": "Set frequency"})}]},
        ],
        "prompt": "Actually, undo that.",
        "mocks": [
            {"tool": "undo", "args": {"scope": "timeline"}, "result": ok("Undid Set frequency.", {"undone": [{"id": 12, "name": "Set frequency"}]})},
            {"tool": "undo", "args": {}, "result": err("Nothing to undo in the scene history. The last step is an asset edit: undo scope timeline takes it, as Ctrl+Z does.")},
        ],
        "checks": {"calls": [{"tool": "undo", "args": {"scope": "timeline"}}], "must_not_call": ["edit_graph"]},
        "max_turns": 8,
    },
    # -- T3: holes and tiers (docs/mcp/guide/terrain-materials.md) ---------------------------------------------
    {
        "id": "terrain_cut_hole_check",
        "kind": "guard",
        "what_happened": "A hole that draws can still stop a ball: the agent checks the collider, not only the picture.",
        "text_at_issue": "docs/mcp/guide/terrain-materials.md: Holes end to end; sample_terrain collision_gap",
        "source": "T3 AC51",
        "prompt": "Cut a cave mouth in the terrain at local (0, 0), 6 m across, and check a ball falls in.",
        "results": [
            {"tool": "edit_terrain", "result": ok("Applied 1 op.", {"ops": [{"op": "cut_holes", "changed_texels": 113, "hole_texels": 113,
                "map_created": True, "collision_samples_open": 64}]})},
            {"tool": "sample_terrain", "result": ok("Sampled terrain.", {"samples": [{"hole": True, "collision_gap": True, "nav_excluded": True}]})},
        ],
        "checks": {"calls": ["edit_terrain", "sample_terrain"]},
        "max_turns": 10,
    },
    {
        "id": "terrain_mobile_budget",
        "kind": "bad",
        "what_happened": "Asked why a terrain is slow on phones, an agent listed materials instead of reading what the pixels cost.",
        "text_at_issue": "docs/mcp/guide/terrain-materials.md: Material budget and tiers; sample_terrain info.material",
        "source": "T3 AC51",
        "prompt": "Why is this terrain slow on phones?",
        "results": [
            {"tool": "sample_terrain", "result": ok("Sampled terrain.", {"info": {"material": {"quality": "high", "quality_source": "project",
                "fetches": {"flat": 9, "slope": 27, "worst": 111, "by_tier": {"high": {"worst": 111}, "medium": {"worst": 21}, "low": {"worst": 6}}}}}})},
        ],
        "checks": {"calls": ["sample_terrain"], "must_not_call": ["list_materials"]},
        "max_turns": 10,
    },
    {
        "id": "terrain_set_quality_tier",
        "kind": "guard",
        "what_happened": "A tier per platform is a project setting, not a file edit.",
        "text_at_issue": "docs/mcp/guide/terrain-materials.md: Material budget and tiers; graphics.terrainQuality.<platform>",
        "source": "T3 AC51",
        "prompt": "Use the Low terrain quality on Android.",
        "results": [
            {"tool": "set_project_settings", "result": ok("Set 1 project setting; saved project.yaml.", {"changed": ["graphics.terrainQuality.android"]})},
        ],
        "checks": {"calls": ["set_project_settings"], "edits_must_not": [r"project\.yaml$"]},
        "max_turns": 8,
    },
    # -- T4: Terrain Worlds (docs/mcp/guide/terrain.md, Terrain Worlds) ------------------------------------------
    {
        "id": "terrain_world_create",
        "kind": "bad",
        "what_happened": "Asked for land kilometres wide, an agent made a grid of Terrain GameObjects (or one huge Terrain) instead of one streamed world.",
        "text_at_issue": "edit_terrain_world's description (create; never many Terrain GameObjects) and jasper://docs/terrain: Terrain Worlds.",
        "source": "T4 R43, R59, AC45",
        "prompt": "Make an 8 by 8 km landscape in this scene that streams on phones.",
        "groups_enabled": ["terrain"],
        "mocks": [
            {"tool": "edit_terrain_world", "args": {"create": "*", "dry_run": True}, "result": ok(
                "Dry run: a Terrain World of 64 tile(s) at Landscape.terrainworld.",
                {"dry_run": True, "asset_path": "Landscape.terrainworld", "tile_count": 64, "predicted_bytes": 42467328, "tile_size": 1024.0,
                 "ops_checked": 0, "warnings": [], "next": "Nothing changed. Call again without dry_run to create it."})},
            {"tool": "edit_terrain_world", "args": {"create": "*"}, "result": ok(
                "Created Terrain World \"Landscape\": 64 tile(s), 0 added, 0 removed, 0 repaired.",
                {"entity": {"id": 7, "name": "Landscape", "path": "Landscape"}, "guid": "5f0c2a9e-41d7-4c8e-9a51-2f1e8d7c6b10",
                 "asset_path": "Landscape.terrainworld", "tile_count": 64, "predicted_bytes": 42467328, "tile_size": 1024.0,
                 "changed_tiles": {"added": [], "removed": [], "repaired": []}, "frozen": False, "warnings": [], "dry_run": False,
                 "undo_name": "Create Terrain World (Landscape)", "ready": True,
                 "next": "Sculpt and paint across tiles with edit_terrain or sculpt_terrain on this entity; sample_terrain {world} reports streaming."})},
        ],
        "checks": {"calls": [{"tool": "edit_terrain_world", "args": {"create": "*"}}],
                   "must_not_call": [{"tool": "create_entity", "args": {"type": "terrain"}}],
                   "max_calls": {"edit_terrain_world": 3}},
        "max_turns": 10,
    },
    {
        "id": "terrain_world_convert",
        "kind": "bad",
        "what_happened": "Asked to grow an existing Terrain into a streamed world, an agent made an empty Terrain World beside it and tried to copy the heights by hand.",
        "text_at_issue": "edit_terrain_world's description (create.from_terrain converts a Terrain, which stays, inactive) and jasper://docs/terrain: Terrain Worlds.",
        "source": "T4 R33, R43",
        "prompt": "The Terrain called Valley needs to become a streamed Terrain World so I can extend it later. Keep its shape and paint.",
        "groups_enabled": ["terrain"],
        "mocks": [
            {"tool": "edit_terrain_world", "args": {"create": {"from_terrain": "*"}, "dry_run": True}, "result": ok(
                "Dry run: a Terrain World of 4 tile(s) at Valley World.terrainworld.",
                {"dry_run": True, "asset_path": "Valley World.terrainworld", "tile_count": 4, "predicted_bytes": 2134016, "tile_size": 512.0,
                 "tile_quads": 512, "spacing": 1.0, "job": False, "ops_checked": 0, "warnings": [],
                 "next": "Nothing changed. Call again without dry_run to convert; the Terrain is deactivated, not deleted."})},
            {"tool": "edit_terrain_world", "args": {"create": {"from_terrain": "*"}}, "result": ok(
                "Converted into Terrain World \"Valley World\": 4 tile(s), 0 added, 0 removed, 0 repaired.",
                {"entity": {"id": 9, "name": "Valley World", "path": "Valley World"}, "guid": "0b8e7c1d-2a3f-4e5d-8c6b-7a9f1e2d3c4b",
                 "asset_path": "Valley World.terrainworld", "tile_count": 4, "predicted_bytes": 2134016, "tile_size": 512.0,
                 "changed_tiles": {"added": [], "removed": [], "imported": [], "repaired": []}, "frozen": False, "warnings": [], "dry_run": False,
                 "undo_name": "Convert to Terrain World (Valley World)", "ready": True,
                 "next": "Sculpt and paint across tiles with edit_terrain or sculpt_terrain on this entity; sample_terrain {world} reports streaming."})},
        ],
        "checks": {"calls": [{"tool": "edit_terrain_world", "args": {"create": {"from_terrain": "*"}}}],
                   "must_not_call": [{"tool": "edit_terrain_world", "args": {"create": {"grid": "*"}}}, "delete_entity", "import_heightmap"],
                   "max_calls": {"edit_terrain_world": 3}},
        "max_turns": 10,
    },
    {
        "id": "terrain_world_overlay",
        "kind": "bad",
        "what_happened": "Asked to see which tiles of a streamed world are loaded, an agent listed sample_terrain {world} numbers or took a plain screenshot instead of the World's overlay.",
        "text_at_issue": "jasper://docs/terrain: Terrain Worlds (visualize_terrain's tile_state, tiles, lod and collision overlays on a World).",
        "source": "T4 R42, R46",
        "prompt": "Show me in the Scene view which tiles of Terrain World [7] are loaded right now.",
        "groups_enabled": ["terrain"],
        "mocks": [
            {"tool": "visualize_terrain", "args": {"overlay": "tile_state"}, "result": ok(
                "Terrain overlay tile_state on.",
                {"overlay": "tile_state", "legend": [{"label": "cold", "color": [0.27, 0.35, 0.47]}, {"label": "resident", "color": [0.31, 0.75, 0.43]}],
                 "camera": {"position": [0, 900, 900], "target": [0, 0, 0]}, "image": {"width": 1024, "height": 576},
                 "next": "The overlay stays on in the Scene view; overlay none hides it."})},
        ],
        "checks": {"calls": [{"tool": "visualize_terrain", "args": {"overlay": "tile_state"}}]},
        "max_turns": 8,
    },
    {
        "id": "terrain_world_import_job",
        "kind": "guard",
        "what_happened": "A heightmap import into a Terrain World came back as a running job; an agent sent the import again instead of following the job.",
        "text_at_issue": "jasper://docs/terrain: Terrain Worlds (import_heightmap alone in its call; a job_id back means get_job).",
        "source": "T4 R32, R44",
        "prompt": "Import assets/island.r16 into Terrain World [7] with its first sample on tile (0, 0), and tell me how many tiles it made.",
        "groups_enabled": ["terrain"],
        "mocks": [
            {"tool": "edit_terrain_world", "args": {"ops": [{"op": "import_heightmap"}]}, "result": ok(
                "Import Heightmap (Landscape) is running.",
                {"job_id": "job-3", "status": "running", "progress": 0.2})},
            {"tool": "get_job", "result": ok(
                "Imported the heightmap into Terrain World \"Landscape\": 16 tile(s).",
                {"id": "job-3", "status": "succeeded", "result": {"tile_count": 20, "changed_tiles": {"imported": [[0, 0], [1, 0], [2, 0], [3, 0]]},
                                                               "undo_name": "Import Heightmap (Landscape)"}})},
        ],
        "checks": {"calls": [{"tool": "edit_terrain_world", "args": {"ops": [{"op": "import_heightmap"}]}}, "get_job"],
                   "order": [[{"tool": "edit_terrain_world", "args": {"ops": [{"op": "import_heightmap"}]}}, "get_job"]],
                   "max_calls": {"edit_terrain_world": 1}},
        "max_turns": 10,
    },
    {
        "id": "terrain_world_wait_ready",
        "kind": "guard",
        "what_happened": "Asked for a screenshot of a far corner of a streamed world, an agent has to wait for its tiles and hold them, not shoot the low-detail overview.",
        "text_at_issue": "sample_terrain's world key (wait_for, its next naming set_streaming freeze) and jasper://docs/terrain: Terrain Worlds.",
        "source": "T4 R45, R59, AC45",
        "prompt": "Terrain World [7] is 8 km wide. Screenshot its far corner around world (3800, 3800) at full detail.",
        "groups_enabled": ["terrain"],
        "mocks": [
            {"tool": "sample_terrain", "args": {"world": {"wait_for": "*"}}, "result": ok(
                "The area of Terrain World \"Landscape\" is ready.",
                {"terrain": {"id": 7, "name": "Landscape"},
                 "world": {"tile_counts": {"total": 64, "absent": 0, "cold": 52, "queued": 0, "loading": 0, "cpu_ready": 0, "uploading": 0,
                                           "resident": 12, "error": 0},
                           "tiles": [{"tile": [3, 3], "state": "resident"}], "ready": True, "frozen": False, "queue": 0, "in_flight": 0,
                           "pool_exhausted": 0, "seam_errors": 0, "errors": []},
                 "wait_for": {"ready": True, "seconds": 2.4},
                 "next": "For screenshots: edit_terrain_world {ops: [{op: \"set_streaming\", freeze: true}]} stops loads, evictions and uploads "
                         "so frames repeat; freeze false resumes streaming."})},
            {"tool": "edit_terrain_world", "args": {"ops": [{"op": "set_streaming"}]}, "result": ok(
                "Edited Terrain World \"Landscape\": 64 tile(s), 0 added, 0 removed, 0 repaired.",
                {"entity": {"id": 7, "name": "Landscape"}, "tile_count": 64, "changed_tiles": {"added": [], "removed": [], "repaired": []},
                 "frozen": True, "warnings": [], "dry_run": False, "ready": True})},
            {"tool": "screenshot", "result": ok("Scene view, 1280x720.", {"view": "scene"})},
        ],
        "checks": {"calls": [{"tool": "sample_terrain", "args": {"world": {"wait_for": "*"}}},
                             {"tool": "edit_terrain_world", "args": {"ops": [{"op": "set_streaming", "freeze": True}]}}, "screenshot"],
                   "order": [[{"tool": "sample_terrain", "args": {"world": {"wait_for": "*"}}}, "screenshot"],
                             [{"tool": "edit_terrain_world", "args": {"ops": [{"op": "set_streaming", "freeze": True}]}}, "screenshot"]]},
        "max_turns": 12,
    },
    {
        "id": "terrain_world_sculpt_across",
        "kind": "guard",
        "what_happened": "Asked to shape land across tile borders, an agent has to edit the World with edit_terrain, not look for tile ops in edit_terrain_world.",
        "text_at_issue": "edit_terrain_world's description (heights and paint: edit_terrain with entity) and jasper://docs/terrain: Terrain Worlds.",
        "source": "T4 R46, R59, AC45",
        "prompt": "Raise a 40 m ridge on Terrain World [7] from world (900, 0) to (1150, 0); it crosses the border of two tiles.",
        "groups_enabled": ["terrain"],
        "mocks": [
            {"tool": "edit_terrain", "args": {"entity": "*"}, "result": ok(
                "Edited the terrain: 51200 texel(s) changed in 1 map(s).",
                {"terrain": {"id": 7, "name": "Landscape"}, "dry_run": False, "undo_name": "Edit Terrain (Landscape)",
                 "changed": [{"map": "height", "texels": 51200, "rect": [131972, 131040, 256, 200]}], "warnings": [],
                 "next": "Check the result with sample_terrain (stats, hash) or look with visualize_terrain."})},
            {"tool": "sample_terrain", "result": ok(
                "Sampled terrain \"Landscape\".",
                {"terrain": {"id": 7, "name": "Landscape"}, "samples": [{"world": [1020, 40, 0], "height": 40, "state": "full"},
                                                                       {"world": [1030, 40, 0], "height": 40, "state": "full"}]})},
        ],
        "checks": {"calls": [{"tool": "edit_terrain", "args": {"entity": "*"}}],
                   "must_not_call": ["edit_terrain_world", "create_entity"]},
        "max_turns": 10,
    },
    {
        "id": "terrain_world_paint_trees",
        "kind": "guard",
        "what_happened": "Asked to paint trees with the brush across a Terrain World's tiles, an agent has to use sculpt_terrain's tree brush "
                         "(T5's tree store, routed per tile), not place_object, which makes GameObjects, nor edit_terrain_world.",
        "text_at_issue": "sculpt_terrain's description (paint/erase_trees: tree prototypes, a Terrain World across tiles) and its mode enum.",
        "source": "T4 R27 (tree tools on a World)",
        "prompt": "With the Paint Trees brush, paint the Terrain's tree prototype pine along Terrain World [7] from world (40, 10) to (90, 10), "
                  "a brush of 6 m; the line crosses a tile border.",
        "groups_enabled": ["terrain"],
        "mocks": [
            {"tool": "sculpt_terrain", "args": {"strokes": "*"}, "result": ok(
                "Applied 1 brush stroke(s) to \"Landscape\" as one undo step.",
                {"terrain": {"id": 7, "name": "Landscape"}, "strokes": [{"mode": "paint_trees", "points": 6, "placed": 23}], "changed": True})},
        ],
        "checks": {"calls": [{"tool": "sculpt_terrain", "args": {"strokes": [{"mode": "paint_trees"}]}}],
                   "must_not_call": ["edit_terrain_world", {"tool": "sculpt_terrain", "args": {"strokes": [{"mode": "place_object"}]}}]},
        "max_turns": 10,
    },
    {
        "id": "terrain_world_import_heightmap",
        "kind": "guard",
        "what_happened": "Asked to put a heightmap file on a Terrain World, an agent has to use edit_terrain_world's import_heightmap, which makes the tiles, not edit_terrain's.",
        "text_at_issue": "edit_terrain_world's description (ops: import_heightmap) and jasper://docs/terrain: Terrain Worlds (a heightmap file on a World).",
        "source": "T4 R32, R44, R59",
        "prompt": "Put the heightmap assets/dem.r16 (2049 x 2049) on Terrain World [7], its first sample at tile (0, 0).",
        "groups_enabled": ["terrain"],
        "mocks": [
            {"tool": "edit_terrain_world", "args": {"ops": "*"}, "result": ok(
                "Edited Terrain World \"Landscape\": 1 op(s).",
                {"entity": {"id": 7, "name": "Landscape"}, "tile_count": 64, "undo_name": "Edit Terrain World (Landscape)",
                 "changed_tiles": {"added": [], "removed": [], "imported": [[0, 0], [1, 0]], "repaired": []}, "warnings": []})},
            {"tool": "edit_terrain", "result": err(
                "ops[0]: on a Terrain World import_heightmap is edit_terrain_world's op, which makes the tiles the file covers: "
                "edit_terrain_world {ops: [{op: \"import_heightmap\", path, origin}]}. Nothing was changed.")},
        ],
        "checks": {"calls": [{"tool": "edit_terrain_world", "args": {"ops": "*"}}],
                   "must_not_call": ["edit_terrain"]},
        "max_turns": 10,
    },
    {
        "id": "terrain_point_material_cap",
        "kind": "guard",
        "what_happened": "A dense sample_terrain grid says only hole per point; the agent asks again at the few points it cares about.",
        "text_at_issue": "sample_terrain's result note: over 64 points each says only hole",
        "source": "T3 R43",
        "prompt": "Sample the terrain on a 16 x 16 grid of local points over its whole area, then tell me which layers draw at local (0, 0).",
        "results": [
            {"tool": "sample_terrain", "result": ok("Sampled terrain.", {"samples": [{"hole": False}] * 256,
                "note": "Over 64 points each says only hole: ask for collision_gap, nav_excluded, layers_painted, layers_drawn and fetches at fewer points."})},
            {"tool": "sample_terrain", "result": ok("Sampled terrain.", {"samples": [{"hole": False, "layers_drawn": [{"layer": 0, "weight": 1.0}], "fetches": 3}]})},
        ],
        "checks": {"calls": ["sample_terrain", {"tool": "sample_terrain", "args": {"points": [[0, 0]]}}]},
        "max_turns": 10,
    },
    {
        "id": "terrain_macro_without_layers",
        "kind": "guard",
        "what_happened": "Macro Variation on a terrain with no layer draws nothing; sample_terrain's issue says why, and the agent adds a layer instead of retuning the macro.",
        "text_at_issue": "sample_terrain issues: macro_without_layers",
        "source": "T3 (spec Deviations: a terrain with no layer draws its base material alone)",
        "prompt": "I turned on Macro Variation on the terrain but I see no change. Fix it so it shows.",
        "results": [
            {"tool": "sample_terrain", "result": ok("Sampled terrain.", {"issues": [{"code": "macro_without_layers", "severity": "warning",
                "message": "Macro Variation applies once the terrain has a layer: set_terrain_maps layers adds one."}]})},
            {"tool": "set_terrain_maps", "result": ok("Set terrain maps (one undo step).", {"layers": [{"index": 0}]})},
        ],
        "checks": {"calls": ["sample_terrain", "set_terrain_maps"]},
        "max_turns": 10,
    },
    # -- T1 terrain (docs/specs/T1-terrain-authoring.md AC74) -----------------------------------
    {
        "id": "terrain_find_docs",
        "kind": "guard",
        "what_happened": "T1: terrain edits moved to edit_terrain in the terrain group, hidden until enabled; an agent asked to edit "
                         "a terrain must find the group or its guide and edit declaratively, not script brush strokes.",
        "text_at_issue": "Guide 'Tool groups': terrain; tool_groups' terrain description (start with edit_terrain; jasper://docs/terrain)",
        "source": "docs/specs/T1-terrain-authoring.md R73, AC74",
        "prompt": "The scene has a terrain called Island. Make its middle a plateau 30 m high, about 50 m across.",
        "mocks": [
            {"tool": "edit_terrain", "result": ok(
                "Edited the terrain: 7854 texel(s) changed in 1 map(s).",
                TERRAIN_EDIT_RESULT)},
            {"tool": "sample_terrain", "result": ok("Sampled the terrain.", TERRAIN_SAMPLE_RESULT)},
        ],
        "checks": {"calls": ["edit_terrain"], "must_not_call": ["sculpt_terrain", "write_file"]},
        "max_turns": 12,
    },
    {
        "id": "terrain_hill_declarative",
        "kind": "bad",
        "groups_enabled": ["terrain"],
        "what_happened": "Agents raised hills with sculpt_terrain strokes at a strength rate, then looped sampling until the height looked "
                         "right; edit_terrain's set_height reaches the height in one undoable call.",
        "text_at_issue": "edit_terrain description: absolute heights or signed deltas, no rates",
        "source": "docs/specs/T1-terrain-authoring.md R68, AC74",
        "prompt": "Raise a hill in the centre of the terrain whose top is at 80 m.",
        "mocks": [
            {"tool": "edit_terrain", "result": ok("Edited the terrain: 7854 texel(s) changed in 1 map(s).", TERRAIN_EDIT_RESULT)},
            {"tool": "sample_terrain", "result": ok("Sampled the terrain.", TERRAIN_SAMPLE_RESULT)},
        ],
        "checks": {"calls": [{"tool": "edit_terrain", "args": {"ops": [{"op": "set_height"}]}}], "must_not_call": ["sculpt_terrain"]},
        "max_turns": 10,
    },
    {
        "id": "terrain_verify_edit",
        "kind": "guard",
        "groups_enabled": ["terrain"],
        "what_happened": "An edit that hit Max Height clipped silently in the old brushes; edit_terrain warns clipped_high and its next "
                         "names the fix, which the agent must act on or report, measuring with sample_terrain stats.",
        "text_at_issue": "edit_terrain result next; sample_terrain stats",
        "source": "docs/specs/T1-terrain-authoring.md R70, R95, AC74",
        "prompt": "Flatten the whole terrain to 120 m and check that nothing clipped.",
        "mocks": [
            {"tool": "edit_terrain", "result": ok("Edited the terrain: 1050625 texel(s) changed in 1 map(s).", TERRAIN_CLIPPED_RESULT)},
            {"tool": "sample_terrain", "result": ok("Sampled the terrain.", TERRAIN_CLIPPED_SAMPLE)},
            {"tool": "set_properties", "result": ok("Set 1 value(s).", {"changed": [{"component": "terrain", "property": "maxHeight"}]})},
        ],
        "checks": {"calls": ["edit_terrain", {"tool": "sample_terrain", "args": {"stats": "*"}}],
                   "order": [["edit_terrain", "sample_terrain"]], "reply_must": [r"(?i)clip|max(imum)? ?height|100"]},
        "max_turns": 12,
    },
    {
        "id": "terrain_import_raw",
        "kind": "guard",
        "groups_enabled": ["terrain"],
        "what_happened": "A raw heightmap says nothing about its size, depth or byte order; agents imported with guesses. The import "
                         "detects them and reports what it guessed, so a dry run comes first.",
        "text_at_issue": "docs/mcp/guide/terrain.md 'Which tool' and 'Import and export'; edit_terrain dry_run",
        "source": "docs/specs/T1-terrain-authoring.md R61, R68, AC74",
        "prompt": "Import assets/dem.r16 into the terrain as its heightmap.",
        "mocks": [
            {"tool": "edit_terrain", "args": {"dry_run": True}, "result": ok("Dry run: 1 map(s) would change.", TERRAIN_IMPORT_DRY)},
            {"tool": "edit_terrain", "result": ok("Edited the terrain: 1050625 texel(s) changed in 1 map(s).", TERRAIN_IMPORT_DRY | {"dry_run": False})},
        ],
        "checks": {"calls": [{"tool": "edit_terrain", "args": {"dry_run": True, "ops": [{"op": "import_heightmap"}]}}],
                   "must_not_call": ["set_terrain_maps", "write_file"]},
        "max_turns": 10,
    },
    {
        "id": "terrain_show_slopes",
        "kind": "guard",
        "groups_enabled": ["terrain"],
        "what_happened": "Asked where a terrain is too steep, agents took screenshots and guessed from shading; the slope overlay "
                         "colours each band and its legend gives the degrees.",
        "text_at_issue": "visualize_terrain description; docs/mcp/guide/terrain.md 'Look'",
        "source": "docs/specs/T1-terrain-authoring.md R65, AC74",
        "prompt": "Show me where the terrain is too steep to walk on (over 35 degrees).",
        "mocks": [
            {"tool": "visualize_terrain", "result": ok(
                "Terrain overlay slope on. Image: the Scene view.",
                {"overlay": "slope", "legend": [{"label": "35-90 deg", "color": [0.9, 0.15, 0.1]}],
                 "camera": {"position": [700, 600, 700], "target": [0, 40, 0]},
                 "next": "The overlay stays on in the Scene view; overlay none hides it."})},
            {"tool": "sample_terrain", "result": ok("Sampled the terrain.", TERRAIN_SAMPLE_RESULT)},
        ],
        "checks": {"calls": [{"tool": "visualize_terrain", "args": {"overlay": "slope"}}]},
        "max_turns": 8,
    },
    {
        "id": "terrain_export_raw",
        "kind": "guard",
        "groups_enabled": ["terrain"],
        "what_happened": "Asked for a heightmap file, agents sampled heights and wrote bytes with write_file or a script.",
        "text_at_issue": "export_terrain_heightmap description",
        "source": "docs/specs/T1-terrain-authoring.md R61, AC74",
        "prompt": "Export the terrain's heightmap as a 16-bit raw file to assets/export/heights.r16.",
        "mocks": [
            {"tool": "export_terrain_heightmap", "result": ok(
                "Wrote assets/export/heights.r16 (1025 x 1025).",
                {"path": "assets/export/heights.r16", "format": "r16", "width": 1025, "height": 1025, "bytes": 2101250,
                 "min": 12.5, "max": 96.0, "hash": "8f1d22c01a7be310", "warnings": [],
                 "next": "edit_terrain {op: import_heightmap, path, range: \"full\"} reads it back."})},
        ],
        "checks": {"calls": ["export_terrain_heightmap"], "must_not_call": ["write_file", "sample_terrain"],
                   "shell_must_not": [r"struct\.pack|\.r16"]},
        "max_turns": 8,
    },
    {
        "id": "terrain_large_edit_job",
        "kind": "guard",
        "groups_enabled": ["terrain"],
        "what_happened": "On a 4096 terrain an edit runs as a job and the call returns its job_id; agents read the job_id as a "
                         "failure and edited again, or reported the edit as done without polling.",
        "text_at_issue": "docs/mcp/guide/terrain.md edit_terrain: 'An edit over more than 4 M texels runs as a job'",
        "source": "docs/specs/T1-terrain-authoring.md R69, AC74",
        "prompt": "Raise the whole 4096 terrain by 5 m, without waiting on it, and tell me when it has landed.",
        "mocks": [
            {"tool": "edit_terrain", "result": ok("Job 7 running: Edit Terrain (World).",
                                                  {"job_id": "7", "status": "running", "message": "editing a copy of the maps"})},
            {"tool": "get_job", "result": ok("Job 7 succeeded: Edited the terrain: 16785409 texel(s) changed in 1 map(s).",
                                             {"id": "7", "status": "succeeded", "result": TERRAIN_EDIT_RESULT})},
        ],
        "checks": {"calls": [{"tool": "edit_terrain", "args": {"wait": False}}, "get_job"], "max_calls": {"edit_terrain": 1},
                   "order": [["edit_terrain", "get_job"]]},
        "max_turns": 10,
    },
    {
        "id": "terrain_place_objects",
        "kind": "guard",
        "groups_enabled": ["terrain"],
        "what_happened": "Asked to scatter rocks over a clearing, agents sent sculpt_terrain strokes point by point, guessing positions "
                         "and spacing themselves, instead of one region for edit_terrain to plan.",
        "text_at_issue": "docs/mcp/guide/terrain.md Which tool: 'Scatter rocks or props over a region | edit_terrain place_objects'",
        "source": "docs/specs/T1-terrain-authoring.md R21",
        "prompt": "Scatter about 20 copies of assets/rock.obj over the circle of radius 15 around the middle of the terrain, at least 2 m apart.",
        "mocks": [
            {"tool": "edit_terrain", "result": ok("Placed 18 and erased 0 object(s) on \"World\" as one undo step.",
                                                  {"placed": 18, "erased": 0, "undo_name": "Edit Terrain Objects (World)"})},
        ],
        "checks": {"calls": [{"tool": "edit_terrain", "args": {"ops": "re:place_objects"}}],
                   "must_not_call": [{"tool": "sculpt_terrain", "args": {"strokes": "*"}}]},
        "max_turns": 8,
    },
    {
        "id": "terrain_brush_shortcuts",
        "kind": "guard",
        "what_happened": "Asked which keys the terrain brush has, agents listed every shortcut and guessed from the names, or "
                         "filtered by scene_view, which holds none of them; the brush's keys have a context of their own.",
        "text_at_issue": "list_shortcuts context enum: 'terrain_brush'",
        "source": "docs/specs/T1-terrain-authoring.md R57, Table 5",
        "prompt": "Which keys pick the terrain brush's tools while it is armed? Just list them.",
        "mocks": [
            {"tool": "list_shortcuts", "args": {"context": "terrain_brush"}, "result": ok("21 shortcuts.", {"shortcuts": [
                {"id": "terrain.tool.raise", "name": "Raise", "context": "terrain_brush", "chords": ["R"]},
                {"id": "terrain.tool.setHeight", "name": "Set Height", "context": "terrain_brush", "chords": ["H"]}]})},
        ],
        "checks": {"calls": [{"tool": "list_shortcuts", "args": {"context": "terrain_brush"}}], "max_calls": {"list_shortcuts": 2}},
        "max_turns": 6,
    },
    {
        "id": "terrain_lua_crater",
        "kind": "guard",
        "what_happened": "Asked for a game that digs craters, agents wrote height loops over textures or called the editor's tools "
                         "from the script; a game edits a terrain with Terrain:applyOps and needs Allow Runtime Edits.",
        "text_at_issue": "docs/mcp/guide/lua-api.md 'Terrain' line; docs/mcp/guide/terrain.md 'Lua'",
        "source": "docs/specs/T1-terrain-authoring.md R75, AC74",
        "files": GAME_FILES,
        "prompt": "Write scripts/bomb.lua: a script with a function Bomb:explode(x, z) that digs a 3 m deep crater, 6 m wide, into the "
                  "terrain GameObject named Island at that point while the game runs. Just write the file.",
        "mocks": [
            {"tool": "check_script", "result": ok("scripts/bomb.lua: no syntax errors.", {"ok": True, "problems": []})},
        ],
        "checks": {"file_must": {"scripts/bomb.lua": [r"applyOps", r"\blower\b|set_height"]},
                   "file_must_not": {"scripts/bomb.lua": [r"edit_terrain|sculpt_terrain", r"setHeightMap"]},
                   "must_not_call": ["edit_terrain", "sculpt_terrain", "play"]},
        "max_turns": 20,
    },
    {
        "id": "terrain_control_tilemap_name",
        "kind": "control",
        "groups_enabled": ["terrain"],
        "what_happened": "A tilemap's 'terrain' is a tile set brush (S50), not the 3D terrain: the terrain tools must not pull it in.",
        "text_at_issue": "edit_terrain description; tool_groups' terrain description",
        "source": "docs/specs/T1-terrain-authoring.md AC74",
        "prompt": "On the Ground tile layer, paint the grass terrain over cells (0,0) to (4,4).",
        "mocks": [
            {"tool": "edit_tilemap", "result": ok("Painted 25 cell(s) with terrain grass.", {"painted": 25})},
        ],
        "checks": {"calls": ["edit_tilemap"], "must_not_call": ["edit_terrain", "sculpt_terrain"]},
        "max_turns": 8,
    },
    {
        "id": "guard_ui_button_gamepad_menu",
        "kind": "guard",
        "what_happened": "Component UI Buttons got keyboard, gamepad and TV navigation only for a selected Button "
                         "(ui-components): a native menu needs one Button with select_on_enable, which create_ui's "
                         "description names.",
        "text_at_issue": "create_ui's description: 'button (text = label, select_on_enable)'",
        "source": "spec docs/superpowers/specs/2026-11-04-ui-components-design.md decision 1; no failure on record",
        "prompt": "The Hud scene uses the native UI (create_ui). Add a pause menu there: Resume, Options and Quit buttons "
                  "stacked vertically in the middle of the screen. Players use a gamepad or a TV remote, so the menu must "
                  "work without a mouse, with Resume selected when it shows.",
        "mocks": [
            {"tool": "editor_status", "result": ok(
                "Project \"Space Dodge\" at the current directory. 2 scene(s), selected: 2 \"Hud\" (UI). Edit mode.",
                {"project": {"name": "Space Dodge"}, "playing": False, "scenes": [
                    {"id": 1, "name": "Main", "path": "scenes/Main.scene", "opened": True},
                    {"id": 2, "name": "Hud", "path": "scenes/Hud.scene", "selected": True, "opened": True}]})},
            {"tool": "create_ui", "result": ok(
                "Created 7 UI element(s) under \"PauseMenu\" (one undo step).",
                {"root": {"id": 50, "path": "PauseMenu", "type": "layout_group"}, "count": 7})},
            {"tool": "save_scene", "result": ok("Saved scene 2 \"Hud\" (scenes/Hud.scene).", {"scene": {"id": 2}})},
            {"tool": "get_entity", "result": ok(
                "GameObject 51 \"Resume\": button. selectable: {navigable: true, navigation: {down: Options}}",
                {"id": 51, "selectable": {"navigable": True, "navigation": {"up": None, "down": {"id": 53, "path": "PauseMenu/Options"}}}})},
        ],
        "checks": {"calls": [{"tool": "create_ui", "args": {"root": {"children": [{"select_on_enable": True}]}}}],
                   "must_not_call": ["create_ui_document"]},
        "max_turns": 12,
    },
    {
        "id": "guard_ui_button_color_refusal",
        "kind": "guard",
        "what_happened": "An edit of a Button's ui.color reported success and was lost at its next state change; "
                         "set_properties now refuses it and names the Button field (Doriax 88198667b, ui-components).",
        "text_at_issue": "McpValues::appendChangeCommands: 'UIComponent.color of ... is set by its Button (transition ...): "
                         "set button.colorNormal ...'",
        "source": "Doriax 88198667b; spec 2026-11-04-ui-components decision 13",
        "prompt": "Make the Play button (GameObject 12 in the open scene) red.",
        "mocks": [
            {"tool": "set_properties", "args": {"component": "re:^ui$|UIComponent"}, "result": err(
                "UIComponent.color of GameObject 12 \"Play\" is set by its Button (transition color_tint_and_sprite_swap): set "
                "button.colorNormal (and colorHovered, colorPressed, colorSelected, colorDisabled), or set button.transition "
                "to sprite_swap or none first.")},
            {"tool": "set_properties", "result": ok("Set button.colorNormal on GameObject 12 \"Play\" (one undo step).",
                                                    {"entities": [{"id": 12}]})},
            {"tool": "get_entity", "result": ok("GameObject 12 \"Play\": recttransform, ui, image, button.",
                                               {"id": 12, "components": ["recttransform", "ui", "image", "button"]})},
        ],
        "checks": {"any_of": [{"calls": [{"tool": "set_properties", "args": {"component": "button"}}]},
                              {"calls": [{"tool": "set_properties", "args": {"values": {"button": "*"}}}]},
                              {"calls": [{"tool": "set_properties", "args": {"changes": [{"component": "button"}]}}]}],
                   "max_calls": {"set_properties": 3}},
        "max_turns": 10,
    },
    {
        "id": "guard_copied_scene_ids",
        "kind": "guard",
        "what_happened": "A copied .scene used to keep its original's scene id; copies get their own now, and the result says "
                         "which (copied_scenes): an agent wiring the copy as a child scene needs that id.",
        "text_at_issue": "move_asset result: copied_scenes [{path, scene_id, name}]",
        "source": "scene-integrity spec (Doriax e0a38fe33)",
        "prompt": "Copy the folder levels to levels_v2 with the editor, then tell me the scene id and the scene name the copied "
                  "Forest scene got.",
        "mocks": [
            {"tool": "move_asset", "args": {"copy": True}, "result": ok(
                "Copied levels to levels_v2 (new GUIDs).",
                {"from": "levels", "to": "levels_v2", "copy": True, "files_written": ["levels_v2/Forest.scene"],
                 "copied_scenes": [{"path": "levels_v2/Forest.scene", "scene_id": 2718281828, "name": "Forest 1"}]})},
        ],
        "checks": {"calls": [{"tool": "move_asset", "args": {"copy": True}}], "reply_must": [r"2718281828", r"Forest 1"]},
        "max_turns": 8,
    },
    {
        "id": "guard_scene_view_after_load",
        "kind": "guard",
        "playing": True,
        "what_happened": "After the game loads another scene, the Scene view draws that scene (from the editor camera); "
                         "get_play_state says which scene it draws (scene_view_scene_id).",
        "text_at_issue": "get_play_state result: scene_view_scene_id",
        "source": "scene-integrity spec (Doriax 1e6ef8baa)",
        "prompt": "The game is playing and just loaded another level. Which scene does the editor's Scene view show now? Answer "
                  "with the scene's name.",
        "mocks": [
            {"tool": "get_play_state", "result": ok(
                "Play state: playing (\"Menu\"). Session: 12 s, 0 error(s) logged. Script coroutines: 0, timers: 0.",
                {"state": "playing", "scene": {"id": 1, "name": "Menu"},
                 "running_scenes": [{"id": 3001, "name": "Menu", "main": False}, {"id": 3002, "name": "Arena", "main": True}],
                 "scene_view_scene_id": 3002})},
        ],
        "checks": {"calls": ["get_play_state"], "reply_must": [r"Arena"]},
        "max_turns": 6,
    },
    {
        "id": "guard_game_cursor_released",
        "kind": "guard",
        "playing": True,
        "what_happened": "A game that captures the cursor gives it back to the editor on Escape in the Game view; "
                         "get_play_state says whether it did (input.cursor).",
        "text_at_issue": "get_play_state result: input.cursor {mode, released_to_editor}",
        "source": "scene-integrity spec (Doriax e5d7aad14)",
        "prompt": "The game is playing. Does the game hold the mouse cursor right now, or has it been given back to the editor?",
        "mocks": [
            {"tool": "get_play_state", "result": ok(
                "Play state: playing (\"Main\"). Session: 30 s, 0 error(s) logged. Script coroutines: 0, timers: 0.",
                {"state": "playing", "scene": {"id": 1, "name": "Main"},
                 "input": {"actions": {}, "touches": 0, "keys_held": [],
                           "cursor": {"mode": "captured", "released_to_editor": True}}})},
        ],
        "checks": {"calls": ["get_play_state"], "reply_must": [r"(?i)(given back|released|editor (has|holds))"]},
        "max_turns": 6,
    },
    {
        "id": "guard_model_parts_mesh",
        "kind": "guard",
        "what_happened": "A glTF with several mesh nodes keeps its geometry on child parts; an agent turned shadows off on the "
                         "model's own (empty) mesh and reported success. set_properties now warns and names the parts.",
        "text_at_issue": "set_properties result warning: '\"Robot\" draws 2 part(s) (Robot/Body, Robot/Eyes): set the mesh "
                         "on them, or use the Model API in Lua'",
        "source": "Doriax cc550567a; docs/superpowers/specs/2026-11-04-scripting-design.md 5.9",
        "prompt": "The Robot model (GameObject 1003) should not cast shadows. Turn its shadow casting off.",
        "mocks": [
            {"tool": "set_properties", "args": {"entity": "re:^(1003|Robot)$"}, "result": ok(
                "Set 1 property on 1 GameObject(s) (one undo step). New values: {\"mesh\":{\"castShadows\":false}} Warning: "
                "\"Robot\" draws 2 part(s) (Robot/Body, Robot/Eyes): set the mesh on them, or use the Model API in Lua.",
                {"scene_id": 1, "entities": [{"id": 1003, "path": "Robot"}], "values": {"mesh": {"castShadows": False}},
                 "warnings": ["\"Robot\" draws 2 part(s) (Robot/Body, Robot/Eyes): set the mesh on them, or use the Model API in Lua"]})},
            {"tool": "set_properties", "result": ok(
                "Set 1 property on 1 GameObject(s) (one undo step). New values: {\"mesh\":{\"castShadows\":false}}",
                {"scene_id": 1, "entities": [{"id": 1004, "path": "Robot/Body"}], "values": {"mesh": {"castShadows": False}}})},
        ],
        "checks": {"calls": [{"tool": "set_properties", "args": {"entity": "re:Body|1004"}},
                             {"tool": "set_properties", "args": {"entity": "re:Eyes|1005"}}]},
        "max_turns": 12,
    },
    {
        "id": "guard_cpp_lua_only_static",
        "kind": "guard",
        "what_happened": "Completion and the API search offered Engine::deltatime to a C++ script: it is a Lua property backed by "
                         "Engine::getDeltatime(), and the C++ does not compile. search_engine_api now marks it cpp_callable false.",
        "text_at_issue": "search_engine_api result: cpp_callable false, cpp_note 'Lua only. C++: Engine::getDeltatime()'",
        "source": "Doriax 755972f92; docs/superpowers/specs/2026-11-04-scripting-design.md 5.12",
        "prompt": "In scripts/Mover.cpp, make Mover::update move the GameObject along x by speed times the frame's delta time. "
                  "Check the engine API name first.",
        "files": {
            "scripts/Mover.h": "#pragma once\n#include \"ScriptBase.h\"\n\nclass Mover : public jasperengine::ScriptBase {\n"
                               "public:\n    float speed = 2.0f;\n    void update();\n};\n",
            "scripts/Mover.cpp": "#include \"Mover.h\"\n#include \"Engine.h\"\n\nvoid Mover::update() {\n}\n",
        },
        "mocks": [
            {"tool": "search_engine_api", "result": ok(
                "API symbols for \"deltatime\":\n- Engine.deltatime [Constant]\n- Engine.getDeltatime() -> float [Function]",
                {"results": [{"detail": "Engine.deltatime", "kind": "Constant", "cpp_callable": False,
                              "cpp_note": "Lua only. C++: Engine::getDeltatime()"},
                             {"detail": "Engine.getDeltatime() -> float", "kind": "Function"}], "docs": []})},
        ],
        "checks": {"file_must": {"scripts/Mover.cpp": [r"Engine::getDeltatime\(\)"]},
                   "file_must_not": {"scripts/Mover.cpp": [r"Engine::deltatime\b"]}},
        "max_turns": 14,
    },
    {
        "id": "guard_lua_api_hierarchy_and_defaults",
        "kind": "guard",
        "what_happened": "Lua had no way to read a GameObject's parent or children, and an omitted Rigidbody2D wake read as false; "
                         "the Lua API guide now lists Unity's hierarchy members and the wake default.",
        "text_at_issue": "docs/mcp/guide/lua-api.md 4.2 (parent, setParent, childCount, getChild, findChild) and 4.13 (wake "
                         "defaults to true)",
        "source": "docs/superpowers/specs/2026-11-04-scripting-design.md 6.4.2, 6.4.4",
        "prompt": "Going by the editor's Lua API guide: in a Lua script, how do I get my GameObject's parent and its first child, "
                  "and does rb2d:applyForceToCenter(f) without a second argument wake a sleeping body? Answer in a few lines, "
                  "change nothing.",
        "checks": {"must_not_call": ["write_file", "eval_lua"],
                   "reply_must": [r"\.parent\b", r"getChild\(\s*0\s*\)", r"wak(e|es)"],
                   "reply_must_not": [r"(stays?|remains?|left) asleep", r"does(n.?t| not) wake"]},
        "max_turns": 10,
    },
    {
        "id": "guard_lua_api_model_parts",
        "kind": "guard",
        "what_happened": "On a model imported from a glTF with several mesh nodes, Mesh/Model calls changed the root's empty mesh; "
                         "they now reach the parts, and the Lua API guide says how.",
        "text_at_issue": "docs/mcp/guide/lua-api.md 4.8 'A model made of parts'",
        "source": "Doriax cc550567a; docs/superpowers/specs/2026-11-04-scripting-design.md 5.8",
        "prompt": "Going by the editor's Lua API guide: on a Model loaded from a glTF with several mesh nodes, does "
                  "model:setColor(1, 0, 0) tint every part, and which part does model:getMaterial(2) read? Answer in a few lines, "
                  "change nothing.",
        "checks": {"must_not_call": ["write_file", "eval_lua"],
                   "reply_must": [r"(each|every|all( the)?) parts?", r"(first|0) submesh|submesh 0", r"(across|part after part|counts?)"]},
        "max_turns": 10,
    },

    # -- agent-authoring: building scenes, the guide past the client cut, terrain, shaders ------------
    {
        "id": "bad_repeat_without_prefab",
        "kind": "bad",
        "what_happened": "Agents placed repeated objects (coins, enemies) as independent copies, so a later change had to be "
                         "repeated on each; Doriax a48d1ce3a added 'build once, place instances' to its guidance.",
        "text_at_issue": "Guide 'Building scenes': 'What repeats is one Prefab (create_prefab)'; duplicate_entities' next",
        "source": "Doriax a48d1ce3a (editor AI authoring rules), agent-authoring spec",
        "prompt": "Add a Coin (a small yellow sphere) to the Main scene and four more coins 3 units apart along x.",
        "mocks": [
            {"tool": "create_entity", "result": ok("Created Sphere \"Coin\" [30] at Coin (one undo step).",
                                                   {"entity_id": 30, "entity": {"id": 30, "name": "Coin", "path": "Coin"},
                                                    "undo": {"id": 4, "name": "eval: Create GameObject (Coin)"}})},
            {"tool": "set_properties", "result": ok("Set 1 property of GameObject 30 \"Coin\".", {"entity": {"id": 30}})},
            {"tool": "create_prefab", "result": ok("Created prefab assets/prefabs/Coin.prefab from \"Coin\"; it is now an instance.",
                                                   {"path": "assets/prefabs/Coin.prefab", "instance": {"id": 30, "path": "Coin"}})},
            {"tool": "instantiate_prefab", "result": ok("Placed an instance of assets/prefabs/Coin.prefab: \"Coin (1)\" [31].",
                                                        {"entity": {"id": 31, "path": "Coin (1)"}})},
            {"tool": "duplicate_entities", "result": ok(
                "Duplicated 1 GameObject(s) (one undo step); the copies are selected. The copies are independent GameObjects: for "
                "an object placed more than once, make it a Prefab (create_prefab) and place instances (instantiate_prefab), so "
                "one edit changes every copy.",
                {"scene_id": 1, "created": [{"id": 31, "path": "Coin (1)"}],
                 "next": "The copies are independent GameObjects: for an object placed more than once, make it a Prefab "
                         "(create_prefab) and place instances (instantiate_prefab), so one edit changes every copy."})},
            {"tool": "set_transform", "result": ok("Moved 1 GameObject.", {"entities": [{"id": 31}]})},
            {"tool": "save_scene", "result": ok("Saved scene 1 \"Main\" (scenes/Main.scene).", {"scene": {"id": 1}})},
        ],
        "checks": {"calls": ["create_prefab"], "max_calls": {"create_entity": 2}},
        "max_turns": 24,
    },
    {
        "id": "bad_bob_in_script",
        "kind": "bad",
        "what_happened": "Asked for a bobbing pickup, agents wrote an onUpdate script that moves the transform every frame, "
                         "which does not preview in the editor or show in the Animation window.",
        "text_at_issue": "Guide 'Building scenes': 'motion is edit_animation, not per-frame code'",
        "source": "Doriax a48d1ce3a (animate natively), agent-authoring spec",
        "files": GAME_FILES,
        "prompt": "Make the Crate bob up and down by half a unit, forever.",
        "mocks": [
            {"tool": "edit_animation", "result": ok(
                "Created Animation \"Crate Bob\" [40] targeting Crate (1 tween, loop, autoplay; one undo step).",
                {"animation": {"id": 40, "path": "Crate Bob"}, "undo": {"id": 5, "name": "eval: Edit Animation"}})},
            {"tool": "preview_animation", "result": ok("Previewing \"Crate Bob\" at 0.5 s.", {"time": 0.5})},
            {"tool": "save_scene", "result": ok("Saved scene 1 \"Main\" (scenes/Main.scene).", {"scene": {"id": 1}})},
        ],
        "checks": {"calls": ["edit_animation"], "must_not_call": ["create_script", "attach_script"],
                   "edits_must_not": [r"\.lua$", r"\.cpp$"]},
        "max_turns": 16,
    },
    {
        "id": "bad_hud_widgets_in_3d",
        "kind": "bad",
        "what_happened": "A score label was built with create_ui inside the 3D level, where it sits in the world and moves with "
                         "the camera; the result now says where a HUD goes.",
        "text_at_issue": "create_ui result next: 'In a 3D scene UI widgets sit in its world ... set_child_scene, or ... "
                         "create_ui_document'",
        "source": "Doriax a48d1ce3a (HUD in its own UI scene); agent-authoring spec",
        "prompt": "Show the player's score in the top-left corner of the screen while the game runs (\"Score: 0\" for now).",
        "mocks": [
            {"tool": "create_ui", "args": {"scene": "re:^(1|Main|scenes/Main.scene)$"}, "result": ok(
                "Created 1 UI element(s) under \"Score\" (one undo step). In a 3D scene UI widgets sit in its world and move "
                "with its camera: build a HUD or menu in a UI scene attached with set_child_scene, or as an HTML UI document "
                "(create_ui_document).",
                {"root": {"id": 50, "name": "Score"}, "count": 1,
                 "next": "In a 3D scene UI widgets sit in its world and move with its camera: build a HUD or menu in a UI scene "
                         "attached with set_child_scene, or as an HTML UI document (create_ui_document)."})},
            {"tool": "create_ui", "result": ok("Created 1 UI element(s) under \"Score\" (one undo step).",
                                               {"root": {"id": 3, "name": "Score"}, "count": 1})},
            {"tool": "create_scene", "result": ok("Created scene 2 \"Hud\" (UI) at scenes/Hud.scene.",
                                                  {"scene": {"id": 2, "name": "Hud", "path": "scenes/Hud.scene"}})},
            {"tool": "set_child_scene", "result": ok("Attached \"Hud\" as a child scene of \"Main\".",
                                                     {"scene": {"id": 1}, "child": {"id": 2}})},
            {"tool": "create_ui_document", "result": ok(
                "Created UI Document \"Hud\" [51] with assets/ui/hud.html, hud.css and hud.lua.",
                {"entity": {"id": 51, "path": "Hud"}, "files": ["assets/ui/hud.html", "assets/ui/hud.css", "assets/ui/hud.lua"]})},
            {"tool": "save_scene", "result": ok("Saved scene.", {"scene": {"id": 1}})},
        ],
        "checks": {"any_of": [{"calls": ["create_ui_document"]}, {"calls": ["set_child_scene"]}]},
        "max_turns": 24,
    },
    {
        "id": "guard_rotation_not_euler",
        "kind": "guard",
        "what_happened": "Euler degrees sent as a quaternion ([90, 0, 0, 1]) were stored as given and scaled the object; the "
                         "server now refuses them and names the Euler form.",
        "text_at_issue": "McpValues::parseQuaternion: '[90, 0, 0, 1] looks like Euler degrees: use {\"euler\": [90, 0, 0]}.'",
        "source": "Doriax 720e314e7; agent-authoring spec",
        "prompt": "Rotate the Crate 90 degrees around the X axis.",
        "mocks": [
            {"tool": "set_transform", "args": {"rotation": {"euler": "*"}}, "result": ok("Set the transform of 1 GameObject.",
                                                                                     {"entities": [{"id": 15}]})},
            {"tool": "set_transform", "result": err(
                "rotation: [90, 0, 0, 1] looks like Euler degrees: use {\"euler\": [90, 0, 0]}.")},
            {"tool": "set_properties", "args": {"values": {"transform": {"rotation": {"euler": "*"}}}},
             "result": ok("Set 1 property of GameObject 15 \"Crate\".", {"entity": {"id": 15}})},
            {"tool": "set_properties", "result": err(
                "transform.rotation on GameObject 15 \"Crate\": Property rotation is quat: [90, 0, 0, 1] looks like Euler "
                "degrees: use {\"euler\": [90, 0, 0]}.")},
        ],
        "checks": {"any_of": [{"calls": [{"tool": "set_transform", "args": {"rotation": {"euler": "*"}}}]},
                              {"calls": [{"tool": "set_properties", "args": {"values": {"transform": {"rotation": {"euler": "*"}}}}}]}],
                   "max_errors": {"*": 1}},
        "max_turns": 10,
    },
    {
        "id": "guard_guide_past_the_cut",
        "kind": "guard",
        "what_happened": "Claude Code keeps the first 4096 characters of the server instructions; the physics, prefab, shader "
                         "and HTML UI digests came after the cut. The intro now says to read the rest.",
        "text_at_issue": "Guide intro: 'Text cut before \"## Other areas\"? read_file jasper://guide.'",
        "source": "Claude Code 2.1.318 capMcpInstructions (4096 characters); agent-authoring spec",
        "prompt": "Before you change anything: what does this editor's agent guide say about moving physics bodies? Quote it.",
        "mocks": [
            {"tool": "read_file", "args": {"path": "re:^jasper://guide"}, "result": ok(
                "jasper://guide lines 1-120 of 120:\n...\n## Physics\nAdd a body and its collider in one step: edit_colliders "
                "with body_type + add (a Rigidbody without a collider does not collide). Collider sizes are local: never "
                "pre-scale or guess them; omit them to fit the mesh. Forces go in onFixedUpdate, not times deltatime; move a "
                "body through the body, not its transform (jasper://docs/physics).\n...",
                {"path": "jasper://guide", "total_lines": 120, "start_line": 1, "lines": 120})},
            {"tool": "read_file", "args": {"path": "re:^jasper://docs/physics"}, "result": ok(
                "jasper://docs/physics lines 1-40 of 400:\n# Physics\n...\nMove a body through body.position / "
                "body.linearVelocity, never its transform.", {"path": "jasper://docs/physics", "total_lines": 400})},
        ],
        "checks": {"any_of": [{"calls": [{"tool": "read_file", "args": {"path": "re:^jasper://guide"}}]},
                              {"calls": [{"tool": "read_file", "args": {"path": "re:^jasper://docs/physics"}}]}],
                   "reply_must": [r"(?i)transform"]},
        "max_turns": 8,
    },
    {
        "id": "bad_terrain_grass",
        "kind": "bad",
        "what_happened": "Asked for grass on a terrain, agents set foliageLayers through set_properties with scene-file keys "
                         "and no density map, so nothing drew; edit_terrain_details validates and makes the map.",
        "text_at_issue": "edit_terrain_details: 'A layer draws nothing without a density map: density_map {fill: 0-1}'",
        "source": "Doriax b2375ea0c (set_terrain_foliage_layer); agent-authoring spec",
        "prompt": "Cover the terrain Ground with grass using assets/models/grass.glb. It's only decoration, keep it cheap.",
        "groups_enabled": ["terrain"],
        "mocks": [
            {"tool": "edit_terrain_details", "args": {"add": "*"}, "result": ok(
                "Set the detail layers of Ground (one undo step): 1 layer(s).",
                {"terrain": {"id": 11, "path": "Ground"}, "added": [0], "layers": [{"index": 0, "mesh": "assets/models/grass.glb",
                 "density_map": "assets/terrain/terrain_1_11_density.png", "density": 2, "cast_shadows": False}]})},
            {"tool": "edit_terrain_details", "result": ok("Ground has 0 detail layer(s).", {"terrain": {"id": 11}, "layers": []})},
            {"tool": "sample_terrain", "result": ok("Terrain Ground: 32 x 32, max height 4; 1 detail layer (8400 drawn).",
                                                    {"info": {"details": [{"index": 0, "drawn": 8400}]}})},
        ],
        "checks": {"calls": [{"tool": "edit_terrain_details", "args": {"add": [{"density_map": "*"}]}}],
                   "must_not_call": [{"tool": "set_properties", "args": {"values": {"terrain": {"foliageLayers": "*"}}}}]},
        "max_turns": 16,
    },
    {
        "id": "guard_detail_layer_error_names_the_fix",
        "kind": "guard",
        "what_happened": "A detail layer whose density map had only a GPU-only cook drew nothing; sample_terrain showed drawn 0 and "
                         "nothing else, so agents repainted or replaced the layer. The layer's error names the map and the fix.",
        "text_at_issue": "sample_terrain details[i].error (MeshSystem foliage): 'its density map ... has no pixels the CPU can read: "
                         "no CPU-readable cook of it arrived; reimport it (reimport_textures)'",
        "source": "agent-authoring follow-ups (review of 767dfb9bb, item 3)",
        "prompt": "The grass on the terrain Ground stopped showing. Find out why and fix it; don't change the layer.",
        "groups_enabled": ["terrain", "assets_pipeline"],
        "mocks": [
            {"tool": "sample_terrain", "result": ok(
                "Terrain Ground: 32 x 32, max height 4; 1 detail layer (0 drawn).",
                {"info": {"size": 32, "mesh_built": True, "details": [{
                    "index": 0, "mesh": "assets/models/grass.glb", "density_map": "assets/terrain/painted.png", "density": 2,
                    "drawn": 0, "error": "its density map assets/terrain/painted.png has no pixels the CPU can read: no CPU-readable "
                                         "cook of it arrived; reimport it (reimport_textures)"}]}})},
            {"tool": "reimport_textures", "result": ok(
                "Reimported 1 texture(s).",
                {"textures": [{"path": "assets/terrain/painted.png", "platforms": {"desktop": {"status": "cooked", "cpu_readable": True,
                                                                                            "format": "R8"}}}], "timed_out": False})},
            {"tool": "edit_terrain_details", "result": ok("Ground has 1 detail layer(s).", {"terrain": {"id": 11}, "layers": [
                {"index": 0, "mesh": "assets/models/grass.glb", "density_map": "assets/terrain/painted.png", "density": 2}]})},
        ],
        "checks": {"calls": [{"tool": "sample_terrain"}, {"tool": "reimport_textures", "args": {"paths": ["assets/terrain/painted.png"]}}],
                   "order": [["sample_terrain", "reimport_textures"]],
                   "must_not_call": [{"tool": "edit_terrain_details", "args": {"update": "*"}},
                                     {"tool": "edit_terrain_details", "args": {"remove": "*"}},
                                     {"tool": "sculpt_terrain"}, "write_file"]},
        "max_turns": 14,
    },
    {
        "id": "bad_shader_override_by_building",
        "kind": "bad",
        "what_happened": "Shader overrides could only change through build_project, so agents started a build to exclude one "
                         "shader variant; edit_build_shaders edits them without building.",
        "text_at_issue": "edit_build_shaders: 'Build Settings > Shaders without building'",
        "source": "Doriax b2323b44b (set_export_shaders); agent-authoring spec",
        "prompt": "Exclude the shader variant 0x0000000a00000001 from the game's builds. Don't build anything now.",
        "mocks": [
            {"tool": "edit_build_shaders", "result": ok(
                "Shader overrides set (one project undo step, project.yaml saved): 0 added, 1 excluded besides the scenes' own shaders.",
                {"added": [], "excluded": [{"key": "0x0000000a00000001", "name": "Mesh (Ult)"}]})},
            {"tool": "get_build_info", "result": ok("Host linux. 1 scene(s) in the build, 12 shader(s).",
                                                    {"shader_count": 12, "cpp_scripts": {"can_build": True, "missing_tools": ""}})},
        ],
        "checks": {"calls": [{"tool": "edit_build_shaders", "args": {"exclude": ["0x0000000a00000001"]}}],
                   "must_not_call": ["build_project"]},
        "max_turns": 12,
    },
    {
        "id": "bad_cpp_without_toolchain",
        "kind": "bad",
        "what_happened": "On Windows get_build_info said the desktop toolchain was there while Play refused C++ scripts (it "
                         "checks for a compiler compatible with the editor); agents wrote C++ that never built.",
        "text_at_issue": "get_build_info's result: cpp_scripts {can_build, missing_tools} (Play's own check); cpp-scripting.md 'Choosing Lua or C++'",
        "source": "Doriax 3fc70298e (check_cpp_build_tools, language guidance); agent-authoring spec",
        "prompt": "Make the Crate spin around Y with a C++ script attached to it. Check first that C++ can build on this machine.",
        "mocks": [
            {"tool": "get_build_info", "result": ok(
                "Host windows. Desktop player: available; Web: no emsdk; source export: available. 1 scene(s) in the build, 9 shader(s).",
                {"host_platform": "windows", "desktop": {"available": True, "missing_tools": ""},
                 "cpp_scripts": {"can_build": False, "missing_tools": "- C++ toolchain compatible with this editor: not found. "
                                 "Select an available compiler in Preferences > External Tools.\n",
                                 "kit": {"c_compiler": "", "cxx_compiler": "", "generator": ""}}})},
            {"tool": "compile_scripts", "result": err("C++ build tools are missing:\n- C++ toolchain compatible with this editor: "
                                                      "not found. Select an available compiler in Preferences > External Tools.")},
            {"tool": "create_script", "result": ok("Created scripts/Spinner.lua and attached it to Crate.",
                                                   {"path": "scripts/Spinner.lua", "attached": {"id": 15}})},
        ],
        "checks": {"calls": ["get_build_info"], "reply_must": [r"(?i)(compiler|toolchain|build tools)"]},
        "max_turns": 16,
    },
    {
        "id": "guard_pixel_perfect_camera",
        "kind": "guard",
        "what_happened": "Pixel art drawn at a fractional scale (columns 2 and 3 px wide, blur): the fix is the camera's Pixel "
                         "Perfect Camera component (S41), not a post-processing pixelate, a shader fork or the scene's Fixed "
                         "Resolution.",
        "text_at_issue": "None since the guide's 'pixel art' line was cut (it passed without it): the component's name in list_component_types",
        "source": "S41 (docs/specs/S41-pixel-perfect-camera.md R3, the @asynkimo failure); no failure on record",
        "prompt": "My pixel-art game looks wrong at 1366x768: some pixel columns are wider than others and sprites shimmer "
                  "when the camera moves. Make the Main Camera pixel perfect at 320x180.",
        "mocks": [
            {"tool": "find_entities", "result": ok("1 GameObject(s) match:\nMain Camera [3] camera transform",
                                                   {"entities": [{"id": 3, "path": "Main Camera"}]})},
            {"tool": "get_entity", "args": {"entity": "re:Main Camera|^3$"}, "result": ok(
                "Main Camera [3] components: camera transform; camera: type ortho", {"id": 3, "path": "Main Camera"})},
            {"tool": "add_component", "result": ok("Added pixelperfectcamera to GameObject 3 \"Main Camera\" (one undo step).",
                                                   {"entity": {"id": 3, "path": "Main Camera"}, "component": "pixelperfectcamera"})},
            {"tool": "set_properties", "result": ok("Set 2 properties on GameObject 3 \"Main Camera\".", {"entity": {"id": 3}})},
            {"tool": "save_scene", "result": ok("Saved scene 1 \"Main\" (scenes/Main.scene).", {"scene": {"id": 1}})},
        ],
        "checks": {"calls": [{"tool": "add_component", "args": {"component": "re:(?i)pixel.?perfect"}}],
                   "must_not_call": ["fork_shader", "apply_post_processing_preset",
                                     {"tool": "set_scene_settings", "args": {"settings": {"fixed_resolution_enabled": True}}}]},
        "max_turns": 16,
    },
    {
        "id": "guard_pixel_font_preview",
        "kind": "guard",
        "groups_enabled": ["pixel_art"],
        "what_happened": "Asked whether a bitmap font covers a string and how it looks, an agent should draw the string with "
                         "preview_font text at scale (its result lists the missing characters) instead of building a scene, "
                         "playing and taking screenshots (S51).",
        "text_at_issue": "preview_font description: '`text` at a whole `scale` ... with the missing characters listed'; its text and scale parameters",
        "source": "S51 (docs/specs/S51-pixel-fonts.md R26, AC16); no failure on record",
        "files": {"assets/fonts/hud.bitmapfont": "{\"format\": \"jasperengine.bitmapfont\", \"version\": 1, "
                                                 "\"source\": {\"type\": \"grid\", \"image\": \"hud@6x9.png\"}}"},
        "prompt": "Does assets/fonts/hud.bitmapfont have every character of 'SCORE 120 \u00a1Ol\u00e9!'? Show me that "
                  "string in it, three times its size.",
        "mocks": [
            {"tool": "preview_font", "args": {"path": "re:hud\\.bitmapfont"}, "result": ok(
                "assets/fonts/hud.bitmapfont: the text at x3 (78x10 font pixels), white on a dark checker. Missing (drawn as "
                "the fallback glyph): \u00a1\u00e9.",
                {"font": {"kind": "bitmap", "native_size": 9, "line_height": 10, "baseline": 7, "glyphs": 95, "colored": False,
                          "scale": 3, "width": 242, "height": 38, "missing": ["\u00a1", "\u00e9"]}})},
            {"tool": "get_asset_info", "args": {"path": "re:hud\\.bitmapfont"}, "result": ok(
                "assets/fonts/hud.bitmapfont: font, a bitmap font: 95 glyphs, 9 px (line 10, baseline 7).",
                {"path": "assets/fonts/hud.bitmapfont", "type": "font", "font": {"kind": "bitmap", "glyphs": 95,
                 "native_size": 9, "missing_ascii": ""}})},
        ],
        "checks": {"calls": [{"tool": "preview_font", "args": {"path": "re:hud\\.bitmapfont", "text": "re:SCORE", "scale": 3}}],
                   "must_not_call": ["play", "create_entity"], "reply_must": [r"(?i)\u00e9|missing|lack"]},
        "max_turns": 8,
    },
    {
        "id": "guard_bitmap_font_write_report",
        "kind": "guard",
        "what_happened": "Making a pixel font from a sheet, an agent should write the .bitmapfont with write_file and read the "
                         "import report it returns (glyphs, typed errors with the field to fix), not hand-edit a scene's "
                         "font or guess (S51).",
        "text_at_issue": "write_file result: a .bitmapfont's import report (font, errors with their field)",
        "source": "S51 (docs/specs/S51-pixel-fonts.md R28, AC17); no failure on record",
        "files": {"assets/fonts/hud@6x9.png": "binary placeholder"},
        "prompt": "Turn assets/fonts/hud@6x9.png (printable ASCII on a 6x9 grid) into a bitmap font and tell me if it imported "
                  "cleanly.",
        "mocks": [
            {"tool": "write_file", "args": {"path": "re:\\.bitmapfont$"}, "result": ok(
                "Created assets/fonts/hud.bitmapfont (120 bytes; undo with scope project). Bitmap font: 95 glyphs, 9 px, line "
                "10, baseline 7.",
                {"path": "assets/fonts/hud.bitmapfont", "created": True, "font": {"glyphs": 95, "native_size": 9,
                 "missing_ascii": "", "partial_alpha_share": 0.0}})},
            {"tool": "get_asset_info", "args": {"path": "re:\\.bitmapfont$"}, "result": ok(
                "assets/fonts/hud.bitmapfont: font, a bitmap font: 95 glyphs, 9 px (line 10, baseline 7); no import errors.",
                {"path": "assets/fonts/hud.bitmapfont", "type": "font", "font": {"kind": "bitmap", "glyphs": 95, "native_size": 9}})},
            {"tool": "get_asset_info", "result": ok("assets/fonts/hud@6x9.png: texture 96x54, RGBA.", {"path": "assets/fonts/hud@6x9.png", "type": "texture"})},
            {"tool": "preview_font", "result": ok("Preview of assets/fonts/hud.bitmapfont at scale 1: 0 missing characters.", {"missing": []})},
        ],
        "checks": {"calls": [{"tool": "write_file", "args": {"path": "re:\\.bitmapfont$"}}],
                   "shell_must_not": [r"python", r"convert\s"], "reply_must": [r"95|clean|glyph"]},
        "max_turns": 12,
    },
    {
        "id": "guard_font_metrics_info",
        "kind": "guard",
        "groups_enabled": ["pixel_art"],
        "what_happened": "Asked for a pixel font's metrics, an agent should read them from get_asset_info (native size, "
                         "baseline, kerning, pixel report) instead of opening the sheet or the JSON, or drawing it, and measuring (S51).",
        "text_at_issue": "preview_font description: 'Metrics, kerning, pixel report: get_asset_info'",
        "source": "S51 (docs/specs/S51-pixel-fonts.md R27, AC17); no failure on record",
        "files": {"assets/fonts/hud.fnt": "info face=\"Hud\" size=9\ncommon lineHeight=10 base=7\n"},
        "prompt": "What is the native pixel size and baseline of assets/fonts/hud.fnt, and does it have antialiased pixels?",
        "mocks": [
            {"tool": "get_asset_info", "args": {"path": "re:hud\\.fnt"}, "result": ok(
                "assets/fonts/hud.fnt: font, a bitmap font: 95 glyphs, 9 px (line 10, baseline 7). Pixel report: not antialiased (0% partial alpha).",
                {"path": "assets/fonts/hud.fnt", "type": "font", "font": {"kind": "bitmap", "native_size": 9, "baseline": 7},
                 "pixel_report": {"antialiased": False, "partial_alpha_share": 0.0, "native_size": 9}})},
        ],
        "checks": {"calls": [{"tool": "get_asset_info", "args": {"path": "re:hud\\.fnt"}}],
                   "must_not_call": ["read_file", "view_asset", "preview_font"], "reply_must": [r"9", r"7"]},
        "max_turns": 10,
    },
    {
        "id": "guard_game_view_pixel_scale",
        "kind": "guard",
        "what_happened": "Checking pixel art at 1x (one physical pixel per art pixel) is the Game view's Pixel Scale, not its "
                         "zoom Scale (1 to 5, the image fitted first) or a resolution change.",
        "text_at_issue": "configure_game_view pixel_scale: 'Pixel Perfect scale; 0 auto.'",
        "source": "S41 (R20, AC15); no failure on record",
        "prompt": "The main camera has a Pixel Perfect Camera. Show the Game view at 1x pixel scale so I can check the art pixel "
                  "for pixel, then take a Game view screenshot.",
        "mocks": [
            {"tool": "configure_game_view", "args": {"pixel_scale": 1}, "result": ok(
                "Game view: aspect free, scale 1, pixel scale 1.", {"aspect": "free", "scale": 1, "pixel_scale": 1})},
            {"tool": "configure_game_view", "result": ok("Game view: aspect free, scale 1, pixel scale 0.",
                                                         {"aspect": "free", "scale": 1, "pixel_scale": 0})},
            {"tool": "screenshot", "result": ok("Game view 1280x720 (scaled to 768x432).",
                                               {"view": "game", "width": 1280, "height": 720})},
        ],
        "checks": {"calls": [{"tool": "configure_game_view", "args": {"pixel_scale": 1}}],
                   "must_not_call": [{"tool": "configure_game_view", "args": {"resolution": "*"}}]},
        "max_turns": 12,
    },
    # -- S42 the pixel-art validator: the pixel_art group's description, the tool's preview-then-output and `next` -------
    {
        "id": "guard_pixel_art_check_route",
        "kind": "guard",
        "what_happened": "New tool (S42): blurry, uneven pixel art is check_pixel_art's to measure and fix (one call to "
                         "preview, the same with output: true to apply); without the group's description and the result's `next` "
                         "an agent edits import settings and filters by hand, or stops at the preview.",
        "text_at_issue": "tool_groups' pixel_art: 'Pixel art: checks, lints, cleanup, palettes, fonts (jasper://docs/pixel-art)'; check_pixel_art: 'fix previews the changes; "
                         "the same call with output:true applies them as one undo step'; the results' `next`",
        "source": "S42 R22 (new tool text); no failure on record",
        "prompt": "Our pixel-art game looks blurry and some sprite columns are wider than others on a 1366x768 laptop. "
                  "Find what is wrong with the pixel-art setup and fix all of it.",
        "mocks": [
            {"tool": "check_pixel_art", "after": {"tool": "check_pixel_art", "args": {"output": True}}, "result": ok(
                "Pixel art check: clean, 0 FAIL, 0 WARN (13 rules).", {"verdict": "clean", "counts": {"fail": 0, "warn": 0}})},
            {"tool": "get_project_settings", "after": {"tool": "check_pixel_art", "args": {"output": True}}, "result": ok(
                "pixel_art.enabled: true; pixel_art.upscale_mode: integer; graphics.default_filter_mode: point; graphics.scaling_mode: fit",
                {"settings": {"pixel_art.enabled": True, "pixel_art.upscale_mode": "integer", "graphics.default_filter_mode": "point"}})},
            {"tool": "check_pixel_art", "args": {"fix": ["all"], "output": True}, "result": ok(
                "Pixel art check: fixable, 2 FAIL, 1 WARN (13 rules). Applied 3 fix(es) as \"Fix Pixel Art (3)\"; after: "
                "clean, 0 FAIL, 0 WARN.",
                {"verdict": "fixable", "applied": {"undo_step": {"id": 9, "name": "Fix Pixel Art (3)"}, "changed": ["project"]},
                 "after": {"verdict": "clean", "counts": {"fail": 0, "warn": 0}}})},
            {"tool": "check_pixel_art", "args": {"fix": ["all"]}, "result": ok(
                "Pixel art check: fixable, 2 FAIL, 1 WARN (13 rules). Fix plan: 3 fix(es), 6 change(s); nothing changed. "
                "Preview only, nothing changed: the same call with output: true applies these 6 change(s) as one undo step.",
                {"verdict": "fixable", "fix_plan": [{"key": "filtering:scenes/Main.scene#/Hero:mesh.submeshes[0].material.baseColorTexture",
                                                     "option": "default", "label": "Set both filters of /Hero to Nearest", "changes": []}],
                 "next": "Preview only, nothing changed: the same call with output: true applies these 6 change(s) as one undo step."})},
            {"tool": "check_pixel_art", "result": ok(
                "Pixel art check: fixable, 2 FAIL, 1 WARN (13 rules). check_pixel_art {fix: [\"all\"]} previews 3 fix(es); "
                "add output: true to apply.",
                {"verdict": "fixable", "counts": {"fail": 2, "warn": 1, "fixable": 3, "needs_person": 0},
                 "findings": [{"id": "F1", "rule": "integer_scale", "status": "FAIL", "message": "Main scales its pixels by a fraction on some screens."},
                              {"id": "F2", "rule": "filtering", "status": "FAIL", "message": "hero.png is filtered bilinear on Hero."},
                              {"id": "F3", "rule": "pixel_art_mode", "status": "WARN", "message": "The project draws pixel art but Pixel Art Mode is off."}],
                 "next": "check_pixel_art {fix: [\"all\"]} previews 3 fix(es); add output: true to apply."})},
        ],
        "checks": {"calls": [{"tool": "check_pixel_art", "args": {"fix": ["all"], "output": True}}],
                   "must_not_call": ["set_texture_import_settings", "set_project_settings", "write_file"],
                   "edits_must_not": [r"\.(meta|scene|yaml)$"]},
        "max_turns": 12,
    },
    {
        "id": "guard_pixel_art_needs_person",
        "kind": "guard",
        "what_happened": "New tool (S42): a finding no automatic change fixes (art drawn at another texel size) has no fix and "
                         "needs a person; without the result's `next` an agent scales the sprite until the number passes.",
        "text_at_issue": "check_pixel_art's `next`: 'The findings need a person: report them (needs_person) instead of "
                         "changing scale or art.'",
        "source": "S42 R12, D5 (new tool text); no failure on record",
        "prompt": "Run the pixel-art check on our project and get it to pass.",
        "mocks": [
            {"tool": "check_pixel_art", "result": ok(
                "Pixel art check: needs_person, 1 FAIL, 0 WARN (13 rules). The findings need a person: report them (needs_person) "
                "instead of changing scale or art.",
                {"verdict": "needs_person", "counts": {"fail": 1, "warn": 0, "fixable": 0, "needs_person": 1},
                 "findings": [{"id": "F1", "key": "texel_size:scenes/Main.scene#/Boss", "rule": "texel_size", "status": "FAIL",
                               "measured": {"x": 1.5, "y": 1.5}, "expected": {"x": 1, "y": 1}, "fix": None,
                               "needs_person": "the art itself is not one texel per pixel at this size: fix the art or its Pixels "
                                               "Per Unit, never the scale",
                               "message": "Boss's pixels are 1.5 x 1.5 base pixels, not 1 x 1."}],
                 "next": "The findings need a person: report them (needs_person) instead of changing scale or art."})},
        ],
        "checks": {"must_not_call": ["set_transform", {"tool": "set_properties", "args": {"values": {"transform": "*"}}},
                                     {"tool": "check_pixel_art", "args": {"output": True}}]},
        "max_turns": 8,
    },
    # -- S52: 2D draw order (Sorting Layers through set_project_settings and set_properties), the unknown-layer
    # error, Sprite Batch for bulk sprites, the profiling guide's new batch breaks --------
    {
        "id": "guard_sorting_layer_route",
        "kind": "guard",
        "what_happened": "New area (S52): putting a 2D renderer in front of others is a Sorting Layer (the setting "
                         "tags_and_layers.sorting_layers, then the renderer's sortingLayer); without the setting and the doc agents "
                         "moved GameObjects in the hierarchy or changed z, which 2D cameras ignore.",
        "text_at_issue": "jasper://docs/rendering-2d; "
                         "tags_and_layers.sorting_layers: 'Sorting Layers in draw order, first drawn first, as [{id, name}]; must include "
                         "\"Default\"'",
        "source": "S52 R34, R36 (new MCP text); no failure on record",
        "prompt": "In our 2D scene the Tree GameObjects draw behind the Player sprite. Make every tree draw in front of the "
                  "player, using a proper foreground layer.",
        "mocks": [
            {"tool": "find_entities", "args": {"name": "re:(?i)player"}, "result": ok("1 GameObject(s) match:\nPlayer [20] sprite transform",
                                                  {"entities": [{"id": 20, "name": "Player"}]})},
            {"tool": "get_entity", "args": {"entity": "re:(?i)^(12|13|14|tree.*)$"}, "result": ok(
                "Tree [12] components: sprite transform; sprite: texture assets/sprites/tree.png; mesh: sortingLayer Default, sortingOrder 0", {"id": 12, "name": "Tree"})},
            {"tool": "get_entity", "args": {"entity": "re:(?i)^(20|player)$"}, "result": ok(
                "Player [20] components: sprite transform; sprite: texture assets/sprites/ship.png; mesh: sortingLayer Default, sortingOrder 0", {"id": 20, "name": "Player"})},
            {"tool": "read_file", "args": {"path": "re:rendering-2d"}, "result": ok(
                "# 2D draw order and Sprite Batch\n\nA 2D camera draws renderers by Sorting Layer (its place in "
                "tags_and_layers.sorting_layers, first drawn first), then Order in Layer, then the Hierarchy. Add layers with "
                "set_project_settings; set a renderer's with set_properties {\"sortingLayer\": \"Foreground\"}.")},
            {"tool": "find_entities", "result": ok("3 GameObjects: Tree (12), Tree (13), Tree (14).",
                                                  {"entities": [{"id": 12, "name": "Tree"}, {"id": 13, "name": "Tree"},
                                                                {"id": 14, "name": "Tree"}]})},
            {"tool": "get_project_settings", "result": ok("tags_and_layers.sorting_layers: [{\"id\": 0, \"name\": \"Default\"}]",
                                                         {"values": {"tags_and_layers.sorting_layers": [{"id": 0, "name": "Default"}]}})},
            {"tool": "set_project_settings", "result": ok("Set 1 project setting; saved project.yaml.",
                                                         {"changed": ["tags_and_layers.sorting_layers"]})},
            {"tool": "set_properties", "result": ok("Set 1 property on 3 GameObjects.", {"entities": [{"id": 12}, {"id": 13}, {"id": 14}]})},
            {"tool": "screenshot", "result": ok("Game view 1280x720 (scaled to 768x432).", {"view": "game"})},
            {"tool": "save_scene", "result": ok("Saved scene 1 \"Main\" (scenes/Main.scene).", {"scene": {"id": 1}})},
        ],
        "checks": {"calls": [{"tool": "set_project_settings", "args": {"values": {"tags_and_layers.sorting_layers": "*"}}},
                             {"tool": "set_properties"}],
                   "must_not_call": ["move_entity", {"tool": "set_transform"}]},
        "max_turns": 14,
    },
    {
        "id": "bad_unknown_sorting_layer",
        "kind": "bad",
        "what_happened": "Agents set sortingLayer: \"Foreground\" on a sprite before the project had that layer, then fell "
                         "back to Order in Layer alone; the error names the fix (S52).",
        "text_at_issue": "set_properties error: 'no Sorting Layer is named \"Foreground\"; add it with set_project_settings "
                         "tags_and_layers.sorting_layers'",
        "source": "S52 R34 (new error text); paraphrased from the design review",
        "prompt": "Put the Clouds sprite (GameObject 21) on a Sorting Layer named Foreground.",
        "mocks": [
            {"tool": "get_entity", "args": {"entity": "re:(?i)^(21|clouds)$"}, "result": ok(
                "Clouds [21] components: sprite transform; sprite: texture assets/sprites/clouds.png; mesh: sortingLayer Default, sortingOrder 0", {"id": 21, "name": "Clouds"})},
            {"tool": "set_properties", "after": "set_project_settings", "result": ok(
                "Set 1 property on GameObject 21 \"Clouds\".", {"entity": {"id": 21}})},
            {"tool": "set_properties", "result": err(
                "Property sortingLayer: no Sorting Layer is named \"Foreground\"; add it with set_project_settings "
                "tags_and_layers.sorting_layers")},
            {"tool": "get_project_settings", "result": ok("tags_and_layers.sorting_layers: [{\"id\": 0, \"name\": \"Default\"}]",
                                                         {"values": {"tags_and_layers.sorting_layers": [{"id": 0, "name": "Default"}]}})},
            {"tool": "set_project_settings", "result": ok("Set 1 project setting; saved project.yaml.",
                                                         {"changed": ["tags_and_layers.sorting_layers"]})},
        ],
        "checks": {"calls": [{"tool": "set_properties"}],
                   "any_of": [{"calls": [{"tool": "set_project_settings", "args": {"values": {"tags_and_layers.sorting_layers": ["*", "Foreground"]}}}]},
                              {"calls": [{"tool": "set_project_settings", "args": {"values": {"tags_and_layers.sorting_layers": ["*", {"name": "Foreground"}]}}}]}],
                   "must_not_call": [{"tool": "set_properties", "args": {"values": {"mesh": {"sortingOrder": "*"}}}}]},
        "max_turns": 10,
    },
    {
        "id": "bad_bullets_as_gameobjects",
        "kind": "bad",
        "what_happened": "Asked for a bullet-hell pattern of 2,000 bullets, agents created one GameObject per bullet (create_entity "
                         "in loops, Instantiate in Lua): 2,000 transforms updated every frame; a Sprite Batch draws them from one "
                         "GameObject with flat streams (S52).",
        "text_at_issue": "jasper://docs/rendering-2d 'Sprite Batch' (add_component spritebatch, SpriteBatch:setPositions)",
        "source": "S52 R10-R12, R36 (new MCP text); paraphrased from the design review",
        "files": {"scripts/spawner.lua": "-- empty spawner script\n"},
        "prompt": "Our boss (GameObject 30, which has a Bullet sprite child, GameObject 31) should fire a ring of 2000 bullets "
                  "that move outwards every frame. Write it in scripts/spawner.lua.",
        "mocks": [
            {"tool": "get_entity", "args": {"entity": "re:(?i)^(30|boss)$"}, "result": ok(
                "Boss [30] components: sprite transform; children: Bullet [31]", {"id": 30, "name": "Boss"})},
            {"tool": "get_entity", "args": {"entity": "re:(?i)^(31|bullet|boss/bullet)$"}, "result": ok(
                "Bullet [31] components: sprite transform; sprite: texture assets/sprites/bullet.png, 8x8", {"id": 31, "name": "Bullet"})},
            {"tool": "get_scene_tree", "result": ok("Scene 1 \"Main\" (2D, 3 GameObjects).\nMain Camera [10] camera\nBoss [30] sprite\n  Bullet [31] sprite\n", {"scene_id": 1})},
            {"tool": "read_file", "args": {"path": "re:rendering-2d"}, "result": ok(
                "## Sprite Batch\nUse a Sprite Batch for many copies of one Sprite's frames (bullets, coins). 1. add_component "
                "spritebatch on a GameObject with a Sprite. 2. Fill it from a script: SpriteBatch(scene, entity), "
                "batch:setPositions({x1, y1, ...}).")},
            {"tool": "add_component", "result": ok("Added Sprite Batch to GameObject 31 \"Bullet\".", {"entity": {"id": 31}})},
            {"tool": "write_file", "result": ok("Wrote scripts/spawner.lua.", {"path": "scripts/spawner.lua"})},
            {"tool": "check_script", "result": ok("scripts/spawner.lua: no errors.", {"ok": True})},
        ],
        # add_component spritebatch, or the script's SpriteBatch(scene, entity), which adds the component (SpriteBatch.cpp)
        "checks": {"max_calls": {"create_entity": 2},
                   "any_of": [{"file_must": {"scripts/spawner.lua": [r"SpriteBatch", r"setPositions"]}},
                              {"calls": [{"tool": "write_file", "args": {"content": "re:(?s)SpriteBatch.*setPositions"}}]}]},
        "max_turns": 14,
    },
    {
        "id": "guard_sort_bucket_break",
        "kind": "guard",
        "what_happened": "Reading get_render_stats batching.breaks, agents met sort_bucket (S52) and forked shaders or merged "
                         "textures; the profiling guide says a Sorting Layer, Order in Layer or Sorting Group change breaks a "
                         "batch by design and the fix is one layer and order for sprites that may batch.",
        "text_at_issue": "jasper://docs/profiling Dynamic Batching table: 'sort_bucket: another Sorting Layer, Order in Layer or "
                         "Sorting Group: put sprites that may batch in one'",
        "source": "S52 R35 (new guide rows); no failure on record",
        "prompt": "The coins in the open scene (our 2D level) draw in 40 draw calls even though they share one sprite sheet. Find out why and fix "
                  "it so they batch.",
        "mocks": [
            {"tool": "set_properties", "args": {"changes": [{"component": "sprite", "property": "sortingOrder"}]}, "result": err(
                "Unknown property \"sortingOrder\" of sprite on GameObject 50 \"Coin\". Properties: texture, width, height, pivot, flipX, "
                "flipY, frame, frames, color (custom ones also by their scene-file keys, see get_entity detail=\"yaml\").")},
            {"tool": "set_properties", "args": {"values": {"sprite": {"sortingOrder": "*"}}}, "result": err(
                "Unknown property \"sortingOrder\" of sprite on GameObject 50 \"Coin\". Properties: texture, width, height, pivot, flipX, "
                "flipY, frame, frames, color (custom ones also by their scene-file keys, see get_entity detail=\"yaml\").")},
            {"tool": "get_entity", "args": {"entity": "re:^(5[0-9]|[6-8][0-9]|coin.*)$"}, "result": ok(
                "Coin [50] components: sprite transform; sprite: texture assets/sprites/coins.png (frame coin_0); mesh: sortingLayer Default, "
                "sortingOrder 0 (each Coin has its own: 0-39)", {"id": 50, "name": "Coin"})},
            {"tool": "get_scene_tree", "result": ok("Scene 1 \"Main\" (2D, 42 GameObjects).\nMain Camera [10] camera\nPlayer [20] sprite\nCoin [50] sprite\n... Coin [89] sprite\n", {"scene_id": 1})},
            {"tool": "get_render_stats", "result": ok(
                "Game view: 46 draws, batching 3 batches (saved 2); breaks: sort_bucket 38, texture 1.",
                {"draw_calls": 46, "batching": {"batches": 3, "saved_by_batching": 2,
                                                "breaks": {"sort_bucket": 38, "texture": 1}}})},
            {"tool": "read_file", "args": {"path": "re:profiling"}, "result": ok(
                "| `sort_bucket` | another Sorting Layer, Order in Layer or Sorting Group: put sprites that may batch in one "
                "(`jasper://docs/rendering-2d`) |")},
            {"tool": "find_entities", "result": ok("40 GameObjects named Coin (50-89), each with Order in Layer 0-39.",
                                                  {"entities": [{"id": 50 + i, "name": "Coin"} for i in range(40)]})},
            {"tool": "set_properties", "result": ok("Set 1 property on 40 GameObjects.", {"entities": [{"id": 50}]})},
        ],
        "checks": {"any_of": [{"calls": [{"tool": "set_properties", "args": {"values": {"mesh": {"sortingOrder": "*"}}}}]},
                              {"calls": [{"tool": "set_properties", "args": {"changes": [{"component": "mesh", "property": "sortingOrder"}]}}]}],
                   "must_not_call": ["fork_shader", {"tool": "edit_sprite_atlas"}]},
        "max_turns": 14,
    },
    {
        "id": "guard_transparency_sort_axis",
        "kind": "guard",
        "what_happened": "New setting (S52): a top-down 2D game wants lower characters in front of higher ones within one "
                         "Sorting Layer; the Custom Axis does it in one setting, where agents otherwise wrote a script setting "
                         "every sprite's Order in Layer from its y each frame.",
        "text_at_issue": "graphics.transparency_sort_mode: 'custom_axis: in 2D cameras, renderers of one Sorting Layer and Order "
                         "in Layer draw by graphics.transparency_sort_axis (a larger dot(axis, position) first)'; "
                         "graphics.transparency_sort_axis: 'The Custom Axis, default [0, 1, 0] (a higher y draws first: top-down)'",
        "source": "S52 R4 (new setting text); no failure on record",
        "files": GAME_FILES,
        "prompt": "This is a top-down 2D game. Characters lower on the screen should be drawn in front of characters higher up, "
                  "like in most top-down RPGs. Set that up for the whole project.",
        "mocks": [
            {"tool": "get_project_settings", "result": ok(
                "graphics.transparency_sort_mode: \"default\" (default | custom_axis: in 2D cameras, renderers of one Sorting Layer "
                "and Order in Layer draw by graphics.transparency_sort_axis (a larger dot(axis, position) first)); "
                "graphics.transparency_sort_axis: [0, 1, 0] (the Custom Axis, default [0, 1, 0] (a higher y draws first: top-down))",
                {"values": {"graphics.transparency_sort_mode": "default", "graphics.transparency_sort_axis": [0, 1, 0]}})},
            {"tool": "set_project_settings", "result": ok("Set 1 project setting; saved project.yaml.",
                                                         {"changed": ["graphics.transparency_sort_mode"]})},
        ],
        "checks": {"calls": [{"tool": "set_project_settings", "args": {"values": {"graphics.transparency_sort_mode": "custom_axis"}}}],
                   "must_not_call": [{"tool": "write_file", "args": {"content": "re:sortingOrder"}}]},
        "max_turns": 10,
    },
    {
        "id": "bad_sorting_layer_rename_by_name",
        "kind": "bad",
        "what_happened": "Renaming a Sorting Layer, agents passed the new name list, which made a new layer and left the "
                         "renderers of the old one on Default; an {id, name} entry keeps the layer and its renderers (S52).",
        "text_at_issue": "tags_and_layers.sorting_layers: '... names, or {id, name} to rename a layer (it keeps its renderers)'",
        "source": "S52 R1, R34 (review finding: a rename by names moved renderers to Default)",
        "prompt": "Rename the Sorting Layer \"Front\" to \"Foreground\". Sprites already on it must stay on it.",
        "mocks": [
            {"tool": "get_project_settings", "after": "set_project_settings", "result": ok(
                "tags_and_layers.sorting_layers: [{\"id\": 0, \"name\": \"Default\"}, {\"id\": 3, \"name\": \"Foreground\"}]",
                {"values": {"tags_and_layers.sorting_layers": [{"id": 0, "name": "Default"}, {"id": 3, "name": "Foreground"}]}})},
            {"tool": "get_project_settings", "result": ok(
                "tags_and_layers.sorting_layers: [{\"id\": 0, \"name\": \"Default\"}, {\"id\": 3, \"name\": \"Front\"}] "
                "(Sorting Layers in draw order, first drawn first, as [{id, name}]; must include \"Default\". Pass the complete "
                "list: names, or {id, name} to rename a layer (it keeps its renderers); an unlisted layer is removed (its "
                "renderers draw on Default).)",
                {"values": {"tags_and_layers.sorting_layers": [{"id": 0, "name": "Default"}, {"id": 3, "name": "Front"}]}})},
            {"tool": "set_project_settings", "result": ok("Set 1 project setting; saved project.yaml.",
                                                         {"changed": ["tags_and_layers.sorting_layers"]})},
        ],
        "checks": {"calls": [{"tool": "set_project_settings",
                              "args": {"values": {"tags_and_layers.sorting_layers": ["*", {"id": 3, "name": "Foreground"}]}}}]},
        "max_turns": 8,
    },
    {
        "id": "bad_tilemap_terrain_needs_tile_of_terrain",
        "kind": "bad",
        "what_happened": "Asked to paint a lake, agents filled the area with one water tile, so the shore had no transitions; "
                         "the terrain op paints the terrain and chooses the edge tiles (S50 R23, R75).",
        "text_at_issue": "edit_tilemap description: 'Ops: jasper://docs/tilemaps'; guide 'Other areas': "
                         "'tilemaps (edit_tilemap)'; jasper://docs/tilemaps (terrain op)",
        "source": "S50 R75 (new op vocabulary); paraphrased from the design review",
        "prompt": "In the open scene, the Tilemap Layer 12 uses assets/Tilesets/overworld.tileset, which has a terrain named "
                  "Water with its edge tiles set up. Paint a lake of Water from cell (2, 2) to (8, 6), with proper shores.",
        "mocks": [
            {"tool": "find_entities", "result": ok("1 GameObject(s) match:\nGrid/Tilemap Layer [12] tilemaplayer transform", {"entities": [{"id": 12, "path": "Grid/Tilemap Layer"}]})},
            {"tool": "get_entity", "args": {"entity": "re:(?i)^(12|.*tilemap layer.*)$"}, "result": ok(
                "Tilemap Layer [12] components: tilemaplayer transform; tilemaplayer: tileset assets/Tilesets/overworld.tileset, 0 cells", {"id": 12, "name": "Tilemap Layer"})},
            {"tool": "get_scene_tree", "result": ok("Scene 1 \"Main\" (2D, 3 GameObjects).\nMain Camera [10] camera\nGrid [11] grid\n  Tilemap Layer [12] tilemaplayer\n", {"scene_id": 1})},
            {"tool": "get_tilemap", "result": ok("Layer \"Tilemap\": 0 cells.", {"layers": [{"name": "Tilemap"}], "tiles": 0})},
            {"tool": "edit_tilemap", "result": ok("Layer \"Tilemap\": 35 cells, 35 changed (one undo step).", {"changed_cells": 35, "warnings": []})},
        ],
        "checks": {"calls": [{"tool": "edit_tilemap", "args": {"ops": [{"op": "terrain"}]}}]},
        "max_turns": 8,
    },
    {
        "id": "guard_tilemap_preview_before_large_edit",
        "kind": "guard",
        "what_happened": "preview: true on edit_tilemap runs the ops and returns the result and image without writing; "
                         "an agent asked only to see the result must use it (S50 R75).",
        "text_at_issue": "edit_tilemap description: 'preview writes nothing'",
        "source": "S50 R75",
        "prompt": "Show me what filling Tilemap Layer 12 from (0, 0) to (3, 3) with the tile grass would look like, without "
                  "changing the scene.",
        "mocks": [
            {"tool": "find_entities", "result": ok("1 GameObject(s) match:\nGrid/Tilemap Layer [12] tilemaplayer transform", {"entities": [{"id": 12, "path": "Grid/Tilemap Layer"}]})},
            {"tool": "get_entity", "args": {"entity": "re:(?i)^(12|.*tilemap layer.*)$"}, "result": ok(
                "Tilemap Layer [12] components: tilemaplayer transform; tilemaplayer: tileset assets/Tilesets/overworld.tileset, 0 cells", {"id": 12, "name": "Tilemap Layer"})},
            {"tool": "get_scene_tree", "result": ok("Scene 1 \"Main\" (2D, 3 GameObjects).\nMain Camera [10] camera\nGrid [11] grid\n  Tilemap Layer [12] tilemaplayer\n", {"scene_id": 1})},
            {"tool": "get_tilemap", "result": ok("Layer \"Tilemap\": 0 cells.", {"layers": [{"name": "Tilemap"}], "tiles": 0})},
            {"tool": "edit_tilemap", "result": ok("Preview only (nothing written). Layer \"Tilemap\": 16 cells, 16 changed.", {"preview": True, "changed_cells": 16})},
        ],
        "checks": {"calls": [{"tool": "edit_tilemap", "args": {"preview": True}}]},
        "max_turns": 8,
    },
    {
        "id": "guard_tileset_check_before_painting",
        "kind": "guard",
        "what_happened": "edit_tileset check: true returns the terrain coverage report (the masks without a tile) and "
                         "writes nothing; asked which masks a tileset lacks, agents should use it rather than read the "
                         ".tileset JSON and work the masks out themselves, or take a preview's contact sheet (S50 R77). "
                         "The prompt names neither checking nor leaving the file alone. Agents read the file the prompt "
                         "names before any call, so no result can say it: only the guide's Code line, inside the 4096 "
                         "characters Claude Code keeps (0/2 without it; 2/4 as an Other areas pointer, past the cut).",
        "text_at_issue": "Guide 'Code': 'but a .tileset's masks or coverage: edit_tileset check, not its JSON'; edit_tileset "
                         "description: 'check: true returns only the coverage report.'",
        "source": "S50 R77; S50 review (the earlier prompt said 'Don't change anything', which the check parameter's name answers); "
                  "eval runs s50-1, s50-p4 and th-code-short",
        "prompt": "Which terrain masks of assets/Tilesets/overworld.tileset have no tile yet?",
        "files": {"assets/Tilesets/overworld.tileset": '{\n  "version": 1,\n  "name": "Overworld",\n  "tile_size": [16, 16],\n'
                  '  "sources": [{"id": 0, "kind": "texture", "texture": {"path": "Tilesets/overworld.png"}, "tile_size": [16, 16]}],\n'
                  '  "terrain_sets": [{"name": "Ground", "mode": "blob", "terrains": [{"name": "Grass", "color": "#5fa04eff"}]}],\n'
                  '  "tiles": [{"id": 0, "source": 0, "at": [0, 0], "terrain": {"set": 0, "center": 0}}],\n  "next_id": 1\n}\n'},
        "mocks": [
            {"tool": "edit_tileset", "args": {"check": True},
             "result": ok("WARN: 1 tiles, 46 missing masks, 0 problems.",
                          {"verdict": "WARN", "tiles": 1, "missing_masks": 46,
                           "terrain_sets": [{"name": "Ground", "mode": "blob", "required": 47, "missing_count": 46}], "problems": []})},
            {"tool": "edit_tileset",
             "result": ok("Preview of assets/Tilesets/overworld.tileset: 1 tiles (WARN). output: true writes it.",
                          {"path": "assets/Tilesets/overworld.tileset", "created": False, "tiles": 1,
                           "report": {"verdict": "WARN", "missing_masks": 46},
                           "next": "A preview: nothing was written. Pass output: true to write it."})},
        ],
        "checks": {"calls": [{"tool": "edit_tileset", "args": {"check": True}}],
                   "must_not_call": [{"tool": "edit_tileset", "args": {"output": True}}]},
        "max_turns": 8,
    },
    {
        "id": "guard_tileset_image_other_use_reported",
        "kind": "guard",
        "what_happened": "Making a texture a tileset's image switches its import to a pixel-art Sprite (no mips, point "
                         "filtering, uncompressed; S50 R12, R53), also when a material uses the same image. Only edit_tileset's "
                         "`next` says the material's texture changed; asked to report what changes for the material, the agent "
                         "must pass it on instead of answering that nothing changed.",
        "text_at_issue": "edit_tileset result next: '<image> now imports as a pixel-art Sprite (no mips, point filtering, "
                         "uncompressed) for the tileset; it is also used by <use>, which now reads it as a Sprite too.'",
        "source": "S50 review: a tileset image imported before its .tileset kept S14's default type (mips, seams in exports)",
        "prompt": "Make a tileset from assets/art/bricks.png with 16x16 tiles. The Wall material uses the same image: tell me "
                  "whether anything about it changes.",
        "mocks": [
            {"tool": "get_asset_info", "args": {"path": "re:(bricks\\.tileset|Wall\\.material)$"}, "after": "edit_tileset", "result": ok(
                "assets/art/bricks.tileset: tileset, 8 tiles of 16x16 from assets/art/bricks.png (imported as a pixel-art Sprite: no mips, point, uncompressed).",
                {"type": "tileset"})},
            {"tool": "get_asset_info", "args": {"path": "re:bricks\\.png$"}, "result": ok(
                "assets/art/bricks.png: texture 128x16 RGBA, imported as Default (mips, bilinear, compressed); used by assets/materials/Wall.material (baseColorTexture).",
                {"path": "assets/art/bricks.png", "type": "texture"})},
            {"tool": "list_assets", "result": ok("2 assets: assets/art/bricks.png (texture), assets/materials/Wall.material (material).",
                {"assets": [{"path": "assets/art/bricks.png"}, {"path": "assets/materials/Wall.material"}]})},
            {"tool": "edit_tileset",
             "result": ok("Wrote assets/art/bricks.tileset: 8 tiles (PASS, one undo step).",
                          {"path": "assets/art/bricks.tileset", "created": True, "tiles": 8, "sources": 1,
                           "written": "assets/art/bricks.tileset", "warnings": [],
                           "next": "assets/art/bricks.png now imports as a pixel-art Sprite (no mips, point filtering, "
                                   "uncompressed) for the tileset; it is also used by baseColorTexture of "
                                   "assets/materials/Wall.material, which now reads it as a Sprite too."})},
        ],
        "checks": {"calls": ["edit_tileset"], "reply_must": [r"sprite|mip|point|filter|compress"]},
        "max_turns": 12,
    },
    {
        "id": "guard_import_tilemap_preview_first",
        "kind": "guard",
        "what_happened": "import_tilemap previews by default and writes only with output: true; asked what a map would bring "
                         "in, agents should take the preview (grids, layers, tilesets, skipped items) instead of reading "
                         "the map's JSON by hand, and must not import it (S50 R78). The prompt does not say the call is "
                         "safe: only the description does.",
        "text_at_issue": "import_tilemap description: 'a preview (grids, layers, tilesets, skipped, warnings) until output: true writes'",
        "source": "S50 R78; S50 review (the earlier prompt said 'Don't import it yet', which made the must-not trivially pass)",
        "prompt": "What would importing assets/maps/level.tmj bring into the open scene?",
        "files": {"assets/maps/level.tmj": '{"type": "map", "orientation": "orthogonal", "width": 10, "height": 12, "tilewidth": 16, '
                  '"tileheight": 16, "infinite": false, "layers": [{"type": "tilelayer", "name": "Ground", "width": 10, "height": 12, '
                  '"data": [1, 1, 1]}, {"type": "objectgroup", "name": "Spawns", "objects": []}], '
                  '"tilesets": [{"firstgid": 1, "source": "overworld.tsj"}]}\n'},
        "mocks": [
            {"tool": "import_tilemap", "result": ok("Preview of assets/maps/level.tmj: 1 grid(s), 2 layer(s), 1 tileset(s), 120 tiles. output: true imports it.",
                                                     {"preview": True, "counts": {"grids": 1, "layers": 2, "tilesets": 1, "tiles": 120},
                                                      "next": "A preview: nothing was written. Pass output: true to import it (one undo step)."})},
        ],
        "checks": {"must_not_call": [{"tool": "import_tilemap", "args": {"output": True}}],
                   "calls": [{"tool": "import_tilemap"}]},
        "max_turns": 10,
    },
    {
        "id": "control_tilemap_trigger_word_is_not_edit",
        "kind": "control",
        "what_happened": "A question about what a tilemap is must not pull in the tilemap tools.",
        "text_at_issue": "Guide 'Other areas': 'tilemaps (edit_tilemap)'",
        "source": "S50 R82 (control for the Other areas entry)",
        "prompt": "In one sentence, what is the difference between a Sprite and a Tilemap in a 2D game? Answer without using any tools.",
        "mocks": [],
        "checks": {"must_not_call": ["edit_tilemap", "get_tilemap"]},
        "max_turns": 2,
    },
    {
        "id": "bad_sprite_batch_without_sprite",
        "kind": "bad",
        "what_happened": "Agents added a Sprite Batch to an empty GameObject and then filled it from Lua; it draws its "
                         "GameObject's Sprite, so add_component refuses it and the error names the fix (S52).",
        "text_at_issue": "add_component error: 'A Sprite Batch goes on a GameObject with a Sprite (add a Sprite first).'",
        "source": "S52 R10 (new error text); paraphrased from the design review",
        "prompt": "GameObject 40 \"Coins\" is an empty GameObject in the open scene. Make it draw many coins from one Sprite "
                  "Batch, using the texture sprites/coin.png. Don't write the script yet.",
        "mocks": [
            {"tool": "add_component", "args": {"component": "re:(?i)^sprite$"}, "result": ok(
                "Added Sprite to GameObject 40 \"Coins\".", {"entity": {"id": 40}})},
            {"tool": "add_component", "after": "add_component", "result": ok("Added Sprite Batch to GameObject 40 \"Coins\".",
                                                                             {"entity": {"id": 40}})},
            {"tool": "add_component", "result": err("A Sprite Batch goes on a GameObject with a Sprite (add a Sprite first).")},
            {"tool": "set_properties", "result": ok("Set 1 property on GameObject 40 \"Coins\".", {"entity": {"id": 40}})},
        ],
        "checks": {"calls": [{"tool": "add_component", "args": {"component": "re:(?i)^sprite$"}},
                             {"tool": "add_component", "args": {"component": "re:(?i)sprite.?batch"}}],
                   "max_errors": {"add_component": 1},
                   "must_not_call": ["create_entity"]},
        "max_turns": 12,
    },
    # -- S48 Pixel FX: the "Other areas" entry and capture_contact_sheet's lint and preview-then-write ----------------
    {
        "id": "guard_contact_sheet_lint",
        "kind": "guard",
        "what_happened": "New area (S48): checking that an effect ends empty and stays in its bounds is capture_contact_sheet "
                         "with lint in the pixel_art group; without the guide's index entry an agent takes screenshots in Play "
                         "and judges them by eye.",
        "text_at_issue": "Guide 'Other areas': 'pixel-fx (capture_contact_sheet)'; capture_contact_sheet: 'lint: checks (spill, "
                         "one_shot_end, ...) and a verdict'",
        "source": "S48 R27, R28 (new tool text); no failure on record",
        "prompt": "The Explosion GameObject in the Main scene is a one-shot pixel-art burst. Check that it ends with nothing "
                  "left on screen and that nothing flies outside its area, and tell me what you find.",
        "mocks": [
            {"tool": "preview_particle_effect", "args": {"view": "pixel"}, "result": ok(
                "Explosion: 96 ticks of 0.008333 s (0.8 s), peak 40 particles. Verdict WARN.\nWARN spill: 14 px drawn outside the bounds plus "
                "margin (seed 3, frame 5): lower the speed, spread, lifetime or size that throws it out.\nPASS one_shot_end: 0.\nPASS off_grid: 0.",
                {"verdict": "WARN", "checks": [{"id": "spill", "status": "WARN", "measured": 14}, {"id": "one_shot_end", "status": "PASS"}]})},
            {"tool": "preview_particle_effect", "result": ok(
                "Explosion: three-quarter view. Verdict PASS.\nPASS never_ends: 0.\nSKIP spill: Measured on the pixel view's cells: run with view pixel.",
                {"verdict": "PASS", "checks": [{"id": "spill", "status": "SKIP", "limited_by": "Measured on the pixel view's cells: run with view pixel."}]})},
            {"tool": "read_file", "args": {"path": "re:explosion\\.particles$"}, "result": ok(
                "assets/fx/explosion.particles lines 1-9 of 9:\n{\"format\": \"jasperengine.particles\", \"version\": 3, \"duration\": 0.8, "
                "\"loop\": false,\n \"emitters\": [{\"burst\": 40, \"lifetime\": [0.4, 0.8], \"speed\": [30, 60], \"shape\": \"circle\", "
                "\"renderMode\": \"pixel\"}]}", {"path": "assets/fx/explosion.particles"})},
            {"tool": "get_asset_info", "args": {"path": "re:explosion\\.particles$"}, "result": ok(
                "assets/fx/explosion.particles: particle effect, 1 emitter, one-shot, duration 0.8 s.", {"path": "assets/fx/explosion.particles", "type": "particles"})},
            {"tool": "list_assets", "result": ok("1 asset: assets/fx/explosion.particles (particles).", {"assets": [{"path": "assets/fx/explosion.particles"}]})},
            {"tool": "get_particle_effect", "result": ok("assets/fx/explosion.particles: 1 emitter, burst 40, lifetime 0.4-0.8 s, speed 30-60, "
                "one-shot, duration 0.8 s, Render Mode Pixel.", {"path": "assets/fx/explosion.particles"})},
            {"tool": "find_entities", "result": ok("1 GameObject(s) match:\nExplosion [40] particles transform", {"entities": [{"id": 40, "path": "Explosion"}]})},
            {"tool": "get_scene_tree", "result": ok("Scene 1 \"Main\" (2D, 3 GameObjects).\nMain Camera [10] camera\nPlayer [12] sprite\nExplosion [40] particles\n", {"scene_id": 1})},
            {"tool": "get_entity", "args": {"entity": "re:(?i)^(40|Explosion)$"}, "result": ok("Explosion [40] components: particles transform; particles: effect assets/fx/explosion.particles (one-shot, 0.8 s), "
                "playOnAwake true, loop false, sortingLayer Default", {"id": 40, "name": "Explosion"})},
            {"tool": "capture_contact_sheet", "args": {"lint": "*"}, "result": ok(
                "Captured 8 frames of Explosion (isolated) at scale 4: a 803x256 sheet of 100x100 frames. Checks WARN, verdict "
                "fixable:\nWARN spill: 14.0. 14 px drawn outside the 4x4 bounds + 24 px margin (seed 3, frame 5) keep it inside "
                "the margin: lower the speed, spread, lifetime or size that throws it out, or move its origin\nPASS "
                "one_shot_end: 0.0\nFix what each WARN's fix_hint says, then capture again with lint.",
                {"target": "Explosion", "mode": "isolated", "scale": 4, "cell": [100, 100], "verdict": "fixable",
                 "check_status": "WARN",
                 "checks": [{"id": "spill", "kind": "effect", "status": "WARN", "measured": 14, "threshold": 0,
                             "fix_hint": "keep it inside the margin: lower the speed, spread, lifetime or size that throws it out"},
                            {"id": "one_shot_end", "kind": "effect", "status": "PASS", "measured": 0, "threshold": 0}],
                 "next": "Fix what each WARN's fix_hint says, then capture again with lint."})},
            {"tool": "capture_contact_sheet", "result": ok(
                "Captured 8 frames of Explosion (isolated) at scale 4: a 803x256 sheet of 100x100 frames.",
                {"target": "Explosion", "mode": "isolated", "scale": 4,
                 "next": "Preview only: the same arguments plus output: true and path write the PNG (one undo)."})},
        ],
        # the particle preview's pixel view runs the same S48 checks (jasper://docs/particles): a second documented route
        "checks": {"any_of": [{"calls": [{"tool": "capture_contact_sheet", "args": {"target": {"entity": "re:(?i)^(explosion|40)$"}, "lint": "*"}}]},
                              {"calls": [{"tool": "preview_particle_effect", "args": {"entity": "re:(?i)^(explosion|40)$", "view": "pixel"}}]},
                              {"calls": [{"tool": "preview_particle_effect", "args": {"path": "re:explosion\\.particles$", "view": "pixel"}}]}],
                   "must_not_call": [{"tool": "play", "args": {"action": "start"}}, "record_game"],
                   "reply_must": [r"(?i)spill|outside"]},
        "max_turns": 14,
    },
    {
        "id": "guard_contact_sheet_write",
        "kind": "guard",
        "what_happened": "New tool (S48): capture_contact_sheet previews by default; asked to save a sheet, an agent must "
                         "call again with output: true and path, which the description and the result's `next` say.",
        "text_at_issue": "capture_contact_sheet: 'A preview until output: true writes path (one undo)'; the result's `next`",
        "source": "S48 R27 (new tool text); no failure on record",
        "prompt": "Save a contact sheet of the Hero GameObject's walk clip assets/anim/walk.anim to assets/sheets/walk.png.",
        "mocks": [
            {"tool": "capture_contact_sheet", "args": {"output": True}, "result": ok(
                "Captured 8 frames of Hero playing assets/anim/walk.anim (isolated) at scale 6: a 821x350 sheet of 64x64 "
                "frames. Wrote assets/sheets/walk.png as one undo step (undo with scope project).",
                {"target": "Hero playing assets/anim/walk.anim", "path": "assets/sheets/walk.png",
                 "undo": {"id": 7, "name": "Create File walk.png"}})},
            {"tool": "capture_contact_sheet", "result": ok(
                "Captured 8 frames of Hero playing assets/anim/walk.anim (isolated) at scale 6: a 821x350 sheet of 64x64 "
                "frames.\nPreview only: the same arguments plus output: true and path write the PNG (one undo).",
                {"target": "Hero playing assets/anim/walk.anim",
                 "next": "Preview only: the same arguments plus output: true and path write the PNG (one undo)."})},
        ],
        "checks": {"calls": [{"tool": "capture_contact_sheet",
                              "args": {"target": {"animation": "re:walk\\.anim"}, "output": True, "path": "re:sheets/walk\\.png"}}],
                   "must_not_call": ["write_file", "screenshot", "record_game"], "shell_must_not": [r"(?i)ffmpeg|magick|montage"]},
        "max_turns": 10,
    },
    {
        "id": "guard_contact_sheet_effect_target",
        "kind": "guard",
        "what_happened": "New tool text (S48 R27, WP-D): capture_contact_sheet draws a GameObject playing a PixelFX effect "
                         "(target.effect); asked to check how an enemy burns away, an agent captures that target instead of "
                         "starting Play and taking screenshots by hand.",
        "text_at_issue": "capture_contact_sheet: '...; effect: a PixelFX effect), a .particles asset or a screen transition' "
                         "and the target's effect / effect_options keys",
        "source": "S48 R27 (new tool text); no failure on record",
        "prompt": "Show me frames of the Enemy sprite burning away with the PixelFX dissolve effect over 0.9 seconds, so I can "
                  "check that the burn looks right.",
        "mocks": [
            {"tool": "capture_contact_sheet", "args": {"target": {"effect": "dissolve"}}, "result": ok(
                "Captured 8 frames of Enemy with dissolve (isolated) at scale 6: a 821x350 sheet of 64x64 frames.\nPreview "
                "only: the same arguments plus output: true and path write the PNG (one undo).",
                {"target": "Enemy with dissolve", "frames": [{"index": i, "t": round(i * 0.9 / 7, 3)} for i in range(8)],
                 "next": "Preview only: the same arguments plus output: true and path write the PNG (one undo)."})},
            {"tool": "capture_contact_sheet", "result": ok(
                "Captured 8 frames of Enemy (isolated) at scale 6: a 821x350 sheet of 64x64 frames.",
                {"target": "Enemy"})},
        ],
        "checks": {"calls": [{"tool": "capture_contact_sheet",
                              "args": {"target": {"effect": "dissolve"}}}],
                   "must_not_call": [{"tool": "play", "args": {"action": "start"}}, "record_game", "screenshot"]},
        "max_turns": 10,
    },
    {
        "id": "guard_contact_sheet_transition_target",
        "kind": "guard",
        "what_happened": "New tool text (S48 R27, AC12): capture_contact_sheet draws a screen transition kind by itself "
                         "(target.transition) and its transition_edges check measures the rim; asked whether the iris rim keeps "
                         "one width, an agent captures it with that lint instead of playing the game and judging screenshots.",
        "text_at_issue": "capture_contact_sheet: '... a .particles asset or a screen transition at N times'; target.transition",
        "source": "S48 R27, R28 (new tool text); no failure on record",
        "prompt": "Check that the engine's built-in iris screen transition's coloured rim stays the same width all along its edge while it closes.",
        "mocks": [
            # the editor's own answers (a new project), so a search finds what the real one finds
            {"tool": "search_engine_api", "args": {"query": "re:(?i)iris|transition"}, "result": ok("API symbols for \"iris transition\":\n- TransitionKind.IRIS [EnumMember]\n- AnimationPlayer:isInTransition() -> bool [Method]\n- Animator:isInTransition() -> bool [Method]\n- AudioMixer.transitionTo(string snapshot, number seconds, AudioTimeOptions opts) -> boolean [Function]\n- AudioMixer.transitionToSnapshots(string[] snapshots, number[] weights, number seconds, AudioTimeOptions opts) -> boolean [Function]\n- AudioMixer::isTransitioning() -> bool [CppMethod] (C++ scripts only; not callable from Lua. Look for a bound property on AudioMixer instead.)\n- Button.transition [Property]\n- Button::getTransition() -> ButtonTransition [CppMethod] (C++ scripts only; not callable from Lua. Use the bound property Button.transition instead.)\n- Button::setTransition(ButtonTransition transition) [CppMethod] (C++ scripts only; not callable from Lua. Use the bound property Button.transition instead.)\n- ButtonComponent.transition [Property]\n- Mesh.fadeTransitionWidth [Property]\n- Mesh::getLodFadeTransitionWidth() -> float [CppMethod] (C++ scripts only; not callable from Lua. Look for a bound property on Mesh instead.)\n- Mesh::setLodFadeTransitionWidth(float width) [CppMethod] (C++ scripts only; not callable from Lua. Look for a bound property on Mesh instead.)\n- UISystem::transitionWritesColor(ButtonTransition transition) -> bool [CppMethod] (C++ scripts only; not callable from Lua. Look for a bound property on UISystem instead.)\n- UISystem::transitionWritesTexture(ButtonTransition transition) -> bool [CppMethod] (C++ scripts only; not callable from Lua. Look for a bound property on UISystem instead.)\n- class Transition [Class]\nIn the reference docs:\n- jasper://docs/html-ui-effects (HTML UI effect catalogue, line 107): | [`iris-wipe`](#iris-wipe) | filter | transition | low | Wipes the content away with a circle growing from a centre |\n- jasper://docs/lua-api/2-8-1-transition-static-screen-transitio (2.8.1 Transition (static): screen transitions, line 5): kind and option). Kinds are names or `TransitionKind` values (`IRIS`, `BLINDS`, `SLASH`,\n- jasper://docs/lua-api-reference/transition (Transition, line 16): - `Transition.loadScene(scene: string, kind?: TransitionKindName|integer, duration?: number, options?: TransitionOptions) -> TransitionHandle`: Covers, calls `options.onCovered`, loads the scene stack (SceneManager.loadScene), holds while it loads and reveals once the new scenes drew a frame; a load ...\n- jasper://docs/lua-api-reference/enums (Enums, line 179): - **TransitionKind**: The screen transitions' kinds (Transition.play's first argument; a name works as well) (`IRIS` = 0, `BLINDS` = 1, `SLASH` = 2, `PIXEL_DISSOLVE` = 3, `CRT_OFF` = 4, `BURN_AWAY` = 5, `FADE` = 6)\n- jasper://docs/pixel-fx (Screen transitions, line 63): Transition.cover(TransitionKind.IRIS, 0.4, {shape = \"diamond\"})\n- jasper://docs/pixel-fx (Screen transitions, line 64): Transition.reveal(\"iris\", 0.4)\n- jasper://docs/pixel-fx (Screen transitions, line 66): Transition.loadScene(\"Level2\", \"iris\", 0.8, {shape = \"diamond\", ramp = \"Palettes/ui.palette#ramp:gold\"})\n- jasper://docs/pixel-fx (Screen transitions, line 43): | `iris` | a hole closes on the centre | `shape` circle, diamond or square; `center` (Vector2, normalized from the top-left) or `centerObject` (a GameObject) |")},
            {"tool": "search_engine_source", "args": {"query": "re:(?i)iris"}, "result": ok("40 match(es) (truncated).\ncore/device/GpuTiersData.cpp:190: {\"platform\":\"apple\",\"kind\":\"name\",\"vendor\":\"intel\",\"name\":\"intel iris graphics 540\",\"devices\":[\"0x1926\",\"0x1927\"],\"tier\":\"medium\",\"from\":\"forge low\"},\ncore/device/GpuTiersData.cpp:191: {\"platform\":\"apple\",\"kind\":\"name\",\"vendor\":\"intel\",\"name\":\"intel iris graphics 6100\",\"devices\":[\"0x162b\"],\"tier\":\"low\",\"from\":\"forge verylow\"},\ncore/device/GpuTiersData.cpp:193: R\"JEGPU(    {\"platform\":\"apple\",\"kind\":\"name\",\"vendor\":\"intel\",\"name\":\"intel iris plus graphics\",\"devices\":[\"0x3ea2\",\"0x3ea5\",\"0x3ea6\",\"0x3ea7\",\"0x3ea8\"],\"tier\":\"high\",\"from\":\"forge medium\"},")},
            {"tool": "capture_contact_sheet", "args": {"target": {"transition": "iris"}, "lint": "*"}, "result": ok(
                "Captured 8 frames of transition iris (isolated) at scale 2: a 1288x356 sheet of 160x90 frames. Checks PASS, "
                "verdict clean:\nPASS transition_edges: 0.0",
                {"target": "transition iris", "verdict": "clean", "check_status": "PASS",
                 "checks": [{"id": "transition_edges", "kind": "effect", "status": "PASS", "measured": 0, "threshold": 0}]})},
            {"tool": "capture_contact_sheet", "result": ok(
                "Captured 8 frames of transition iris (isolated) at scale 2: a 1288x356 sheet of 160x90 frames.",
                {"target": "transition iris"})},
        ],
        "checks": {"calls": [{"tool": "capture_contact_sheet", "args": {"target": {"transition": "iris"}, "lint": "*"}}],
                   "must_not_call": [{"tool": "play", "args": {"action": "start"}}, "record_game", "screenshot"],
                   "reply_must": [r"(?i)rim|width|edge"]},
        "max_turns": 14,
    },
    {
        "id": "guard_contact_sheet_too_large",
        "kind": "guard",
        "what_happened": "New error text (S48 review): capture_contact_sheet caps a transition's capture size; the error "
                         "capture_too_large names the fix, and an agent retries smaller instead of giving up or looping.",
        "text_at_issue": "capture_contact_sheet error: capture_too_large with 'lower target.transition_options' width, height or "
                         "pixel_size, or ask for fewer frames'",
        "source": "S48 review, finding 6 (new error text); no failure on record",
        "prompt": "Capture an iris screen transition's contact sheet at 1024 by 1024 art pixels with pixel size 16 and 64 frames, "
                  "and show me the sheet.",
        "mocks": [
            {"tool": "capture_contact_sheet", "once": True, "args": {"frames": 64}, "result": err(
                "Can't capture the contact sheet:\n- capture_too_large: a 16384 x 16384 frame x 64 frames is 17179869184 px of frames; "
                "a capture takes at most 4096 px a side and 33554432 px over all its frames Fix: lower "
                "target.transition_options' width, height or pixel_size, or ask for fewer frames")},
            {"tool": "capture_contact_sheet", "result": ok(
                "Captured 8 frames of transition iris (isolated) at scale 1: a 1288x176 sheet of 160x90 frames.",
                {"target": "transition iris", "scale": 1, "cell": [160, 90],
                 "next": "Preview only: the same arguments plus output: true and path write the PNG (one undo)."})},
        ],
        "checks": {"calls": [{"tool": "capture_contact_sheet", "args": {"target": {"transition": "iris"}}}],
                   "reply_must": [r"(?i)too large|limit|cap|at most", r"(?i)smaller|lower|fewer|reduce|pixel.?size"]},
        "max_turns": 10,
    },
    {
        "id": "guard_contact_sheet_running",
        "kind": "guard",
        "what_happened": "New error text (S48 review): a capture is refused while another runs; the error names get_job, and "
                         "the agent waits for the job then retries instead of concluding the tool is broken.",
        "text_at_issue": "capture_contact_sheet error: capture_running with 'wait for job <id> with get_job, or cancel_job it, "
                         "then call again'",
        "source": "S48 review, finding 12 (new error text); no failure on record",
        "prompt": "Capture a contact sheet of the Explosion GameObject and tell me its size.",
        "mocks": [
            {"tool": "find_entities", "result": ok("1 GameObject(s) match:\nExplosion [40] particles transform", {"entities": [{"id": 40, "path": "Explosion"}]})},
            {"tool": "get_scene_tree", "result": ok("Scene 1 \"Main\" (2D, 3 GameObjects).\nMain Camera [10] camera\nPlayer [12] sprite\nExplosion [40] particles\n", {"scene_id": 1})},
            {"tool": "get_entity", "args": {"entity": "re:(?i)^(40|Explosion)$"}, "result": ok("Explosion [40] components: particles transform", {"id": 40, "name": "Explosion"})},
            {"tool": "capture_contact_sheet", "once": True, "result": err(
                "Can't capture the contact sheet:\n- capture_running: another capture is still running (job job_7) Fix: wait for "
                "job job_7 with get_job, or cancel_job it, then call again")},
            {"tool": "get_job", "result": ok("job_7: succeeded", {"job_id": "job_7", "status": "succeeded"})},
            {"tool": "capture_contact_sheet", "result": ok(
                "Captured 8 frames of Explosion (isolated) at scale 4: a 803x256 sheet of 100x100 frames.",
                {"target": "Explosion", "scale": 4, "cell": [100, 100]})},
        ],
        "checks": {"calls": [{"tool": "get_job"}, {"tool": "capture_contact_sheet"}],
                   "must_not_call": ["record_game", "screenshot"],
                   "reply_must": [r"803|100"]},
        "max_turns": 10,
    },
    {
        "id": "guard_contact_sheet_reload_disclosed",
        "kind": "guard",
        "what_happened": "New result text (S48 review): capture_contact_sheet's determinism check in Play reloads the played "
                         "scene; the result's next says the game restarted, so the agent reports it instead of assuming the "
                         "game's state is unchanged.",
        "text_at_issue": "capture_contact_sheet: 'lint: checks (..., determinism, ...)' (without the check names agents compared "
                         "two plain captures by hand: 0 of 6 samples); its next: 'The determinism check reloaded the played scene "
                         "for its two fresh captures, so the game's state restarted.'",
        "source": "S48 review, finding 10 (new result text); no failure on record",
        "prompt": "While the game is playing, check with a contact sheet that the Random GameObject's frames are deterministic, "
                  "and tell me anything I need to know about the state of the game afterwards.",
        "mocks": [
            {"tool": "find_entities", "result": ok("1 GameObject(s) match:\nRandom [41] script sprite transform", {"entities": [{"id": 41, "path": "Random"}]})},
            {"tool": "get_scene_tree", "result": ok("Scene 1 \"Main\" (2D, 3 GameObjects).\nMain Camera [10] camera\nPlayer [12] sprite\nRandom [41] script sprite\n", {"scene_id": 1})},
            {"tool": "get_entity", "args": {"entity": "re:(?i)^(41|Random)$"}, "result": ok("Random [41] components: script sprite transform", {"id": 41, "name": "Random"})},
            {"tool": "play", "result": ok("Playing the Main scene.", {"state": "playing"})},
            {"tool": "get_play_state", "result": ok("Playing.", {"state": "playing"})},
            {"tool": "capture_contact_sheet", "args": {"lint": "*"}, "result": ok(
                "Captured 3 frames of Random (scene) at scale 1: a 322x116 sheet of 320x180 frames. Checks FAIL, verdict "
                "needs_person:\nFAIL determinism: 14.0",
                {"target": "Random", "mode": "scene", "verdict": "needs_person", "check_status": "FAIL",
                 "checks": [{"id": "determinism", "kind": "effect", "status": "FAIL", "measured": 14, "threshold": 0}],
                 "next": "The determinism check reloaded the played scene for its two fresh captures, so the game's state restarted. "
                         "Look at the sheet as well as the checks: they measure mistakes, not taste."})},
            {"tool": "capture_contact_sheet", "result": ok(
                "Captured 8 frames of Random (scene) at scale 1: a 2570x182 sheet of 320x180 frames.",
                {"target": "Random", "mode": "scene", "next": "Preview only: the same arguments plus output: true and path write the PNG (one undo)."})},
        ],
        "checks": {"calls": [{"tool": "capture_contact_sheet", "args": {"lint": "*"}}],
                   "reply_must": [r"(?i)reload|restart"]},
        "max_turns": 10,
    },
    # ---- S48 Pixel FX: sprite effects (material presets and library:sprite features)
    {
        "id": "guard_pixel_fx_selout_preset",
        "kind": "guard",
        "what_happened": "New text (S48 R38): list_material_presets names the Pixel FX presets; an agent asked for a sel-out "
                         "outline on a sprite applies selout_outline instead of hand-writing a shader or an outline pass.",
        "text_at_issue": "list_material_presets: 'selout_outline (Sel-Out Outline): library:sprite with a one-texel selective "
                         "outline (sel-out) ...'",
        "source": "S48 R38 (new result text); no failure on record",
        "prompt": "Give the Hero sprite a pixel-art sel-out outline.",
        "mocks": [
            {"tool": "list_material_presets", "result": ok(
                "12 material preset(s); apply_material_preset applies one:\noutline_hull (Outline (hull)): Outline as an extra "
                "pass ...\npixel_outline (Pixel Outline): library:sprite with a one-texel outline on the 4 neighbours (Outline "
                "style PIXEL4) in the ramp's darkest colour; the quad grows so it is never cut at the frame edge.\n"
                "selout_outline (Sel-Out Outline): library:sprite with a one-texel selective outline (sel-out): each outline "
                "texel takes the colour of the art it touches, one ramp step darker.\ndrop_shadow (Drop Shadow): library:sprite "
                "with a drop shadow one texel right and down ...",
                {"presets": [{"id": "outline_hull", "name": "Outline (hull)"}, {"id": "pixel_outline", "name": "Pixel Outline"},
                             {"id": "selout_outline", "name": "Sel-Out Outline"}, {"id": "drop_shadow", "name": "Drop Shadow"}]})},
            {"tool": "apply_material_preset", "result": ok(
                "Applied preset selout_outline to 1 submesh(es) (one undo step).",
                {"preset": "selout_outline", "applied": [{"entity": {"id": 7, "name": "Hero"}, "submesh": 0}]})},
            {"tool": "find_entities", "result": ok("1 GameObject: 7 Hero (sprite)", {"entities": [{"id": 7, "path": "Hero"}]})},
            {"tool": "get_entity", "result": ok("GameObject 7 \"Hero\": transform, sprite, mesh (submeshes[0] material, shader "
                                               "library:sprite).", {"id": 7})},
            {"tool": "list_shaders", "result": ok("Library shaders (mesh): library:lit, library:sprite (outline, ...) ...",
                                                 {"shaders": [{"name": "library:sprite", "type": "mesh"}]})},
            {"tool": "set_properties", "result": ok("Set 1 property of GameObject 7 \"Hero\".", {"entity": {"id": 7}})},
            {"tool": "update_material", "result": ok("Updated the material.", {"entity": {"id": 7}})},
        ],
        "checks": {"calls": [{"tool": "apply_material_preset", "args": {"preset": "selout_outline"}}],
                   "must_not_call": ["fork_shader"], "edits_must_not": [r"\.(frag|vert|glsl)$"]},
        "max_turns": 12,
    },
    {
        "id": "guard_drivable_car_wheel_colliders",
        "kind": "guard",
        "what_happened": "A drivable car is a dynamic body with a Wheel Collider on a child at each wheel (suspension and "
                         "tire friction on Jolt's vehicle constraint), not four wheel bodies on hinge joints, which bounce and "
                         "slide sideways.",
        "text_at_issue": "component Wheel Collider 'A car wheel with suspension and tire friction, below a dynamic Rigidbody'; "
                         "physics guide 'Vehicles (Wheel Collider)': 'A drivable car is a dynamic Rigidbody ... with a Wheel "
                         "Collider ... on a child GameObject at each wheel: not four bodies on hinge joints'",
        "source": "S96 REQ-47 (WP-A); no failure on record",
        "prompt": "The Kart in the current scene is just a box mesh. Make it a drivable car with four wheels.",
        "mocks": [
            {"tool": "find_entities", "result": ok("1 GameObject: 21 Kart (mesh)", {"entities": [{"id": 21, "path": "Kart"}]})},
            {"tool": "get_entity", "result": ok("GameObject 21 \"Kart\": transform, mesh (cube, scale 2 1 4).", {"id": 21})},
            {"tool": "list_component_types", "result": ok(
                "... Rigidbody (rigidbody): Rigidbody with colliders, simulated by Jolt; Joint 3D (joint3d): Connects two "
                "Rigidbodies; Character Controller (charactercontroller): A capsule moved by script that climbs steps and "
                "slopes; Wheel Collider (wheelcollider): A car wheel with suspension and tire friction, below a dynamic "
                "Rigidbody; Vehicle Drivetrain (vehicledrivetrain): An engine, gear box and differentials driving the body's "
                "Wheel Colliders ...",
                {"components": [{"key": "rigidbody", "name": "Rigidbody"}, {"key": "joint3d", "name": "Joint 3D"},
                                {"key": "wheelcollider", "name": "Wheel Collider"},
                                {"key": "vehicledrivetrain", "name": "Vehicle Drivetrain"}]})},
            {"tool": "edit_colliders", "result": ok("Kart: dynamic body, 1 collider (box 2 x 1 x 4).", {"entity": {"id": 21}})},
            {"tool": "create_entity", "result": ok("Created GameObject 30 \"Wheel\" under 21 \"Kart\".",
                                                   {"entity_id": 30, "entity": {"id": 30}})},
            {"tool": "add_component", "args": {"component": "re:(?i)wheel.?collider"}, "result": ok(
                "Added Wheel Collider to GameObject 30 \"Wheel\".", {"entity": {"id": 30}})},
            {"tool": "run_command", "result": ok("Ran GameObject > 3D Object > Vehicle...", {"ran": "gameobject.create.3DObject.vehicle"})},
            {"tool": "set_properties", "result": ok("Set 1 property.", {"entity": {"id": 21}})},
        ],
        "checks": {"any_of": [{"calls": [{"tool": "add_component", "args": {"component": "re:(?i)wheel.?collider"}}]},
                              {"calls": [{"tool": "run_command", "args": {"id": "gameobject.create.3DObject.vehicle"}}]}],
                   "must_not_call": [{"tool": "add_component", "args": {"component": "re:(?i)joint"}}, "set_joint"]},
        "max_turns": 16,
    },
]
