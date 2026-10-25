"""UI Document editor contracts through MCP: creation, metadata, paths, command history,
scene storage, clipboard, prefabs, asset typing, graphics configuration and the Scene view.

The phase-0 and phase-1 component scenarios share one scratch editor. Core document behavior
is owned by the native HTML UI tests; these cases protect the editor's public adapters.
"""

import yaml

from harness import Editor
from htmlui_pixels import Shot, check_color, count_near

TIMEOUT = 900

S: dict = {}

RED_HTML = """<html>
<head>
    <title>red</title>
    <style>
        body { display: block; position: absolute; left: 0; top: 0; width: 100%; height: 100%; margin: 0; }
        #box { display: block; position: absolute; left: 0; top: 0; width: 100%; height: 100%; background-color: #ff0000; }
    </style>
</head>
<body><div id="box"></div></body>
</html>
"""

BLUE_HTML = RED_HTML.replace("#ff0000", "#0000ff").replace("<title>red</title>", "<title>blue</title>")

RED_DOC = "assets/ui/red.html"
BLUE_DOC = "assets/ui/blue.html"
RED = (255, 0, 0)
BLUE = (0, 0, 255)
CENTER = (640, 360)

DEFAULTS = {"interactive": True, "renderMode": "screen", "textureWidth": 1024, "textureHeight": 1024,
            "pixelRatio": 0.0}


async def doc_values(ed: Editor, entity, scene=None) -> dict:
    got = await ed.call("get_entity", entity=entity, scene=scene if scene is not None else S["scene"],
                        components=["uidocument"])
    return got["components"]["uidocument"]


async def game_shot(ed: Editor, label: str) -> Shot:
    shot = await ed.call_raw("screenshot", {"view": "game", "scene": S["scene"], "max_width": 1280})
    ed.check(not shot.is_error and shot.images, f"{label}: no screenshot: {shot.text[:300]}")
    (ed.scratch.root / f"html-ui-editor-{label}.png").write_bytes(shot.images[0][0])
    return Shot(shot.images[0][0], shot.value)


def saved_documents(path) -> list:
    data = yaml.safe_load(path.read_text())
    found = []

    def walk(node):
        if isinstance(node, dict):
            comps = node.get("components")
            if isinstance(comps, dict) and "uidocument" in comps:
                found.append(comps["uidocument"])
            for v in node.values():
                walk(v)
        elif isinstance(node, list):
            for v in node:
                walk(v)

    walk(data)
    return found


async def test_create_ui_document(ed: Editor):
    for path, content in ((RED_DOC, RED_HTML), (BLUE_DOC, BLUE_HTML), ("assets/ui/style.css", "body { display: block; }\n")):
        await ed.call("write_file", path=path, content=content)

    made = await ed.call("create_scene", name="HtmlUiEditor", type="UI")
    S["scene"] = made["scene"]["id"]
    out = await ed.call("create_entity", scene=S["scene"], type="ui_document", name="Menu")
    S["doc"] = out["entity_id"]
    ed.check_eq(out["type"], "ui_document")
    ed.check_in("uidocument", out["components"])
    got = await ed.call("get_entity", scene=S["scene"], entity=S["doc"], components=["transform", "uidocument"])
    ed.check("transform" in got["components"], f"a Transform: {list(got['components'])}")

    v = await doc_values(ed, S["doc"])
    ed.check_eq(v["filename"], "", "no file until the Inspector's Source is set")
    for key, value in DEFAULTS.items():
        ed.check_eq(v[key], value, f"default {key}")
    ed.check(v.get("inputSurface") in (None, 0, "", {}) or not v.get("inputSurface"), f"no input surface: {v.get('inputSurface')}")


