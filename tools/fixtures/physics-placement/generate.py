"""Builds the physics-placement fixture: a saved project made only from engine primitives (boxes,
spheres, cylinders, a torus and a terrain; no imported assets), one scene per Physics Placement
scenario (docs/superpowers/specs/2026-10-23-physics-placement-design.md section 10). README.md
next to this file lists the scenes, their entity paths and what placing them should do.
tools/mcp/tests/test_place_physics_cli.py and the place_with_physics tests run on it.

    uv run --no-project --with mcp --with pyyaml python tools/fixtures/physics-placement/generate.py [out dir]

(out dir defaults to this directory; JASPER_EDITOR selects the editor.) The pebbles' layout comes
from a fixed seed and the terrain's from a fixed noise seed, so a re-run builds the same scenes.
"""
import asyncio
import random
import shutil
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent
sys.path.insert(0, str(REPO / "tools" / "mcp"))
from harness import Editor  # noqa: E402

SCENES = ["TableCrate", "RingPeg", "Stack", "Plates", "Pebbles", "Ramp", "Room"]


async def entity(ed, scene, type_, name, position, parent=None, scale=None, euler=None, geometry=None):
    """a GameObject at a local position; geometry replaces a primitive's mesh (set_mesh_geometry)"""
    args = {"scene": scene, "type": type_, "name": name, "position": position, "select": False}
    if parent is not None:
        args["parent"] = parent
    if scale is not None:
        args["scale"] = scale
    if euler is not None:
        args["rotation"] = {"euler": euler}
    eid = (await ed.call("create_entity", **args))["entity_id"]
    if geometry:
        await ed.call("set_mesh_geometry", scene=scene, entity=eid, **geometry)
    return eid


async def box(ed, scene, name, position, size, parent=None, euler=None):
    """a 1 m cube scaled to size (world scale is baked into the placement shapes)"""
    return await entity(ed, scene, "cube", name, position, parent=parent, scale=size, euler=euler)


async def view(ed, scene, position, target):
    cam = await entity(ed, scene, "camera", "View", position)
    await ed.call("set_properties", scene=scene, entity=cam,
                  values={"camera": {"useTarget": True, "target": target, "yfov": 0.8, "far": 300.0}})
    await ed.call("set_main_camera", scene=scene, entity=cam)


async def floor(ed, scene, size=12.0):
    """a 0.2 m thick slab whose top is y = 0"""
    await box(ed, scene, "Floor", [0, -0.1, 0], [size, 0.2, size])


async def table_crate(ed, scene):
    await floor(ed, scene)
    # a table of five boxes: the top's upper face at y = 1.0
    table = await entity(ed, scene, "empty", "Table", [0, 0, 0])
    await box(ed, scene, "Top", [0, 0.95, 0], [1.6, 0.1, 1.0], parent=table)
    for i, (x, z) in enumerate([(-0.7, -0.4), (0.7, -0.4), (-0.7, 0.4), (0.7, 0.4)]):
        await box(ed, scene, f"Leg{i + 1}", [x, 0.45, z], [0.08, 0.9, 0.08], parent=table)
    # an open-top crate of five boxes standing on the table: inside 0.52 x 0.52, floor at
    # y = 1.04, rim at y = 1.42
    props = await entity(ed, scene, "empty", "Props", [0, 0, 0])
    crate = await entity(ed, scene, "empty", "Crate", [0, 1.0, 0], parent=props)
    await box(ed, scene, "Bottom", [0, 0.02, 0], [0.6, 0.04, 0.6], parent=crate)
    await box(ed, scene, "WallN", [0, 0.23, 0.28], [0.6, 0.38, 0.04], parent=crate)
    await box(ed, scene, "WallS", [0, 0.23, -0.28], [0.6, 0.38, 0.04], parent=crate)
    await box(ed, scene, "WallE", [0.28, 0.23, 0], [0.04, 0.38, 0.52], parent=crate)
    await box(ed, scene, "WallW", [-0.28, 0.23, 0], [0.04, 0.38, 0.52], parent=crate)
    # three small boxes in the air above the crate's opening
    await box(ed, scene, "Box1", [-0.1, 1.75, -0.08], [0.14, 0.14, 0.14], parent=props, euler=[12, 25, 4])
    await box(ed, scene, "Box2", [0.09, 2.0, 0.06], [0.12, 0.12, 0.12], parent=props, euler=[-8, 40, 15])
    await box(ed, scene, "Box3", [0.0, 2.3, 0.1], [0.16, 0.1, 0.12], parent=props, euler=[30, 5, -10])
    # two unit cubes on the floor, interpenetrating by 0.2 m along x (mode separate)
    overlap = await entity(ed, scene, "empty", "Overlap", [3.5, 0, 0])
    await box(ed, scene, "A", [0, 0.5, 0], [1, 1, 1], parent=overlap)
    await box(ed, scene, "B", [0.8, 0.5, 0], [1, 1, 1], parent=overlap)
    await view(ed, scene, [1.8, 3.4, -5.8], [1.6, 0.9, 0])


