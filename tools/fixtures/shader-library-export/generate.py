"""Builds the shader-library-export fixture: a saved project (no Lua) whose scenes draw with every
library shader, with non-default parameters, textures and features, each mesh shader in each of its
lighting modes, the blend modes, sprites, points, UI images and the three skies. Every animated value
(speeds, frame rates) is zero so a frame looks the same whenever it is taken: the editor's render of
a scene and the exported game's screenshot must match (check.sh here).

Scenes (one camera each):
  Lit.scene      every mesh shader in each lighting mode, with lights (one per-vertex), shadows,
                 fog and the procedural sky (clouds)
  Blend.scene    every mesh shader in each blend mode before a backdrop, the keyword cases that
                 change the state, the panoramic sky
  2D.scene       library:sprite Sprites (each feature), library:particle Points (each blend mode
                 and feature), the gradient sky
  UI.scene       library:ui_fx Images (each feature and blend mode) over a striped backdrop
  Stencil.scene  phase 2: every material preset (editor/material/MaterialPresets.cpp, applied with
                 the MCP tool apply_material_preset), so the extra pass and the stencil and colour
                 write state go through scene YAML and Factory export: outlines (screen and world
                 width; a blended object, which the hull preset gives the visible-only stencil
                 outline), a through-walls stencil outline, a see-through
                 silhouette, draw-once transparency, inside/outside stencil masks and sprite masks
  Phase3B.scene  phase 3, scene-reading shaders B: library:water with DEPTH_FOAM, ABSORPTION and
                 REFRACTION (a sloping bed, rocks crossing the surface), library:glass (blurred, normal
                 mapped, a sphere reflecting a cubemap, pixel lit) and library:heat_haze (a quad with SOFT,
                 a fresnel sphere) before a striped wall, every scroll frozen

    uv run --no-project --with mcp --with pyyaml python tools/fixtures/shader-library-export/generate.py [out dir]

(out dir defaults to this directory; JASPER_EDITOR selects the editor). The textures are copied from
tools/fixtures/shader-library/assets/textures.
"""
import asyncio
import base64
import json
import math
import re
import shutil
import struct
import sys
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent
sys.path.insert(0, str(REPO / "tools" / "mcp"))
from harness import Editor  # noqa: E402

TEXTURES = REPO / "tools" / "fixtures" / "shader-library" / "assets" / "textures"
T = "assets/textures/"
M = "submeshes[0].material."
BLENDS = ["opaque", "alpha", "premultiplied", "additive", "multiply"]


def write_gltf(path: Path, positions, normals, uvs, colors, indices):
    """a one-primitive glTF with the buffer embedded"""
    blob = b""
    views, accessors = [], []

    def add(data, fmt, count, kind, ctype, target, minmax=False):
        nonlocal blob
        offset = len(blob)
        chunk = struct.pack("<%d%s" % (len(data), fmt), *data)
        blob += chunk
        while len(blob) % 4:
            blob += b"\0"
        views.append({"buffer": 0, "byteOffset": offset, "byteLength": len(chunk), "target": target})
        acc = {"bufferView": len(views) - 1, "componentType": ctype, "count": count, "type": kind}
        if minmax:
            acc["min"] = [min(data[i::3]) for i in range(3)]
            acc["max"] = [max(data[i::3]) for i in range(3)]
        accessors.append(acc)
        return len(accessors) - 1

    flat = lambda rows: [v for row in rows for v in row]  # noqa: E731
    attributes = {
        "POSITION": add(flat(positions), "f", len(positions), "VEC3", 5126, 34962, True),
        "NORMAL": add(flat(normals), "f", len(normals), "VEC3", 5126, 34962),
        "TEXCOORD_0": add(flat(uvs), "f", len(uvs), "VEC2", 5126, 34962),
        "COLOR_0": add(flat(colors), "f", len(colors), "VEC4", 5126, 34962),
    }
    index = add(indices, "H", len(indices), "SCALAR", 5123, 34963)
    gltf = {
        "asset": {"version": "2.0", "generator": "shader-library-export/generate.py"},
        "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0, "name": path.stem}],
        "meshes": [{"name": path.stem, "primitives": [{"attributes": attributes, "indices": index, "material": 0}]}],
        "materials": [{"name": "white", "pbrMetallicRoughness": {"baseColorFactor": [1, 1, 1, 1]}}],
        "buffers": [{"byteLength": len(blob),
                     "uri": "data:application/octet-stream;base64," + base64.b64encode(blob).decode()}],
        "bufferViews": views, "accessors": accessors,
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(gltf, separators=(",", ":")) + "\n")


def write_models(models: Path):
    """uv_sphere.gltf (radius 0.5, a warm-top to cool-bottom vertex colour) and box.gltf (a unit cube,
    one vertex colour per face): uv, normals and colours, so every library shader's inputs exist"""
    slices, stacks = 24, 16
    pos, nor, uv, col, idx = [], [], [], [], []
    for j in range(stacks + 1):
        v = j / stacks
        theta = v * math.pi
        for i in range(slices + 1):
            u = i / slices
            phi = u * 2 * math.pi
            n = (math.sin(theta) * math.cos(phi), math.cos(theta), -math.sin(theta) * math.sin(phi))
            pos.append([round(c * 0.5, 6) for c in n])
            nor.append([round(c, 6) for c in n])
            uv.append([u, v])
            col.append([1.0 - 0.4 * v, 0.85 - 0.05 * v, 0.6 + 0.4 * v, 1.0])
    for j in range(stacks):
        for i in range(slices):
            a = j * (slices + 1) + i
            b = a + slices + 1
            idx += [a, b, a + 1, a + 1, b, b + 1]
    write_gltf(models / "uv_sphere.gltf", pos, nor, uv, col, idx)

    pos, nor, uv, col, idx = [], [], [], [], []
    faces = [  # normal, u axis, v axis, colour
        ((0, 0, 1), (1, 0, 0), (0, -1, 0), (1.0, 1.0, 1.0)),
        ((0, 0, -1), (-1, 0, 0), (0, -1, 0), (0.8, 0.9, 1.0)),
        ((1, 0, 0), (0, 0, -1), (0, -1, 0), (1.0, 0.85, 0.7)),
        ((-1, 0, 0), (0, 0, 1), (0, -1, 0), (0.75, 1.0, 0.75)),
        ((0, 1, 0), (1, 0, 0), (0, 0, 1), (1.0, 1.0, 0.7)),
        ((0, -1, 0), (1, 0, 0), (0, 0, -1), (0.7, 0.7, 0.9)),
    ]
    for n, ua, va, c in faces:
        base = len(pos)
        for (su, sv) in ((0, 0), (1, 0), (1, 1), (0, 1)):
            p = [0.5 * n[k] + (su - 0.5) * ua[k] + (sv - 0.5) * va[k] for k in range(3)]
            pos.append(p)
            nor.append(list(map(float, n)))
            uv.append([float(su), float(sv)])
            col.append([*c, 1.0])
        idx += [base, base + 2, base + 1, base, base + 3, base + 2]
    write_gltf(models / "box.gltf", pos, nor, uv, col, idx)


