"""W2 prefab tools (editor/mcp/tools/PrefabTools.cpp) against the real editor.

The workflow of editor/project/PREFABS.md through MCP: create -> instantiate -> modify an
instance -> overrides listed -> apply / revert (property, state, added/removed component,
added/removed GameObject) -> variant and variant of a variant (apply targets along the chain)
-> Prefab Mode edits reach the instances except overridden values -> unpack / replace ->
add to / remove from a prefab -> undo of each step -> save and reload.

Tests share one editor (a scratch copy of the TappyPlane fixture) and run in order; state
built by earlier tests is kept in S.
"""

import asyncio

import yaml

from harness import Editor

TIMEOUT = 900

SCENE = 1  # Main Scene of the fixture
S: dict = {}

PREFAB_TOOLS = ("create_prefab", "create_prefab_variant", "instantiate_prefab", "replace_prefab", "unpack_prefab",
                "add_to_prefab", "remove_from_prefab", "get_prefab_info", "prefab_overrides", "prefab_mode",
                "list_prefabs", "set_standalone_prefab")
LEGACY_REMOVED = ("create_prefab_from_entity", "import_prefab_instance", "add_entity_to_prefab",
                  "remove_entity_from_prefab", "apply_prefab_component", "revert_prefab_component")


# ---------------------------------------------------------------------------
# Scene helpers


async def make_object(ed: Editor, name: str, kind: str = "box", parent=None, scene=SCENE, position=None) -> int:
    args = {"scene": scene, "name": name, "type": kind}
    if parent is not None:
        args["parent"] = parent
    if position is not None:
        args["position"] = position
    made = await ed.call("create_entity", args)
    eid = made["entity_id"]
    ed.check(eid is not None, f"create_entity returned no id: {made!r}")
    return eid


async def set_vec3(ed: Editor, entity, component: str, prop: str, value, scene=SCENE):
    return await ed.call("set_properties", scene=scene, entity=entity, values={component: {prop: list(value)}})


async def get_vec3(ed: Editor, entity, component: str, prop: str, scene=SCENE) -> list:
    v = await ed.call("get_entity", scene=scene, entity=entity, components=[component], properties=[prop])
    val = v["components"][component.lower()][prop]
    return [round(float(x), 4) for x in val]


async def delete(ed: Editor, entity, scene=SCENE):
    return await ed.call("delete_entities", scene=scene, entity=entity)


async def undo(ed: Editor, scene=SCENE, steps: int = 1):
    return await ed.call("undo", scene=scene, steps=steps)


async def redo(ed: Editor, scene=SCENE, steps: int = 1):
    return await ed.call("redo", scene=scene, steps=steps)


# ---------------------------------------------------------------------------
# Prefab helpers


async def info(ed: Editor, entity, scene=SCENE, **kw) -> dict:
    return await ed.call("get_prefab_info", scene=scene, entity=entity, **kw)


async def overrides(ed: Editor, entity, scene=SCENE, **kw) -> list:
    return (await info(ed, entity, scene=scene, usage=False, **kw))["overrides"]


def find_override(entries: list, kind: str, **match):
    for e in entries:
        if e["kind"] != kind:
            continue
        ok = True
        for k, v in match.items():
            got = e.get(k)
            if isinstance(v, (list, tuple)):
                ok = ok and set(v) <= set(got or [])
            else:
                ok = ok and got == v
        if ok:
            return e
    return None


def prefab_file(ed: Editor, rel: str) -> dict:
    return yaml.safe_load((ed.project / rel).read_text())


def prefab_member(doc: dict, name: str) -> dict | None:
    """A GameObject node of a prefab file by name, searching children too."""
    def walk(nodes):
        for n in nodes or []:
            if n.get("name") == name:
                return n
            found = walk(n.get("children"))
            if found:
                return found
        return None
    return walk(doc.get("members"))


def close_to(a, b, eps=1e-3) -> bool:
    return all(abs(float(x) - float(y)) < eps for x, y in zip(a, b))


# ---------------------------------------------------------------------------
# Tests