async def test_component_type_info(ed: Editor):
    one = await ed.call("list_component_types", component="uidocument")
    props = one["properties"]
    for name, kind in (("filename", "string"), ("sortOrder", "int"), ("enabled", "bool"),
                       ("interactive", "bool"), ("renderMode", "enum"), ("textureWidth", "int"),
                       ("textureHeight", "int"), ("inputSurface", "entity"), ("pixelRatio", "float")):
        ed.check(name in props, f"{name} missing: {sorted(props)}")
        ed.check(kind in props[name]["type"].lower(), f"{name} type {props[name]['type']}")
    ed.check_eq(sorted(props["renderMode"].get("options", [])), ["screen", "texture"],
                f"renderMode values: {props['renderMode']}")
    ed.check_eq(props["textureWidth"].get("default"), 1024)
    ed.check_eq(props["interactive"].get("default"), True)
    ed.check_eq(props["sortOrder"].get("default"), 0)
    ed.check_eq(props["enabled"].get("default"), True)


async def test_add_remove_component(ed: Editor):
    target = (await ed.call("create_entity", scene=S["scene"], type="empty", name="Added"))["entity_id"]
    await ed.call("add_component", scene=S["scene"], entity=target, component="UIDocument",
                  values={"uidocument": {"filename": RED_DOC, "sortOrder": 2}})
    ed.check_eq((await doc_values(ed, target))["filename"], RED_DOC)
    await ed.check_error("add_component", scene=S["scene"], entity=target, component="uidocument", contains="already")
    ed.check_eq((await doc_values(ed, target))["sortOrder"], 2, "re-add changed the component")
    await ed.call("remove_component", scene=S["scene"], entity=target, component="uidocument")
    got = await ed.call("get_entity", scene=S["scene"], entity=target, detail="summary")
    ed.check("uidocument" not in got.get("components", {}), f"still has the component: {got}")
    await ed.call("undo", scene=S["scene"])
    values = await doc_values(ed, target)
    ed.check_eq((values["filename"], values["sortOrder"]), (RED_DOC, 2), "remove undone")
    await ed.call("delete_entities", scene=S["scene"], entities=[target])


async def test_filename_validation(ed: Editor):
    for bad, words in (("../outside.html", "project-relative"), ("/tmp/outside.html", "project-relative"),
                       ("scripts/outside.html", "assets")):
        await ed.check_error("set_properties", scene=S["scene"], entity=S["doc"],
                             values={"uidocument": {"filename": bad}}, contains=words)
    ed.check_eq((await doc_values(ed, S["doc"]))["filename"], "", "a rejected path changed the value")


async def test_set_every_field(ed: Editor):
    surface = (await ed.call("create_entity", scene=S["scene"], type="empty", name="Surface"))["entity_id"]
    S["surface"] = surface
    await ed.call("set_properties", scene=S["scene"], entity=S["doc"], values={"uidocument": {
        "filename": RED_DOC, "sortOrder": 3, "interactive": False, "renderMode": "texture",
        "textureWidth": 512, "textureHeight": 256, "inputSurface": surface, "pixelRatio": 2.0}})
    v = await doc_values(ed, S["doc"])
    ed.check_eq((v["filename"], v["sortOrder"], v["interactive"], v["renderMode"], v["textureWidth"], v["textureHeight"],
                 v["pixelRatio"]), (RED_DOC, 3, False, "texture", 512, 256, 2.0), f"read back: {v}")
    ref = v["inputSurface"]
    ed.check(ref == surface or (isinstance(ref, dict) and ref.get("id") == surface), f"inputSurface: {ref}")

    # texture sizes are clamped to 1..8192
    await ed.call("set_properties", scene=S["scene"], entity=S["doc"], values={"uidocument": {"textureWidth": 9000, "textureHeight": 0}})
    v = await doc_values(ed, S["doc"])
    ed.check_eq((v["textureWidth"], v["textureHeight"]), (8192, 1), "clamped texture size")

    # a render mode that does not exist is refused, naming the valid ones
    msg = await ed.call("set_properties", scene=S["scene"], entity=S["doc"], values={"uidocument": {"renderMode": "world"}},
                        expect_error=True)
    ed.check("screen" in msg and "texture" in msg, f"renderMode error: {msg}")

    await ed.call("set_properties", scene=S["scene"], entity=S["doc"], values={"uidocument": {"filename": ""}})
    ed.check_eq((await doc_values(ed, S["doc"]))["filename"], "", "empty source is allowed")
    await ed.call("set_properties", scene=S["scene"], entity=S["doc"], values={"uidocument": {"filename": RED_DOC}})

    # undo / redo of one change
    await ed.call("set_properties", scene=S["scene"], entity=S["doc"], values={"uidocument": {"interactive": True}})
    await ed.call("undo", scene=S["scene"])
    ed.check_eq((await doc_values(ed, S["doc"]))["interactive"], False, "undo")
    await ed.call("redo", scene=S["scene"])
    ed.check_eq((await doc_values(ed, S["doc"]))["interactive"], True, "redo")


