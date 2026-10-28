"""Lattice MCP surface (spec 2026-10-23-lattice-deformer-design.md §10): edit_lattice, create_entity
type lattice / lattice_tracks, set_scene_camera display.show_all_lattices, the jasper://docs/lattice guide.

- creation around a model (edit_lattice create and create_entity, same fit) and around a sprite
  (flat cage), one undo step, the lattice fallback (selected / only lattice);
- every op (set, move, reset, preset, mirror, fit, bind, unbind, pose, bake) and every selector,
  checked against hand-computed numbers; undo/redo restore exactly; a failing op rolls the whole
  call back with an indexed error listing the choices; edits are refused while playing;
- resolution resamples (coincident points exact, the shape kept), sample numbers (sharp/extend and
  fade), the warnings (folded cells, more than 4 lattices, missing lattice, no mesh, ...);
- a save / reload in a second editor re-saving byte-identically, a prefab instance per-point
  override recorded by edit_lattice;
- a Lua script driving offsets in play mode, read back with edit_lattice during play;
- a Scene view screenshot with show_all_lattices against a golden image (golden/lattice_show_all.png;
  JASPER_UPDATE_GOLDEN=1 rewrites it);
- the guide names only tools, parameters, op keys, properties and values that exist.

Runs in a new empty project; the tests share one editor and run in order (state in S).
"""

import math
import os
import re
import shutil
import sys
from pathlib import Path

from harness import REPO, Editor

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import pngdiff  # noqa: E402  (tools/mcp/pngdiff.py)

TIMEOUT = 1200
EDITOR = {"create": True}

S: dict = {}

HERE = Path(__file__).resolve().parent
GOLDEN = HERE / "golden" / "lattice_show_all.png"
SPHERE = REPO / "tools/fixtures/shader-library-export/assets/models/uv_sphere.gltf"
CHECKER = REPO / "tools/fixtures/scene-inputs/assets/textures/checker.png"


# -- helpers --------------------------------------------------------------------


def close(a, b, tol=1e-4):
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        return abs(a - b) <= tol
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(close(x, y, tol) for x, y in zip(a, b))
    if isinstance(a, dict) and isinstance(b, dict):
        return a.keys() == b.keys() and all(close(a[k], b[k], tol) for k in a)
    return a == b


def check_close(ed, actual, expected, what="", tol=1e-4):
    ed.check(close(actual, expected, tol), f"{what}: expected {expected!r}, got {actual!r}")


def idx(res, i, j, k):
    return i + res[0] * (j + res[1] * k)


def rest(res, i, j, k):
    return [i / (res[0] - 1) - 0.5, j / (res[1] - 1) - 0.5, k / (res[2] - 1) - 0.5]


async def el(ed, **args):
    """edit_lattice in the test scene."""
    args.setdefault("scene", S["scene"])
    return await ed.call("edit_lattice", args)


async def el_error(ed, contains=None, **args):
    args.setdefault("scene", S["scene"])
    raw = await ed.call_raw("edit_lattice", args)
    ed.check(raw.is_error, f"edit_lattice should fail: {args} -> {raw.text[:300]}")
    if contains:
        for part in [contains] if isinstance(contains, str) else contains:
            ed.check(part in raw.text, f"error should contain {part!r}: {raw.text}")
    return raw.text


async def offsets(ed, lattice, scene=None) -> list:
    got = await ed.call("get_entity", scene=scene or S["scene"], entity=lattice, components=["lattice"], properties=["offsets"])
    return got["components"]["lattice"]["offsets"]


async def comp(ed, entity, component, *props, scene=None) -> dict:
    args = {"scene": scene or S["scene"], "entity": entity, "components": [component]}
    if props:
        args["properties"] = list(props)
    return (await ed.call("get_entity", args))["components"][component]


async def exists(ed, entity, scene=None) -> bool:
    raw = await ed.call_raw("get_entity", {"scene": scene or S["scene"], "entity": entity})
    return not raw.is_error


async def undo(ed, scene=None):
    await ed.call("undo", scene=scene or S["scene"])


async def redo(ed, scene=None):
    await ed.call("redo", scene=scene or S["scene"])


async def cage(ed, name, position=(0, 0, 0), scale=(2, 2, 2), resolution=None, interpolation=None, outside=None):
    """A lattice made with create_entity type lattice (no targets), optionally set up by edit_lattice."""
    made = await ed.call("create_entity", scene=S["scene"], type="lattice", name=name, position=list(position),
                         scale=list(scale))
    entity = made["entity_id"]
    args = {}
    if resolution:
        args["resolution"] = resolution
    if interpolation:
        args["interpolation"] = interpolation
    if outside:
        args["outside"] = outside
    if args:
        await el(ed, lattice=entity, **args)
    return entity


def point_offsets(read: dict) -> dict:
    """{index: offset} of a detail: points read."""
    return {p["index"]: p["offset"] for p in read["points"]}


async def setup(ed: Editor) -> dict:
    """Scene LatticeLab with the sphere model and the checker texture in the project, once per editor."""
    if S.get("ed") is ed:
        await ed.call("select_scene", scene=S["scene"])
        return S
    S.clear()
    S["ed"] = ed
    models = ed.project / "assets" / "models"
    models.mkdir(parents=True, exist_ok=True)
    shutil.copy(SPHERE, models / "uv_sphere.gltf")
    textures = ed.project / "assets" / "textures"
    textures.mkdir(parents=True, exist_ok=True)
    shutil.copy(CHECKER, textures / "checker.png")
    await ed.call("sync_files")
    made = await ed.call("create_scene", name="LatticeLab", type="3D", path="scenes/LatticeLab")
    S["scene"] = made["scene"]["id"]
    return S


# -- creation -------------------------------------------------------------------