async def test_tools_registered(ed: Editor):
    names = ed.tool_names
    missing = [n for n in PREFAB_TOOLS if n not in names]
    ed.check(not missing, f"prefab tools missing: {missing}")
    stale = [n for n in LEGACY_REMOVED if n in names]
    ed.check(not stale, f"superseded legacy prefab tools still registered: {stale}")
    ed.check("scene_id" not in ed.tool("set_standalone_prefab").input_schema["properties"],
             "set_standalone_prefab still has the legacy schema")


async def test_create_prefab(ed: Editor):
    crate = await make_object(ed, "Crate", "box", position=[0, 0, 0])
    lid = await make_object(ed, "Lid", "sphere", parent=crate, position=[0, 1, 0])
    made = await ed.call("create_prefab", scene=SCENE, entity=crate, path="prefabs/Crate.prefab")
    item = made["prefabs"][0]
    ed.check_eq(item["path"], "prefabs/Crate.prefab", "created path")
    ed.check(item["guid"], "no guid")
    ed.check_eq(item["instance"]["id"], crate, "the GameObject itself becomes the instance root")
    ed.check((ed.project / "prefabs/Crate.prefab").exists(), "prefab file not written")
    doc = prefab_file(ed, "prefabs/Crate.prefab")
    ed.check(prefab_member(doc, "Lid") is not None, f"Lid not in the prefab file: {doc}")

    i = await info(ed, crate)
    ed.check(i["is_instance_root"] and i["in_prefab_instance"], f"not an instance root: {i}")
    ed.check_eq(i["prefab"]["path"], "prefabs/Crate.prefab")
    ed.check_eq(i["prefab"]["gameobject_count"], 2)
    ed.check_eq(i["override_count"], 0, "fresh instance has overrides")
    li = await info(ed, lid)
    ed.check_eq(li["owner"]["prefab"], "prefabs/Crate.prefab")
    ed.check(not li["owner"]["is_root"], "Lid is a member, not a root")

    # errors: an existing file, a member of an instance, bad references
    await ed.check_error("create_prefab", scene=SCENE, entity=crate, path="prefabs/Crate.prefab", contains="exists")
    await ed.check_error("create_prefab", scene=SCENE, entity=lid, path="prefabs/Lid.prefab", contains="unpack")
    await ed.check_error("create_prefab", scene=SCENE, entity="No/Such/Thing", contains="No GameObject")

    # several roots -> one prefab each, then undo removes them and the instances
    a = await make_object(ed, "TmpA", "box")
    b = await make_object(ed, "TmpB", "sphere")
    many = await ed.call("create_prefab", scene=SCENE, entities=[a, b], directory="prefabs/tmp")
    paths = sorted(p["path"] for p in many["prefabs"])
    ed.check_eq(paths, ["prefabs/tmp/TmpA.prefab", "prefabs/tmp/TmpB.prefab"], "one prefab per root")
    await undo(ed)
    ed.check(not (await info(ed, a))["in_prefab_instance"], "undo of create_prefab left TmpA an instance")
    ed.check(not (ed.project / "prefabs/tmp/TmpA.prefab").exists(), "undo of create_prefab left the file")
    await redo(ed)
    ed.check((await info(ed, a))["is_instance_root"], "redo of create_prefab")
    await undo(ed)
    for e in (b, a):
        await delete(ed, e)
    S.update(crate=crate, lid=lid)


