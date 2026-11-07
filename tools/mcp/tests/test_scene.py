"""Scene and entity MCP contracts, each arranged in its own tiny scene."""

import math

from harness import Editor

EDITOR = {"create": True, "name": "Scene contracts"}
CI = True


async def scene(ed, name):
    return (await ed.call("create_scene", name=name, type="3D", path=f"scenes/{name}"))["scene"]["id"]


async def create(ed, sid, name, kind="empty", **kwargs):
    return (await ed.call("create_entity", scene=sid, type=kind, name=name, **kwargs))["entity_id"]


async def value(ed, sid, entity, component, prop):
    got = await ed.call("get_entity", scene=sid, entity=entity, components=[component], properties=[prop])
    return got["components"][component][prop]


async def ids(ed, sid):
    return {e["id"] for e in (await ed.call("find_entities", scene=sid))["entities"]}


async def test_scene_lifecycle(ed: Editor):
    sid = (await ed.call("create_scene", name="Unsaved", type="2D"))["scene"]["id"]
    await ed.check_error("save_scene", scene=sid, contains="path")
    await ed.call("save_scene", scene=sid, path="scenes/Lifecycle.scene")
    child = await create(ed, sid, "SavedChild")
    first_camera = await create(ed, sid, "FirstCamera", "camera")
    second_camera = await create(ed, sid, "SecondCamera", "camera")
    await ed.call("set_main_camera", scene=sid, entity=first_camera)
    await ed.call("set_main_camera", scene=sid, entity=second_camera)
    await ed.check_error("set_main_camera", scene=sid, entity=child, contains="no Camera")
    await ed.call("undo", scene=sid)
    listed = {item["id"]: item for item in (await ed.call("list_scenes"))["scenes"]}
    ed.check_eq(listed[sid]["main_camera"]["id"], first_camera, "camera selection is one undo step")
    await ed.call("redo", scene=sid)
    await ed.call("set_scene_settings", scene=sid, settings={"background_color": [0, 0, 1, 1]})
    await ed.call("save_scene", scene=sid, path="scenes/SavedAs.scene")
    await ed.call("rename_scene", scene=sid, name="Renamed")
    await ed.call("undo", scene=sid)
    ed.check_eq((await ed.call("get_scene_tree", scene=sid, format="json"))["scene_name"], "Unsaved")
    await ed.call("set_scene_settings", scene=sid, settings={"background_color": [1, 0, 0, 1]})
    await ed.check_error("close_scene", scene=sid)
    await ed.call("close_scene", scene=sid, unsaved="discard")
    opened = (await ed.call("open_scene", scene="scenes/SavedAs.scene"))["scene"]
    reopened = opened["id"]
    ed.check_eq(opened["main_camera"]["id"], second_camera, "main-camera selection survives scene reload")
    settings = await ed.call("get_scene_settings", scene=reopened, names=["background_color"])
    ed.check_eq(settings["settings"]["background_color"], [0, 0, 1, 1], "discard does not save the authored red edit")
    ed.check_eq((await ed.call("get_entity", scene=reopened, entity="SavedChild", detail="summary"))["id"], child)
    await ed.call("delete_scene", scene=reopened, delete_file=True)
    ed.check(not (ed.project / "scenes/SavedAs.scene").exists(), "delete_scene removes the chosen file")
    await ed.call("undo", scope="project")
    ed.check((ed.project / "scenes/SavedAs.scene").is_file(), "project undo restores the saved scene")


