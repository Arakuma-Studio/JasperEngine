"""The terrain brushes and generators as the editor made them before T1's operation engine.

The fixture was recorded from the unmigrated editor (T1 DC11), so these tests are the regression
proof of the migration: green before it and green after it. To record again from a build known to
be good: JASPER_TERRAIN_BASELINE_RECORD=1 tools/mcp/run-tests.sh tools/mcp/tests/test_terrain_baseline.py
"""

import base64
import hashlib
import json
import os
import struct
import zlib
from pathlib import Path

from harness import Editor
import pngdiff

EDITOR = {"create": True, "name": "Terrain Baseline"}
CI = True

FIXTURE = Path(__file__).parent / "fixtures" / "terrain_brush_baseline.json"
RECORDING = os.environ.get("JASPER_TERRAIN_BASELINE_RECORD") == "1"

SIZE = 64
RESOLUTION = 64
MAX_HEIGHT = 16.0
HEIGHT_TOLERANCE = 1  # 1/65535 (T1 R17)
# 1/255 per stroke (T1 R17, D-26): the blend and density maps here are written by two strokes, and each
# rounds where the unmigrated editor truncated, so the second starts up to a step away and can end two
# off, on the few texels where both fall on the same side
BYTE_TOLERANCE = 2
BYTE_SHARE_PAST_ONE = 0.05

# A drag across the map, turning once: every brush runs the same path
PATH = [[-20.0, -18.0], [-10.0, -4.0], [2.0, 3.0], [12.0, -6.0], [20.0, 10.0]]

COMMON = {"points": PATH, "size": 7.0, "strength": 0.5, "seconds_per_point": 0.05}

HEIGHT_STROKES = {
    "raise": {"mode": "raise"},
    "lower": {"mode": "lower"},
    "smooth": {"mode": "smooth"},
    "flatten": {"mode": "flatten", "flatten_height": 0.6},
    "sharpen": {"mode": "sharpen"},
    "noise": {"mode": "noise", "noise_size": 3.0},
    "terrace": {"mode": "terrace", "terrace_steps": 5},
    "stamp": {"mode": "stamp", "mask": "assets/stamp_mask.png", "rotation": 30.0},
    "erode": {"mode": "erode"},
    "ramp": {"mode": "ramp"},
    # the other falloffs and the square brush, once each
    "raise_square_linear": {"mode": "raise", "shape": "square", "falloff": "linear"},
    "lower_constant": {"mode": "lower", "falloff": "constant"},
}

BLEND_STROKES = {
    "paint_layer": {"mode": "paint_layer", "layer": 1},
    "paint_layer_keep": {"mode": "paint_layer", "layer": 2, "normalize": False},
    "paint_base": {"mode": "paint_base", "layer": 0},
}

DENSITY_STROKES = {
    "paint_density": {"mode": "paint_density", "layer": 0},
    "erase_density": {"mode": "erase_density", "layer": 0},
}

GENERATED = {
    "flat": {"generate": "flat", "base_height": 0.25, "resolution": RESOLUTION},
    "middle": {"generate": "middle", "resolution": RESOLUTION},
    "fractal_noise": {"generate": "fractal_noise", "resolution": RESOLUTION, "seed": 7, "amplitude": 0.25,
                      "base_height": 0.5, "frequency": 3.0, "octaves": 4},
}


def pack(values, fmt: str) -> str:
    return base64.b64encode(zlib.compress(struct.pack(f"<{len(values)}{fmt}", *values), 9)).decode("ascii")


def unpack(text: str, fmt: str) -> list:
    raw = zlib.decompress(base64.b64decode(text))
    return list(struct.unpack(f"<{len(raw) // struct.calcsize(fmt)}{fmt}", raw))


def worst_difference(actual: list, expected: list) -> int:
    assert len(actual) == len(expected), f"{len(actual)} values against {len(expected)}"
    return max((abs(a - b) for a, b in zip(actual, expected)), default=0)


