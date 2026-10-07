"""W1 tests: scenes, hierarchy, GameObjects, components, every property type (docs/mcp/DESIGN.md).

Runs against a scratch copy of the TappyPlane fixture (a 2D "Main Scene" and a UI "HUD Scene"),
plus one empty new project (test_empty_project_flow) that builds a small 3D scene from scratch,
saves it, and reopens it in a second editor to check what was saved.
"""

import math
import shutil

from harness import Editor

TIMEOUT = 900

LAB = {}  # the 3D scene most tests work in: {"id": ...}


# -- helpers --------------------------------------------------------------------


def close(a, b, tol=1e-3):
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        return abs(a - b) <= tol
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(close(x, y, tol) for x, y in zip(a, b))
    if isinstance(a, dict) and isinstance(b, dict):
        return a.keys() == b.keys() and all(close(a[k], b[k], tol) for k in a)
    return a == b


def check_close(ed, actual, expected, what=""):
    ed.check(close(actual, expected), f"{what}: expected {expected!r}, got {actual!r}")


async def lab(ed):
    """Creates (once) and selects the 3D Lab scene; returns its id."""
    if "id" not in LAB:
        made = await ed.call("create_scene", name="Lab", type="3D", path="scenes/Lab")
        LAB["id"] = made["scene"]["id"]
    await ed.call("select_scene", scene=LAB["id"])
    return LAB["id"]


async def create(ed, type_="empty", **args):
    made = await ed.call("create_entity", type=type_, scene=LAB.get("id"), **args)
    return made["entity_id"]


async def values(ed, entity, component, *names, scene=None):
    """{property: value} of one component (only `names` when given)."""
    args = {"entity": entity, "components": [component], "scene": scene or LAB.get("id")}
    if names:
        args["properties"] = list(names)
    got = await ed.call("get_entity", **args)
    return got["components"][component]


async def value(ed, entity, component, name, scene=None):
    return (await values(ed, entity, component, name, scene=scene))[name]


async def set_props(ed, entity, vals, scene=None):
    return await ed.call("set_properties", entity=entity, values=vals, scene=scene or LAB.get("id"))


async def undo(ed, scene=None):
    await ed.call("undo", scene=scene or LAB.get("id"))


async def redo(ed, scene=None):
    await ed.call("redo", scene=scene or LAB.get("id"))


async def tree_names(ed, scene=None):
    got = await ed.call("get_scene_tree", scene=scene or LAB.get("id"), format="json")
    names = []

    def walk(nodes):
        for n in nodes:
            names.append(n["name"])
            walk(n.get("children", []))
    walk(got["nodes"])
    return names


async def exists(ed, entity_id, scene=None):
    got = await ed.call("find_entities", scene=scene or LAB.get("id"), limit=100000)
    return any(e["id"] == entity_id for e in got["entities"])


# -- tool list --------------------------------------------------------------------


async def test_tool_list(ed: Editor):
    names = {t.name for t in await ed.tools()}
    mine = {"list_scenes", "create_scene", "open_scene", "close_scene", "select_scene", "save_scene", "rename_scene",
            "delete_scene", "set_child_scene", "set_main_camera", "get_scene_settings", "set_scene_settings",
            "get_scene_tree", "find_entities", "create_entity", "delete_entities", "duplicate_entities", "rename_entity",
            "move_entity", "set_entity_state", "get_selection", "select_entities", "copy_entities", "paste_entities",
            "get_entity", "set_properties", "set_transform", "add_component", "remove_component", "reset_component",
            "copy_component", "paste_component", "list_component_types", "set_mesh_geometry"}
    missing = mine - names
    ed.check(not missing, f"missing tools: {sorted(missing)}")
    replaced = {"list_scene_entities", "inspect_entity", "inspect_scene", "inspect_component", "set_entity_transform",
                "delete_entity", "duplicate_entity", "reparent_entity", "move_entity_order", "set_component_property",
                "set_component_properties", "add_project_scene", "save_all_scenes", "set_scene_property",
                "add_child_scene", "remove_child_scene", "set_child_scene_start_active", "load_child_scene_inline",
                "get_component", "set_property", "regenerate_mesh_geometry", "import_project_model"}
    ed.check(not (replaced & names), f"legacy tools still listed: {sorted(replaced & names)}")


# -- scenes -----------------------------------------------------------------------


async def test_list_scenes(ed: Editor):
    got = await ed.call("list_scenes")
    scenes = {s["name"]: s for s in got["scenes"]}
    ed.check_in("Main Scene", scenes)
    ed.check_in("HUD Scene", scenes)
    main = scenes["Main Scene"]
    ed.check_eq(main["type"], "2D", "Main Scene type")
    ed.check(main["selected"] and main["opened"] and main["start_scene"], f"Main Scene flags: {main}")
    ed.check_eq(scenes["HUD Scene"]["type"], "UI")
    ed.check_eq(main["path"], "Main Scene.scene")


async def test_scene_lifecycle(ed: Editor):
    sid = await lab(ed)
    scenes = {s["id"]: s for s in (await ed.call("list_scenes"))["scenes"]}
    ed.check_eq(scenes[sid]["path"], "scenes/Lab.scene")
    ed.check_eq(scenes[sid]["type"], "3D")
    ed.check((ed.project / "scenes/Lab.scene").exists(), "Lab.scene was not written")

    # untitled scene: save needs a path; then Save As moves it
    made = await ed.call("create_scene", name="Tmp", type="2D")
    tmp = made["scene"]["id"]
    ed.check_eq(made["scene"]["path"], "", "untitled path")
    await ed.check_error("save_scene", scene=tmp, contains="path")
    saved = await ed.call("save_scene", scene=tmp, path="scenes/Tmp")
    ed.check_eq(saved["scene"]["path"], "scenes/Tmp.scene")
    await ed.call("create_entity", scene=tmp, type="sprite", name="S1")
    as_ = await ed.call("save_scene", scene=tmp, path="scenes/Tmp2.scene")
    ed.check_eq(as_["scene"]["path"], "scenes/Tmp2.scene", "save as path")
    ed.check((ed.project / "scenes/Tmp2.scene").exists(), "Save As did not write the new file")
    ed.check("S1" in (ed.project / "scenes/Tmp2.scene").read_text(), "Save As file lacks the new GameObject")

    # rename (undoable), unique names
    await ed.call("rename_scene", scene=tmp, name="Tmp Renamed")
    await ed.check_error("rename_scene", scene=tmp, name="Main Scene", contains="unique")
    await ed.call("undo", scene=tmp)
    ed.check_eq({s["id"]: s for s in (await ed.call("list_scenes"))["scenes"]}[tmp]["name"], "Tmp", "rename undone")

    # close (unsaved: cancel fails), open by path, select
    await ed.call("set_scene_settings", scene=tmp, settings={"background_color": [1, 0, 0, 1]})
    await ed.check_error("close_scene", scene=tmp)
    await ed.call("close_scene", scene=tmp, unsaved="discard")
    listed = {s["id"]: s for s in (await ed.call("list_scenes"))["scenes"]}
    ed.check(tmp in listed and not listed[tmp]["opened"], "closed scene should stay listed, closed")
    opened = await ed.call("open_scene", scene="scenes/Tmp2.scene")
    ed.check_eq(opened["scene"]["id"], tmp, "reopened scene id")
    ed.check(opened["scene"]["selected"], "open_scene selects")
    await ed.call("select_scene", scene=sid)
    await ed.check_error("select_scene", scene="no such scene", contains="Scenes:")

    # save all
    await ed.call("create_entity", scene=tmp, type="sprite", name="S2")
    out = await ed.call("save_scene", all=True)
    ed.check(any(s["id"] == tmp for s in out["saved"]), f"save all should save Tmp: {out}")

    # delete from the project (file kept), undo, then delete with the file
    await ed.call("delete_scene", scene=tmp)
    ed.check(tmp not in {s["id"] for s in (await ed.call("list_scenes"))["scenes"]}, "scene not removed")
    ed.check((ed.project / "scenes/Tmp2.scene").exists(), "file should be kept")
    await ed.call("undo", scope="project")
    back = {s["name"]: s for s in (await ed.call("list_scenes"))["scenes"]}
    ed.check_in("Tmp", back, "scene back after undo")
    tmp = back["Tmp"]["id"]
    await ed.call("delete_scene", scene=tmp, delete_file=True)
    ed.check(not (ed.project / "scenes/Tmp2.scene").exists(), "file should be deleted")
    await ed.call("undo", scope="project")
    ed.check((ed.project / "scenes/Tmp2.scene").exists(), "file back after undo")
    await ed.call("delete_scene", scene="scenes/Tmp2.scene", delete_file=True)
    await ed.call("select_scene", scene=sid)


