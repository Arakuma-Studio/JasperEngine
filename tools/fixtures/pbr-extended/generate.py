"""Builds the materials and lighting fixtures (docs/specs/S11-materials-lighting.md AC11, AC15):

build/pbr-extended, or the directory given (benchmarks, `jasperengine-editor benchmark --scene
"<name>"`; written, not checked in: 200 spheres take 12 MB of scene file each):
  Lit Baseline       200 spheres of the built-in Lit, a sun (the reference frame time)
  Lobes Off          the same spheres carrying every lobe's values with each lobe disabled: draws
                     with the shader of before (within 2% of Lit Baseline and of a pre-S11 build)
  Lobe <name>        the same spheres with one lobe on (Clearcoat, Sheen, Anisotropy, Iridescence,
                     Transmission, Specular, Hex)
  Terrain Baseline   a terrain with 4 painted layers
  Hex Terrain        the same terrain, each layer hex tiled

tools/fixtures/pbr-extended-export (tools/smoke-test.sh, HTML5 and Android exports): runtime shader
compilation off, so a variant the export did not precompile is a missing shader (an error):
  Lobes              one sphere per lobe (clear coat with its maps), Khronos ClearCoatTest (packed
                     glTF maps), a sun in lux at 5500 K, a point light in lumen
  Transmission       a glass sphere (transmission, volume, IOR 1.5) before a striped wall
  Hex                a hex tiled floor and a hex tiled terrain
  PBX_SCENE=<name> in the game's environment loads that scene instead of Lobes (SceneSelect.lua).

    uv run --no-project --with-requirements tools/mcp/requirements.txt python tools/fixtures/pbr-extended/generate.py [dir] [--export-only | --benchmarks-only]

(JASPER_EDITOR selects the editor; it needs an S11 build.) Textures come from
tools/fixtures/shader-library/assets/textures, the model from engine/tests/render/assets/khronos.
"""
import asyncio
import json
import math
import shutil
import struct
import sys
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent
EXPORT = REPO / "tools" / "fixtures" / "pbr-extended-export"
sys.path.insert(0, str(REPO / "tools" / "mcp"))
from harness import Editor  # noqa: E402

TEXTURES = REPO / "tools" / "fixtures" / "shader-library" / "assets" / "textures"
KHRONOS = REPO / "engine" / "tests" / "render" / "assets" / "khronos"
T = "assets/textures/"
M = "submeshes[0].material."

LOBES = {
    "Clearcoat": {"clearcoat": {"enabled": True, "factor": 1.0, "roughness": 0.05}},
    "Sheen": {"sheen": {"enabled": True, "color": [0.8, 0.3, 0.5], "roughness": 0.5}},
    "Anisotropy": {"anisotropy": {"enabled": True, "strength": 0.8, "rotation": 0.5}},
    "Iridescence": {"iridescence": {"enabled": True, "factor": 1.0, "ior": 1.3}},
    "Transmission": {"transmission": {"enabled": True, "factor": 1.0}, "volume": {"enabled": True, "thickness": 0.3},
                     "specular": {"enabled": True, "ior": 1.5}},
    "Specular": {"specular": {"enabled": True, "factor": 0.8, "color": [1.0, 0.8, 0.6]}},
    "Hex": {"hex": {"enabled": True}},
}

SCENE_SELECT = """-- PBX_SCENE=<scene name> in the environment loads that scene instead of this one, so one
-- exported build shows every scene of the fixture (tools/smoke-test.sh SMOKE_GAME_RUNS).
local SceneSelect = { properties = {} }

function SceneSelect:init()
    local name = os.getenv("PBX_SCENE")
    if name and name ~= "" and name ~= "Lobes" then
        SceneManager.loadScene(name)
    end
end

return SceneSelect
"""


def png(width: int, height: int, pixel) -> bytes:
    raw = bytearray()
    for y in range(height):
        raw.append(0)
        for x in range(width):
            raw.extend(pixel(x, y))

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(bytes(raw))) + chunk(b"IEND", b""))


def unpack_glb(glb: Path, gltf: Path) -> None:
    """a .glb as a .gltf with its buffer and images in files beside it: the textures a scene
    keeps of the model are then paths, which a scene file round-trips (embedded images are ids)"""
    data = glb.read_bytes()
    json_length = struct.unpack_from("<I", data, 12)[0]
    doc = json.loads(data[20:20 + json_length])
    bin_start = 20 + json_length
    bin_length = struct.unpack_from("<I", data, bin_start)[0]
    blob = data[bin_start + 8:bin_start + 8 + bin_length]
    for i, image in enumerate(doc.get("images", [])):
        if "bufferView" in image:
            view = doc["bufferViews"][image.pop("bufferView")]
            start = view.get("byteOffset", 0)
            ext = ".jpg" if image.get("mimeType") == "image/jpeg" else ".png"
            name = f"{gltf.stem}_image{i}{ext}"
            (gltf.parent / name).write_bytes(blob[start:start + view["byteLength"]])
            image.pop("mimeType", None)
            image["uri"] = name
    doc["buffers"][0]["uri"] = gltf.stem + ".bin"
    (gltf.parent / (gltf.stem + ".bin")).write_bytes(blob)
    gltf.write_text(json.dumps(doc, indent=1))


