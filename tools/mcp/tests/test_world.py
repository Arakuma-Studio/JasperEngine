"""W6 world tools: terrain brushes, tilemaps, sprites, sprite animations, keyframe animations,
colliders and joints, UI trees, particles, shape points, models.

Builds a small 2D platformer level (tilemap from an ASCII grid, an animated player sprite with a
Rigidbody 2D and a capsule collider), a UI panel with a button and text, and sculpts a 3D terrain;
every edit is checked and undone. Runs on a scratch copy of the TappyPlane fixture.
"""

import asyncio

from harness import Editor, ToolError

WORLD_TOOLS = (
    "sculpt_terrain", "sample_terrain", "edit_colliders", "set_joint", "edit_tilemap", "get_tilemap",
    "set_sprite_frames", "set_sprite_animation", "edit_animation", "get_animation", "preview_animation",
    "create_ui", "set_rect_layout", "create_particles", "get_model_info", "set_model", "set_shape_points",
    "set_terrain_maps",
)
SUPERSEDED = ("add_rigidbody_collider", "add_rigidbody2d_collider", "add_tilemap_tile", "remove_tilemap_tile",
              "create_terrain_heightmap", "create_terrain_blendmap", "set_terrain_textures", "add_terrain_collision",
              "duplicate_tilemap_tile", "add_animation_action", "remove_animation_action",
              "set_keyframe_times", "set_keyframe_easing")

LEVEL = """
..........
....==....
.@......@.
##########
"""

# Scene ids made by the tests, by kind
SCENES = {}


def approx(a, b, tol=0.02):
    return abs(float(a) - float(b)) <= tol * max(1.0, abs(float(b)))


async def new_scene(ed: Editor, kind: str, name: str):
    """Create and select the shared scene for this kind; return its id."""
    if kind not in SCENES:
        made = await ed.call("create_scene", name=name, type=kind)
        scene = made["scene"]
        ed.check(scene["selected"] and scene["name"].startswith(name), f"new scene is not selected: {scene!r}")
        SCENES[kind] = scene["id"]
    return SCENES[kind]


async def components(ed: Editor, scene, entity) -> set:
    result = await ed.call("get_entity", scene=scene, entity=entity, detail="summary")
    return set(result["components"])


async def exists(ed: Editor, scene, entity) -> bool:
    try:
        await ed.call("get_entity", scene=scene, entity=entity)
        return True
    except ToolError:
        return False


# -- registry ------------------------------------------------------------------


async def test_world_tools_listed(ed: Editor):
    missing = [t for t in WORLD_TOOLS if not ed.has_tool(t)]
    ed.check(not missing, f"world tools missing: {missing}")
    still = [t for t in SUPERSEDED if ed.has_tool(t)]
    ed.check(not still, f"superseded legacy tools still listed: {still}")


# -- 2D platformer level -----------------------------------------------------------