async def test_child_scenes_and_main_camera(ed: Editor):
    sid = await lab(ed)
    hud = "HUD Scene"
    out = await ed.call("set_child_scene", scene=sid, child=hud, start_active=False)
    child = out["scene"]["child_scenes"][0]
    ed.check_eq(child["name"], "HUD Scene")
    ed.check_eq(child["start_active"], False)
    out = await ed.call("set_child_scene", scene=sid, child=hud, start_active=True)
    ed.check_eq(out["scene"]["child_scenes"][0]["start_active"], True)
    await undo(ed)
    lst = {s["id"]: s for s in (await ed.call("list_scenes"))["scenes"]}
    ed.check_eq(lst[sid]["child_scenes"][0]["start_active"], False, "start_active undone")
    await ed.check_error("set_child_scene", scene=sid, child=sid, contains="own child")
    out = await ed.call("set_child_scene", scene=sid, child=hud, attached=False)
    ed.check(not out["scene"].get("child_scenes"), "detached")
    await undo(ed)
    lst = {s["id"]: s for s in (await ed.call("list_scenes"))["scenes"]}
    ed.check(lst[sid].get("child_scenes"), "detach undone")
    await ed.call("set_child_scene", scene=sid, child=hud, attached=False)

    cam = await create(ed, "camera", name="MainCam")
    cam2 = await create(ed, "camera", name="OtherCam")
    out = await ed.call("set_main_camera", entity=cam2)
    ed.check_eq(out["main_camera"]["id"], cam2)
    await undo(ed)
    lst = {s["id"]: s for s in (await ed.call("list_scenes"))["scenes"]}
    ed.check_eq(lst[sid]["main_camera"]["id"], cam, "main camera undone")
    cube = await create(ed, "cube", name="NotACam")
    await ed.check_error("set_main_camera", entity=cube, contains="no Camera")
    await ed.call("delete_entities", entities=[cam2, cube])


async def test_scene_settings(ed: Editor):
    await lab(ed)
    got = await ed.call("get_scene_settings", detail=True)
    settings, schema = got["settings"], got["schema"]
    for name in ("background_color", "shadows_quality", "light_state", "physics_gravity_3d", "ssao_enabled",
                 "fixed_resolution_filter", "post_process", "default_mesh_shader", "ssr_max_steps"):
        ed.check_in(name, settings)
    ed.check_eq(schema["shadows_quality"]["options"], ["none", "low", "medium", "high"])
    before = settings
    out = await ed.call("set_scene_settings", settings={
        "background_color": "#336699", "shadows_quality": "High", "light_state": "on",
        "physics_gravity_3d": [0, -20, 0], "ssao_enabled": True, "ssao_radius": 0.75, "ssr_max_steps": 32,
        "fixed_resolution_filter": "linear", "ambient_light_2d_color": [0.5, 0.5, 0.5]})
    s = out["settings"]
    check_close(ed, s["background_color"], [0.2, 0.4, 0.6, before["background_color"][3]], "hex color keeps alpha")
    ed.check_eq(s["shadows_quality"], "high")
    ed.check_eq(s["light_state"], "on")
    ed.check_eq(s["ssao_enabled"], True)
    ed.check_eq(s["fixed_resolution_filter"], "linear")
    # one undo step for all of them
    await undo(ed)
    after = (await ed.call("get_scene_settings"))["settings"]
    for k in ("background_color", "shadows_quality", "light_state", "physics_gravity_3d", "ssao_enabled",
              "ssao_radius", "ssr_max_steps", "fixed_resolution_filter"):
        ed.check(close(after[k], before[k]), f"{k} not undone: {after[k]} vs {before[k]}")
    # errors
    await ed.check_error("set_scene_settings", settings={"no_such": 1}, contains="background_color")
    await ed.check_error("set_scene_settings", settings={"shadows_quality": "ultra"}, contains="medium")
    await ed.check_error("set_scene_settings", settings={"post_process": [{"shader": "nope/missing"}]}, contains="fork")
    # all-or-nothing
    await ed.check_error("set_scene_settings", settings={"ssao_enabled": True, "ssao_radius": "big"})
    ed.check_eq((await ed.call("get_scene_settings", names=["ssao_enabled"]))["settings"]["ssao_enabled"],
                before["ssao_enabled"], "a failed set must change nothing")
    # post-process passes (built-in passthrough) round trip
    await ed.call("set_scene_settings", settings={"post_process": [{"shader": "", "enabled": False, "uniforms": {"k": 2}}]})
    pp = (await ed.call("get_scene_settings", names=["post_process"]))["settings"]["post_process"]
    ed.check_eq(pp, [{"shader": "", "enabled": False, "uniforms": {"k": [2.0, 0.0, 0.0, 0.0]}}])
    await undo(ed)


# -- hierarchy ----------------------------------------------------------------------


async def test_scene_tree_and_find(ed: Editor):
    # fixture scene
    text = await ed.call_raw("get_scene_tree", {"scene": "Main Scene", "depth": 1})
    ed.check("Background [1010]" in text.text and "  Background 1 [1011] mesh, sprite" in text.text,
             f"tree text: {text.text[:600]}")
    got = await ed.call("get_scene_tree", scene="Main Scene", format="json", root="Rocks")
    ed.check_eq(got["root"]["path"], "Rocks")
    ed.check_eq(len(got["nodes"][0]["children"]), 8)
    small = await ed.call("get_scene_tree", scene="Main Scene", limit=3)
    ed.check(small["truncated"], "limit should truncate")
    found = await ed.call("find_entities", scene="Main Scene", name="rock*")
    ed.check_eq(found["count"], 9, "Rocks + 8 rocks (case-insensitive glob)")
    found = await ed.call("find_entities", scene="Main Scene", name="Rock Top*", under="Rocks", component="sprite")
    ed.check_eq(found["count"], 4)
    ed.check(all(e["path"].startswith("Rocks/") for e in found["entities"]), "paths under Rocks")
    found = await ed.call("find_entities", scene="Main Scene", component=["audiosource"])
    ed.check_eq(found["count"], 4, "audio sources")
    found = await ed.call("find_entities", scene="Main Scene", path="Ground/*")
    ed.check_eq(found["count"], 2)
    await ed.check_error("find_entities", scene="Main Scene", component="nonsense", contains="Valid names")


