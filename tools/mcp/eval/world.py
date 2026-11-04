# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""The project every sample sees, as the editor would report it: one open scene "Main"
(scenes/Main.scene, the start scene) with a camera, ground, player, gun, enemy and crate.

default_mocks() goes after a case's own mocks (the first match wins), so a case only mocks the
calls its decision is about. Texts follow the real results dump_surface.py recorded; a few reuse
them verbatim ({"recorded": key}).
"""

SCENE = {"id": 1, "name": "Main", "path": "scenes/Main.scene"}
ENTITIES = [
    (10, "Main Camera", "camera transform"),
    (11, "Ground", "mesh rigidbody transform"),
    (12, "Player", "script sprite transform"),
    (13, "Player/Gun", "sprite transform"),
    (18, "Player/Shield", "sprite transform"),
    (14, "Enemy", "script sprite transform"),
    (15, "Crate", "mesh transform"),
]
TREE = ("Main Camera [10] camera\nGround [11] mesh rigidbody\nPlayer [12] script sprite\n  Gun [13] sprite\n"
        "  Shield [18] sprite\nEnemy [14] script sprite\nCrate [15] mesh\nDirectional Light [16] light\n"
        "Skybox [17] skybox (no transform)\n")
# what get_entity shows of each component (the real result lists every property; these are the ones cases touch)
COMPONENTS = {
    "transform": lambda eid: {"position": {11: [0, -0.5, 0], 15: [0, 3, 0], 10: [0, 5, -10]}.get(eid, [0, 0, 0]),
                              "rotation": [0, 0, 0, 1], "scale": {11: [20, 1, 20], 15: [2, 2, 2]}.get(eid, [1, 1, 1]),
                              "layer": 0, "visible": True},
    "mesh": lambda eid: {"numSubmeshes": 1, "castShadows": True,
                         "submeshes[0].material": {"shader": "library:lit", "baseColorFactor": [1, 1, 1, 1]}},
    "rigidbody": lambda eid: {"type": "static", "colliders": [{"shape": "box", "half_extents": [0.5, 0.5, 0.5]}]},
    "sprite": lambda eid: {"texture": "assets/sprites/ship.png", "width": 64, "height": 64},
    "script": lambda eid: {"scripts": [{"path": "scripts/player.lua", "properties": {"speed": 5}}]},
    "camera": lambda eid: {"type": "perspective", "yfov": 0.75, "useTarget": False},
}


def _ok(text, structured=None):
    return {"text": text, "structured": structured}


def _err(text):
    return {"text": text, "is_error": True}


def _entity(eid, path, comps):
    name = path.split("/")[-1]
    values = {c: COMPONENTS[c](eid) for c in comps.split()}
    # shaped like the real get_entity (dump_surface.py recorded one), so it goes out as structuredContent
    return {**_ok(f"{name} [{eid}] components: {comps}",
                  {"id": eid, "name": name, "path": path, "active": True, "active_in_hierarchy": True, "layer": 0,
                   "tag": "", "scene_id": 1, "child_count": 0, "components": values}), "real": True}


def default_mocks(playing: bool = False) -> list[dict]:
    mocks = [
        {"tool": "editor_status", "result": _ok(
            f"Project \"Space Dodge\" at the current directory. 1 scene(s), selected: 1 \"Main\". "
            f"{'Playing' if playing else 'Edit mode'}.",
            {"project": {"name": "Space Dodge", "path": "."}, "playing": playing,
             "scenes": [{**SCENE, "selected": True, "start_scene": True}], "jobs": [], "dialog": None})},
        {"tool": "list_scenes", "result": _ok(
            "1 scene(s):\n1 \"Main\" 3D scenes/Main.scene open selected start",
            {"scenes": [{**SCENE, "type": "3D", "opened": True, "selected": True, "start_scene": True,
                         "modified": False, "entity_count": 8}], "selected_scene_id": 1})},
        {"tool": "get_scene_tree", "result": _ok(
            "Scene 1 \"Main\" (3D, 8 GameObjects). Each line: Name [id] components (flags); children indented.\n" + TREE,
            {"entity_count": 8, "scene_id": 1, "scene_name": "Main", "tree": TREE, "truncated": False})},
        {"tool": "find_entities", "args": {"name": "re:(?i)player"}, "result": _ok(
            "1 GameObject(s) match:\nPlayer [12] script sprite", {"entities": [{"id": 12, "path": "Player"}]})},
        {"tool": "find_entities", "result": _ok(
            "6 GameObject(s) match:\n" + "\n".join(f"{p} [{i}] {c}" for i, p, c in ENTITIES),
            {"entities": [{"id": i, "path": p} for i, p, _ in ENTITIES]})},
    ]
    for eid, path, comps in ENTITIES:
        name = path.split("/")[-1]
        for ref in (f"re:^({eid}|{path}|{name})$",):
            mocks.append({"tool": "get_entity", "args": {"entity": ref}, "result": _entity(eid, path, comps)})
    mocks += [
        {"tool": "read_console", "args": {"since": "*"}, "result": _ok(
            "0 entries (0 error(s), 0 warning(s)).\nnext_cursor: 30", {"entries": [], "next_cursor": 30})},
        {"tool": "read_console", "result": _ok(
            "3 entries (0 error(s), 0 warning(s)):\n[info #1] [gpu] shader creation: parallel\n"
            "[info #3] Opened project \"Space Dodge\"\n[info #9] Synced engine API in project\nnext_cursor: 30",
            {"entries": [{"id": 1, "level": "info", "message": "[gpu] shader creation: parallel"}], "next_cursor": 30})},
        {"tool": "get_play_state", "after": "play", "result": _ok(
            "Play state: playing (\"Main\"). Session: 3 s, 0 error(s) logged. Script coroutines: 0, timers: 0.",
            {"state": "playing"})},
        {"tool": "get_play_state", "result": _ok(
            "Play state: playing (\"Main\"). Session: 12 s, 0 error(s) logged. Script coroutines: 0, timers: 0."
            if playing else "Play state: stopped.", {"state": "playing" if playing else "stopped"})},
        {"tool": "get_project_settings", "result": {"recorded": "project_settings_all"}},
        {"tool": "search_engine_api", "args": {"query": "re:(?i)rotat"}, "result": {"recorded": "search_api_rotate"}},
        {"tool": "search_engine_api", "result": {"recorded": "search_api_events"}},
        {"tool": "save_scene", "result": _ok("Saved scene 1 \"Main\" (scenes/Main.scene).", {"scene_id": 1})},
        {"tool": "screenshot", "result": _ok("Game view 1280x720 (scaled to 1024x576).", {"view": "game", "width": 1280,
                                                                                          "height": 720})},
        {"tool": "check_script", "result": _ok("No problems.", {"ok": True, "problems": []})},
        {"tool": "list_component_types", "result": {"recorded": "list_component_types"}},
        {"tool": "get_scene_settings", "result": {"recorded": "get_scene_settings"}},
        {"tool": "list_post_processing_presets", "result": _ok(
            "8 presets: none, mobile, mobile_adaptive, console, stylised, crt_retro, film, vhs (each: the post_processing "
            "fields it sets and its cost)", {"presets": [{"id": p} for p in ("none", "mobile", "mobile_adaptive", "console",
                                                                            "stylised", "crt_retro", "film", "vhs")]})},
        {"tool": "select_scene", "result": _ok("Selected scene 1 \"Main\".", {"scene_id": 1})},
        {"tool": "set_project_settings", "result": _ok("Set 1 project setting(s); saved project.yaml.", {"changed": []})},
    ]
    if playing:
        mocks += [
            {"tool": "eval_lua", "result": _ok("=> null", {"values": [None], "printed": [], "console": []})},
            {"tool": "wait", "result": _ok("Waited 2.00 s, 120 frame(s), game time +2.000 s. Playing.",
                                          {"reason": "seconds", "state": "playing", "console": []})},
            {"tool": "play", "args": {"action": "stop"}, "result": _ok(
                "Stopped. \"Main\" played 12 s and was stopped (0 error(s), 0 warning(s) logged). The scene is back to "
                "its edit-time state.", {"state": "stopped"})},
        ]
    else:
        mocks += [
            {"tool": "eval_lua", "args": {"allow_edit_mode": True}, "result": _ok(
                "=> null\n(edit mode: changes to the scene are not undoable)", {"values": [None]})},
            {"tool": "eval_lua", "result": {"recorded": "eval_lua_edit_mode"}},
            {"tool": "play", "args": {"action": "start"}, "result": _ok(
                "Playing \"Main\". Started in 0.31 s. No errors in the console. next_cursor: 30", {"running_scenes": [{"id": 1, "main": True, "name": "Main"}],
                                                     "problems": [], "next_cursor": 30})},
            {"tool": "wait", "after": "play", "result": _ok("Waited 2.00 s, 120 frame(s), game time +2.000 s. Playing.",
                                                           {"reason": "seconds", "state": "playing", "console": []})},
            {"tool": "wait", "result": _err("game_seconds needs the game running (play start first).")},
            {"tool": "play", "args": {"action": "stop"}, "result": _ok(
                "Stopped. \"Main\" played 3 s and was stopped (0 error(s), 0 warning(s) logged). The scene is back to "
                "its edit-time state.", {"state": "stopped"})},
        ]
    for eid, path, comps in ENTITIES:
        name = path.split("/")[-1]
        values = {c: COMPONENTS[c](eid) for c in comps.split() if c in ("transform", "rigidbody")}
        if eid == 15:
            values["transform"]["position"] = [0, 1.0, 0]  # the Crate after falling onto the Ground
        mocks.append({"tool": "get_runtime_entity", "args": {"entity": f"re:^({eid}|{path}|{name})$"},
                      "after": None if playing else "play", "result": _ok(
                          f"{name} [{eid}] (running) " + ", ".join(f"{k} {v}" for k, v in values.items()),
                          {"id": eid, "path": path, "components": values})})
    return mocks