async def test_tilemap_from_ascii(ed: Editor):
    ed.require_tools("edit_tilemap", "get_tilemap")
    sid = await new_scene(ed, "2d", "Platformer")
    made = await ed.call("edit_tilemap", scene=sid, create={"name": "Level", "position": [0, 0, 0]},
                         texture="assets/sprites/plane-sheet.png",
                         tileset={"columns": 3, "rows": 1, "names": ["grass", "brick", "coin"]},
                         cell_size=[32, 32],
                         ops=[{"op": "paint", "grid": LEVEL, "legend": {"#": "grass", "=": "brick", "@": "coin"}}])
    level = made["tilemap"]
    ed.check_eq(made["created"], True)
    ed.check_eq(made["tile_rects"], 3, "tile rects from the 3x1 slice")
    ed.check_eq([r["name"] for r in made["rects"]], ["grass", "brick", "coin"])
    ed.check_eq(made["rects"][1]["rect"], [88, 0, 88, 73], "second slice rect in pixels")
    ed.check_eq(made["tiles"], 10 + 2 + 2, "one tile per painted character")
    lines = made["grid"]["lines"]
    ed.check_eq(lines, ["....==....", ".@......@.", "##########"], "ASCII view of the painted level")
    ed.check_eq(made["grid"]["rows"], [0, 2], "bottom line is row 0")

    # get_tilemap sees the same grid; legend characters map back to the rect names
    read = await ed.call("get_tilemap", scene=sid, entity=level["id"], include_tiles=True, limit=3)
    legend = read["grid"]["legend"]
    ed.check_eq(sorted(legend.values()), ["brick", "coin", "grass"])
    ed.check_eq(len(read["grid"]["lines"]), 3)
    ed.check_eq(read["tile_list"][0]["size"], [32, 32])
    ed.check_eq(read["texture"], "assets/sprites/plane-sheet.png")

    # fill + erase in one call, then undo restores the painted level
    edited = await ed.call("edit_tilemap", scene=sid, entity=level["id"],
                           ops=[{"op": "erase", "from": [1, 1], "to": [8, 1]},
                                {"op": "fill", "tile": "brick", "from": [0, 4], "to": [2, 4]}])
    ed.check_eq(edited["tiles"], 10 + 2 + 3)
    ed.check_eq(edited["erased"], 2)
    await ed.call("undo", scene=sid)
    back = await ed.call("get_tilemap", scene=sid, entity=level["id"])
    ed.check_eq(back["tiles"], 14, "undo restores the painted tiles")
    ed.check_eq(back["grid"]["lines"], ["....bb....", ".c......c.", "gggggggggg"], "get_tilemap names cells by their rects")

    # bad legend fails without changing anything
    err = await ed.call("edit_tilemap", scene=sid, entity=level["id"], ops=[{"op": "paint", "grid": "#X#", "legend": {"#": "grass"}}],
                        expect_error=True)
    ed.check_in("not in the legend", err)
    err = await ed.call("edit_tilemap", scene=sid, entity=level["id"], ops=[{"op": "fill", "tile": "lava", "from": [0, 0], "to": [1, 1]}],
                        expect_error=True)
    ed.check_in("grass", err, "unknown rect error lists the rects")
    still = await ed.call("get_tilemap", scene=sid, entity=level["id"])
    ed.check_eq(still["tiles"], 14, "failed edits change nothing")

    # undoing the creating call removes the tilemap; redo brings it back
    await ed.call("undo", scene=sid)
    ed.check(not await exists(ed, sid, level["id"]), "undo of the creating call removes the Tilemap")
    await ed.call("redo", scene=sid)
    again = await ed.call("get_tilemap", scene=sid, entity=level["id"])
    ed.check_eq(again["tiles"], 14, "redo restores the whole level")


