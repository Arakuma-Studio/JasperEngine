# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""Model import over MCP (spec S94, P7: docs/specs/S94-rendering-anim-gaps.md F39-F47).

- F44/F45: a new project places models with Preserve Hierarchy (every glTF node its own GameObject);
  get_model_info lists what draws each mesh node (drawn_by); a material or mesh value sent to a node
  whose mesh the model's root draws fails naming the root and preserveHierarchy; a write of
  model.preserveHierarchy reloads the model in the same undo step.
- F47: create_entity applies animationEntities before the load (no per-clip GameObjects are made),
  set_properties reloads in one undo step, and in Auto an Animator suppresses them.
- F40: get_model_info reports the root translations a take starts away from (first_frame_differs)
  with a next naming rest_pose; set_model_import_settings rest_pose: first_frame imports that pose.
- F39: a clip-only FBX (a skeleton and a take, Unity's Model@Clip.fbx) imports, and an Animation
  Player on the rigged model plays its take.

The models are variants of editor/tests/fixtures/rigged_cube.fbx written here.
"""

from __future__ import annotations

import asyncio
import re
import time

import harness
from harness import Editor, REPO

CI = True
TIMEOUT = 600
EDITOR = {"create": True, "name": "Model Import"}

FIXTURE = REPO / "editor" / "tests" / "fixtures" / "rigged_cube.fbx"
BOX = REPO / "tools" / "fixtures" / "shader-library-export" / "assets" / "models" / "box.gltf"
S: dict = {}


def without_block(text: str, header: str) -> str:
    start = text.index("\t" + header)
    end = text.index("\n\t}\n", start) + 4
    return text[:start] + text[end:]


def without_connections(text: str, ids: set[str]) -> str:
    kept = []
    for line in text.split("\n"):
        m = re.match(r'\s*C: "O[OP]",(\d+),(\d+)', line)
        if m and (m.group(1) in ids or m.group(2) in ids):
            continue
        kept.append(line)
    return "\n".join(kept)


def without_take(text: str) -> str:
    for block in ("AnimationStack: 800,", "AnimationLayer: 801,", "AnimationCurveNode: 802,", "AnimationCurve: 803,",
                  "AnimationCurveNode: 804,", "AnimationCurve: 805,"):
        text = without_block(text, block)
    return without_connections(text, {"800", "801", "802", "803", "804", "805"})


def with_objects(text: str, objects: str, connections: str) -> str:
    return text.replace("\n}\nConnections:  {\n", "\n" + objects + "}\nConnections:  {\n" + connections, 1)


ROOT_TAKE = (
    '\tAnimationCurveNode: 806, "AnimCurveNode::T", "" {\n\t\tProperties70:  {\n'
    '\t\t\tP: "d|X", "Number", "", "A",0\n\t\t\tP: "d|Y", "Number", "", "A",-1\n\t\t\tP: "d|Z", "Number", "", "A",0\n'
    '\t\t}\n\t}\n'
    '\tAnimationCurve: 807, "AnimCurve::", "" {\n\t\tDefault: 0\n\t\tKeyVer: 4009\n'
    '\t\tKeyTime: *2 {\n\t\t\ta: 0,46186158000\n\t\t}\n\t\tKeyValueFloat: *2 {\n\t\t\ta: -3,-3\n\t\t}\n'
    '\t\tKeyAttrFlags: *1 {\n\t\t\ta: 4\n\t\t}\n\t\tKeyAttrDataFloat: *4 {\n\t\t\ta: 0,0,0,0\n\t\t}\n'
    '\t\tKeyAttrRefCount: *1 {\n\t\t\ta: 2\n\t\t}\n\t}\n')
ROOT_TAKE_CONNECTIONS = '\tC: "OO",806,801\n\tC: "OP",806,400, "Lcl Translation"\n\tC: "OP",807,806, "d|Y"\n'


async def wait_until(ed: Editor, what: str, probe, timeout: float = 60.0):
    deadline = time.monotonic() + timeout
    while True:
        value = await probe()
        if value:
            return value
        if time.monotonic() > deadline:
            ed.fail(f"timed out waiting for {what}")
        await asyncio.sleep(0.5)


async def model_info(ed: Editor, entity) -> dict:
    async def loaded():
        info = await ed.call("get_model_info", entity=entity)
        return info if not info["loading"] else None
    return await wait_until(ed, "the model to load", loaded)


async def test_setup(ed: Editor):
    ed.require_tools("import_files", "create_entity", "get_model_info", "set_properties", "set_model_import_settings")
    incoming = ed.scratch.root / "incoming"
    incoming.mkdir(parents=True, exist_ok=True)
    text = FIXTURE.read_text()
    files = {
        "rigged.fbx": text,
        # one skinned mesh and no take: the root draws it; the Cube node is the bones' parent, so a joint
        "shark.fbx": without_take(text).replace('\tC: "OO",400,0\n', '\tC: "OO",400,200\n'),
        "offset.fbx": with_objects(text, ROOT_TAKE, ROOT_TAKE_CONNECTIONS),
    }
    clip = text
    for block in ("Geometry: 100,", "Geometry: 702,", "Model: 200,", "Material: 300,", "Texture: 310,", "Deformer: 600,",
                  "Deformer: 601,", "Deformer: 602,", "Deformer: 700,", "Deformer: 701,", "AnimationCurveNode: 804,",
                  "AnimationCurve: 805,"):
        clip = without_block(clip, block)
    files["rigged@Turn.fbx"] = without_connections(clip, {"100", "702", "200", "300", "310", "600", "601", "602", "700", "701",
                                                          "804", "805"})
    for name, body in files.items():
        (incoming / name).write_text(body)
    (incoming / "box.gltf").write_bytes(BOX.read_bytes())
    r = await ed.call("import_files", sources=[str(incoming / name) for name in [*files, "box.gltf"]], to_dir="assets/models")
    ed.check("fbx_import_failed" not in r, f"every FBX imports: {r!r}")
    ed.check_eq(sorted(r.get("fbx_imports", [])), sorted(f"assets/models/{name}" for name in files), f"{r!r}")


async def test_new_project_preserves_hierarchy(ed: Editor):
    key = "mesh.preserve_hierarchy_for_new_models"
    got = await ed.call("get_project_settings", keys=[key])
    ed.check_eq(got["settings"][key], True, "a new project's Preserve Hierarchy for New Models")
    box = (await ed.call("create_entity", type="model", name="Box", model="assets/models/box.gltf"))["entity_id"]
    info = await model_info(ed, box)
    ed.check_eq(info["preserve_hierarchy"], True, f"{info!r}")
    ed.check(info["mesh_nodes"], f"the file's mesh nodes are listed: {info!r}")
    drawn = harness.find_key(info["mesh_nodes"][0]["drawn_by"], "id", "entity_id")
    ed.check(drawn != box, f"a child GameObject draws the lone mesh: {info['mesh_nodes']!r}")
    ed.check_eq(harness.find_key(info["mesh_nodes"][0]["entity"], "id", "entity_id"), drawn, "the node's own GameObject")

    # off: the loader bakes it into the Model's own GameObject, in one undo step with the reload
    await ed.call("set_properties", entity=box, values={"model": {"preserveHierarchy": False}})
    info = await model_info(ed, box)
    ed.check_eq(harness.find_key(info["mesh_nodes"][0]["drawn_by"], "id", "entity_id"), box, f"{info['mesh_nodes']!r}")
    await ed.call("undo")
    info = await model_info(ed, box)
    ed.check_eq(info["preserve_hierarchy"], True, "one undo restores the value")
    ed.check(harness.find_key(info["mesh_nodes"][0]["drawn_by"], "id", "entity_id") != box, "... and the layout")


async def test_node_material_error_names_root(ed: Editor):
    shark = (await ed.call("create_entity", type="model", name="Shark", model="assets/models/shark.fbx",
                           values={"model": {"preserveHierarchy": False}}))["entity_id"]
    info = await model_info(ed, shark)
    nodes = info["mesh_nodes"]
    ed.check_eq(len(nodes), 1, f"{info!r}")
    ed.check_eq(harness.find_key(nodes[0]["drawn_by"], "id", "entity_id"), shark, f"the root draws the skinned mesh: {nodes!r}")
    node = harness.find_key(nodes[0]["entity"], "id", "entity_id")
    ed.check(node is not None, f"the mesh node has a GameObject (a joint): {nodes!r}")
    msg = await ed.check_error("set_properties", entity=node, values={"mesh": {"castShadows": False}}, contains="preserveHierarchy")
    ed.check_in("Shark", msg, "the error names the root")

    await ed.call("set_properties", entity=shark, values={"model": {"preserveHierarchy": True}})
    info = await model_info(ed, shark)
    drawn = harness.find_key(info["mesh_nodes"][0]["drawn_by"], "id", "entity_id")
    ed.check(drawn != shark, f"each node draws its own mesh: {info['mesh_nodes']!r}")
    await ed.call("set_properties", entity=drawn, values={"mesh": {"castShadows": False}})


async def test_create_entity_animation_entities_false(ed: Editor):
    rig = (await ed.call("create_entity", type="model", name="Quiet", model="assets/models/rigged.fbx",
                         values={"model": {"animationEntities": False}}))["entity_id"]
    info = await model_info(ed, rig)
    ed.check_eq(info["animations"], [], f"no per-clip GameObject was ever made: {info!r}")
    found = harness.find_entities(await ed.call("find_entities", component="animation"))
    ed.check_eq(found, [], "no Animation GameObject exists in the scene (none made, then removed)")
    # a later write reloads the model in the same undo step
    await ed.call("set_properties", entity=rig, values={"model": {"animationEntities": True}})
    info = await model_info(ed, rig)
    ed.check_eq(len(info["animations"]), 1, f"the reload made the take's: {info!r}")
    await ed.call("undo")
    info = await model_info(ed, rig)
    ed.check_eq(info["animations"], [], "one undo")
    # Auto (a new project's): an Animator on the model plays its clips, so none are made
    driven = (await ed.call("create_entity", type="model", name="Driven", model="assets/models/rigged.fbx",
                            components=["animator"]))["entity_id"]
    info = await model_info(ed, driven)
    ed.check_eq(info["animations"], [], f"an Animator suppresses them: {info!r}")
    plain = (await ed.call("create_entity", type="model", name="Plain", model="assets/models/rigged.fbx"))["entity_id"]
    ed.check_eq(len((await model_info(ed, plain))["animations"]), 1, "one clip and no Animator: as before")


async def test_rest_pose_first_frame(ed: Editor):
    offset = (await ed.call("create_entity", type="model", name="Offset", model="assets/models/offset.fbx"))["entity_id"]
    info = await model_info(ed, offset)
    differs = info.get("first_frame_differs") or []
    ed.check([d for d in differs if d["node"] == "Root"], f"the take starts with Root away from its node: {info!r}")
    ed.check_in("rest_pose", info.get("next", ""), "next names the setting")

    await ed.call("set_model_import_settings", paths=["assets/models/offset.fbx"], settings={"rest_pose": "first_frame"})

    async def reposed():
        got = await ed.call("get_model_info", entity=offset)
        return got if not got["loading"] and not got.get("first_frame_differs") else None
    info = await wait_until(ed, "the model to reload in the first frame's pose", reposed)
    ed.check("next" not in info, f"nothing left to suggest: {info!r}")
    root = next(b for b in info["bones"] if b["name"] == "Root")
    position = harness.find_key(await ed.call("get_entity", entity=root["id"], components=["transform"]), "position")
    ed.check(abs(position[1] + 3.0) < 1e-3, f"Root stands where the take starts: {position!r}")


async def test_clip_only_fbx_plays_on_the_rig(ed: Editor):
    sampled = await ed.call("get_animation", clip="assets/models/rigged@Turn.fbx#Take 001", times=[0, 1])
    paths = {t["path"] for t in sampled["samples"]["tracks"]}
    ed.check(any(p.endswith("Bone") for p in paths), f"the take animates Bone: {paths!r}")

    rig = (await ed.call("create_entity", type="model", name="Turner", model="assets/models/rigged.fbx"))["entity_id"]
    await model_info(ed, rig)
    await ed.call("add_component", entity=rig, component="animationplayer",
                  values={"animationplayer": {"clips": [{"clip": "assets/models/rigged@Turn.fbx#Take 001"}]}})
    players = (await ed.call("get_animation", entity=rig))["players"]
    ed.check(players and players[0]["bindings"][0]["resolved"] >= 1, f"the clip binds to the rig: {players!r}")
    r = await ed.call_raw("preview_animation", {"animation": rig, "action": "seek", "time": 1.0})
    ed.check(not r.is_error, r.text)
    bone = next((b for b in r.structured["bound"] if b["path"].endswith("Bone")), None)
    ed.check(bone is not None, f"Bone is bound: {r.structured['bound']!r}")
    await ed.call_raw("preview_animation", {"animation": rig, "action": "stop"})