async def test_save_reopen_yaml(ed: Editor):
    await ed.call("set_properties", scene=S["scene"], entity=S["doc"], values={"uidocument": {
        "enabled": False, "interactive": False, "textureWidth": 640, "textureHeight": 480, "pixelRatio": 1.5}})
    saved = await ed.call("save_scene", scene=S["scene"], path="scenes/HtmlUiEditor")
    path = ed.project / saved["scene"]["path"]
    S["path"] = saved["scene"]["path"]
    docs = saved_documents(path)
    ed.check_eq(len(docs), 1)
    stored = docs[0]
    ed.check_eq(stored.get("filename"), "ui/red.html", f"asset-relative filename: {stored}")
    ed.check_eq((stored.get("sortOrder"), stored.get("enabled")), (3, False), "phase-0 keys")
    ed.check_eq((stored.get("interactive"), stored.get("renderMode"), stored.get("textureWidth"), stored.get("textureHeight"),
                 stored.get("pixelRatio")), (False, "texture", 640, 480, 1.5), f"phase-1 keys: {stored}")
    ed.check("inputSurface" in stored, f"inputSurface stored: {stored}")

    await ed.call("close_scene", scene=S["scene"])
    await ed.call("open_scene", scene=S["path"])
    found = await ed.call("find_entities", scene=S["scene"], name="Menu")
    ed.check_eq(found["count"], 1)
    S["doc"] = found["entities"][0]["id"]
    S["surface"] = (await ed.call("find_entities", scene=S["scene"], name="Surface"))["entities"][0]["id"]
    v = await doc_values(ed, S["doc"])
    ed.check_eq((v["filename"], v["sortOrder"], v["interactive"], v["renderMode"], v["textureWidth"], v["textureHeight"],
                 v["pixelRatio"]), (RED_DOC, 3, False, "texture", 640, 480, 1.5), f"reopened: {v}")
    ed.check_eq(v["enabled"], False, "disabled survives reopening")
    ref = v["inputSurface"]
    ed.check(ref == S["surface"] or (isinstance(ref, dict) and ref.get("id") == S["surface"]), f"reopened inputSurface: {ref}")

    # back to the defaults: the phase-1 keys leave the file, the phase-0 ones stay
    await ed.call("set_properties", scene=S["scene"], entity=S["doc"], values={"uidocument": {
        "interactive": True, "renderMode": "screen", "textureWidth": 1024, "textureHeight": 1024, "pixelRatio": 0,
        "inputSurface": None, "sortOrder": 0, "enabled": True}})
    await ed.call("save_scene", scene=S["scene"])
    stored = saved_documents(path)[0]
    for key in DEFAULTS:
        ed.check(key not in stored, f"default {key} written: {stored}")
    ed.check("inputSurface" not in stored, f"default inputSurface written: {stored}")
    ed.check_eq((stored.get("filename"), stored.get("sortOrder"), stored.get("enabled")), ("ui/red.html", 0, True))