async def test_player_sprite_animation_and_body(ed: Editor):
    ed.require_tools("set_sprite_frames", "set_sprite_animation", "edit_colliders")
    sid = await new_scene(ed, "2d", "Platformer")
    sprite = await ed.call("set_sprite_frames", scene=sid, create={"name": "Player", "position": [64, 96, 0]},
                           texture="assets/sprites/plane-sheet.png", slice={"columns": 3, "rows": 1}, pivot="center")
    player = sprite["sprite"]
    ed.check_eq(len(sprite["frames"]), 3)
    ed.check_eq(sprite["frames"][2]["rect"], [176, 0, 88, 73])
    ed.check_eq(sprite["size"], [88, 73], "the sprite takes the frame size")
    ed.check_eq(sprite["pivot"], "center")

    anim = await ed.call("set_sprite_animation", scene=sid, sprite=player["id"], name="Fly", fps=12,
                         frames=["frame_0", "frame_1", "frame_2", 1])
    ed.check_eq(anim["frames"], [0, 1, 2, 1])
    ed.check_eq(anim["frame_ms"], [83, 83, 83, 83])
    ed.check_eq(anim["autoplay"], True)
    fly = anim["animation"]
    ed.check_in("spriteanimation", await components(ed, sid, fly["id"]))
    retimed = await ed.call("set_sprite_animation", scene=sid, sprite=player["id"], animation=fly["id"], frames=[0, 1],
                            frame_ms=[100, 200], loop=False)
    ed.check_eq(retimed["created"], False)
    ed.check_eq(retimed["frame_ms"], [100, 200])
    await ed.call("undo", scene=sid)
    err = await ed.call("set_sprite_animation", scene=sid, sprite=player["id"], frames=[7], expect_error=True)
    ed.check_in("frames", err)

    # body + capsule fitted to the 88x73 sprite (centered pivot): horizontal capsule, radius 36.5
    body = await ed.call("edit_colliders", scene=sid, entity=player["id"], body_type="dynamic",
                         add=[{"shape": "capsule", "friction": 0.2}])
    ed.check_eq(body["dimension"], "2d")
    ed.check_eq(body["body_added"], True)
    ed.check_eq(body["body_type"], "dynamic")
    cap = body["colliders"][0]
    ed.check_eq(cap["shape"], "capsule")
    ed.check(approx(cap["radius"], 36.5), f"capsule radius fitted to the sprite: {cap}")
    ed.check(approx(cap["point_a"][0], -7.5) and approx(cap["point_b"][0], 7.5), f"capsule along x: {cap}")
    ed.check(approx(cap["friction"], 0.2), "friction")
    ed.check_in("rigidbody2d", await components(ed, sid, player["id"]))

    # add a feet sensor box, update the capsule, remove the box; list
    more = await ed.call("edit_colliders", scene=sid, entity=player["id"],
                         add=[{"shape": "box", "size": [40, 8], "center": [0, -36], "sensor": True}],
                         update=[{"index": 0, "restitution": 0.5}])
    ed.check_eq(len(more["colliders"]), 2)
    ed.check_eq(more["colliders"][1]["points"], [[-20, -40], [20, -40], [20, -32], [-20, -32]])
    ed.check_eq(more["colliders"][1]["sensor"], True)
    ed.check(approx(more["colliders"][0]["restitution"], 0.5))
    fewer = await ed.call("edit_colliders", scene=sid, entity=player["id"], remove=[1])
    ed.check_eq(len(fewer["colliders"]), 1)
    listed = await ed.call("edit_colliders", scene=sid, entity=player["id"])
    ed.check_eq(len(listed["colliders"]), 1)
    err = await ed.call("edit_colliders", scene=sid, entity=player["id"], remove=[5], expect_error=True)
    ed.check_in("out of range", err)

    # undo three collider calls: the Rigidbody 2D is gone, the sprite stays
    await ed.call("undo", scene=sid, steps=3)
    comps = await components(ed, sid, player["id"])
    ed.check("rigidbody2d" not in comps, f"undo removed the Rigidbody 2D: {comps}")
    ed.check_in("sprite", comps)
    await ed.call("redo", scene=sid)
    ed.check_in("rigidbody2d", await components(ed, sid, player["id"]))

    # a static ground body + a revolute joint between the two
    ground = await ed.call("edit_colliders", scene=sid, entity="Level", add=[{"shape": "box", "size": [320, 32], "center": [160, 16]}])
    ed.check_eq(ground["body_type"], "static")
    joint = await ed.call("set_joint", scene=sid, body_a=player["id"], body_b="Level", type="revolute", anchor_a=[0, 0])
    ed.check_eq(joint["dimension"], "2d")
    ed.check_eq(joint["type"], "revolute")
    ed.check_eq(joint["body_b"]["name"], "Level")
    ed.check_eq(joint["auto_anchors"], False)
    edited = await ed.call("set_joint", scene=sid, joint=joint["joint"]["id"], type="distance", rope=True)
    ed.check_eq(edited["created"], False)
    ed.check_eq(edited["type"], "distance")
    await ed.call("undo", scene=sid, steps=2)
    ed.check(not await exists(ed, sid, joint["joint"]["id"]), "undo removes the joint")


async def test_shape_points(ed: Editor):
    ed.require_tools("set_shape_points")
    sid = await new_scene(ed, "2d", "Platformer")
    ed.require_tools("create_entity")
    made = await ed.call("create_entity", name="Hill", type="mesh_polygon")
    hill = made["entity_id"]
    r = await ed.call("set_shape_points", scene=sid, entity=hill, points=[[0, 0], [100, 0], [100, 50], [50, 80], [0, 50]],
                      color=[0.2, 0.6, 0.2])
    ed.check_eq(r["component"], "mesh_polygon")
    ed.check_eq(r["count"], 5)
    circle = await ed.call("set_shape_points", scene=sid, entity=hill, circle={"radius": 40, "segments": 12})
    ed.check_eq(circle["count"], 12)
    await ed.call("undo", scene=sid, steps=2)
    err = await ed.call("set_shape_points", scene=sid, entity=hill, points=[[0, 0]], expect_error=True)
    ed.check_in("3 points", err)


# -- UI ------------------------------------------------------------------------------