def mat(shader, params=None, textures=None, features=None, **state):
    """a library material: {shader, params, textures, features, state}"""
    return {"shader": shader, "params": params or {}, "textures": textures or {},
            "features": features or [], "state": state}


# One non-default material per mesh shader (time frozen: every speed and rate is zero), and the
# shape that shows it best. Feature combinations vary between lighting modes (see LIT_VARIANTS).
MESH = {
    "decal": ("plane", mat("library:decal",
        {"tint": [1.0, 0.55, 0.2, 1.0], "tiling": [1, 1, 0, 0], "opacity": 0.85},
        {"u_baseTexture": T + "fx_decal.png"}, ["VERTEX_COLOR"])),
    "directional_tint": ("cube", mat("library:directional_tint",
        {"color": [0.95, 0.95, 1.0, 1.0], "colorA": [0.2, 0.85, 0.3, 1.0], "colorC": [0.1, 0.3, 0.9, 1.0],
         "tiling": [2, 2, 0, 0]},
        {"u_baseTexture": T + "checker.png"})),
    "dissolve": ("sphere", mat("library:dissolve",
        {"baseColor": [0.35, 0.6, 1.0, 1.0], "tiling": [2, 1, 0, 0], "cutoff": 0.45, "edgeWidth": 0.1,
         "edgeColor": [4.0, 0.6, 2.5, 1.0], "noiseTiling": [1.5, 1.5, 0.2, 0], "worldNoiseScale": 1.4},
        {"u_baseTexture": T + "bricks.png", "u_noiseTexture": T + "fx_noise.png"}, ["VERTEX_COLOR"])),
    "fake_lit": ("sphere", mat("library:fake_lit",
        {"baseColor": [1.0, 0.8, 0.6, 1.0], "tiling": [2, 2, 0, 0], "shadowColor": [0.35, 0.05, 0.25, 1.0],
         "shadowPower": 2.0, "shadowStrength": 0.8, "rimScale": 0.9, "rimPower": 2.0, "matcapGain": 1.3,
         "fogColor": [0.9, 0.4, 0.2, 1.0], "fogStart": 2.0, "fogLength": 8.0, "emissionColor": [0.3, 0.0, 0.5, 1.0],
         "aoStrength": 0.7, "shadowTextureStrength": 0.8},
        {"u_baseTexture": T + "floor_tiles.png", "u_matcapTexture": T + "matcap_studio.png",
         "u_shadowTexture": T + "bake_shadow.png", "u_emissionTexture": T + "stripes.png"},
        ["MATCAP", "AXIS_FOG"])),
    "flipbook": ("cube", mat("library:flipbook",
        {"tint": [1.0, 0.85, 0.6, 1.0], "columns": 4, "rows": 4, "fps": 0.0, "startFrame": 6, "frameCount": 16},
        {"u_sheetTexture": T + "fx_flipbook.png"}, ["VERTEX_COLOR"])),
    "foliage": ("cube", mat("library:foliage",
        {"baseColor": [0.7, 1.0, 0.6, 1.0], "tiling": [2, 2, 0, 0], "alphaCutoff": 0.4,
         "translucencyColor": [1.0, 0.5, 0.1, 1.0], "translucency": 0.5, "windStrength": 0.15,
         "windSpeed": 0.0, "gustStrength": 0.0, "gustFrequency": 0.0, "flutter": 0.0},
        {"u_baseTexture": T + "leaf.png"}, ["ALPHA_CLIP"])),
    "hologram": ("sphere", mat("library:hologram",
        {"tint": [1.0, 0.3, 0.8, 1.0], "fill": 0.35, "scanlineDensity": 8.0, "scanlineSpeed": 0.0,
         "rimPower": 1.5, "rimIntensity": 2.0, "flickerAmount": 0.0, "flickerSpeed": 0.0},
        {"u_scanlineTexture": T + "fx_scanlines.png"}, ["SCANLINE_TEXTURE"])),
    "matcap": ("sphere", mat("library:matcap",
        {"baseColor": [1.0, 0.85, 0.6, 1.0], "tiling": [3, 3, 0, 0], "matcapGain": 1.2, "matcapPower": 1.5,
         "rimColor": [0.2, 0.6, 2.0, 1.0], "rimPower": 2.5},
        {"u_matcapTexture": T + "matcap_shine.png", "u_baseTexture": T + "checker.png"},
        ["RIM", "MATCAP_MODE_MULTIPLY"])),
    "panner": ("cube", mat("library:panner",
        {"tint": [1.0, 0.9, 0.5, 1.0], "tiling": [2, 1, 0.25, 0], "speed": [0, 0],
         "secondTiling": [1, 2, 0, 0.3], "secondSpeed": [0, 0], "distortionStrength": 0.1,
         "distortionSpeed": [0, 0]},
        {"u_baseTexture": T + "stripes.png", "u_secondTexture": T + "fx_streaks.png",
         "u_distortionTexture": T + "fx_noise.png"},
        ["SECOND_LAYER", "DISTORTION", "COMBINE_ADD"])),
    "rim": ("sphere", mat("library:rim",
        {"rimColor": [1.0, 0.4, 0.1, 1.0], "power": 1.5, "intensity": 3.0, "fill": 0.15,
         "textureIntensity": 0.8, "textureTiling": [6, 3, 0, 0], "textureSpeed": [0, 0]},
        {"u_patternTexture": T + "fx_hex.png"}, ["TEXTURE"])),
    "simple_lit": ("sphere", mat("library:simple_lit",
        {"baseColor": [1.0, 0.6, 0.4, 1.0], "tiling": [3, 2, 0, 0], "specularColor": [0.6, 0.6, 0.6, 1.0],
         "gloss": 0.8, "normalStrength": 1.5, "emissionColor": [0.15, 0.0, 0.3, 1.0]},
        {"u_baseTexture": T + "checker.png", "u_normalTexture": T + "fx_water_normal.png",
         "u_emissionTexture": T + "stripes.png"},
        ["NORMAL_MAP", "EMISSION"])),
    "spin_bob": ("cube", mat("library:spin_bob",
        {"baseColor": [0.3, 0.9, 0.5, 1.0], "tiling": [2, 2, 0, 0], "spinSpeed": 0.0, "bobHeight": 0.25,
         "bobFrequency": 0.0, "phaseVariation": 0.5},
        {"u_baseTexture": T + "checker.png"}, ["VERTEX_COLOR"])),
    "sprite": ("cube", mat("library:sprite",
        {"outlineColor": [1.0, 0.9, 0.1, 1.0], "outlineWidth": 2.0, "flashColor": [1.0, 0.1, 0.1, 1.0],
         "flashAmount": 0.4},
        {}, ["OUTLINE", "FLASH"])),
    "toon": ("sphere", mat("library:toon",
        {"baseColor": [0.55, 0.8, 1.0, 1.0], "tiling": [2, 2, 0, 0], "shadowColor": [0.25, 0.1, 0.45, 1.0],
         "bands": 3.0, "bandSmoothness": 0.04, "specularColor": [1.5, 1.5, 1.2, 1.0], "specularSize": 0.25,
         "rimColor": [1.0, 0.6, 0.2, 1.0], "rimPower": 3.0},
        {"u_baseTexture": T + "noise_stylized.png", "u_rampTexture": T + "toon_ramp.png"},
        ["SPECULAR", "RIM"])),
    "triplanar": ("cube", mat("library:triplanar",
        {"baseColor": [1.0, 0.95, 0.85, 1.0], "tiling": 1.5, "sharpness": 6.0, "noiseTiling": 0.5,
         "dirtColor": [0.25, 0.1, 0.02, 1.0], "dirtStrength": 0.8},
        {"u_baseTexture": T + "bricks.png", "u_topTexture": T + "floor_tiles.png",
         "u_bottomTexture": T + "ceiling_planks.png", "u_noiseTexture": T + "noise.png"},
        ["TOP_BOTTOM", "NOISE_MASK"])),
    "unlit": ("cube", mat("library:unlit",
        {"baseColor": [1.0, 0.75, 0.75, 1.0], "tiling": [2, 2, 0.5, 0], "alphaCutoff": 0.4},
        {"u_baseTexture": T + "character.png"}, ["VERTEX_COLOR", "ALPHA_CLIP"])),
    "vertex_color": ("cube", mat("library:vertex_color",
        {"baseColor": [0.6, 0.9, 1.0, 1.0], "tiling": [3, 3, 0, 0]},
        {"u_baseTexture": T + "checker.png"}, ["TEXTURE"])),
    "vertex_lit": ("sphere", mat("library:vertex_lit",
        {"baseColor": [1.0, 0.9, 0.5, 1.0], "tiling": [3, 2, 0, 0], "emissionColor": [0.4, 0.1, 0.0, 1.0]},
        {"u_baseTexture": T + "bricks.png", "u_emissionTexture": T + "stripes.png"},
        ["VERTEX_COLOR", "EMISSION"])),
    "water": ("plane", mat("library:water",
        {"shallowColor": [0.1, 0.8, 0.5, 0.6], "deepColor": [0.02, 0.1, 0.3, 0.95], "fresnelPower": 2.0,
         "normalStrength": 1.0, "layerScale": 2.0, "layerSpeed": [0, 0], "secondLayerScale": 1.5,
         "secondLayerSpeed": [0, 0], "foamColor": [1.0, 0.9, 0.8, 1.0], "foamAmount": 0.35, "gloss": 0.7,
         "waveHeight": 0.1, "waveSpeed": 0.0},
        {"u_normalTexture": T + "fx_water_normal.png", "u_noiseTexture": T + "fx_noise.png"}, ["WAVES"])),
}

