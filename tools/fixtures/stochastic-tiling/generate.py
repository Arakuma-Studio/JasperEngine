"""Builds the stochastic-tiling fixture: a saved project (no Lua) with a terrain and three library
shader meshes tiling the same pebble texture, every one with Stochastic Tiling OFF, so the scene is
also an "old" scene (what an editor without the feature saves). tools/mcp/tests/
test_stochastic_tiling.py turns the feature on per terrain layer and per material, screenshots
both, re-saves the scene and checks the untouched file re-saves byte-identically.

Main Scene.scene (cameras, the main one first):
  TerrainCam   low over a 96 m terrain, its one PBR layer (pebbles colour, normal and packed
               occlusion/roughness maps) tiled every 2 m to the horizon (grazing angles, mips),
               a steep mound in the middle distance (the triplanar side projections)
  PlaneCam     a 12 m quad with library:simple_lit (NORMAL_MAP, the pebble maps tiled 6x6), a
               library:triplanar box and a library:unlit quad with the striped texture
  LitCam       the built-in Lit material (x = -120): a 30 m ground plane tiled 10x10 with every
               map (pebbles base colour, normal, metallic-roughness and occlusion from the packed
               map, the glowing orange pebble as emission), a 3 m box with the same material, and
               an alpha-masked foliage card (leaves cutout, double-sided) casting the shadow of a
               spot light onto the ground
  A low sun from the side (normal-map relief), the spot light (shadows) and the ambient.

    uv run --no-project --with mcp --with pyyaml python tools/fixtures/stochastic-tiling/generate.py [out dir]
    uv run --no-project --with mcp --with pyyaml python tools/fixtures/stochastic-tiling/generate.py --add-lit [out dir]

(out dir defaults to this directory; JASPER_EDITOR selects the editor: generate it with an editor
built without Stochastic Tiling, e.g. main's, so the scene is an old one; --add-lit opens the
fixture as it is and adds only the Lit part, which an editor with the feature saves as an old one
too while the feature is off: off writes nothing). The textures come from make_assets.py.
"""
import asyncio
import shutil
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent
sys.path.insert(0, str(REPO / "tools" / "mcp"))
from harness import Editor  # noqa: E402

T = "assets/textures/"
M = "submeshes[0].material."


async def camera(ed, scene, name, position, target, yfov=0.8):
    r = await ed.call("create_entity", scene=scene, type="camera", name=name, position=position, select=False)
    await ed.call("set_properties", scene=scene, entity=r["entity_id"],
                  values={"camera": {"useTarget": True, "target": target, "yfov": yfov, "far": 400.0}})
    return r["entity_id"]


async def library_mesh(ed, scene, type_, name, position, values, rotation=None, scale=None):
    args = {"scene": scene, "type": type_, "name": name, "position": position, "select": False}
    if rotation:
        args["rotation"] = rotation
    if scale:
        args["scale"] = scale
    eid = (await ed.call("create_entity", **args))["entity_id"]
    # the shader first: its manifest checks the names of the values set next
    await ed.call("set_properties", scene=scene, entity=eid, values={"mesh": {M + "shader": values[M + "shader"]}})
    await ed.call("set_properties", scene=scene, entity=eid, values={"mesh": values})
    return eid


async def lit_mesh(ed, scene, type_, name, position, geometry, material, rotation=None, extra=None):
    """a built-in Lit mesh: generated geometry (tiled UVs), then its material"""
    args = {"scene": scene, "type": type_, "name": name, "position": position, "select": False}
    if rotation:
        args["rotation"] = rotation
    eid = (await ed.call("create_entity", **args))["entity_id"]
    await ed.call("set_mesh_geometry", scene=scene, entity=eid, **geometry)
    values = {M + key: value for key, value in material.items()}
    values.update(extra or {})
    await ed.call("set_properties", scene=scene, entity=eid, values={"mesh": values})
    return eid


