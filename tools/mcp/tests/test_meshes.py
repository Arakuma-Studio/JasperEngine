"""The meshes group's detail level tools (docs/specs/S54-lod-crossfade.md R14, R15, AC7):
capture_model_lods on a model file (levels built on the CPU, a contact sheet) and on a GameObject
(the renderer, a forced level per tile), its group hidden until tool_groups enables it, and the
LOD numbers of get_render_stats.

.mesh assets (docs/specs/S98-editor-tooling-gaps.md F1, ledger E1: scene files embedded mesh buffers):
- test_scene_save_moves_large_meshes_into_mesh_assets: a scene save writes a sphere's geometry to
  "<scene folder>/<scene name>/<GameObject>.mesh" and the scene names it instead of holding its buffers
  (a box stays inline); get_asset_info reads the file; the reopened scene shows the geometry again.
- test_save_as_is_one_undo_step: set_mesh_geometry save_as writes the new primitive to the asset and
  shows it, one undo step; save_as alone saves the current geometry; a model's mesh is refused.
- test_a_script_assigns_a_mesh_asset: Mesh.meshAsset in Play reads the file at once.

Runs in one new empty project; tests run in order and share the editor.
"""

from __future__ import annotations

import base64
import json
import math
import struct
from pathlib import Path

import pngdiff
from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Meshes CI"}
TIMEOUT = 900

S: dict = {}


def write_sphere_gltf(path: Path, slices: int = 48, stacks: int = 24) -> None:
    """A UV sphere of radius 1 with normals, one primitive, an embedded buffer."""
    positions: list[float] = []
    normals: list[float] = []
    for y in range(stacks + 1):
        phi = math.pi * y / stacks
        for x in range(slices + 1):
            theta = 2.0 * math.pi * x / slices
            p = (math.sin(phi) * math.cos(theta), math.cos(phi), math.sin(phi) * math.sin(theta))
            positions.extend(p)
            normals.extend(p)
    indices: list[int] = []
    for y in range(stacks):
        for x in range(slices):
            a = y * (slices + 1) + x
            b = a + slices + 1
            if y > 0:
                indices.extend((a, b, a + 1))
            if y + 1 < stacks:
                indices.extend((a + 1, b, b + 1))
    vertex_count = len(positions) // 3
    pos_bytes = struct.pack(f"<{len(positions)}f", *positions)
    nor_bytes = struct.pack(f"<{len(normals)}f", *normals)
    idx_bytes = struct.pack(f"<{len(indices)}H", *indices)
    blob = pos_bytes + nor_bytes + idx_bytes
    doc = {
        "asset": {"version": "2.0"},
        "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
        "materials": [{"pbrMetallicRoughness": {"baseColorFactor": [0.9, 0.5, 0.2, 1.0]}}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2, "material": 0}]}],
        "buffers": [{"byteLength": len(blob), "uri": "data:application/octet-stream;base64," + base64.b64encode(blob).decode()}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": len(pos_bytes)},
            {"buffer": 0, "byteOffset": len(pos_bytes), "byteLength": len(nor_bytes)},
            {"buffer": 0, "byteOffset": len(pos_bytes) + len(nor_bytes), "byteLength": len(idx_bytes)},
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": vertex_count, "type": "VEC3",
             "min": [-1.0, -1.0, -1.0], "max": [1.0, 1.0, 1.0]},
            {"bufferView": 1, "componentType": 5126, "count": vertex_count, "type": "VEC3"},
            {"bufferView": 2, "componentType": 5123, "count": len(indices), "type": "SCALAR"},
        ],
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(doc))


async def setup(ed: Editor) -> dict:
    if S.get("ed") is ed:
        await ed.call("select_scene", scene=S["scene"])
        return S
    S.clear()
    S["ed"] = ed
    made = await ed.call("create_scene", name="Lods", type="3D", path="scenes/Lods")
    S["scene"] = made["scene"]["id"]
    await ed.call("select_scene", scene=S["scene"])
    sphere = await ed.call("create_entity", scene=S["scene"], type="sphere", name="Ball")
    S["sphere"] = sphere["entity_id"]
    await ed.call("create_entity", scene=S["scene"], type="directional_light", name="Sun")
    write_sphere_gltf(ed.project / "models" / "ball.gltf")
    return S