# Other feature sets per lighting mode, so a shader's features are not only seen in one variant
LIT_VARIANTS = {
    ("dissolve", "pixel"): ["WORLD_NOISE"],
    ("fake_lit", "vertex"): ["SHADOW_TEXTURE", "VERTEX_AO", "EMISSION"],
    ("foliage", "vertex"): ["ALPHA_CLIP", "WIND_MASK_HEIGHT"],
    ("simple_lit", "vertex"): ["VERTEX_COLOR"],
    ("spin_bob", "pixel"): [],
    ("toon", "vertex"): ["RAMP"],
    ("toon", "mixed"): ["RAMP", "RIM"],
    ("triplanar", "pixel"): ["SPACE_OBJECT", "NOISE_MASK"],
    ("vertex_color", "pixel"): [],
}


def mesh_values(m, lighting=None, blend=None, features=None):
    v = {M + "shader": m["shader"]}
    for k, val in m["params"].items():
        v[M + "shaderParams." + k] = val
    for k, val in m["textures"].items():
        v[M + "shaderTextures." + k] = val
    v[M + "shaderFeatures"] = list(m["features"] if features is None else features)
    if lighting:
        v[M + "lighting"] = lighting
    if blend:
        v[M + "blendMode"] = blend
    return v


MODELS = {"sphere": "assets/models/uv_sphere.gltf", "cube": "assets/models/box.gltf",
          "overlap": "assets/models/overlap_quads.gltf"}
OVERLAP = REPO / "tools" / "fixtures" / "shader-library" / "assets" / "models" / "overlap_quads.gltf"


async def mesh(ed, scene, name, shape, values, position, rotation=None, scale=None, geometry=None):
    """a library-shaded mesh: sphere and cube are the glTF models (a model keeps its edited submeshes
    through its load), plane a generated one (geometry: set_mesh_geometry arguments)"""
    args = {"scene": scene, "name": name, "position": position, "select": False}
    if shape in MODELS:
        args.update(type="model", model=MODELS[shape])
    else:
        args["type"] = shape
    if rotation is not None:
        args["rotation"] = rotation
    if scale is not None:
        args["scale"] = scale
    r = await ed.call("create_entity", **args)
    eid = r["entity_id"]
    if geometry:
        await ed.call("set_mesh_geometry", scene=scene, entity=eid, **geometry)
    # the shader first: its manifest checks the names of the values set next
    await ed.call("set_properties", scene=scene, entity=eid, values={"mesh": {M + "shader": values[M + "shader"]}})
    await ed.call("set_properties", scene=scene, entity=eid, values={"mesh": values})
    return eid


async def camera(ed, scene, position, target, yfov=0.75):
    r = await ed.call("create_entity", scene=scene, type="camera", name="Camera", position=position, select=False)
    await ed.call("set_properties", scene=scene, entity=r["entity_id"],
                  values={"camera": {"useTarget": True, "target": target, "yfov": yfov, "far": 200.0}})
    await ed.call("set_main_camera", scene=scene, entity=r["entity_id"])