def scene_id(made):
    return made["scene"]["id"] if isinstance(made["scene"], dict) else made["scene"]


def find(value, *keys):
    if isinstance(value, dict):
        for k in keys:
            if k in value:
                return value[k]
        for v in value.values():
            got = find(v, *keys)
            if got is not None:
                return got
    elif isinstance(value, list):
        for v in value:
            got = find(v, *keys)
            if got is not None:
                return got
    return None


async def with_component(ed, sid, component):
    found = await ed.call("find_entities", scene=sid, component=component)
    return [find(e, "entity_id", "id") for e in found.get("entities", [])]


async def camera(ed, sid, position, target):
    """the scene's main camera at position, looking at target (a new scene draws through the
    default camera, which is no GameObject)"""
    cams = await with_component(ed, sid, "CameraComponent")
    if not cams:
        cams = [(await ed.call("create_entity", scene=sid, type="camera", name="Camera", select=False))["entity_id"]]
    if not await with_component(ed, sid, "LightComponent"):
        await ed.call("create_entity", scene=sid, type="directional_light", name="Sun", select=False)
    cam = cams[0]
    await ed.call("set_transform", scene=sid, entity=cam, position=list(position))
    await ed.call("set_properties", scene=sid, entity=cam, values={"camera": {"useTarget": True, "target": list(target)}})
    await ed.call("set_main_camera", scene=sid, entity=cam)


async def sun(ed, sid):
    return (await with_component(ed, sid, "LightComponent"))[0]


def pbr_paths(pbr):
    """{lobe: {field: value}} as set_properties paths"""
    return {f"{M}pbr.{lobe}.{field}": value for lobe, fields in pbr.items() for field, value in fields.items()}


def material(pbr=None, texture=None):
    values = {M + "baseColorFactor": [0.75, 0.2, 0.15, 1], M + "metallicFactor": 0.1, M + "roughnessFactor": 0.45}
    if texture:
        values[M + "baseColorTexture"] = texture
        values[M + "baseColorFactor"] = [1, 1, 1, 1]
    if pbr is not None:
        values.update(pbr_paths(pbr))
    return values


async def sphere_grid(ed, sid, values):
    """200 spheres filling the view: 20 columns, 10 rows"""
    ids = []
    for row in range(10):
        for col in range(20):
            made = await ed.call("create_entity", scene=sid, type="sphere", name=f"S{row}_{col}", select=False,
                                 position=[(col - 9.5) * 1.05, (row - 4.5) * 1.05, 0])
            ids.append(made["entity_id"])
    await ed.call("set_properties", scene=sid, entities=ids, values={"mesh": values})
    await camera(ed, sid, (0, 0, 13), (0, 0, 0))


def lobes_off():
    """every lobe's values of LOBES, each disabled"""
    off = {}
    for lobes in LOBES.values():
        for lobe, fields in lobes.items():
            off.setdefault(lobe, {}).update({k: v for k, v in fields.items() if k != "enabled"})
            off[lobe]["enabled"] = False
    return off


async def terrain(ed, sid, hex_tiling: bool):
    made = await ed.call("create_entity", scene=sid, type="terrain", name="Terrain", select=False)
    tid = made["entity_id"]
    colors = ["floor_tiles.png", "bricks.png", "checker.png", "ceiling_planks.png"]
    # an 8-bit height map: a generated one is 16-bit (R16), which GLES3 and WebGL2 cannot sample
    height = ed.project / "assets" / "textures" / "terrain_height.png"
    if not height.exists():
        def h(x, y):
            v = int(128 + 30 * math.sin(x * 0.11) * math.cos(y * 0.07) + 12 * math.sin((x + y) * 0.23))
            return (v, v, v, 255)
        height.write_bytes(png(128, 128, h))
        await ed.call("sync_files")
    await ed.call("set_terrain_maps", scene=sid, entity=tid, heightmap=T + "terrain_height.png",
                  blendmap={"color": [0.45, 0.35, 0.2], "resolution": 64},
                  layers=[{"index": i, "color": T + c, "hex_tiling": hex_tiling, "hex_rotation": 1.0} for i, c in enumerate(colors)])
    await camera(ed, sid, (0, 40, 60), (0, 0, 0))
    return tid


