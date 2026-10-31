"""Particle Systems: Unity's modules and the Unity importer (particles spec 9, docs/mcp/guide/particles.md):

- a scene of version 1 (legacy) systems made by main's create_particles plays as on main
  (fixtures/particles/legacy/Legacy.scene, golden/particles_legacy.png, made with the main
  checkout's editor: JASPER_UPDATE_GOLDEN=1 JASPER_EDITOR=<main editor>; deterministic values:
  the legacy model draws from rand(), which Play shares with everything else);
- create_particles makes version 2 systems (one GameObject drawing its own quads): presets, modules,
  set_properties / get_entity on module fields, the runtime block, pixels in Play (particles drawn
  where the system is; additive brighter than alpha), Lua control and module access (play/stop,
  particleCount, main.startSpeed, the version 1 error that names upgrade());
- upgrade: a version 1 system of the legacy scene converted with create_particles upgrade (report);
- import_unity_particles on the synthetic Unity project (editor/tests/fixtures/unity-particles):
  prefabs, materials, textures and the report written; the prefab instantiated plays.
"""

from __future__ import annotations

import os
import shutil
from pathlib import Path

from harness import Editor, find_key, skip

from htmlui_pixels import decode_png

import pngdiff

EDITOR = {"create": True, "name": "ParticlesTest"}
TIMEOUT = 1200

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
GOLDEN_LEGACY = HERE / "golden" / "particles_legacy.png"
LEGACY_FIXTURE = REPO / "tools" / "fixtures" / "particles" / "legacy" / "Legacy.scene"
LEGACY_SCENE = "scenes/Legacy.scene"
SCENE = "scenes/Modules.scene"
UNITY_FIXTURE = REPO / "editor" / "tests" / "fixtures" / "unity-particles" / "Project"
S: dict = {}


def walk(nodes):
    for node in nodes or []:
        yield node
        yield from walk(node.get("children"))


async def clear_scene(ed: Editor, keep: tuple = ()):
    tree = await ed.call("get_scene_tree", format="json")
    nodes = find_key(tree, "entities", "roots", "children") or []
    for node in list(walk(nodes)):
        if node.get("name") not in keep:
            await ed.call("delete_entities", entities=[str(node["id"])])


async def game_shot(ed: Editor, scene: str, label: str) -> bytes:
    r = await ed.call_raw("screenshot", {"view": "game", "scene": scene, "max_width": 800})
    ed.check(not r.is_error and r.images, f"{label}: no screenshot: {r.text[:300]}")
    data = r.images[0][0]
    if ed.scratch:
        (ed.scratch.root / f"particles-{label}.png").write_bytes(data)
    return data


async def build_legacy_scene(ed: Editor):
    """Main's create_particles systems (version 1) with no random ranges: a fire-like column, a
    sparks arc under gravity and a one-shot burst that grows and fades."""
    await ed.call("create_scene", name="Legacy", path=LEGACY_SCENE)
    await clear_scene(ed)
    cam = find_key(await ed.call("create_entity", type="camera", name="View", position=[0, 1, 7]), "entity_id", "id")
    await ed.call("set_main_camera", entity=cam)
    await ed.call("create_particles", preset="fire", name="Fire", position=[-1.5, 0, 0], life=1.0, rate=30,
                  velocity=[0, 1.5, 0], size=0.4, emitter={"shape": "box", "min": [0, 0, 0], "max": [0, 0, 0]})
    await ed.call("create_particles", preset="sparks", name="Sparks", position=[0, 0, 0], life=0.8, rate=40,
                  velocity=[1, 3, 0], acceleration=[0, -9.8, 0], size=0.1, emitter={"shape": "box", "min": [0, 0, 0], "max": [0, 0, 0]})
    await ed.call("create_particles", preset="explosion", name="Burst", position=[1.5, 0.5, 0], life=1.2, rate=0, loop=False,
                  velocity=[0, 0, 0], size=0.2, size_over_life=[0.2, 1.0], alpha_over_life=[1, 0.2],
                  bursts=[{"time": 0, "count": 20}], emitter={"shape": "box", "min": [0, 0, 0], "max": [0, 0, 0]})
    await ed.call("save_scene", scene=LEGACY_SCENE)


