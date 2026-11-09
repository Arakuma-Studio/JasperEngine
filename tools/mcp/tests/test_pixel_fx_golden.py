"""Old projects after S48's Pixel FX (docs/specs/S48-pixel-fx.md R47, AC27). The pixel goldens of the sprite features
(AC2-AC4, AC10) are the core GPU suite SpritePixelFxGpu (engine/tests/core/render/SpritePixelFxGpuTest.cpp: an exact
headless framebuffer at an integer scale, and Vulkan through GRAPHIC_BACKEND).

  test_resave_identical   every scene of the shader library's export fixture (library:sprite with each pre-S48 feature,
                          OUTLINE among them) and the fixture .material files re-save byte for byte after open and save:
                          the new features, keyword group and properties add nothing to a material that does not use
                          them, and no format version moved
"""

from __future__ import annotations

import shutil

import harness
from harness import Editor

CI = True
TIMEOUT = 900
EDITOR = {"project": harness.REPO / "tools" / "fixtures" / "shader-library-export"}

SCENES = ["Lit.scene", "Blend.scene", "2D.scene", "UI.scene", "Stencil.scene", "Phase3A.scene", "Phase3B.scene"]
MATERIALS = [harness.REPO / "tools" / "fixtures" / "textures" / "materials" / "Rock.material",
             harness.REPO / "tools" / "fixtures" / "procmat-baked" / "assets" / "brick_wall" / "brick_wall.material"]


async def test_resave_identical(ed: Editor):
    ed.require_tools("open_scene", "save_scene", "update_material", "get_asset_info")
    for name in SCENES:
        path = ed.project / name
        before = path.read_bytes()
        await ed.call("open_scene", scene=name, close_previous=True, unsaved="discard")
        # Save As onto itself: the editor writes the whole file again
        saved = await ed.call_raw("save_scene", {"scene": name, "path": name})
        ed.check(not saved.is_error, f"{name}: save failed: {saved.text[:300]}")
        ed.check(path.read_bytes() == before, f"{name} re-saves byte for byte")

    # a .material written again through update_material with a value it already has
    (ed.project / "assets" / "materials").mkdir(parents=True, exist_ok=True)
    for source in MATERIALS:
        copy = ed.project / "assets" / "materials" / source.name
        shutil.copyfile(source, copy)
        rel = f"assets/materials/{source.name}"
        await ed.call("sync_files", paths=[rel], full=False)
        info = await ed.call("get_asset_info", path=rel)
        material = info.get("material") or {}
        ed.check("shader" in material or "baseColorFactor" in material, f"{rel}: get_asset_info reads the material: {list(info)}")
        before = copy.read_bytes()
        same = {"baseColorFactor": material.get("baseColorFactor", [1, 1, 1, 1])}
        written = await ed.call_raw("update_material", {"path": rel, "values": same})
        ed.check(not written.is_error, f"{rel}: update_material failed: {written.text[:300]}")
        ed.check(copy.read_bytes() == before, f"{rel} re-saves byte for byte")