async def test_entity_refs_and_errors(ed: Editor):
    await lab(ed)
    a = await create(ed, "empty", name="Dup")
    b = await create(ed, "empty", name="Dup")    # made unique ("Dup 2") by the editor
    await ed.call("rename_entity", entity=b, name="Dup")
    child = await create(ed, "empty", name="Kid", parent=b)
    gun_a = await create(ed, "empty", name="Gun", parent=a)
    gun_b = await create(ed, "empty", name="Gun", parent=b)   # "Gun 2": the editor makes names unique
    await ed.call("rename_entity", entity=gun_b, name="Gun")
    ed.check_eq((await ed.call("get_entity", entity="Dup", detail="summary"))["id"], a, "a path wins over names")
    msg = await ed.check_error("get_entity", entity="Gun", contains="matches 2")
    ed.check("Dup/Gun" in msg and "Dup#1/Gun" in msg, f"ambiguity lists the paths: {msg}")
    await ed.check_error("get_entity", entity={"name": "Dup"}, contains="pass an id")
    ed.check_eq((await ed.call("get_entity", entity="Dup#1/Gun", detail="summary"))["id"], gun_b)
    ed.check_eq((await ed.call("get_entity", entity={"path": "Dup/Gun"}, detail="summary"))["id"], gun_a)
    ed.check_eq((await ed.call("get_entity", entity="Dup#1", detail="summary"))["id"], b)
    ed.check_eq((await ed.call("get_entity", entity="Dup#1/Kid", detail="summary"))["id"], child)
    ed.check_eq((await ed.call("get_entity", entity={"path": "Dup"}, detail="summary"))["id"], a)
    ed.check_eq((await ed.call("get_entity", entity={"name": "Kid"}, detail="summary"))["id"], child)
    ed.check_eq((await ed.call("get_entity", entity={"id": a, "name": "whatever"}, detail="summary"))["id"], a)
    ed.check_eq((await ed.call("get_entity", entity=str(a), detail="summary"))["id"], a)
    await ed.check_error("get_entity", entity=999999, contains="No GameObject with id")
    await ed.check_error("get_entity", entity="No/Such", contains="get_scene_tree")
    await ed.check_error("get_entity", entity=a, scene="NoSuchScene", contains="Scenes:")
    await ed.check_error("set_properties", entity=a, values={"nonsense": {"x": 1}}, contains="Valid names")
    await ed.check_error("set_properties", entity=a, values={"transform": {"nope": 1}}, contains="position")
    await ed.check_error("set_properties", entity=a, values={"sprite": {"width": 3}}, contains="no sprite")
    await ed.call("delete_entities", entities=[a, b])


async def test_create_every_kind(ed: Editor):
    await lab(ed)
    kinds = ed.tool("create_entity").input_schema["properties"]["type"]["enum"]
    ed.check(len(kinds) > 45, f"kinds from the schema enum: {kinds}")
    made = []
    for kind in kinds:
        out = await ed.call("create_entity", type=kind, name="K_" + kind, select=False)
        if kind not in ("empty", "bare"):
            ed.check(out["components"], f"{kind}: no components: {out}")
        made.append(out["entity_id"])
    ed.check_in("label", [name.lower() for name in await tree_names(ed)])
    await ed.check_error("create_entity", type="definitely_not", contains="cube")
    # Multi delete, one undo restores them all.
    some = made[:4]
    rest = made[4:]
    await ed.call("delete_entities", entities=some)
    ed.check(not any([await exists(ed, e) for e in some]), "deleted")
    await undo(ed)
    ed.check(all([await exists(ed, e) for e in some]), "one undo restores all deleted")
    await redo(ed)
    ed.check(not any([await exists(ed, e) for e in some]), "redo deletes again")
    await ed.call("delete_entities", entities=rest)


async def test_create_with_values_one_undo(ed: Editor):
    await lab(ed)
    parent = await create(ed, "empty", name="Level")
    out = await ed.call("create_entity", type="cube", name="Crate", parent="Level", position=[1, 2, 3],
                        rotation={"euler": [0, 90, 0]}, scale=[2, 2, 2], components=["rigidbody"],
                        values={"mesh": {"submeshes[0].material.baseColorFactor": "#ff8000", "castShadows": False},
                                "rigidbody": {"type": "dynamic"}})
    crate = out["entity_id"]
    ed.check_eq(out["entity"]["path"], "Level/Crate")
    ed.check_in("rigidbody", out["components"])
    t = await values(ed, crate, "transform", "position", "rotation", "scale")
    check_close(ed, t["position"], [1, 2, 3], "position")
    check_close(ed, t["rotation"]["euler"], [0, 90, 0], "euler")
    check_close(ed, t["scale"], [2, 2, 2], "scale")
    m = await values(ed, crate, "mesh", "submeshes[0].material.baseColorFactor", "castShadows")
    check_close(ed, m["submeshes[0].material.baseColorFactor"], [1, 128 / 255, 0, 1], "hex color")
    ed.check_eq(m["castShadows"], False)
    ed.check_eq(await value(ed, crate, "rigidbody", "type"), "dynamic")
    # one undo removes it all; redo brings it back with its values
    await undo(ed)
    ed.check(not await exists(ed, crate), "create undone in one step")
    await redo(ed)
    ed.check(await exists(ed, crate), "redo")
    ed.check_eq(await value(ed, crate, "rigidbody", "type"), "dynamic", "values after redo")
    # a bad value fails the whole call and leaves nothing behind
    before = await tree_names(ed)
    await ed.check_error("create_entity", type="cube", name="BadCube", values={"rigidbody": {"type": "flying"}},
                         contains="no rigidbody")
    await ed.check_error("create_entity", type="cube", name="BadCube", components=["rigidbody"],
                         values={"rigidbody": {"type": "flying"}}, contains="dynamic")
    ed.check_eq(await tree_names(ed), before, "nothing created by failed calls")
    # legacy shapes still work
    legacy = await ed.call("create_entity", name="LegacyBox", type="box", position={"x": 1, "y": 2, "z": 3})
    check_close(ed, await value(ed, legacy["entity_id"], "transform", "position"), [1, 2, 3])
    await ed.call("delete_entities", entities=[parent, legacy["entity_id"]])