async def test_ui_panel_with_button(ed: Editor):
    ed.require_tools("create_ui", "set_rect_layout")
    sid = await new_scene(ed, "ui", "Menu")
    tree = await ed.call("create_ui", scene=sid, root={
        "type": "panel", "name": "PauseMenu", "text": "Paused",
        "layout": {"anchor": "center", "size": [320, 220]},
        "children": [
            {"type": "text", "name": "Hint", "text": "Game paused", "font_size": 24, "text_color": [1, 1, 0.5, 1],
             "layout": {"anchor": "top_center", "position": [0, 50]}},
            {"type": "layout_group", "name": "Buttons", "direction": "vertical", "layout": {"anchor": "stretch", "margins": [20, 90, 20, 20]},
             "children": [
                 {"type": "button", "name": "Resume", "text": "Resume", "color": [0.2, 0.5, 0.9, 1],
                  "layout": {"size": [160, 40]}},
                 {"type": "progress_bar", "name": "Volume", "value": 0.75},
             ]},
        ]})
    root = tree["root"]
    ed.check_eq(tree["count"], 5)
    ed.check_eq(root["name"], "PauseMenu")
    ed.check_eq(root["type"], "panel")
    ed.check_eq(root["text_entity"]["name"], "HeaderText")
    buttons = root["children"][1]
    resume = buttons["children"][0]
    ed.check_eq(resume["path"], "PauseMenu/Buttons/Resume")
    ed.check_eq(resume["text_entity"]["name"], "Label")
    ed.check_in("button", await components(ed, sid, resume["id"]))
    ed.check_in("progressbar", await components(ed, sid, buttons["children"][1]["id"]))

    laid = await ed.call("set_rect_layout", scene=sid, items=[
        {"entity": root["id"], "anchor": "top_right", "size": [300, 200], "position": [-10, 10]},
        {"entity": "PauseMenu/Hint", "anchor": "stretch", "margins": 8},
    ])
    first = laid["items"][0]["layout"]
    ed.check_eq(first["anchor"], "top_right")
    ed.check_eq(first["size"], [300, 200])
    ed.check_eq(first["position_offset"], [-10, 10])
    hint = laid["items"][1]["layout"]
    ed.check_eq(hint["anchor_offsets"], [8, 8, -8, -8])
    ed.check_eq(hint["size"][0], 300 - 16, "stretch fills the parent's width minus the margins")
    ed.check(hint["size"][1] < 200 - 16 + 1, f"and its height minus the margins: {hint}")
    await ed.call("undo", scene=sid)

    err = await ed.call("create_ui", scene=sid, root={"type": "panel", "children": [{"type": "slider"}]}, expect_error=True)
    ed.check_in("button", err, "bad type lists the valid ones")
    ed.check(not await exists(ed, sid, "Panel"), "a failed create_ui leaves nothing behind")

    await ed.call("undo", scene=sid)
    ed.check(not await exists(ed, sid, root["id"]), "one undo removes the whole UI tree")
    ed.check(not await exists(ed, sid, resume["id"]), "including the children")


# -- 3D terrain, animation, particles --------------------------------------------------------


async def test_terrain_sculpt(ed: Editor):
    ed.require_tools("sculpt_terrain", "sample_terrain", "create_entity")
    sid = await new_scene(ed, "3d", "World")
    made = await ed.call("create_entity", name="Ground", type="terrain")
    terrain = made["entity_id"]

    # raise a hill at the centre: the height map is created (flat, middle level) by the stroke
    hill = await ed.call("sculpt_terrain", scene=sid, entity=terrain, strokes=[
        {"mode": "raise", "points": [[0, 0]], "size": 20, "strength": 1, "seconds_per_point": 0.4},
    ])
    ed.check_eq(hill["strokes"][0]["mode"], "raise")
    probe = await ed.call("sample_terrain", scene=sid, entity=terrain, points=[[0, 0], [80, 80]])
    centre, far = probe["samples"][0], probe["samples"][1]
    ed.check(centre["normalized"] > far["normalized"] + 0.2, f"the stroke raised the centre: {centre} vs {far}")
    ed.check(approx(far["normalized"], 0.5, 0.05), f"far away stays at the start level: {far}")
    grid = await ed.call("sample_terrain", scene=sid, entity=terrain, grid=5)
    ed.check_eq(len(grid["grid"]["world_heights"]), 5)
    ed.check(grid["grid"]["max"] > grid["grid"]["min"])

    # a flatten stroke and a ramp in one call = one undo step
    await ed.call("sculpt_terrain", scene=sid, entity=terrain, strokes=[
        {"mode": "flatten", "points": [[0, 0]], "size": 10, "strength": 1, "flatten_height": 0.5, "seconds_per_point": 2},
        {"mode": "smooth", "points": [[-20, 0], [20, 0]], "size": 8},
    ])
    flat = await ed.call("sample_terrain", scene=sid, entity=terrain, points=[[0, 0]])
    ed.check(flat["samples"][0]["normalized"] < centre["normalized"], "flatten lowered the peak")
    await ed.call("undo", scene=sid)
    again = await ed.call("sample_terrain", scene=sid, entity=terrain, points=[[0, 0]])
    ed.check(approx(again["samples"][0]["normalized"], centre["normalized"], 0.01), "undo restores the hill")
    await ed.call("undo", scene=sid)
    gone = await ed.call("sample_terrain", scene=sid, entity=terrain, points=[[0, 0]])
    ed.check(gone["samples"][0]["normalized"] < centre["normalized"] - 0.2, "undo of the first stroke removes the hill")

    # texture paint on layer 1 (creates the blend map)
    await ed.call("sculpt_terrain", scene=sid, entity=terrain, strokes=[{"mode": "paint_layer", "layer": 1, "points": [[0, 0]], "size": 10}])
    info = await ed.call("sample_terrain", scene=sid, entity=terrain, grid=0)
    ed.check(len(info["info"]["blend_maps"]) >= 1, f"painting made a blend map: {info['info']}")
    err = await ed.call("sculpt_terrain", scene=sid, entity=terrain, strokes=[{"mode": "dig", "points": [[0, 0]]}], expect_error=True)
    ed.check_in("raise", err)


