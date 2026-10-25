"""Prefab MCP transactions on authored two-object assets, independent of sample games."""

import asyncio
import time

from harness import Editor

EDITOR = {"create": True, "name": "Prefab contracts"}


async def fixture(ed, name):
    sid = (await ed.call("create_scene", name=name, type="3D", path=f"scenes/{name}"))["scene"]["id"]
    root = (await ed.call("create_entity", scene=sid, type="empty", name="Root", scale=[1, 1, 1]))["entity_id"]
    child = (await ed.call("create_entity", scene=sid, type="empty", name="Child", parent=root, position=[0, 1, 0]))["entity_id"]
    path = f"prefabs/{name}.prefab"
    await ed.call("create_prefab", scene=sid, entity=root, path=path)
    other = (await ed.call("instantiate_prefab", scene=sid, prefab=path, name="Other", position=[5, 0, 0]))["instance"]["id"]
    return sid, root, child, other, path


async def value(ed, sid, entity, name):
    out = await ed.call("get_entity", scene=sid, entity=entity, components=["transform"], properties=[name])
    return out["components"]["transform"][name]


async def edit(ed, sid, entity, name, data):
    await ed.call("set_properties", scene=sid, entity=entity, values={"transform": {name: data}})


async def info(ed, sid, entity):
    return await ed.call("get_prefab_info", scene=sid, entity=entity, usage=False)


async def test_property_override_apply_revert(ed: Editor):
    sid, root, child, other, path = await fixture(ed, "Properties")
    ed.check_eq(await value(ed, sid, other, "position"), [5, 0, 0], "instantiation applies the authored placement")
    await edit(ed, sid, "Other/Child", "scale", [2, 2, 2])
    await ed.call("prefab_overrides", scene=sid, entity="Other/Child", action="apply", component="transform", properties=["scale"])
    ed.check_eq(await value(ed, sid, child, "scale"), [2, 2, 2], "apply reaches another instance")
    ed.check_eq((await info(ed, sid, other))["overrides"], [])
    await ed.call("undo", scene=sid)
    ed.check_eq(await value(ed, sid, child, "scale"), [1, 1, 1])
    ed.check((await info(ed, sid, other))["overrides"], "undo restores the local override")
    await ed.call("redo", scene=sid)
    ed.check_eq(await value(ed, sid, child, "scale"), [2, 2, 2])
    await edit(ed, sid, "Other/Child", "scale", [3, 3, 3])
    await ed.call("prefab_overrides", scene=sid, entity="Other/Child", action="revert", component="transform", properties=["scale"])
    ed.check_eq(await value(ed, sid, "Other/Child", "scale"), [2, 2, 2], "revert uses the current asset value")
    await ed.call("undo", scene=sid)
    ed.check_eq(await value(ed, sid, "Other/Child", "scale"), [3, 3, 3], "revert is undoable")


async def test_other_override_kinds(ed: Editor):
    sid, root, child, other, path = await fixture(ed, "Structure")
    await ed.call("add_component", scene=sid, entity="Other/Child", component="audiosource")
    await ed.call("prefab_overrides", scene=sid, entity="Other/Child", action="apply", kind="added_component")
    got = await ed.call("get_entity", scene=sid, entity=child, detail="summary")
    ed.check_in("audiosource", got["components"], "component application reaches the other instance")
    await ed.call("undo", scene=sid)
    got = await ed.call("get_entity", scene=sid, entity=child, detail="summary")
    ed.check("audiosource" not in got["components"], "undo restores the asset and other instance")
    await ed.call("prefab_overrides", scene=sid, entity="Other/Child", action="revert", kind="added_component")
    flag = (await ed.call("create_entity", scene=sid, type="empty", name="Flag", parent=other))["entity_id"]
    await ed.call("prefab_overrides", scene=sid, entity=flag, action="apply", kind="added_gameobject")
    ed.check_eq((await info(ed, sid, "Root/Flag"))["owner"]["prefab"], path)
    await ed.call("undo", scene=sid)
    await ed.call("prefab_overrides", scene=sid, entity=flag, action="revert", kind="added_gameobject")
    await ed.check_error("get_entity", scene=sid, entity="Other/Flag", contains="No GameObject")
    await ed.call("delete_entities", scene=sid, entity="Other/Child")
    removed = (await info(ed, sid, other))["overrides"]
    ed.check(any(item["kind"] == "removed_gameobject" and item["name"] == "Child" for item in removed))
    await ed.call("prefab_overrides", scene=sid, entity=other, action="revert", kind="removed_gameobject", name="Child")
    ed.check_eq(await value(ed, sid, "Other/Child", "position"), [0, 1, 0])
    extra = (await ed.call("create_entity", scene=sid, type="empty", name="Extra", parent=root))["entity_id"]
    await ed.call("add_to_prefab", scene=sid, entity=extra)
    ed.check_eq((await info(ed, sid, "Other/Extra"))["owner"]["prefab"], path)
    await ed.call("remove_from_prefab", scene=sid, entity=extra)
    await ed.check_error("get_entity", scene=sid, entity="Other/Extra", contains="No GameObject")
    ed.check(not (await info(ed, sid, extra)).get("owner"), "removing membership leaves the original object as ordinary scene content")
    await ed.call("undo", scene=sid)
    ed.check_eq((await info(ed, sid, "Other/Extra"))["owner"]["prefab"], path)


