# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""The eval's cases: one decision point each (README.md). Paraphrased from real sessions and
fixes, with no user data. Results are in the editor's real shapes: {"recorded": key} reuses a
result dump_surface.py recorded from a built editor; hand-written ones copy the server's text.
"""

import json

KINDS = ("bad", "guard", "control")

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
        "text_at_issue": "Generated AGENTS.md, Source references: 'An editor key is rebound with `set_shortcut` (tool group "
                         "`editor_ux`), not in that source.'; list_shortcuts context: 'curve', 'color_picker'",
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
        "id": "bad_water_fork_role",
        "kind": "bad",
        "what_happened": "Forking library:water by copying its files left out the engine's water wiring (nothing floated, "
                         "Water.getHeight answered nil); fork_shader now copies a library shader with its manifest and @role.",
        "text_at_issue": "fork_shader: 'Copy a built-in shader, or a library one (source), ... material (the submeshes "
                         "drawing source, or submesh)'; source: 'keeps its manifest and @role'",
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
]