async def test_scene_settings(ed: Editor):
    sid = await scene(ed, "Settings")
    schema = (await ed.call("get_scene_settings", scene=sid, detail=True))["schema"]
    # S10: Auto (the platform's tier) and Ultra (PCSS) next to the stored ints of before
    ed.check_eq(schema["shadows_quality"]["options"], ["auto", "none", "low", "medium", "high", "ultra"])
    await ed.check_error("set_scene_settings", scene=sid, settings={"shadows_quality": "extreme"}, contains="medium")
    await ed.call("set_scene_settings", scene=sid, settings={"background_color": [0, 0, 0, 0.4], "ssao_enabled": False})
    await ed.call("set_scene_settings", scene=sid, settings={"background_color": "#336699", "ssao_enabled": True})
    got = (await ed.call("get_scene_settings", scene=sid, names=["background_color", "ssao_enabled"]))["settings"]
    for actual, expected in zip(got["background_color"], [0.2, 0.4, 0.6, 0.4]):
        ed.check(abs(actual - expected) < 1e-5, "hex RGB preserves the previously authored alpha")
    ed.check_eq(got["ssao_enabled"], True)
    await ed.call("undo", scene=sid)
    ed.check_eq((await ed.call("get_scene_settings", scene=sid, names=["ssao_enabled"]))["settings"]["ssao_enabled"], False)
    await ed.check_error("set_scene_settings", scene=sid, settings={"ssao_enabled": True, "ssao_radius": "big"})
    ed.check_eq((await ed.call("get_scene_settings", scene=sid, names=["ssao_enabled"]))["settings"]["ssao_enabled"], False,
                "an invalid field rolls back the complete settings edit")


async def test_entity_refs_and_errors(ed: Editor):
    sid = await scene(ed, "Addressing")
    first = await create(ed, sid, "Twin")
    second = await create(ed, sid, "Other")
    await ed.call("rename_entity", scene=sid, entity=second, name="Twin")
    a = await create(ed, sid, "Target", parent=first)
    b = await create(ed, sid, "OtherTarget", parent=str(second))
    await ed.call("rename_entity", scene=sid, entity=b, name="Target")
    ed.check_eq((await ed.call("get_entity", scene=str(sid), entity=str(a), detail="summary"))["id"], a)
    ed.check_eq((await ed.call("get_entity", scene=sid, entity="Twin#1/Target", detail="summary"))["id"], b)
    error = await ed.check_error("get_entity", scene=sid, entity="Target", contains="matches 2")
    ed.check("Twin/Target" in error and "Twin#1/Target" in error, "ambiguity exposes usable qualified paths")
    found = await ed.call("find_entities", scene=sid, name="tar*", under=first)
    ed.check_eq({e["id"] for e in found["entities"]}, {a}, "glob and ancestry filters intersect")
    await ed.call("select_entities", scene=sid, entities=[str(a), str(b)])
    ed.check_eq({e["id"] for e in (await ed.call("get_selection", scene=sid))["entities"]}, {a, b})
    await ed.call("set_project_settings", values={"tags_and_layers.tags": ["SelectorTarget"]})
    await ed.call("set_entity_state", scene=str(sid), entities=[str(a), str(b)], active=False, tag="SelectorTarget")
    found = await ed.call("find_entities", scene=sid, active=False, tag="SelectorTarget")
    ed.check_eq({e["id"] for e in found["entities"]}, {a, b})
    await ed.call("delete_entities", scene=str(sid), entities=[str(first)])
    await ed.check_error("get_entity", scene=str(sid), entity=str(a), contains="No GameObject")


async def test_create_with_values_one_undo(ed: Editor):
    sid = await scene(ed, "CreateTransaction")
    made = await ed.call("create_entity", scene=sid, type="cube", name="Crate", position=[1, 2, 3],
                         components=["rigidbody"], values={"rigidbody": {"type": "dynamic"}, "mesh": {"castShadows": False}})
    crate = made["entity_id"]
    ed.check_eq(await value(ed, sid, crate, "transform", "position"), [1, 2, 3])
    ed.check_eq(await value(ed, sid, crate, "rigidbody", "type"), "dynamic")
    ed.check_eq(await value(ed, sid, crate, "mesh", "castShadows"), False)
    await ed.call("undo", scene=sid)
    ed.check(crate not in await ids(ed, sid), "one undo removes the entire composed creation")
    await ed.call("redo", scene=sid)
    ed.check_eq(await value(ed, sid, crate, "rigidbody", "type"), "dynamic")
    before = await ids(ed, sid)
    await ed.check_error("create_entity", scene=sid, type="cube", name="Rejected", components=["rigidbody"],
                         values={"rigidbody": {"type": "flying"}}, contains="dynamic")
    ed.check_eq(await ids(ed, sid), before, "failed initialization leaves no partial entity")