def texel_centres() -> list:
    step = SIZE / RESOLUTION
    half = SIZE * 0.5
    return [[(x + 0.5) * step - half, (y + 0.5) * step - half] for y in range(RESOLUTION) for x in range(RESOLUTION)]


async def new_terrain(ed: Editor, sid, name: str):
    terrain = (await ed.call("create_entity", scene=sid, name=name, type="terrain",
                             values={"terrain": {"terrainSize": SIZE, "maxHeight": MAX_HEIGHT, "resolution": 8,
                                                 "rootGridSize": 1, "levels": 1}}))["entity_id"]
    await ed.call("set_terrain_maps", scene=sid, entity=terrain, heightmap=GENERATED["fractal_noise"])
    return terrain


async def read_heights(ed: Editor, sid, terrain) -> list:
    samples = (await ed.call("sample_terrain", scene=sid, entity=terrain, space="local", points=texel_centres()))["samples"]
    return [round(sample["normalized"] * 65535.0) for sample in samples]


async def saved_channels(ed: Editor, sid, relative: str, channels: list) -> list:
    await ed.call("save_scene", scene=sid, path="scenes/Brushes.scene")
    _, _, rgba = pngdiff.decode((ed.project / relative).read_bytes())
    values = []
    for channel in channels:
        values.extend(rgba[channel::4])
    return values


def density_map_path(entity: dict) -> str:
    density = entity["components"]["terrain"]["foliageLayers"][0]["densityMap"]
    return density["path"] if isinstance(density, dict) else density


def compare_or_record(store: dict, key: str, values: list, fmt: str, tolerance: int, ed: Editor):
    if RECORDING:
        store[key] = pack(values, fmt)
        return
    expected = unpack(store[key], fmt)
    worst = worst_difference(values, expected)
    ed.check(worst <= tolerance, f"{key}: off by {worst} steps from the unmigrated editor (at most {tolerance})")
    if tolerance > 1:
        past_one = sum(1 for a, b in zip(values, expected) if abs(a - b) > 1) / len(expected)
        print(f"     [baseline] {key}: {past_one:.4%} of the values are two steps off")
        ed.check(past_one <= BYTE_SHARE_PAST_ONE, f"{key}: {past_one:.2%} of the values are more than one step off")


def load_fixture() -> dict:
    if FIXTURE.exists():
        return json.loads(FIXTURE.read_text())
    assert RECORDING, f"{FIXTURE} is missing"
    return {"heights": {}, "blend": {}, "density": {}, "placement": {}, "generated": {}}


def save_fixture(fixture: dict):
    if RECORDING:
        FIXTURE.write_text(json.dumps(fixture, indent=1, sort_keys=True) + "\n")


