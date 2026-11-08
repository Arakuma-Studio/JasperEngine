"""Saved files do not depend on what was drawn or loaded (spec S63).

- test_fixture_scenes_save_the_same_drawn_or_not (R3, the guard): every scene of every project of
  tools/fixtures is opened and saved in one batch (no frame: nothing drawn, no texture loaded),
  drawn in both views with its assets loaded (a screenshot waits for them), and saved again. The
  scene file and every generated source must not change by a byte. A difference is fixed, or the
  fixture holds what the first draw settles and the spec's Deviations say why: none is left silent.
- test_sprite_frames_generate_as_authored (R1): a Sprite whose frames are in pixels generates those
  pixels, with its texture unloaded, held back on its worker or loaded.
- test_weather_map_wrap_stays_authored (R2): a draw does not write a Volumetric Clouds' weather map.

test_save_determinism.py holds the cases found before this guard, each on its own scene.
Oracles: the files on disk, byte for byte.
"""

from __future__ import annotations

import shutil
from pathlib import Path

import yaml

import pngdiff
from harness import Editor
from scene_history import FIXTURES, first_difference

CI = True
EDITOR = {"create": True, "name": "Saved Output CI"}
TIMEOUT = 2400

PARITY = FIXTURES / "reflection-parity"
TEXTURE_DELAY_ENV = "JASPER_TEXTURE_LOAD_DELAY_MS"
FRAMES_AFTER_DRAW = 10
FRAME_TIMEOUT = 300
SHOT_TIMEOUT = 300


def fixture_projects() -> list[Path]:
    projects = sorted(p.parent for p in FIXTURES.glob("*/project.yaml") if any(p.parent.rglob("*.scene")))
    if (PARITY / "project.yaml").exists() and PARITY not in projects:
        projects.append(PARITY)
    return projects


def scenes_of(project: Path) -> list[str]:
    return sorted(p.relative_to(project).as_posix() for p in project.rglob("*.scene") if ".jasperengine" not in p.parts)


def generated_files(project: Path) -> dict[str, bytes]:
    """{relative path: bytes} of what the editor generated for the project's code"""
    root = project / ".jasperengine" / "generated"
    return {p.relative_to(root).as_posix(): p.read_bytes() for p in sorted(root.rglob("*")) if p.is_file()} if root.is_dir() else {}


def differences(before: dict[str, bytes], after: dict[str, bytes]) -> list[str]:
    """What changed between two saves. A file that appeared is no finding: a frame may write the
    sources of the project's other scenes (Project::writeMissingSceneSources), and it is compared
    when its scene's turn comes; each scene's own source exists after its first save."""
    out = []
    for name in sorted(before):
        if name not in after:
            out.append(f"{name}: disappeared")
        elif before[name] != after[name]:
            out.append(f"{name}:\n{first_difference(before[name], after[name])}")
    return out


async def gpu_creation_idle(ed: Editor, polls: int = 300) -> None:
    for _ in range(polls):
        stats = (await ed.call("get_render_stats"))["shader_creation"]
        if stats["pending_shaders"] == 0 and stats["pending_pipelines"] == 0 and stats["pending_warm_draws"] == 0:
            return
        await ed.call("wait", frames=10, timeout_seconds=FRAME_TIMEOUT)


async def open_and_save_undrawn(ed: Editor, rel: str) -> None:
    """Opens and saves a scene in one batch: no frame has run, so nothing was drawn or loaded"""
    r = await ed.call("batch", calls=[{"tool": "open_scene", "arguments": {"scene": rel, "close_previous": True}},
                                      {"tool": "save_scene", "arguments": {"scene": rel, "allow_data_loss": True}}])
    ed.check_eq(r.get("failed"), 0, f"{rel}: opened and saved in one batch: {[x for x in r['results'] if not x.get('ok')]}")


async def draw_with_assets(ed: Editor, sid) -> None:
    """Both views drawn with every texture and model loaded (a screenshot waits for them)"""
    for view in ("scene", "game"):
        shot = await ed.call_raw("screenshot", {"view": view, "scene": sid, "max_width": 320, "timeout_seconds": SHOT_TIMEOUT})
        ed.check(not shot.is_error, f"the {view} view draws: {shot.text[:300]}")
    await ed.call("wait", frames=FRAMES_AFTER_DRAW, timeout_seconds=FRAME_TIMEOUT)
    await gpu_creation_idle(ed)


