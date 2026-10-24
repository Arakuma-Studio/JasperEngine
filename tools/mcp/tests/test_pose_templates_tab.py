"""The Pose Animation window's Templates tab (spec: pose animation phase 6, section 4) on the pose
fixture project.

Without a character the window's empty state lists the templates (cards without thumbnails, the
hint "Bind a character to preview templates"), filtered by category and search -> bound to the hero
every card gets its thumbnail (the template's middle pose on the hero) and the tab draws (a
non-flat window region) -> a card click's code path creates a new animation from the template at
the save dialog's default path <model dir>/<template name>.poseanim (numbered when taken),
previewing the hero's model, opens it on the Pose tab; it is one project undo step -> Replace
Poses in Current Animation / Append Poses (one undo step each, the preview model kept) -> Save
Current as Template shows a "user" card; Show in Project; Delete and its undo.

The tab's actions run through the window's own functions via focus_window's `pose.templates` test
hook (JASPER_MCP_TEST_HOOKS=1, set by the harness). JASPER_SHOTS_DIR (optional) receives the
screenshots.
"""

from __future__ import annotations

import json
import os
import sys
from pathlib import Path

import harness
from harness import Editor

sys.path.insert(0, str(harness.REPO / "tools" / "mcp"))
import pngdiff  # noqa: E402

EDITOR = {"project": harness.REPO / "tools" / "fixtures" / "pose-animation"}
TIMEOUT = 900

# built-ins sorted by category, then name
BUILTINS = ["Jump", "Punch", "Sit Down", "Idle", "Bow", "Wave", "Cartoon Walk", "Run", "Sneak", "Walk"]
LOCOMOTION = {"Walk", "Run", "Sneak", "Cartoon Walk"}
WALK_FILE = "assets/models/Walk.poseanim"


async def focus(ed: Editor, **pose) -> dict:
    args = {"window": "pose_animation"}
    if pose:
        args["pose"] = pose
    return await ed.call("focus_window", **args)


async def state(ed: Editor) -> dict:
    return (await focus(ed))["state"]


def file_json(ed: Editor, rel: str) -> dict:
    return json.loads((ed.project / rel).read_text(encoding="utf-8"))


def settings(data: dict) -> dict:
    """A .poseanim file's settings: frame rate, loop, feel and humanize."""
    return {k: data[k] for k in ("frame_rate", "loop", "feel", "humanize")}


def builtin_json(name: str) -> dict:
    folder = harness.REPO / "editor" / "resources" / "pose-templates"
    for path in folder.glob("*.poseanim"):
        data = json.loads(path.read_text(encoding="utf-8"))
        if data["template"]["name"] == name:
            return data
    raise AssertionError(f"no built-in template {name}")


async def window_region(ed: Editor, r: dict, shot: str):
    """The window's region of the editor screenshot (saved to JASPER_SHOTS_DIR/<shot>.png when set)."""
    shots = os.environ.get("JASPER_SHOTS_DIR")
    path = Path(shots) / f"{shot}.png" if shots else None
    png = await ed.screenshot(view="editor", max_width=0, path=path)
    x, y = int(r["pos"][0]), int(r["pos"][1])
    w, h = int(r["size"][0]), int(r["size"][1])
    ed.check(w > 100 and h > 100, f"window size {r['size']}")
    return pngdiff.crop(pngdiff.decode(png), x, y, w, h)


async def wait_for_thumbnails(ed: Editor, at_least: int) -> dict:
    """Every card on screen (at least `at_least` of them) shows its rendered thumbnail."""
    st = {}
    for _ in range(60):
        st = await state(ed)
        t = st.get("templates", {})
        if t.get("visible", 0) >= at_least and t.get("thumbnails", 0) == t.get("visible"):
            return st
        await ed.call("wait", seconds=0.25)
    ed.check(False, f"the thumbnails of the {at_least}+ cards on screen render: {st.get('templates')}")
    return st