async def test_sculpt_terrain_matches_baseline(ed: Editor):
    fixture = load_fixture()
    sid = (await ed.call("create_scene", name="Brushes", type="3d"))["scene"]["id"]

    mask = bytearray()
    for y in range(16):
        for x in range(16):
            value = 255 * x // 15
            mask += bytes([value, value, value, 255])
    mask_path = ed.project / "assets" / "stamp_mask.png"
    mask_path.parent.mkdir(parents=True, exist_ok=True)
    mask_path.write_bytes(pngdiff.encode(16, 16, bytes(mask)))

    for name, stroke in HEIGHT_STROKES.items():
        terrain = await new_terrain(ed, sid, name)
        before = await read_heights(ed, sid, terrain)
        await ed.call("sculpt_terrain", scene=sid, entity=terrain, space="local", strokes=[{**COMMON, **stroke}])
        after = await read_heights(ed, sid, terrain)
        ed.check(after != before, f"{name}: the stroke changed the map")
        compare_or_record(fixture["heights"], name, after, "H", HEIGHT_TOLERANCE, ed)
        await ed.call("delete_entities", scene=sid, entities=[terrain])

    for name, stroke in BLEND_STROKES.items():
        terrain = await new_terrain(ed, sid, name)
        await ed.call("sculpt_terrain", scene=sid, entity=terrain, space="local", map_resolution=RESOLUTION,
                      strokes=[{**COMMON, "mode": "paint_layer", "layer": 0, "points": PATH[:2]}, {**COMMON, **stroke}])
        info = (await ed.call("sample_terrain", scene=sid, entity=terrain, grid=0))["info"]
        values = await saved_channels(ed, sid, info["blend_maps"][0], [0, 1, 2])
        compare_or_record(fixture["blend"], name, values, "B", BYTE_TOLERANCE, ed)
        await ed.call("delete_entities", scene=sid, entities=[terrain])

    for name, stroke in DENSITY_STROKES.items():
        terrain = await new_terrain(ed, sid, name)
        await ed.call("set_properties", scene=sid, entity=terrain, values={"terrain": {"foliageLayers": [{"density": 1.0}]}})
        await ed.call("sculpt_terrain", scene=sid, entity=terrain, space="local", map_resolution=RESOLUTION,
                      strokes=[{**COMMON, "mode": "paint_density", "layer": 0, "points": PATH[:3]}, {**COMMON, **stroke}])
        fields = await ed.call("get_entity", scene=sid, entity=terrain, components=["terrain"], properties=["foliageLayers"])
        values = await saved_channels(ed, sid, density_map_path(fields), [0])
        compare_or_record(fixture["density"], name, values, "B", BYTE_TOLERANCE, ed)
        await ed.call("delete_entities", scene=sid, entities=[terrain])

    save_fixture(fixture)


async def test_placement_counts_and_spacing_match_baseline(ed: Editor):
    """The placement RNG was never seeded, so only the counts and the spacing rule are comparable."""
    fixture = load_fixture()
    sid = (await ed.call("create_scene", name="Placement", type="3d"))["scene"]["id"]
    model = ed.project / "assets" / "peg.obj"
    model.parent.mkdir(parents=True, exist_ok=True)
    model.write_text("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n")
    terrain = await new_terrain(ed, sid, "Placement")
    # the second point is inside the first's spacing: refused; the third is far enough
    placed = await ed.call("sculpt_terrain", scene=sid, entity=terrain, space="local", strokes=[
        {"mode": "place_object", "asset": "assets/peg.obj", "points": [[0, 0], [0.5, 0], [12, 12]], "size": 0.1,
         "spacing": 4.0, "instanced": False}])
    erased = await ed.call("sculpt_terrain", scene=sid, entity=terrain, space="local", strokes=[
        {"mode": "erase_object", "points": [[12, 12]], "size": 3.0}])
    counts = {"placed": placed["strokes"][0]["placed"], "erased": erased["strokes"][0]["erased"]}
    if RECORDING:
        fixture["placement"] = counts
    else:
        ed.check_eq(counts, fixture["placement"], "as many placed and erased as the unmigrated editor")
    save_fixture(fixture)


async def test_generated_maps_match_baseline(ed: Editor):
    fixture = load_fixture()
    sid = (await ed.call("create_scene", name="Generated", type="3d"))["scene"]["id"]
    for name, request in GENERATED.items():
        terrain = (await ed.call("create_entity", scene=sid, name=name, type="terrain",
                                 values={"terrain": {"terrainSize": SIZE, "maxHeight": MAX_HEIGHT}}))["entity_id"]
        await ed.call("set_terrain_maps", scene=sid, entity=terrain, heightmap=request)
        info = (await ed.call("sample_terrain", scene=sid, entity=terrain, grid=0))["info"]
        digest = hashlib.sha256((ed.project / info["height_map"]).read_bytes()).hexdigest()
        if RECORDING:
            fixture["generated"][name] = digest
        else:
            ed.check_eq(digest, fixture["generated"][name], f"{name}: file bytes as the unmigrated editor wrote them")
    save_fixture(fixture)