async def test_create_around_model(ed: Editor):
    s = await setup(ed)
    sid = s["scene"]
    ball = (await ed.call("create_entity", scene=sid, type="model", name="Ball", model="assets/models/uv_sphere.gltf",
                          position=[0, 1, 0], scale=[1, 2, 1]))["entity_id"]
    s["ball"] = ball
    made = await el(ed, create={"targets": [ball], "name": "BallCage"})
    lat = made["lattice"]["id"]
    ed.check_eq(made["resolution"], [3, 3, 3], "default resolution")
    ed.check_eq([t["entity"]["id"] for t in made["targets"]], [ball], "bound to the ball")
    ed.check_eq(made["targets"][0]["stage"], "after_skinning")
    ed.check_eq(made["targets"][0]["meshes"], 1)
    ed.check_eq(made["moved_points"], 0)
    ed.check_eq(made["warnings"], [], "a fresh cage has no warnings")
    ed.check_eq(made["ops"][0], {"op": "create", "bound": 1})
    t = await comp(ed, lat, "transform", "position", "scale")
    # the sphere is a unit-diameter sphere (bounds -0.5..0.5) scaled [1, 2, 1], padded by 2% (min 0.01)
    check_close(ed, t["position"], [0, 1, 0], "fitted centre", tol=1e-3)
    check_close(ed, t["scale"], [1.02, 2.04, 1.02], "fitted size (2% padding)", tol=2e-3)
    sel = await ed.call("get_selection", scene=sid)
    ed.check_eq([e["id"] for e in sel["entities"]], [lat], "the new lattice is selected")
    mod = await comp(ed, ball, "latticemodifier")
    ed.check_eq(mod["bindings[0].lattice"]["id"], lat, "the ball's modifier names the cage")
    # one undo step: the cage and the binding go together
    await undo(ed)
    ed.check(not await exists(ed, lat), "undo removes the cage")
    raw = await ed.call_raw("get_entity", {"scene": sid, "entity": ball, "components": ["latticemodifier"]})
    ed.check(raw.is_error or not raw.structured.get("components", {}).get("latticemodifier"), f"undo removes the binding: {raw.text[:200]}")
    await redo(ed)
    ed.check(await exists(ed, lat), "redo brings it back")

    # create_entity type lattice with targets: the same fit and binding (the editor command)
    other = (await ed.call("create_entity", scene=sid, type="model", name="Ball2", model="assets/models/uv_sphere.gltf",
                           position=[0, 1, 0], scale=[1, 2, 1]))["entity_id"]
    before_sel = [e["id"] for e in (await ed.call("get_selection", scene=sid))["entities"]]
    made2 = await ed.call("create_entity", scene=sid, type="lattice", name="Ball2Cage", targets=[other], select=False)
    lat2 = made2["entity_id"]
    ed.check_eq([b["id"] for b in made2["bound"]], [other], "create_entity binds the target")
    t2 = await comp(ed, lat2, "transform", "position", "scale", "rotation")
    check_close(ed, t2["position"], t["position"], "same centre as edit_lattice create", tol=1e-5)
    check_close(ed, t2["scale"], t["scale"], "same size as edit_lattice create", tol=1e-5)
    sel = await ed.call("get_selection", scene=sid)
    ed.check_eq([e["id"] for e in sel["entities"]], before_sel, "select: false keeps the selection")
    await undo(ed)
    ed.check(not await exists(ed, lat2), "create_entity lattice is one undo step")
    await ed.call("delete_entities", scene=sid, entity=other)
    s["cage"] = lat

    # a target without a mesh is not bound (noted), a parent holds the cage keeping the fit
    empty = (await ed.call("create_entity", scene=sid, type="empty", name="Nothing", position=[5, 0, 0]))["entity_id"]
    holder = (await ed.call("create_entity", scene=sid, type="empty", name="Holder", position=[0, 3, 0]))["entity_id"]
    made3 = await ed.call("create_entity", scene=sid, type="lattice", name="Held", targets=[ball, empty], parent=holder)
    ed.check_eq([b["id"] for b in made3["bound"]], [ball], "only the mesh target is bound")
    ed.check("no mesh" in made3["notes"][0], f"note for the target without a mesh: {made3}")
    world = (await ed.call("get_entity", scene=sid, entity=made3["entity_id"], detail="summary"))["world"]["position"]
    check_close(ed, world, [0, 1, 0], "the fit is kept under the parent (world)", tol=1e-3)
    await undo(ed)
    ed.check(not await exists(ed, made3["entity_id"]), "parented create undone in one step")
    mod = await comp(ed, ball, "latticemodifier", "bindings")
    ed.check_eq(len(mod["bindings"]), 1, "the undone cage's binding is gone")
    await ed.call("delete_entities", scene=sid, entities=[empty, holder])
    errors = await ed.call_raw("create_entity", {"scene": sid, "type": "cube", "targets": [ball]})
    ed.check(errors.is_error and "only for type" in errors.text, errors.text)


async def test_create_around_sprite(ed: Editor):
    await setup(ed)
    made = await ed.call("create_scene", name="LatticeLab2D", type="2D", path="scenes/LatticeLab2D")
    sid = made["scene"]["id"]
    sprite = (await ed.call("create_entity", scene=sid, type="sprite", name="Flag", position=[100, 80, 0],
                            values={"sprite": {"width": 128, "height": 64}, "mesh": {"submeshes[0].material.baseColorTexture": "assets/textures/checker.png"}}))["entity_id"]
    made = await ed.call("edit_lattice", scene=sid, create={"targets": [sprite]})
    ed.check_eq(made["resolution"][2], 2, f"a flat target gets 2 points across: {made['resolution']}")
    ed.check_eq(made["resolution"][:2], [3, 3])
    ed.check_eq(made["targets"][0]["entity"]["id"], sprite)
    t = await comp(ed, made["lattice"]["id"], "transform", "position", "scale", scene=sid)
    # the sprite's pivot is its bottom-left corner: the cage is centred on the drawn quad
    check_close(ed, t["position"][:2], [100 + 64, 80 + 32], "centred on the sprite", tol=0.5)
    check_close(ed, t["scale"][:2], [128 * 1.02, 64 * 1.02], "fitted to the sprite", tol=0.5)
    # lattice tracks: an action for the lattice, as morph tracks are for a mesh
    tracks = await ed.call("create_entity", scene=sid, type="lattice_tracks", parent=made["lattice"]["id"])
    ed.check_in("latticetracks", tracks["components"])
    ed.check_in("keyframetracks", tracks["components"])
    action = await comp(ed, tracks["entity_id"], "action", "target", scene=sid)
    ed.check_eq(action["target"]["id"], made["lattice"]["id"], "the action targets the lattice")
    bent = await ed.call("edit_lattice", scene=sid, lattice=made["lattice"]["id"],
                         ops=[{"op": "preset", "name": "bend", "axis": "x", "toward": "y", "amount": 40}])
    ed.check(bent["moved_points"] > 0 and bent["warnings"] == [], f"the flat cage bends: {bent}")
    S["scene2d"] = sid
    await ed.call("select_scene", scene=S["scene"])


async def test_lattice_fallback(ed: Editor):
    s = await setup(ed)
    await ed.call("select_entities", scene=s["scene"], entity=s["cage"])
    read = await el(ed)
    ed.check_eq(read["lattice"]["id"], s["cage"], "the selected lattice")
    ed.check("ops" not in read, "a read reports no ops")
    await ed.call("select_entities", scene=s["scene"], entities=[])
    # nothing selected and only one lattice: that one
    read = await el(ed)
    ed.check_eq(read["lattice"]["id"], s["cage"], "the scene's only lattice")
    second = await cage(ed, "Second", position=(10, 0, 0))
    await ed.call("select_entities", scene=s["scene"], entities=[])
    await el_error(ed, contains=["pass lattice", "BallCage", "Second"])
    await el_error(ed, lattice=s["ball"], contains=["is not a lattice", "lattices:"])
    await el_error(ed, lattice=s["cage"], create={}, contains="not both")
    await ed.call("delete_entities", scene=s["scene"], entity=second)


# -- selectors and ops ----------------------------------------------------------