async def build_lit(ed, scene):
    """the built-in Lit material's part (x = -120)"""
    pebbles = {
        "baseColorTexture": T + "pebbles.png",
        "normalTexture": T + "pebbles_normal.png",
        "metallicRoughnessTexture": T + "pebbles_orm.png",
        "occlusionTexture": T + "pebbles_orm.png",
        "emissiveTexture": T + "pebbles_emissive.png",
        "emissiveFactor": [1.0, 1.0, 1.0],
        "metallicFactor": 1.0,
        "roughnessFactor": 0.7,
    }
    await lit_mesh(ed, scene, "plane", "LitGround", [-120.0, 0.0, 0.0], {"shape": "plane", "width": 30, "depth": 30, "tiles": 10},
                   pebbles)
    await lit_mesh(ed, scene, "cube", "LitBox", [-113.0, 1.5, 3.0], {"shape": "box", "width": 3, "height": 3, "depth": 3, "tiles": 2},
                   pebbles)
    await lit_mesh(ed, scene, "wall", "LitFoliage", [-124.0, 2.0, 2.0], {"shape": "wall", "width": 8, "height": 4, "tiles": 2}, {
        "baseColorTexture": T + "leaves.png",
        "alphaMode": "MASK",
        "alphaCutoff": 0.5,
        "roughnessFactor": 0.8,
        "metallicFactor": 0.0,
    }, extra={"submeshes[0].faceCulling": False})
    # behind the card, its leaves' shadow falling towards the camera
    spot = (await ed.call("create_entity", scene=scene, type="spot_light", name="LitSpot", position=[-124.0, 8.0, 10.0],
                          select=False))["entity_id"]
    await ed.call("set_properties", scene=scene, entity=spot, values={"light": {
        "intensity": 1000.0, "range": 40.0, "shadows": True, "direction": [0.0, -0.6, -0.8],
        "innerConeCos": 0.85, "outerConeCos": 0.7}})
    await camera(ed, scene, "LitCam", [-120.0, 7.0, -14.0], [-120.0, 0.5, 4.0])


async def build(ed, scene):
    found = await ed.call("find_entities", scene=scene, component="light")
    items = found.get("entities", found) if isinstance(found, dict) else found
    for e in items:
        await ed.call("set_properties", scene=scene, entity=e["id"],
                      values={"light": {"intensity": 3.0, "shadows": False, "direction": [-0.85, -0.45, 0.25]}})

    terrain = (await ed.call("create_entity", scene=scene, type="terrain", name="Ground", select=False))["entity_id"]
    await ed.call("set_properties", scene=scene, entity=terrain,
                  values={"terrain": {"terrainSize": 96.0, "maxHeight": 12.0, "textureDetailTiles": 48.0}})
    await ed.call("set_terrain_maps", scene=scene, entity=terrain,
                  heightmap={"generate": "flat", "base_height": 0.0, "resolution": 128},
                  blendmap={"color": [1, 0, 0], "resolution": 16},
                  layers=[{"index": 0, "pbr": True, "color": T + "pebbles.png", "normal": T + "pebbles_normal.png",
                           "occlusion": T + "pebbles_orm.png", "roughness": T + "pebbles_orm.png"}])
    # a steep mound, its flanks on the side projections
    await ed.call("sculpt_terrain", scene=scene, entity=terrain, strokes=[
        {"mode": "raise", "points": [[0, 18]] * 12, "size": 14, "strength": 1.0, "falloff": "smooth",
         "seconds_per_point": 0.5}])

    await camera(ed, scene, "TerrainCam", [0.0, 4.5, -40.0], [0.0, 0.0, 10.0])
    await ed.call("set_main_camera", scene=scene,
                  entity=(await ed.call("find_entities", scene=scene, name="TerrainCam"))["entities"][0]["id"])

    # the library shaders, off the terrain (x = 120)
    await library_mesh(ed, scene, "plane", "SimpleLitQuad", [120.0, 0.0, 0.0], {
        M + "shader": "library:simple_lit",
        M + "shaderFeatures": ["NORMAL_MAP"],
        M + "shaderParams": {"tiling": [6, 6, 0, 0], "gloss": 0.35, "specularColor": [0.08, 0.08, 0.08, 1]},
        M + "shaderTextures": {"u_baseTexture": T + "pebbles.png", "u_normalTexture": T + "pebbles_normal.png"},
    }, scale=[1.2, 1, 1.2])
    await library_mesh(ed, scene, "cube", "TriplanarBox", [129.0, 2.5, 5.0], {
        M + "shader": "library:triplanar",
        M + "shaderParams": {"tiling": 0.5},
        M + "shaderTextures": {"u_baseTexture": T + "pebbles.png"},
    }, scale=[5, 5, 5])
    await library_mesh(ed, scene, "plane", "UnlitStripes", [111.0, 2.5, 6.0], {
        M + "shader": "library:unlit",
        M + "shaderParams": {"tiling": [4, 4, 0, 0]},
        M + "shaderTextures": {"u_baseTexture": T + "stripes.png"},
    }, rotation={"euler": [-90, 0, 0]}, scale=[0.5, 1, 0.5])
    await camera(ed, scene, "PlaneCam", [120.0, 11.0, -16.0], [120.0, 1.0, 3.0])
    await build_lit(ed, scene)