async def terrain_nine(ed, sid):
    tid = await terrain(ed, sid, False)
    colors = ["floor_tiles.png", "bricks.png", "checker.png", "ceiling_planks.png"]
    await ed.call("set_terrain_maps", scene=sid, entity=tid,
                  layers=[{"index": i, "color": T + colors[i % 4], "normal": T + colors[(i + 1) % 4], "roughness": T + colors[(i + 2) % 4]} for i in range(9)])
    return tid


async def build_benchmarks(out: Path) -> None:
    async with Editor(create=True, name="PbrExtended") as ed:
        textures = ed.project / "assets" / "textures"
        textures.mkdir(parents=True, exist_ok=True)
        for name in ("floor_tiles.png", "bricks.png", "checker.png", "ceiling_planks.png"):
            shutil.copy(TEXTURES / name, textures / name)
        await ed.call("sync_files")
        names = []

        first = (await ed.call("list_scenes"))["scenes"][0]["id"]
        await ed.call("rename_scene", scene=first, name="Lit Baseline")
        await sphere_grid(ed, first, material())
        await ed.call("save_scene", scene=first, path="Lit Baseline.scene")
        names.append("Lit Baseline")

        sid = scene_id(await ed.call("create_scene", name="Lobes Off", path="Lobes Off.scene"))
        await sphere_grid(ed, sid, material(lobes_off()))
        await ed.call("save_scene", scene=sid)
        names.append("Lobes Off")

        for lobe, pbr in LOBES.items():
            name = f"Lobe {lobe}"
            sid = scene_id(await ed.call("create_scene", name=name, path=f"{name}.scene"))
            await sphere_grid(ed, sid, material(pbr, T + "floor_tiles.png" if lobe == "Hex" else None))
            await ed.call("save_scene", scene=sid)
            names.append(name)

        for name, hexed in (("Terrain Baseline", False), ("Hex Terrain", True)):
            sid = scene_id(await ed.call("create_scene", name=name, path=f"{name}.scene"))
            await terrain(ed, sid, hexed)
            await ed.call("save_scene", scene=sid)
            names.append(name)

        # T3 R35: nine overlapping PBR layers, what the tiers are measured on
        sid = scene_id(await ed.call("create_scene", name="Terrain Nine", path="Terrain Nine.scene"))
        await terrain_nine(ed, sid)
        await ed.call("save_scene", scene=sid)
        names.append("Terrain Nine")

        await ed.call("save_project")
        copy_project(ed.project, out, [f"{n}.scene" for n in names])