async def test_terrain_maps(ed: Editor):
    ed.require_tools("set_terrain_maps", "sample_terrain", "create_entity")
    sid = await new_scene(ed, "3d", "World")
    made = await ed.call("create_entity", name="Maps", type="terrain")
    terrain = made["entity_id"]
    # a generated height map, a constant blend map and two layer textures: one undo step
    out = await ed.call("set_terrain_maps", scene=sid, entity=terrain,
                        heightmap={"generate": "fractal_noise", "resolution": 64, "seed": 7, "amplitude": 0.3},
                        blendmap={"color": [0, 1, 0], "resolution": 32},
                        layers=[{"index": 0, "color": "assets/sprites/rock.png"},
                                {"index": 1, "color": "assets/sprites/rock-down.png", "pbr": True}])
    hpath, bpath = out["heightmap"]["path"], out["blendmap"]["path"]
    ed.check((ed.project / hpath).exists() and (ed.project / bpath).exists(), f"maps written: {out!r}")
    ed.check_eq(out["heightmap"]["seed"], 7)
    ed.check_eq(out["info"]["height_map"], hpath)
    ed.check_eq(out["info"]["surface_layers"], 2)
    grid = await ed.call("sample_terrain", scene=sid, entity=terrain, grid=5)
    ed.check(grid["grid"]["max"] > grid["grid"]["min"], f"noise has relief: {grid['grid']}")
    await ed.call("undo", scene=sid)
    info = await ed.call("sample_terrain", scene=sid, entity=terrain, grid=0)
    ed.check(info["info"]["surface_layers"] == 0 and not info["info"]["blend_maps"], f"undo clears it: {info['info']}")
    # an existing image as the height map; errors name the bad field
    flat = await ed.call("set_terrain_maps", scene=sid, entity=terrain, heightmap=hpath)
    ed.check_eq(flat["info"]["height_map"], hpath)
    err = await ed.call("set_terrain_maps", scene=sid, entity=terrain, layers=[{"index": 0, "normal": "nope.png"}],
                        expect_error=True)
    ed.check_in("normal", err)
    err = await ed.call("set_terrain_maps", scene=sid, entity=terrain, heightmap={"generate": "mountains"}, expect_error=True)
    ed.check_in("fractal_noise", err)
    await ed.call("delete_entities", scene=sid, entity=terrain)