async def test_selectors(ed: Editor):
    s = await setup(ed)
    lat = await cage(ed, "SelCage", position=(20, 0, 0))
    s["sel"] = lat

    async def count(select):
        got = await el(ed, lattice=lat, ops=[{"op": "reset", "select": select}])
        return got["ops"][0]["points"]

    ed.check_eq(await count("all"), 27)
    ed.check_eq(await count("exterior"), 26)
    ed.check_eq(await count("interior"), 1)
    for face in ("+x", "-x", "+y", "-y", "+z", "-z", "top", "bottom", "left", "right", "front", "back"):
        ed.check_eq(await count(face), 9, face)
    ed.check_eq(await count({"face": "top", "i": [0, 1]}), 6, "face and an i range intersect")
    ed.check_eq(await count({"i": 0, "j": [1, 2], "k": 2}), 2)
    ed.check_eq(await count({"points": [[0, 0, 0], 5, [2, 2, 2]]}), 3)
    ed.check_eq(await count([[0, 0, 0], [1, 1, 1]]), 2, "a bare point list")
    # world box and sphere on the cage at [20, 0, 0], size 2: points at x 19, 20, 21
    ed.check_eq(await count({"box": {"min": [20.5, -2, -2], "max": [22, 2, 2]}}), 9, "box: the +x face")
    ed.check_eq(await count({"box": {"min": [22, 2, 2], "max": [20.5, -2, -2]}}), 9, "box min/max swapped")
    ed.check_eq(await count({"sphere": {"center": [20, 0, 0], "radius": 1.01}}), 7, "sphere: centre + 6 face centres")
    ed.check_eq(await count({"sphere": {"center": [21, 1, 1], "radius": 0.1}, "face": "+y"}), 1)

    # moved points are tested where they are now
    await el(ed, lattice=lat, ops=[{"op": "set", "points": [{"ijk": [1, 1, 1], "offset": [0, 0, 0.45]}]}])
    ed.check_eq(await count({"box": {"min": [19.9, -0.1, 0.8], "max": [20.1, 0.1, 1.0]}}), 2, "box on current positions")

    # errors: indexed, listing the choices, nothing changed
    await el_error(ed, lattice=lat, ops=[{"op": "reset", "select": "+w"}],
                   contains=["ops[0].select", "+x", "exterior", "front"])
    await el_error(ed, lattice=lat, ops=[{"op": "reset"}, {"op": "reset", "select": {"face": "middle"}}],
                   contains=["ops[1].select.face", "+x, -x, +y"])
    await el_error(ed, lattice=lat, ops=[{"op": "reset", "select": {"i": 3}}], contains=["ops[0].select.i", "0..2"])
    await el_error(ed, lattice=lat, ops=[{"op": "reset", "select": {"points": [[0, 3, 0]]}}],
                   contains=["ops[0].select.points[0]", "out of range"])
    await el_error(ed, lattice=lat, ops=[{"op": "reset", "select": {"box": {"min": [0, 0, 0], "max": [1, 1, 1]}}}],
                   contains=["ops[0].select", "selects no points", "span world"])
    await el_error(ed, lattice=lat, ops=[{"op": "reset", "select": {"corner": 1}}], contains=["unknown key", "sphere"])
    await el_error(ed, lattice=lat, ops=[{"op": "reset", "select": "selected"}], contains="selected in the Scene view")
    small = await cage(ed, "Small", position=(30, 0, 0), resolution=[2, 2, 2])
    await el_error(ed, lattice=small, ops=[{"op": "reset", "select": "interior"}], contains="interior")
    await ed.call("delete_entities", scene=s["scene"], entity=small)
    await el(ed, lattice=lat, ops=[{"op": "reset"}])


