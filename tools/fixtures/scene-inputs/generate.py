"""Builds Saved.scene of the scene-inputs fixture: the fixture's scene-input shaders on GameObjects
saved in a scene (no Lua), so the editor's Scene and Game views draw them, the export collects and
precompiles their variants (with the depth programs every mesh needs while a shader reads the scene
depth) and the exported game draws them from the saved data (SI_SCENE=saved).

    uv run --no-project --with mcp --with pyyaml python tools/fixtures/scene-inputs/generate.py

JASPER_EDITOR selects the editor (default: this checkout's build). Writes Saved.scene and adds it
to project.yaml.
"""
import asyncio
import shutil
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent
sys.path.insert(0, str(REPO / "tools" / "mcp"))
from harness import Editor  # noqa: E402

M = "submeshes[0].material."


async def shape(ed, scene, kind, name, position, geometry, material, rotation=None):
    args = {"scene": scene, "type": kind, "name": name, "position": position, "select": False}
    if rotation is not None:
        args["rotation"] = rotation
    eid = (await ed.call("create_entity", **args))["entity_id"]
    await ed.call("set_mesh_geometry", scene=scene, entity=eid, **geometry)
    # the shader first: its manifest checks the parameters set next
    await ed.call("set_properties", scene=scene, entity=eid, values={"mesh": {M + "shader": material[M + "shader"]}})
    await ed.call("set_properties", scene=scene, entity=eid, values={"mesh": material})
    await ed.call("set_properties", scene=scene, entity=eid, values={"mesh": {"castShadows": False}})
    return eid


async def build(ed, scene):
    await ed.call("set_scene_settings", scene=scene, settings={"background_color": [0.08, 0.1, 0.16, 1.0]})
    cam = (await ed.call("create_entity", scene=scene, type="camera", name="Camera", position=[0.0, 5.5, 10.0],
                         select=False))["entity_id"]
    await ed.call("set_properties", scene=scene, entity=cam,
                  values={"camera": {"useTarget": True, "target": [0.0, 0.8, 0.0], "yfov": 0.75, "far": 100.0}})
    await ed.call("set_main_camera", scene=scene, entity=cam)

    grid = {M + "shader": "shaders/si_world_grid"}
    await shape(ed, scene, "plane", "Ground", [0, 0, 0], {"shape": "plane", "width": 16, "depth": 12}, grid)
    await shape(ed, scene, "cube", "Tilted box", [-4.0, 0.4, 0.0], {"shape": "box", "width": 1.6, "height": 1.6, "depth": 1.6},
                grid, rotation={"euler": [25, 30, 15]})
    await shape(ed, scene, "sphere", "Bump", [-1.8, 0.0, 1.2], {"shape": "sphere", "radius": 1.0}, grid)
    lo, hi = [-6.0, -0.5, -2.0], [-0.4, 3.0, 3.2]
    await shape(ed, scene, "cube", "Depth grid volume", [(lo[i] + hi[i]) / 2 for i in range(3)],
                {"shape": "box", "width": hi[0] - lo[0], "height": hi[1] - lo[1], "depth": hi[2] - lo[2]},
                {M + "shader": "shaders/si_depth_grid", M + "shaderParams.boxMin": lo, M + "shaderParams.boxMax": hi})

    colors = [[0.95, 0.2, 0.15, 1], [0.95, 0.75, 0.1, 1], [0.2, 0.75, 0.3, 1], [0.2, 0.45, 0.95, 1]]
    for i, c in enumerate(colors):
        await shape(ed, scene, "cube", f"Block {i + 1}", [-0.6 + i * 1.5, 1.5, -3.6], {"shape": "box", "width": 1.4, "height": 3.0, "depth": 0.5},
                    {M + "shader": "library:unlit", M + "shaderParams.baseColor": c})
    await shape(ed, scene, "sphere", "Glow sphere", [1.8, 0.3, 1.4], {"shape": "sphere", "radius": 1.2},
                {M + "shader": "shaders/si_intersection"})
    await shape(ed, scene, "sphere", "Glass sphere", [2.2, 1.6, -2.0], {"shape": "sphere", "radius": 1.0},
                {M + "shader": "shaders/si_refraction"})

    # a heat-haze sprite over the left blocks
    eid = (await ed.call("create_entity", scene=scene, type="sprite", name="Haze sprite", position=[-0.2, 2.0, -2.8],
                         select=False))["entity_id"]
    await ed.call("set_properties", scene=scene, entity=eid, values={
        "sprite": {"width": 64, "height": 64, "pivotPreset": "center"},
        "mesh": {M + "baseColorTexture": "assets/textures/checker.png", "receiveLights": False, "castShadows": False}})
    await ed.call("set_transform", scene=scene, entity=eid, scale=2.2 / 64)
    await ed.call("set_properties", scene=scene, entity=eid, values={"mesh": {M + "shader": "shaders/si_haze"}})

    # soft points crossing the ground at the right
    pts = []
    for k in range(9):
        pts.append({"position": [(k % 3 - 1) * 0.9, -0.1 + (k % 3) * 0.25, (k // 3 - 1) * 0.9], "color": [1, 1, 1, 1],
                    "size": 0.9, "rotation": 0.0, "textureRect": [0, 0, 1, 1], "visible": True})
    eid = (await ed.call("create_entity", scene=scene, type="points", name="Soft points", position=[5.0, 0.0, 1.5],
                         select=False))["entity_id"]
    await ed.call("set_properties", scene=scene, entity=eid, values={"points": {"customShader": "shaders/si_soft_points"}})
    await ed.call("set_properties", scene=scene, entity=eid, values={"points": {"points": pts}})


async def main() -> None:
    async with Editor(project=str(HERE)) as ed:
        # a re-run: the scene made last time goes first (create_scene does not overwrite a file)
        scenes = (await ed.call("list_scenes"))["scenes"]
        for old in [s for s in scenes if s.get("path") == "Saved.scene"]:
            await ed.call("delete_scene", scene=old["id"], delete_file=True)
        made = await ed.call("create_scene", name="Saved", path="Saved.scene")
        scene = made["scene"]["id"] if isinstance(made["scene"], dict) else made["scene"]
        await build(ed, scene)
        await ed.call("save_scene", scene=scene)
        await ed.call("save_project")
        problems = await ed.call("check_shader")
        print("check_shader:", len(problems["results"]), "variants,", problems["failed"], "failed,",
              len(problems["findings"]), "findings")
        for name in ("Saved.scene", "project.yaml"):
            shutil.copy(ed.project / name, HERE / name)


if __name__ == "__main__":
    asyncio.run(main())