async def test_filename_change_reloads(ed: Editor):
    shot = await game_shot(ed, "red")
    check_color(ed, shot, CENTER, RED, "the red document")
    await ed.call("set_properties", scene=S["scene"], entity=S["doc"], values={"uidocument": {"filename": BLUE_DOC}})
    shot = await game_shot(ed, "blue")
    check_color(ed, shot, CENTER, BLUE, "the new filename's document")
    await ed.call("undo", scene=S["scene"])
    shot = await game_shot(ed, "red-again")
    check_color(ed, shot, CENTER, RED, "undo loads the old document again")


async def test_copy_paste_and_duplicate(ed: Editor):
    await ed.call("copy_entities", scene=S["scene"], entity=S["doc"])
    out = await ed.call("paste_entities", scene=S["scene"])
    pasted = out["created"][0]["id"]
    v = await doc_values(ed, pasted)
    ed.check_eq((v["filename"], v["sortOrder"], v["enabled"]), (RED_DOC, 0, True), "pasted values")

    # into another scene (another registry)
    out = await ed.call("paste_entities", scene="Main Scene")
    other = out["created"][0]["id"]
    v = await doc_values(ed, other, scene="Main Scene")
    ed.check_eq((v["filename"], v["sortOrder"]), (RED_DOC, 0), "pasted into Main Scene")
    await ed.call("undo", scene="Main Scene")

    dup = await ed.call("duplicate_entities", scene=S["scene"], entity=S["doc"])
    copy = dup["created"][0]["id"] if isinstance(dup, dict) and "created" in dup else dup[0]["id"]
    v = await doc_values(ed, copy)
    ed.check_eq((v["filename"], v["sortOrder"], v["enabled"]), (RED_DOC, 0, True), "duplicated values")
    await ed.call("delete_entities", scene=S["scene"], entities=[pasted, copy])


async def test_copy_paste_component(ed: Editor):
    await ed.call("set_properties", scene=S["scene"], entity=S["doc"], values={"uidocument": {"interactive": False, "pixelRatio": 1.25}})
    target = (await ed.call("create_entity", scene=S["scene"], type="ui_document", name="Target"))["entity_id"]
    await ed.call("set_properties", scene=S["scene"], entity=target, values={"uidocument": {"textureWidth": 77, "renderMode": "texture"}})
    await ed.call("copy_component", scene=S["scene"], entity=S["doc"], component="uidocument")
    await ed.call("paste_component", scene=S["scene"], entity=target)
    v = await doc_values(ed, target)
    # every field is the copied one's: defaults omitted from the copy still overwrite the target's values
    ed.check_eq((v["filename"], v["interactive"], v["pixelRatio"], v["textureWidth"], v["renderMode"]),
                (RED_DOC, False, 1.25, 1024, "screen"), f"pasted values: {v}")
    await ed.call("undo", scene=S["scene"])
    v = await doc_values(ed, target)
    ed.check_eq((v["textureWidth"], v["renderMode"]), (77, "texture"), "paste undone")
    await ed.call("delete_entities", scene=S["scene"], entities=[target])


async def test_prefab_override(ed: Editor):
    made = await ed.call("create_prefab", scene=S["scene"], entity=S["doc"], path="prefabs/HtmlDoc.prefab")
    ed.check_eq(made["prefabs"][0]["path"], "prefabs/HtmlDoc.prefab")
    inst = await ed.call("instantiate_prefab", scene=S["scene"], prefab="prefabs/HtmlDoc.prefab", name="Doc 2")
    second = inst["instance"]["id"]
    v = await doc_values(ed, second)
    ed.check_eq((v["filename"], v["sortOrder"], v["interactive"], v["pixelRatio"]),
                (RED_DOC, 0, False, 1.25), "prefab instance preserves the document")
    await ed.call("set_properties", scene=S["scene"], entity=second, values={"uidocument": {"sortOrder": 9, "interactive": True}})
    info = await ed.call("get_prefab_info", scene=S["scene"], entity=second)
    overrides = str(info.get("overrides"))
    ed.check("sortOrder" in overrides and "interactive" in overrides, f"overrides: {overrides}")
    await ed.call("prefab_overrides", action="revert", scene=S["scene"], entity=second, component="uidocument",
                  properties=["sortOrder"])
    v = await doc_values(ed, second)
    ed.check_eq((v["sortOrder"], v["interactive"]), (0, True), "sortOrder reverted, interactive kept")
    await ed.call("delete_entities", scene=S["scene"], entities=[second])