async def test_move_entity(ed: Editor):
    sid = await scene(ed, "Reparent")
    parent = await create(ed, sid, "Parent", position=[10, 0, 0], scale=[2, 2, 2])
    child = await create(ed, sid, "Child", position=[12, 4, 0])
    await ed.call("move_entity", scene=sid, entity=child, parent=parent)
    ed.check_eq(await value(ed, sid, child, "transform", "position"), [1, 2, 0], "reparenting preserves world position")
    await ed.check_error("move_entity", scene=sid, entity=parent, parent=child, contains="own child")
    ed.check_eq((await ed.call("get_entity", scene=sid, entity=parent, detail="summary"))["parent"], None,
                "cycle rejection leaves the hierarchy intact")
    sibling = await create(ed, sid, "Sibling", parent=parent)
    await ed.call("move_entity", scene=sid, entity=child, after=sibling)
    tree = await ed.call("get_scene_tree", scene=sid, root=parent, format="json")
    ed.check_eq([item["id"] for item in tree["nodes"][0]["children"]], [sibling, child], "after controls sibling order")
    await ed.call("move_entity", scene=sid, entity=child, before=sibling)
    await ed.call("undo", scene=sid)
    tree = await ed.call("get_scene_tree", scene=sid, root=parent, format="json")
    ed.check_eq([item["id"] for item in tree["nodes"][0]["children"]], [sibling, child], "sibling reorder is undoable")
    await ed.call("set_transform", scene=sid, entity=child, position=[14, 4, 0], space="world")
    ed.check_eq(await value(ed, sid, child, "transform", "position"), [2, 2, 0])
    await ed.call("set_transform", scene=sid, entity=child, position=[1, 0, 0], relative=True)
    ed.check_eq(await value(ed, sid, child, "transform", "position"), [3, 2, 0])
    await ed.call("undo", scene=sid)
    ed.check_eq(await value(ed, sid, child, "transform", "position"), [2, 2, 0])
    await ed.call("move_entity", scene=sid, entity=child, parent=None, keep_world_transform=False)
    ed.check_eq(await value(ed, sid, child, "transform", "position"), [2, 2, 0], "explicit local mode preserves local position")


async def test_property_types_roundtrip(ed: Editor):
    sid = await scene(ed, "ValueEncoding")
    image = await create(ed, sid, "Image", "image")
    cube = await create(ed, sid, "Cube", "cube")
    camera = await create(ed, sid, "RenderTarget", "camera")
    light = await create(ed, sid, "Light", "point_light")
    await ed.call("set_properties", scene=sid, entity=light, values={"light": {"type": "Spot"}})
    full = await ed.call("get_entity", scene=sid, entity=light, components=["light"], properties=["type"], detail="full")
    ed.check_eq(full["components"]["light"]["type"]["value"], "spot", "full detail exposes the current canonical enum value")
    await ed.check_error("set_properties", scene=sid, entity=light, values={"light": {"type": 7}}, contains="not one of")
    ed.check_eq(await value(ed, sid, light, "light", "type"), "spot", "invalid enum leaves the value intact")
    label = await create(ed, sid, "Label", "text")
    button = await create(ed, sid, "Button", "button")
    await ed.call("set_properties", scene=sid, entity=image, values={"ui": {"color": "#00ff0080"}})
    await ed.call("set_properties", scene=sid, entity=image, values={"ui": {"color": [1, 0, 0]}})
    colour = await value(ed, sid, image, "ui", "color")
    ed.check_eq(colour[:3], [1, 0, 0])
    ed.check(abs(colour[3] - 128 / 255) < 1e-5, "RGB writes preserve alpha")
    await ed.call("set_properties", scene=sid, entity=cube, values={"transform": {"rotation": {"euler": [0, 0, 90]}}})
    rotation = await value(ed, sid, cube, "transform", "rotation")
    for actual, expected in zip(rotation["quat"], [0, 0, math.sqrt(0.5), math.sqrt(0.5)]):
        ed.check(abs(actual - expected) < 1e-5, "Euler input produces the public XYZW quaternion convention")
    await ed.call("set_properties", scene=sid, entity=image, values={"ui": {"texture": {"camera": camera}}})
    ed.check_eq((await value(ed, sid, image, "ui", "texture"))["camera"]["id"], camera)
    ed.check_eq(await value(ed, sid, camera, "camera", "renderToTexture"), True)
    await ed.call("set_properties", scene=sid, entity=button, values={"button": {"label": {"id": label}}})
    ed.check_eq((await value(ed, sid, button, "button", "label"))["id"], label)
    await ed.call("set_properties", scene=sid, entity=button, values={"button": {"label": None}})
    ed.check_eq(await value(ed, sid, button, "button", "label"), None)


