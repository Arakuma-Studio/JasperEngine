"""edit_terrain_world and sample_terrain {world}: Terrain Worlds through MCP (T4 R43-R46; AC27, AC39).

Each test makes its own small world (64-quad tiles of 1 m, so 64 m a tile) in a scene of its own.
"""

import asyncio
import hashlib
import time
from pathlib import Path

from harness import Editor

EDITOR = {"create": True, "name": "Terrain World"}
CI = True

SMALL = {"tile_quads": 64, "spacing": 1, "max_height": 32, "grid": {"width": 2, "length": 2, "origin": [-1, -1]}}
STATUS_KEYS = ("tile_counts", "tiles", "gpu_bytes", "cpu_bytes", "budgets", "queue", "in_flight", "uploads", "main_thread_ms",
               "draws", "nodes", "collision", "seam_errors", "pool_exhausted", "late_pages", "errors")


async def new_world(ed: Editor, name: str, **create) -> tuple:
    sid = (await ed.call("create_scene", name=name, type="3d"))["scene"]["id"]
    out = await ed.call("edit_terrain_world", scene=sid, create={"name": name, **SMALL, **create})
    return sid, out["entity"]["id"], out


def world_folder(ed: Editor, name: str) -> Path:
    found = [p for p in ed.project.rglob(f"{name}.terrainworld") if p.is_dir()]
    ed.check_eq(len(found), 1, f"one {name}.terrainworld folder: {found}")
    return found[0]