async def test_asset_types(ed: Editor):
    listed = await ed.call("list_assets", directory="assets/ui", type="html")
    ed.check(RED_DOC in str(listed) and BLUE_DOC in str(listed), f"html assets: {listed}")
    ed.check("style.css" not in str(listed), f"a .css listed as html: {listed}")
    ed.check(all(e["type"] == "html" for e in listed["entries"]), f"types: {listed}")
    listed = await ed.call("list_assets", directory="assets/ui", type="css")
    ed.check("style.css" in str(listed) and "red.html" not in str(listed), f"css assets: {listed}")
    info = await ed.call("get_asset_info", path=RED_DOC)
    ed.check_eq(info["type"], "html")
    ed.check_eq(info.get("inside_assets_dir"), True)


async def test_msaa_setting(ed: Editor):
    got = await ed.call("get_project_settings", keys=["graphics.html_ui_msaa_samples"])
    ed.check_eq(got["settings"]["graphics.html_ui_msaa_samples"], 4, "default sample count")
    await ed.call("set_project_settings", values={"graphics.html_ui_msaa_samples": 2})
    got = await ed.call("get_project_settings", keys=["graphics.html_ui_msaa_samples"])
    ed.check_eq(got["settings"]["graphics.html_ui_msaa_samples"], 2)
    msg = await ed.call("set_project_settings", values={"graphics.html_ui_msaa_samples": 3}, expect_error=True)
    ed.check_in("1, 2, 4 or 8", msg)
    text = (ed.project / "project.yaml").read_text()
    ed.check_eq(yaml.safe_load(text)["htmlUiMsaaSamples"], 2, "saved graphics setting")
    ed.check_eq((await ed.call("get_project_settings", keys=["graphics.html_ui_msaa_samples"]))["settings"]["graphics.html_ui_msaa_samples"], 2, "invalid sample count leaves the setting unchanged")


async def test_scene_view_shows_document(ed: Editor):
    """Documents show in the Scene view of a UI scene in edit mode (p1-render composites them
    on the canvas rectangle in the 2D/UI camera)."""
    await ed.call("select_scene", scene=S["scene"])
    await ed.call("set_properties", scene=S["scene"], entity=S["doc"], values={"uidocument": {"filename": RED_DOC, "enabled": True}})
    await ed.call("select_entities", scene=S["scene"], entities=[S["doc"]])
    # the whole canvas (0,0)-(1280,720) in view
    await ed.call("set_scene_camera", scene=S["scene"], center=[640, 360], zoom=2.0)

    async def red_pixels(label: str) -> int:
        shot = await ed.call_raw("screenshot", {"view": "scene", "scene": S["scene"], "max_width": 800})
        ed.check(not shot.is_error and shot.images, f"{label}: no scene screenshot: {shot.text[:300]}")
        png = shot.images[0][0]
        (ed.scratch.root / f"html-ui-editor-scene-{label}.png").write_bytes(png)
        return count_near(png, RED)

    shown = await red_pixels("shown")
    await ed.call("set_properties", scene=S["scene"], entity=S["doc"], values={"uidocument": {"enabled": False}})
    hidden = await red_pixels("hidden")
    await ed.call("set_properties", scene=S["scene"], entity=S["doc"], values={"uidocument": {"enabled": True}})
    ed.check(shown > 200, f"the Scene view shows the red document ({shown} red samples)")
    ed.check(hidden < shown // 10, f"disabled: no document in the Scene view ({hidden} red samples)")