async def test_terrain_maps_rebuild_mesh(ed: Editor):
    """A height map set together with a blend map or layers in one call still rebuilds the terrain
    mesh and collision: the blend map / layers change ran after the height map's and used to clear
    the rebuild request, leaving the old mesh."""
    ed.require_tools("set_terrain_maps", "sample_terrain", "create_entity", "generate_placeholder_art")
    sid = await new_scene(ed, "3d", "World")
    layer_a, layer_b = "assets/terrain_rebuild/a.png", "assets/terrain_rebuild/b.png"
    for path, color in ((layer_a, [0.4, 0.4, 0.4, 1]), (layer_b, [0.2, 0.6, 0.2, 1])):
        await ed.call("generate_placeholder_art", path=path, width=8, height=8, color=color, overwrite=True)
    made = await ed.call("create_entity", name="Rebuilt", type="terrain")
    terrain = made["entity_id"]

    async def mesh_built() -> dict:
        info = {}
        for _ in range(50):
            info = (await ed.call("sample_terrain", scene=sid, entity=terrain, grid=0))["info"]
            if info.get("mesh_built"):
                break
            await asyncio.sleep(0.2)
        return info

    for seed, extra in ((3, {"blendmap": {"color": [1, 0, 0], "resolution": 16}}),
                        (4, {"layers": [{"index": 0, "color": layer_a}]}),
                        (5, {"blendmap": {"color": [0, 1, 0], "resolution": 16},
                             "layers": [{"index": 1, "color": layer_b}]})):
        out = await ed.call("set_terrain_maps", scene=sid, entity=terrain,
                            heightmap={"generate": "fractal_noise", "resolution": 32, "seed": seed}, **extra)
        info = await mesh_built()
        ed.check_eq(info.get("height_map"), out["heightmap"]["path"])
        ed.check(info.get("mesh_built"), f"the mesh was not rebuilt from the new height map (with {sorted(extra)}): {info}")
    await ed.call("delete_entities", scene=sid, entity=terrain)


async def test_keyframe_animation(ed: Editor):
    ed.require_tools("edit_animation", "get_animation", "preview_animation", "create_entity")
    sid = await new_scene(ed, "3d", "World")
    made = await ed.call("create_entity", name="Lift", type="box")
    box = made["entity_id"]
    anim = await ed.call("edit_animation", scene=sid, target=box, name="Bob", loop=True, autoplay=True, tracks=[
        {"channel": "position", "keys": [{"time": 0, "value": [0, 0, 0], "ease": "sine_in_out"}, {"time": 1, "value": [0, 2, 0]},
                                         {"time": 2, "value": [0, 0, 0]}]},
        {"channel": "rotation", "keys": [{"time": 0, "value": {"euler": [0, 0, 0]}}, {"time": 2, "value": {"euler": [0, 90, 0]}}]},
    ], tweens=[{"type": "scale", "from": [1, 1, 1], "to": [2, 2, 2], "duration": 0.5, "ease": "back_out", "start_time": 2}])
    ed.check_eq(anim["created"], True)
    ed.check_eq(anim["loop"], True)
    ed.check_eq(anim["autoplay"], True)
    kinds = [a["kind"] for a in anim["actions"]]
    ed.check_eq(kinds, ["position_track", "rotation_track", "scale_tween"])
    pos = anim["actions"][0]
    ed.check_eq([k["time"] for k in pos["keys"]], [0, 1, 2])
    ed.check_eq(pos["keys"][0]["ease"], "sine_in_out")
    ed.check_eq(pos["keys"][1]["ease"], "linear")
    ed.check_eq(anim["actions"][2]["tween"]["ease"], "back_out")
    ed.check(approx(anim["length"], 2.5), f"length: {anim['length']}")
    bob = anim["animation"]

    # merge a key into the position track; rewrite timing; read back
    merged = await ed.call("edit_animation", scene=sid, animation=bob["id"], tracks=[
        {"channel": "position", "mode": "merge", "keys": [{"time": 0.5, "value": [0, 1.5, 0]}]}],
        set_frames=[{"index": 2, "start_time": 2.5}])
    ed.check_eq([k["time"] for k in merged["actions"][0]["keys"]], [0, 0.5, 1, 2])
    ed.check_eq(merged["actions"][2]["start_time"], 2.5)
    read = await ed.call("get_animation", scene=sid, entity=box)
    ed.check_eq(len(read["animations"]), 1, "get_animation on the target finds its animation")

    # preview: seek poses the target, stop restores it
    seek = await ed.call("preview_animation", scene=sid, animation=bob["id"], action="seek", time=1.0)
    ed.check(approx(seek["target_transform"]["position"][1], 2.0, 0.05), f"seek to 1s puts the box at y=2: {seek}")
    stop = await ed.call("preview_animation", scene=sid, animation=bob["id"], action="stop")
    ed.check(approx(stop["target_transform"]["position"][1], 0.0, 0.01), f"stop restores the authored pose: {stop}")
    play = await ed.call("preview_animation", scene=sid, animation=bob["id"], action="play", wait_ms=400)
    ed.check(play["playing"] and play["previewing"], f"play runs the preview: {play}")
    ed.check(play["target_transform"]["position"][1] > 0.05, f"playing moved the box: {play}")
    stop = await ed.call("preview_animation", scene=sid, animation=bob["id"], action="stop")
    ed.check(approx(stop["target_transform"]["position"][1], 0.0, 0.01), "stop restores it again")

    # remove the tween (and its GameObject), then undo everything back to no animation
    tween = merged["actions"][2]["action"]["id"]
    removed = await ed.call("edit_animation", scene=sid, animation=bob["id"], remove_actions=[2])
    ed.check_eq(len(removed["actions"]), 2)
    ed.check(not await exists(ed, sid, tween), "removed tween GameObject deleted")
    await ed.call("undo", scene=sid)
    ed.check(await exists(ed, sid, tween), "undo brings the tween back")
    await ed.call("undo", scene=sid, steps=2)
    ed.check(not await exists(ed, sid, bob["id"]), "undo of the creating call removes the animation")
    err = await ed.call("edit_animation", scene=sid, target=box, tracks=[{"channel": "position", "keys": [{"time": 0, "value": [1, 2]}]}],
                        expect_error=True)
    ed.check_in("[x, y, z]", err)