async def build_export(out: Path) -> None:
    async with Editor(create=True, name="PbrExtendedExport") as ed:
        textures = ed.project / "assets" / "textures"
        textures.mkdir(parents=True, exist_ok=True)
        for name in ("floor_tiles.png", "bricks.png", "checker.png", "ceiling_planks.png", "stripes.png"):
            shutil.copy(TEXTURES / name, textures / name)
        # a clear coat map (R factor, G roughness) and a clear coat normal map
        (textures / "clearcoat.png").write_bytes(png(64, 64, lambda x, y: (255 if (x // 8 + y // 8) % 2 else 96, 40 + y * 3, 255, 255)))
        (textures / "clearcoat_normal.png").write_bytes(png(64, 64, lambda x, y: (128 + (24 if x % 16 < 8 else -24), 128, 240, 255)))
        model = ed.project / "assets" / "models" / "ClearCoatTest"
        model.mkdir(parents=True, exist_ok=True)
        unpack_glb(KHRONOS / "ClearCoatTest" / "glTF-Binary" / "ClearCoatTest.glb", model / "ClearCoatTest.gltf")
        shutil.copy(KHRONOS / "ClearCoatTest" / "LICENSE.md", model / "LICENSE.md")
        await ed.call("sync_files")

        lobes = (await ed.call("list_scenes"))["scenes"][0]["id"]
        await ed.call("rename_scene", scene=lobes, name="Lobes")
        await camera(ed, lobes, (0, 0, 13), (0, 0, 0))
        x = -6.3
        for lobe, pbr in LOBES.items():
            made = await ed.call("create_entity", scene=lobes, type="sphere", name=lobe, select=False, position=[x, 1.2, 0])
            if lobe == "Clearcoat":
                pbr = {"clearcoat": {"enabled": True, "factor": 1.0, "roughness": 0.1, "map": T + "clearcoat.png",
                                     "normalMap": T + "clearcoat_normal.png"}}
            values = material(pbr, T + "floor_tiles.png" if lobe == "Hex" else None)
            await ed.call("set_properties", scene=lobes, entity=made["entity_id"], values={"mesh": values})
            x += 2.1
        await ed.call("create_entity", scene=lobes, type="model", name="ClearCoatTest", select=False,
                      model="assets/models/ClearCoatTest/ClearCoatTest.gltf", position=[0, -1.8, -2], scale=[0.5, 0.5, 0.5])
        sun_id = await sun(ed, lobes)
        await ed.call("set_light_unit", scene=lobes, entity=sun_id, unit="lux")
        await ed.call("set_properties", scene=lobes, entity=sun_id, values={"light": {"useColorTemperature": True, "colorTemperature": 5500}})
        lamp = await ed.call("create_entity", scene=lobes, type="point_light", name="Lamp", select=False, position=[0, 2.5, 2])
        await ed.call("set_properties", scene=lobes, entity=lamp["entity_id"], values={"light": {"intensity": 2.0, "range": 12}})
        await ed.call("set_light_unit", scene=lobes, entity=lamp["entity_id"], unit="lumen")
        made = await ed.call("create_entity", scene=lobes, type="empty", name="SceneSelect", select=False)
        await ed.call("create_script", kind="lua", name="SceneSelect", content=SCENE_SELECT)
        await ed.call("attach_script", scene=lobes, entity=made["entity_id"], path="SceneSelect.lua")
        await ed.call("save_scene", scene=lobes, path="Lobes.scene")

        sid = scene_id(await ed.call("create_scene", name="Transmission", path="Transmission.scene"))
        wall = await ed.call("create_entity", scene=sid, type="cube", name="Wall", select=False, position=[0, 1, -2], scale=[8, 4, 0.1])
        await ed.call("set_properties", scene=sid, entity=wall["entity_id"], values={"mesh": {M + "baseColorTexture": T + "stripes.png"}})
        glass = await ed.call("create_entity", scene=sid, type="sphere", name="Glass", select=False, position=[0, 1, 0], scale=[2, 2, 2])
        await ed.call("set_properties", scene=sid, entity=glass["entity_id"], values={"mesh": {
            M + "baseColorFactor": [1, 1, 1, 1], M + "metallicFactor": 0, M + "roughnessFactor": 0.05,
            **pbr_paths({"transmission": {"enabled": True, "factor": 1.0},
                         "volume": {"enabled": True, "thickness": 1.0, "attenuationColor": [0.7, 0.9, 1.0], "attenuationDistance": 4.0},
                         "specular": {"enabled": True, "ior": 1.5}})}})
        await camera(ed, sid, (0, 1, 5), (0, 1, 0))
        await ed.call("save_scene", scene=sid)

        sid = scene_id(await ed.call("create_scene", name="Hex", path="Hex.scene"))
        await terrain(ed, sid, True)
        floor = await ed.call("create_entity", scene=sid, type="plane", name="Floor", select=False, position=[0, 8, 30], scale=[6, 1, 6])
        await ed.call("set_properties", scene=sid, entity=floor["entity_id"], values={"mesh": {
            M + "baseColorTexture": T + "floor_tiles.png", M + "pbr.hex.enabled": True, "submeshes[0].textureRect": [0, 0, 16, 16]}})
        await ed.call("save_scene", scene=sid)

        await ed.call("save_project")
        copy_project(ed.project, out, ["Lobes.scene", "Transmission.scene", "Hex.scene", "SceneSelect.lua"])
        # runtime shader compilation off (Project Settings > Graphics): every variant from the build
        text = (out / "project.yaml").read_text()
        if "runtimeShaders:" in text:
            text = text.replace("runtimeShaders: true", "runtimeShaders: false")
        else:
            text = text.rstrip("\n") + "\nruntimeShaders: false\n"
        (out / "project.yaml").write_text(text)


def copy_project(project: Path, out: Path, files) -> None:
    out.mkdir(parents=True, exist_ok=True)
    for name in ["project.yaml", *files]:
        shutil.copy(project / name, out / name)
    if (out / "assets").exists():
        shutil.rmtree(out / "assets")
    shutil.copytree(project / "assets", out / "assets", ignore=shutil.ignore_patterns("*.meta", ".*"))
    # the terrains' generated height and blend maps
    if (out / "terrain_maps").exists():
        shutil.rmtree(out / "terrain_maps")
    if (project / "terrain_maps").exists():
        shutil.copytree(project / "terrain_maps", out / "terrain_maps", ignore=shutil.ignore_patterns("*.meta", ".*"))


async def main(args) -> None:
    export_only = "--export-only" in args
    benchmarks_only = "--benchmarks-only" in args
    args = [a for a in args if a not in ("--export-only", "--benchmarks-only")]
    if not export_only:
        await build_benchmarks(Path(args[0]) if args else REPO / "build" / "pbr-extended")
    if not benchmarks_only:
        await build_export(EXPORT)


if __name__ == "__main__":
    asyncio.run(main(sys.argv[1:]))
