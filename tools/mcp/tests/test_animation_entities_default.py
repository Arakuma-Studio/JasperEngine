# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""S74 R38, AC16 (docs/specs/S74-clip-quality.md): Project Settings > Animation > Animation Entities.

- A new project gets Auto: a model of 5 or more clips placed in a scene loads with
  animationEntities false; one of fewer clips keeps them; Always places it as before.
- An older project (no key in project.yaml) is Always: the same model keeps its Animation entities,
  and saving writes no key.
"""

from __future__ import annotations

import shutil

from harness import Editor, REPO

CI = True
TIMEOUT = 600
EDITOR = {"create": True, "name": "Animation Entities"}

HERO = REPO / "tools" / "fixtures" / "pose-animation" / "assets" / "models" / "hero_animated.gltf"    # 5 clips
ARM = REPO / "tools" / "fixtures" / "vertex-animation" / "assets" / "models" / "rigid_arm.gltf"      # 2 clips
KEY = "animation.animation_entities"


async def setting(ed: Editor) -> str:
    return (await ed.call("get_project_settings", keys=[KEY]))["settings"][KEY]


async def place(ed: Editor, name: str, model: str) -> bool:
    await ed.call("create_entity", type="model", name=name, model=model)
    got = await ed.call("get_entity", entity=name, components=["model"], properties=["animationEntities"])
    return got["components"]["model"]["animationEntities"]


async def test_a_new_project_places_many_clips_without_entities(ed: Editor):
    ed.check_eq(await setting(ed), "auto", "a new project's Animation Entities")
    ed.check("animationEntities: auto" in (ed.project / "project.yaml").read_text(), "project.yaml holds it")
    models = ed.project / "assets" / "models"
    models.mkdir(parents=True, exist_ok=True)
    shutil.copy(HERO, models / "hero.gltf")
    shutil.copy(ARM, models / "arm.gltf")
    await ed.call("sync_files", paths=["assets/models/hero.gltf", "assets/models/arm.gltf"])
    ed.check_eq(await place(ed, "Hero", "assets/models/hero.gltf"), False, "5 clips: no Animation entities")
    ed.check_eq(await place(ed, "Arm", "assets/models/arm.gltf"), True, "2 clips: as before")
    # redo places it the same way
    await ed.call("undo")
    await ed.call("undo")
    await ed.call("redo")
    got = await ed.call("get_entity", entity="Hero", components=["model"], properties=["animationEntities"])
    ed.check_eq(got["components"]["model"]["animationEntities"], False, "after undo and redo")
    await ed.call("set_project_settings", values={KEY: "always"})
    ed.check_eq(await place(ed, "Hero2", "assets/models/hero.gltf"), True, "Always: an Animation entity per clip")


async def test_an_old_project_keeps_its_behaviour(ed: Editor):
    async with Editor(project=REPO / "tools" / "fixtures" / "pose-animation") as old:
        ed.check("animationEntities" not in (old.project / "project.yaml").read_text(), "the fixture has no key")
        ed.check_eq(await setting(old), "always", "no key: Always")
        ed.check_eq(await place(old, "Walker", "assets/models/hero_animated.gltf"), True, "5 clips keep their entities")
        await old.call("save_project")
        ed.check("animationEntities" not in (old.project / "project.yaml").read_text(), "saving adds no key")


async def test_a_project_from_a_template_is_new_too(ed: Editor):
    target = ed.scratch.root / "from-template"
    await ed.call("create_project", path=str(target), name="From Template", template="empty", unsaved="discard")
    ed.check_eq(await setting(ed), "auto", "a template's project is a new project")
    ed.check("animationEntities: auto" in (target / "project.yaml").read_text(), "its project.yaml holds it")
