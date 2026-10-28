"""The FluidSimulation component (Pavel Dobryakov's WebGL fluid as an engine feature) as an editor
component, through MCP.

Non-visual (the fluid's pixels belong to the engine's tests): list_component_types lists
fluidsimulation with every serializable field of FluidSimulationComponent (and none of its runtime
ones), create_entity type fluid_simulation (a Sprite showing the fluid, sized for 2D and 3D scenes),
add_component / set_properties / get_entity for every property type with undo and redo, the scene
round trip (YAML key fluidsimulation, only non-default keys), "fluid|<entity>" texture links (set
as {"fluid": ...} and as the link id, saved, reopened, followed by copy / paste and duplicate),
copy / paste of the component, a prefab with an override, remove + undo, and the exported C++ of a
scene with a fluid (build_project mode source).

Runs in a new empty project (EDITOR create).
"""

import math
import re

import yaml

from harness import Editor

EDITOR = {"create": True, "name": "Fluid Component"}
CI = True
TIMEOUT = 900

S: dict = {}

KEY = "fluidsimulation"

# every serializable field: (type, default) as list_component_types reports them
PROPS = {
    "enabled": ("bool", True),
    "output": ("enum", "material"),
    "interactive": ("bool", True),
    "aspectRatio": ("float", 0.0),
    "simResolution": ("uint", 128),
    "dyeResolution": ("uint", 1024),
    "densityDissipation": ("float", 1.0),
    "velocityDissipation": ("float", 0.2),
    "pressure": ("float", 0.8),
    "pressureIterations": ("uint", 20),
    "curl": ("float", 30.0),
    "gradientScale": ("float", 1.0),
    "simulationRate": ("float", 60.0),
    "maxSubsteps": ("uint", 4),
    "timeScale": ("float", 1.0),
    "paused": ("bool", False),
    "splatRadius": ("float", 0.25),
    "splatForce": ("float", 6000.0),
    "colorful": ("bool", True),
    "colorUpdateSpeed": ("float", 10.0),
    "splatsOnStart": ("bool", True),
    "randomSplatCount": ("uint", 0),
    "seed": ("uint", 0),
    "shading": ("bool", True),
    "backColor": ("vector3", [0.0, 0.0, 0.0]),
    "transparent": ("bool", False),
    "bloom": ("bool", True),
    "bloomIterations": ("uint", 8),
    "bloomResolution": ("uint", 256),
    "bloomIntensity": ("float", 0.8),
    "bloomThreshold": ("float", 0.6),
    "bloomSoftKnee": ("float", 0.7),
    "sunrays": ("bool", True),
    "sunraysResolution": ("uint", 196),
    "sunraysWeight": ("float", 1.0),
}

# a non-default value of each (set together, read back, saved, reopened, exported)
CHANGED = {
    "enabled": False,
    "output": "texture",
    "interactive": False,
    "aspectRatio": 1.5,
    "simResolution": 64,
    "dyeResolution": 512,
    "densityDissipation": 2.5,
    "velocityDissipation": 0.5,
    "pressure": 0.6,
    "pressureIterations": 30,
    "curl": 12.0,
    "gradientScale": 0.5,
    "simulationRate": 120.0,
    "maxSubsteps": 2,
    "timeScale": 0.5,
    "paused": True,
    "splatRadius": 0.4,
    "splatForce": 3000.0,
    "colorful": False,
    "colorUpdateSpeed": 4.0,
    "splatsOnStart": False,
    "randomSplatCount": 12,
    "seed": 4000000000,
    "shading": False,
    "backColor": [0.1, 0.2, 0.3],
    "transparent": True,
    "bloom": False,
    "bloomIterations": 4,
    "bloomResolution": 128,
    "bloomIntensity": 0.3,
    "bloomThreshold": 0.9,
    "bloomSoftKnee": 0.2,
    "sunrays": False,
    "sunraysResolution": 98,
    "sunraysWeight": 0.4,
}

RUNTIME = ("simulator", "pendingSplats", "pendingBursts", "pendingSteps", "needReset", "revision")