async def test_variants(ed: Editor):
    sid, root, child, other, path = await fixture(ed, "Variants")
    await edit(ed, sid, other, "scale", [3, 3, 3])
    variant = (await ed.call("create_prefab_variant", scene=sid, entity=other, path="prefabs/Variant.prefab"))["path"]
    await ed.call("create_prefab_variant", base=variant, path="prefabs/Variant2.prefab")
    nested = (await ed.call("instantiate_prefab", scene=sid, prefab="prefabs/Variant2.prefab", name="Nested"))["instance"]["id"]
    ed.check_eq((await info(ed, sid, nested))["prefab"]["variant_chain"], ["prefabs/Variant2.prefab", variant, path])
    ed.check_eq(await value(ed, sid, nested, "scale"), [3, 3, 3])
    await edit(ed, sid, "Nested/Child", "position", [0, 4, 0])
    await ed.call("prefab_overrides", scene=sid, entity="Nested/Child", action="apply", component="transform", properties=["position"], target=path)
    ed.check_eq(await value(ed, sid, child, "position"), [0, 4, 0], "targeting the base reaches a base instance")
    opened = await ed.call("prefab_mode", action="open", prefab=variant, auto_save=False)
    stage, root_in_stage = opened["scene_id"], opened["root"]["id"]
    await ed.call("prefab_overrides", scene=stage, entity=root_in_stage, action="revert", component="transform", properties=["scale"])
    await ed.call("prefab_mode", action="close", unsaved="save")
    ed.check_eq(await value(ed, sid, other, "scale"), [1, 1, 1])
    ed.check_eq(await value(ed, sid, nested, "scale"), [1, 1, 1], "variant-of-variant follows a saved variant stage")
    await ed.call("set_standalone_prefab", prefab="prefabs/Variant2.prefab", standalone=True)
    await ed.call("save_scene", scene=sid)
    async with Editor(project=ed.project) as reopened:
        loaded = (await reopened.call("open_scene", scene="scenes/Variants.scene"))["scene"]["id"]
        reopened.check_eq(await value(reopened, loaded, "Nested/Child", "position"), [0, 4, 0], "applied base data survives reopening")
        reopened.check_eq(await value(reopened, loaded, "Nested", "scale"), [1, 1, 1])
        assets = (await reopened.call("list_prefabs", filter="Variant2"))["prefabs"]
        reopened.check(assets[0]["standalone"], "standalone export eligibility persists in project settings")


async def test_prefab_mode(ed: Editor):
    sid, root, child, other, path = await fixture(ed, "Stage")
    await edit(ed, sid, child, "position", [0, 7, 0])
    opened = await ed.call("prefab_mode", action="open", prefab=path, auto_save=False)
    stage = opened["scene_id"]
    await edit(ed, stage, "Root/Child", "position", [0, 2, 0])
    ed.check_eq(await value(ed, sid, "Other/Child", "position"), [0, 1, 0], "unsaved edits do not reach instances")
    await ed.call("prefab_mode", action="save")
    ed.check_eq(await value(ed, sid, "Other/Child", "position"), [0, 2, 0])
    ed.check_eq(await value(ed, sid, child, "position"), [0, 7, 0], "saving preserves an instance override")
    await ed.call("prefab_mode", action="status", auto_save=True)
    await edit(ed, stage, "Root/Child", "position", [0, 5, 0])
    deadline = time.monotonic() + 10
    while await value(ed, sid, "Other/Child", "position") != [0, 5, 0]:
        ed.check(time.monotonic() < deadline, "auto save did not update the instance")
        await asyncio.sleep(0.1)
    await ed.call("prefab_mode", action="status", auto_save=False)
    await edit(ed, stage, "Root/Child", "position", [0, 9, 0])
    await ed.check_error("prefab_mode", action="close", unsaved="cancel", contains="unsaved")
    await ed.call("prefab_mode", action="close", unsaved="discard")
    ed.check_eq(await value(ed, sid, "Other/Child", "position"), [0, 5, 0], "discard leaves the saved asset intact")
    ed.check_eq(await value(ed, sid, child, "position"), [0, 7, 0])


async def test_unpack_and_replace(ed: Editor):
    sid, root, child, other, path = await fixture(ed, "Replacement")
    await edit(ed, sid, other, "scale", [3, 3, 3])
    variant = (await ed.call("create_prefab_variant", scene=sid, entity=other, path="prefabs/ReplacementVariant.prefab"))["path"]
    await edit(ed, sid, child, "scale", [4, 4, 4])
    await ed.call("replace_prefab", scene=sid, entity=root, prefab=variant)
    ed.check_eq((await info(ed, sid, root))["prefab"]["path"], variant)
    ed.check_eq(await value(ed, sid, child, "scale"), [4, 4, 4], "replacement preserves a matched override")
    await ed.call("undo", scene=sid)
    ed.check_eq((await info(ed, sid, root))["prefab"]["path"], path)
    await ed.call("unpack_prefab", scene=sid, entity=other)
    ed.check_eq((await info(ed, sid, other))["prefab"]["path"], path, "one-level unpack follows the base")
    await ed.call("unpack_prefab", scene=sid, entity=other, completely=True)
    ed.check(not (await info(ed, sid, other))["in_prefab_instance"])
    ed.check_eq(await value(ed, sid, "Other/Child", "position"), [0, 1, 0], "unpack retains content")
    destination = (await ed.call("create_entity", scene=sid, type="empty", name="Destination"))["entity_id"]
    await ed.check_error("move_entity", scene=sid, entity=child, parent=destination, contains="Open the Prefab")
    ed.check_eq((await ed.call("get_entity", scene=sid, entity=child, detail="summary"))["parent"]["id"], root)
    await ed.call("move_entity", scene=sid, entity=root, parent=destination)
    ed.check_eq((await ed.call("get_entity", scene=sid, entity=root, detail="summary"))["parent"]["id"], destination)