async def sky(ed, scene, shader, params, textures=None, features=None):
    tree = await ed.call("find_entities", scene=scene, component="skybox")
    items = tree.get("entities", tree) if isinstance(tree, dict) else tree
    if items:
        eid = items[0]["id"]
    else:
        eid = (await ed.call("create_entity", scene=scene, type="skybox", name="Skybox", select=False))["entity_id"]
    await ed.call("set_properties", scene=scene, entity=eid, values={"skybox": {"customShader": shader}})
    values = {"shaderUniforms." + k: v for k, v in params.items()}
    for k, v in (textures or {}).items():
        values["shaderTextures." + k] = v
    if features:
        values["shaderFeatures"] = features
    await ed.call("set_properties", scene=scene, entity=eid, values={"skybox": values})


async def lights(ed, scene, sun_shadows=True):
    found = await ed.call("find_entities", scene=scene, component="light")
    items = found.get("entities", found) if isinstance(found, dict) else found
    for e in items:
        await ed.call("set_properties", scene=scene, entity=e["id"],
                      values={"light": {"intensity": 3.0, "shadows": sun_shadows, "direction": [-0.4, -1.0, -0.6]}})


async def build_lit(ed, scene):
    """every mesh shader in each lighting mode on a ground, lit by a sun, a point light, a per-vertex
    spot light, with shadows and fog"""
    shaders = {s["shader"]: s for s in (await ed.call("list_shaders", type="mesh"))["shaders"]}
    await lights(ed, scene)
    await mesh(ed, scene, "Ground", "plane", {M + "shader": "library:simple_lit",
               M + "shaderParams.baseColor": [0.55, 0.55, 0.6, 1.0], M + "shaderParams.tiling": [12, 12, 0, 0],
               M + "shaderTextures.u_baseTexture": T + "floor_tiles.png", M + "lighting": "pixel"},
               [0, 0, -3.5], geometry={"shape": "plane", "width": 24, "depth": 14})
    r = await ed.call("create_entity", scene=scene, type="point_light", name="Point", position=[-4.5, 2.2, -1.5],
                      select=False)
    await ed.call("set_properties", scene=scene, entity=r["entity_id"],
                  values={"light": {"intensity": 6.0, "range": 7.0, "color": [1.0, 0.6, 0.3], "shadows": True}})
    r = await ed.call("create_entity", scene=scene, type="spot_light", name="Spot (vertex)", position=[4.5, 4.0, -2.5],
                      select=False)
    await ed.call("set_properties", scene=scene, entity=r["entity_id"],
                  values={"light": {"intensity": 12.0, "range": 10.0, "color": [0.4, 0.7, 1.0], "shadows": True,
                                    "direction": [0, -1, 0], "shading": "vertex"}})
    r = await ed.call("create_entity", scene=scene, type="fog", name="Fog", select=False)
    await ed.call("set_properties", scene=scene, entity=r["entity_id"],
                  values={"fog": {"type": "linear", "color": [0.75, 0.8, 0.9], "linearStart": 9.0, "linearEnd": 30.0}})

    cells = []
    for short, (shape, m) in MESH.items():
        for lighting in shaders[m["shader"]]["lighting"]:
            cells.append((short, shape, m, lighting))
    columns = 8
    for i, (short, shape, m, lighting) in enumerate(cells):
        col, row = i % columns, i // columns
        x = (col - (columns - 1) / 2) * 1.6
        z = -row * 1.7
        features = LIT_VARIANTS.get((short, lighting))
        if features is not None and short in ("toon", "triplanar", "fake_lit", "foliage") and not features:
            features = None
        values = mesh_values(m, lighting=lighting, features=features)
        rotation, geometry = None, None
        y = 0.6
        if shape == "plane":
            y = 0.03
            geometry = {"shape": "plane", "width": 1.4, "depth": 1.4, "tiles": 16 if short == "water" else 1}
        elif shape == "cube":
            rotation = {"euler": [0, 30, 0]}
        await mesh(ed, scene, f"{short}-{lighting}", shape, values, [x, y, z], rotation=rotation, geometry=geometry)
    print("lit:", len(cells), "objects")
    await camera(ed, scene, [0, 6.2, 6.0], [0, 0.0, -3.4])
    await sky(ed, scene, "library:procedural_sky",
              {"skyColor": [0.1, 0.25, 0.8, 1], "horizonColor": [0.9, 0.7, 0.6, 1], "sunSize": 2.5, "exposure": 1.1,
               "cloudCoverage": 0.55, "cloudSpeed": [0, 0, 0, 0], "cloudColor": [1.0, 0.95, 0.9, 1]},
              {"u_cloudTexture": T + "noise.png"}, ["CLOUDS"])


# facing the Blend scene's camera: a plane stands up, a cube shows three faces
TILT = {"plane": {"euler": [90, 0, 0]}, "cube": {"euler": [20, 30, 0]}}


async def build_blend(ed, scene):
    """every mesh shader in each blend mode (rows) before a striped backdrop, the keyword cases
    that change the state, the panoramic sky"""
    await lights(ed, scene, sun_shadows=False)
    names = list(MESH)
    spacing = 1.15
    await mesh(ed, scene, "Backdrop", "plane", {M + "shader": "library:unlit",
               M + "shaderTextures.u_baseTexture": T + "stripes.png", M + "shaderParams.tiling": [8, 2, 0, 0],
               M + "shaderParams.baseColor": [0.9, 0.9, 0.9, 1.0]},
               [0, 3.2, -1.2], rotation={"euler": [90, 0, 0]},
               geometry={"shape": "plane", "width": spacing * len(names) + 1, "depth": 8.4})
    for row, blend in enumerate(BLENDS):
        for col, short in enumerate(names):
            shape, m = MESH[short]
            x = (col - (len(names) - 1) / 2) * spacing
            y = 6.4 - row * 1.15
            rotation = TILT.get(shape)
            geometry = {"shape": "plane", "width": 1.0, "depth": 1.0} if shape == "plane" else None
            await mesh(ed, scene, f"{short}-{blend}", shape, mesh_values(m, blend=blend), [x, y, 0],
                       rotation=rotation, scale=None if geometry else [0.85, 0.85, 0.85], geometry=geometry)
    # the features and keywords that change the render state
    cases = [
        ("hologram-alpha", "sphere", mat("library:hologram", {"tint": [0.2, 1.0, 0.4, 1], "scanlineSpeed": 0.0,
                                                               "flickerAmount": 0.0}, {}, ["BLEND_MODE_ALPHA"])),
        ("decal-multiply", "plane", mat("library:decal", {"tint": [0.9, 0.3, 0.9, 1]},
                                        {"u_baseTexture": T + "fx_decal.png"}, ["DECAL_BLEND_MULTIPLY"])),
        ("flipbook-clip", "cube", mat("library:flipbook", {"fps": 0.0, "startFrame": 9, "alphaCutoff": 0.3},
                                      {"u_sheetTexture": T + "fx_flipbook.png"}, ["ALPHA_CLIP"])),
        ("flipbook-blend", "cube", mat("library:flipbook", {"fps": 0.0, "startFrame": 3, "startTime": 0.5},
                                       {"u_sheetTexture": T + "fx_flipbook.png"}, ["FRAME_BLEND"])),
        ("matcap-replace", "sphere", mat("library:matcap", {"matcapGain": 1.5},
                                         {"u_matcapTexture": T + "matcap_studio.png"}, ["MATCAP_MODE_REPLACE"])),
        ("matcap-add", "sphere", mat("library:matcap", {"baseColor": [0.4, 0.2, 0.1, 1]},
                                     {"u_matcapTexture": T + "matcap_studio.png", "u_baseTexture": T + "bricks.png"},
                                     ["MATCAP_MODE_ADD"])),
        ("panner-multiply", "cube", mat("library:panner", {"speed": [0, 0], "tiling": [3, 3, 0, 0]},
                                        {"u_baseTexture": T + "fx_hex.png"}, ["COMBINE_MULTIPLY", "VERTEX_COLOR"])),
        ("unlit-cutout", "cube", mat("library:unlit", {"alphaCutoff": 0.6}, {"u_baseTexture": T + "leaf.png"},
                                     ["ALPHA_CLIP"])),
        ("sprite-premultiplied", "cube", mat("library:sprite", {}, {}, [], )),
    ]
    for i, (label, shape, m) in enumerate(cases):
        x = (i - (len(cases) - 1) / 2) * 1.6
        rotation = TILT.get(shape)
        blend = "premultiplied" if label == "sprite-premultiplied" else None
        geometry = {"shape": "plane", "width": 1.2, "depth": 1.2} if shape == "plane" else None
        await mesh(ed, scene, label, shape, mesh_values(m, blend=blend), [x, 0.3, 0.6], rotation=rotation,
                   geometry=geometry)
    await camera(ed, scene, [0, 3.6, 16.5], [0, 3.4, 0])
    await sky(ed, scene, "library:panoramic_sky", {"tint": [1.0, 0.9, 0.8, 1], "exposure": 1.2, "rotation": 0.3},
              {"u_panoramaTexture": T + "panorama.png"})