async def test_fixture_scenes_save_the_same_drawn_or_not():
    """R3. Every finding of every scene is reported at once, not only the first"""
    findings: list[str] = []
    compared = 0
    for fixture in fixture_projects():
        async with Editor(project=fixture, label=f"guard-{fixture.name}") as ed:
            for rel in scenes_of(ed.project):
                await open_and_save_undrawn(ed, rel)
                path = ed.project / rel
                undrawn, undrawn_sources = path.read_bytes(), generated_files(ed.project)
                await draw_with_assets(ed, rel)
                await ed.call("save_scene", scene=rel, allow_data_loss=True)
                found = differences({rel: undrawn}, {rel: path.read_bytes()}) + differences(undrawn_sources, generated_files(ed.project))
                compared += 1
                if found:
                    print(f"[guard] {fixture.name}/{rel}: DIFFERS\n" + "\n".join(found))
                    findings.append(f"{fixture.name}/{rel}: " + "\n".join(found))
                else:
                    print(f"[guard] {fixture.name}/{rel}: same bytes")
    print(f"[guard] {compared} scene(s) compared, {len(findings)} differ")
    assert compared > 0, "the guard compared no scene: a filter that matches nothing is not a pass"
    assert not findings, "saved output depends on what was drawn or loaded:\n" + "\n\n".join(findings)


# -- R1: sprite frames ---------------------------------------------------------------------------------


SHEET = "assets/sheet.png"
SHEET_SIZE = (64, 32)


def sheet_png() -> bytes:
    width, height = SHEET_SIZE
    # two 32 x 32 cells: red and green
    pixels = bytearray()
    for _ in range(height):
        for x in range(width):
            pixels += bytes((255, 0, 0, 255) if x < 32 else (0, 255, 0, 255))
    return pngdiff.encode(width, height, bytes(pixels))


def scene_source(project: Path, scene: str) -> str:
    """The generated source of a scene (Factory::sceneToFileName: scene_<name>_<id>.cpp)"""
    sources = [data.decode(errors="replace") for name, data in generated_files(project).items() if name.startswith(f"scene_{scene}_") and name.endswith(".cpp")]
    assert len(sources) == 1, f"one generated source for {scene}: {sorted(generated_files(project))}"
    return sources[0]


async def test_sprite_frames_generate_as_authored(ed: Editor):
    """A Sprite with frames in pixels: drawn and saved, opened and saved in one batch (its texture not
    loaded), drawn and saved again. The same file and generated code each time, with the pixels the
    user authored. A later editor holding the texture back on its worker generates the same."""
    (ed.project / SHEET).parent.mkdir(parents=True, exist_ok=True)
    (ed.project / SHEET).write_bytes(sheet_png())
    rel = "Frames.scene"
    path = ed.project / rel
    sid = (await ed.call("create_scene", name="Frames", type="2D", path=rel))["scene"]["id"]
    await ed.call("set_sprite_frames", scene=sid, create={"name": "Hero"}, texture=SHEET,
                  frames=[{"name": "red", "rect": [0, 0, 32, 32]}, {"name": "green", "rect": [32, 0, 32, 32]}], frame=1)
    await draw_with_assets(ed, sid)
    await ed.call("save_scene", scene=sid)
    drawn, drawn_code = path.read_bytes(), scene_source(ed.project, "Frames")

    # the authored pixels, in the file and in the code, whatever the load state
    frames = next(e for e in yaml.safe_load(drawn)["entities"] if e.get("name") == "Hero")["components"]["sprite"]["framesRect"]
    ed.check_eq([f["rect"] for f in frames], [[0, 0, 32, 32], [32, 0, 32, 32]], "the file holds the authored rects")
    ed.check_in("sprite.framesRect[1].rect = Rect(32", drawn_code, "the code holds the authored rect, in pixels")

    await ed.call("close_scene", scene=sid)
    await open_and_save_undrawn(ed, rel)
    ed.check(path.read_bytes() == drawn, f"the file, its texture not loaded: {first_difference(drawn, path.read_bytes())}")
    undrawn_code = scene_source(ed.project, "Frames")
    ed.check(undrawn_code == drawn_code, f"the code, its texture not loaded: {first_difference(drawn_code.encode(), undrawn_code.encode())}")

    await draw_with_assets(ed, sid)
    await ed.call("save_scene", scene=sid)
    ed.check(path.read_bytes() == drawn, f"the file, drawn again: {first_difference(drawn, path.read_bytes())}")
    again = scene_source(ed.project, "Frames")
    ed.check(again == drawn_code, f"the code, drawn again: {first_difference(drawn_code.encode(), again.encode())}")