async def test_instantiate_prefab(ed: Editor):
    made = await ed.call("instantiate_prefab", scene=SCENE, prefab="prefabs/Crate.prefab", position=[5, 0, 0], name="Crate B")
    b = made["instance"]["id"]
    ed.check_eq(made["instance"]["name"], "Crate B")
    ed.check_eq(made["gameobjects"], 1, "members imported")
    ed.check(close_to(await get_vec3(ed, b, "Transform", "position"), [5, 0, 0]), "position not applied")
    i = await info(ed, b)
    ed.check_eq(i["override_count"], 0, "a fresh instance has no overrides (name/position are default overrides)")
    ed.check_eq(i["prefab"]["usage"]["total_instances"], 2)
    lst = await ed.call("list_prefabs", filter="crate")
    ed.check_eq([p["path"] for p in lst["prefabs"]], ["prefabs/Crate.prefab"])
    ed.check_eq(lst["prefabs"][0]["instances"], 2)

    # one undo step removes it (name included), redo brings it back with the same id
    await undo(ed)
    ed.check_eq((await ed.call("get_prefab_info", prefab="prefabs/Crate.prefab"))["usage"]["total_instances"], 1)
    await redo(ed)
    ed.check_eq((await info(ed, "Crate B"))["instance_root"]["id"], b)

    await ed.check_error("instantiate_prefab", scene=SCENE, prefab="prefabs/Nope.prefab", contains="list_prefabs")
    S.update(b=b, b_lid=(await info(ed, "Crate B/Lid"))["entity"]["id"])


async def test_property_override_apply_revert(ed: Editor):
    b, b_lid = S["b"], S["b_lid"]
    a_lid = S["lid"]
    await set_vec3(ed, b_lid, "Transform", "scale", [2, 2, 2])
    ov = await overrides(ed, b, values=True)
    e = find_override(ov, "property", component="transform", properties=["scale"])
    ed.check(e is not None, f"scale override not listed: {ov}")
    ed.check_eq(e["entity"]["id"], b_lid)
    ed.check(close_to(e["values"]["scale"]["value"], [2, 2, 2]) and close_to(e["values"]["scale"]["source"], [1, 1, 1]),
             f"values: {e['values']}")
    ed.check_eq([t["prefab"] for t in e["apply_targets"]], ["prefabs/Crate.prefab"])
    ed.check(e["apply_targets"][0]["can_apply"], "cannot apply")
    ed.check_in("Apply to Prefab 'Crate'", e["apply_targets"][0]["label"])
    # the other instance is untouched
    ed.check(close_to(await get_vec3(ed, a_lid, "Transform", "scale"), [1, 1, 1]), "instance edit leaked to another instance")

    # revert (shorthand selector), undo, redo
    await ed.call("prefab_overrides", action="revert", scene=SCENE, entity=b_lid, component="transform", properties=["scale"])
    ed.check(close_to(await get_vec3(ed, b_lid, "Transform", "scale"), [1, 1, 1]), "revert did not restore the value")
    ed.check_eq(await overrides(ed, b), [], "revert left the override")
    await undo(ed)
    ed.check(close_to(await get_vec3(ed, b_lid, "Transform", "scale"), [2, 2, 2]), "undo of revert")
    ed.check(find_override(await overrides(ed, b), "property", properties=["scale"]), "undo of revert lost the override")

    # errors: wrong target, selector that matches nothing
    await ed.check_error("prefab_overrides", action="apply", scene=SCENE, entity=b_lid, component="transform",
                         target="prefabs/tmp/none.prefab", contains="target")
    await ed.check_error("prefab_overrides", action="revert", scene=SCENE, entity=b_lid, component="sprite",
                         contains="matches no override")

    # apply the listed entry as is: every instance follows, the override goes
    entry = find_override(await overrides(ed, b), "property", properties=["scale"])
    r = await ed.call("prefab_overrides", action="apply", scene=SCENE, overrides=[entry])
    ed.check_eq(r["operations"][0]["target"], "prefabs/Crate.prefab")
    ed.check(close_to(await get_vec3(ed, a_lid, "Transform", "scale"), [2, 2, 2]), "apply did not reach the other instance")
    ed.check_eq(await overrides(ed, b), [], "apply left the override")
    i = await ed.call("get_prefab_info", prefab="prefabs/Crate.prefab", usage=False)
    ed.check(i["unsaved_changes"], "applied prefab should be modified in memory until the scene is saved")
    await undo(ed)
    ed.check(close_to(await get_vec3(ed, a_lid, "Transform", "scale"), [1, 1, 1]), "undo of apply: other instance")
    ed.check(find_override(await overrides(ed, b), "property", properties=["scale"]), "undo of apply: override back")
    await redo(ed)
    ed.check(close_to(await get_vec3(ed, a_lid, "Transform", "scale"), [2, 2, 2]), "redo of apply")