async def test_ops_numbers(ed: Editor):
    s = await setup(ed)
    res = [3, 3, 3]
    lat = await cage(ed, "OpCage", position=(1, 0, 0), scale=(2, 2, 2))
    s["op"] = lat

    async def pts(**args):
        return point_offsets(await el(ed, lattice=lat, detail="points", **args))

    # set: offset / position, local / world, ijk / index
    p = await pts(ops=[{"op": "set", "points": [
        {"ijk": [1, 2, 1], "offset": [0, 0.25, 0]},
        {"index": idx(res, 2, 2, 2), "position": [3, 1, 1], "space": "world"},      # local (1, .5, .5): offset (.5, 0, 0)
        {"ijk": [0, 0, 0], "position": [-0.5, -0.75, -0.5]},                         # local position
        {"ijk": [2, 0, 0], "offset": [0, 1, 0], "space": "world"}]}])               # 1 world = 0.5 local
    check_close(ed, p[idx(res, 1, 2, 1)], [0, 0.25, 0], "set offset")
    check_close(ed, p[idx(res, 2, 2, 2)], [0.5, 0, 0], "set world position")
    check_close(ed, p[idx(res, 0, 0, 0)], [0, -0.25, 0], "set local position")
    check_close(ed, p[idx(res, 2, 0, 0)], [0, 0.5, 0], "set world offset")
    read = await el(ed, lattice=lat)
    ed.check_eq(read["moved_points"], 4)
    check_close(ed, read["max_offset"], {"local": 0.5, "world": 1.0}, "max_offset local and world")
    await el(ed, lattice=lat, ops=[{"op": "reset", "select": "all"}])

    # move: translation local / world, rotation (twist sense: +x goes to -z around +y), scale, pivots
    p = await pts(ops=[{"op": "move", "select": "top", "offset": [0, 0.1, 0]}])
    ed.check(all(close(p[idx(res, i, 2, k)], [0, 0.1, 0]) for i in range(3) for k in range(3)) and len(p) == 9, f"move top: {p}")
    p = await pts(ops=[{"op": "reset"}, {"op": "move", "select": "top", "offset": [0, 0.5, 0], "space": "world"}])
    check_close(ed, p[idx(res, 0, 2, 0)], [0, 0.25, 0], "world offset to local")
    p = await pts(ops=[{"op": "reset"}, {"op": "move", "select": "all", "rotate": [0, 90, 0], "pivot": "lattice"}])
    check_close(ed, p[idx(res, 2, 1, 1)], [-0.5, 0, -0.5], "rotate 90 about y: +x goes to -z")
    ed.check(idx(res, 1, 1, 1) not in p, "the pivot point stays")
    p = await pts(ops=[{"op": "reset"}, {"op": "move", "select": "top", "scale": 2}])
    check_close(ed, p[idx(res, 2, 2, 2)], [0.5, 0, 0.5], "scale 2 about the top face's centre")
    ed.check(idx(res, 1, 2, 1) not in p, "the centre of the face (the pivot) stays")
    p = await pts(ops=[{"op": "reset"}, {"op": "move", "select": "top", "scale": [1, 1, 2], "pivot": [0, 0, -0.5]}])
    check_close(ed, p[idx(res, 0, 2, 2)], [0, 0, 1.0], "scale about a local pivot")
    p = await pts(ops=[{"op": "reset"}, {"op": "move", "select": {"points": [[2, 1, 1]]}, "rotate": [0, 0, 90],
                                          "pivot": [1, 0, 0], "space": "world"}])
    # world pivot = the cage's centre: +x (local .5 = world 2) turns to +y (counterclockwise about +z)
    check_close(ed, p[idx(res, 2, 1, 1)], [-0.5, 0.5, 0], "rotate about a world pivot")
    # falloff: the top centre up 0.5, linear radius 0.6: neighbours 0.5 away get 1/6
    p = await pts(ops=[{"op": "reset"}, {"op": "move", "select": [[1, 2, 1]], "offset": [0, 0.5, 0],
                                          "falloff": {"radius": 0.6, "curve": "linear"}}])
    check_close(ed, p[idx(res, 1, 2, 1)], [0, 0.5, 0], "falloff: the selected point")
    check_close(ed, p[idx(res, 0, 2, 1)], [0, 0.5 / 6, 0], "falloff: a neighbour", tol=1e-4)
    check_close(ed, p[idx(res, 1, 1, 1)], [0, 0.5 / 6, 0], "falloff: the one below")
    ed.check(idx(res, 0, 2, 0) not in p, "falloff: beyond the radius")
    p = await pts(ops=[{"op": "reset"}, {"op": "move", "select": [[1, 2, 1]], "offset": [0, 0.5, 0],
                                          "falloff": {"radius": 0.6, "curve": "constant"}}])
    check_close(ed, p[idx(res, 0, 2, 1)], [0, 0.5, 0], "constant falloff")
    await el_error(ed, lattice=lat, ops=[{"op": "move", "select": "top"}], contains=["ops[0]", "offset", "rotate", "scale"])
    await el_error(ed, lattice=lat, ops=[{"op": "move", "offset": [0, 1, 0], "falloff": {"radius": 1, "curve": "cubic"}}],
                   contains=["ops[0].falloff.curve", "smooth, linear, sharp, constant"])
    await el_error(ed, lattice=lat, ops=[{"op": "move", "offset": [0, 1, 0], "pivot": "middle"}],
                   contains=["ops[0].pivot", "center, lattice"])
    await el_error(ed, lattice=lat, ops=[{"op": "move", "offst": [0, 1, 0]}], contains=["ops[0]", "unknown key \"offst\""])

    # presets (spec §4), on all points
    p = await pts(ops=[{"op": "reset"}, {"op": "preset", "name": "twist", "axis": "y", "amount": 90}])
    check_close(ed, p[idx(res, 2, 2, 1)], [-0.5, 0, -0.5], "twist 90: the top +x point goes to -z")
    ed.check(all(idx(res, i, 0, k) not in p for i in range(3) for k in range(3)), "twist: the bottom stays")
    p = await pts(ops=[{"op": "reset"}, {"op": "preset", "name": "squash", "amount": 0.5}])
    r = 1 / math.sqrt(0.5)
    check_close(ed, p[idx(res, 2, 2, 2)], [0.5 * r - 0.5, -0.25, 0.5 * r - 0.5], "squash 0.5: volume kept")
    p = await pts(ops=[{"op": "reset"}, {"op": "preset", "name": "shear", "axis": "y", "amount": 0.2}])
    check_close(ed, p[idx(res, 0, 2, 0)], [0, 0, 0.2], "shear toward the next axis (y -> z)")
    p = await pts(ops=[{"op": "reset"}, {"op": "preset", "name": "taper", "axis": "y", "amount": 0.5}])
    check_close(ed, p[idx(res, 2, 2, 2)], [-0.25, 0, -0.25], "taper 0.5: the top half as wide")
    p = await pts(ops=[{"op": "reset"}, {"op": "preset", "name": "bulge", "axis": "y", "amount": 1}])
    check_close(ed, p[idx(res, 2, 1, 2)], [0.5, 0, 0.5], "bulge 1: the middle twice as wide")
    p = await pts(ops=[{"op": "reset"}, {"op": "preset", "name": "stretch", "amount": 1}])
    check_close(ed, p[idx(res, 1, 2, 1)], [0, 0.5, 0], "stretch 1: twice as long")
    p = await pts(ops=[{"op": "reset"}, {"op": "preset", "name": "bend", "axis": "y", "toward": "x", "amount": 90}])
    # the axis line (centre column) becomes a quarter circle of length 1 from the bottom: the top centre
    # ends at (2/pi, -0.5 + 2/pi) local
    check_close(ed, p[idx(res, 1, 2, 1)], [2 / math.pi, -0.5 + 2 / math.pi - 0.5, 0], "bend 90: the axis arc", tol=1e-4)
    n1 = await pts(ops=[{"op": "reset"}, {"op": "preset", "name": "noise", "amount": 0.1, "seed": 7}])
    n2 = await pts(ops=[{"op": "reset"}, {"op": "preset", "name": "noise", "amount": 0.1, "seed": 7}])
    ed.check(n1 == n2 and len(n1) > 20, "noise: the same seed gives the same offsets")
    ed.check(all(math.sqrt(sum(c * c for c in v)) <= 0.1 + 1e-6 for v in n1.values()), "noise within amount")
    n3 = await pts(ops=[{"op": "reset"}, {"op": "preset", "name": "noise", "amount": 0.1, "seed": 8}])
    ed.check(n3 != n1, "another seed, another shape")
    p = await pts(ops=[{"op": "reset"}, {"op": "set", "points": [{"ijk": [0, 0, 0], "offset": [0.1, 0, 0]}]},
                       {"op": "preset", "name": "shear", "axis": "y", "amount": 0.2, "mode": "replace", "select": "top"}])
    check_close(ed, p[idx(res, 0, 0, 0)], [0.1, 0, 0], "replace on the top leaves other points")
    check_close(ed, p[idx(res, 0, 2, 0)], [0, 0, 0.2], "replace sets the selected points")
    await el_error(ed, lattice=lat, ops=[{"op": "preset", "name": "wobble", "amount": 1}],
                   contains=["ops[0].name", "squash, stretch, taper, twist, bend, bulge, shear, noise"])
    await el_error(ed, lattice=lat, ops=[{"op": "preset", "name": "twist"}], contains=["ops[0].amount", "degrees"])
    await el_error(ed, lattice=lat, ops=[{"op": "preset", "name": "squash", "amount": 2}], contains="0.95")
    await el_error(ed, lattice=lat, ops=[{"op": "preset", "name": "bend", "axis": "y", "toward": "y", "amount": 30}],
                   contains="toward")
    await el_error(ed, lattice=lat, ops=[{"op": "preset", "name": "twist", "axis": "w", "amount": 1}],
                   contains=["ops[0].axis", "x, y, z"])

    # mirror: + half onto - half, the middle plane loses its x
    p = await pts(ops=[{"op": "reset"}, {"op": "set", "points": [{"ijk": [2, 2, 2], "offset": [0.1, 0.2, 0.3]},
                                                                  {"ijk": [1, 0, 0], "offset": [0.4, 0.1, 0]}]},
                       {"op": "mirror", "axis": "x"}])
    check_close(ed, p[idx(res, 0, 2, 2)], [-0.1, 0.2, 0.3], "mirrored point")
    check_close(ed, p[idx(res, 1, 0, 0)], [0, 0.1, 0], "middle plane: x zeroed")
    p = await pts(ops=[{"op": "mirror", "axis": "x", "from": "-"}])
    check_close(ed, p[idx(res, 2, 2, 2)], [0.1, 0.2, 0.3], "from - copies back")
    await el_error(ed, lattice=lat, ops=[{"op": "mirror", "axis": "x", "from": "left"}], contains=["ops[0].from", "\"+\""])

    # poses
    got = await el(ed, lattice=lat, ops=[{"op": "pose", "save": "Mirrored"}, {"op": "reset"},
                                         {"op": "pose", "apply": "Mirrored", "weight": 0.5}], detail="points")
    ed.check_eq(got["poses"], ["Mirrored"])
    check_close(ed, point_offsets(got)[idx(res, 2, 2, 2)], [0.05, 0.1, 0.15], "pose applied at half weight")
    await el_error(ed, lattice=lat, ops=[{"op": "pose", "apply": "Nope"}], contains=["ops[0].apply", "Mirrored"])
    await el_error(ed, lattice=lat, ops=[{"op": "pose", "save": "A", "apply": "B"}], contains="exactly one")
    got = await el(ed, lattice=lat, ops=[{"op": "pose", "delete": "Mirrored"}, {"op": "reset"}])
    ed.check_eq(got["poses"], [])

    # unknown ops and bad shapes
    await el_error(ed, lattice=lat, ops=[{"op": "explode"}], contains=["ops[0].op", "set, move, reset, preset, mirror, fit, bind"])
    await el_error(ed, lattice=lat, ops=[{"select": "all"}], contains="ops[0]")
    await el_error(ed, lattice=lat, ops=[{"op": "set", "points": [{"ijk": [0, 0, 0]}]}],
                   contains=["ops[0].points[0]", "offset or position"])
    await el_error(ed, lattice=lat, ops=[{"op": "set", "points": [{"ijk": [3, 0, 0], "offset": [0, 0, 0]}]}],
                   contains=["ops[0].points[0].ijk", "out of range"])
    await el_error(ed, lattice=lat, interpolation="bicubic", contains=["interpolation", "sharp, smooth, cubic"])
    await el_error(ed, lattice=lat, resolution=[3, 17, 3], contains=["resolution", "2..16"])
    await el_error(ed, lattice=lat, fade_distance=-1, contains="fade_distance")