async def ring_peg(ed, scene):
    await floor(ed, scene)
    # a 0.8 m peg (radius 6 cm) standing on the floor, a ring (inner radius 0.2 m) above it
    await entity(ed, scene, "cylinder", "Peg", [0, 0.4, 0],
                 geometry={"shape": "cylinder", "radius": 0.06, "height": 0.8, "slices": 32})
    await entity(ed, scene, "torus", "Ring", [0, 1.3, 0],
                 geometry={"shape": "torus", "radius": 0.25, "ring_radius": 0.05, "sides": 16, "rings": 48})
    await view(ed, scene, [1.2, 1.6, -1.8], [0, 0.6, 0])


async def stack(ed, scene):
    await floor(ed, scene)
    # twenty 0.4 m boxes in a column, 2 mm apart, alternately nudged 1 cm sideways
    column = await entity(ed, scene, "empty", "Stack", [0, 0, 0])
    for i in range(20):
        x = 0.01 if i % 2 else -0.01
        await box(ed, scene, f"Box{i + 1:02d}", [x, 0.2 + i * 0.402 + 0.002, 0], [0.4, 0.4, 0.4], parent=column)
    await view(ed, scene, [8.0, 5.0, -12.0], [0, 3.8, 0])


async def plates(ed, scene):
    await floor(ed, scene)
    # six 12 mm plates tilted in the air, and four pencils (7 mm thick, 18 cm long)
    group = await entity(ed, scene, "empty", "Plates", [0, 0, 0])
    tilts = [[0, 0, 0], [8, 30, -5], [-12, 75, 10], [20, 10, 3], [-5, 120, -18], [3, 160, 25]]
    for i, euler in enumerate(tilts):
        await box(ed, scene, f"Plate{i + 1}", [0.05 * (i % 3) - 0.05, 0.4 + 0.25 * i, 0.04 * (i % 2)], [0.5, 0.012, 0.35],
                  parent=group, euler=euler)
    pencils = await entity(ed, scene, "empty", "Pencils", [0, 0, 0])
    for i in range(4):
        await entity(ed, scene, "cylinder", f"Pencil{i + 1}", [0.6 + 0.1 * i, 0.3 + 0.12 * i, -0.2 + 0.12 * i], parent=pencils,
                     euler=[0, 35 * i, 90], geometry={"shape": "cylinder", "radius": 0.0035, "height": 0.18, "slices": 12})
    await view(ed, scene, [1.8, 2.2, -2.8], [0.3, 0.7, 0])


async def pebbles(ed, scene):
    # a 40 m terrain of gentle fractal hills, 0.6 to 1.8 m high around the middle
    terrain = await entity(ed, scene, "terrain", "Ground", [0, 0, 0])
    await ed.call("set_properties", scene=scene, entity=terrain, values={"terrain": {"terrainSize": 40.0, "maxHeight": 4.0}})
    await ed.call("set_terrain_maps", scene=scene, entity=terrain,
                  heightmap={"generate": "fractal_noise", "base_height": 0.3, "amplitude": 0.15, "frequency": 3.0,
                             "octaves": 3, "seed": 1234, "resolution": 129},
                  blendmap={"color": [1, 0, 0], "resolution": 16})
    # thirty pebbles, spheres and boxes from 6 to 30 cm, 1 to 3 m above the ground in a 6 m square
    rnd = random.Random(7)
    spots = [[rnd.uniform(-3.0, 3.0), rnd.uniform(-3.0, 3.0)] for _ in range(30)]
    heights = (await ed.call("sample_terrain", scene=scene, entity=terrain, points=spots))["samples"]
    group = await entity(ed, scene, "empty", "Pebbles", [0, 0, 0])
    for i, ((x, z), sample) in enumerate(zip(spots, heights)):
        size = rnd.uniform(0.06, 0.3)
        y = sample["height"] + 1.0 + rnd.uniform(0.0, 2.0)
        euler = [rnd.uniform(-60, 60), rnd.uniform(0, 360), rnd.uniform(-60, 60)]
        if i % 2:
            await box(ed, scene, f"Pebble{i + 1:02d}", [x, y, z], [size, size * rnd.uniform(0.5, 1.0), size * rnd.uniform(0.7, 1.0)],
                      parent=group, euler=euler)
        else:
            await entity(ed, scene, "sphere", f"Pebble{i + 1:02d}", [x, y, z], parent=group, euler=euler,
                         scale=[size, size * rnd.uniform(0.6, 1.0), size],
                         geometry={"shape": "sphere", "radius": 0.5, "slices": 16, "stacks": 8})
    await view(ed, scene, [6.0, 5.0, -8.0], [0, 1.0, 0])


