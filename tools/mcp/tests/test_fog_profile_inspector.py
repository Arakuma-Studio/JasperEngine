"""The Fog Profile Inspector (docs/specs/S101-vapor.md R23, R24, R26, R27, AC11), driven like a person
(xdotool in the harness's Xvfb, aimed where find_ui_item says a control is drawn now).

1. A Fog linked to a .fogprofile draws the Profile row and a Set by profile box on every look row;
   ticking one copies the Fog's value into the file, unticking removes it, and each is one undo
   step that undoes the file.
2. A ticked row's value edits the profile, not the Fog.
3. Unlink copies what the Fog showed into its own fields and clears the link (one undo step).
4. Assets > Create > Fog Profile writes a .fogprofile (undo removes it), and the Inspector shows
   it with every field "not set".
5. GameObject > Effects > Fog Zone makes a Fog Zone whose Inspector draws its shape rows, the
   Profile row and, linked, only the fields its profile sets.

Screenshots of each step go to $JE_EVIDENCE_DIR (else the scratch directory).
"""

from __future__ import annotations

import json
import os
from pathlib import Path

from harness import Editor
import ui_input

EDITOR = {"xvfb": "harness", "create": True, "name": "Fog Profile Inspector"}
CI = True
TIMEOUT = 900

# a created project's assets folder is its root: Fog.profile stores the project-relative path
PROFILE = "FogProfiles/murky.fogprofile"
LOOK = {"version": 1, "name": "Murky", "description": "Dark and soupy",
        "fields": {"attenuationDistance": 25.0, "baseHeight": 4.0, "colorMode": "lit", "albedo": [0.2, 0.25, 0.2]}}
S: dict = {}


def evidence(ed: Editor) -> Path:
    folder = Path(os.environ.get("JE_EVIDENCE_DIR") or ed.scratch.root) / "fog-profile-inspector"
    folder.mkdir(parents=True, exist_ok=True)
    return folder


async def shot(ed: Editor, name: str) -> None:
    await ed.screenshot("editor", evidence(ed) / f"{name}.png", focus=False, max_width=0)


def fields(ed: Editor, path: str = PROFILE) -> dict:
    return json.loads((ed.project / path).read_text())["fields"]


async def setup(ed: Editor) -> dict:
    if S.get("ed") is ed:
        return S
    S.clear()
    S["ed"] = ed
    S["ui"] = ui_input.for_editor(ed)
    made = await ed.call("create_scene", name="FogInspector", type="3D", path="scenes/FogInspector")
    S["scene"] = made["scene"]["id"]
    await ed.call("select_scene", scene=S["scene"])
    fog = await ed.call("create_entity", scene=S["scene"], type="fog", name="Fog")
    S["fog"] = fog["entity_id"]
    (ed.project / PROFILE).parent.mkdir(parents=True, exist_ok=True)
    (ed.project / PROFILE).write_text(json.dumps(LOOK, indent=2) + "\n")
    await ed.call("sync_files", paths=[PROFILE], full=False)
    await ed.call("set_properties", scene=S["scene"], entity=S["fog"],
                  values={"fog": {"type": "height", "profile": PROFILE, "maxOpacity": 0.8}})
    await ed.call("tool_groups", enable=["editor_ux"])
    await ed.call("select_entities", scene=S["scene"], entities=[S["fog"]])
    await ed.call("focus_window", window="inspector")
    return S


async def fog_values(ed: Editor, *names: str) -> dict:
    got = await ed.call("get_entity", scene=S["scene"], entity=S["fog"], components=["fog"], properties=list(names))
    return got["components"]["fog"]


async def test_linked_fog_ticks_and_unticks_rows(ed: Editor):
    s = await setup(ed)
    ui = s["ui"]
    await ui.find("##fog_set_by_profile_maxOpacity", "Inspector")
    layout = await ed.call("get_inspector_layout", scene=s["scene"], entities=[s["fog"]])
    paths = [row["path"] for row in layout["rows"] if row.get("component", "").lower() == "fog"]
    ed.check_in("profile", paths, "the Profile row is drawn")
    ed.check(paths.index("profile") < paths.index("type"), f"the Profile row is at the top: {paths}")
    for path in ("attenuationDistance", "baseHeight", "maxOpacity", "albedo"):
        ed.check_in(path, paths, f"the {path} row is drawn while linked")
    await shot(ed, "1-linked")

    await ui.click_item("##fog_set_by_profile_maxOpacity", "Inspector")
    await ui_input.frames(ed, 4)
    ed.check_eq(fields(ed).get("maxOpacity"), 0.8, "ticking copies the Fog's value into the profile")
    await ui.click_item("##fog_set_by_profile_baseHeight", "Inspector")
    await ui_input.frames(ed, 4)
    ed.check("baseHeight" not in fields(ed), f"unticking removes the override: {fields(ed)}")
    await shot(ed, "2-ticked")
    await ed.call("undo")
    ed.check_eq(fields(ed).get("baseHeight"), 4.0, "one undo puts the untick back")
    await ed.call("undo")
    ed.check_eq(fields(ed), LOOK["fields"], "the second undoes the tick, file and all")


