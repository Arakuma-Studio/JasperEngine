# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""The eval's cases: one decision point each (README.md). Paraphrased from real sessions and
fixes, with no user data. Results are in the editor's real shapes: {"recorded": key} reuses a
result dump_surface.py recorded from a built editor; hand-written ones copy the server's text.
"""

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


def ok(text, structured=None):
    return {"text": text, "structured": structured}


def err(text):
    return {"text": text, "is_error": True}


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
    {
        "id": "guard_navigation_door_in_play",
        "playing": True,
        "kind": "guard",
        "what_happened": "S55 runtime areas: a door is a NavMesh Modifier with Apply At Runtime and a bridge a NavMesh Link "
                         "with Activated; in play the agent toggles them with set_runtime_property and reads the path again, "
                         "instead of rebaking (bake_navigation is refused in play) or editing the scene.",
        "text_at_issue": "docs/mcp/guide/navigation.md 'Doors and bridges in play'; get_navigation_info area_overrides / "
                         "links[].activated; visualize_navigation show.area_overrides",
        "source": "docs/specs/S55-navigation-gameplay-api.md R45 (no failure on record)",
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

    # -- S54 navigation diagnostics: the new tool text (no failure on record yet) ----------------
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
        "source": "S54 R30 (docs/specs/S54-navigation-diagnostics.md); no failure on record",
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
        "source": "S54 R13, R32 (docs/specs/S54-navigation-diagnostics.md); no failure on record",
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
        "source": "S54 R33 (docs/specs/S54-navigation-diagnostics.md); no failure on record",
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
        "source": "S54 R30 (docs/specs/S54-navigation-diagnostics.md); no failure on record",
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
        "what_happened": "S55 R42 added query_navigation's random op (around `from` within max_distance, seeded); guards that "
                         "agents use it instead of sampling guessed points or writing Lua.",
        "text_at_issue": "query_navigation: '... locate (...), random, move_along'; from: 'Optional for random.'; max_distance: "
                         "'Default: sample 2, locate 5. closest_edge, random.'; seed: 'random.'",
        "source": "docs/specs/S55-navigation-gameplay-api.md R42 (new op; no failure on record)",
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
        "what_happened": "S55 R42 added query_navigation's move_along (a controller step along the surface, blocked by walls); "
                         "guards that agents use it rather than a raycast or a path for a one-step move.",
        "text_at_issue": "query_navigation: '... locate (...), random, move_along'",
        "source": "docs/specs/S55-navigation-gameplay-api.md R42 (new op; no failure on record)",
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
]