async def test_delete_duplicate_rename(ed: Editor):
    await lab(ed)
    a = await create(ed, "sphere", name="Ball")
    kid = await create(ed, "empty", name="BallKid", parent=a)
    dup = await ed.call("duplicate_entities", entities=[a])
    copy = dup["created"][0]["id"]
    ed.check(copy != a, "new id")
    copy_tree = await ed.call("get_scene_tree", root=copy, format="json")
    ed.check_eq([c["name"] for c in copy_tree["nodes"][0]["children"]], ["BallKid"], "children duplicated")
    await undo(ed)
    ed.check(not await exists(ed, copy), "duplicate undone")
    await ed.call("rename_entity", entity=a, name="Ball2")
    ed.check_eq((await ed.call("get_entity", entity=a, detail="summary"))["name"], "Ball2")
    await undo(ed)
    ed.check_eq((await ed.call("get_entity", entity=a, detail="summary"))["name"], "Ball")
    await ed.call("delete_entities", entity=a)
    ed.check(not await exists(ed, a) and not await exists(ed, kid), "deleted with children")
    await undo(ed)
    ed.check(await exists(ed, a) and await exists(ed, kid), "delete undone")
    await ed.call("delete_entities", entity=a)


async def test_move_entity(ed: Editor):
    await lab(ed)
    p = await create(ed, "empty", name="MoveParent", position=[10, 0, 0])
    a = await create(ed, "empty", name="MA")
    b = await create(ed, "empty", name="MB")
    c = await create(ed, "empty", name="MC", position=[1, 1, 1])
    await ed.call("move_entity", entities=[a, b], parent=p)
    kids = (await ed.call("get_scene_tree", root=p, format="json"))["nodes"][0]["children"]
    ed.check_eq([k["name"] for k in kids], ["MA", "MB"])
    await ed.call("move_entity", entity=c, parent=p, index=0)
    kids = (await ed.call("get_scene_tree", root=p, format="json"))["nodes"][0]["children"]
    ed.check_eq([k["name"] for k in kids], ["MC", "MA", "MB"])
    check_close(ed, await value(ed, c, "transform", "position"), [-9, 1, 1], "world kept (local adapted)")
    await ed.call("move_entity", entity=c, after=b)
    kids = (await ed.call("get_scene_tree", root=p, format="json"))["nodes"][0]["children"]
    ed.check_eq([k["name"] for k in kids], ["MA", "MB", "MC"])
    await ed.call("move_entity", entity=c, before=a)
    kids = (await ed.call("get_scene_tree", root=p, format="json"))["nodes"][0]["children"]
    ed.check_eq([k["name"] for k in kids], ["MC", "MA", "MB"])
    await undo(ed)
    kids = (await ed.call("get_scene_tree", root=p, format="json"))["nodes"][0]["children"]
    ed.check_eq([k["name"] for k in kids], ["MA", "MB", "MC"], "move undone")
    # to the root keeping the local values
    await ed.call("move_entity", entity=c, parent=None, keep_world_transform=False)
    got = await ed.call("get_entity", entity=c, detail="summary")
    ed.check(got["parent"] is None, "at root")
    check_close(ed, await value(ed, c, "transform", "position"), [-9, 1, 1], "local kept")
    await undo(ed)
    check_close(ed, await value(ed, c, "transform", "position"), [-9, 1, 1], "undo")
    ed.check_eq((await ed.call("get_entity", entity=c, detail="summary"))["parent"]["id"], p, "undo reparent")
    await ed.check_error("move_entity", entity=p, parent=a, contains="own child")
    await ed.check_error("move_entity", entity=a, contains="exactly one")
    await ed.call("delete_entities", entities=[p, c])


async def test_entity_state(ed: Editor):
    await lab(ed)
    a = await create(ed, "cube", name="StateCube")
    b = await create(ed, "cube", name="StateCube2")
    out = await ed.call("set_entity_state", entities=[a, b], active=False, tag="Player", layer="UI")
    for e in out["entities"]:
        ed.check(e["active"] is False and e["tag"] == "Player", f"state: {e}")
    layer = out["entities"][0]["layer"]
    ed.check(layer > 0, "layer by name")
    found = await ed.call("find_entities", tag="Player", active=False)
    ed.check_eq({e["id"] for e in found["entities"]}, {a, b})
    await undo(ed)
    got = await ed.call("get_entity", entity=a, detail="summary")
    ed.check(got["active"] and got["tag"] == "Untagged" and got["layer"] == 0, f"state undone: {got}")
    await ed.check_error("set_entity_state", entity=a, tag="NoSuchTag_x", contains="not defined")
    await ed.check_error("set_entity_state", entity=a, layer=99, contains="0-31")
    await ed.call("delete_entities", entities=[a, b])


async def test_selection(ed: Editor):
    await lab(ed)
    a = await create(ed, "empty", name="SelA")
    b = await create(ed, "empty", name="SelB")
    out = await ed.call("select_entities", entities=[a])
    ed.check_eq([e["id"] for e in out["entities"]], [a])
    out = await ed.call("select_entities", entity=b, add=True)
    ed.check_eq([e["id"] for e in out["entities"]], [a, b])
    got = await ed.call("get_selection")
    ed.check_eq([e["id"] for e in got["entities"]], [a, b])
    out = await ed.call("select_entities", entities=[])
    ed.check_eq(out["entities"], [])
    await ed.call("delete_entities", entities=[a, b])


async def test_copy_paste_entities(ed: Editor):
    await lab(ed)
    a = await create(ed, "cube", name="CopyMe", position=[3, 0, 0])
    await create(ed, "empty", name="CopyKid", parent=a)
    target = await create(ed, "empty", name="PasteHere")
    await ed.call("copy_entities", entity=a)
    out = await ed.call("paste_entities", parent=target)
    pasted = out["created"][0]
    ed.check_eq(pasted["path"], "PasteHere/CopyMe")
    check_close(ed, await value(ed, pasted["id"], "transform", "position"), [3, 0, 0])
    await undo(ed)
    ed.check(not await exists(ed, pasted["id"]), "paste undone")
    # into another scene
    out = await ed.call("paste_entities", scene="Main Scene")
    ed.check_eq(out["created"][0]["name"], "CopyMe")
    await ed.call("undo", scene="Main Scene")
    # cut
    await ed.call("copy_entities", entity=a, cut=True)
    ed.check(not await exists(ed, a), "cut deletes")
    out = await ed.call("paste_entities")
    ed.check_eq(out["created"][0]["name"], "CopyMe")
    await ed.call("delete_entities", entities=[out["created"][0]["id"], target])
    await ed.check_error("paste_entities", parent="NoSuchParent_zz")


# -- components and properties ---------------------------------------------------------


async def test_get_entity_details(ed: Editor):
    await lab(ed)
    e = await create(ed, "cube", name="Detail")
    summary = await ed.call("get_entity", entity=e, detail="summary")
    ed.check_eq(set(summary["components"]), {"transform", "mesh"})
    ed.check_eq(summary["path"], "Detail")
    vals = await ed.call("get_entity", entity=e)
    ed.check("castShadows" in vals["components"]["mesh"], "values detail")
    ed.check(not any(k.startswith("submeshes[1]") for k in vals["components"]["mesh"]), "unused submeshes hidden")
    full = await ed.call("get_entity", entity=e, detail="full", components=["mesh"], properties=["submeshes[0].material.*"])
    alpha = full["components"]["mesh"]["submeshes[0].material.alphaMode"]
    ed.check_eq(alpha["type"], "enum")
    ed.check_eq(alpha["options"], ["auto", "opaque", "mask", "blend"])
    ed.check_eq(alpha["default"], "auto")
    ed.check(all(k.startswith("submeshes[0].material.") for k in full["components"]["mesh"]), "property glob")
    yaml = await ed.call("get_entity", entity=e, detail="yaml", components=["transform"])
    ed.check_in("position", yaml["components"]["transform"])
    await ed.call("delete_entities", entity=e)


