""".mesh assets (docs/specs/S98-editor-tooling-gaps.md F1, ledger E1: scene files embedded mesh buffers).

- test_scene_save_moves_large_meshes_into_mesh_assets: a scene save writes a sphere's geometry to
  "<scene folder>/<scene name>/<GameObject>.mesh" and the scene names it instead of holding its buffers
  (a box stays inline); get_asset_info reads the file; the reopened scene shows the geometry again.
- test_save_as_is_one_undo_step: set_mesh_geometry save_as writes the new primitive to the asset and
  shows it, one undo step; save_as alone saves the current geometry; a model's mesh is refused.
- test_a_script_assigns_a_mesh_asset: Mesh.meshAsset in Play reads the file at once.

Oracles: the files on disk, the tools' results, the console.
"""

from __future__ import annotations

from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Mesh Assets"}


def mesh_of(text: str, name: str) -> str:
    """The `mesh:` block of the GameObject `name` in a scene file's text."""
    entity = text.split(f"name: {name}\n", 1)[1].split("  - type: Entity", 1)[0]
    return entity.split("mesh:", 1)[1] if "mesh:" in entity else ""


async def test_scene_save_moves_large_meshes_into_mesh_assets(ed: Editor):
    sid = (await ed.call("create_scene", name="Shapes", type="3D", path="scenes/Shapes"))["scene"]["id"]
    await ed.call("create_entity", scene=sid, type="sphere", name="Ball")
    await ed.call("create_entity", scene=sid, type="sphere", name="Twin")
    await ed.call("create_entity", scene=sid, type="cube", name="Crate")
    await ed.call("save_scene", scene=sid)

    scene_file = ed.project / "scenes" / "Shapes.scene"
    text = scene_file.read_text()
    files = sorted(p.relative_to(ed.project).as_posix() for p in ed.project.rglob("*.mesh"))
    ed.check(len(files) == 1 and files[0].endswith("Shapes/Ball.mesh"), f"one file for the two spheres, beside the scene: {files}")
    asset = files[0]
    ed.check((ed.project / (asset + ".meta")).exists(), "the .mesh has a GUID (its .meta)")
    ball = mesh_of(text, "Ball")
    ed.check("meshAsset:" in ball and "meshAssetGuid:" in ball and "buffer:" not in ball, f"the sphere names the file: {ball[:400]}")
    ed.check("meshAsset:" in mesh_of(text, "Twin"), "its twin names the same file")
    crate = mesh_of(text, "Crate")
    ed.check("buffer:" in crate and "meshAsset:" not in crate, "a box (under 4 KB) stays in the scene")
    ed.check(text.startswith("formatVersion: 9\n"), f"scene format 9: {text[:40]}")

    info = await ed.call("get_asset_info", path=asset)
    ed.check_eq(info["type"], "mesh", f"{info}")
    ed.check(info["vertex_count"] > 100 and info["index_count"] > 0, f"the sphere's geometry: {info}")
    ed.check_eq(sorted(u["entity"]["name"] for u in info["users"]), ["Ball", "Twin"], f"who shows it: {info['users']}")
    listed = await ed.call("list_assets", pattern="*.mesh", type="mesh")
    ed.check(any(a["path"] == asset for a in listed["assets"]), f"list_assets type mesh: {listed}")

    # saved again: not a byte changes
    await ed.call("save_scene", scene=sid)
    ed.check_eq(scene_file.read_text(), text, "a second save writes the same scene")

    # reopened: the geometry comes back from the file (save_as alone needs it to be there)
    await ed.call("close_scene", scene=sid, unsaved="discard")
    sid = (await ed.call("open_scene", scene="scenes/Shapes.scene"))["scene"]["id"]
    await ed.call("wait", frames=3)
    copy = await ed.call("set_mesh_geometry", scene=sid, entity="Ball", save_as="meshes/copy.mesh")
    size = copy["local_size"]
    ed.check(all(abs(v - 2.0) < 0.01 for v in size), f"the reopened sphere is the 2 m one it was: {size}")
    ed.check((ed.project / "meshes" / "copy.mesh").read_bytes() == (ed.project / asset).read_bytes(), "the same geometry, the same bytes")


async def test_save_as_is_one_undo_step(ed: Editor):
    sid = (await ed.call("create_scene", name="SaveAs", type="3D", path="scenes/SaveAs"))["scene"]["id"]
    await ed.call("create_entity", scene=sid, type="cube", name="Pillar")
    r = await ed.call("set_mesh_geometry", scene=sid, entity="Pillar", shape="cylinder", radius=0.5, height=3, save_as="meshes/pillar.mesh")
    ed.check_eq(r["mesh_asset"], "meshes/pillar.mesh", f"{r}")
    ed.check(abs(r["local_size"][1] - 3.0) < 0.01, f"the cylinder read back from the file: {r}")
    ed.check((ed.project / "meshes" / "pillar.mesh").exists(), "the file is written")

    async def shown() -> str:
        entity = await ed.call("get_entity", scene=sid, entity="Pillar", components=["mesh"])
        return entity["components"]["mesh"].get("meshAsset") or ""

    ed.check_eq(await shown(), "meshes/pillar.mesh", "the Mesh field shows it")
    await ed.call("undo", scene=sid)
    ed.check_eq(await shown(), "", "undo: the cube again, its own geometry (one step)")
    ed.check((ed.project / "meshes" / "pillar.mesh").exists(), "the file stays")
    await ed.call("redo", scene=sid)
    ed.check_eq(await shown(), "meshes/pillar.mesh", "redo")

    # a primitive without save_as makes the geometry the mesh's own again
    await ed.call("set_mesh_geometry", scene=sid, entity="Pillar", shape="box")
    ed.check_eq(await shown(), "", "a new shape lets go of the file")

    # another GameObject shows the file through its Mesh field
    await ed.call("create_entity", scene=sid, type="cube", name="Copy")
    await ed.call("set_properties", scene=sid, entity="Copy", values={"mesh": {"meshAsset": "meshes/pillar.mesh"}})
    await ed.call("wait", frames=2)
    info = await ed.call("get_asset_info", path="meshes/pillar.mesh")
    ed.check_eq([u["entity"]["name"] for u in info["users"]], ["Copy"], f"set_properties shows it: {info['users']}")

    bad = await ed.call("set_mesh_geometry", scene=sid, entity="Pillar", save_as="meshes/pillar.glb", expect_error=True)
    ed.check(".mesh" in bad, f"another extension is refused with the fix: {bad}")


async def test_a_script_assigns_a_mesh_asset(ed: Editor):
    sid = (await ed.call("create_scene", name="Script", type="3D", path="scenes/Script"))["scene"]["id"]
    await ed.call("create_entity", scene=sid, type="sphere", name="Source")
    await ed.call("set_mesh_geometry", scene=sid, entity="Source", shape="torus", save_as="meshes/ring.mesh")
    await ed.call("create_entity", scene=sid, type="cube", name="Target")
    await ed.call("play", action="start")
    try:
        r = await ed.call("eval_lua", code="local m = object('Target', 'Mesh'); m.meshAsset = 'meshes/ring.mesh'; "
                                            "return {asset = m.meshAsset, width = m:getAABB():getSize().x}")
    finally:
        await ed.call("play", action="stop")
    value = r["values"][0]
    ed.check_eq(value["asset"], "meshes/ring.mesh", f"{r}")
    ed.check(abs(value["width"] - 3.0) < 0.01, f"the torus (radius 1, ring 0.5) at once: {r}")