async def test_other_override_kinds(ed: Editor):
    b, b_lid = S["b"], S["b_lid"]
    # state (name), added component, added GameObject, removed GameObject
    await ed.call("rename_entity", scene=SCENE, entity=b_lid, name="Lid Renamed")
    await ed.call("add_component", scene=SCENE, entity=b_lid, component="AudioSourceComponent")
    flag = await make_object(ed, "Flag", "box", parent=b)
    ov = await overrides(ed, b)
    ed.check(find_override(ov, "state", state=["name"]), f"name state override missing: {ov}")
    added_comp = [e for e in ov if e["kind"] == "added_component"]
    ed.check(added_comp, f"added component missing: {ov}")
    ed.check(find_override(ov, "added_gameobject"), f"added GameObject missing: {ov}")

    # revert all of one GameObject's overrides in one call (state + added component)
    await ed.call("prefab_overrides", action="revert", scene=SCENE, entity=b_lid)
    ov = await overrides(ed, b)
    ed.check_eq([e["kind"] for e in ov], ["added_gameobject"], "reverting the Lid's overrides")
    ed.check_eq((await info(ed, b_lid))["entity"]["name"], "Lid", "name not reverted")

    # apply the added GameObject: Crate (and instance A) get a Flag
    await ed.call("prefab_overrides", action="apply", scene=SCENE, entity=flag, kind="added_gameobject")
    ed.check_eq((await ed.call("get_prefab_info", prefab="prefabs/Crate.prefab", usage=False))["gameobject_count"], 3)
    ed.check((await info(ed, "Crate/Flag"))["owner"]["prefab"] == "prefabs/Crate.prefab", "A did not get the Flag")
    await undo(ed)
    ed.check_eq((await ed.call("get_prefab_info", prefab="prefabs/Crate.prefab", usage=False))["gameobject_count"], 2)

    # revert the added GameObject (deletes it), undo
    await ed.call("prefab_overrides", action="revert", scene=SCENE, entity=flag, kind="added_gameobject")
    ed.check_eq(await overrides(ed, b), [], "added GameObject not reverted")
    await undo(ed)
    ed.check(find_override(await overrides(ed, b), "added_gameobject"), "undo of the added GameObject's revert")
    await ed.call("prefab_overrides", action="revert", scene=SCENE, entity=flag, kind="added_gameobject")

    # removed GameObject: delete B's Lid, it is listed; revert by name brings it back
    await delete(ed, b_lid)
    ov = await overrides(ed, b)
    rem = find_override(ov, "removed_gameobject", name="Lid")
    ed.check(rem is not None, f"removed GameObject missing: {ov}")
    ed.check_eq(rem["prefab"], "prefabs/Crate.prefab")
    await ed.call("prefab_overrides", action="revert", scene=SCENE, entity=b, kind="removed_gameobject", name="Lid")
    ed.check_eq(await overrides(ed, b), [], "removed GameObject not reverted")
    S["b_lid"] = (await info(ed, "Crate B/Lid"))["entity"]["id"]

    # revert all / apply all
    await set_vec3(ed, S["b_lid"], "Transform", "position", [0, 3, 0])
    await ed.call("rename_entity", scene=SCENE, entity=S["b_lid"], name="Top")
    r = await ed.call("prefab_overrides", action="revert", scene=SCENE, entity=b, all=True)
    ed.check_eq(r["remaining"], 0)
    await undo(ed)
    ed.check_eq(len(await overrides(ed, b)), 2, "undo of revert all")
    r = await ed.call("prefab_overrides", action="apply", scene=SCENE, entity=b, all=True)
    ed.check_eq(r["remaining"], 0)
    ed.check_eq((await info(ed, "Crate/Top"))["entity"]["id"], S["lid"], "apply all: A's Lid renamed by the prefab")
    await undo(ed)
    ed.check_eq((await info(ed, S["lid"]))["entity"]["name"], "Lid", "undo of apply all")
    await ed.call("prefab_overrides", action="revert", scene=SCENE, entity=b, all=True)