async def test_property_types_roundtrip(ed: Editor):
    """Every PropertyType: read, write a new value, read it back; then write the read value back unchanged."""
    await lab(ed)
    cube = await create(ed, "cube", name="Types")
    light = await create(ed, "point_light", name="TypesLight")
    audio = await create(ed, "audio_source", name="TypesAudio")
    text = await create(ed, "text", name="TypesText")
    image = await create(ed, "image", name="TypesImage")
    button = await create(ed, "button", name="TypesButton")
    tween = await create(ed, "position_tween", name="TypesTween")
    cam = await create(ed, "camera", name="TypesCam")
    scrollbar = await create(ed, "scrollbar", name="TypesScrollbar")

    cases = [
        # (entity, component, property, new value, expected read back)
        (cube, "mesh", "castShadows", False, False),                                   # Bool
        (image, "image", "patchMarginLeft", 7, 7),                                     # UInt
        (scrollbar, "scrollbar", "barMarginLeft", -3, -3),                              # Int
        (cam, "camera", "cullingMask", 5, 5),                                           # UInt
        (light, "light", "intensity", 2.5, 2.5),                                        # Float
        (audio, "audiosource", "volume", 0.25, 0.25),                                   # Double
        (text, "text", "text", "Hello MCP", "Hello MCP"),                               # String
        (text, "recttransform", "pivot", {"x": 0.5, "y": 0.25}, [0.5, 0.25]),           # Vector2
        (cube, "transform", "position", [4, 5, 6], [4, 5, 6]),                          # Vector3
        (image, "ui", "color", "#00ff0080", [0, 1, 0, 128 / 255]),                      # Vector4 (hex)
        (image, "ui", "color", [1, 0, 0], [1, 0, 0, 128 / 255]),                        # Vector4 rgb keeps alpha
        (light, "light", "type", "Spot", "spot"),                                       # Enum (label)
        (light, "light", "type", 0, "directional"),                                     # Enum (int)
        (cam, "camera", "type", "orthographic", "orthographic"),                        # Enum (name)
        (tween, "tween", "function", "Quad In Out", "quad_in_out"),                     # Ease (label)
        (tween, "tween", "function", "EASE_IN_OUT_CUBIC", "cubic_in_out"),              # Ease (other naming)
        (tween, "tween", "function", "sine_out", "sine_out"),                           # Ease (name)
        (image, "ui", "texture", "assets/sprites/rock.png", "assets/sprites/rock.png"),  # Texture
        (text, "text", "font", "assets/fonts/KenneyFutureNarrow.ttf", ["assets/fonts/KenneyFutureNarrow.ttf"]),  # Font
        (button, "button", "label", {"name": "TypesText"}, None),                       # Entity (checked below)
    ]
    covered = set()
    for entity, comp, prop, new, expected in cases:
        full = await ed.call("get_entity", entity=entity, scene=LAB["id"], detail="full", components=[comp], properties=[prop])
        covered.add(full["components"][comp][prop]["type"])
        await set_props(ed, entity, {comp: {prop: new}})
        got = await value(ed, entity, comp, prop)
        if expected is not None:
            check_close(ed, got, expected, f"{comp}.{prop}")
        # the read value written back is accepted and changes nothing
        await set_props(ed, entity, {comp: {prop: got}})
        check_close(ed, await value(ed, entity, comp, prop), got, f"{comp}.{prop} round trip")
    label = await value(ed, button, "button", "label")
    ed.check_eq(label["id"], text, "Entity property by {name}")
    await set_props(ed, button, {"button": {"label": None}})
    ed.check(await value(ed, button, "button", "label") is None, "Entity null")

    # Quat: euler, quat, and both (the changed one wins)
    await set_props(ed, cube, {"transform": {"rotation": {"euler": [0, 0, 90]}}})
    rot = await value(ed, cube, "transform", "rotation")
    check_close(ed, rot["euler"], [0, 0, 90], "euler write")
    check_close(ed, rot["quat"], [0, 0, math.sin(math.pi / 4), math.cos(math.pi / 4)], "quat read")
    rot["euler"] = [0, 45, 0]
    await set_props(ed, cube, {"transform": {"rotation": rot}})
    check_close(ed, (await value(ed, cube, "transform", "rotation"))["euler"], [0, 45, 0], "edited euler wins")
    await set_props(ed, cube, {"transform": {"rotation": [0, 0, 0, 1]}})
    check_close(ed, (await value(ed, cube, "transform", "rotation"))["euler"], [0, 0, 0], "quat array")

    # Texture objects: sampler settings and a camera render texture
    await set_props(ed, image, {"ui": {"texture": {"path": "assets/sprites/rock.png", "min_filter": "nearest",
                                                   "wrap_u": "clamp_to_edge"}}})
    tex = await value(ed, image, "ui", "texture")
    ed.check_eq(tex["min_filter"], "nearest")
    ed.check_eq(tex["path"], "assets/sprites/rock.png")
    await set_props(ed, image, {"ui": {"texture": tex}})
    ed.check_eq(await value(ed, image, "ui", "texture"), tex, "texture object round trip")
    rt_cam = await create(ed, "camera", name="RTCam")
    await set_props(ed, image, {"ui": {"texture": {"camera": "RTCam"}}})
    tex = await value(ed, image, "ui", "texture")
    ed.check_eq(tex["camera"]["id"], rt_cam)
    ed.check_eq(await value(ed, rt_cam, "camera", "renderToTexture"), True, "camera renders to texture")
    await set_props(ed, image, {"ui": {"texture": None}})
    ed.check(await value(ed, image, "ui", "texture") is None, "texture cleared")
    await ed.check_error("set_properties", entity=image, values={"ui": {"texture": "assets/sprites/missing.png"}},
                         contains="does not exist")

    # Material: inline object (partial), then the whole object round trip
    await set_props(ed, cube, {"mesh": {"submeshes[0].material": {"baseColorFactor": [0.2, 0.3, 0.4, 1],
                                                                   "roughnessFactor": 0.25, "alphaMode": "blend",
                                                                   "baseColorTexture": "assets/sprites/rock.png"}}})
    mat = await value(ed, cube, "mesh", "submeshes[0].material")
    check_close(ed, mat["baseColorFactor"], [0.2, 0.3, 0.4, 1])
    ed.check_eq(mat["alphaMode"], "blend")
    ed.check_eq(mat["baseColorTexture"], "assets/sprites/rock.png")
    ed.check(close(mat["metallicFactor"], 1.0), "untouched field kept")
    await set_props(ed, cube, {"mesh": {"submeshes[0].material": mat}})
    ed.check(close(await value(ed, cube, "mesh", "submeshes[0].material"), mat), "material round trip")
    await ed.check_error("set_properties", entity=cube, values={"mesh": {"submeshes[0].material": {"shininess": 1}}},
                         contains="baseColorFactor")

    # EntityReference (a script property of the fixture's GameController)
    ref = await value(ed, "Game", "script", "scripts[0].background1", scene="Main Scene")
    ed.check_eq(ref["name"], "Background 1")
    await set_props(ed, "Game", {"script": {"scripts[0].background1": "Ground/Ground 1"}}, scene="Main Scene")
    ref2 = await value(ed, "Game", "script", "scripts[0].background1", scene="Main Scene")
    ed.check_eq(ref2["path"], "Ground/Ground 1")
    await set_props(ed, "Game", {"script": {"scripts[0].background1": ref}}, scene="Main Scene")
    ed.check_eq(await value(ed, "Game", "script", "scripts[0].background1", scene="Main Scene"), ref, "ref round trip")
    await ed.call("undo", scene="Main Scene", steps=3)

    covered |= {"quaternion", "material", "entity_reference"}  # checked above (types asserted below)
    full = await ed.call("get_entity", entity=cube, detail="full", components=["transform", "mesh"],
                         properties=["rotation", "submeshes[0].material", "shaderUniforms"])
    ed.check_eq(full["components"]["transform"]["rotation"]["type"], "quaternion")
    ed.check_eq(full["components"]["mesh"]["submeshes[0].material"]["type"], "material")
    ed.check_eq(full["components"]["mesh"]["shaderUniforms"]["type"], "custom")
    covered.add("custom")
    full = await ed.call("get_entity", entity="Game", scene="Main Scene", detail="full", components=["script"],
                         properties=["scripts[0].background1"])
    ed.check_eq(full["components"]["script"]["scripts[0].background1"]["type"], "entity_reference")
    every = {"bool", "int", "uint", "float", "double", "string", "vector2", "vector3", "vector4", "quaternion",
             "enum", "ease", "texture", "font", "material", "entity", "entity_reference", "custom"}
    ed.check_eq(covered, every, "every property type covered")

    # undo of a property edit
    await set_props(ed, light, {"light": {"intensity": 9}})
    await undo(ed)
    ed.check(close(await value(ed, light, "light", "intensity"), 2.5), "property edit undone")
    await ed.call("delete_entities", entities=[cube, light, audio, text, image, button, tween, cam, rt_cam, scrollbar])