def close(a, b, tol=1e-4):
    if isinstance(a, bool) or isinstance(b, bool):
        return a == b
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        return abs(a - b) <= tol * max(1.0, abs(b))
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(close(x, y, tol) for x, y in zip(a, b))
    return a == b


def check_values(ed: Editor, got: dict, expected: dict, what: str):
    for name, value in expected.items():
        ed.check(name in got, f"{what}: {name} missing in {sorted(got)}")
        ed.check(close(got[name], value), f"{what}: {name} is {got[name]!r}, expected {value!r}")


async def fluid_values(ed: Editor, entity, scene=None) -> dict:
    got = await ed.call("get_entity", entity=entity, scene=scene if scene is not None else S["scene"],
                        components=[KEY])
    return got["components"][KEY]


async def texture_of(ed: Editor, entity, scene=None):
    got = await ed.call("get_entity", entity=entity, scene=scene if scene is not None else S["scene"],
                        components=["mesh"], properties=["submeshes[0].material.baseColorTexture"])
    return got["components"]["mesh"]["submeshes[0].material.baseColorTexture"]


def scene_entities(path) -> list:
    """GameObjects of a saved .scene file (the YAML nodes that have components)."""
    data = yaml.safe_load(path.read_text())
    found = []

    def walk(node):
        if isinstance(node, dict):
            if "components" in node and isinstance(node["components"], dict):
                found.append(node)
            for v in node.values():
                walk(v)
        elif isinstance(node, list):
            for v in node:
                walk(v)

    walk(data)
    return found


# ---------------------------------------------------------------------------- the type


async def test_component_type_listed(ed: Editor):
    await ed.console_since(None, limit=1)
    listed = await ed.call("list_component_types")
    entries = {c["name"]: c for c in listed["components"]}
    ed.check(KEY in entries, f"{KEY} not listed: {sorted(entries)}")
    ed.check_eq(entries[KEY]["display_name"], "Fluid Simulation")
    ed.check(entries[KEY]["addable"], "Add Component > Effects lists it")
    # the components after it in the enum are still listed (the loop bound is the type count)
    ed.check("uidocument" in entries and "reflectionprobe" in entries, "the last types listed")

    for name in ("FluidSimulation", "fluid", "Fluid Simulation", "FluidSimulationComponent"):
        one = await ed.call("list_component_types", component=name)
        ed.check_eq(one["component"], KEY, f"{name} resolves")
    props = one["properties"]
    ed.check_eq(sorted(props), sorted(PROPS), "every serializable field and no other")
    for name, (kind, default) in PROPS.items():
        ed.check_eq(props[name]["type"], kind, f"{name} type")
        ed.check(close(props[name].get("default"), default), f"{name} default {props[name].get('default')!r}")
    ed.check_eq(props["output"]["options"], ["screen", "material", "texture"])
    for name in RUNTIME:
        ed.check(name not in props, f"runtime field {name} is a property")


async def test_create_entity(ed: Editor):
    made = await ed.call("create_scene", name="Fluids", type="2D")
    S["scene"] = made["scene"]["id"]
    ent = await ed.call("create_entity", scene=S["scene"], type="fluid_simulation")
    S["fluid"] = ent["entity_id"]
    got = await ed.call("get_entity", scene=S["scene"], entity=S["fluid"], components=[KEY, "sprite"])
    ed.check_eq(got["name"] if "name" in got else got["entity"]["name"], "Fluid Simulation", "Unity menu name")
    comps = got["components"]
    ed.check(KEY in comps and "sprite" in comps, f"a Sprite with the fluid: {sorted(comps)}")
    ed.check_eq((comps["sprite"]["width"], comps["sprite"]["height"]), (512, 512), "512 px in a 2D scene")
    ed.check_eq(comps["sprite"]["pivotPreset"], "center")
    check_values(ed, comps[KEY], {k: v for k, (_, v) in PROPS.items()}, "created defaults")

    # the alias, in a 3D scene: 4 units
    made = await ed.call("create_scene", name="Fluids3D", type="3D")
    S["scene3d"] = made["scene"]["id"]
    ent = await ed.call("create_entity", scene=S["scene3d"], type="fluid", name="Panel")
    got = await ed.call("get_entity", scene=S["scene3d"], entity=ent["entity_id"], components=["sprite"])
    ed.check_eq((got["components"]["sprite"]["width"], got["components"]["sprite"]["height"]), (4, 4),
                "4 units in a 3D scene")
    await ed.call("undo", scene=S["scene3d"])
    found = await ed.call("find_entities", scene=S["scene3d"], name="Panel")
    ed.check_eq(found["count"], 0, "create undone")
    await ed.call("select_scene", scene=S["scene"])


