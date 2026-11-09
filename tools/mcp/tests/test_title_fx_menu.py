"""S83's catalogue in the Title Designer (docs/specs/S83-title-effects-generators.md AC19, R39-R43,
§6.10; S82 R17, AC38): the Add Effect menu's categories in order with counts, English and
Japanese search, the generator section, a recipe added in one step named after it (a person's
Enter in the palette), the seed dice as one step, the cost badge on an effect, and a following
generator whose layer was deleted showing "Follows a deleted layer" with Pick Layer.
"""

import json

import harness
from harness import Editor
from ui_input import Ui

CI = True
EDITOR = {"create": True, "name": "Title Fx Menu", "xvfb": "harness"}
TIMEOUT = 600

EFFECT_ORDER = ["Glow", "Shadow & Depth", "Stroke & Edge", "Color", "Texture & Material", "Stylize", "Blur", "Distort",
                "Generators that follow this layer"]
SEP = " › "

S: dict = {}


async def setup(ed: Editor) -> dict:
    if S.get("ed") is ed:
        return S
    S.clear()
    S["ed"] = ed
    await ed.call("tool_groups", enable=["title_art", "editor_tools", "editor_ux"])
    await ed.call("set_project_settings", values={"title_art.auto_bake": False})
    return S


async def focus(ed: Editor, **keys) -> dict:
    return await ed.call("focus_window", window="title_designer", title_designer=keys)


async def palette(ed: Editor, filter: str, query: str) -> dict:
    """The Add palette's state once it has drawn with this query (focus_window's result state is
    read after the palette opens over the window's focus)."""
    state: dict = {}
    for _ in range(5):
        state = (await focus(ed, add={"filter": filter, "query": query}))["state"]
        add = state.get("add")
        if add is not None and add.get("query") == query and add.get("rail") is not None:
            return add
    ed.fail(f"the Add palette did not open: {state}")
    return {}


async def top_step(ed: Editor) -> str:
    history = await ed.call("get_undo_history", scope="project", limit=1)
    steps = history.get("steps") or history.get("entries") or []
    return steps[0].get("name", "") if steps else ""


async def test_add_effect_categories_and_search(ed: Editor):
    """AC19: the categories in §6.10's order with counts; "power glow" finds Light Glow, "かすれ"
    Dry Brush (style setting) and Distress, "fire" the generator section."""
    await setup(ed)
    path = "assets/fxmenu/menu.titleart"
    await ed.call("edit_title", path=path, create={"text": "MENU"}, bake="never")
    await focus(ed, path=path, select="Title", panel="properties")
    add = await palette(ed, "effects", "")
    rail = [r["category"] for r in add["rail"]]
    expected = ["Effects" + SEP + c for c in EFFECT_ORDER]
    ed.check_eq(rail, expected, "the Add Effect categories in §6.10's order")
    ed.check(all(r["count"] > 0 for r in add["rail"]), f"every category counts its entries: {add['rail']}")
    add = await palette(ed, "effects", "power glow")
    ed.check_in("fx:light_glow", add["entries"], "power glow finds Light Glow")
    add = await palette(ed, "effects", "かすれ")
    ed.check_in("style:dry_brush", add["entries"], "かすれ finds Dry Brush (a style setting)")
    ed.check_in("fx:distress", add["entries"], "and Distress")
    add = await palette(ed, "", "fire")
    ed.check(any(e.startswith("gen:fire") for e in add["entries"]), f"fire shows the generator section: {add['entries']}")
    await focus(ed, add=False)


async def test_recipe_is_one_step(ed: Editor):
    """AC19: adding a recipe (Enter on the highlighted entry, a person's key) is one undo step
    named after it, and its effect lands in the stack."""
    await setup(ed)
    ui = Ui(ed)
    path = "assets/fxmenu/recipe.titleart"
    await ed.call("edit_title", path=path, create={"text": "LONG"}, bake="never")
    await focus(ed, path=path, select="Title", panel="properties")
    add = await palette(ed, "effects", "long shadow")
    ed.check_eq(add["entries"][:1], ["recipe:long_shadow"], f"the recipe is the first hit: {add['entries']}")
    await ui.key("Return")
    await ui.frames(4)
    ed.check_eq(await top_step(ed), "Add Effect Long Shadow to Title", "one step named after the recipe")
    effects = [e.get("type") for e in json.loads((ed.project / path).read_text())["layers"][0].get("effects", [])]
    ed.check_eq(effects, ["extrude"], "Long Shadow is an Extrude")


async def test_seed_dice_and_cost_badge(ed: Editor):
    """AC19 (R39, R41): the seed dice is one step "Randomize Seed"; the effect row has its cost badge."""
    await setup(ed)
    ui = Ui(ed)
    path = "assets/fxmenu/seed.titleart"
    await ed.call("edit_title", path=path, create={"text": "SEED"}, bake="never",
                  ops=[{"op": "add_effect", "layer": "Title", "type": "light_glow"},
                       {"op": "add_layer", "type": "generator", "generator": "speed_lines", "follow": "Title"}])
    await focus(ed, path=path, select="Title/fx:light_glow", panel="properties")
    badges = []
    for _ in range(10):
        await ui.frames(2)
        for cls in ("C0", "C1", "C2", "C3", "C4"):
            badges += (await ed.call("find_ui_item", label=cls)).get("items", [])
        if badges:
            break
    ed.check(len(badges) > 0, "the effect's page shows its cost badge")
    got = await ed.call("get_title", path=path)
    generator = next(layer["name"] for layer in got["layers"] if layer.get("type") == "generator")
    await focus(ed, select=generator, panel="properties")
    before = (ed.project / path).read_text()
    # the generator's section sits under Transform: fold it so the dice is in view
    await ui.click_item("Transform", "Title Designer")
    await ui.click_revealed("\uf522 Randomize Seed")  # the whole label finds it out of view
    await ui.frames(4)
    ed.check_eq(await top_step(ed), "Randomize Seed", "the dice is one step")
    ed.check(before != (ed.project / path).read_text(), "the seed changed")


async def test_follow_missing_warning(ed: Editor):
    """AC19 (R43): a following generator whose layer is deleted shows the warning and Pick Layer."""
    await setup(ed)
    path = "assets/fxmenu/follow.titleart"
    await ed.call("edit_title", path=path, create={"text": "FOLLOW"}, bake="never",
                  ops=[{"op": "add_layer", "type": "text", "name": "Other", "values": {"text": "B"}},
                       {"op": "add_layer", "type": "generator", "generator": "fire", "follow": "Other", "name": "Flames"}])
    await ed.call("edit_title", path=path, ops=[{"op": "remove_layer", "layer": "Other"}])
    await focus(ed, path=path, select="Flames", panel="properties")
    await ed.call("editor_status")
    items = (await ed.call("find_ui_item", label="Pick Layer")).get("items", [])
    ed.check(len(items) > 0, "Pick Layer is offered under the warning")
    checks = await ed.call("check_title", path=path)
    ed.check("fx_follow_missing" in str(checks), "check_title reports the same problem")