async def test_enum_errors(ed: Editor):
    await lab(ed)
    e = await create(ed, "rigidbody2d", name="EnumBody")
    msg = await ed.check_error("set_properties", entity=e, values={"rigidbody2d": {"type": "flying"}})
    for name in ("static", "kinematic", "dynamic"):
        ed.check(name in msg, f"error lists {name}: {msg}")
    await ed.check_error("set_properties", entity=e, values={"rigidbody2d": {"type": 7}}, contains="not one of")
    await set_props(ed, e, {"rigidbody2d": {"type": "KINEMATIC"}})
    ed.check_eq(await value(ed, e, "rigidbody2d", "type"), "kinematic")
    await ed.call("delete_entities", entity=e)


async def test_custom_roundtrip(ed: Editor):
    """Custom properties (collider lists, points, bursts) through their scene-file encoding."""
    await lab(ed)
    body = await create(ed, "rigidbody2d", name="CustomBody")
    collider = {"type": "circle", "radius": 25, "density": 2}
    await ed.call("set_properties", entity=body, changes=[{"component": "rigidbody2d", "property": "colliders", "insert": collider}])
    got = await values(ed, body, "rigidbody2d", "numColliders", "colliders[0].type", "colliders[0].radius", "colliders")
    ed.check_eq(got["numColliders"], 1)
    ed.check_eq(got["colliders[0].type"], "circle", "typed view of the inserted element")
    check_close(ed, got["colliders[0].radius"], 25)
    # typed edit of an element, then the whole list written back unchanged
    await set_props(ed, body, {"rigidbody2d": {"colliders[0].radius": 30}})
    lst = await value(ed, body, "rigidbody2d", "colliders")
    await set_props(ed, body, {"rigidbody2d": {"colliders": lst}})
    ed.check_eq(await value(ed, body, "rigidbody2d", "colliders"), lst, "colliders round trip")
    check_close(ed, await value(ed, body, "rigidbody2d", "colliders[0].radius"), 30)
    # second element, then remove the first; one undo puts it back
    await ed.call("set_properties", entity=body, changes=[
        {"component": "rigidbody2d", "property": "colliders", "insert": {"type": "polygon", "vertices": [[0, 0], [10, 0], [0, 10]]}}])
    ed.check_eq(await value(ed, body, "rigidbody2d", "numColliders"), 2)
    await ed.call("set_properties", entity=body, changes=[{"component": "rigidbody2d", "property": "colliders", "remove": 0}])
    ed.check_eq(await value(ed, body, "rigidbody2d", "colliders[0].type"), "polygon")
    await undo(ed)
    ed.check_eq(await value(ed, body, "rigidbody2d", "colliders[0].type"), "circle", "remove undone")

    caster = await create(ed, "shadow_caster_2d", name="CustomCaster")
    pts = await value(ed, caster, "shadowcaster2d", "points")
    ed.check(isinstance(pts, list) and len(pts) == 4, f"points: {pts}")
    new_pts = [[0, 0], [20, 0], [10, 15]]
    await set_props(ed, caster, {"shadowcaster2d": {"points": new_pts}})
    check_close(ed, await value(ed, caster, "shadowcaster2d", "points"), new_pts)

    ps = await create(ed, "particle_system", name="CustomParticles")
    await set_props(ed, ps, {"particlesystem": {"bursts": [{"time": 0.5, "minCount": 3, "maxCount": 7}]}})
    bursts = await value(ed, ps, "particlesystem", "bursts")
    ed.check(len(bursts) == 1 and bursts[0]["minCount"] == 3 and bursts[0]["maxCount"] == 7, f"bursts: {bursts}")
    await set_props(ed, ps, {"particlesystem": {"spriteInitializer.frames": [0, 2, 4]}})
    ed.check_eq(await value(ed, ps, "particlesystem", "spriteInitializer.frames"), [0, 2, 4])
    await ed.check_error("set_properties", entity=ps, changes=[{"component": "particlesystem", "property": "bursts", "remove": 5}],
                         contains="out of range")

    # shaderUniforms (typed custom)
    cube = await create(ed, "cube", name="Uniforms")
    await set_props(ed, cube, {"mesh": {"shaderUniforms": {"tint": [1, 0.5, 0, 1], "power": 2}}})
    ed.check_eq(await value(ed, cube, "mesh", "shaderUniforms"), {"tint": [1, 0.5, 0, 1], "power": [2, 0, 0, 0]})
    await ed.call("delete_entities", entities=[body, caster, ps, cube])


async def test_set_properties_atomic(ed: Editor):
    await lab(ed)
    a = await create(ed, "cube", name="AtomA")
    b = await create(ed, "cube", name="AtomB")
    out = await ed.call("set_properties", entities=[a, b], changes=[
        {"component": "transform", "property": "position", "value": [7, 7, 7]},
        {"component": "mesh", "property": "castShadows", "value": False},
        {"component": "transform", "property": "scale", "value": [3, 3, 3]}])
    check_close(ed, out["values"]["transform"]["position"], [7, 7, 7], "read back")
    for e in (a, b):
        check_close(ed, await value(ed, e, "transform", "scale"), [3, 3, 3])
    await undo(ed)
    for e in (a, b):
        check_close(ed, await value(ed, e, "transform", "position"), [0, 0, 0], "one undo for all")
        ed.check_eq(await value(ed, e, "mesh", "castShadows"), True)
    # one bad change: nothing applies
    await ed.check_error("set_properties", entity=a, changes=[
        {"component": "transform", "property": "position", "value": [9, 9, 9]},
        {"component": "mesh", "property": "castShadows", "value": "yes"}], contains="true or false")
    check_close(ed, await value(ed, a, "transform", "position"), [0, 0, 0], "nothing applied")
    await ed.call("delete_entities", entities=[a, b])