async def test_add_component(ed: Editor):
    ent = await ed.call("create_entity", scene=S["scene"], type="empty", name="Bare")
    S["bare"] = ent["entity_id"]
    await ed.call("add_component", scene=S["scene"], entity=S["bare"], component="FluidSimulation",
                  values={KEY: {"curl": 5, "output": "screen"}})
    v = await fluid_values(ed, S["bare"])
    ed.check(close(v["curl"], 5.0), f"curl {v['curl']}")
    ed.check_eq(v["output"], "screen")
    await ed.check_error("add_component", scene=S["scene"], entity=S["bare"], component="fluid", contains="already")
    summary = await ed.call("get_entity", scene=S["scene"], entity=S["bare"], detail="summary")
    ed.check(KEY in str(summary).lower(), f"summary lacks the component: {summary}")


async def test_set_every_property(ed: Editor):
    before = await fluid_values(ed, S["fluid"])
    await ed.call("set_properties", scene=S["scene"], entity=S["fluid"], values={KEY: CHANGED})
    check_values(ed, await fluid_values(ed, S["fluid"]), CHANGED, "set")

    # undo and redo of the whole edit
    await ed.call("undo", scene=S["scene"])
    check_values(ed, await fluid_values(ed, S["fluid"]), before, "undo")
    await ed.call("redo", scene=S["scene"])
    check_values(ed, await fluid_values(ed, S["fluid"]), CHANGED, "redo")

    # the other value forms: enum label and int, hex colour (no linear conversion)
    await ed.call("set_properties", scene=S["scene"], entity=S["fluid"],
                  values={KEY: {"output": "Screen", "backColor": "#336699"}})
    v = await fluid_values(ed, S["fluid"])
    ed.check_eq(v["output"], "screen")
    ed.check(close(v["backColor"], [0x33 / 255, 0x66 / 255, 0x99 / 255]), f"hex colour as given: {v['backColor']}")
    await ed.call("set_properties", scene=S["scene"], entity=S["fluid"], values={KEY: {"output": 1}})
    ed.check_eq((await fluid_values(ed, S["fluid"]))["output"], "material")
    await ed.call("undo", scene=S["scene"], steps=2)
    check_values(ed, await fluid_values(ed, S["fluid"]), CHANGED, "undo of the forms")

    # refusals
    await ed.check_error("set_properties", scene=S["scene"], entity=S["fluid"], values={KEY: {"output": "nowhere"}},
                         contains="screen")
    await ed.check_error("set_properties", scene=S["scene"], entity=S["fluid"], values={KEY: {"simResolution": -1}},
                         contains="non-negative")
    await ed.check_error("set_properties", scene=S["scene"], entity=S["fluid"], values={KEY: {"simulator": 1}})


