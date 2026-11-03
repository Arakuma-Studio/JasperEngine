"""Kept with editor/tests/fixtures/upstream-input-project: run from a checkout of origin/main 047eccf9b as
tools/mcp/tests/test_zz_save_input_fixture.py (tools/build.sh linux first), it made that project.

Saves a project with edited Project Settings > Input with this (upstream) editor: the fixture of
JasperEngine's ProjectInputMigrationTest (an older editor's project.yaml input node)."""

import json
import shutil
from pathlib import Path

import yaml

from harness import Editor

EDITOR = {"create": True, "name": "Upstream Input Fixture"}
TIMEOUT = 900

OUT = Path(__file__).resolve().parents[3] / "build" / "upstream-input-fixture"

PROBE = """local InputProbe = { properties = {} }

function InputProbe:init()
    RegisterEngineEvent(self, "onUpdate")
end

function InputProbe:onUpdate()
    _G.dash = Input.getButtonDown("Dash")
end

return InputProbe
"""


async def test_save_fixture(ed: Editor):
    settings = await ed.call("get_project_settings", keys=["input.actions"])
    actions = settings["settings"]["input.actions"]
    # edits a player makes in Project Settings > Input: tuned knobs, a renamed axis, new actions
    actions[0]["bindings"][0]["gravity"] = 5
    actions[0]["bindings"][0]["sensitivity"] = 6
    actions[1]["bindings"][1]["dead"] = 0.25
    actions[6]["name"] = "Look X"
    actions[6]["bindings"][0]["sensitivity"] = 0.2
    actions.append({"name": "Dash", "type": "button", "description": "quick move: \"dash\"",
                    "press_point": 0.3,
                    "bindings": [{"kind": "buttons", "positive": ["left shift", "joystick button 5", "Mouse 3"],
                                  "gamepad": 1, "gravity": 0, "sensitivity": 0, "dead": 0},
                                 {"kind": "gamepad_axis", "axis": "right_trigger", "gamepad": 2, "dead": 0.1,
                                  "sensitivity": 1.5}]})
    actions.append({"name": "1", "type": "axis", "description": "true",
                    "bindings": [{"kind": "buttons", "negative": ["[1]", "0"], "positive": ["Alpha2", "page up"],
                                  "snap": True, "invert": True},
                                 {"kind": "mouse_delta", "axis": "wheel", "sensitivity": 0.5, "invert": True},
                                 {"kind": "mouse_delta", "axis": "y", "sensitivity": 0.05},
                                 {"kind": "gamepad_axis", "axis": "right_y", "gamepad": "any", "dead": 0.19,
                                  "sensitivity": 1, "invert": True}]})
    await ed.call("set_project_settings", values={"input.actions": actions})
    await ed.call("create_entity", name="InputProbe", type="empty")
    await ed.call("create_script", kind="lua", name="InputProbe", entity="InputProbe", content=PROBE)
    await ed.call("save_project", untitled_scene_dir="scenes")
    project_yaml = yaml.safe_load((ed.project / "project.yaml").read_text())
    ed.check_eq(project_yaml["input"]["version"], 1, "the input node")
    ed.check_eq(str(project_yaml["input"]["actions"][-1]["name"]), "1", "saved")

    if OUT.exists():
        shutil.rmtree(OUT)
    shutil.copytree(ed.project, OUT, ignore=shutil.ignore_patterns(".jasperengine", "*.log"))
    saved = await ed.call("get_project_settings", keys=["input.actions"])
    # what this editor reads back as its definition (get_project_settings input.actions)
    (OUT / "expected-input-actions.json").write_text(json.dumps({"version": 1, "actions": saved["settings"]["input.actions"]},
                                                                indent=2, ensure_ascii=False) + "\n")