async def test_set_transform(ed: Editor):
    await lab(ed)
    p = await create(ed, "empty", name="TP", position=[10, 0, 0], scale=[2, 2, 2])
    c = await create(ed, "empty", name="TC", parent=p)
    await ed.call("set_transform", entity=c, position=[12, 4, 0], space="world")
    got = await ed.call("get_entity", entity=c, detail="summary")
    check_close(ed, got["world"]["position"], [12, 4, 0], "world position")
    check_close(ed, await value(ed, c, "transform", "position"), [1, 2, 0], "local from world")
    await ed.call("set_transform", entity=c, position=[1, 0, 0], relative=True)
    check_close(ed, await value(ed, c, "transform", "position"), [2, 2, 0], "relative")
    await ed.call("set_transform", entity=c, rotation={"euler": [0, 30, 0]}, scale=3)
    t = await values(ed, c, "transform", "rotation", "scale")
    check_close(ed, t["rotation"]["euler"], [0, 30, 0])
    check_close(ed, t["scale"], [3, 3, 3])
    await undo(ed)
    check_close(ed, await value(ed, c, "transform", "scale"), [1, 1, 1], "undo")
    await ed.check_error("set_transform", entity=c, space="galaxy")
    await ed.call("delete_entities", entity=p)


async def test_components(ed: Editor):
    await lab(ed)
    a = await create(ed, "cube", name="CompA")
    b = await create(ed, "cube", name="CompB")
    out = await ed.call("add_component", entities=[a, b], component=["rigidbody", "audiosource"],
                        values={"rigidbody": {"type": "kinematic"}, "audiosource": {"volume": 0.5}})
    ed.check_eq(len(out["added"]), 4)
    ed.check_eq(await value(ed, b, "rigidbody", "type"), "kinematic")
    await undo(ed)
    ed.check("rigidbody" not in (await ed.call("get_entity", entity=a, detail="summary"))["components"], "add undone")
    await redo(ed)
    await ed.check_error("add_component", entity=a, component="rigidbody", contains="already")
    await ed.check_error("add_component", entity=a, component="prefab", contains="cannot be added")

    # reset
    await set_props(ed, a, {"audiosource": {"volume": 0.1}, "rigidbody": {"type": "dynamic"}})
    await ed.call("reset_component", entity=a, component="audiosource")
    check_close(ed, await value(ed, a, "audiosource", "volume"), 1.0, "reset to default")
    await undo(ed)
    check_close(ed, await value(ed, a, "audiosource", "volume"), 0.1, "reset undone")

    # copy / paste values
    await ed.call("copy_component", entity=a, component="rigidbody")
    await ed.call("paste_component", entity=b)
    ed.check_eq(await value(ed, b, "rigidbody", "type"), "dynamic", "pasted values")
    await undo(ed)
    ed.check_eq(await value(ed, b, "rigidbody", "type"), "kinematic", "paste undone")
    c = await create(ed, "empty", name="CompC")
    await ed.check_error("paste_component", entity=c, contains="as_new")
    await ed.call("paste_component", entity=c, as_new=True)
    ed.check_eq(await value(ed, c, "rigidbody", "type"), "dynamic", "paste as new")

    # remove (multi)
    await ed.call("remove_component", entities=[a, b], component="audiosource")
    ed.check("audiosource" not in (await ed.call("get_entity", entity=b, detail="summary"))["components"])
    await undo(ed)
    ed.check("audiosource" in (await ed.call("get_entity", entity=b, detail="summary"))["components"], "remove undone")
    await ed.check_error("remove_component", entity=c, component="light", contains="no light")
    await ed.call("delete_entities", entities=[a, b, c])


async def test_single_property_and_full_detail(ed: Editor):
    """One change through set_properties.changes; get_entity detail full gives types; misspelled args fail."""
    await lab(ed)
    e = await create(ed, "point_light", name="OneProp")
    before = await value(ed, e, "light", "intensity")
    out = await ed.call("set_properties", entity=e, changes=[{"component": "light", "property": "intensity", "value": 4}])
    check_close(ed, out["values"]["light"]["intensity"], 4)
    got = await ed.call("get_entity", entity=e, components=["light"], detail="full")
    props = got["components"]["light"]
    ed.check_eq(props["type"]["value"], "point", f"enum in full detail: {props['type']!r}")
    check_close(ed, props["intensity"]["value"], 4)
    await undo(ed)
    check_close(ed, await value(ed, e, "light", "intensity"), before, "undo")
    await ed.check_error("get_entity", entity=e, component="light", contains="Unknown argument")
    await ed.call("delete_entities", entity=e)


async def test_set_mesh_geometry(ed: Editor):
    await lab(ed)
    e = await create(ed, "cube", name="Shape")
    out = await ed.call("set_mesh_geometry", entity=e, shape="sphere", radius=2)
    ed.check_eq(out["shape"], "sphere")
    check_close(ed, out["local_size"], [4, 4, 4], "sphere size")
    out = await ed.call("set_mesh_geometry", entity=e, shape="box", width=3, height=1, depth=2)
    check_close(ed, out["local_size"], [3, 1, 2], "box size")
    await undo(ed)
    got = await ed.call("get_entity", entity=e, detail="summary")
    ed.check(got, "entity still there after undo")
    await ed.check_error("set_mesh_geometry", entity=e, shape="cone", contains="shape must be")
    s = await create(ed, "sprite", name="NotPrimitive")
    await ed.check_error("set_mesh_geometry", entity=s, shape="box", contains="generated")
    await ed.call("delete_entities", entities=[e, s])


async def test_list_component_types(ed: Editor):
    out = await ed.call("list_component_types")
    names = {c["name"]: c for c in out["components"]}
    ed.check(names["rigidbody2d"]["addable"], "rigidbody2d addable")
    ed.check_eq(names["rigidbody2d"]["display_name"], "Rigidbody 2D")
    one = await ed.call("list_component_types", component="Light")
    ed.check_eq(one["properties"]["type"]["options"], ["directional", "point", "spot"])
    ed.check_in("intensity", one["properties"])
    # Unity display names and aliases resolve
    await ed.call("list_component_types", component="Sprite Renderer")
    await ed.call("list_component_types", component="audio_source")