def check_levels(ed: Editor, data: dict, what: str) -> None:
    levels = data["levels"]
    ed.check(len(levels) >= 2, f"{what}: more than LOD 0 ({levels})")
    ed.check_eq(levels[0]["level"], 0, f"{what}: LOD 0 first")
    ed.check(levels[0]["psnr_db"] is None and levels[0]["switches_below_px"] is None, f"{what}: LOD 0 has no PSNR or switch height")
    for before, after in zip(levels, levels[1:]):
        ed.check(after["triangles"] < before["triangles"], f"{what}: each level has fewer triangles ({levels})")
        ed.check(after["error"] >= before["error"], f"{what}: errors grow with the level ({levels})")
    for entry in levels[1:]:
        ed.check(entry["error"] > 0.0, f"{what}: a coarser level deviates ({entry})")
        ed.check(entry["switches_below_px"] is not None and entry["switches_below_px"] > 0, f"{what}: a switch height ({entry})")
        ed.check(entry["psnr_db"] is None or entry["psnr_db"] > 10.0, f"{what}: a level looks like LOD 0 ({entry})")


async def test_meshes_group_is_hidden_until_enabled(ed: Editor):
    default = {t.name for t in await ed.tools(groups=None)}
    ed.check("capture_model_lods" not in default, "capture_model_lods is hidden until the meshes group is enabled")
    await ed.call("tool_groups", enable=["meshes"])
    ed.check("capture_model_lods" in {t.name for t in await ed.tools(groups=None)}, "listed once meshes is enabled")


async def test_capture_model_lods_of_a_model_file(ed: Editor):
    ed.require_tools("capture_model_lods")
    await setup(ed)
    r = await ed.call_raw("capture_model_lods", {"path": "models/ball.gltf", "size": 128, "save_path": "captures/ball_lods.png"})
    ed.check(not r.is_error, r.text)
    data = r.value
    ed.check_eq(data["source"], "built", "a source model's levels are built as the runtime builds them")
    check_levels(ed, data, "model")
    ed.check(r.images and r.images[0][0].startswith(b"\x89PNG"), "the contact sheet comes back as a PNG")
    ed.check_eq(data["width"], 128 * len(data["levels"]), "one tile per level, side by side")
    ed.check((ed.project / "captures" / "ball_lods.png").is_file(), "save_path wrote the sheet")
    # only the levels asked for (and LOD 0 first)
    two = await ed.call("capture_model_lods", path="models/ball.gltf", levels=[0, 1], size=64)
    ed.check_eq([e["level"] for e in two["levels"]], [0, 1])


async def test_capture_model_lods_refuses_bad_arguments(ed: Editor):
    ed.require_tools("capture_model_lods")
    await setup(ed)
    both = await ed.call("capture_model_lods", path="models/ball.gltf", entity=S["sphere"], expect_error=True)
    ed.check("not both" in both, both)
    missing = await ed.call("capture_model_lods", path="models/none.gltf", expect_error=True)
    ed.check("path" in missing, missing)


async def test_capture_model_lods_of_a_gameobject(ed: Editor):
    ed.require_tools("capture_model_lods", "frame_entity")
    s = await setup(ed)
    await ed.call("frame_entity", scene=s["scene"], entity=s["sphere"])
    r = await ed.call_raw("capture_model_lods", {"scene": s["scene"], "entity": s["sphere"], "size": 160})
    ed.check(not r.is_error, r.text)
    data = r.value
    ed.check_eq(data["source"], "renderer")
    check_levels(ed, data, "entity")
    ed.check(r.images and r.images[0][0].startswith(b"\x89PNG"), "the tiles come back as one PNG")
    # the forced levels are given back: the mesh selects its level again
    got = await ed.call("get_entity", scene=s["scene"], entity=s["sphere"], components=["mesh"])
    ed.check("mesh" in got["components"], got)


async def test_render_stats_count_levels(ed: Editor):
    ed.require_tools("get_render_stats")
    await setup(ed)
    await ed.screenshot("scene", scene=S["scene"])
    stats = await ed.call("get_render_stats", view="scene")
    ed.check("vertices" in stats and stats["vertices"] >= stats["triangles"], f"vertices are reported: {stats.get('vertices')}")
    lod = stats["lod"]
    for key in ("lod_histogram", "fading", "culled_by_size", "fade_draws"):
        ed.check(key in lod, f"lod.{key} is reported: {lod}")
    ed.check_eq(len(lod["lod_histogram"]), 4, "one count per level")
    ed.check(sum(lod["lod_histogram"]) >= 1, f"the sphere is counted at its level: {lod}")