async def test_save_close_reopen(ed: Editor):
    saved = await ed.call("save_scene", scene=S["scene"], path="scenes/Fluids")
    path = saved["scene"]["path"]
    nodes = {n.get("name"): n for n in scene_entities(ed.project / path) if KEY in n["components"]}
    ed.check("Fluid Simulation" in nodes and "Bare" in nodes, f"saved fluids: {sorted(map(str, nodes))}")
    stored = nodes["Fluid Simulation"]["components"][KEY]
    for name, value in CHANGED.items():
        ed.check(name in stored, f"YAML lacks {name}: {stored}")
        if name == "output":
            ed.check_eq(stored[name], "texture")
        else:
            ed.check(close(stored[name], value), f"YAML {name} = {stored[name]!r}")
    # only what differs from the defaults (enabled is always written)
    bare = nodes["Bare"]["components"][KEY]
    ed.check_eq(sorted(bare), ["curl", "enabled", "output"], f"non-default keys only: {bare}")
    ed.check_eq(bare["output"], "screen")
    for name in RUNTIME:
        ed.check(name not in stored, f"runtime {name} saved")

    await ed.call("close_scene", scene=S["scene"])
    await ed.call("open_scene", scene=path)
    found = await ed.call("find_entities", scene=S["scene"], name="Fluid Simulation")
    ed.check_eq(found["count"], 1, "fluid after reopen")
    S["fluid"] = found["entities"][0]["id"]
    check_values(ed, await fluid_values(ed, S["fluid"]), CHANGED, "reopened")
    S["bare"] = (await ed.call("find_entities", scene=S["scene"], name="Bare"))["entities"][0]["id"]
    v = await fluid_values(ed, S["bare"])
    ed.check(close(v["curl"], 5.0) and v["output"] == "screen" and close(v["pressure"], 0.8), f"bare reopened: {v}")

    # a missing key is the default (a hand-edited scene)
    await ed.call("set_properties", scene=S["scene"], entity=S["fluid"],
                  values={KEY: {k: d for k, (_, d) in PROPS.items()}})
    check_values(ed, await fluid_values(ed, S["fluid"]), {k: d for k, (_, d) in PROPS.items()}, "back to defaults")


# ---------------------------------------------------------------------------- texture links


async def test_texture_link(ed: Editor):
    # a texture-only fluid and two displays: a sprite ({"fluid": ...}) and a UI image (the link id)
    await ed.call("set_properties", scene=S["scene"], entity=S["bare"], values={KEY: {"output": "texture"}})
    S["display"] = (await ed.call("create_entity", scene=S["scene"], type="sprite", name="Display"))["entity_id"]
    await ed.call("set_properties", scene=S["scene"], entity=S["display"],
                  values={"mesh": {"submeshes[0].material.baseColorTexture": {"fluid": "Bare"}}})
    tex = await texture_of(ed, S["display"])
    ed.check(isinstance(tex, dict) and "fluid" in tex, f"read back as a fluid link: {tex}")
    ed.check_eq(tex["fluid"]["id"], S["bare"])

    S["image"] = (await ed.call("create_entity", scene=S["scene"], type="image", name="Screen"))["entity_id"]
    await ed.call("set_properties", scene=S["scene"], entity=S["image"],
                  values={"ui": {"texture": f"fluid|{S['bare']}"}})
    got = await ed.call("get_entity", scene=S["scene"], entity=S["image"], components=["ui"], properties=["texture"])
    ed.check_eq(got["components"]["ui"]["texture"]["fluid"]["id"], S["bare"], "the link id form")

    # undo / redo of a link
    await ed.call("undo", scene=S["scene"])
    got = await ed.call("get_entity", scene=S["scene"], entity=S["image"], components=["ui"], properties=["texture"])
    ed.check(got["components"]["ui"]["texture"] is None, f"link undone: {got['components']['ui']['texture']}")
    await ed.call("redo", scene=S["scene"])

    # refusals: no fluid there, a bad id
    await ed.check_error("set_properties", scene=S["scene"], entity=S["image"],
                         values={"ui": {"texture": {"fluid": "Display"}}}, contains="FluidSimulation")
    await ed.check_error("set_properties", scene=S["scene"], entity=S["image"],
                         values={"ui": {"texture": "fluid|abc"}}, contains="link")
    # a shader texture slot takes images only
    await ed.call("set_properties", scene=S["scene"], entity=S["display"],
                  values={"mesh": {"submeshes[0].material.shader": "library:unlit"}})
    await ed.check_error("set_properties", scene=S["scene"], entity=S["display"],
                         values={"mesh": {"submeshes[0].material.shaderTextures.u_baseTexture": {"fluid": "Bare"}}},
                         contains="image")
    await ed.call("undo", scene=S["scene"])

    # saved as source: fluid, reopened as a link to the same GameObject
    saved = await ed.call("save_scene", scene=S["scene"])
    text = (ed.project / saved["scene"]["path"]).read_text()
    ed.check("source: fluid" in text, "YAML texture source fluid")
    ed.check(re.search(r"fluid: %d\b" % S["bare"], text), "YAML names the fluid entity")
    await ed.call("close_scene", scene=S["scene"])
    await ed.call("open_scene", scene=saved["scene"]["path"])
    S["bare"] = (await ed.call("find_entities", scene=S["scene"], name="Bare"))["entities"][0]["id"]
    S["display"] = (await ed.call("find_entities", scene=S["scene"], name="Display"))["entities"][0]["id"]
    S["image"] = (await ed.call("find_entities", scene=S["scene"], name="Screen"))["entities"][0]["id"]
    tex = await texture_of(ed, S["display"])
    ed.check_eq(tex["fluid"]["id"], S["bare"], "reopened link")