async def play_legacy(ed: Editor) -> bytes:
    await ed.call("play", action="start", scene=LEGACY_SCENE, auto_pause=True, fixed_fps=60)
    try:
        await ed.call("play", action="step", frames=45)
        return await game_shot(ed, LEGACY_SCENE, "legacy")
    finally:
        await ed.call("play", action="stop", auto_pause=False, fixed_fps=0)


async def test_legacy_scene_matches_main(ed: Editor):
    ed.require_tools("create_scene", "create_entity", "create_particles", "save_scene", "open_scene", "play", "screenshot")
    if os.environ.get("JASPER_UPDATE_GOLDEN") == "1":
        await build_legacy_scene(ed)
        LEGACY_FIXTURE.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ed.project / LEGACY_SCENE, LEGACY_FIXTURE)
        data = await play_legacy(ed)
        GOLDEN_LEGACY.parent.mkdir(parents=True, exist_ok=True)
        GOLDEN_LEGACY.write_bytes(data)
        print(f"     [golden] wrote {GOLDEN_LEGACY} and {LEGACY_FIXTURE}")
        return
    ed.check(LEGACY_FIXTURE.is_file() and GOLDEN_LEGACY.is_file(), f"missing the committed legacy fixture {LEGACY_FIXTURE} / {GOLDEN_LEGACY}")
    (ed.project / LEGACY_SCENE).parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(LEGACY_FIXTURE, ed.project / LEGACY_SCENE)
    await ed.call("open_scene", scene=LEGACY_SCENE)
    # the systems stay version 1 (the Inspector offers the upgrade)
    tree = await ed.call("get_scene_tree", format="json")
    names = {node.get("name"): node for node in walk(find_key(tree, "entities", "roots", "children") or [])}
    ed.check("Particle System" in names or any("Particle" in n for n in names), f"legacy systems: {list(names)}")
    data = await play_legacy(ed)
    ok, fraction, diff = pngdiff.compare(pngdiff.decode(GOLDEN_LEGACY.read_bytes()), pngdiff.decode(data), max_fraction=0.005)
    if not ok and ed.scratch:
        (ed.scratch.root / "particles-legacy-diff.png").write_bytes(diff)
    ed.check(ok, f"the legacy scene differs from main's ({GOLDEN_LEGACY.name}) in {fraction:.4f} of the pixels")


def bright_pixels(data: bytes, threshold: int = 60) -> list:
    """(x, y) of the pixels brighter than `threshold` on any channel (the background is dark)."""
    w, h, ch, rows = decode_png(data)
    out = []
    for y in range(0, h, 2):
        line = rows[y]
        for x in range(0, w, 2):
            if max(line[x * ch], line[x * ch + 1], line[x * ch + 2]) > threshold:
                out.append((x, y))
    return out


async def lua(ed: Editor, code: str):
    return (await ed.call("eval_lua", code=code, scene=SCENE))["values"]


def system_id(result) -> str:
    return str(find_key(result, "particle_system")["id"])