async def test_ticked_row_edits_the_profile(ed: Editor):
    s = await setup(ed)
    ui = s["ui"]
    own = (await fog_values(ed, "attenuationDistance"))["attenuationDistance"]
    item = await ui.find("##fog_profile_value_attenuationDistance", "Inspector")
    ed.check(item, "a ticked row draws the profile's value")
    await ui.double_click(ui.centre(item["rect"]))
    await ui.key("ctrl+a")
    await ui.type("60")
    await ui.key("Return")
    await ui_input.frames(ed, 4)
    ed.check_eq(fields(ed).get("attenuationDistance"), 60.0, "the edit is written to the profile")
    ed.check_eq((await fog_values(ed, "attenuationDistance"))["attenuationDistance"], own, "the Fog's own value is untouched")
    await ed.call("undo")
    ed.check_eq(fields(ed).get("attenuationDistance"), 25.0, "undo puts the file back")


async def test_unlink_keeps_the_look(ed: Editor):
    s = await setup(ed)
    ui = s["ui"]
    await ui.click_item("##fog_profile_unlink", "Inspector")
    await ui_input.frames(ed, 4)
    got = await fog_values(ed, "profile", "attenuationDistance", "baseHeight", "colorMode")
    ed.check(not got.get("profile"), f"the link is cleared: {got}")
    ed.check_eq(got["attenuationDistance"], 25.0, "the profile's value is the Fog's own now")
    ed.check_eq(got["colorMode"], "lit")
    await shot(ed, "3-unlinked")
    await ed.call("undo")
    got = await fog_values(ed, "profile", "attenuationDistance")
    ed.check("murky.fogprofile" in json.dumps(got.get("profile")), f"undo links it again: {got}")


async def test_create_fog_profile_menu(ed: Editor):
    s = await setup(ed)
    ui = s["ui"]
    await ed.call("run_command", id="assets.createFogProfile")
    await ed.call("respond_dialog", choice="cancel")  # keep the name it was made with
    created = sorted(p.relative_to(ed.project).as_posix() for p in ed.project.rglob("New Fog Profile*.fogprofile"))
    ed.check(created, "Create > Fog Profile writes a .fogprofile")
    ed.check_eq(json.loads((ed.project / created[0]).read_text()).get("fields", {}), {}, "it sets no field yet")
    ed.check(await ui.find("##asset_set_attenuationDistance", "Inspector"), "the Inspector shows it with every field")
    ed.check(not await ui.find("##fog_profile_value_attenuationDistance", "Inspector", frames=4), "unticked: no value, \"not set\"")
    await shot(ed, "4-asset")
    await ed.call("undo", scope="project")
    ed.check(not (ed.project / created[0]).exists(), "undo removes it")
    await ed.call("select_entities", scene=s["scene"], entities=[s["fog"]])


async def test_fog_zone_menu_and_inspector(ed: Editor):
    s = await setup(ed)
    ui = s["ui"]
    await ed.call("select_scene", scene=s["scene"])
    await ed.call("run_command", id="gameobject.create.effects.fogZone")
    selection = await ed.call("get_selection", scene=s["scene"])
    zone = selection["entities"][0]
    zone = zone["id"] if isinstance(zone, dict) else zone
    got = await ed.call("get_entity", scene=s["scene"], entity=zone, detail="summary")
    ed.check_in("fogzone", [c.lower().replace(" ", "") for c in got["components"]], "GameObject > Effects > Fog Zone")
    await ed.call("set_properties", scene=s["scene"], entity=zone, values={"fogzone": {"profile": PROFILE}})
    await ui.find("##zone_set_attenuationDistance", "Inspector")
    layout = await ed.call("get_inspector_layout", scene=s["scene"], entities=[zone])
    paths = [row["path"] for row in layout["rows"] if "zone" in row.get("component", "").lower()]
    for path in ("shape", "size", "blendDistance", "priority", "weight", "profile"):
        ed.check_in(path, paths, f"the zone's {path} row")
    ed.check(await ui.find("##zone_set_baseHeight", "Inspector"), "a field the profile sets is a row")
    ed.check(not await ui.find("##zone_set_maxOpacity", "Inspector", frames=4), "a field it does not set is not")
    await shot(ed, "5-zone")