async def test_variants(ed: Editor):
    b = S["b"]
    # B gets an override, then becomes an instance of a new variant that takes it
    await set_vec3(ed, b, "Transform", "scale", [3, 3, 3])
    ed.check(find_override(await overrides(ed, b), "property", properties=["scale"]), "root scale override")
    v = await ed.call("create_prefab_variant", scene=SCENE, entity=b)
    ed.check_eq(v["path"], "prefabs/Crate Variant.prefab")
    ed.check_eq(v["base"], "prefabs/Crate.prefab")
    i = await info(ed, b)
    ed.check_eq(i["prefab"]["path"], "prefabs/Crate Variant.prefab")
    ed.check_eq(i["prefab"]["variant_chain"], ["prefabs/Crate Variant.prefab", "prefabs/Crate.prefab"])
    ed.check_eq(i["override_count"], 0, "the variant took the instance's overrides")
    ed.check(close_to(await get_vec3(ed, b, "Transform", "scale"), [3, 3, 3]), "creating a variant changed the instance")
    # undo: B is an instance of Crate again with its override
    await undo(ed)
    i = await info(ed, b)
    ed.check_eq(i["prefab"]["path"], "prefabs/Crate.prefab", "undo of create variant")
    ed.check(find_override(i["overrides"], "property", properties=["scale"]), "undo of create variant: override back")
    await redo(ed)
    ed.check_eq((await info(ed, b))["prefab"]["path"], "prefabs/Crate Variant.prefab", "redo of create variant")

    # a variant of the variant, from the asset
    v2 = await ed.call("create_prefab_variant", base="prefabs/Crate Variant.prefab", path="prefabs/Crate Variant 2.prefab")
    ed.check_eq(v2["base"], "prefabs/Crate Variant.prefab")
    a2 = await ed.call("get_prefab_info", prefab="prefabs/Crate Variant 2.prefab")
    ed.check_eq(a2["variant_chain"], ["prefabs/Crate Variant 2.prefab", "prefabs/Crate Variant.prefab", "prefabs/Crate.prefab"])
    ed.check_in("prefabs/Crate Variant 2.prefab", (await ed.call("get_prefab_info", prefab="prefabs/Crate Variant.prefab"))["variants"])
    c = (await ed.call("instantiate_prefab", scene=SCENE, prefab="prefabs/Crate Variant 2.prefab", position=[-5, 0, 0], name="Crate C"))["instance"]["id"]
    ci = await info(ed, c)
    ed.check_eq(ci["nesting"][-1]["levels"], a2["variant_chain"], "levels of a variant-of-variant instance")
    ed.check(close_to(await get_vec3(ed, c, "Transform", "scale"), [3, 3, 3]), "variant 2 inherits the variant's scale")

    # an override on C's Lid: apply targets along the chain, apply to the base
    c_lid = (await info(ed, "Crate C/Lid"))["entity"]["id"]
    await set_vec3(ed, c_lid, "Transform", "position", [0, 4, 0])
    e = find_override(await overrides(ed, c), "property", properties=["position"])
    targets = [t["prefab"] for t in e["apply_targets"]]
    ed.check_eq(sorted(targets), sorted(a2["variant_chain"]), "apply targets of a chain")
    labels = " | ".join(t["label"] for t in e["apply_targets"])
    ed.check_in("Base Prefab", labels, "labels of chain targets")
    await ed.call("prefab_overrides", action="apply", scene=SCENE, entity=c_lid, component="transform",
                  properties=["position"], target="prefabs/Crate.prefab")
    ed.check(close_to(await get_vec3(ed, S["lid"], "Transform", "position"), [0, 4, 0]), "apply to base reached a base instance")
    ed.check_eq(await overrides(ed, c), [], "apply to base left the override")
    await undo(ed)
    ed.check(close_to(await get_vec3(ed, S["lid"], "Transform", "position"), [0, 1, 0]), "undo of apply to base")
    await ed.call("prefab_overrides", action="revert", scene=SCENE, entity=c, all=True)
    S.update(c=c)