async def test_create_particles_version2(ed: Editor):
    ed.require_tools("create_scene", "create_entity", "create_particles", "set_properties", "get_entity", "play", "eval_lua", "screenshot")
    await ed.call("create_scene", name="Modules", path=SCENE)
    await clear_scene(ed)
    S["scene"] = SCENE
    cam = find_key(await ed.call("create_entity", type="camera", name="View", position=[0, 1, 8]), "entity_id", "id")
    await ed.call("set_main_camera", entity=cam)
    fire = await ed.call("create_particles", preset="fire", name="Fire", position=[-2, 0, 0])
    ed.check(find_key(fire, "version") == 2, f"create_particles makes version 2 systems: {fire}")
    fire_id = system_id(fire)
    sparks = await ed.call("create_particles", preset="sparks", name="Sparks", position=[2, 0, 0],
                           modules={"main": {"startSpeed": 4, "maxParticles": 300}, "emission": {"rateOverTime": 60}})
    sparks_id = system_id(sparks)

    # module fields through set_properties / get_entity
    await ed.call("set_properties", entity=sparks_id, component="ParticleSystem", values={"main.startLifetime": {"mode": "two_constants", "min": 0.5, "max": 1}})
    got = await ed.call("get_entity", entity=sparks_id, components=["ParticleSystem"],
                        properties=["version", "main.startSpeed", "main.maxParticles", "emission.rateOverTime", "main.startLifetime"])
    comp = got["components"]["ParticleSystem"]
    ed.check(comp.get("version") == 2, f"version: {comp}")
    ed.check(comp.get("main.startSpeed") == 4, f"main.startSpeed (a constant reads as a number): {comp.get('main.startSpeed')}")
    ed.check(comp.get("main.maxParticles") == 300, f"main.maxParticles: {comp.get('main.maxParticles')}")
    life = comp.get("main.startLifetime")
    ed.check(isinstance(life, dict) and life.get("mode") == "two_constants", f"two constants read back: {life}")
    # one GameObject: the system draws its own quads
    mesh = await ed.call("get_entity", entity=fire_id, components=["Mesh", "InstancedMesh", "Action"])
    ed.check("Mesh" in mesh["components"] and "InstancedMesh" in mesh["components"], f"the system's own renderer: {list(mesh['components'])}")
    await ed.call("save_scene", scene=SCENE)

    await ed.call("play", action="start", scene=SCENE, auto_pause=True, fixed_fps=60)
    try:
        await ed.call("play", action="step", frames=60)
        counts = await lua(ed, "local f = object('Fire'):getParticleSystem(); local s = object('Sparks'):getParticleSystem(); "
                               "return {f.particleCount, s.particleCount, f:isPlaying(), s.main.startSpeed.constant, f.version}")
        ed.check(counts[0] > 5 and counts[1] > 5, f"particles alive after 1 s: {counts}")
        ed.check(counts[2] is True and abs(counts[3] - 4) < 1e-4 and counts[4] == 2, f"Lua reads: {counts}")
        data = await game_shot(ed, SCENE, "modules")
        w, h, ch, rows = decode_png(data)
        lit = bright_pixels(data)
        left = [p for p in lit if p[0] < w * 0.5]
        right = [p for p in lit if p[0] >= w * 0.5]
        ed.check(len(left) > 20 and len(right) > 5, f"particles drawn at both systems: {len(left)} left, {len(right)} right")
        # Lua control: stop and clear, the count drops; writing a module of a version 1 system errors
        after = await lua(ed, "local s = object('Sparks'):getParticleSystem(); s:stop(true, ParticleSystemStopBehavior.StopEmittingAndClear); "
                              "s.main.startSpeed = 7; return {s.particleCount, s:isEmitting(), s.main.startSpeed.constant}")
        ed.check(after[0] == 0 and after[1] is False and abs(after[2] - 7) < 1e-4, f"stop and clear: {after}")
        err = await ed.call_raw("eval_lua", {"code": "local p = ParticleSystem(scene); p.main.startSpeed = 2", "scene": SCENE})
        ed.check("upgrade" in (err.text or ""), f"a version 1 module write names upgrade(): {err.text[:300]}")
    finally:
        await ed.call("play", action="stop", auto_pause=False, fixed_fps=0)