async def build_2d(ed, scene):
    """library:sprite Sprites (each feature), library:particle Points"""
    await lights(ed, scene, sun_shadows=False)
    sprites = [
        ("plain", {}, {}, []),
        ("outline", {"outlineColor": [1.0, 0.85, 0.1, 1.0], "outlineWidth": 2.0}, {}, ["OUTLINE"]),
        ("hue", {"hueShift": 150.0, "hueSpeed": 0.0}, {}, ["HUE_SHIFT"]),
        ("flash", {"flashColor": [1.0, 0.1, 0.1, 1.0], "flashAmount": 0.6}, {}, ["FLASH"]),
        ("grayscale", {"grayscaleAmount": 0.8}, {}, ["GRAYSCALE"]),
        ("replace", {"replaceColorR": [0.6, 0.15, 0.9, 1.0], "replaceColorG": [0.1, 0.5, 1.0, 1.0],
                     "replaceColorB": [1.0, 0.45, 0.0, 1.0]},
         {"u_replaceMaskTexture": T + "character_mask.png"}, ["COLOR_REPLACE"]),
        ("overlay", {"overlayColor": [0.2, 0.9, 1.0, 1.0], "overlayTiling": [2, 2, 0, 0], "overlayAmount": 0.7},
         {"u_overlayTexture": T + "stripes.png"}, ["OVERLAY"]),
        ("glow", {"glowColor": [3.0, 1.0, 0.15, 1.0], "glowScale": 0.8, "glowPower": 3.0}, {}, ["GLOW"]),
    ]
    for i, (label, params, textures, features) in enumerate(sprites):
        x = (i - (len(sprites) - 1) / 2) * 1.7
        blend = "premultiplied" if label == "plain" else None
        r = await ed.call("create_entity", scene=scene, type="sprite", name=f"sprite-{label}", position=[x, 2.6, 0],
                          select=False)
        eid = r["entity_id"]
        await ed.call("set_properties", scene=scene, entity=eid, values={
            "sprite": {"width": 64, "height": 64, "pivotPreset": "center"},
            "mesh": {M + "baseColorTexture": T + ("soft.png" if label == "glow" else "character.png"),
                     "receiveLights": False}})
        await ed.call("set_transform", scene=scene, entity=eid, scale=1.5 / 64)
        await ed.call("set_properties", scene=scene, entity=eid,
                      values={"mesh": mesh_values(mat("library:sprite", params, textures, features), blend=blend)})

    # particles: each blend mode, then GLOW and EROSION
    point_sets = [(b, [], {}) for b in ["auto"] + BLENDS] + [
        ("glow", ["GLOW"], {"glowColor": [3.0, 0.8, 2.5, 1.0], "glowScale": 0.8, "glowPower": 2.0}),
        ("erosion", ["EROSION"], {"erosionSoftness": 0.15, "erosionTiling": 1.5}),
    ]
    for i, (label, features, params) in enumerate(point_sets):
        x = (i - (len(point_sets) - 1) / 2) * 1.7
        pts = []
        for k in range(4):
            pts.append({"position": [(k % 2 - 0.5) * 0.6, (k // 2 - 0.5) * 0.6, 0],
                        "color": [[1, 0.5, 0.2, 1], [0.3, 0.8, 1, 1], [0.6, 1, 0.3, 0.8], [1, 1, 1, 0.6]][k],
                        "size": 0.8, "rotation": 0.0, "textureRect": [0, 0, 1, 1], "visible": True})
        r = await ed.call("create_entity", scene=scene, type="points", name=f"particle-{label}",
                          position=[x, 0.8, 0], select=False)
        eid = r["entity_id"]
        values = {"customShader": "library:particle", "texture": T + "soft.png", "points": pts,
                  "shaderFeatures": features}
        if label in BLENDS:
            values["blendMode"] = label
        for k, v in params.items():
            values["shaderUniforms." + k] = v
        if "EROSION" in features:
            values["shaderTextures.u_erosionTexture"] = T + "fx_noise.png"
        await ed.call("set_properties", scene=scene, entity=eid,
                      values={"points": {"customShader": "library:particle"}})
        await ed.call("set_properties", scene=scene, entity=eid, values={"points": values})

    await camera(ed, scene, [0, 1.4, 10.5], [0, 1.4, 0])
    await sky(ed, scene, "library:gradient_sky",
              {"topColor": [0.3, 0.05, 0.4, 1], "horizonColor": [1.0, 0.5, 0.25, 1], "bottomColor": [0.05, 0.1, 0.2, 1],
               "topExponent": 0.45, "horizonOffset": 0.05, "intensity": 1.1})


async def build_ui(ed, hud):
    """library:ui_fx Images, each feature and each blend mode, over a striped backdrop (a UI scene:
    the editor's Game view draws a scene without its child scenes, so this one is compared alone)"""
    r = await ed.call("create_entity", scene=hud, type="image", name="Backdrop", select=False)
    await ed.call("set_rect_layout", scene=hud, items=[{"entity": r["entity_id"], "anchor": "top_left",
                  "position": [0, 0], "size": [1280, 720]}])
    await ed.call("set_properties", scene=hud, entity=r["entity_id"],
                  values={"ui": {"texture": T + "stripes.png", "color": [0.8, 0.75, 0.9, 1.0]}})
    # UI: each feature, then each blend mode (TINT_OVERLAY so the colour changes)
    ui_sets = [
        ("plain", [], {}, None),
        ("grayscale", ["GRAYSCALE"], {"grayscaleAmount": 0.9}, None),
        ("hue", ["HUE_SHIFT"], {"hueShift": 200.0, "hueSpeed": 0.0}, None),
        ("tint", ["TINT_OVERLAY"], {"tintColor": [0.1, 0.9, 0.3, 1.0], "tintAmount": 0.6}, None),
        # still (speed 0) the band sits just before the element's origin: tilted, it crosses the element
        ("shine", ["SHINE"], {"shineSpeed": 0.0, "shineWidth": 80.0, "shineAngle": -60.0,
                              "shineColor": [1.0, 1.0, 0.6, 0.9]}, None),
    ] + [(b, ["TINT_OVERLAY"], {"tintColor": [0.2, 0.4, 1.0, 1.0], "tintAmount": 0.5}, b) for b in ["auto"] + BLENDS]
    for i, (label, features, params, blend) in enumerate(ui_sets):
        col, row = i % 4, i // 4
        r = await ed.call("create_entity", scene=hud, type="image", name=f"ui_fx-{label}", select=False)
        eid = r["entity_id"]
        await ed.call("set_rect_layout", scene=hud, items=[{"entity": eid, "anchor": "top_left",
                      "position": [80 + col * 300, 60 + row * 200], "size": [260, 120]}])
        values = {"customShader": "library:ui_fx", "texture": T + "button.png", "shaderFeatures": features}
        for k, v in params.items():
            values["shaderUniforms." + k] = v
        if blend:
            values["blendMode"] = blend
        await ed.call("set_properties", scene=hud, entity=eid, values={"ui": {"customShader": "library:ui_fx"}})
        await ed.call("set_properties", scene=hud, entity=eid, values={"ui": values})


async def preset(ed, scene, eid, name, values=None):
    """applies an editor material preset to the entity's first submesh, then pass/material values"""
    await ed.call("apply_material_preset", scene=scene, entity=eid, submesh=0, preset=name)
    if values:
        await ed.call("set_properties", scene=scene, entity=eid, values={"mesh": values})


P = M + "extraPass."


async def build_stencil(ed, scene):
    """phase 2: each material preset, the outline width modes and keywords, masks and sprite masks"""
    await lights(ed, scene, sun_shadows=False)
    unlit = lambda r, g, b, a=1.0: {M + "shader": "library:unlit",  # noqa: E731
                                    M + "shaderParams.baseColor": [r, g, b, a]}
    await mesh(ed, scene, "Backdrop", "plane", unlit(0.45, 0.47, 0.52), [0, 2.0, -2.5],
               rotation={"euler": [90, 0, 0]}, geometry={"shape": "plane", "width": 18, "depth": 9})

    async def wall(x, y):
        await mesh(ed, scene, f"wall-{x}", "plane", unlit(0.3, 0.32, 0.36), [x, y - 0.35, 0.9],
                   rotation={"euler": [90, 0, 0]}, geometry={"shape": "plane", "width": 1.6, "depth": 0.7})

    # row 1: Outline (hull): screen width, world width, distance scale, texture, a blended object
    outlines = [
        ("outline-screen", "cube", unlit(0.85, 0.3, 0.25), {P + "shaderParams.screenWidth": 4.0}),
        ("outline-world", "cube", unlit(0.85, 0.3, 0.25),
         {P + "shaderFeatures": ["WIDTH_MODE_WORLD"], P + "shaderParams.worldWidth": 0.06}),
        ("outline-distance", "sphere", unlit(0.9, 0.9, 0.85),
         {P + "shaderFeatures": ["WIDTH_MODE_WORLD", "DISTANCE_SCALE"], P + "shaderParams.worldWidth": 0.03,
          P + "shaderParams.referenceDistance": 5.0, P + "shaderParams.maxWidth": 0.1}),
        ("outline-texture", "cube", unlit(0.95, 0.95, 0.95),
         {P + "shaderFeatures": ["WIDTH_MODE_WORLD", "TEXTURE"], P + "shaderParams.worldWidth": 0.08,
          P + "shaderTextures.u_outlineTexture": T + "checker.png", P + "shaderParams.outlineColor": [1, 0.3, 0.05, 1]}),
        ("outline-glass", "cube", {**unlit(0.2, 0.45, 1.0, 0.45), M + "blendMode": "alpha"},
         {P + "shaderParams.outlineColor": [0.0, 0.05, 0.3, 1.0]}),
    ]
    for i, (name, shape, base, values) in enumerate(outlines):
        eid = await mesh(ed, scene, name, shape, base, [-6.0 + i * 3.0, 4.2, 0], rotation=TILT.get(shape),
                         scale=[0.9, 0.9, 0.9])
        await preset(ed, scene, eid, "outline_hull", values)

    # row 2: Outline (stencil) through a wall and visible only, see-through silhouette flat and fresnel
    hidden = [
        ("outline-stencil", "outline_stencil", {}),
        ("outline-stencil-visible", "outline_stencil", {P + "shaderFeatures": ["VISIBILITY_VISIBLE_ONLY"]}),
        ("silhouette-flat", "see_through_silhouette", {}),
        ("silhouette-fresnel", "see_through_silhouette",
         {P + "shaderFeatures": ["SILHOUETTE_MODE_FRESNEL"], P + "shaderParams.color": [0.2, 1.0, 0.4, 0.9]}),
    ]
    for i, (name, which, values) in enumerate(hidden):
        x = -4.5 + i * 3.0
        eid = await mesh(ed, scene, name, "sphere", unlit(0.95, 0.75, 0.3), [x, 2.1, 0])
        await preset(ed, scene, eid, which, values)
        await wall(x, 2.1)

    # row 3: draw-once beside a stacking copy, a square mask with an inside reader, a leaf-shaped
    # mask with an outside reader, a sprite mask over an inside and an outside sprite
    # one mesh of three overlapping squares: stacking its overlaps, then with Draw-once transparency
    for i, once in enumerate((False, True)):
        eid = await mesh(ed, scene, f"overlap-{'once' if once else 'stacked'}", "overlap",
                         {**unlit(0.25, 0.65, 1.0, 0.45), M + "blendMode": "alpha"}, [-7.0 + i * 1.7, 0.0, 0.2])
        if once:
            await preset(ed, scene, eid, "draw_once_transparency")
    for i, (reader, shaped) in enumerate((("visible_inside_mask", False), ("visible_outside_mask", True))):
        x = -3.0 + i * 3.0
        eid = await mesh(ed, scene, f"masked-{reader}", "sphere", unlit(0.9, 0.15, 0.1), [x, 0.0, 0])
        await preset(ed, scene, eid, reader)
        mid = await mesh(ed, scene, f"mask-{reader}", "plane", unlit(1.0, 0.0, 1.0), [x, 0.0, 0.8],
                         rotation={"euler": [90, 0, 0]}, geometry={"shape": "plane", "width": 0.8, "depth": 0.8})
        await preset(ed, scene, mid, "stencil_mask",
                     {M + "shaderFeatures": ["ALPHA_CLIP"], M + "shaderTextures.u_maskTexture": T + "leaf.png"}
                     if shaped else None)
    for i, reader in enumerate(("visible_inside_mask", "visible_outside_mask")):
        x = 3.0 + i * 2.0
        for name, texture, z in ((f"sprite-{reader}", "checker.png", 0.0), (f"sprite-mask-{i}", "character.png", 0.05)):
            r = await ed.call("create_entity", scene=scene, type="sprite", name=name, position=[x, 0.0, z],
                              select=False)
            eid = r["entity_id"]
            await ed.call("set_properties", scene=scene, entity=eid, values={
                "sprite": {"width": 64, "height": 64, "pivotPreset": "center"},
                "mesh": {M + "baseColorTexture": T + texture, M + "alphaMode": "blend", "receiveLights": False}})
            await ed.call("set_transform", scene=scene, entity=eid, scale=1.5 / 64)
            if texture == "checker.png":
                await ed.call("set_properties", scene=scene, entity=eid, values={"mesh": {M + "shader": "library:sprite"}})
                await preset(ed, scene, eid, reader)
            else:
                await preset(ed, scene, eid, "sprite_mask")

    await camera(ed, scene, [0, 2.0, 13.0], [0, 2.0, 0])


def write_sky_cross(path: Path, face=32):
    """a horizontal-cross cubemap (4:3, the single-file layout the engine reads): a blue sky darkening
    to the zenith over a pale horizon, a brown ground; a plain RGB PNG written without imaging libraries"""
    width, height = face * 4, face * 3

    def color(x, y):
        tx, ty = x // face, y // face
        if (tx, ty) not in ((1, 0), (0, 1), (1, 1), (2, 1), (3, 1), (1, 2)):
            return (0, 0, 0)
        if ty == 0:
            return (40, 90, 190)
        if ty == 2:
            return (90, 70, 50)
        up = 1.0 - 2.0 * ((y - face) + 0.5) / face  # +1 top of a side face, -1 bottom
        if up < 0.0:
            return (110, 90, 65)
        k = up ** 0.6
        return tuple(int(round(h + (z - h) * k)) for h, z in zip((225, 230, 235), (40, 90, 190)))

    raw = b"".join(b"\0" + bytes(c for x in range(width) for c in color(x, y)) for y in range(height))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) + \
        chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(png)


SKY_CROSS = "assets/textures/p3b_sky_cross.png"


async def build_phase3b(ed, scene):
    """phase 3 B: water's scene features, glass and heat haze, every scroll and drift frozen"""
    await lights(ed, scene)
    unlit = lambda r, g, b, a=1.0: {M + "shader": "library:unlit",  # noqa: E731
                                    M + "shaderParams.baseColor": [r, g, b, a]}
    lit = lambda r, g, b: {M + "shader": "library:simple_lit", M + "lighting": "pixel",  # noqa: E731
                           M + "shaderParams.baseColor": [r, g, b, 1.0]}
    await mesh(ed, scene, "Ground", "plane", lit(0.42, 0.44, 0.48), [0, 0, -1.0],
               geometry={"shape": "plane", "width": 16, "depth": 10})
    # behind the glass a wall of sharp vertical stripes (the checker along u only), behind the haze a grid
    await mesh(ed, scene, "Stripes", "plane", {**unlit(0.3, 0.45, 1.0),
               M + "shaderTextures.u_baseTexture": T + "checker.png", M + "shaderParams.tiling": [2, 0.001, 0, 0]},
               [-0.2, 1.4, -3.2], geometry={"shape": "box", "width": 4.0, "height": 2.8, "depth": 0.2})
    await mesh(ed, scene, "Grid", "plane", {**unlit(0.9, 0.9, 0.9),
               M + "shaderTextures.u_baseTexture": T + "checker.png", M + "shaderParams.tiling": [3, 2, 0, 0]},
               [3.9, 1.4, -3.2], geometry={"shape": "box", "width": 4.0, "height": 2.8, "depth": 0.2})

    # the pool: a checker bed rising out of the water at the back, a rock and a tilted block crossing
    # the surface, a lip and two side walls
    slope = math.degrees(math.atan(1.0 / 3.6))
    await mesh(ed, scene, "Bed", "plane", {**unlit(0.85, 0.85, 0.8),
               M + "shaderTextures.u_baseTexture": T + "checker.png", M + "shaderParams.tiling": [4, 4, 0, 0]},
               [-3.6, 0.45, 0.2], rotation={"euler": [slope, 0, 0]},
               geometry={"shape": "box", "width": 3.6, "height": 0.1, "depth": 3.6 / math.cos(math.radians(slope))})
    grey = lit(0.5, 0.48, 0.45)
    await mesh(ed, scene, "Rock", "sphere", grey, [-4.3, 0.6, 0.9], scale=[0.8, 0.8, 0.8])
    await mesh(ed, scene, "Block", "cube", grey, [-2.8, 0.6, 0.5], rotation={"euler": [25, 35, 15]},
               scale=[0.5, 1.0, 0.5])
    for name, position, size in (("Lip", [-3.6, 0.4, 2.0], (3.7, 0.8, 0.1)),
                                 ("Side-left", [-5.4, 0.5, 0.2], (0.1, 1.0, 3.6)),
                                 ("Side-right", [-1.8, 0.5, 0.2], (0.1, 1.0, 3.6))):
        await mesh(ed, scene, name, "plane", lit(0.6, 0.6, 0.62), position,
                   geometry={"shape": "box", "width": size[0], "height": size[1], "depth": size[2]})
    await mesh(ed, scene, "Water", "plane", mesh_values(mat("library:water",
               {"layerScale": 2.0, "layerSpeed": [0, 0], "secondLayerScale": 1.3, "secondLayerSpeed": [0, 0],
                "foamAmount": 0.08, "shoreFoamWidth": 0.15, "absorptionDepth": 0.8, "refractionStrength": 0.05},
               {"u_normalTexture": T + "fx_water_normal.png", "u_noiseTexture": T + "fx_noise.png"},
               ["DEPTH_FOAM", "ABSORPTION", "REFRACTION"]), lighting="pixel"),
               [-3.6, 0.7, 0.2], geometry={"shape": "plane", "width": 3.6, "depth": 3.6})

    # glass: a frosted pane, a normal-mapped absorbing pane, a sphere reflecting the sky cubemap (lit)
    pane = {"shape": "box", "width": 0.9, "height": 1.3, "depth": 0.04}
    await mesh(ed, scene, "Glass-frosted", "plane", mesh_values(mat("library:glass", {"blurRadius": 3.0}, {},
               ["BLUR"])), [-1.4, 0.75, -2.5], rotation={"euler": [0, 20, 0]}, geometry=pane)
    await mesh(ed, scene, "Glass-normal", "plane", mesh_values(mat("library:glass",
               {"thickness": 0.4, "absorption": 3.0, "absorptionColor": [0.3, 0.75, 0.45, 1.0], "tiling": [2, 2, 0, 0]},
               {"u_normalTexture": T + "fx_water_normal.png"}, ["NORMAL_MAP"])), [-0.1, 0.75, -2.0], geometry=pane)
    await mesh(ed, scene, "Glass-sphere", "sphere", mesh_values(mat("library:glass",
               {"thickness": 0.5, "reflectionColor": [1.0, 1.0, 1.0, 1.0], "reflectionStrength": 1.5},
               {"u_reflectionTexture": {"cube": SKY_CROSS}}, ["REFLECTION_MAP"]), lighting="pixel"),
               [1.1, 0.65, -2.3], scale=[1.1, 1.1, 1.1])

    # heat haze before the grid: a quad with a soft intersection with the ground, a fresnel sphere
    frozen = {"scrollSpeed": [0, 0], "secondScrollSpeed": [0, 0], "strength": 0.03}
    await mesh(ed, scene, "Haze-quad", "plane", mesh_values(mat("library:heat_haze", {**frozen, "noiseTiling": [1, 1, 0.3, 0.1]},
               {"u_noiseTexture": T + "fx_noise.png"}, ["SOFT"])), [3.0, 0.7, -2.4],
               geometry={"shape": "wall", "width": 1.2, "height": 1.6})
    await mesh(ed, scene, "Haze-sphere", "sphere", {**mesh_values(mat("library:heat_haze", frozen,
               {"u_noiseTexture": T + "fx_noise.png"}, ["EDGE_FADE_FRESNEL"])), M + "cullMode": "back"},
               [4.6, 1.0, -2.4], scale=[1.2, 1.2, 1.2])

    await camera(ed, scene, [-0.6, 3.6, 7.2], [-0.6, 0.5, -1.0])


def scene_id(made):
    return made["scene"]["id"] if isinstance(made["scene"], dict) else made["scene"]


# The one script: SLX_SCENE=<name> in the environment loads that scene instead of the start scene
# (Lit), so one exported build shows each scene (tools/smoke-test.sh SMOKE_GAME_RUNS)
SCENE_SELECT = """-- SLX_SCENE=<scene name> in the environment loads that scene instead of this one, so the
-- export check (shader-library-export/check.py) screenshots every scene from one build.
local SceneSelect = { properties = {} }

function SceneSelect:init()
    local name = os.getenv("SLX_SCENE")
    if name and name ~= "" and name ~= "Lit" then
        SceneManager.loadScene(name)
    end
end

return SceneSelect
"""


async def main(out: Path) -> None:
    used = sorted(set(re.findall(r'T \+ "([a-z_]+\.png)"', Path(__file__).read_text())))
    async with Editor(create=True, name="ShaderLibraryExport") as ed:
        textures = ed.project / "assets" / "textures"
        textures.mkdir(parents=True, exist_ok=True)
        for png in used:
            shutil.copy(TEXTURES / png, textures / png)
        write_models(ed.project / "assets" / "models")
        write_sky_cross(ed.project / SKY_CROSS)
        shutil.copy(OVERLAP, ed.project / "assets" / "models" / OVERLAP.name)
        await ed.call("sync_files")

        lit = (await ed.call("list_scenes"))["scenes"][0]["id"]
        await ed.call("rename_scene", scene=lit, name="Lit")
        await build_lit(ed, lit)
        r = await ed.call("create_entity", scene=lit, type="empty", name="SceneSelect", select=False)
        await ed.call("create_script", kind="lua", name="SceneSelect", content=SCENE_SELECT)
        await ed.call("attach_script", scene=lit, entity=r["entity_id"], path="SceneSelect.lua")
        await ed.call("save_scene", scene=lit, path="Lit.scene")

        blend = scene_id(await ed.call("create_scene", name="Blend", path="Blend.scene"))
        await build_blend(ed, blend)
        await ed.call("save_scene", scene=blend)

        two_d = scene_id(await ed.call("create_scene", name="2D", path="2D.scene"))
        await build_2d(ed, two_d)
        await ed.call("save_scene", scene=two_d)

        ui = scene_id(await ed.call("create_scene", name="UI", path="UI.scene", type="UI"))
        await build_ui(ed, ui)
        await ed.call("save_scene", scene=ui)

        stencil = scene_id(await ed.call("create_scene", name="Stencil", path="Stencil.scene"))
        await build_stencil(ed, stencil)
        await ed.call("save_scene", scene=stencil)

        phase3b = scene_id(await ed.call("create_scene", name="Phase3B", path="Phase3B.scene"))
        await build_phase3b(ed, phase3b)
        await ed.call("save_scene", scene=phase3b)

        await ed.call("save_project")
        problems = await ed.call("check_shader")
        failed = [r for r in problems["results"] if not r["ok"]]
        print("check_shader:", len(problems["results"]), "variants,", problems["failed"], "failed", failed,
              problems["findings"])

        out.mkdir(parents=True, exist_ok=True)
        for name in ("project.yaml", "Lit.scene", "Blend.scene", "2D.scene", "UI.scene", "Stencil.scene",
                     "Phase3B.scene", "SceneSelect.lua"):
            shutil.copy(ed.project / name, out / name)
        if (out / "assets").exists():
            shutil.rmtree(out / "assets")
        shutil.copytree(ed.project / "assets", out / "assets",
                        ignore=shutil.ignore_patterns("*.meta", ".*"))


if __name__ == "__main__":
    asyncio.run(main(Path(sys.argv[1]) if len(sys.argv) > 1 else HERE))
