"""Domain authoring contracts on independent, minimal scenes and images."""

from harness import Editor, ToolError
import pngdiff

EDITOR = {"create": True, "name": "World Contracts"}
CI = True


def image_file(ed: Editor, relative: str):
    path = ed.project / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(pngdiff.encode(8, 4, bytes([255, 255, 255, 255]) * 32))


async def scene(ed: Editor, kind: str, name: str):
    return (await ed.call("create_scene", name=name, type=kind))["scene"]["id"]


async def components(ed: Editor, scene, entity) -> set:
    result = await ed.call("get_entity", scene=scene, entity=entity, detail="summary")
    return set(result["components"])


async def exists(ed: Editor, scene, entity) -> bool:
    try:
        await ed.call("get_entity", scene=scene, entity=entity)
        return True
    except ToolError:
        return False


async def test_tilemap_from_ascii(ed: Editor):
    sid = await scene(ed, "2d", "Grid")
    texture = "assets/grid.png"
    image_file(ed, texture)
    made = await ed.call("edit_tilemap", scene=sid, create={"name": "Tiles"}, texture=texture,
                         tileset={"columns": 2, "rows": 1, "names": ["grass", "stone"]}, cell_size=[4, 4],
                         ops=[{"op": "paint", "grid": "ab\nb.", "legend": {"a": "grass", "b": "stone"}}])
    tilemap = made["tilemap"]["id"]
    actual = await ed.call("get_tilemap", scene=sid, entity=tilemap, include_tiles=True)
    cells = {(tuple(tile["position"][:2]), tile["rect"]) for tile in actual["tile_list"]}
    ed.check_eq(cells, {((0, 4), 0), ((4, 4), 1), ((0, 0), 1)}, "ASCII top line maps to the highest row")
    await ed.call("edit_tilemap", scene=sid, entity=tilemap,
                  ops=[{"op": "erase", "from": [0, 1], "to": [1, 1]},
                       {"op": "fill", "tile": "grass", "from": [1, 0], "to": [1, 0]}])
    await ed.call("undo", scene=sid)
    restored = await ed.call("get_tilemap", scene=sid, entity=tilemap, include_tiles=True)
    ed.check_eq({(tuple(tile["position"][:2]), tile["rect"]) for tile in restored["tile_list"]}, cells,
                "one undo restores all operations in the call")
    await ed.check_error("edit_tilemap", scene=sid, entity=tilemap,
                         ops=[{"op": "clear"}, {"op": "paint", "grid": "X", "legend": {}}], contains="legend")
    unchanged = await ed.call("get_tilemap", scene=sid, entity=tilemap, include_tiles=True)
    ed.check_eq({(tuple(tile["position"][:2]), tile["rect"]) for tile in unchanged["tile_list"]}, cells,
                "an invalid later operation rolls back earlier edits")
    await ed.call("undo", scene=sid)
    removed = await ed.call("find_entities", scene=sid, name="Tiles")
    ed.check_eq(removed["count"], 0, "a refused edit adds no undo entry before the creation")


async def test_sprite_animation_frame_names_and_fps(ed: Editor):
    sid = await scene(ed, "2d", "SpriteFrames")
    texture = "assets/frames.png"
    image_file(ed, texture)
    sprite = await ed.call("set_sprite_frames", scene=sid, create={"name": "Animated"}, texture=texture,
                           slice={"columns": 2, "rows": 1, "names": ["left", "right"]}, inline=True)
    animation = await ed.call("set_sprite_animation", scene=sid, sprite=sprite["sprite"]["id"],
                              frames=["right", "left"], fps=20)
    ed.check_eq(animation["frames"], [1, 0], "frame names resolve in the authored sheet order")
    ed.check_eq(animation["frame_ms"], [50, 50], "20 frames per second is 50 milliseconds per frame")


