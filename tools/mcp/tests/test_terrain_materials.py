"""T3's agent surface: tiers and material fields through sample_terrain, set_properties and
set_project_settings; the project's defaults; the export of cooked array slices (R39-R46, R51; AC21, AC37-AC39, AC43).
"""

import json
import shutil
import tempfile
from pathlib import Path

import yaml

from harness import Editor

EDITOR = {"create": True, "name": "Terrain Materials"}
CI = True

MATERIAL_KEYS = {"quality", "quality_source", "limit_layers_per_pixel", "layers", "blend_maps_read", "fetches", "texture_slots", "detail_array", "features"}


async def make_terrain(ed, name="Hills"):
    scene = (await ed.call("list_scenes"))["scenes"][0]["id"]
    made = await ed.call("create_entity", scene=scene, type="terrain", name=name, select=False)
    return scene, made["entity_id"]


async def material(ed, scene, entity):
    return (await ed.call("sample_terrain", scene=scene, entity=entity, grid=0))["info"]["material"]


async def test_agent_fields(ed):
    scene, entity = await make_terrain(ed, "Fields")
    info = await material(ed, scene, entity)
    assert MATERIAL_KEYS <= set(info), info
    assert set(info["fetches"]) >= {"flat", "slope", "worst", "by_tier"}
    assert set(info["fetches"]["by_tier"]) == {"high", "medium", "low"}
    assert info["texture_slots"]["limit"] == 16

    # a tier by reflection, and what decided it
    await ed.call("set_properties", scene=scene, entity=entity, values={"terrain": {"quality": "LOW"}})
    info = await material(ed, scene, entity)
    assert info["quality"] == "low" and info["quality_source"] == "component"
    assert info["fetches"]["worst"] <= 6

    # an invalid value is refused atomically: the valid one beside it is not written either
    limited = info["limit_layers_per_pixel"]
    await ed.call("set_properties", scene=scene, entity=entity, values={"terrain": {"quality": "ULTRA", "limitLayersPerPixel": not limited}}, expect_error=True)
    info = await material(ed, scene, entity)
    assert info["quality"] == "low" and info["limit_layers_per_pixel"] == limited
    await ed.call("undo")
    info = await material(ed, scene, entity)
    assert info["quality"] != "low", "undo restores the tier"

    # a point says what it draws
    sampled = await ed.call("sample_terrain", scene=scene, entity=entity, points=[[0, 0]], grid=0)
    point = sampled["samples"][0]
    assert {"hole", "collision_gap", "nav_excluded", "layers_painted", "layers_drawn", "fetches"} <= set(point), point
    assert point["hole"] is False
    assert (await ed.call("sample_terrain", scene=scene, entity=entity, grid=0))["info"]["holes"] is None

    # stats of a rectangle
    stats = await ed.call("sample_terrain", scene=scene, entity=entity, grid=0, stats=["coverage"], region={"min": [-5, -5], "max": [5, 5]})
    assert stats["stats"]["coverage"]["hole_fraction"] == 0

    # a tier per platform through the settings, validated
    await ed.call("set_project_settings", values={"graphics.terrainQuality.android": "low"})
    settings = await ed.call("get_project_settings", keys=["graphics.terrainQuality.android", "graphics.shipped_terrain_tiers"])
    assert "low" in json.dumps(settings)
    await ed.call("set_project_settings", values={"graphics.terrainQuality.android": "ultra"}, expect_error=True)


async def test_project_defaults():
    # the saved project outlives its editor's scratch directory here, outside the repository
    keep = Path(tempfile.mkdtemp(prefix="t3-terrain-defaults-"))
    project_path = keep / "project"
    # a new project from the template: the table of R51
    async with Editor(create=True, name="Terrain Defaults") as ed:
        # the file as a save writes it: a new project's may still be on its way to the disk
        await ed.call("save_project")
        project_yaml = yaml.safe_load((ed.project / "project.yaml").read_text())
        tiers = project_yaml["graphics"]["terrainQuality"]
        assert tiers == {"macos": "high", "ios": "low", "web": "medium", "android": "low", "linux": "high", "windows": "high"}, tiers
        assert project_yaml["graphics"]["compressTerrainLayers"] is True
        scene = (await ed.call("list_scenes"))["scenes"][0]["id"]
        made = await ed.call("create_entity", scene=scene, type="terrain", name="New", select=False)
        info = await material(ed, scene, made["entity_id"])
        assert info["limit_layers_per_pixel"] is True, "a new terrain limits its layers (R53)"
        await ed.call("save_project")
        shutil.copytree(ed.project, project_path)

    # an old project has no key: High everywhere, and none is written on a re-save
    text = yaml.safe_load((project_path / "project.yaml").read_text())
    del text["graphics"]["terrainQuality"]
    del text["graphics"]["compressTerrainLayers"]
    (project_path / "project.yaml").write_text(yaml.safe_dump(text))
    async with Editor(project=project_path) as ed:
        scene = (await ed.call("list_scenes"))["scenes"][0]["id"]
        terrains = await ed.call("sample_terrain", scene=scene, grid=0)
        assert terrains["info"]["material"]["quality_source"] == "project"
        assert terrains["info"]["material"]["quality"] == "high"
        await ed.call("save_project")
        saved = yaml.safe_load((Path(ed.project) / "project.yaml").read_text())
    shutil.rmtree(keep)
    assert "terrainQuality" not in (saved.get("graphics") or {}), "an old project writes no key"


async def test_ui_readout(ed):
    scene, entity = await make_terrain(ed, "Readout")
    await ed.call("select_entities", scene=scene, entities=[entity])
    layout = await ed.call("get_inspector_layout", scene=scene, entities=[entity])
    text = json.dumps(layout)
    for row in ("quality", "limitLayersPerPixel", "offscreenShadows", "macroMode", "resampleEnabled", "heightBlendSharpness", "macroNormal"):
        assert row in text, row
    before = await material(ed, scene, entity)
    flipped = not before["limit_layers_per_pixel"]
    await ed.call("set_properties", scene=scene, entity=entity, values={"terrain": {"limitLayersPerPixel": flipped}})
    after = await material(ed, scene, entity)
    assert after["limit_layers_per_pixel"] is flipped
    await ed.call("undo")
    assert (await material(ed, scene, entity))["limit_layers_per_pixel"] == before["limit_layers_per_pixel"]