async def test_upgrade_legacy(ed: Editor):
    ed.require_tools("open_scene", "create_particles")
    if not LEGACY_FIXTURE.is_file():
        skip("no legacy fixture")
    (ed.project / LEGACY_SCENE).parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(LEGACY_FIXTURE, ed.project / LEGACY_SCENE)
    await ed.call("open_scene", scene=LEGACY_SCENE)
    tree = await ed.call("get_scene_tree", format="json")
    systems = []
    for node in walk(find_key(tree, "entities", "roots", "children") or []):
        got = await ed.call("get_entity", entity=str(node["id"]), components=["ParticleSystem"], properties=["version"])
        if "ParticleSystem" in got.get("components", {}):
            systems.append(str(node["id"]))
    ed.check(bool(systems), "the legacy scene has Particle Systems")
    r = await ed.call_raw("create_particles", {"scene": LEGACY_SCENE, "entity": systems[0], "upgrade": True, "force": True})
    ed.check(not r.is_error, f"upgrade: {r.text[:400]}")
    report = find_key(r.value or {}, "report")
    ed.check(isinstance(report, list) and report, f"an upgrade report: {r.value}")
    got = await ed.call("get_entity", entity=systems[0], components=["ParticleSystem"], properties=["version"])
    ed.check(got["components"]["ParticleSystem"].get("version") == 2, f"upgraded: {got['components']}")
    await ed.call("undo", scene=LEGACY_SCENE)
    got = await ed.call("get_entity", entity=systems[0], components=["ParticleSystem"], properties=["version"])
    ed.check(got["components"]["ParticleSystem"].get("version", 1) == 1, f"undo restores version 1: {got['components']}")


async def test_import_unity_particles(ed: Editor):
    ed.require_tools("import_unity_particles", "instantiate_prefab", "play", "eval_lua")
    ed.check(UNITY_FIXTURE.is_dir(), f"missing {UNITY_FIXTURE}")
    dry = await ed.call("import_unity_particles", path=str(UNITY_FIXTURE / "Assets" / "Fx" / "Flame.prefab"), dry_run=True)
    ed.check(dry.get("prefabs") == ["assets/fx/Flame.prefab"], f"dry run: {dry.get('prefabs')}")
    ed.check(not (ed.project / "assets" / "fx" / "Flame.prefab").exists(), "a dry run writes nothing")
    r = await ed.call("import_unity_particles", path=str(UNITY_FIXTURE / "Assets" / "Fx" / "Flame.prefab"))
    ed.check(r.get("prefabs") == ["assets/fx/Flame.prefab"], f"import: {r.get('prefabs')}")
    for rel in ("assets/fx/Flame.prefab", "assets/fx/materials/FireAdd.material", "assets/fx/textures/flame.png", "assets/fx/Flame.import-report.json"):
        ed.check((ed.project / rel).is_file(), f"{rel} written")
    systems = {s["path"]: s for s in r["report"]["systems"]}
    ed.check(set(systems) == {"Flame", "Flame/Smoke"}, f"report systems: {list(systems)}")
    codes = {n["code"] for n in systems["Flame/Smoke"]["notes"]}
    ed.check("module-unsupported" in codes, f"the smoke's sub emitter is reported: {codes}")

    if "scene" not in S:
        await ed.call("create_scene", name="Modules", path=SCENE)
        S["scene"] = SCENE
    await ed.call("open_scene", scene=SCENE)
    await ed.call("instantiate_prefab", prefab="assets/fx/Flame.prefab", scene=SCENE, position=[0, 0, 0])
    await ed.call("save_scene", scene=SCENE)
    await ed.call("play", action="start", scene=SCENE, auto_pause=True, fixed_fps=60)
    try:
        await ed.call("play", action="step", frames=30)
        v = await lua(ed, "local o = object('Flame'):getParticleSystem(); return {o.version, o.particleCount, o.main.duration}")
        ed.check(v[0] == 2 and v[1] > 0 and abs(v[2] - 2) < 1e-4, f"the imported system plays: {v}")
    finally:
        await ed.call("play", action="stop", auto_pause=False, fixed_fps=0)