async def test_prefab_mode(ed: Editor):
    a_lid = S["lid"]
    # A overrides its Lid's position; a fresh instance D does not
    await set_vec3(ed, a_lid, "Transform", "position", [0, 7, 0])
    d = (await ed.call("instantiate_prefab", scene=SCENE, prefab="prefabs/Crate.prefab", position=[0, 0, 5], name="Crate D"))["instance"]["id"]
    d_lid = (await info(ed, "Crate D/Lid"))["entity"]["id"]

    st = await ed.call("prefab_mode", action="open", prefab="prefabs/Crate.prefab", auto_save=False)
    stage = st["scene_id"]
    ed.check(not st["in_context"], "opened by path should be isolation")
    ed.check_eq(st["root"]["name"], "Crate")
    status = await ed.call("prefab_mode", action="status")
    ed.check(status["active"] and status["shown_stage_scene_id"] == stage, f"status: {status}")
    # the stage is a scene other tools address by its id; paths start at the prefab root
    si = await info(ed, "Crate/Lid", scene=stage)
    ed.check(si["is_prefab_asset_object"], f"stage GameObject: {si}")
    await set_vec3(ed, "Crate/Lid", "Transform", "position", [0, 2, 0], scene=stage)
    ed.check((await ed.call("prefab_mode", action="status"))["stages"][0]["modified"], "stage not modified after an edit")
    ed.check(close_to(await get_vec3(ed, d_lid, "Transform", "position"), [0, 1, 0]), "unsaved stage reached instances")
    saved = await ed.call("prefab_mode", action="save")
    ed.check(not saved["modified"], "still modified after save")
    ed.check(close_to(await get_vec3(ed, d_lid, "Transform", "position"), [0, 2, 0]), "Prefab Mode edit did not reach instance D")
    ed.check(close_to(await get_vec3(ed, a_lid, "Transform", "position"), [0, 7, 0]), "Prefab Mode edit overwrote A's override")
    doc = prefab_file(ed, "prefabs/Crate.prefab")
    lid_node = prefab_member(doc, "Lid")
    ed.check(close_to(lid_node["components"]["transform"]["position"], [0, 2, 0]), f"file not saved: {lid_node}")

    # undo in the stage's own history, save again
    await undo(ed, scene=stage)
    await ed.call("prefab_mode", action="save", scene=stage)
    ed.check(close_to(await get_vec3(ed, d_lid, "Transform", "position"), [0, 1, 0]), "stage undo + save")

    # Auto Save: an edit is written a moment later with no save call
    await ed.call("prefab_mode", action="status", auto_save=True)
    await set_vec3(ed, "Crate/Lid", "Transform", "position", [0, 5, 0], scene=stage)
    for _ in range(50):
        await asyncio.sleep(0.2)
        if close_to(await get_vec3(ed, d_lid, "Transform", "position"), [0, 5, 0]):
            break
    else:
        ed.fail("Auto Save did not write the stage")

    # unsaved changes and close: discard keeps the asset as saved
    await ed.call("prefab_mode", action="status", auto_save=False)
    await set_vec3(ed, "Crate/Lid", "Transform", "position", [0, 9, 0], scene=stage)
    await ed.check_error("prefab_mode", action="close", unsaved="cancel", contains="unsaved")
    closed = await ed.call("prefab_mode", action="close", unsaved="discard")
    ed.check(not closed["active"], "still in Prefab Mode")
    ed.check(close_to(await get_vec3(ed, d_lid, "Transform", "position"), [0, 5, 0]), "discarded edit reached instances")

    # in context, from an instance's member; GameObjects added in the stage reach the instances
    st = await ed.call("prefab_mode", action="open", scene=SCENE, entity=d_lid)
    ed.check(st["in_context"] and st["context_scene_id"] == SCENE, f"in context: {st}")
    ed.check_eq(st["context_root"]["id"], d)
    stage = st["scene_id"]
    await make_object(ed, "Handle", "box", parent="Crate", scene=stage)
    await ed.call("prefab_mode", action="close", unsaved="save")
    ed.check((await info(ed, "Crate D/Handle"))["owner"]["prefab"] == "prefabs/Crate.prefab", "added in Prefab Mode: D")
    ed.check((await info(ed, "Crate/Handle"))["owner"]["prefab"] == "prefabs/Crate.prefab", "added in Prefab Mode: A")
    ed.check_eq((await ed.call("prefab_mode", action="status"))["active"], False)
    S.update(d=d)