# S98 REQ-34/35: a box whose five materials each name a diffuse texture, three missing and two
# present with a tiling (editor/tests/util/FbxImporterWarningsTest.cpp holds the same model)
HALL_TEXTURES = ["missing_wall.psd", "missing_floor.png", "missing_roof.tga", "tile_a.png", "tile_b.png"]


def hall_fbx() -> str:
    objects, connections = [], ['\tC: "OO",200,0', '\tC: "OO",100,200']
    for i, name in enumerate(HALL_TEXTURES):
        tiled = i >= 3
        objects.append(f'\tMaterial: 30{i}, "Material::M{i}", "" {{\n\t\tVersion: 102\n\t\tShadingModel: "phong"\n\t\tMultiLayer: 0\n'
                       f'\t\tProperties70:  {{\n\t\t\tP: "DiffuseColor", "Color", "", "A",0.8,0.8,0.8\n\t\t}}\n\t}}')
        props = '\t\tProperties70:  {\n\t\t\tP: "Scaling", "Vector", "", "A",2,2,1\n\t\t}\n' if tiled else ""
        objects.append(f'\tTexture: 31{i}, "Texture::T{i}", "" {{\n\t\tType: "TextureVideoClip"\n\t\tVersion: 202\n'
                       f'\t\tTextureName: "Texture::T{i}"\n\t\tFileName: "textures/{name}"\n\t\tRelativeFilename: "textures/{name}"\n'
                       f'\t\tModelUVTranslation: 0,0\n\t\tModelUVScaling: {"2,2" if tiled else "1,1"}\n{props}\t}}')
        connections += [f'\tC: "OO",30{i},200', f'\tC: "OP",31{i},30{i}, "DiffuseColor"']
    return ('; FBX 7.4.0 project file\nFBXHeaderExtension:  {\n\tFBXHeaderVersion: 1003\n\tFBXVersion: 7400\n\tCreator: "JasperEngine tests"\n}\n'
            'GlobalSettings:  {\n\tVersion: 1000\n\tProperties70:  {\n\t\tP: "UpAxis", "int", "Integer", "",1\n'
            '\t\tP: "UpAxisSign", "int", "Integer", "",1\n\t\tP: "FrontAxis", "int", "Integer", "",2\n'
            '\t\tP: "FrontAxisSign", "int", "Integer", "",1\n\t\tP: "CoordAxis", "int", "Integer", "",0\n'
            '\t\tP: "CoordAxisSign", "int", "Integer", "",1\n\t\tP: "UnitScaleFactor", "double", "Number", "",100\n\t}\n}\n'
            'Objects:  {\n\tGeometry: 100, "Geometry::Box", "Mesh" {\n'
            '\t\tVertices: *24 {\n\t\t\ta: -1,-1,-1,1,-1,-1,1,1,-1,-1,1,-1,-1,-1,1,1,-1,1,1,1,1,-1,1,1\n\t\t}\n'
            '\t\tPolygonVertexIndex: *24 {\n\t\t\ta: 0,3,2,-2,4,5,6,-8,0,1,5,-5,3,7,6,-3,0,4,7,-4,1,2,6,-6\n\t\t}\n'
            '\t\tGeometryVersion: 124\n\t\tLayerElementUV: 0 {\n\t\t\tVersion: 101\n\t\t\tName: "UVMap"\n'
            '\t\t\tMappingInformationType: "ByPolygonVertex"\n\t\t\tReferenceInformationType: "IndexToDirect"\n'
            '\t\t\tUV: *8 {\n\t\t\t\ta: 0,0,1,0,1,1,0,1\n\t\t\t}\n'
            '\t\t\tUVIndex: *24 {\n\t\t\t\ta: 0,1,2,3,0,1,2,3,0,1,2,3,0,1,2,3,0,1,2,3,0,1,2,3\n\t\t\t}\n\t\t}\n'
            '\t\tLayerElementMaterial: 0 {\n\t\t\tVersion: 101\n\t\t\tName: ""\n\t\t\tMappingInformationType: "ByPolygon"\n'
            '\t\t\tReferenceInformationType: "IndexToDirect"\n\t\t\tMaterials: *6 {\n\t\t\t\ta: 0,1,2,3,4,4\n\t\t\t}\n\t\t}\n'
            '\t\tLayer: 0 {\n\t\t\tVersion: 100\n\t\t\tLayerElement:  {\n\t\t\t\tType: "LayerElementUV"\n\t\t\t\tTypedIndex: 0\n\t\t\t}\n'
            '\t\t\tLayerElement:  {\n\t\t\t\tType: "LayerElementMaterial"\n\t\t\t\tTypedIndex: 0\n\t\t\t}\n\t\t}\n\t}\n'
            '\tModel: 200, "Model::Hall", "Mesh" {\n\t\tVersion: 232\n\t\tShading: T\n\t\tCulling: "CullingOff"\n\t}\n'
            + "\n".join(objects) + "\n}\nConnections:  {\n" + "\n".join(connections) + "\n}\n")