async def test_link_follows_copies(ed: Editor):
    # the fluid and its display copied together: the copy shows the copied fluid
    await ed.call("copy_entities", scene=S["scene"], entities=[S["bare"], S["display"]])
    out = await ed.call("paste_entities", scene=S["scene"])
    made = {c["name"]: c["id"] for c in out["created"]}
    fluid_copy = next(i for n, i in made.items() if n.startswith("Bare"))
    display_copy = next(i for n, i in made.items() if n.startswith("Display"))
    ed.check_eq((await texture_of(ed, display_copy))["fluid"]["id"], fluid_copy, "pasted pair relinked")

    # the display alone keeps its fluid (the same scene)
    out = await ed.call("duplicate_entities", scene=S["scene"], entity=S["display"])
    dup = out["created"][0]["id"] if isinstance(out, dict) and "created" in out else out[0]["id"]
    ed.check_eq((await texture_of(ed, dup))["fluid"]["id"], S["bare"], "duplicate keeps the fluid")

    # into another scene without the fluid: the link is cleared
    await ed.call("copy_entities", scene=S["scene"], entity=S["display"])
    out = await ed.call("paste_entities", scene=S["scene3d"])
    other = out["created"][0]["id"]
    tex = await texture_of(ed, other, scene=S["scene3d"])
    ed.check(tex is None or "fluid" not in tex, f"no fluid there: {tex}")
    await ed.call("undo", scene=S["scene3d"])
    await ed.call("delete_entities", scene=S["scene"], entities=[fluid_copy, display_copy, dup])

    # removing the fluid unbinds; undo binds again
    await ed.call("remove_component", scene=S["scene"], entity=S["bare"], component=KEY)
    tex = await texture_of(ed, S["display"])
    ed.check_eq(tex["fluid"]["id"], S["bare"], "the link id stays (unbound) while the fluid is gone")
    await ed.call("undo", scene=S["scene"])
    v = await fluid_values(ed, S["bare"])
    ed.check_eq(v["output"], "texture", "remove undone")


# ---------------------------------------------------------------------------- copies and prefabs


async def test_copy_paste_component(ed: Editor):
    await ed.call("set_properties", scene=S["scene"], entity=S["fluid"],
                  values={KEY: {"curl": 44, "seed": 77, "backColor": [0.5, 0.25, 0.125]}})
    target = (await ed.call("create_entity", scene=S["scene"], type="sprite", name="Target"))["entity_id"]
    await ed.call("copy_component", scene=S["scene"], entity=S["fluid"], component=KEY)
    await ed.call("paste_component", scene=S["scene"], entity=target, as_new=True)
    v = await fluid_values(ed, target)
    check_values(ed, v, {"curl": 44.0, "seed": 77, "backColor": [0.5, 0.25, 0.125]}, "pasted as new")

    # Paste Component Values over an existing one: a key the source leaves out is its default
    await ed.call("set_properties", scene=S["scene"], entity=target, values={KEY: {"curl": 1, "bloom": False}})
    await ed.call("paste_component", scene=S["scene"], entity=target)
    v = await fluid_values(ed, target)
    check_values(ed, v, {"curl": 44.0, "bloom": True, "seed": 77}, "pasted values")
    await ed.call("undo", scene=S["scene"])
    check_values(ed, await fluid_values(ed, target), {"curl": 1.0, "bloom": False}, "paste undone")
    await ed.call("delete_entities", scene=S["scene"], entities=[target])


