"""Particle Systems: Unity's modules and the Unity importer (particles spec 9, docs/mcp/guide/particles.md):

- a scene of version 1 (legacy) systems made by main's create_particles plays as on main
  (fixtures/particles/legacy/Legacy.scene, golden/particles_legacy.png, made with the main
  checkout's editor: JASPER_UPDATE_GOLDEN=1 JASPER_EDITOR=<main editor>; deterministic values:
  the legacy model draws from rand(), which Play shares with everything else);
"""

from __future__ import annotations

import os
import shutil
from pathlib import Path

from harness import Editor, find_key, skip

import pngdiff

EDITOR = {"create": True, "name": "ParticlesTest"}
TIMEOUT = 1200

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
GOLDEN_LEGACY = HERE / "golden" / "particles_legacy.png"
LEGACY_FIXTURE = REPO / "tools" / "fixtures" / "particles" / "legacy" / "Legacy.scene"
LEGACY_SCENE = "scenes/Legacy.scene"
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