async def test_bind_fit_unbind(ed: Editor):
    s = await setup(ed)
    sid = s["scene"]
    lat = s["op"]
    box = (await ed.call("create_entity", scene=sid, type="cube", name="OpBox", position=[1, 0, 0]))["entity_id"]
    group = (await ed.call("create_entity", scene=sid, type="empty", name="OpGroup", position=[1, 0, 3]))["entity_id"]
    child = (await ed.call("create_entity", scene=sid, type="sphere", name="OpChild", parent=group))["entity_id"]
    got = await el(ed, lattice=lat, ops=[
        {"op": "bind", "target": box, "weight": 0.5, "stage": "before_skinning", "mask": "red", "submesh": 0},
        {"op": "bind", "target": group}])
    targets = {t["entity"]["id"]: t for t in got["targets"]}
    ed.check_eq(targets[box]["weight"], 0.5)
    ed.check_eq(targets[box]["stage"], "before_skinning")
    ed.check_eq(targets[box]["mask"], "red")
    ed.check_eq(targets[box]["submesh"], 0)
    ed.check_eq(targets[group]["meshes"], 1, "a group without a mesh binds with include_children")
    # binding again changes only what is given
    got = await el(ed, lattice=lat, ops=[{"op": "bind", "target": box, "weight": 1.5}])
    t = {t["entity"]["id"]: t for t in got["targets"]}[box]
    ed.check_eq([t["weight"], t["stage"], t["mask"]], [1.5, "before_skinning", "red"], "re-bind keeps the other settings")
    await el_error(ed, lattice=lat, ops=[{"op": "bind", "target": box, "weight": 3}], contains=["ops[0].weight", "0..2"])
    await el_error(ed, lattice=lat, ops=[{"op": "bind", "target": box, "mask": "purple"}],
                   contains=["ops[0].mask", "inverted_red"])
    await el_error(ed, lattice=lat, ops=[{"op": "bind", "target": lat}], contains="cannot deform itself")
    await el_error(ed, lattice=lat, ops=[{"op": "bind", "target": "NoSuchThing"}], contains="ops[0].target")
    # fit around the targets (default) and around one given
    got = await el(ed, lattice=lat, ops=[{"op": "fit", "targets": [box], "padding": 0}])
    t = await comp(ed, lat, "transform", "position", "scale")
    check_close(ed, t["position"], [1, 0, 0], "fit centre", tol=1e-4)
    check_close(ed, t["scale"], [1.01, 1.01, 1.01], "fit size: the cube + the 0.01 minimum padding", tol=1e-4)
    await el(ed, lattice=lat, ops=[{"op": "fit"}])
    t = await comp(ed, lat, "transform", "position")
    ed.check(t["position"][2] > 1.0, f"fit to every target spans the group too: {t}")
    # unbind
    got = await el(ed, lattice=lat, ops=[{"op": "unbind", "target": group}])
    ed.check_eq([t["entity"]["id"] for t in got["targets"]], [box])
    await el_error(ed, lattice=lat, ops=[{"op": "unbind", "target": group}], contains="not bound")
    await el(ed, lattice=lat, ops=[{"op": "unbind", "target": box}])
    await ed.call("delete_entities", scene=sid, entities=[box, group])
    del child


async def test_undo_redo_and_rollback(ed: Editor):
    s = await setup(ed)
    lat = s["op"]
    before = await offsets(ed, lat)
    tb = await comp(ed, lat, "transform", "position", "scale")
    got = await el(ed, lattice=lat, resolution=[4, 3, 3], interpolation="smooth", outside="extend", fade_distance=0.5,
                   ops=[{"op": "preset", "name": "twist", "amount": 45},
                        {"op": "move", "select": "top", "offset": [0, 0.2, 0]},
                        {"op": "mirror", "axis": "z"},
                        {"op": "pose", "save": "P"}])
    ed.check_eq([o["op"] for o in got["ops"]], ["preset", "move", "mirror", "pose"])
    after = await offsets(ed, lat)
    after_lat = await comp(ed, lat, "lattice", "resolutionX", "interpolation", "outside", "fadeDistance", "poses")
    await undo(ed)
    ed.check_eq(await offsets(ed, lat), before, "one undo restores the offsets exactly")
    back = await comp(ed, lat, "lattice", "resolutionX", "interpolation", "outside", "fadeDistance", "poses")
    ed.check_eq([back["resolutionX"], back["interpolation"], back["outside"], back["fadeDistance"], back["poses"]],
                [3, "cubic", "fade", 0.25, []], "and the settings")
    ed.check_eq(await comp(ed, lat, "transform", "position", "scale"), tb)
    await redo(ed)
    ed.check_eq(await offsets(ed, lat), after, "redo restores the edit exactly")
    ed.check_eq(await comp(ed, lat, "lattice", "resolutionX", "interpolation", "outside", "fadeDistance", "poses"), after_lat)

    # a failing op rolls back everything the call did before it, and leaves no undo step
    text = await el_error(ed, lattice=lat, resolution=[3, 3, 3],
                          ops=[{"op": "move", "select": "all", "offset": [0, 1, 0]}, {"op": "reset", "select": "+q"}],
                          contains=["ops[1].select", "rolled back"])
    ed.check_eq(await offsets(ed, lat), after, f"rolled back: {text}")
    await undo(ed)
    ed.check_eq(await offsets(ed, lat), before, "the failed call left no undo step of its own")
    await el(ed, lattice=lat, ops=[{"op": "reset"}])


async def test_resolution_resample(ed: Editor):
    s = await setup(ed)
    lat = await cage(ed, "ResCage", position=(0, 0, 10), scale=(2, 2, 2), interpolation="sharp")
    await el(ed, lattice=lat, ops=[{"op": "preset", "name": "bend", "axis": "y", "amount": 60},
                                   {"op": "preset", "name": "twist", "axis": "y", "amount": 30}])
    old = await offsets(ed, lat)
    probes = [[x * 0.37, y * 0.41, 10 + z * 0.43] for x in (-2, 0, 2) for y in (-2, -1, 0, 1, 2) for z in (-2, 2)]
    before = (await el(ed, lattice=lat, sample=probes))["samples"]
    got = await el(ed, lattice=lat, resolution=[5, 5, 5])
    ed.check_eq(got["resolution"], [5, 5, 5])
    new = await offsets(ed, lat)
    ed.check_eq(len(new), 125)
    for i in range(3):
        for j in range(3):
            for k in range(3):
                ed.check_eq(new[idx([5, 5, 5], 2 * i, 2 * j, 2 * k)], old[idx([3, 3, 3], i, j, k)],
                            f"coincident point ({i},{j},{k}) keeps its offset exactly")
    after = (await el(ed, lattice=lat, sample=probes))["samples"]
    for a, b in zip(before, after):
        check_close(ed, b["deformed"], a["deformed"], f"the shape is kept at {a['point']}", tol=1e-5)
    # set_properties refuses a lone resolution write and points here
    raw = await ed.call_raw("set_properties", {"scene": s["scene"], "entity": lat, "values": {"lattice": {"resolutionX": 4}}})
    ed.check(raw.is_error and "edit_lattice resolution" in raw.text, raw.text)
    await ed.call("delete_entities", scene=s["scene"], entity=lat)