async def test_terrain_sculpt(ed: Editor):
    sid = await scene(ed, "3d", "TerrainEdits")
    terrain = (await ed.call("create_entity", scene=sid, name="Ground", type="terrain",
                             values={"terrain": {"terrainSize": 16, "maxHeight": 4,
                                                   "resolution": 8, "rootGridSize": 1, "levels": 1}}))["entity_id"]
    await ed.call("set_terrain_maps", scene=sid, entity=terrain,
                  heightmap={"generate": "flat", "base_height": 0.25, "resolution": 16})

    async def heights():
        samples = await ed.call("sample_terrain", scene=sid, entity=terrain, points=[[0, 0], [6, 6]])
        return [sample["normalized"] for sample in samples["samples"]]

    before = await heights()
    ed.check(all(abs(value - 0.25) < 0.001 for value in before), f"authored quarter-height map: {before}")
    await ed.call("sculpt_terrain", scene=sid, entity=terrain,
                  strokes=[{"mode": "raise", "points": [[0, 0]], "size": 3, "strength": 1,
                            "falloff": "constant", "seconds_per_point": 0.2}])
    raised = await heights()
    ed.check(raised[0] > 0.30 and abs(raised[1] - 0.25) < 0.001, f"brush raises only its region: {raised}")
    await ed.call("undo", scene=sid)
    restored = await heights()
    ed.check(all(abs(value - 0.25) < 0.001 for value in restored), f"undo restores height pixels: {restored}")

    await ed.call("set_terrain_maps", scene=sid, entity=terrain,
                  heightmap={"generate": "fractal_noise", "resolution": 16, "seed": 7, "amplitude": 0.3},
                  blendmap={"color": [1, 0, 0], "resolution": 4})
    noisy = await ed.call("sample_terrain", scene=sid, entity=terrain, grid=4)
    ed.check(noisy["grid"]["max"] > noisy["grid"]["min"], "fractal-noise maps have relief")
    await ed.call("undo", scene=sid)
    restored = await heights()
    ed.check(all(abs(value - 0.25) < 0.001 for value in restored), f"combined map update is one undo: {restored}")
    info = (await ed.call("sample_terrain", scene=sid, entity=terrain, grid=0))["info"]
    ed.check_eq(info["blend_maps"], [], "undo also removes the newly assigned blend map")


async def test_relative_looping_tracks_and_spin(ed: Editor):
    sid = await scene(ed, "3d", "Preview")
    target = (await ed.call("create_entity", scene=sid, name="Mover", type="box", position=[4, 1, 0]))["entity_id"]
    made = await ed.call("edit_animation", scene=sid, target=target,
                         tracks=[{"channel": "position", "relative": True,
                                  "keys": [{"time": 0, "value": [0, 0, 0]}, {"time": 1, "value": [0, 2, 0]}]}],
                         tweens=[{"type": "rotation", "spin": True, "axis": [0, 1, 0], "angle": 360, "duration": 1}])
    animation = made["animation"]["id"]
    try:
        preview = await ed.call("preview_animation", scene=sid, animation=animation, action="seek", time=0.75)
        position = preview["target_transform"]["position"]
        ed.check(all(abs(a - b) < 0.01 for a, b in zip(position, [4, 2.5, 0])), f"relative translation uses the authored base: {position}")
        ed.check(abs(preview["target_transform"]["rotation"]["quat"][1]) > 0.5, "a full-turn tween rotates instead of taking the identity shortcut")
    finally:
        stopped = await ed.call("preview_animation", scene=sid, animation=animation, action="stop")
    ed.check(all(abs(a - b) < 0.001 for a, b in zip(stopped["target_transform"]["position"], [4, 1, 0])),
             "stopping preview restores the authored transform")