async def ramp(ed, scene):
    await floor(ed, scene)
    # a 4 m board tilted 20 degrees (its high end at +x), three balls of the same mesh (radius
    # 0.5) at scale 0.35, 0.66 and 1.5 resting just above its upper half
    await box(ed, scene, "Ramp", [0, 0.8, 0], [4.0, 0.2, 3.2], euler=[0, 0, 20])
    balls = await entity(ed, scene, "empty", "Balls", [0, 0, 0])
    for name, scale, z in (("Small", 0.35, -1.15), ("Medium", 0.66, -0.55), ("Large", 1.5, 0.65)):
        r = 0.5 * scale
        x = 1.3
        surface = 0.8 + x * 0.36397 + 0.1 / 0.93969   # tan 20, cos 20
        await entity(ed, scene, "sphere", name, [x, surface + r / 0.93969 + 0.05, z], parent=balls, scale=[scale] * 3,
                     geometry={"shape": "sphere", "radius": 0.5, "slices": 24, "stacks": 12})
    await view(ed, scene, [0.5, 3.0, -7.0], [0, 0.8, 0])


async def room(ed, scene):
    # a 30 m room (floor and four 3 m walls under one root): too large to be a mover, so it is
    # environment; a box and a ball in the air inside it
    root = await entity(ed, scene, "empty", "Room", [0, 0, 0])
    await box(ed, scene, "Floor", [0, -0.1, 0], [30, 0.2, 30], parent=root)
    await box(ed, scene, "WallN", [0, 1.5, 15], [30, 3, 0.2], parent=root)
    await box(ed, scene, "WallS", [0, 1.5, -15], [30, 3, 0.2], parent=root)
    await box(ed, scene, "WallE", [15, 1.5, 0], [0.2, 3, 30], parent=root)
    await box(ed, scene, "WallW", [-15, 1.5, 0], [0.2, 3, 30], parent=root)
    furniture = await entity(ed, scene, "empty", "Furniture", [0, 0, 0])
    await box(ed, scene, "Box", [2.0, 1.5, 3.0], [0.8, 0.8, 0.8], parent=furniture, euler=[0, 30, 0])
    await entity(ed, scene, "sphere", "Ball", [-3.0, 2.0, -2.0], parent=furniture,
                 geometry={"shape": "sphere", "radius": 0.5, "slices": 24, "stacks": 12})
    await view(ed, scene, [8.0, 9.0, -14.0], [0, 0.5, 0])


BUILDERS = {"TableCrate": table_crate, "RingPeg": ring_peg, "Stack": stack, "Plates": plates, "Pebbles": pebbles,
            "Ramp": ramp, "Room": room}


async def main(out: Path) -> None:
    async with Editor(create=True, name="PhysicsPlacement") as ed:
        for i, name in enumerate(SCENES):
            if i == 0:
                scene = (await ed.call("list_scenes"))["scenes"][0]["id"]
                await ed.call("rename_scene", scene=scene, name=name)
                await ed.call("save_scene", scene=scene, path=f"{name}.scene")
            else:
                scene = (await ed.call("create_scene", name=name, path=f"{name}.scene"))["scene"]["id"]
            await BUILDERS[name](ed, scene)
            await ed.call("save_scene", scene=scene)
        await ed.call("save_project")

        out.mkdir(parents=True, exist_ok=True)
        for name in ["project.yaml"] + [f"{s}.scene" for s in SCENES]:
            shutil.copy(ed.project / name, out / name)
        # the terrain's generated maps (under assets/ and terrain_maps/)
        for sub in ("assets", "terrain_maps"):
            if (out / sub).exists():
                shutil.rmtree(out / sub)
            if (ed.project / sub).exists():
                shutil.copytree(ed.project / sub, out / sub, ignore=shutil.ignore_patterns("*.meta", ".*"))


if __name__ == "__main__":
    asyncio.run(main(Path(sys.argv[1]) if len(sys.argv) > 1 else HERE))