async def test_sample_numbers(ed: Editor):
    s = await setup(ed)
    lat = await cage(ed, "SampleCage", position=(0, 0, -10), scale=(2, 2, 2), resolution=[2, 2, 2],
                     interpolation="sharp", outside="extend")
    await el(ed, lattice=lat, ops=[{"op": "move", "select": "+y", "offset": [0, 0.5, 0]}])
    got = await el(ed, lattice=lat, sample=[[0, 0, -10], [0, 1, -10], [0, 2, -10], [0.5, -1, -10.5]])
    sm = got["samples"]
    check_close(ed, sm[0]["deformed"], [0, 0.5, -10], "the centre rises by half the top's lift (world)")
    check_close(ed, sm[0]["stretch"], 1.5, "trilinear stretch: 1 + 0.5")
    check_close(ed, sm[1]["deformed"], [0, 2, -10], "the top rises by 1 world unit")
    check_close(ed, sm[2]["deformed"], [0, 3, -10], "extend: beyond the top keeps the border lift")
    check_close(ed, sm[3]["deformed"], [0.5, -1, -10.5], "the bottom face stays")
    check_close(ed, sm[0]["point"], [0, 0, -10], "the input point is echoed")
    got = await el(ed, lattice=lat, outside="fade", fade_distance=0.25, sample=[[0, 2, -10], [0, 1.25, -10]])
    check_close(ed, got["samples"][0]["deformed"], [0, 2, -10], "fade: 0.5 local beyond the cage is untouched")
    # 0.125 local beyond (half the fade): f = 1 - smoothstep(0.5) = 0.5 of a 0.5 local lift = 0.5 world
    check_close(ed, got["samples"][1]["deformed"], [0, 1.75, -10], "fade: half way out, half the lift")
    await el_error(ed, lattice=lat, sample=[[0, 0]], contains="sample[0]")
    s["sample_cage"] = lat


async def test_warnings(ed: Editor):
    s = await setup(ed)
    sid = s["scene"]
    lat = s["sample_cage"]
    got = await el(ed, lattice=lat, ops=[{"op": "reset"}, {"op": "move", "select": "+y", "offset": [0, -1.5, 0]}])
    kinds = {w["kind"]: w for w in got["warnings"]}
    ed.check_in("folded_cells", kinds, f"folded warning: {got['warnings']}")
    fold = kinds["folded_cells"]
    ed.check_eq(fold["cells"], [[0, 0, 0]], "the folded cell's (i, j, k)")
    ed.check("Fix:" in fold["message"] and "(0,0,0)" in fold["message"], fold["message"])
    ed.check_in("no_targets", kinds, "a lattice bound to nothing says so")
    got = await el(ed, lattice=lat, ops=[{"op": "reset"}])
    ed.check("folded_cells" not in {w["kind"] for w in got["warnings"]}, "reset clears the fold")

    # more than 4 lattices on one mesh
    crowd = (await ed.call("create_entity", scene=sid, type="cube", name="Crowd", position=[0, 0, -20]))["entity_id"]
    cages = []
    for n in range(5):
        made = await el(ed, create={"targets": [crowd], "name": f"Crowd{n}"})
        cages.append(made["lattice"]["id"])
    kinds = {w["kind"]: w for w in made["warnings"]}
    ed.check_in("too_many_lattices", kinds, f"{made['warnings']}")
    ed.check_eq(kinds["too_many_lattices"]["count"], 5)
    # a binding to a deleted lattice
    await ed.call("delete_entities", scene=sid, entity=cages[4])
    got = await el(ed, lattice=cages[0])
    kinds = {w["kind"]: w for w in got["warnings"]}
    ed.check_in("missing_lattice", kinds, f"{got['warnings']}")
    ed.check("too_many_lattices" not in kinds, "four lattices are fine")
    # a target without a mesh, a group without include_children, disabled, zero scale
    empty = (await ed.call("create_entity", scene=sid, type="empty", name="EmptyTarget"))["entity_id"]
    group = (await ed.call("create_entity", scene=sid, type="empty", name="GroupTarget"))["entity_id"]
    await ed.call("create_entity", scene=sid, type="cube", name="GroupChild", parent=group)
    got = await el(ed, lattice=lat, ops=[{"op": "bind", "target": empty}, {"op": "bind", "target": group, "include_children": False}])
    msgs = [w["message"] for w in got["warnings"] if w["kind"] == "no_mesh"]
    ed.check(len(msgs) == 2 and any("include_children: true" in m for m in msgs), f"no_mesh warnings: {msgs}")
    await ed.call("set_properties", scene=sid, entity=lat, values={"lattice": {"enabled": False}, "transform": {"scale": [0, 2, 2]}})
    got = await el(ed, lattice=lat)
    kinds = {w["kind"] for w in got["warnings"]}
    ed.check({"disabled", "zero_scale"} <= kinds, f"{kinds}")
    S["warning_kinds"] = kinds | {"folded_cells", "no_targets", "too_many_lattices", "missing_lattice", "no_mesh"}
    await ed.call("delete_entities", scene=sid, entities=[lat, empty, group, crowd] + cages[:4])


# -- files, prefabs, play -------------------------------------------------------


async def test_save_reload_byte_identical(ed: Editor):
    s = await setup(ed)
    sid = s["scene"]
    await el(ed, lattice=s["cage"], interpolation="smooth", ops=[
        {"op": "preset", "name": "bend", "axis": "y", "toward": "x", "amount": 35},
        {"op": "set", "points": [{"ijk": [1, 1, 1], "offset": [0.1, -0.2, 0.3]}]},
        {"op": "pose", "save": "Bent"},
        {"op": "bind", "target": s["ball"], "weight": 0.75}])
    await ed.call("save_scene", scene=sid)
    path = ed.project / "scenes/LatticeLab.scene"
    saved = path.read_text()
    expected = await offsets(ed, s["cage"])
    async with Editor(project=ed.project, label="lattice-mcp-reload") as ed2:
        scene_file = ed2.project / "scenes/LatticeLab.scene"
        ed2.check_eq(scene_file.read_text(), saved, "the copy holds the saved scene")
        sid2 = (await ed2.call("open_scene", scene="scenes/LatticeLab.scene"))["scene"]["id"]
        got = await ed2.call("edit_lattice", scene=sid2, lattice="BallCage", detail="points")
        ed2.check_eq(got["interpolation"], "smooth")
        ed2.check_eq(got["poses"], ["Bent"])
        ed2.check_eq([t["weight"] for t in got["targets"]], [0.75])
        off = (await ed2.call("get_entity", scene=sid2, entity="BallCage", components=["lattice"],
                              properties=["offsets"]))["components"]["lattice"]["offsets"]
        ed2.check_eq(off, expected, "every offset reloads exactly")
        scene_file.write_text("")
        await ed2.call("set_properties", scene=sid2, entity="BallCage", values={"lattice": {"fadeDistance": 0.5}})
        await ed2.call("set_properties", scene=sid2, entity="BallCage", values={"lattice": {"fadeDistance": 0.25}})
        await ed2.call("save_scene", scene=sid2)
        resaved = scene_file.read_text()
        if resaved != saved:
            import difflib
            diff = "".join(list(difflib.unified_diff(saved.splitlines(True), resaved.splitlines(True)))[:60])
            ed2.fail("re-save is not byte-identical:\n" + diff)