async def test_ui_panel_with_button(ed: Editor):
    sid = await scene(ed, "ui", "TreeUndo")
    original = {entity["id"] for entity in (await ed.call("find_entities", scene=sid))["entities"]}
    await ed.call("create_ui", scene=sid, root={
        "type": "container", "name": "UndoTreeRoot", "children": [
            {"type": "button", "name": "UndoTreeButton", "text": "Go"},
            {"type": "container", "name": "UndoTreeBranch", "children": [
                {"type": "text", "name": "UndoTreeText", "text": "Ready"}]},
        ]})
    before = await ed.call("find_entities", scene=sid, name="UndoTree*")
    ed.check({"UndoTreeRoot", "UndoTreeButton", "UndoTreeBranch", "UndoTreeText"}
             <= {entity["name"] for entity in before["entities"]}, "the authored tree exists before undo")
    await ed.call("undo", scene=sid)
    after = await ed.call("find_entities", scene=sid)
    ed.check_eq({entity["id"] for entity in after["entities"]}, original,
                "one undo restores every original entity and removes all generated descendants")
async def test_particles(ed: Editor):
    ed.require_tools("create_particles")
    sid = await scene(ed, "3d", "World")
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
    sid = await scene(ed, "3d", "JointConstraint")
    await ed.call("set_scene_settings", scene=sid, settings={"physics_gravity_3d": [0, -10, 0]})
    for name, position, body_type in (("JointPost", [0, 0, 0], "static"),
                                      ("JointCrate", [0, 4, 0], "dynamic"),
                                      ("FreeCrate", [3, 4, 0], "dynamic")):
        entity = (await ed.call("create_entity", scene=sid, name=name, type="box", position=position))["entity_id"]
        await ed.call("edit_colliders", scene=sid, entity=entity, body_type=body_type, add=[{"shape": "box"}])
    await ed.call("set_joint", scene=sid, type="fixed", body_a="JointPost", body_b="JointCrate")
    try:
        await ed.call("play", action="start", scene=sid, fixed_fps=60, auto_pause=True)
        await ed.call("wait", frames=30)
        heights = (await ed.call("eval_lua", scene=sid,
                                code="return object('JointCrate').position.y, object('FreeCrate').position.y"))["values"]
        ed.check(abs(heights[0] - 4) < 0.05, f"the authored fixed joint holds its dynamic body: {heights}")
        ed.check(heights[1] < 3.5, f"the unconstrained control falls under the same real simulation: {heights}")
    finally:
        await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)


async def test_tilemap_past_old_capacity(ed: Editor):
    """A tilemap holds as many tiles as it draws (16,383), not the old 2,048 (mesh-memory D7)."""
    sid = await scene(ed, "2d", "BigGrid")
    texture = "assets/big-grid.png"
    image_file(ed, texture)
    grid = "\n".join(["a" * 70] * 30)
    made = await ed.call("edit_tilemap", scene=sid, create={"name": "Big"}, texture=texture,
                         tileset={"columns": 2, "rows": 1, "names": ["grass", "stone"]}, cell_size=[4, 4],
                         ops=[{"op": "paint", "grid": grid, "legend": {"a": "grass"}}])
    tilemap = made["tilemap"]["id"]
    actual = await ed.call("get_tilemap", scene=sid, entity=tilemap)
    ed.check_eq(actual["tiles"], 2100, "2,100 tiles are kept")
    over = "\n".join(["a" * 128] * 128)
    await ed.check_error("edit_tilemap", scene=sid, entity=tilemap,
                         ops=[{"op": "paint", "grid": over, "legend": {"a": "grass"}}], contains="16383")


async def test_sprite_frames_past_old_capacity(ed: Editor):
    """A sprite takes 200 frames (the old capacity was 128)."""
    sid = await scene(ed, "2d", "ManyFrames")
    texture = "assets/many-frames.png"
    image_file(ed, texture)
    frames = [{"name": f"f{i}", "rect": [i % 8, 0, 1, 1]} for i in range(200)]
    sprite = await ed.call("set_sprite_frames", scene=sid, create={"name": "Many"}, texture=texture, frames=frames, inline=True)
    ed.check_eq(len(sprite["frames"]), 200, "every frame is kept")