async def test_relative_looping_tracks_and_spin(ed: Editor):
    ed.require_tools("edit_animation", "get_animation", "preview_animation", "create_entity")
    sid = await new_scene(ed, "3d", "World")
    made = await ed.call("create_entity", name="Bobber", type="cube", position=[4, 1, 0])
    box = made["entity_id"]
    anim = await ed.call("edit_animation", scene=sid, target=box, name="Bob", tracks=[
        {"channel": "position", "relative": True, "loop": True,
         "keys": [{"time": 0, "value": [0, 0, 0]}, {"time": 1, "value": [0, 2, 0]}]},
    ], tweens=[{"type": "rotation", "spin": True, "axis": [0, 1, 0], "angle": 360, "duration": 1, "ping_pong": True}])
    track, spin = anim["actions"][0], anim["actions"][1]
    ed.check(track["relative"] and track["loop"], f"track flags: {track}")
    ed.check_eq(spin["kind"], "rotation_tween")
    ed.check(spin["tween"]["spin"] and spin["tween"]["ping_pong"], f"tween flags: {spin}")
    ed.check(approx(spin["tween"]["angle"], 360) and spin["tween"]["axis"] == [0, 1, 0], f"spin: {spin}")
    ed.check(approx(spin["effective_duration"], 2.0), f"ping-pong takes twice the duration: {spin}")
    bob = anim["animation"]

    # relative: offsets from where the box is, so it keeps x 4 and rises from y 1
    seek = await ed.call("preview_animation", scene=sid, animation=bob["id"], action="seek", time=1.0)
    pos = seek["target_transform"]["position"]
    ed.check(approx(pos[0], 4.0) and approx(pos[1], 3.0, 0.05), f"relative track at 1s: {seek}")
    # a full turn of spin is turned about 264 degrees at 0.75 s (a tween to the same end rotation would not turn)
    seek = await ed.call("preview_animation", scene=sid, animation=bob["id"], action="seek", time=0.75)
    quat = seek["target_transform"]["rotation"]["quat"]
    ed.check(abs(quat[1]) > 0.5, f"spinning around y at 0.75s: {seek}")
    stop = await ed.call("preview_animation", scene=sid, animation=bob["id"], action="stop")
    ed.check(approx(stop["target_transform"]["position"][1], 1.0, 0.01), f"stop restores the authored pose: {stop}")

    # flags on an existing track; morph tracks cannot be relative; a spin needs no `to`
    edited = await ed.call("edit_animation", scene=sid, animation=bob["id"], tracks=[
        {"channel": "position", "mode": "merge", "relative": False, "keys": [{"time": 0.5, "value": [0, 1, 0]}]}])
    ed.check(not edited["actions"][0]["relative"] and edited["actions"][0]["loop"], f"relative off, loop kept: {edited['actions'][0]}")
    err = await ed.call("edit_animation", scene=sid, animation=bob["id"], tracks=[
        {"channel": "morph", "relative": True, "keys": [{"time": 0, "value": [0]}]}], expect_error=True)
    ed.check_in("relative", err)
    err = await ed.call("edit_animation", scene=sid, animation=bob["id"], tweens=[{"type": "scale", "spin": True, "to": [1, 1, 1]}],
                        expect_error=True)
    ed.check_in("spin", err)
    await ed.call("undo", scene=sid, steps=2)
    ed.check(not await exists(ed, sid, bob["id"]), "undo removes the animation")
    await ed.call("delete_entities", scene=sid, entity=box)


