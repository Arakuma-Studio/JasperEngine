"""Unity animation assets over MCP (docs/specs/S94-rendering-anim-gaps.md F35, F36, AC33, AC34):
edit_animation create.unity imports a Unity YAML .anim (tracks, the report with what did not map and
its `next`, the clip's Unity source, one project undo step), refuses a muscle-only clip with the
limit named, and an engine call on a Unity .anim names the import route; edit_animator_controller
create.unity brings the controller with the clips and the mask it uses (GUIDs through the fixture
Unity project), a model clip reported with its name; edit_avatar_mask create.unity. Inputs:
editor/tests/fixtures/unity (hand-written Unity YAML)."""

from __future__ import annotations

import json
import shutil

from harness import Editor, REPO

EDITOR = {"create": True, "name": "Unity Animation Import"}
CI = True
TIMEOUT = 300

UNITY = REPO / "editor" / "tests" / "fixtures" / "unity"
ANIMATIONS = UNITY / "Project" / "Assets" / "Animations"


async def test_import_unity_anim(ed: Editor):
    clip = "assets/anim/idle.anim"
    r = await ed.call_raw("edit_animation", {"clip": clip, "create": {"unity": str(UNITY / "Idle.anim")}})
    ed.check(not r.is_error, f"create.unity failed: {r.text[:800]}")
    out = r.structured
    ed.check(out["created"], out)
    ed.check_eq(out["unity"]["tracks"], 9, json.dumps(out["unity"])[:800])
    ed.check_eq(out["unity"]["muscle_curves"]["count"], 1)
    ed.check(any("MonoBehaviour.speed" in u["binding"] for u in out["unity"]["unmapped"]), out["unity"]["unmapped"])
    ed.check("list_properties" in out.get("next", ""), f"an unmapped binding gives a next: {out.get('next')}")
    data = json.loads((ed.project / clip).read_text())
    ed.check_eq(data["wrap"], "loop")
    ed.check_eq(data["source"]["kind"], "unity")
    ed.check_eq(len(data["events"]), 2)

    # one project undo step
    await ed.call("undo", scope="project")
    ed.check(not (ed.project / clip).exists(), "undo removes the imported clip")

    # a muscle-only clip is refused with the limit named
    await ed.check_error("edit_animation", {"clip": "assets/anim/wave.anim", "create": {"unity": str(UNITY / "HumanoidWave.anim")}},
                         contains="humanoid (muscle-space) clip")
    # a Unity .anim opened as an engine clip names the import route
    (ed.project / "assets" / "anim").mkdir(parents=True, exist_ok=True)
    shutil.copyfile(UNITY / "Idle.anim", ed.project / "assets" / "anim" / "raw.anim")
    await ed.check_error("edit_animation", {"clip": "assets/anim/raw.anim", "wrap": "loop"}, contains="create: {unity")


async def test_import_unity_controller_and_mask(ed: Editor):
    path = "assets/anim/hero.controller"
    out = await ed.call("edit_animator_controller", {"path": path, "create": {"unity": str(ANIMATIONS / "Hero.controller")}})
    ed.check_eq(len(out["files"]), 5, out["files"])
    for name in ("Walk.anim", "Run.anim", "Idle.anim", "UpperBody.mask"):
        ed.check((ed.project / "assets" / "anim" / name).exists(), f"{name} imported beside the controller")
    ed.check(any('"Jump"' in u["reason"] for u in out["unity"]["unmapped"]), out["unity"]["unmapped"])
    ed.check("set_state" in out.get("next", ""), f"unmapped items give a next: {out.get('next')}")
    layers = json.loads((ed.project / path).read_text())["layers"]
    ed.check_eq([layer["name"] for layer in layers], ["Base Layer", "Upper"])
    # relative to the project's assets folder (this project's is its root)
    ed.check_eq(layers[1]["mask"]["path"], "assets/anim/UpperBody.mask")
    ed.check(layers[1]["mask"]["guid"] == json.loads((ed.project / "assets" / "anim" / "UpperBody.mask").read_text())["guid"], layers[1]["mask"])

    mask = "assets/anim/upper.mask"
    out = await ed.call("edit_avatar_mask", {"path": mask, "create": {"unity": str(ANIMATIONS / "UpperBody.mask")}})
    ed.check_eq(out["files"], [mask])
    data = json.loads((ed.project / mask).read_text())
    ed.check_eq(data["default"], 0)
    await ed.check_error("edit_avatar_mask", {"path": mask, "create": {"unity": str(ANIMATIONS / "UpperBody.mask")}}, contains="already exists")
    await ed.call("undo", scope="project")
    ed.check(not (ed.project / mask).exists(), "undo removes the imported mask")