async def test_custom_roundtrip(ed: Editor):
    sid = await scene(ed, "CustomValues")
    body = await create(ed, sid, "Body", "rigidbody2d")
    await ed.call("set_properties", scene=sid, entity=body,
                  changes=[{"component": "Rigidbody 2D", "property": "colliders", "insert": {"type": "circle", "radius": 25}}])
    await ed.call("set_properties", scene=sid, entity=body,
                  changes=[{"component": "rigidbody2d", "property": "colliders", "insert": {"type": "circle", "radius": 7}}])
    await ed.call("set_properties", scene=sid, entity=body,
                  changes=[{"component": "rigidbody2d", "property": "colliders", "remove": 0}])
    ed.check_eq(await value(ed, sid, body, "rigidbody2d", "colliders[0].radius"), 7)
    await ed.call("undo", scene=sid)
    ed.check_eq(await value(ed, sid, body, "rigidbody2d", "colliders[0].radius"), 25)
    await ed.check_error("set_properties", scene=sid, entity=body,
                         changes=[{"component": "rigidbody2d", "property": "colliders", "remove": 9}], contains="out of range")
    ed.check_eq(await value(ed, sid, body, "rigidbody2d", "colliders[0].radius"), 25)
    cube = await create(ed, sid, "Uniforms", "cube")
    await ed.call("set_properties", scene=sid, entity=cube, values={"mesh": {"shaderUniforms": {"tint": [1, 0.5, 0, 1], "power": 2}}})
    ed.check_eq(await value(ed, sid, cube, "mesh", "shaderUniforms"), {"tint": [1, 0.5, 0, 1], "power": [2, 0, 0, 0]})


async def test_set_properties_atomic(ed: Editor):
    sid = await scene(ed, "EditTransaction")
    a = await create(ed, sid, "A", position=[1, 0, 0])
    b = await create(ed, sid, "B", position=[2, 0, 0])
    await ed.call("set_properties", scene=sid, entities=[a, b], values={"transform": {"position": [7, 7, 7], "scale": [3, 3, 3]}})
    for entity in [a, b]:
        ed.check_eq(await value(ed, sid, entity, "transform", "scale"), [3, 3, 3])
    await ed.call("undo", scene=sid)
    ed.check_eq(await value(ed, sid, a, "transform", "position"), [1, 0, 0])
    ed.check_eq(await value(ed, sid, b, "transform", "position"), [2, 0, 0], "one undo restores every target")
    await ed.check_error("set_properties", scene=sid, entities=[a, b], changes=[
        {"component": "transform", "property": "position", "value": [9, 9, 9]},
        {"component": "transform", "property": "scale", "value": "not a vector"}])
    ed.check_eq(await value(ed, sid, a, "transform", "position"), [1, 0, 0])
    ed.check_eq(await value(ed, sid, b, "transform", "position"), [2, 0, 0], "rejection leaves all targets unchanged")


async def test_components(ed: Editor):
    sid = await scene(ed, "ComponentClipboard")
    source = await create(ed, sid, "Source", "rigidbody")
    target = await create(ed, sid, "Target", "rigidbody")
    empty = await create(ed, sid, "Empty")
    await ed.call("set_properties", scene=sid, entity=source, values={"rigidbody": {"type": "dynamic"}})
    await ed.call("set_properties", scene=sid, entity=target, values={"rigidbody": {"type": "kinematic"}})
    await ed.call("copy_component", scene=sid, entity=source, component="rigidbody")
    await ed.call("paste_component", scene=sid, entity=target)
    ed.check_eq(await value(ed, sid, target, "rigidbody", "type"), "dynamic")
    await ed.call("undo", scene=sid)
    ed.check_eq(await value(ed, sid, target, "rigidbody", "type"), "kinematic")
    await ed.check_error("paste_component", scene=sid, entity=empty, contains="as_new")
    await ed.call("paste_component", scene=sid, entity=empty, as_new=True)
    ed.check_eq(await value(ed, sid, empty, "rigidbody", "type"), "dynamic")