def files(folder: Path) -> dict:
    return {str(p.relative_to(folder)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(folder.rglob("*")) if p.is_file()}


def tiles(folder: Path) -> set:
    return {p.name for p in (folder / "tiles").glob("*.jtile")}


async def settled(folder: Path, expected: dict, timeout: float = 20.0) -> dict:
    """The folder's files once they equal `expected`, or as they are at the timeout (edits reach disk
    at the flush points, after a debounce)."""
    deadline = time.monotonic() + timeout
    while True:
        now = files(folder)
        if now == expected or time.monotonic() > deadline:
            return now
        await asyncio.sleep(0.1)


async def tiles_made(folder: Path, names: set, timeout: float = 20.0) -> set:
    """The folder's tile files once they include `names`, or as they are at the timeout."""
    deadline = time.monotonic() + timeout
    while not names <= tiles(folder) and time.monotonic() < deadline:
        await asyncio.sleep(0.1)
    return tiles(folder)


async def test_tool_hidden_until_group(ed: Editor):
    default = {t.name for t in await ed.tools(groups=None)}
    ed.check("edit_terrain_world" not in default, "hidden until the terrain group is enabled")
    await ed.call("tool_groups", enable=["terrain"])
    ed.check("edit_terrain_world" in {t.name for t in await ed.tools(groups=None)}, "listed once enabled")
    await ed.call("tool_groups", disable=["terrain"])


async def test_create_and_sample(ed: Editor):
    sid, world, out = await new_world(ed, "Created")
    ed.check_eq(out["tile_count"], 4, "a 2 x 2 grid")
    ed.check(out["guid"] and out["predicted_bytes"] > 0, f"GUID and predicted memory: {out}")
    ed.check_eq(out["undo_name"], "Create Terrain World (Created)")
    folder = world_folder(ed, "Created")
    ed.check((folder / "manifest.yaml").is_file(), "the manifest is written")
    ed.check_eq(len(tiles(folder)), 4, "one file per tile")

    sampled = await ed.call("sample_terrain", scene=sid, entity=world, world={})
    status = sampled["world"]
    for key in STATUS_KEYS:
        ed.check(key in status, f"world.{key} is reported: {sorted(status)}")
    ed.check_eq(status["tile_counts"]["total"], 4)
    ed.check_eq(sorted(t["tile"] for t in status["tiles"]), [[-1, -1], [-1, 0], [0, -1], [0, 0]], "every tile with its state")
    asked = await ed.call("sample_terrain", scene=sid, entity=world, world={"tiles": [[0, 0], [5, 5]]})
    ed.check_eq([t["state"] for t in asked["world"]["tiles"]][1], "absent", f"a tile outside the world: {asked['world']['tiles']}")

    points = (await ed.call("sample_terrain", scene=sid, entity=world, points=[[10, 10], [1000, 1000]]))["samples"]
    ed.check(points[0]["state"] in ("overview", "full"), f"a point on a tile: {points[0]}")
    ed.check_eq(points[1]["state"], "absent", "a point off the grid")


async def test_plain_terrain_world_key_names_create(ed: Editor):
    sid = (await ed.call("create_scene", name="Plain", type="3d"))["scene"]["id"]
    terrain = (await ed.call("create_entity", scene=sid, name="Plain", type="terrain"))["entity_id"]
    message = await ed.check_error("sample_terrain", scene=sid, entity=terrain, world={})
    ed.check("edit_terrain_world" in message and "create" in message, f"the error names the fix: {message}")
    message = await ed.check_error("edit_terrain_world", scene=sid, entity=terrain, ops=[{"op": "add_tiles", "tiles": [[0, 0]]}])
    ed.check("edit_terrain_world {create}" in message, f"a plain Terrain is refused with the fix: {message}")


async def test_add_remove_tiles_with_undo(ed: Editor):
    sid, world, _ = await new_world(ed, "Tiles")
    folder = world_folder(ed, "Tiles")
    before = files(folder)

    out = await ed.call("edit_terrain_world", scene=sid, entity=world, ops=[{"op": "add_tiles", "tiles": [[1, 0]]}])
    ed.check_eq(out["changed_tiles"]["added"], [[1, 0]])
    ed.check_eq(out["tile_count"], 5)
    ed.check("1_0.jtile" in tiles(folder), f"the new tile's file: {tiles(folder)}")
    added = files(folder)
    await ed.call("undo", scene=sid)
    ed.check_eq(await settled(folder, before), before, "one undo removes the tile and leaves the other files byte-identical")
    await ed.call("redo", scene=sid)
    ed.check_eq(await settled(folder, added), added, "redo makes it again, the same bytes")
    await ed.call("undo", scene=sid)
    await settled(folder, before)

    out = await ed.call("edit_terrain_world", scene=sid, entity=world, ops=[{"op": "remove_tiles", "tiles": [[0, 0]]}])
    ed.check_eq(out["changed_tiles"]["removed"], [[0, 0]])
    ed.check("0_0.jtile" not in tiles(folder), "the removed tile's file is gone")
    await ed.call("undo", scene=sid)
    ed.check_eq(await settled(folder, before), before, "undo puts the removed tile back byte for byte")


async def test_all_or_nothing(ed: Editor):
    sid, world, _ = await new_world(ed, "Atomic")
    folder = world_folder(ed, "Atomic")
    before = files(folder)
    message = await ed.check_error("edit_terrain_world", scene=sid, entity=world,
                                   ops=[{"op": "add_tiles", "tiles": [[1, 0]]}, {"op": "remove_tiles", "tiles": [[5, 5]]}])
    ed.check("ops[1]" in message and "(5, 5)" in message, f"the error names the op and the tile: {message}")
    ed.check_eq(files(folder), before, "the first op did not run either")
    message = await ed.check_error("edit_terrain_world", scene=sid, entity=world, ops=[{"op": "add_tile", "tiles": [[1, 0]]}])
    ed.check("add_tiles" in message and "set_streaming" in message, f"the valid ops are listed: {message}")
    message = await ed.check_error("edit_terrain_world", scene=sid, entity=world, ops=[{"op": "add_tiles", "tiles": [[1, 0]], "height": 3}])
    ed.check("ops[0].height" in message, f"an unknown field is named: {message}")
    message = await ed.check_error("edit_terrain_world", scene=sid, create={"name": "Bad", "tile_quads": 100})
    ed.check("tile_quads" in message, f"create's fields are checked: {message}")
    ed.check(not list(ed.project.rglob("Bad.terrainworld")), "a refused create writes no folder")
    ed.check_eq(files(folder), before, "nothing changed")


async def test_dry_run_writes_nothing(ed: Editor):
    sid, world, _ = await new_world(ed, "Dry")
    folder = world_folder(ed, "Dry")
    before = files(folder)

    async def step_ids():
        return [entry["id"] for entry in (await ed.call("get_undo_history", scene=sid, scope="scene"))["entries"]]

    history = await step_ids()
    dry = await ed.call("edit_terrain_world", scene=sid, entity=world, dry_run=True, ops=[{"op": "add_tiles", "tiles": [[1, 0], [1, -1]]}])
    ed.check_eq(dry["dry_run"], True)
    ed.check_eq(dry["ops"][0]["added"], [[1, 0], [1, -1]], "the tiles it would add")
    ed.check_eq(dry["tile_count"], 6, "and the count after")
    ed.check_eq(files(folder), before, "a dry run writes nothing")
    ed.check_eq(await step_ids(), history, "and adds no undo step")
    made = await ed.call("edit_terrain_world", scene=sid, dry_run=True, create={"name": "DryCreate", **SMALL})
    ed.check_eq(made["tile_count"], 4)
    ed.check(not list(ed.project.rglob("DryCreate.terrainworld")), "a dry-run create writes no folder")


async def test_wait_and_freeze_repeat_frames(ed: Editor):
    sid, world, _ = await new_world(ed, "Frozen")
    ready = await ed.call("sample_terrain", scene=sid, entity=world, timeout_seconds=60,
                          world={"wait_for": {"x": 0, "z": 0, "radius": 32}})
    ed.check_eq(ready["wait_for"]["ready"], True, f"the area loads: {ready.get('wait_for')}")
    ed.check("set_streaming" in ready["next"] and "freeze" in ready["next"], f"next names the freeze: {ready['next']}")
    out = await ed.call("edit_terrain_world", scene=sid, entity=world, ops=[{"op": "set_streaming", "freeze": True}])
    ed.check_eq(out["frozen"], True)
    try:
        first = hashlib.sha256(await ed.screenshot("scene")).hexdigest()
        second = hashlib.sha256(await ed.screenshot("scene")).hexdigest()
        ed.check_eq(first, second, "frozen streaming draws the same frame twice")
    finally:
        await ed.call("edit_terrain_world", scene=sid, entity=world, ops=[{"op": "set_streaming", "freeze": False}])


async def test_wait_timeout_lists_missing_tiles(ed: Editor):
    # far from the Scene view's camera, and frozen in the same call: no tile of it can load
    sid, world, _ = await new_world(ed, "Far", grid={"width": 2, "length": 2, "origin": [10, 10]})
    await ed.call("edit_terrain_world", scene=sid, entity=world, ops=[{"op": "set_streaming", "freeze": True}])
    try:
        message = await ed.check_error("sample_terrain", scene=sid, entity=world, timeout_seconds=1,
                                       world={"wait_for": {"x": 10.5 * 64, "z": 10.5 * 64, "radius": 8}})
        ed.check("(10, 10)" in message, f"the timeout lists the missing tile: {message}")
        ed.check("freeze: false" in message, f"and says streaming is frozen: {message}")
    finally:
        await ed.call("edit_terrain_world", scene=sid, entity=world, ops=[{"op": "set_streaming", "freeze": False}])


async def test_play_refusals_by_name(ed: Editor):
    sid, world, _ = await new_world(ed, "Playing")
    await ed.call("save_scene", scene=sid, path="scenes/Playing.scene")
    await ed.call("play", action="start", scene=sid, auto_pause=True)
    try:
        for op in ({"op": "add_tiles", "tiles": [[1, 0]]}, {"op": "repair_seams"}, {"op": "set_collision", "radius": 64}):
            message = await ed.check_error("edit_terrain_world", scene=sid, entity=world, ops=[op])
            ed.check(op["op"] in message and "Play" in message, f"{op['op']} is refused by name in Play: {message}")
        message = await ed.check_error("edit_terrain_world", scene=sid, create={"name": "InPlay", **SMALL})
        ed.check("create" in message and "Play" in message, f"create is refused in Play: {message}")
        out = await ed.call("edit_terrain_world", scene=sid, entity=world,
                            ops=[{"op": "set_streaming", "freeze": True}, {"op": "set_focus", "x": 10, "z": 10}])
        ed.check_eq(out["frozen"], True, "set_streaming runs in Play")
        focus = out["focus_ids"][0]
        out = await ed.call("edit_terrain_world", scene=sid, entity=world, ops=[{"op": "remove_focus", "id": focus}])
        ed.check_eq(out["warnings"], [], "remove_focus removed the focus set_focus made")
    finally:
        await ed.call("play", action="stop")


async def overlay_legend(ed: Editor, sid: str, world, overlay: str) -> list:
    shown = await ed.call_raw("visualize_terrain", {"scene": sid, "entity": world, "overlay": overlay, "framing": "top"})
    ed.check(not shown.is_error and shown.images, f"{overlay} is drawn and captured: {shown.texts}")
    return [stop["label"] for stop in shown.value["legend"]]


async def test_visualize_world_overlays(ed: Editor):
    """R42, R46: visualize_terrain draws a World's overlays from its runtime, refuses the other kind's, and works in Play."""
    sid, world, _ = await new_world(ed, "Overlaid")
    ed.check_eq(await overlay_legend(ed, sid, world, "tiles"), ["tile border"], "Tile Borders")
    states = await overlay_legend(ed, sid, world, "tile_state")
    ed.check("resident" in states and "pinned" in states, f"Tile State's legend: {states}")
    levels = await overlay_legend(ed, sid, world, "lod")
    ed.check(any(label.startswith("level") or label == "overview" for label in levels), f"LOD Level shows what the view drew: {levels}")
    collision = await overlay_legend(ed, sid, world, "collision")
    ed.check(len(collision) == 1 and "Play" in collision[0], f"no collider out of Play: {collision}")

    message = await ed.check_error("visualize_terrain", scene=sid, entity=world, overlay="height")
    ed.check("Terrain World" in message and "tile_state" in message, f"a Terrain's overlay on a World names the World's: {message}")
    terrain = (await ed.call("create_entity", scene=sid, name="Plain", type="terrain"))["entity_id"]
    message = await ed.check_error("visualize_terrain", scene=sid, entity=terrain, overlay="tiles")
    ed.check("Terrain World" in message and "height" in message, f"a World's overlay on a Terrain names a Terrain's: {message}")
    await ed.call("delete_entities", scene=sid, entities=[terrain])

    await ed.call("save_scene", scene=sid, path="scenes/Overlaid.scene")
    await ed.call("play", action="start", scene=sid)
    try:
        states = await overlay_legend(ed, sid, world, "tile_state")
        ed.check("resident" in states, f"Tile State in Play: {states}")
        deadline = time.monotonic() + 30.0
        while True:
            collision = await overlay_legend(ed, sid, world, "collision")
            if "fine chunk" in collision or time.monotonic() > deadline:
                break
            await ed.call("wait", seconds=5, frames=10)
        ed.check("fine chunk" in collision, f"Collision Chunks in Play shows the collider: {collision}")
    finally:
        await ed.call("play", action="stop")
        await ed.call("visualize_terrain", scene=sid, entity=world, overlay="none", screenshot=False)


async def test_brush_across_border(ed: Editor):
    """AC27: strokes over the grid's edge make the tile they reach; one undo removes it, files byte-identical."""
    sid, world, _ = await new_world(ed, "Border")
    folder = world_folder(ed, "Border")
    before = files(folder)

    # the grid ends at x = 64: a circle on that edge reaches tiles (1, -1) and (1, 0)
    await ed.call("edit_terrain", scene=sid, entity=world, ops=[
        {"op": "raise", "delta": 4, "shape": {"type": "circle", "center": [64, 0], "radius": 6}}])
    made = await tiles_made(folder, {"1_0.jtile", "1_-1.jtile"})
    ed.check({"1_0.jtile", "1_-1.jtile"} <= made, f"the stroke made the tiles it reached: {made}")
    samples = (await ed.call("sample_terrain", scene=sid, entity=world, points=[[63.99, 2], [64.01, 2]]))["samples"]
    ed.check(abs(samples[0]["height"] - samples[1]["height"]) < 0.05 and samples[0]["height"] > 1,
             f"the same height on both sides of the border: {samples}")
    status = (await ed.call("sample_terrain", scene=sid, entity=world, world={}))["world"]
    ed.check_eq(status["tile_counts"]["total"], 6, "two tiles were made")
    await ed.call("undo", scene=sid)
    ed.check_eq(await settled(folder, before), before, "one undo removes the made tiles and restores every file byte for byte")

    await ed.call("sculpt_terrain", scene=sid, entity=world, strokes=[{"mode": "raise", "points": [[60, 10], [68, 10]], "size": 3}])
    made = await tiles_made(folder, {"1_0.jtile"})
    ed.check("1_0.jtile" in made, f"a brush stroke over the edge makes the tile too: {made}")
    await ed.call("undo", scene=sid)
    ed.check_eq(await settled(folder, before), before, "and one undo takes it back")


async def test_import_heightmap_makes_tiles(ed: Editor):
    """R32: a heightmap file on a World makes the tiles it covers, writes their samples, and undoes as one step."""
    sid, world, _ = await new_world(ed, "Imported")
    folder = world_folder(ed, "Imported")
    before = files(folder)
    # 193 x 129 samples from tile (-1, -1): three tiles across, two down, a ramp along x
    width, height = 193, 129
    row = b"".join((i * 300).to_bytes(2, "little") for i in range(width))
    (ed.project / "assets").mkdir(parents=True, exist_ok=True)
    (ed.project / "assets" / "ridge.r16").write_bytes(row * height)

    message = await ed.check_error("edit_terrain", scene=sid, entity=world, ops=[{"op": "import_heightmap", "path": "assets/ridge.r16"}])
    ed.check("edit_terrain_world" in message, f"edit_terrain's import names the World's op: {message}")

    op = {"op": "import_heightmap", "path": "assets/ridge.r16", "width": width, "height": height, "origin": [-1, -1]}
    dry = await ed.call("edit_terrain_world", scene=sid, entity=world, dry_run=True, ops=[op])
    ed.check_eq(sorted(dry["ops"][0]["imported"]), [[-1, -1], [-1, 0], [0, -1], [0, 0], [1, -1], [1, 0]], "the tiles the file covers")
    ed.check_eq(dry["tile_count"], 6, "two of them new")
    ed.check_eq(files(folder), before, "a dry run writes nothing")

    out = await ed.call("edit_terrain_world", scene=sid, entity=world, ops=[op])
    ed.check_eq(len(out["changed_tiles"]["imported"]), 6)
    made = await tiles_made(folder, {"1_-1.jtile", "1_0.jtile"})
    ed.check({"1_-1.jtile", "1_0.jtile"} <= made, f"the new tiles' files: {made}")
    # the data's range maps to 0..max_height (32 m): sample i is at x = -64 + i, height 32 * i / 192
    samples = (await ed.call("sample_terrain", scene=sid, entity=world, points=[[0, 5], [96, -20], [63.99, 3], [64.01, 3]]))["samples"]
    ed.check(abs(samples[0]["height"] - 32 * 64 / 192) < 0.01, f"an old tile has the file's height: {samples[0]}")
    ed.check(abs(samples[1]["height"] - 32 * 160 / 192) < 0.01, f"a made tile has the file's height: {samples[1]}")
    ed.check(abs(samples[2]["height"] - samples[3]["height"]) < 0.01, f"no step at the border: {samples[2:]}")
    ed.check_eq((await ed.call("sample_terrain", scene=sid, entity=world, world={}))["world"]["seam_errors"], 0, "and no seam error")

    await ed.call("undo", scene=sid)
    ed.check_eq(await settled(folder, before), before, "one undo takes the import back, files byte for byte")
    await ed.call("redo", scene=sid)
    ed.check({"1_-1.jtile", "1_0.jtile"} <= await tiles_made(folder, {"1_-1.jtile", "1_0.jtile"}), "redo puts the import back")
    await ed.call("undo", scene=sid)
    ed.check_eq(await settled(folder, before), before, "and undo again, byte for byte")

    message = await ed.check_error("edit_terrain_world", scene=sid, entity=world, ops=[op, {"op": "rebuild_overview"}])
    ed.check("alone" in message, f"an import is a job, sent alone: {message}")


async def wait_job(ed: Editor, job_id: str, timeout: float = 60.0) -> dict:
    deadline = time.monotonic() + timeout
    while True:
        job = await ed.call("get_job", job_id=job_id)
        if job["status"] != "running" or time.monotonic() > deadline:
            return job
        await asyncio.sleep(0.1)


async def test_import_heightmap_job_and_cancel(ed: Editor):
    """R32: an import runs as a job with progress; cancel_job stops it and leaves the world's folder untouched."""
    sid, world, _ = await new_world(ed, "Streamed")
    folder = world_folder(ed, "Streamed")
    before = files(folder)
    (ed.project / "assets").mkdir(parents=True, exist_ok=True)
    # 129 x 129 samples: four tiles from (0, 0), read in bands
    small = b"".join((((x * 7 + z * 3) % 50000).to_bytes(2, "little")) for z in range(129) for x in range(129))
    (ed.project / "assets" / "hill.r16").write_bytes(small)
    out = await ed.call("edit_terrain_world", scene=sid, entity=world, wait=False,
                        ops=[{"op": "import_heightmap", "path": "assets/hill.r16", "origin": [0, 0]}])
    ed.check("job_id" in out, f"a job: {out}")
    job = await wait_job(ed, out["job_id"])
    ed.check_eq(job["status"], "succeeded", f"the job finishes: {job}")
    ed.check_eq(len(job["result"]["changed_tiles"]["imported"]), 4, "the tiles it covers")
    await ed.call("undo", scene=sid)
    ed.check_eq(await settled(folder, before), before, "undone")

    # a large file, cancelled at once: nothing of it reaches the folder
    stash = ed.project / ".jasperengine" / "trash" / "terrain-world-import"
    kept = set(stash.iterdir()) if stash.exists() else set()
    side = 2049
    row = b"".join((i % 60000).to_bytes(2, "little") for i in range(side))
    (ed.project / "assets" / "big.r16").write_bytes(row * side)
    out = await ed.call("edit_terrain_world", scene=sid, entity=world, wait=False,
                        ops=[{"op": "import_heightmap", "path": "assets/big.r16", "origin": [-16, -16]}])
    await ed.call("cancel_job", job_id=out["job_id"])
    job = await wait_job(ed, out["job_id"])
    ed.check_eq(job["status"], "cancelled", f"cancelled: {job}")
    ed.check_eq(files(folder), before, "the world's folder is untouched")
    ed.check_eq(set(stash.iterdir()) if stash.exists() else set(), kept, "and the job's own tiles are gone")


async def convert_source(ed: Editor, scene: str, name: str) -> str:
    """A Terrain with heights of its own, at the scene's origin."""
    terrain = (await ed.call("create_entity", scene=scene, name=name, type="terrain"))["entity_id"]
    await ed.call("edit_terrain", scene=scene, entity=terrain, create="test_patch", map_resolution=65)
    return terrain


async def test_convert_from_terrain_with_undo(ed: Editor):
    sid = (await ed.call("create_scene", name="Convert", type="3d"))["scene"]["id"]
    terrain = await convert_source(ed, sid, "Hills")
    points = [[0, 0], [5, -7], [12, 3], [-20, 9]]
    before = [p["height"] for p in (await ed.call("sample_terrain", scene=sid, entity=terrain, points=points))["samples"]]

    dry = await ed.call("edit_terrain_world", scene=sid, create={"from_terrain": terrain}, dry_run=True)
    ed.check(dry["dry_run"] and dry["tile_count"] >= 4 and dry["tile_quads"] >= 64 and dry["spacing"] > 0, f"the plan from the Terrain: {dry}")
    ed.check(not [p for p in ed.project.rglob("Hills World*.terrainworld")], "a dry run writes nothing")

    out = await ed.call("edit_terrain_world", scene=sid, create={"from_terrain": terrain})
    ed.check_eq(out["undo_name"], "Convert to Terrain World (Hills World)")
    ed.check_eq(out["tile_count"], dry["tile_count"], "the tiles the dry run said")
    folder = world_folder(ed, "Hills World")
    ed.check((folder / "manifest.yaml").is_file(), "the world's folder is written")
    world = out["entity"]["id"]
    after = [p["height"] for p in (await ed.call("sample_terrain", scene=sid, entity=world, points=points))["samples"]]
    for point, was, now in zip(points, before, after):
        ed.check(abs(was - now) < 0.01, f"the World lies on the Terrain's surface at {point}: {was} then {now}")
    # T4 R27: foliage on a World reports its tile regions
    foliage = (await ed.call("sample_terrain", scene=sid, entity=world, foliage=True, grid=0))["foliage"]
    ed.check("regions" in foliage["totals"], f"a World's foliage counts its tile regions: {foliage['totals']}")

    # undo: the World goes, the Terrain is back; the folder stays, so the next conversion picks another name
    await ed.call("undo", scene=sid)
    await ed.check_error("sample_terrain", scene=sid, entity=world, world={})
    again = [p["height"] for p in (await ed.call("sample_terrain", scene=sid, entity=terrain, points=points))["samples"]]
    ed.check_eq(again, before, "the Terrain is as it was")
    ed.check((folder / "manifest.yaml").is_file(), "an undone conversion leaves its folder")
    dry = await ed.call("edit_terrain_world", scene=sid, create={"from_terrain": terrain}, dry_run=True)
    ed.check_eq(dry["asset_path"].split("/")[-1], "Hills World 1.terrainworld", "the next conversion picks another name")


PINE = "v -1 0 0\nv 1 0 0\nv 1 4 0\nv -1 4 0\nvn 0 0 1\nf 1//1 2//1 3//1\nf 1//1 3//1 4//1\n"


async def test_trees_across_border_with_undo(ed: Editor):
    """T4 R27: sculpt_terrain's tree brush on a World puts each tree in the tile under it, across a border, making
    the tiles' tree files; one undo puts the folder back byte for byte. edit_terrain's place_trees and remove_trees
    name a World's trees by ids that carry their tile."""
    sid, world, _ = await new_world(ed, "Orchard")
    folder = world_folder(ed, "Orchard")
    (ed.project / "assets").mkdir(parents=True, exist_ok=True)
    (ed.project / "assets" / "pine.obj").write_text(PINE)
    await ed.call("set_properties", scene=sid, entity=world, values={"terrain": {"treePrototypes": [{"id": 3, "meshPath": "assets/pine.obj"}]}})
    before = files(folder)

    # z = 16 lies in tiles (-1, 0) and (0, 0), split by x = 0
    out = await ed.call("sculpt_terrain", scene=sid, entity=world, strokes=[
        {"mode": "paint_trees", "points": [[-12, 16], [-8, 16], [-4, 16], [0, 16], [4, 16], [8, 16], [12, 16]], "size": 8, "spacing": 0.5}])
    ed.check(out["strokes"][0]["placed"] > 0, f"the brush placed trees: {out['strokes']}")
    painted = files(folder)
    ed.check({"tiles/-1_0.jetrees", "tiles/0_0.jetrees"} <= set(painted), f"both tiles got a tree file: {sorted(painted)}")
    ed.check("tiles/0_-1.jetrees" not in painted, "a tile the brush did not reach has none")
    await ed.call("undo", scene=sid)
    ed.check_eq(await settled(folder, before), before, "one undo puts both tiles back, byte for byte")
    await ed.call("redo", scene=sid)
    ed.check_eq(await settled(folder, painted), painted, "redo writes the stroke's bytes again")
    await ed.call("undo", scene=sid)
    ed.check_eq(await settled(folder, before), before, "and undo again")

    message = await ed.check_error("sculpt_terrain", scene=sid, entity=world, strokes=[{"mode": "paint_trees", "points": [[500, 500]], "size": 4}])
    ed.check("need ground" in message, f"a stroke over no tile says why: {message}")

    placed = await ed.call("edit_terrain", scene=sid, entity=world, space="local", ops=[
        {"op": "place_trees", "prototype": 0, "points": [[-10, -10], [10, 10]]}])
    ids = placed["ops"][0]["ids"]
    ed.check_eq(len(ids), 2, f"one id a tree: {placed['ops'][0]}")
    ed.check(len(set(ids)) == 2, f"ids of trees in two tiles differ: {ids}")
    ed.check({"tiles/-1_-1.jetrees", "tiles/0_0.jetrees"} <= set(files(folder)), "each tree in its tile's file")
    removed = await ed.call("edit_terrain", scene=sid, entity=world, ops=[{"op": "remove_trees", "ids": [ids[0]]}])
    ed.check_eq(removed["ops"][0]["instances_removed"], 1, f"remove_trees takes a World id: {removed['ops'][0]}")
    await ed.call("undo", scene=sid)
    await ed.call("undo", scene=sid)
    ed.check_eq(await settled(folder, before), before, "both edits undone")


async def test_convert_refuses_rotated_terrain(ed: Editor):
    sid = (await ed.call("create_scene", name="Tilted", type="3d"))["scene"]["id"]
    terrain = await convert_source(ed, sid, "Tilted")
    await ed.call("set_transform", scene=sid, entity=terrain, rotation={"euler": [0, 30, 0]})
    message = await ed.check_error("edit_terrain_world", scene=sid, create={"from_terrain": terrain})
    ed.check("does not rotate or scale" in message and "Nothing was changed" in message, f"the refusal names the fix: {message}")
    ed.check(not [p for p in ed.project.rglob("Tilted World*.terrainworld")], "nothing is written")
    message = await ed.check_error("edit_terrain_world", scene=sid, create={"from_terrain": terrain, "grid": {"width": 2}})
    ed.check("leave out grid" in message, f"a converted world's grid is the Terrain's: {message}")
