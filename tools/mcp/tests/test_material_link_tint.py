"""A value set on one field of a submesh linked to a .material file survives the scene's reopening.

A linked submesh takes every field of its .material file when its scene opens (and follows the
file's edits), so a tint set on it alone (set_properties submeshes[N].material.baseColorFactor)
used to be saved, then replaced by the file's colour on the next open: lost, on origin/main as
well (tools/ci/upgrade_from_main found it). As an inline edit of the whole material and a material
preset already did, an edit of one of its fields makes it the GameObject's own material: unlinked,
the file untouched, the other submeshes linked to the file still following it. Round trips: a plain
GameObject and a prefab instance (an instance override), saved, closed and opened again.
"""

from __future__ import annotations

from harness import Editor, find_key

CI = True
EDITOR = {"create": True, "name": "Material Link Tint"}

MATERIAL = "assets/materials/Blue.material"
BLUE = [0.1, 0.3, 0.9, 1.0]
RED = [0.9, 0.1, 0.1, 1.0]
COLOR = "submeshes[0].material.baseColorFactor"
LINK = "submeshes[0].material"


async def make_cube(ed: Editor, scene: str, name: str, position: list[float]):
    made = await ed.call("create_entity", scene=scene, name=name, type="cube", position=position)
    eid = find_key(made, "entity_id", "id")
    ed.check(eid is not None, f"create_entity returned no id: {made!r}")
    return eid


async def color(ed: Editor, scene: str, entity) -> list[float]:
    got = await ed.call("get_entity", scene=scene, entity=entity, components=["mesh"], properties=[COLOR], detail="values")
    return [round(v, 4) for v in got["components"]["mesh"][COLOR]]


async def linked_file(ed: Editor, scene: str, entity):
    got = await ed.call("get_entity", scene=scene, entity=entity, components=["mesh"], properties=[LINK], detail="values")
    return find_key(got["components"]["mesh"][LINK], "file")


async def reopen(ed: Editor, scene: str) -> None:
    await ed.call("save_scene", scene=scene)
    # the last open scene cannot close: another one holds the editor meanwhile
    await ed.call("create_scene", name="Holder", type="3D", unsaved="discard")
    await ed.call("close_scene", scene=scene, unsaved="cancel")
    await ed.call("open_scene", scene=scene)


async def test_tint_survives_reopen(ed: Editor):
    scene = "scenes/Tint.scene"
    await ed.call("create_scene", path=scene, name="Tint", type="3D")
    await ed.call("create_material", path=MATERIAL, values={"baseColorFactor": BLUE})
    plain = await make_cube(ed, scene, "Plain", [0, 0, 0])
    shared = await make_cube(ed, scene, "Shared", [2, 0, 0])
    for entity in (plain, shared):
        await ed.call("link_material", scene=scene, entity=entity, material=MATERIAL)

    await ed.call("set_properties", scene=scene, entity=plain, values={"mesh": {COLOR: RED}})
    ed.check_eq(await color(ed, scene, plain), RED, "the tint is set")
    ed.check(not await linked_file(ed, scene, plain), "a field edit makes the submesh its own material (unlinked)")
    ed.check_eq(await linked_file(ed, scene, shared), MATERIAL, "the other submesh stays linked")

    await reopen(ed, scene)
    ed.check_eq(await color(ed, scene, "Plain"), RED, "the tint survives the scene's reopening")
    ed.check_eq(await color(ed, scene, "Shared"), BLUE, "the linked submesh keeps the file's colour")
    # the file is untouched, and its edits still reach what is linked to it
    await ed.call("update_material", path=MATERIAL, values={"baseColorFactor": [0.2, 0.8, 0.2, 1.0]})
    await ed.call("wait", frames=1)
    ed.check_eq(await color(ed, scene, "Shared"), [0.2, 0.8, 0.2, 1.0], "the linked submesh follows the file")
    ed.check_eq(await color(ed, scene, "Plain"), RED, "the unlinked one does not")


async def test_instance_tint_survives_reopen(ed: Editor):
    scene = "scenes/TintPrefab.scene"
    await ed.call("create_scene", path=scene, name="TintPrefab", type="3D")
    await ed.call("create_material", path=MATERIAL, values={"baseColorFactor": BLUE}, overwrite=True)
    gem = await make_cube(ed, scene, "Gem", [0, 0, 0])
    await ed.call("link_material", scene=scene, entity=gem, material=MATERIAL)
    await ed.call("create_prefab", scene=scene, entity=gem, path="prefabs/Gem.prefab")
    await ed.call("instantiate_prefab", scene=scene, prefab="prefabs/Gem.prefab", name="Gem B", position=[3, 0, 0])

    # an instance override of the tint: Gem B red, the prefab and Gem blue
    await ed.call("set_properties", scene=scene, entity="Gem B", values={"mesh": {COLOR: RED}})
    await reopen(ed, scene)
    ed.check_eq(await color(ed, scene, "Gem B"), RED, "the instance's tint survives the reopening")
    ed.check_eq(await color(ed, scene, "Gem"), BLUE, "the other instance keeps the prefab's linked material")
    info = await ed.call("get_prefab_info", scene=scene, entity="Gem B")
    ed.check("material" in str(find_key(info, "overrides")), f"the tint is an override of the instance: {info}")