async def test_variant_prefab_mode(ed: Editor):
    """In a variant's Prefab Mode its overrides of the base are instance overrides of the stage root."""
    b, c = S["b"], S["c"]
    st = await ed.call("prefab_mode", action="open", prefab="prefabs/Crate Variant.prefab", auto_save=False)
    stage, root = st["scene_id"], st["root"]["id"]
    ed.check_eq(st["variant_of"], "prefabs/Crate.prefab")
    ov = await overrides(ed, root, scene=stage)
    e = find_override(ov, "property", component="transform", properties=["scale"])
    ed.check(e is not None, f"the variant's scale override is not listed in its Prefab Mode: {ov}")
    ed.check_eq([t["prefab"] for t in e["apply_targets"]], ["prefabs/Crate.prefab"], "apply targets in a variant stage")
    # revert in the stage = back to the base value; saved, every variant instance follows
    await ed.call("prefab_overrides", action="revert", scene=stage, entity=root, component="transform", properties=["scale"])
    ed.check(close_to(await get_vec3(ed, root, "Transform", "scale", scene=stage), [1, 1, 1]), "revert in the variant stage")
    await ed.call("prefab_mode", action="save")
    ed.check(close_to(await get_vec3(ed, b, "Transform", "scale"), [1, 1, 1]), "variant instance after the variant's revert")
    ed.check(close_to(await get_vec3(ed, c, "Transform", "scale"), [1, 1, 1]), "variant-of-variant instance after the variant's revert")
    await undo(ed, scene=stage)
    await ed.call("prefab_mode", action="close", unsaved="save")
    ed.check(close_to(await get_vec3(ed, b, "Transform", "scale"), [3, 3, 3]), "stage undo + save on close")
    status = await ed.call("editor_status")
    ed.check(status.get("dialog") is None, f"a modal dialog was left open: {status.get('dialog')}")


async def test_unpack_and_replace(ed: Editor):
    d, c = S["d"], S["c"]
    r = await ed.call("unpack_prefab", scene=SCENE, entity=d)
    ed.check(r["instances"][0]["now_instance_of"] is None, f"unpack: {r}")
    ed.check(not (await info(ed, d))["in_prefab_instance"], "still an instance after unpack")
    await undo(ed)
    ed.check((await info(ed, d))["is_instance_root"], "undo of unpack")

    # a variant-of-variant instance: one level -> instance of its base; completely -> plain
    r = await ed.call("unpack_prefab", scene=SCENE, entity=c)
    ed.check_eq(r["instances"][0]["now_instance_of"], "prefabs/Crate Variant.prefab", "unpack one level of a variant")
    await undo(ed)
    ed.check_eq((await info(ed, c))["prefab"]["path"], "prefabs/Crate Variant 2.prefab", "undo of variant unpack")
    await ed.call("unpack_prefab", scene=SCENE, entity="Crate C/Lid", completely=True)
    ci = await info(ed, c)
    ed.check(not ci["in_prefab_instance"], "unpack completely")
    await undo(ed)
    ed.check((await info(ed, c))["is_instance_root"], "undo of unpack completely")

    # replace D's prefab by the variant (keeping matched overrides), then undo
    await set_vec3(ed, "Crate D/Lid", "Transform", "scale", [4, 4, 4])
    r = await ed.call("replace_prefab", scene=SCENE, entity=d, prefab="prefabs/Crate Variant.prefab")
    di = await info(ed, d)
    ed.check_eq(di["prefab"]["path"], "prefabs/Crate Variant.prefab", "replace")
    ed.check(find_override(di["overrides"], "property", properties=["scale"]), "replace kept the matched override")
    await undo(ed)
    ed.check_eq((await info(ed, d))["prefab"]["path"], "prefabs/Crate.prefab", "undo of replace")
    await ed.check_error("replace_prefab", scene=SCENE, entity="Crate D/Lid", prefab="prefabs/Crate Variant.prefab",
                         contains="Nothing to replace")