def glb_json(path: Path) -> dict:
    data = path.read_bytes()
    length = struct.unpack("<I", data[12:16])[0]
    return json.loads(data[20:20 + length])


async def test_material_creation_mode_none(ed: Editor):
    """S98 AC30 (MCP side) and AC31: an FBX import with warnings logs one Console line per model, the
    import report (inspect_models) lists each; Material Creation Mode None re-imports it with no
    material and no texture lookup, so nothing is warned, and its submeshes stay."""
    models = ed.project / "models"
    (models / "textures").mkdir(parents=True, exist_ok=True)
    for name in ("tile_a.png", "tile_b.png"):
        (models / "textures" / name).write_bytes(pngdiff.encode(4, 4, bytes([200, 200, 200, 255]) * 16))
    (models / "Hall.fbx").write_text(hall_fbx())
    cursor = (await ed.call("read_console", limit=1))["next_cursor"]
    await ed.call("sync_files", paths=["models/Hall.fbx", "models/textures/tile_a.png", "models/textures/tile_b.png"])
    await ed.call("reimport_models", paths=["models/Hall.fbx"], timeout=600)

    lines = [e["message"] for e in (await ed.call("read_console", since=cursor, contains="Hall.fbx", limit=200))["entries"]
             if e["type"] == "warning"]
    ed.check_eq(len(lines), 1, f"one warning line for the model: {lines}")
    ed.check("3 textures not found" in lines[0] and "tiling/offset not imported on 2 textures" in lines[0], f"with the counts: {lines}")
    report = (await ed.call("inspect_models", paths=["models/Hall.fbx"]))["models"][0]
    warnings = [i["message"] for i in report["issues"] if i["code"] == "fbx-import-warning"]
    ed.check(sum("Texture not found" in w for w in warnings) == 3 and sum("tiling/offset" in w for w in warnings) == 2,
             f"the import report lists all five: {warnings}")
    ed.check_eq(report["settings"]["resolved"]["material_creation_mode"], "import", "Import by default")
    glb = ed.project / ".jasperengine" / "imported" / "models" / "Hall.fbx.glb"
    ed.check_eq(len(glb_json(glb).get("materials", [])), 5, "the import made the five materials")

    await ed.check_error("set_model_import_settings", paths=["models/Hall.fbx"], settings={"material_creation_mode": "none"},
                         platform="mobile", contains="without platform")
    write_sphere_gltf(models / "ball.gltf")
    await ed.call("sync_files", paths=["models/ball.gltf"])
    await ed.check_error("set_model_import_settings", paths=["models/ball.gltf"], settings={"material_creation_mode": "none"},
                         contains="FBX")

    cursor = (await ed.call("read_console", limit=1))["next_cursor"]
    await ed.call("set_model_import_settings", paths=["models/Hall.fbx"], settings={"material_creation_mode": "none"}, timeout=600)
    meta = (models / "Hall.fbx.meta").read_text()
    ed.check("materialCreationMode: none" in meta, f"the sidecar holds it: {meta}")
    doc = glb_json(glb)
    ed.check(not doc.get("materials") and not doc.get("images"), "None: no material and no image in the import")
    ed.check(len(doc["meshes"][0]["primitives"]) >= 5 and all("material" not in p for p in doc["meshes"][0]["primitives"]),
             "its submeshes stay, with the default material")
    warned = [e for e in (await ed.call("read_console", since=cursor, contains="Hall.fbx", limit=200))["entries"] if e["type"] == "warning"]
    ed.check_eq(warned, [], "None warns about no texture")
    report = (await ed.call("inspect_models", paths=["models/Hall.fbx"]))["models"][0]
    ed.check_eq([i for i in report["issues"] if i["code"] == "fbx-import-warning"], [], "the report is empty")
    ed.check_eq(report["settings"]["resolved"]["material_creation_mode"], "none", "and says None")

    # one undo step: Import again, the materials come back
    await ed.call("undo", scope="project")
    await ed.call("reimport_models", paths=["models/Hall.fbx"], timeout=600)
    ed.check_eq(len(glb_json(glb).get("materials", [])), 5, "undo: the materials are imported again")