async def test_a_save_while_the_texture_loads_generates_the_same(ed: Editor):
    """A save with the sheet's decode held on its worker (a frame ran, the size is not known yet)
    writes what a save with it loaded writes"""
    (ed.project / SHEET).parent.mkdir(parents=True, exist_ok=True)
    (ed.project / SHEET).write_bytes(sheet_png())
    rel = "Held.scene"
    path = ed.project / rel
    sid = (await ed.call("create_scene", name="Held", type="2D", path=rel))["scene"]["id"]
    await ed.call("set_sprite_frames", scene=sid, create={"name": "Hero"}, texture=SHEET, frames=[{"rect": [32, 0, 32, 32]}], frame=0)
    await draw_with_assets(ed, sid)
    await ed.call("save_scene", scene=sid)
    loaded, loaded_code = path.read_bytes(), scene_source(ed.project, "Held")
    await ed.call("save_project")
    project_copy = ed.scratch.root / "held-project"
    shutil.copytree(ed.project, project_copy, ignore=shutil.ignore_patterns("editor.lock", "*.je-tmp"))

    async with Editor(project=str(project_copy), label="held-texture", env={TEXTURE_DELAY_ENV: "12000"}) as held:
        await held.call("create_scene", name="Holder", type="2D")
        for scene in (await held.call("list_scenes"))["scenes"]:
            if scene.get("path") == rel and scene.get("opened"):
                await held.call("close_scene", scene=scene["id"], unsaved="discard")
        sid = (await held.call("open_scene", scene=rel))["scene"]["id"]
        await held.call("wait", frames=5)
        await held.call("save_scene", scene=sid)
        # the save met it loading: a screenshot, which waits for textures, gives up naming it
        shot = await held.call_raw("screenshot", {"view": "game", "scene": sid, "timeout_seconds": 1})
        held.check(shot.is_error and "texture load" in shot.text, f"the sheet was still loading: {shot.text[:300]}")
        got = (held.project / rel).read_bytes()
        held.check(got == loaded, f"the file, the texture still loading: {first_difference(loaded, got)}")
        code = scene_source(held.project, "Held")
        held.check(code == loaded_code, f"the code, the texture still loading: {first_difference(loaded_code.encode(), code.encode())}")


# -- a Terrain's maps ---------------------------------------------------------------------------------


def terrain_maps_with_wrap(text: str, wrap: str) -> str:
    """The scene with this wrap on its Terrain's height and blend maps only (not its surface layers)"""
    lines = text.split("\n")
    out = []
    block = None
    for line in lines:
        stripped = line.strip()
        indent = len(line) - len(line.lstrip())
        if stripped in ("heightMap:", "blendMaps:"):
            block = indent
        elif block is not None and stripped and indent <= block:
            block = None
        if block is not None and stripped.startswith(("wrapU: ", "wrapV: ")):
            line = line[:indent] + stripped.split(":")[0] + ": " + wrap
        out.append(line)
    return "\n".join(out)


async def test_terrain_maps_draw_the_same_whatever_wrap_an_old_file_says():
    """An old scene's height and blend maps, Clamp or Repeat in the file, draw the same: a Terrain has
    always drawn them clamped (RenderSystem::loadTerrainTextures, before and after S63, which also
    applies that on load and before a save). Hiding the Terrain changes the image, so it is in it."""
    rel = "Hex.scene"
    async with Editor(project=FIXTURES / "pbr-extended-export", label="terrain-maps") as ed:
        path = ed.project / rel
        authored = path.read_text()
        ed.check("wrapU: repeat" in authored, "the fixture's maps say Repeat, as an old file does")
        shots = {}
        for wrap in ("repeat", "clamp_to_edge", "repeat"):
            await ed.call("create_scene", name="Holder", type="2D", path=f"Holder-{len(shots)}-{wrap}.scene")
            for scene in (await ed.call("list_scenes"))["scenes"]:
                if scene.get("path") == rel and scene.get("opened"):
                    await ed.call("close_scene", scene=scene["id"], unsaved="discard")
            path.write_text(terrain_maps_with_wrap(authored, wrap))
            sid = (await ed.call("open_scene", scene=rel))["scene"]["id"]
            await draw_with_assets(ed, sid)
            shot = await ed.call_raw("screenshot", {"view": "game", "scene": sid, "max_width": 640, "timeout_seconds": SHOT_TIMEOUT})
            ed.check(not shot.is_error, f"the view draws: {shot.text[:300]}")
            first_shot = shots.setdefault(wrap, shot.images[0][0])
            ed.check_eq(first_shot, shot.images[0][0], f"{wrap}: the same file draws the same when opened again")
        ed.check_eq(shots["clamp_to_edge"], shots["repeat"], "a Clamp file draws what a Repeat file draws")
        await ed.call("set_properties", scene=sid, entity="Terrain", values={"transform": {"visible": False}})
        await draw_with_assets(ed, sid)
        hidden = await ed.call_raw("screenshot", {"view": "game", "scene": sid, "max_width": 640, "timeout_seconds": SHOT_TIMEOUT})
        ed.check(not hidden.is_error and hidden.images[0][0] != shots["repeat"], "the Terrain is in the image the comparison looks at")