async def test_model_and_material_link(ed: Editor):
    await lab(ed)
    models = ed.project / "assets" / "models"
    models.mkdir(parents=True, exist_ok=True)
    (models / "tri.obj").write_text("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n")
    out = await ed.call("create_entity", type="model", name="Tri", model="assets/models/tri.obj", position=[0, 0, -2])
    tri = out["entity_id"]
    ed.check_eq(await value(ed, tri, "model", "filename"), "assets/models/tri.obj", "model file loaded")
    ed.check(await value(ed, tri, "mesh", "numSubmeshes") >= 1, "model geometry")
    await undo(ed)
    ed.check(not await exists(ed, tri), "model create undone in one step")
    await ed.check_error("create_entity", type="model", model="assets/models/none.obj", contains="does not exist")

    mats = ed.project / "assets" / "materials"
    mats.mkdir(parents=True, exist_ok=True)
    (mats / "red.material").write_text("baseColorFactor: [1, 0, 0, 1]\nroughnessFactor: 0.5\n")
    cube = await create(ed, "cube", name="Linked")
    await set_props(ed, cube, {"mesh": {"submeshes[0].material": "assets/materials/red.material"}})
    mat = await value(ed, cube, "mesh", "submeshes[0].material")
    ed.check_eq(mat.get("file"), "assets/materials/red.material", f"linked: {mat}")
    check_close(ed, mat["baseColorFactor"], [1, 0, 0, 1])
    # the read value written back keeps the link
    await set_props(ed, cube, {"mesh": {"submeshes[0].material": mat}})
    ed.check_eq((await value(ed, cube, "mesh", "submeshes[0].material")).get("file"), "assets/materials/red.material")
    # an inline edit unlinks; undo links it again
    await set_props(ed, cube, {"mesh": {"submeshes[0].material": {"roughnessFactor": 0.9}}})
    mat2 = await value(ed, cube, "mesh", "submeshes[0].material")
    ed.check("file" not in mat2 and close(mat2["roughnessFactor"], 0.9), f"inline edit unlinks: {mat2}")
    await undo(ed)
    ed.check_eq((await value(ed, cube, "mesh", "submeshes[0].material")).get("file"), "assets/materials/red.material", "undo relinks")
    await set_props(ed, cube, {"mesh": {"submeshes[0].material": None}})
    ed.check("file" not in await value(ed, cube, "mesh", "submeshes[0].material"), "null unlinks")
    await ed.check_error("set_properties", entity=cube, values={"mesh": {"submeshes[0].material": None}}, contains="not linked")
    await ed.call("delete_entities", entity=cube)


async def test_prefab_restructure_block(ed: Editor):
    ed.require_tools("create_prefab")
    await lab(ed)
    root = await create(ed, "cube", name="PrefabRoot")
    await create(ed, "empty", name="PrefabKid", parent=root)
    other = await create(ed, "empty", name="Elsewhere")
    await ed.call("create_prefab", entity=root, path="prefabs/W1Block.prefab")
    got = await ed.call("get_entity", entity="PrefabRoot/PrefabKid", detail="summary")
    ed.check_eq(got["prefab"]["path"], "prefabs/W1Block.prefab")
    ed.check_eq(got["prefab"]["instance_root"]["id"], root)
    await ed.check_error("move_entity", entity="PrefabRoot/PrefabKid", parent=other, contains="Open the Prefab")
    # the instance root itself moves freely
    await ed.call("move_entity", entity=root, parent=other)
    await undo(ed)
    await ed.call("delete_entities", entities=[root, other])


# -- empty project ------------------------------------------------------------------------


async def test_empty_project_flow():
    """A new empty project: build a small 3D scene from scratch, save it, then reopen the project
    in a second editor and check that everything was saved."""
    first = Editor(create=True, keep=True, label="w1-empty")
    try:
        async with first as ed:
            project = ed.project
            scene = (await ed.call("create_scene", name="World", type="3D", path="scenes/World"))["scene"]["id"]
            await ed.call("create_entity", scene=scene, type="plane", name="Ground", scale=[10, 1, 10],
                          values={"mesh": {"submeshes[0].material.baseColorFactor": [0.2, 0.6, 0.2, 1]}})
            await ed.call("create_entity", scene=scene, type="empty", name="Props")
            crate = await ed.call("create_entity", scene=scene, type="cube", name="Crate", parent="Props",
                                  position=[0, 0.5, 0], components=["rigidbody"],
                                  values={"rigidbody": {"type": "dynamic"},
                                          "mesh": {"submeshes[0].material": {"baseColorFactor": "#c08040", "roughnessFactor": 0.3}}})
            await ed.call("duplicate_entities", scene=scene, entity="Props/Crate")
            await ed.call("create_entity", scene=scene, type="camera", name="Main Camera", position=[0, 3, 8],
                          rotation={"euler": [-15, 0, 0]})
            await ed.call("set_main_camera", scene=scene, entity="Main Camera")
            await ed.call("create_entity", scene=scene, type="point_light", name="Lamp", position=[2, 3, 0],
                          values={"light": {"intensity": 3, "color": [1, 0.9, 0.7]}})
            await ed.call("set_scene_settings", scene=scene, settings={"background_color": [0.1, 0.1, 0.2, 1], "shadows_quality": "high"})
            await ed.call("set_entity_state", scene=scene, entity="Lamp", tag="Player")
            await ed.call("save_scene", scene=scene)
            crate_id = crate["entity_id"]
        ed2 = Editor(project=project, label="w1-empty-reopen")
        async with ed2 as ed:
            opened = await ed.call("open_scene", scene="scenes/World.scene")
            sid = opened["scene"]["id"]
            ed.check_eq(opened["scene"]["main_camera"]["name"], "Main Camera", "main camera saved")
            tree = await ed.call("get_scene_tree", scene=sid, format="json")
            names = [n["name"] for n in tree["nodes"]]
            for n in ("Ground", "Props", "Main Camera", "Lamp"):
                ed.check_in(n, names, "saved GameObjects")
            props_node = next(n for n in tree["nodes"] if n["name"] == "Props")
            ed.check_eq([c["name"] for c in props_node["children"]], ["Crate", "Crate 2"], "children and duplicate saved")
            crate = await ed.call("get_entity", scene=sid, entity="Props/Crate", components=["rigidbody", "mesh", "transform"],
                                  properties=["type", "submeshes[0].material", "position"])
            ed.check_eq(crate["id"], crate_id, "entity ids are stable across save/load")
            ed.check_eq(crate["components"]["rigidbody"]["type"], "dynamic")
            check_close(ed, crate["components"]["transform"]["position"], [0, 0.5, 0])
            mat = crate["components"]["mesh"]["submeshes[0].material"]
            check_close(ed, mat["roughnessFactor"], 0.3)
            check_close(ed, mat["baseColorFactor"], [192 / 255, 128 / 255, 64 / 255, 1], "material color saved")
            lamp = await ed.call("get_entity", scene=sid, entity="Lamp", components=["light"], properties=["intensity"])
            check_close(ed, lamp["components"]["light"]["intensity"], 3)
            ed.check_eq(lamp["tag"], "Player")
            cam = await ed.call("get_entity", scene=sid, entity="Main Camera", components=["transform"], properties=["rotation"])
            check_close(ed, cam["components"]["transform"]["rotation"]["euler"], [-15, 0, 0])
            s = (await ed.call("get_scene_settings", scene=sid, names=["background_color", "shadows_quality"]))["settings"]
            check_close(ed, s["background_color"], [0.1, 0.1, 0.2, 1])
            ed.check_eq(s["shadows_quality"], "high")
    finally:
        if first.scratch:
            shutil.rmtree(first.scratch.root, ignore_errors=True)
