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
import struct
import zlib
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
    nodes = find_key(tree, "nodes", "entities", "roots", "children") or []
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
    names = {node.get("name"): node for node in walk(find_key(tree, "nodes", "entities", "roots", "children") or [])}
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
    return (await ed.call("eval_lua", code=code, scene=SCENE))["values"][0]


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
    await ed.call("set_properties", entity=sparks_id, values={"particlesystem": {"main.startLifetime": {"mode": "two_constants", "min": 0.5, "max": 1}}})
    got = await ed.call("get_entity", entity=sparks_id, components=["particlesystem"],
                        properties=["version", "main.startSpeed", "main.maxParticles", "emission.rateOverTime", "main.startLifetime"])
    comp = got["components"]["particlesystem"]
    ed.check(comp.get("version") == 2, f"version: {comp}")
    ed.check(comp.get("main.startSpeed") == 4, f"main.startSpeed (a constant reads as a number): {comp.get('main.startSpeed')}")
    ed.check(comp.get("main.maxParticles") == 300, f"main.maxParticles: {comp.get('main.maxParticles')}")
    life = comp.get("main.startLifetime")
    ed.check(isinstance(life, dict) and life.get("mode") == "two_constants", f"two constants read back: {life}")
    # one GameObject: the system draws its own quads
    mesh = await ed.call("get_entity", entity=fire_id, detail="summary")
    names = str(mesh.get("components"))
    ed.check("mesh" in names and "instancedmesh" in names, f"the system's own renderer: {mesh.get('components')}")
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
    systems = [str(node["id"]) for node in walk(find_key(tree, "nodes", "entities", "roots", "children") or [])
               if "particlesystem" in (node.get("components") or [])]
    ed.check(bool(systems), "the legacy scene has Particle Systems")
    r = await ed.call_raw("create_particles", {"scene": LEGACY_SCENE, "entity": systems[0], "upgrade": True, "force": True})
    ed.check(not r.is_error, f"upgrade: {r.text[:400]}")
    report = find_key(r.value or {}, "report")
    ed.check(isinstance(report, list) and report, f"an upgrade report: {r.value}")
    got = await ed.call("get_entity", entity=systems[0], components=["particlesystem"], properties=["version"])
    ed.check(got["components"]["particlesystem"].get("version") == 2, f"upgraded: {got['components']}")
    await ed.call("undo", scene=LEGACY_SCENE)
    got = await ed.call("get_entity", entity=systems[0], components=["particlesystem"], properties=["version"])
    ed.check(got["components"]["particlesystem"].get("version", 1) == 1, f"undo restores version 1: {got['components']}")


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


# ---------------------------------------------------------------------------- pixels (spec 9)

PIXEL_SCENE = "scenes/Pixels.scene"
SHEET = "assets/textures/sheet2x2.png"
SHEET_COLOURS = [(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0)]   # frames 0..3: TL, TR, BL, BR


def png_rgb(width: int, height: int, pixels) -> bytes:
    raw = b"".join(b"\x00" + bytes(c for px in row for c in px) for row in pixels)

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def lit_extent(data: bytes, threshold: int = 70):
    """(x0, y0, x1, y1, count) of the pixels brighter than the dark background."""
    w, h, ch, rows = decode_png(data)
    xs, ys = [], []
    for y in range(h):
        line = rows[y]
        for x in range(w):
            if max(line[x * ch], line[x * ch + 1], line[x * ch + 2]) > threshold:
                xs.append(x)
                ys.append(y)
    if not xs:
        return None
    return min(xs), min(ys), max(xs), max(ys), len(xs)