# -- R2: the weather map's wrap --------------------------------------------------------------------------


def with_clamped_weather_map(text: str, path: str) -> str:
    """The Clouds' weatherMap block of the file with clamp_to_edge wraps, as an authored setting"""
    data = yaml.safe_load(text)
    clouds = next(e for e in data["entities"] if e.get("name") == "Clouds")["components"]["volumetricclouds"]
    assert clouds["weatherMap"]["path"] == path, clouds["weatherMap"]
    lines = text.split("\n")
    start = next(i for i, line in enumerate(lines) if line.strip() == "weatherMap:")
    for i in range(start, start + 12):
        lines[i] = lines[i].replace("wrapU: repeat", "wrapU: clamp_to_edge").replace("wrapV: repeat", "wrapV: clamp_to_edge")
    return "\n".join(lines)


async def test_weather_map_wrap_stays_authored(ed: Editor):
    """A Volumetric Clouds' weather map authored with Clamp: the wrap stays in the component, in the
    file and in the generated code after the clouds were drawn (they tile whatever the wrap:
    AtmosphereGpu.AWeatherMapTilesWhateverItsWrap)"""
    weather = "assets/weather.png"
    (ed.project / weather).parent.mkdir(parents=True, exist_ok=True)
    (ed.project / weather).write_bytes(pngdiff.encode(8, 8, bytes([200, 200, 200, 255]) * 64))
    rel = "Weather.scene"
    path = ed.project / rel
    sid = (await ed.call("create_scene", name="Weather", type="3D", path=rel))["scene"]["id"]
    clouds = (await ed.call("create_entity", scene=sid, type="volumetric_clouds", name="Clouds"))["entity_id"]
    await ed.call("set_properties", scene=sid, entity=clouds, values={"volumetricclouds": {"weatherMap": weather}})
    await ed.call("save_scene", scene=sid)
    await ed.call("close_scene", scene=sid)
    path.write_text(with_clamped_weather_map(path.read_text(), weather))
    authored = path.read_bytes()

    await open_and_save_undrawn(ed, rel)
    undrawn = path.read_bytes()
    ed.check_in("wrapU: clamp_to_edge", undrawn.decode(), "the authored wrap is loaded and saved")
    code = scene_source(ed.project, "Weather")
    ed.check_in("TextureWrap::CLAMP_TO_EDGE", code, "the authored wrap is generated")

    # until the renderer has drawn them: the noise bake, then the clouds' passes
    for _ in range(60):
        await draw_with_assets(ed, sid)
        info = await ed.call("get_atmosphere_info", scene=sid, view="game")
        if info["clouds"]["noiseReady"] and info["clouds"]["active"]:
            break
    ed.check(info["clouds"]["active"] and info["clouds"]["noiseReady"], f"the clouds draw: {info['clouds']}")
    await ed.call("save_scene", scene=sid)
    drawn = path.read_bytes()
    ed.check(drawn == undrawn, f"the file, the clouds drawn: {first_difference(undrawn, drawn)}")
    again = scene_source(ed.project, "Weather")
    ed.check(again == code, f"the code, the clouds drawn: {first_difference(code.encode(), again.encode())}")
    ed.check_in(b"wrapU: clamp_to_edge", authored, "the test authored the wrap it checks")