async def test_add_and_remove_from_prefab(ed: Editor):
    a = S["crate"]
    count = (await ed.call("get_prefab_info", prefab="prefabs/Crate.prefab", usage=False))["gameobject_count"]
    extra = await make_object(ed, "Extra", "box", parent=a)
    await ed.call("add_to_prefab", scene=SCENE, entity=extra)
    ed.check_eq((await ed.call("get_prefab_info", prefab="prefabs/Crate.prefab", usage=False))["gameobject_count"], count + 1)
    ed.check((await info(ed, "Crate D/Extra"))["owner"]["prefab"] == "prefabs/Crate.prefab", "D did not get Extra")
    await undo(ed)
    ed.check_eq((await ed.call("get_prefab_info", prefab="prefabs/Crate.prefab", usage=False))["gameobject_count"], count)
    await redo(ed)

    await ed.check_error("remove_from_prefab", scene=SCENE, entity=a, contains="instance root")
    await ed.call("remove_from_prefab", scene=SCENE, entity="Crate/Extra")
    ed.check_eq((await ed.call("get_prefab_info", prefab="prefabs/Crate.prefab", usage=False))["gameobject_count"], count)
    xi = await info(ed, "Crate/Extra")
    ed.check(xi["entity"]["name"] == "Extra" and xi.get("owner") is None, f"Extra should stay here as a plain GameObject: {xi}")
    await undo(ed)
    ed.check_eq((await ed.call("get_prefab_info", prefab="prefabs/Crate.prefab", usage=False))["gameobject_count"], count + 1)


async def test_standalone(ed: Editor):
    await ed.call("set_standalone_prefab", prefab="prefabs/Crate Variant 2.prefab", standalone=True)
    lst = await ed.call("list_prefabs", filter="variant 2")
    ed.check(lst["prefabs"][0].get("standalone"), f"not standalone: {lst}")
    ed.check_in("Crate Variant 2.prefab", (ed.project / "project.yaml").read_text(), "project.yaml")
    await ed.call("set_standalone_prefab", prefab="prefabs/Crate Variant 2.prefab", standalone=False)
    ed.check(not (await ed.call("list_prefabs", filter="variant 2"))["prefabs"][0].get("standalone"), "still standalone")


async def test_save_and_reload(ed: Editor):
    b_lid = (await info(ed, "Crate B/Lid"))["entity"]["id"]
    await set_vec3(ed, b_lid, "Transform", "scale", [6, 6, 6])
    await ed.call("save_scene", scene=SCENE)
    async with Editor(project=ed.project) as ed2:
        i = await ed2.call("get_prefab_info", scene=SCENE, entity="Crate B")
        ed2.check_eq(i["prefab"]["path"], "prefabs/Crate Variant.prefab", "reloaded instance prefab")
        e = find_override(i["overrides"], "property", properties=["scale"])
        ed2.check(e is not None and e["entity"]["path"] == "Crate B/Lid", f"override lost on reload: {i['overrides']}")
        ed2.check(close_to(await get_vec3(ed2, "Crate B/Lid", "Transform", "scale"), [6, 6, 6]), "overridden value lost on reload")
        ed2.check(close_to(await get_vec3(ed2, "Crate D/Lid", "Transform", "position"), [0, 5, 0]), "Prefab Mode edit lost on reload")
        ed2.check(close_to(await get_vec3(ed2, "Crate/Lid", "Transform", "position"), [0, 7, 0]), "A's override lost on reload")
        lst = await ed2.call("list_prefabs")
        paths = [p["path"] for p in lst["prefabs"]]
        for p in ("prefabs/Crate.prefab", "prefabs/Crate Variant.prefab", "prefabs/Crate Variant 2.prefab"):
            ed2.check_in(p, paths, "prefab after reload")
        v2 = await ed2.call("get_prefab_info", prefab="prefabs/Crate Variant 2.prefab")
        ed2.check_eq(v2["variant_chain"][-1], "prefabs/Crate.prefab", "variant chain after reload")
