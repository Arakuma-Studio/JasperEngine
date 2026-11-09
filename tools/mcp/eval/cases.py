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
                                     {"tool": "set_sprite_animation"}],
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
            {"tool": "tool_groups", "args": {}, "result": ok("Enabled sprites.", {"enabled": ["sprites"]})},
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
            {"tool": "tool_groups", "args": {}, "result": ok("Enabled sprites.", {"enabled": ["sprites"]})},
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
        "prompt": "assets/art/hero.png is an AI image that only looks like pixel art (fake big pixels, blurred). Clean it into real pixel "
                  "art and keep the result as a new file next to it.",
        "mocks": [
            {"tool": "fix_pixel_art", "args": {"output": True}, "result": ok(
                "fixable: Upscaled pixel art: fixes to 32x32, 16 colors. Wrote assets/art/hero_clean.png as one undo step.",
                {"verdict": "fixable", "written": [{"role": "png", "path": "assets/art/hero_clean.png"}],
                 "undo": {"id": 14, "name": "agent: Pixel Art Cleanup (hero_clean.png)"}})},
            {"tool": "fix_pixel_art", "result": ok(
                "fixable: Upscaled pixel art: fixes to 32x32, 16 colors (grid fit 100%, confidence 98%). Preview only: nothing written.",
                {"verdict": "fixable", "confidence": 0.98, "default_output": "assets/art/hero_clean.png",
                 "next": "Preview only, nothing written: look at the image, adjust recipe if it is wrong, then the same arguments plus "
                         "output: true write exactly these pixels."})},
        ],
        "checks": {"calls": [{"tool": "fix_pixel_art", "args": {"source": "assets/art/hero.png"}},
                             {"tool": "fix_pixel_art", "args": {"source": "assets/art/hero.png", "output": True}}],
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
        "prompt": "Clean up assets/art/photo.jpg into pixel art with the Pixel Art Cleanup tools and write the result. If the tools say "
                  "it needs a person, stop and tell me what they found.",
        "mocks": [
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
                   "must_not_call": ["write_file"], "max_calls": {"fix_pixel_art": 2},
                   "reply_must": [r"(?i)needs? (a )?person|low.?(fit|confidence)|guard"]},
        "max_turns": 8,
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
        "prompt": "Our pixel-art hero sprite assets/sprites/hero.png has to use only the PICO-8 colours. Recolour it onto "
                  "that palette and keep the result in the same file.",
        "mocks": [
            {"tool": "apply_palette", "args": {"output": True}, "result": ok(
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
        "checks": {"calls": [{"tool": "apply_palette", "args": {"palette": "re:(?i)pico", "output": True}}],
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
        "prompt": "Does assets/sprites/tiles.png use only colours of assets/Palettes/level.palette? Don't change any file, "
                  "just tell me.",
        "mocks": [
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
        "checks": {"any_of": [{"calls": ["apply_palette"]}, {"calls": [{"tool": "edit_palette", "args": {"texture": "*"}}]}],
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
                             {"tool": "preview_particle_effect", "args": {"view": "pixel"}}],
                   "must_not_call": ["write_file"], "edits_must_not": [r"\.particles$"]},
        "max_turns": 14,
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
            {"tool": "read_file", "result": ok("# Pixel fonts ...", {})},
        ],
        "checks": {"calls": [{"tool": "write_file", "args": {"path": "re:\\.bitmapfont$"}}],
                   "shell_must_not": [r"python", r"convert\s"], "reply_must": [r"95|clean|glyph"]},
        "max_turns": 8,
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
                "assets/fonts/hud.fnt: font, a bitmap font: 95 glyphs, 9 px (line 10, baseline 7).",
                {"path": "assets/fonts/hud.fnt", "type": "font", "font": {"kind": "bitmap", "native_size": 9, "baseline": 7},
                 "pixel_report": {"antialiased": False, "partial_alpha_share": 0.0, "native_size": 9}})},
        ],
        "checks": {"calls": [{"tool": "get_asset_info", "args": {"path": "re:hud\\.fnt"}}],
                   "must_not_call": ["read_file", "view_asset", "preview_font"], "reply_must": [r"9", r"7"]},
        "max_turns": 6,
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
        "checks": {"calls": [{"tool": "set_project_settings", "args": {"values": {"tags_and_layers.sorting_layers": "re:Foreground"}}},
                             {"tool": "set_properties"}],
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
            {"tool": "read_file", "args": {"path": "re:rendering-2d"}, "result": ok(
                "## Sprite Batch\nUse a Sprite Batch for many copies of one Sprite's frames (bullets, coins). 1. add_component "
                "spritebatch on a GameObject with a Sprite. 2. Fill it from a script: SpriteBatch(scene, entity), "
                "batch:setPositions({x1, y1, ...}).")},
            {"tool": "add_component", "result": ok("Added Sprite Batch to GameObject 31 \"Bullet\".", {"entity": {"id": 31}})},
            {"tool": "write_file", "result": ok("Wrote scripts/spawner.lua.", {"path": "scripts/spawner.lua"})},
            {"tool": "check_script", "result": ok("scripts/spawner.lua: no errors.", {"ok": True})},
        ],
        "checks": {"calls": [{"tool": "add_component", "args": {"component": "re:(?i)sprite.?batch"}}],
                   "max_calls": {"create_entity": 2},
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
        "prompt": "The coins in our 2D level draw in 40 draw calls even though they share one sprite sheet. Find out why and fix "
                  "it so they batch.",
        "mocks": [
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
        "checks": {"calls": [{"tool": "set_properties", "args": {"values": {"mesh": {"sortingOrder": "*"}}}}],
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
                              "args": {"values": {"tags_and_layers.sorting_layers": "re:(?s)3.*Foreground|Foreground.*3"}}}]},
        "max_turns": 8,
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
]