async def test_prefab_point_override(ed: Editor):
    s = await setup(ed)
    sid = s["scene"]
    root = await cage(ed, "PrefabCage", position=(-10, 0, 0))
    await ed.call("create_prefab", scene=sid, entity=root, path="prefabs/PrefabCage.prefab")
    inst = (await ed.call("instantiate_prefab", scene=sid, prefab="prefabs/PrefabCage.prefab", position=[-14, 0, 0],
                          name="PrefabCage B"))["instance"]["id"]
    await el(ed, lattice=inst, ops=[{"op": "set", "points": [{"index": 2, "offset": [0, 0.3, 0]}]}])
    ov = (await ed.call("get_prefab_info", scene=sid, entity=inst, usage=False))["overrides"]
    props = [p for e in ov if e["kind"] == "property" and e.get("component") == "lattice" for p in e["properties"]]
    ed.check_eq(props, ["offsets[2]"], f"edit_lattice records a per-point override: {ov}")
    await undo(ed)
    ov = (await ed.call("get_prefab_info", scene=sid, entity=inst, usage=False))["overrides"]
    props = [p for e in ov if e["kind"] == "property" and e.get("component") == "lattice" for p in e["properties"]]
    ed.check_eq(props, [], f"undo removes the override: {ov}")
    await el(ed, lattice=inst, ops=[{"op": "move", "select": {"points": [5, 7]}, "offset": [0.1, 0, 0]}])
    ov = (await ed.call("get_prefab_info", scene=sid, entity=inst, usage=False))["overrides"]
    props = sorted(p for e in ov if e["kind"] == "property" and e.get("component") == "lattice" for p in e["properties"])
    ed.check_eq(props, ["offsets[5]", "offsets[7]"], f"two moved points, two overrides: {ov}")
    await ed.call("delete_entities", scene=sid, entities=[root, inst])


WAVE = """local Wave = {}

function Wave:init()
    RegisterEngineEvent(self, "onUpdate")
    self.cage = Lattice(self.scene, self.entity)
    self.t = 0
end

function Wave:onUpdate()
    self.t = self.t + Engine.deltatime
    self.cage:setOffset(1, 2, 1, Vector3(0.1 * math.sin(self.t * 3), 0.3, 0))
end

return Wave
"""


async def test_lua_in_play(ed: Editor):
    s = await setup(ed)
    sid = s["scene"]
    lat = await cage(ed, "WaveCage", position=(0, 0, 30))
    await ed.call("create_script", kind="lua", name="LatticeWave", content=WAVE, scene=sid, entity=lat)
    await ed.call("save_scene", scene=sid)
    started = await ed.call("play", action="start", scene=sid)
    ed.check_eq(started["state"], "playing")
    try:
        await ed.call("wait", frames=5)
        a = await el(ed, lattice=lat, detail="points", sample=[[0, 1, 30]])
        ed.check_eq(a["moved_points"], 1, f"the script moved one point: {a}")
        pa = point_offsets(a)[idx([3, 3, 3], 1, 2, 1)]
        check_close(ed, pa[1:], [0.3, 0], "the script's offset, read during play")
        # cubic: the top centre follows its point part way (B-spline), so up but less than 0.3 local
        ed.check(1.1 < a["samples"][0]["deformed"][1] < 1.6, f"sampled during play: {a['samples']}")
        await ed.call("wait", frames=10)
        b = await el(ed, lattice=lat, detail="points")
        pb = point_offsets(b)[idx([3, 3, 3], 1, 2, 1)]
        ed.check(abs(pb[0] - pa[0]) > 1e-4, f"the offsets follow the script frame by frame: {pa} -> {pb}")
        # edits are refused while playing, with the message terrain uses
        await el_error(ed, lattice=lat, ops=[{"op": "reset"}], contains="is playing; stop play mode before editing it.")
        await el_error(ed, lattice=lat, resolution=[4, 4, 4], contains="is playing")
        await el_error(ed, create={}, contains="is playing")
    finally:
        await ed.call("play", action="stop")
    after = await el(ed, lattice=lat)
    ed.check_eq(after["moved_points"], 0, "stopping restores the authored offsets")
    await ed.call("delete_entities", scene=sid, entity=lat)


# -- bake -----------------------------------------------------------------------


async def test_bake(ed: Editor):
    s = await setup(ed)
    sid = s["scene"]
    rock = (await ed.call("create_entity", scene=sid, type="model", name="Rock", model="assets/models/uv_sphere.gltf",
                          position=[0, 0, -30]))["entity_id"]
    made = await el(ed, create={"targets": [rock], "name": "RockCage"},
                    ops=[{"op": "preset", "name": "noise", "amount": 0.15, "seed": 3}])
    lat = made["lattice"]["id"]
    got = await el(ed, lattice=lat, ops=[{"op": "bake"}])
    baked = got["ops"][0]
    ed.check_eq(baked["target"]["id"], rock)
    ed.check(baked["path"].endswith("uv_sphere_lattice.glb"), f"baked path: {baked}")
    files = list(ed.project.rglob("uv_sphere_lattice.glb"))
    ed.check(len(files) == 1 and files[0].stat().st_size > 1000, f"the baked file is written: {files}")
    model = await comp(ed, rock, "model", "filename")
    ed.check(model["filename"].endswith("uv_sphere_lattice.glb"), f"the model switched: {model}")
    ed.check_eq(got["targets"], [], "the baked binding is removed")
    await undo(ed)
    model = await comp(ed, rock, "model", "filename")
    ed.check(model["filename"].endswith("uv_sphere.gltf"), f"undo restores the model: {model}")
    ed.check_eq([t["entity"]["id"] for t in (await el(ed, lattice=lat))["targets"]], [rock], "and the binding")
    ed.check(files[0].exists(), "the file stays")
    # generated meshes cannot be baked: the reason comes through
    box = (await ed.call("create_entity", scene=sid, type="cube", name="BakeBox", position=[3, 0, -30]))["entity_id"]
    await el(ed, lattice=lat, ops=[{"op": "bind", "target": box}])
    await el_error(ed, lattice=lat, ops=[{"op": "bake"}], contains=["ops[0].target", "Rock", "BakeBox"])
    await el_error(ed, lattice=lat, ops=[{"op": "bake", "target": box}], contains="ops[0]")
    await ed.call("delete_entities", scene=sid, entities=[rock, box, lat])


# -- seeing it ------------------------------------------------------------------