def centre_rgb(data: bytes):
    w, h, ch, rows = decode_png(data)
    line = rows[h // 2]
    return tuple(line[(w // 2) * ch:(w // 2) * ch + 3])


async def pixel_scene(ed: Editor, camera: list, target: list = (0, 0, 0)):
    """An empty scene, its camera at `camera` looking at `target`."""
    await ed.call("create_scene", name="Pixels", path=PIXEL_SCENE)
    await clear_scene(ed)
    cam = find_key(await ed.call("create_entity", type="camera", name="View", position=list(camera)), "entity_id", "id")
    await ed.call("set_main_camera", entity=cam)
    await ed.call("set_properties", entity=str(cam), values={"camera": {"target": list(target), "useTarget": True}})
    return str(cam)


async def one_particle(ed: Editor, name: str, modules: dict, texture: str | None = None) -> str:
    """A still particle at the origin (one burst, no speed, a long life), plus `modules`."""
    base = {"main": {"startSpeed": 0, "startLifetime": 100, "startSize": 1, "startColor": [1, 1, 1, 1], "duration": 100},
            "shape": {"enabled": False}, "emission": {"rateOverTime": 0, "bursts": [{"time": 0, "count": 1}]}}
    for module, fields in modules.items():
        base.setdefault(module, {}).update(fields)
    args = {"preset": "none", "name": name, "position": [0, 0, 0], "modules": base}
    if texture:
        args["texture"] = texture
    return system_id(await ed.call("create_particles", **args))


async def play_shot(ed: Editor, label: str, frames: int = 10, camera: str | None = None, positions: list | None = None) -> list:
    """Shots of PIXEL_SCENE in Play after `frames`; with `positions`, one per camera position."""
    await ed.call("save_scene", scene=PIXEL_SCENE)
    await ed.call("play", action="start", scene=PIXEL_SCENE, auto_pause=True, fixed_fps=60)
    shots = []
    try:
        await ed.call("play", action="step", frames=frames)
        for i, position in enumerate(positions or [None]):
            if position is not None:
                await ed.call("eval_lua", scene=PIXEL_SCENE,
                              code=f"local c = object('View', 'Camera'); c.position = Vector3({position[0]}, {position[1]}, {position[2]})")
                await ed.call("play", action="step", frames=2)
            shots.append(await game_shot(ed, PIXEL_SCENE, f"{label}-{i}"))
    finally:
        await ed.call("play", action="stop", auto_pause=False, fixed_fps=0)
    return shots


async def test_render_modes_pixels(ed: Editor):
    ed.require_tools("create_scene", "create_entity", "create_particles", "set_properties", "play", "eval_lua", "screenshot")
    # a billboard faces the camera wherever it orbits: about as wide as high at every angle
    await pixel_scene(ed, [0, 0, 6])
    await one_particle(ed, "Billboard", {"renderer": {"renderMode": "billboard"}})
    shots = await play_shot(ed, "billboard", positions=[[0, 0, 6], [4.2, 0, 4.2], [6, 0, 0.3], [0, 4.2, 4.2]])
    for i, data in enumerate(shots):
        e = lit_extent(data)
        ed.check(e is not None, f"billboard {i}: nothing drawn")
        w, h = e[2] - e[0] + 1, e[3] - e[1] + 1
        ed.check(0.75 < w / h < 1.33 and w > 20, f"billboard {i} faces the camera: {w}x{h}")

    # horizontal: flat on the ground, full seen from above, a sliver at a grazing angle
    await pixel_scene(ed, [0, 6, 0.01])
    await one_particle(ed, "Flat", {"renderer": {"renderMode": "horizontal_billboard"}})
    top, side = await play_shot(ed, "horizontal", positions=[[0, 6, 0.01], [0, 0.4, 6]])
    et, es = lit_extent(top), lit_extent(side)
    ed.check(et is not None and (et[3] - et[1]) > 20, f"horizontal from above: {et}")
    ed.check(es is None or (es[3] - es[1] + 1) * 3 < (es[2] - es[0] + 1), f"horizontal edge-on is thin: {es}")

    # vertical: upright, turned about Y only: from high above it is foreshortened, a billboard is not
    await pixel_scene(ed, [0, 5, 1.5])
    await one_particle(ed, "Upright", {"renderer": {"renderMode": "vertical_billboard"}})
    [high] = await play_shot(ed, "vertical")
    ev = lit_extent(high)
    ed.check(ev is not None, "vertical: nothing drawn")
    ed.check((ev[3] - ev[1] + 1) * 1.8 < (ev[2] - ev[0] + 1), f"a vertical billboard seen from above is short: {ev}")

    # stretched: along its velocity, longer than wide by more than 2
    await pixel_scene(ed, [0, 0, 8])
    await one_particle(ed, "Streak", {"renderer": {"renderMode": "stretch", "lengthScale": 3, "velocityScale": 0.2},
                                      "velocityOverLifetime": {"enabled": True, "x": 0.5, "space": "world"}})
    [streak] = await play_shot(ed, "stretched", frames=5)
    es = lit_extent(streak)
    ed.check(es is not None, "stretched: nothing drawn")
    ed.check((es[2] - es[0] + 1) > 2 * (es[3] - es[1] + 1), f"stretched aspect > 2: {es}")

    # mesh: a cube per particle (a square silhouette, filled, unlike the soft disc)
    await pixel_scene(ed, [0, 0, 6])
    await one_particle(ed, "Cubes", {"renderer": {"renderMode": "mesh", "mesh": "cube", "alignment": "world"}})
    [cube] = await play_shot(ed, "mesh")
    ec = lit_extent(cube)
    ed.check(ec is not None, "mesh: nothing drawn")
    area = (ec[2] - ec[0] + 1) * (ec[3] - ec[1] + 1)
    ed.check(ec[4] > 0.9 * area, f"a cube fills its square ({ec[4]} of {area} pixels)")


async def test_texture_sheet_fps(ed: Editor):
    ed.require_tools("create_scene", "create_particles", "play", "screenshot")
    (ed.project / SHEET).parent.mkdir(parents=True, exist_ok=True)
    rows = []
    for y in range(64):
        rows.append([SHEET_COLOURS[(0 if x < 32 else 1) + (0 if y < 32 else 2)] for x in range(64)])
    (ed.project / SHEET).write_bytes(png_rgb(64, 64, rows))
    await pixel_scene(ed, [0, 0, 4])
    await one_particle(ed, "Sheet", {"textureSheetAnimation": {"enabled": True, "numTilesX": 2, "numTilesY": 2, "timeMode": "fps", "fps": 1},
                                     "main": {"startSize": 2}}, texture=SHEET)
    await ed.call("save_scene", scene=PIXEL_SCENE)
    await ed.call("play", action="start", scene=PIXEL_SCENE, auto_pause=True, fixed_fps=10)
    seen = []
    try:
        for second in range(4):
            await ed.call("play", action="step", frames=5 if second == 0 else 10)
            seen.append(centre_rgb(await game_shot(ed, PIXEL_SCENE, f"sheet-{second}")))
    finally:
        await ed.call("play", action="stop", auto_pause=False, fixed_fps=0)

    def nearest(rgb):
        return min(range(4), key=lambda i: sum((a - b) ** 2 for a, b in zip(rgb, SHEET_COLOURS[i])))

    frames = [nearest(c) for c in seen]
    ed.check(frames == [0, 1, 2, 3], f"one frame a second, in order (TL, TR, BL, BR): {frames} from {seen}")


async def test_age_sorting(ed: Editor):
    ed.require_tools("create_scene", "create_particles", "play", "screenshot")
    # two particles at the same place: the first red, the second (0.5 s later) blue
    results = {}
    for mode in ("oldest_in_front", "youngest_in_front"):
        await pixel_scene(ed, [0, 0, 5])
        await one_particle(ed, "Pair", {
            # start colour by the system's time (fixed keys): red at 0 s, blue at 0.5 s of 100
            "main": {"startColor": {"mode": "gradient", "gradient": {"mode": "fixed", "colors": [[0.004, [1, 0, 0]], [1, [0, 0, 1]]]}}},
            "emission": {"bursts": [{"time": 0, "count": 1}, {"time": 0.5, "count": 1}]},
            "renderer": {"sortMode": mode}})
        [shot] = await play_shot(ed, mode, frames=50)
        results[mode] = centre_rgb(shot)
    old, young = results["oldest_in_front"], results["youngest_in_front"]
    ed.check(old[0] > old[2] + 60, f"oldest in front: the first (red) on top: {old}")
    ed.check(young[2] > young[0] + 60, f"youngest in front: the second (blue) on top: {young}")


async def test_bench_script(ed: Editor):
    """The benchmark fixture's script (tools/fixtures/particles/bench): 2000 particles alive with
    six modules, and the frame stats line it is measured by."""
    ed.require_tools("create_scene", "create_entity", "attach_script", "play", "eval_lua")
    src = REPO / "tools" / "fixtures" / "particles" / "bench" / "scripts" / "ParticlesBench.lua"
    ed.check(src.is_file(), f"missing {src}")
    (ed.project / "scripts").mkdir(parents=True, exist_ok=True)
    shutil.copyfile(src, ed.project / "scripts" / "ParticlesBench.lua")
    await ed.call("create_scene", name="Bench", path="scenes/Bench.scene")
    await clear_scene(ed)
    game = find_key(await ed.call("create_entity", type="empty", name="Game"), "entity_id", "id")
    await ed.call("attach_script", entity=str(game), path="scripts/ParticlesBench.lua")
    await ed.call("save_scene", scene="scenes/Bench.scene")
    await ed.call("play", action="start", scene="scenes/Bench.scene", auto_pause=True, fixed_fps=60)
    try:
        await ed.call("play", action="step", frames=180)
        alive = (await ed.call("eval_lua", scene="scenes/Bench.scene", code=(
            "local n = 0; for _, o in ipairs(script('Game').systems) do n = n + o.particleCount end; return n")))["values"][0]
        ed.check(1800 <= alive <= 2100, f"the bench keeps ~2000 particles alive: {alive}")
    finally:
        await ed.call("play", action="stop", auto_pause=False, fixed_fps=0)