async def test_particles(ed: Editor):
    ed.require_tools("create_particles")
    sid = await new_scene(ed, "3d", "World")
    fire = await ed.call("create_particles", scene=sid, name="Campfire", preset="fire", position=[0, 0, 5])
    ed.check_eq(fire["created"], True)
    ed.check_eq(fire["autoplay"], True)
    ed.check(fire["gradient_stops"] == 3 and fire["rate"] == 60, f"fire preset: {fire}")
    system = fire["particle_system"]
    ed.check_in("particlesystem", await components(ed, sid, system["id"]))
    ed.check_eq(fire["emitter"]["id"], system["id"], "the Particle System GameObject emits and draws itself")
    boom = await ed.call("create_particles", scene=sid, entity=system["id"], preset="explosion", bursts=[{"time": 0, "count": [50, 80]}])
    ed.check_eq(boom["loop"], False)
    ed.check_eq(boom["bursts"], 1)
    tweak = await ed.call("create_particles", scene=sid, entity=system["id"], rate=7, life=[1, 3])
    ed.check_eq(tweak["rate"], 7)
    ed.check_eq(tweak["life"], [1, 3])
    ed.check_eq(tweak["bursts"], 1, "overrides without a preset keep the other settings")
    await ed.call("undo", scene=sid, steps=3)
    ed.check(not await exists(ed, sid, fire["emitter"]["id"]), "undo removes the effect")


async def test_3d_bodies_and_joint(ed: Editor):
    ed.require_tools("edit_colliders", "set_joint", "create_entity")
    sid = await new_scene(ed, "3d", "World")
    made = await ed.call("create_entity", name="Crate", type="box")
    crate = made["entity_id"]
    body = await ed.call("edit_colliders", scene=sid, entity=crate, body_type="dynamic",
                         add=[{"shape": "box"}, {"shape": "sphere", "radius": 0.25, "center": [0, 1, 0], "density": 500}],
                         body={"gravityFactor": 0.5})
    ed.check_eq(body["dimension"], "3d")
    box, ball = body["colliders"]
    ed.check_eq(box["shape"], "box")
    ed.check(all(approx(v, 1.0) for v in box["size"]), f"box fitted to the unit cube mesh: {box}")
    ed.check_eq(ball["center"], [0, 1, 0])
    ed.check(approx(ball["radius"], 0.25) and approx(ball["density"], 500))
    made = await ed.call("create_entity", name="Post", type="box")
    post = made["entity_id"]
    await ed.call("edit_colliders", scene=sid, entity=post, add=[{"shape": "box"}])
    hinge = await ed.call("set_joint", scene=sid, body_a=post, body_b=crate, type="hinge", anchor=[0, 0.5, 0], axis=[0, 0, 1],
                          limits=[-45, 45], properties={"twistMinAngle": -30})
    ed.check_eq(hinge["dimension"], "3d")
    ed.check_eq(hinge["type"], "hinge")
    ed.check_eq(hinge["body_a"]["name"], "Post")
    err = await ed.call("set_joint", scene=sid, body_a=post, type="hinge", expect_error=True)
    ed.check_in("body_b", err)

    # heightfield collision on a terrain
    made = await ed.call("create_entity", name="Hills", type="terrain")
    hills = made["entity_id"]
    field = await ed.call("edit_colliders", scene=sid, entity=hills, add=[{"shape": "heightfield", "samples_size": 128}])
    ed.check_eq(field["colliders"][0]["shape"], "heightfield")
    ed.check_eq(field["colliders"][0]["samples_size"], 128)
    await ed.call("undo", scene=sid, steps=7)
    ed.check(not await exists(ed, sid, crate), "undo removes the crate")


async def test_models_errors(ed: Editor):
    ed.require_tools("get_model_info", "set_model", "create_entity")
    sid = await new_scene(ed, "3d", "World")
    err = await ed.call("get_model_info", scene=sid, entity="Lift", expect_error=True)
    ed.check_in("no Model", err)
    err = await ed.call("set_model", scene=sid, create={"name": "Ship"}, path="assets/missing.glb", expect_error=True)
    ed.check_in("missing.glb", err)


async def test_screenshot(ed: Editor):
    ed.require_tools("screenshot")
    data = await ed.screenshot(view="scene")
    ed.check(len(data) > 1000, "screenshot has content")