async def test_templates_without_a_character(ed: Editor):
    r = await focus(ed)
    st = r["state"]
    ed.check_eq(st["empty_state"], "no_character", f"no character: {st}")
    t = st.get("templates")
    ed.check(t is not None, f"the empty state lists the templates: {st}")
    ed.check_eq(t["cards"], BUILTINS, "the 10 built-in templates, sorted by category then name")
    ed.check(t["hint"] is True, "the hint to bind a character")
    ed.check_eq(t["thumbnails"], 0, "no thumbnails without a character")
    ed.check_eq(t["user"], [], "no project templates yet")
    region = await window_region(ed, r, "01-templates-no-character")
    ed.check(not pngdiff.flat(region, 8), "the window draws the template cards")
    # the filter: category and search words
    t = (await focus(ed, templates={"category": "locomotion"}))["state"]["templates"]
    ed.check_eq(set(t["cards"]), LOCOMOTION, f"category filter: {t}")
    t = (await focus(ed, templates={"category": "", "search": "walk"}))["state"]["templates"]
    ed.check_eq(set(t["cards"]), {"Walk", "Cartoon Walk"}, f"search: {t}")
    t = (await focus(ed, templates={"category": "Gestures", "search": "walk"}))["state"]["templates"]
    ed.check_eq(t["cards"], [], f"nothing matches both: {t}")
    t = (await focus(ed, templates={"category": "", "search": ""}))["state"]["templates"]
    ed.check_eq(len(t["cards"]), 10, "filter cleared")
    # a card click needs a character (the new animation previews on its model)
    await ed.check_error("focus_window", {"window": "pose_animation", "pose": {"templates": {"create": {"name": "Walk"}}}},
                         contains="character")


async def test_thumbnails_on_the_bound_character(ed: Editor):
    r = await focus(ed, entity="Hero")
    ed.check_eq(r["state"]["empty_state"], "no_animation", f"bound, no animation: {r['state']}")
    st = await wait_for_thumbnails(ed, 6)
    ed.check(st["templates"]["hint"] is False, "no hint once a character is bound")
    r = await focus(ed)
    region = await window_region(ed, r, "02-templates-no-animation-hero")
    ed.check(not pngdiff.flat(region, 8), "cards with thumbnails")


async def test_create_from_a_template(ed: Editor):
    r = await focus(ed, templates={"create": {"name": "walk"}})
    hook = r.get("viewport_input", {}).get("templates", {})
    ed.check_eq(hook.get("default_path"), WALK_FILE, f"the save dialog's default: <model dir>/<template name>.poseanim: {r}")
    st = r["state"]
    ed.check_eq(st["animation"], WALK_FILE, f"the new animation is open: {st}")
    ed.check_eq(st["pose_count"], 4, "the template's poses")
    ed.check_eq(st["tab"], "Pose", "the Pose tab")
    ed.check_eq(st["mode"], "posing", "posing its first pose")
    data = file_json(ed, WALK_FILE)
    template = builtin_json("Walk")
    ed.check(data["preview_model"].endswith("models/hero_tpose.gltf"), f"previews on the character's model: {data['preview_model']}")
    ed.check("template" not in data, "an animation, not a template file")
    ed.check_eq([p["name"] for p in data["poses"]], [p["name"] for p in template["poses"]], "the template's poses")
    ed.check_eq(settings(data), settings(template), "the template's feel and humanize")
    # one undo step: undo removes the file, redo brings it back
    await ed.call("undo", scope="project")
    ed.check(not (ed.project / WALK_FILE).exists(), "undo removes the new animation")
    await ed.call("redo", scope="project")
    ed.check((ed.project / WALK_FILE).exists(), "redo writes it again")
    # the default is numbered when the file exists
    r = await focus(ed, templates={"create": {"name": "Jump", "path": ""}})
    ed.check_eq(r["state"]["animation"], "assets/models/Jump.poseanim", "Jump next to the model")
    r = await focus(ed, templates={"create": {"name": "Walk", "path": ""}})
    ed.check_eq(r["viewport_input"]["templates"]["default_path"], "assets/models/Walk 1.poseanim", "numbered default")
    ed.check_eq(r["state"]["animation"], "assets/models/Walk 1.poseanim", "created at the numbered path")
    # an explicit path (the dialog's field); an existing one is refused
    r = await focus(ed, templates={"create": {"name": "Wave", "path": "assets/anim/my wave"}})
    ed.check_eq(r["state"]["animation"], "assets/anim/my wave.poseanim", "the extension is added")
    r = await focus(ed, templates={"create": {"name": "Idle", "path": "assets/anim/Idle.PoseAnim"}})
    ed.check_eq(r["state"]["animation"], "assets/anim/Idle.PoseAnim", "an extension in another case is kept (not doubled)")
    await ed.check_error("focus_window", {"window": "pose_animation", "pose": {"templates": {"create": {"name": "Wave", "path": WALK_FILE}}}},
                         contains="exists")
    await ed.check_error("focus_window", {"window": "pose_animation", "pose": {"templates": {"create": {"name": "Moonwalk"}}}},
                         contains="no pose template")
    await focus(ed, path=WALK_FILE)


async def test_tab_renders_with_thumbnails(ed: Editor):
    r = await focus(ed, tab="templates")
    ed.check_eq(r["state"]["tab"], "Templates", f"the Templates tab: {r['state']}")
    st = await wait_for_thumbnails(ed, 6)
    ed.check_eq(len(st["templates"]["cards"]), 10, "every template")
    r = await focus(ed)
    region = await window_region(ed, r, "03-templates-tab-hero")
    ed.check(not pngdiff.flat(region, 8), "the Templates tab draws")