async def test_prefab_override(ed: Editor):
    made = await ed.call("create_prefab", scene=S["scene"], entity=S["fluid"], path="prefabs/Fluid.prefab")
    ed.check_eq(made["prefabs"][0]["path"], "prefabs/Fluid.prefab")
    text = (ed.project / "prefabs/Fluid.prefab").read_text()
    ed.check(KEY in text and "seed: 77" in text, f"prefab file lacks the component: {text[:800]}")

    inst = await ed.call("instantiate_prefab", scene=S["scene"], prefab="prefabs/Fluid.prefab", name="Fluid 2")
    second = inst["instance"]["id"]
    check_values(ed, await fluid_values(ed, second), {"curl": 44.0, "seed": 77}, "instance values")

    await ed.call("set_properties", scene=S["scene"], entity=second, values={KEY: {"curl": 9, "output": "screen"}})
    info = await ed.call("get_prefab_info", scene=S["scene"], entity=second)
    ed.check("curl" in str(info.get("overrides")) and "output" in str(info.get("overrides")),
             f"overrides not listed: {info.get('overrides')}")
    # an edit of the prefab reaches the instance, the override stays
    await ed.call("set_properties", scene=S["scene"], entity=S["fluid"], values={KEY: {"bloomIntensity": 0.5}})
    await ed.call("prefab_overrides", action="apply", scene=S["scene"], entity=S["fluid"], component=KEY,
                  properties=["bloomIntensity"])
    v = await fluid_values(ed, second)
    check_values(ed, v, {"bloomIntensity": 0.5, "curl": 9.0, "output": "screen"}, "applied, override kept")
    await ed.call("prefab_overrides", action="revert", scene=S["scene"], entity=second, component=KEY,
                  properties=["curl"])
    check_values(ed, await fluid_values(ed, second), {"curl": 44.0, "output": "screen"}, "reverted one override")
    await ed.call("delete_entities", scene=S["scene"], entities=[second])


async def test_remove_and_undo(ed: Editor):
    await ed.call("remove_component", scene=S["scene"], entity=S["fluid"], component=KEY)
    got = await ed.call("get_entity", scene=S["scene"], entity=S["fluid"], detail="summary")
    ed.check(KEY not in str(got.get("components", "")).lower(), f"still has the component: {got}")
    await ed.call("undo", scene=S["scene"])
    check_values(ed, await fluid_values(ed, S["fluid"]), {"curl": 44.0, "seed": 77}, "remove undone")


async def test_console_and_frames(ed: Editor):
    await ed.call("select_scene", scene=S["scene"])
    await ed.screenshot(view="scene")
    await ed.check_alive()
    errors = [e.get("message", "") for e in await ed.console_since(types=["error"], limit=1000)]
    ed.check(not errors, f"Console errors: {errors}")


# ---------------------------------------------------------------------------- export


async def test_source_export(ed: Editor):
    await ed.call("save_scene", scene=S["scene"])
    s = await ed.call("save_project", untitled_scene_dir="scenes")
    scenes = {sc["name"]: sc for sc in s["scenes_in_build"]} if s.get("scenes_in_build") else {}
    if "Fluids" not in scenes:
        await ed.call("edit_build_scenes", action="add", scene=S["scene"])
    await ed.call("edit_build_scenes", action="set_start", scene=S["scene"])
    out = ed.scratch.root / "export-source"
    r = await ed.call("build_project", mode="source", output_dir=str(out), platform="linux", timeout_seconds=600)
    ed.check_eq(r["status"], "succeeded", f"{r}")

    code = ""
    for path in out.rglob("*.cpp"):
        if "engine" in path.relative_to(out).parts[:1]:
            continue
        text = path.read_text(errors="replace")
        if "FluidSimulationComponent" in text:
            code += text
    ed.check("FluidSimulationComponent fluid;" in code, "the component is constructed")
    ed.check("addComponent<FluidSimulationComponent>" in code, "and added")
    ed.check(re.search(r"fluid\.curl = 44\.0+f;", code), "a non-default value exported")
    ed.check(re.search(r"fluid\.seed = 77u;", code), "the seed exported")
    ed.check("getOutputFramebuffer()" in code and "std::make_shared<FluidSimulator>()" in code,
             "the fluid texture link is bound in the export")