async def test_model_and_material_link(ed: Editor):
    sid = await scene(ed, "MaterialIsolation")
    path = ed.project / "assets/materials/Linked.material"
    path.parent.mkdir(parents=True, exist_ok=True)
    authored = "baseColorFactor: [1, 0, 0, 1]\nroughnessFactor: 0.5\n"
    path.write_text(authored)
    first = await create(ed, sid, "First", "cube")
    second = await create(ed, sid, "Second", "cube")
    await ed.call("set_properties", scene=sid, entities=[first, second],
                  values={"mesh": {"submeshes[0].material": "assets/materials/Linked.material"}})
    await ed.call("set_properties", scene=sid, entity=first,
                  values={"mesh": {"submeshes[0].material": {"roughnessFactor": 0.9}}})
    local = await value(ed, sid, first, "mesh", "submeshes[0].material")
    other = await value(ed, sid, second, "mesh", "submeshes[0].material")
    ed.check("file" not in local and abs(local["roughnessFactor"] - 0.9) < 1e-5, "inline edit makes an independent material")
    ed.check_eq(other["file"], "assets/materials/Linked.material")
    ed.check_eq(other["roughnessFactor"], 0.5)
    # (the asset database may have given the file its S04 identity, a guid line, meanwhile)
    content = "".join(line for line in path.read_text().splitlines(keepends=True) if not line.startswith(("guid:", "formerGuids:")))
    ed.check_eq(content, authored, "editing an instance does not rewrite its linked asset")
    await ed.call("undo", scene=sid)
    ed.check_eq((await value(ed, sid, first, "mesh", "submeshes[0].material"))["file"], "assets/materials/Linked.material")


async def test_set_mesh_geometry(ed: Editor):
    sid = await scene(ed, "GeneratedGeometry")
    shape = await create(ed, sid, "Shape", "cube")
    sphere = await ed.call("set_mesh_geometry", scene=sid, entity=shape, shape="sphere", radius=2)
    ed.check_eq(sphere["local_size"], [4, 4, 4], "radius controls the generated mesh's actual bounds")
    box = await ed.call("set_mesh_geometry", scene=sid, entity=shape, shape="box", width=3, height=1, depth=2)
    ed.check_eq(box["local_size"], [3, 1, 2])
    await ed.call("undo", scene=sid)
    restored = await ed.call("eval_lua", scene=sid, allow_edit_mode=True,
                             code=f"return Mesh(scene, {shape}):getAABB():getSize()")
    ed.check_eq(restored["values"], [[4, 4, 4]], "undo restores geometry, not merely an entity handle")
    sprite = await create(ed, sid, "GeneratedSprite", "sprite")
    await ed.check_error("set_mesh_geometry", scene=sid, entity=sprite, shape="box", contains="generated")


async def test_rotations_are_unit_quaternions(ed: Editor):
    """agent-authoring: red before the fix, [90, 0, 0, 1] (Euler degrees typed as a quaternion) was
    stored as given and scaled the cube; near-unit quaternions were stored unnormalized"""
    sid = await scene(ed, "Rotations")
    cube = await create(ed, sid, "Cube", "cube")
    await ed.check_error("set_properties", scene=sid, entity=cube, values={"transform": {"rotation": [90, 0, 0, 1]}},
                         contains="euler")
    await ed.check_error("set_properties", scene=sid, entity=cube, values={"transform": {"rotation": {"x": 90, "y": 0, "z": 0}}},
                         contains="x, y, z and w")
    await ed.check_error("set_transform", scene=sid, entity=cube, rotation=[0, 180, 0, 1], contains="euler")
    rotation = await value(ed, sid, cube, "transform", "rotation")
    ed.check(all(abs(a - b) < 1e-6 for a, b in zip(rotation["quat"], [0, 0, 0, 1])), f"refusals change nothing: {rotation}")
    await ed.call("set_properties", scene=sid, entity=cube, values={"transform": {"rotation": [0, 0, 0.71, 0.71]}})
    quat = (await value(ed, sid, cube, "transform", "rotation"))["quat"]
    ed.check(abs(math.sqrt(sum(c * c for c in quat)) - 1) < 1e-5, f"stored normalized: {quat}")
    ed.check(abs(quat[2] - math.sqrt(0.5)) < 1e-4, f"the same rotation: {quat}")