async def test_template_card_menu_opens_on_a_real_right_click(ed: Editor):
    """A right-click on a Templates card opens its menu (Replace / Append / ...): real mouse events
    through ImGui's queue (pose.input); the menu's OpenPopup and BeginPopup share one ID scope."""
    st = (await focus(ed, tab="templates"))["state"]
    await ed.call("wait", frames=3)
    first = st["templates"]["cards"][0]
    r = await focus(ed, input={"right_click": {"template": first}})
    ed.check_eq(r["state"]["menu"], "template:" + first, f"the template menu is open: {r['state']}")
    r = await focus(ed)
    ed.check(r["state"]["menu"] is None, f"focusing the window closes it: {r['state']}")


async def test_replace_and_append(ed: Editor):
    await focus(ed, path=WALK_FILE)
    before = file_json(ed, WALK_FILE)
    r = await focus(ed, templates={"apply": {"name": "Jump", "mode": "replace"}})
    st = r["state"]
    jump = builtin_json("Jump")
    ed.check_eq(st["pose_count"], len(jump["poses"]), f"Replace: the template's poses: {st}")
    ed.check_eq(st["primary"], 0, "pose 0 selected")
    data = file_json(ed, WALK_FILE)
    ed.check_eq(data["preview_model"], before["preview_model"], "the preview model is kept")
    ed.check_eq(settings(data), settings(jump), "the template's settings")
    ed.check_eq([p["name"] for p in data["poses"]], [p["name"] for p in jump["poses"]], "Jump's poses")
    await ed.call("undo", scope="project")
    ed.check_eq(file_json(ed, WALK_FILE), before, "one undo step restores the animation")
    ed.check_eq((await state(ed))["pose_count"], 4, "the window follows the undo")
    r = await focus(ed, templates={"apply": {"name": "Wave", "mode": "append"}})
    st = r["state"]
    wave = builtin_json("Wave")
    ed.check_eq(st["pose_count"], 4 + len(wave["poses"]), f"Append: poses added: {st}")
    ed.check_eq(st["selection"], [4, 5], "the appended poses are selected")
    ed.check_eq(st["primary"], 4, "the first appended pose is primary")
    data = file_json(ed, WALK_FILE)
    ed.check_eq(settings(data), settings(before), "Append keeps the animation's settings")
    ed.check_eq([p["name"] for p in data["poses"][4:]], [p["name"] for p in wave["poses"]], "Wave's poses at the end")
    await ed.call("undo", scope="project")
    ed.check_eq(file_json(ed, WALK_FILE), before, "undo")
    await ed.check_error("focus_window", {"window": "pose_animation", "pose": {"templates": {"apply": {"name": "Walk", "mode": "mix"}}}},
                         contains="replace or append")


async def test_save_show_and_delete_a_user_template(ed: Editor):
    await focus(ed, path=WALK_FILE)
    await focus(ed, templates={"save": {"name": "My Walk", "description": "The test walk", "category": ""}})
    user_file = ed.project / "pose-templates" / "My Walk.poseanim"
    ed.check(user_file.exists(), "the project template file")
    saved = json.loads(user_file.read_text(encoding="utf-8"))
    ed.check_eq(saved["template"], {"name": "My Walk", "description": "The test walk", "category": "User"}, "name, description, category")
    ed.check_eq(saved["preview_model"], "", "templates have no preview model")
    st = await state(ed)
    ed.check_eq(st["templates"]["user"], ["My Walk"], f"a user card: {st['templates']}")
    ed.check_eq(st["templates"]["cards"][-1], "My Walk", "project templates after the built-ins")
    listed = (await ed.call("get_pose_animation", templates=True))["templates"]
    ed.check(any(t["name"] == "My Walk" and t["user"] for t in listed), "MCP lists it as user")
    await focus(ed, templates={"show": "My Walk"})
    await ed.check_error("focus_window", {"window": "pose_animation", "pose": {"templates": {"show": "Walk"}}}, contains="built-in")
    await ed.check_error("focus_window", {"window": "pose_animation", "pose": {"templates": {"delete": "Walk"}}}, contains="built-in")
    await focus(ed, templates={"delete": "my walk"})
    ed.check(not user_file.exists(), "Delete removes the file")
    ed.check_eq((await state(ed))["templates"]["user"], [], "the card is gone")
    await ed.call("undo", scope="project")
    ed.check(user_file.exists(), "undo brings the template back")
    await ed.call("wait", seconds=0.6)
    ed.check_eq((await state(ed))["templates"]["user"], ["My Walk"], "and its card")