async def add_lit(out: Path) -> None:
    """the Lit part added to the fixture as it is (its terrain maps and the rest untouched)"""
    async with Editor(project=HERE) as ed:
        textures = ed.project / "assets" / "textures"
        for png in sorted((HERE / "assets" / "textures").glob("*.png")):
            shutil.copy(png, textures / png.name)
        await ed.call("sync_files")
        scene = (await ed.call("list_scenes"))["scenes"][0]["id"]
        await build_lit(ed, scene)
        await ed.call("save_scene", scene=scene)
        out.mkdir(parents=True, exist_ok=True)
        shutil.copy(ed.project / "Main Scene.scene", out / "Main Scene.scene")


async def main(out: Path) -> None:
    async with Editor(create=True, name="StochasticTiling") as ed:
        textures = ed.project / "assets" / "textures"
        textures.mkdir(parents=True, exist_ok=True)
        for png in sorted((HERE / "assets" / "textures").glob("*.png")):
            shutil.copy(png, textures / png.name)
        await ed.call("sync_files")

        scene = (await ed.call("list_scenes"))["scenes"][0]["id"]
        await ed.call("rename_scene", scene=scene, name="Main Scene")
        await build(ed, scene)
        await ed.call("save_scene", scene=scene, path="Main Scene.scene")
        await ed.call("save_project")

        out.mkdir(parents=True, exist_ok=True)
        for name in ("project.yaml", "Main Scene.scene"):
            shutil.copy(ed.project / name, out / name)
        if (ed.project / "terrain_maps").exists():
            if (out / "terrain_maps").exists():
                shutil.rmtree(out / "terrain_maps")
            shutil.copytree(ed.project / "terrain_maps", out / "terrain_maps",
                            ignore=shutil.ignore_patterns("*.meta", ".*"))
        if (out / "assets").exists() and out != HERE:
            shutil.rmtree(out / "assets")
        if out != HERE:
            shutil.copytree(ed.project / "assets", out / "assets", ignore=shutil.ignore_patterns("*.meta", ".*"))
        else:
            # the generated maps the editor put under assets/ (terrain height and blend maps)
            for path in (ed.project / "assets").rglob("*"):
                rel = path.relative_to(ed.project)
                if path.is_file() and not path.name.startswith(".") and not (out / rel).exists():
                    (out / rel).parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy(path, out / rel)


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if a != "--add-lit"]
    target = Path(args[0]) if args else HERE
    asyncio.run(add_lit(target) if "--add-lit" in sys.argv[1:] else main(target))