async def test_project_material_creation_mode(ed: Editor):
    """S98 REQ-44: Project Settings > Mesh > Material Creation Mode is the mode of the FBX models that
    set none (a model's own value wins), and settings search finds it."""
    models = ed.project / "models"
    models.mkdir(parents=True, exist_ok=True)
    (models / "Hall.fbx").write_text(hall_fbx())
    await ed.call("sync_files", paths=["models/Hall.fbx"])
    await ed.call("reimport_models", paths=["models/Hall.fbx"], timeout=600)
    glb = ed.project / ".jasperengine" / "imported" / "models" / "Hall.fbx.glb"
    ed.check_eq(len(glb_json(glb).get("materials", [])), 5, "Import by default")

    await ed.call("set_project_settings", values={"mesh.material_creation_mode": "none"})
    project_yaml = (ed.project / "project.yaml").read_text()
    ed.check("materialCreationMode: none" in project_yaml, f"project.yaml holds it: {project_yaml}")
    await ed.call("reimport_models", paths=["models/Hall.fbx"], timeout=600)
    ed.check(not glb_json(glb).get("materials"), "the project's None: no material in the import")
    report = (await ed.call("inspect_models", paths=["models/Hall.fbx"]))["models"][0]
    ed.check_eq(report["settings"]["resolved"]["material_creation_mode"], "none", "the model resolves the project's mode")

    await ed.call("set_model_import_settings", paths=["models/Hall.fbx"], settings={"material_creation_mode": "import"}, timeout=600)
    await ed.call("reimport_models", paths=["models/Hall.fbx"], timeout=600)
    ed.check_eq(len(glb_json(glb).get("materials", [])), 5, "the model's own Import wins")

    found = await ed.call("search_editor", query=":Material Creation Mode")
    keys = [r["key"] for r in found["results"]]
    ed.check("setting:mesh.material_creation_mode" in keys, f"settings search finds it: {keys}")


# ---- .mesh assets (S98 F1) ----
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
    files = sorted(p.relative_to(ed.project).as_posix() for p in (ed.project / "scenes" / "Shapes").rglob("*.mesh"))
    ed.check(len(files) == 1 and files[0].endswith("Shapes/Ball.mesh"), f"one file for the two spheres, beside the scene: {files}")
    asset = files[0]
    ed.check((ed.project / (asset + ".meta")).exists(), "the .mesh has a GUID (its .meta)")
    ball = mesh_of(text, "Ball")
    ed.check("meshAsset:" in ball and "meshAssetGuid:" in ball and "buffer:" not in ball, f"the sphere names the file: {ball[:400]}")
    ed.check("meshAsset:" in mesh_of(text, "Twin"), "its twin names the same file")
    crate = mesh_of(text, "Crate")
    ed.check("buffer:" in crate and "meshAsset:" not in crate, "a box (under 4 KB) stays in the scene")
    ed.check(text.startswith("formatVersion: 10\n"), f"scene format 10: {text[:40]}")

    info = await ed.call("get_asset_info", path=asset)
    ed.check_eq(info["type"], "mesh", f"{info}")
    ed.check(info["vertex_count"] > 100 and info["index_count"] > 0, f"the sphere's geometry: {info}")
    ed.check_eq(sorted(u["entity"]["name"] for u in info["users"]), ["Ball", "Twin"], f"who shows it: {info['users']}")
    listed = await ed.call("list_assets", pattern="*.mesh", type="mesh")
    ed.check(any(a["path"] == asset for a in listed["entries"]), f"list_assets type mesh: {listed}")

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
    await ed.call("play", action="start", scene=sid)
    try:
        r = await ed.call("eval_lua", code="local m = object('Target', 'Mesh'); m.meshAsset = 'meshes/ring.mesh'; "
                                            "return {asset = m.meshAsset, width = m:getAABB():getSize().x}")
    finally:
        await ed.call("play", action="stop")
    value = r["values"][0]
    ed.check_eq(value["asset"], "meshes/ring.mesh", f"{r}")
    ed.check(abs(value["width"] - 3.0) < 0.01, f"the torus (radius 1, ring 0.5) at once: {r}")
