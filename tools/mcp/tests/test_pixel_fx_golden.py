"""Old projects after S48's Pixel FX (docs/specs/S48-pixel-fx.md R47, AC27). The pixel goldens of the sprite features
(AC2-AC4, AC10) are the core GPU suite SpritePixelFxGpu (engine/tests/core/render/SpritePixelFxGpuTest.cpp: an exact
headless framebuffer at an integer scale, and Vulkan through GRAPHIC_BACKEND).

  test_resave_identical   every scene of the shader library's export fixture (library:sprite with each pre-S48 feature,
                          OUTLINE among them) and the fixture .material files: a first save writes them in main's
                          current format (the fixtures predate its format stamp and newer keys: main's to migrate), a
                          second open and save gives the same bytes, nothing S48 adds appears in a file that does not
                          use it, and a legacy OUTLINE variant in the shader key lists keeps its pre-S48 name
                          (library:sprite#...,OUTLINE: no keyword define)
"""

from __future__ import annotations

import difflib
import re
import shutil

import harness
from harness import Editor

CI = True
TIMEOUT = 900
EDITOR = {"project": harness.REPO / "tools" / "fixtures" / "shader-library-export"}

SCENES = ["Lit.scene", "Blend.scene", "2D.scene", "UI.scene", "Stencil.scene", "Phase3A.scene", "Phase3B.scene"]
MATERIALS = [harness.REPO / "tools" / "fixtures" / "textures" / "materials" / "Rock.material",
             harness.REPO / "tools" / "fixtures" / "procmat-baked" / "assets" / "brick_wall" / "brick_wall.material"]


# what only S48's features write: none of it may appear in a project that does not use them
S48_ONLY = re.compile(r"OUTLINE_STYLE|DITHER_FADE|DISSOLVE|RAMP_RECOLOR|DROP_SHADOW|ditherFade|dissolve[A-Z]|ramp(Amount|Low|High|Bevel|Pulse)|"
                      r"shadow(Offset|ColorFrom)|outline(ColorFrom|RampSteps)|inline(Width|Color)|u_fxRampTexture")
SPRITE_KEY = re.compile(r"library:sprite#[A-Z0-9_,]+")


def unified(before: bytes, after: bytes, name: str) -> str:
    return "".join(difflib.unified_diff(before.decode().splitlines(True), after.decode().splitlines(True),
                                        f"{name} (first save)", f"{name} (second save)", n=1))[:4000]


async def test_resave_identical(ed: Editor):
    ed.require_tools("open_scene", "save_scene", "update_material", "get_asset_info")
    for name in SCENES:
        path = ed.project / name
        fixture = path.read_bytes()
        await ed.call("open_scene", scene=name, close_previous=True, unsaved="discard")
        # Save As onto itself: the editor writes the whole file again
        saved = await ed.call_raw("save_scene", {"scene": name, "path": name})
        ed.check(not saved.is_error, f"{name}: save failed: {saved.text[:300]}")
        first = path.read_bytes()
        await ed.call("open_scene", scene=name, close_previous=True, unsaved="discard")
        await ed.call("save_scene", scene=name, path=name)
        second = path.read_bytes()
        if second != first:
            ed.fail(f"{name} re-saves byte for byte:\n{unified(first, second, name)}")
        found = S48_ONLY.search(first.decode())
        ed.check(found is None, f"{name}: nothing of S48 in a scene that does not use it: {found.group(0) if found else ''}")
        # the legacy OUTLINE variants keep their name (the scoped keyword's first value adds no define)
        if b"library:sprite#" in fixture and b",OUTLINE" in fixture:
            ed.check(any(k.endswith(",OUTLINE") or ",OUTLINE," in k for k in SPRITE_KEY.findall(first.decode())),
                     f"{name}: library:sprite's OUTLINE variant keeps its pre-S48 name")

    # a .material written twice through update_material with a value it already has
    (ed.project / "assets" / "materials").mkdir(parents=True, exist_ok=True)
    for source in MATERIALS:
        copy = ed.project / "assets" / "materials" / source.name
        shutil.copyfile(source, copy)
        rel = f"assets/materials/{source.name}"
        await ed.call("sync_files", paths=[rel], full=False)
        info = await ed.call("get_asset_info", path=rel)
        material = info.get("material") or {}
        same = {"baseColorFactor": material.get("baseColorFactor", [1, 1, 1, 1])}
        written = await ed.call_raw("update_material", {"path": rel, "values": same})
        ed.check(not written.is_error, f"{rel}: update_material failed: {written.text[:300]}")
        first = copy.read_bytes()
        await ed.call("update_material", path=rel, values=same)
        second = copy.read_bytes()
        if second != first:
            ed.fail(f"{rel} re-saves byte for byte:\n{unified(first, second, rel)}")
        found = S48_ONLY.search(first.decode())
        ed.check(found is None, f"{rel}: nothing of S48 in a material that does not use it: {found.group(0) if found else ''}")