async def test_show_all_lattices_screenshot(ed: Editor):
    await setup(ed)
    made = await ed.call("create_scene", name="LatticeShot", type="3D", path="scenes/LatticeShot")
    sid = made["scene"]["id"]
    await ed.call("create_entity", scene=sid, type="directional_light", name="Sun", rotation={"euler": [-50, 30, 0]})
    # a thin rod in a cube cage: a 90 degree bend makes a clean quarter arc (in a non-cubic cage the
    # arc is stretched with the cage, which reads as a lean)
    rod = (await ed.call("create_entity", scene=sid, type="model", name="Rod", model="assets/models/uv_sphere.gltf",
                         position=[0, 1.2, 0], scale=[0.45, 2.4, 0.45]))["entity_id"]
    rod_cage = (await ed.call("create_entity", scene=sid, type="lattice", name="RodCage", position=[0, 1.2, 0],
                              scale=[2.6, 2.6, 2.6]))["entity_id"]
    bent = await ed.call("edit_lattice", scene=sid, lattice=rod_cage, resolution=[3, 8, 3],
                         ops=[{"op": "bind", "target": rod},
                              {"op": "preset", "name": "bend", "axis": "y", "toward": "x", "amount": 90}],
                         sample=[[0, 2.5, 0]])
    ed.check_eq(bent["warnings"], [], f"{bent['warnings']}")
    # the rod's top centre (local y 0.5) ends a quarter circle away: x = R, y = R - 0.5 with R = 2/pi (local)
    check_close(ed, bent["samples"][0]["deformed"], [2.6 * 2 / math.pi, 1.2 + 2.6 * (2 / math.pi - 0.5), 0],
                "the top of the cage's axis ends on the quarter arc (cubic: within 2%)", tol=0.06)
    other = await ed.call("create_entity", scene=sid, type="lattice", name="Spare", position=[-2.4, 0.8, 0], scale=[1, 1.6, 1])
    await ed.call("edit_lattice", scene=sid, lattice=other["entity_id"],
                  ops=[{"op": "preset", "name": "twist", "amount": 40}])
    await ed.call("select_entities", scene=sid, entities=[])
    cam = {"scene": sid, "position": [0.0, 1.6, 7.0], "target": [-0.3, 1.1, 0],
           "display": {"show_grid_3d": False, "hide_light_icons": True, "show_all_lattices": True}}
    got = await ed.call("set_scene_camera", cam)
    ed.check(got["display"]["show_all_lattices"] is True, f"the flag reads back: {got.get('display')}")
    shot = await ed.screenshot("scene", path=ed.scratch.root / "lattice_show_all.png" if ed.scratch else None,
                               scene=sid, max_width=640)
    img = pngdiff.decode(shot)
    await ed.call("set_scene_camera", scene=sid, display={"show_all_lattices": False})
    plain = pngdiff.decode(await ed.screenshot("scene", scene=sid, max_width=640))
    same, fraction, _ = pngdiff.compare(img, plain, max_fraction=0.002)
    ed.check(not same, f"show_all_lattices draws the cages ({fraction:.4f} of the pixels differ)")
    if os.environ.get("JASPER_UPDATE_GOLDEN") == "1" or not GOLDEN.exists():
        GOLDEN.parent.mkdir(parents=True, exist_ok=True)
        GOLDEN.write_bytes(shot)
        print(f"     [golden] wrote {GOLDEN}")
    else:
        golden = pngdiff.decode(GOLDEN.read_bytes())
        ed.check_eq(img[:2], golden[:2], f"the Scene view shot has {GOLDEN.name}'s size (width, height)")
        ok, fraction, diff = pngdiff.compare(golden, img, max_fraction=0.01)
        if not ok and ed.scratch:
            (ed.scratch.root / "lattice_show_all_diff.png").write_bytes(diff)
        ed.check(ok, f"the screenshot differs from {GOLDEN.name} in {fraction:.4f} of the pixels")
    await ed.call("select_scene", scene=S["scene"])


# -- the guide ------------------------------------------------------------------


def schema_words(schema, out: set) -> None:
    if isinstance(schema, dict):
        for key, value in schema.items():
            if key == "properties" and isinstance(value, dict):
                out.update(value.keys())
            if key == "enum" and isinstance(value, list):
                out.update(v for v in value if isinstance(v, str))
            schema_words(value, out)
    elif isinstance(schema, list):
        for item in schema:
            schema_words(item, out)


async def test_guide_names_exist(ed: Editor):
    s = await setup(ed)
    got = await ed.session.read_resource("jasper://docs/lattice")
    text = "".join(getattr(c, "text", "") or "" for c in got.contents)
    ed.check(text.startswith("# Lattices") and len(text) > 4000, f"the guide is served: {text[:80]!r}")
    known: set = set(ed.tool_names)
    for t in ed.tool_list:
        schema_words(t.input_schema, known)
    # op keys from the server itself: an unknown key lists the valid ones
    lat = await cage(ed, "GuideCage", position=(0, 40, 0))
    for op in ("set", "move", "reset", "preset", "mirror", "fit", "bind", "unbind", "follow", "pose", "bake"):
        msg = await el_error(ed, lattice=lat, ops=[{"op": op, "zz_unknown": 1}], contains="Keys:")
        known.update(k.strip() for k in msg.split("Keys:")[1].strip().rstrip(".").split(","))
    msg = await el_error(ed, lattice=lat, ops=[{"op": "reset", "select": {"zz_unknown": 1}}], contains="Keys:")
    known.update(k.strip() for k in msg.split("Keys:")[1].strip().rstrip(".").split(","))
    msg = await el_error(ed, lattice=lat, ops=[{"op": "set", "points": [{"zz_unknown": 1}]}], contains="Keys:")
    known.update(k.strip() for k in msg.split("Keys:")[1].strip().rstrip(".").split(","))
    for key in ("stage", "mask"):
        msg = await el_error(ed, lattice=lat, ops=[{"op": "bind", "target": lat, key: "zz"}], contains="Use one of:")
        known.update(k.strip() for k in msg.split("Use one of:")[1].strip().rstrip(".").split(","))
    camera = await ed.call("set_scene_camera", scene=s["scene"])
    known.update(camera["display"].keys())
    # a follower just outside the cage (follower_outside) and one without a rest (follower_no_rest)
    prop = (await ed.call("create_entity", scene=s["scene"], type="sphere", name="GuideProp", position=[0, 41.2, 0]))["entity_id"]
    lone = (await ed.call("create_entity", scene=s["scene"], type="empty", name="GuideLone", position=[0, 40, 0]))["entity_id"]
    await el(ed, lattice=lat, ops=[{"op": "follow", "target": prop}])
    await ed.call("add_component", scene=s["scene"], entity=lone, component="latticefollower",
                  values={"latticefollower": {"lattice": lat}})
    read = await el(ed, lattice=lat, sample=[[0, 40, 0]], detail="points")
    known.update(w["kind"] for w in read["warnings"])
    ed.check({"follower_outside", "follower_no_rest"} <= known, f"the follower warnings: {read['warnings']}")
    def keys(value):
        if isinstance(value, dict):
            for k, v in value.items():
                known.add(k)
                keys(v)
        elif isinstance(value, list):
            for v in value:
                keys(v)
    keys(read)
    for component in ("lattice", "latticemodifier", "latticefollower", "latticetracks"):
        info = await ed.call("list_component_types", component=component)
        known.update(info["properties"].keys())
        for p in info["properties"].values():
            known.update(o for o in p.get("options", []) if isinstance(o, str))
    known.update(S.get("warning_kinds", set()))
    # every snake_case identifier in the guide is a tool, a parameter, an op key, a property,
    # a value or a result key (Lua names are camelCase and checked by test_stubs / lua-api)
    words = set(re.findall(r"(?<![\w./-])[a-z][a-z0-9]*(?:_[a-z0-9]+)+(?![\w(])", text))
    unknown = sorted(w for w in words if w not in known)
    ed.check(not unknown, f"the guide names unknown things: {unknown}")
    # the backticked op and selector words exist
    for word in ("set", "move", "reset", "preset", "mirror", "fit", "bind", "unbind", "follow", "pose", "bake",
                 "center", "lattice", "selected", "exterior", "interior", "all"):
        ed.check(f"`{word}`" in text or f'"{word}"' in text, f"the guide covers {word}")
    rest = next(f["rest"]["id"] for f in read["followers"] if f["rest"])
    await ed.call("delete_entities", scene=s["scene"], entities=[lat, prop, lone, rest])


async def test_budget(ed: Editor):
    import tool_budget
    rows = {t.name: tool_budget.measure(t, False) for t in ed.tool_list}
    ed.check(rows["edit_lattice"]["total"] <= 2600, f"edit_lattice is {rows['edit_lattice']['total']} chars")
    total = sum(r["total"] for r in rows.values())
    # the total of every tool is test_html_ui_tools.py's test_budget (one owner for the limit)
    print(f"     [budget] edit_lattice {rows['edit_lattice']['total']}, total {total}")
