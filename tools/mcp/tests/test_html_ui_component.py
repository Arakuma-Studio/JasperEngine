"""The UIDocument component (HTML/CSS UI, phase 0) as an editor component, through MCP.

Non-visual (the document's pixels are test_html_ui_basic.py's): write_file takes .html/.css,
list_component_types lists uidocument with its properties, add_component / set_properties /
get_entity with the filename asset-path validation, scene save / close / reopen (YAML key
uidocument), copy / paste and duplicate of GameObjects, copy / paste of the component, the
prefab round trip, remove + undo, and a Console without errors except the expected one for a
missing file.

Runs on a scratch copy of the fixture project (assets in assets/: MCP paths are
project-relative, assets/ui/basic.html; the component stores the asset-relative ui/basic.html).
"""

import yaml

from harness import Editor

TIMEOUT = 900

S: dict = {}

HTML = """<html>
<head>
    <title>basic</title>
    <link type="text/css" href="basic.css"/>
</head>
<body>
    <div id="box"></div>
</body>
</html>
"""

CSS = """body { display: block; width: 100%; height: 100%; margin: 0px; }
div { display: block; }
#box { position: absolute; left: 40px; top: 40px; width: 200px; height: 100px; background-color: #ff0000ff; }
"""

DOC = "assets/ui/basic.html"


async def console_errors(ed: Editor) -> list[str]:
    entries = await ed.console_since(types=["error"], limit=1000)
    return [e.get("message", "") for e in entries]


async def doc_values(ed: Editor, entity, scene=None) -> dict:
    got = await ed.call("get_entity", entity=entity, scene=scene if scene is not None else S["scene"],
                        components=["uidocument"])
    return got["components"]["uidocument"]


def scene_entities(path) -> list:
    """GameObjects of a saved .scene file (the YAML documents' entity nodes)."""
    data = yaml.safe_load(path.read_text())
    found = []

    def walk(node):
        if isinstance(node, dict):
            if "components" in node and isinstance(node["components"], dict):
                found.append(node)
            for v in node.values():
                walk(v)
        elif isinstance(node, list):
            for v in node:
                walk(v)

    walk(data)
    return found


async def test_write_ui_files(ed: Editor):
    # Console cursor: errors are counted from here
    await ed.console_since(None, limit=1)
    for path, content in ((DOC, HTML), ("assets/ui/basic.css", CSS)):
        await ed.call("write_file", path=path, content=content)
        ed.check((ed.project / path).read_text() == content, f"{path} not written as given")
        read = await ed.call_raw("read_file", {"path": path})
        ed.check(not read.is_error and "basic" in read.text, f"read_file {path}: {read.text[:300]}")
    # edits too (the text-file check of write_file)
    await ed.call("write_file", path=DOC, edits=[{"old_text": "<title>basic</title>", "new_text": "<title>basic doc</title>"}])
    ed.check("basic doc" in (ed.project / DOC).read_text(), "write_file edits on .html")
    # the other HTML UI extensions are text files as well
    for path in ("assets/ui/extra.htm", "assets/ui/extra.rml", "assets/ui/extra.rcss"):
        await ed.call("write_file", path=path, content="<!-- x -->\n")


async def test_component_type_listed(ed: Editor):
    listed = await ed.call("list_component_types")
    entries = {c["name"]: c for c in listed["components"]}
    ed.check("uidocument" in entries, f"uidocument not listed: {sorted(entries)}")
    ed.check_eq(entries["uidocument"]["display_name"], "UI Document")
    ed.check(entries["uidocument"]["addable"], "uidocument should be addable (Add Component > UI)")

    one = await ed.call("list_component_types", component="UIDocument")
    ed.check_eq(one["component"], "uidocument")
    props = one["properties"]
    # filename is an asset reference (S04: GUID + path), set and read as a path
    for name, kind in (("filename", "asset"), ("sortOrder", "int"), ("enabled", "bool")):
        ed.check(name in props, f"{name} missing: {props}")
        ed.check(kind in props[name]["type"].lower(), f"{name} type {props[name]['type']}")
    ed.check_eq(props["sortOrder"].get("default"), 0)
    ed.check_eq(props["enabled"].get("default"), True)
    # the display name resolves too
    one = await ed.call("list_component_types", component="UI Document")
    ed.check_eq(one["component"], "uidocument")


async def test_add_component(ed: Editor):
    made = await ed.call("create_scene", name="HtmlUiDoc", type="UI")
    S["scene"] = made["scene"]["id"]
    ent = await ed.call("create_entity", scene=S["scene"], type="empty", name="Menu")
    S["doc"] = ent["entity_id"]
    await ed.call("add_component", scene=S["scene"], entity=S["doc"], component="UIDocument",
                  values={"uidocument": {"filename": DOC, "sortOrder": 2}})
    v = await doc_values(ed, S["doc"])
    ed.check_eq(v["filename"], DOC, "filename read back (project-relative)")
    ed.check_eq(v["sortOrder"], 2)
    ed.check_eq(v["enabled"], True)

    # a second one is refused, the first one kept
    await ed.check_error("add_component", scene=S["scene"], entity=S["doc"], component="uidocument", contains="already")
    ed.check_eq((await doc_values(ed, S["doc"]))["sortOrder"], 2, "re-add changed the component")

    summary = await ed.call("get_entity", scene=S["scene"], entity=S["doc"], detail="summary")
    ed.check("uidocument" in str(summary).lower(), f"summary lacks the component: {summary}")


async def test_filename_validation(ed: Editor):
    for bad, words in (("../outside.html", "project-relative"), ("/tmp/outside.html", "project-relative"),
                       ("scripts/outside.html", "assets")):
        await ed.check_error("set_properties", scene=S["scene"], entity=S["doc"],
                             values={"uidocument": {"filename": bad}}, contains=words)
    ed.check_eq((await doc_values(ed, S["doc"]))["filename"], DOC, "a rejected path changed the value")


async def test_set_properties(ed: Editor):
    out = await ed.call("set_properties", scene=S["scene"], entity=S["doc"],
                        values={"uidocument": {"sortOrder": -3, "enabled": False}})
    ed.check("-3" in str(out), f"set_properties result: {out}")
    v = await doc_values(ed, S["doc"])
    ed.check_eq(v["sortOrder"], -3)
    ed.check_eq(v["enabled"], False)
    await ed.call("undo", scene=S["scene"])
    v = await doc_values(ed, S["doc"])
    ed.check_eq(v["sortOrder"], 2, "undo of set_properties")
    ed.check_eq(v["enabled"], True, "undo of set_properties")

    # an empty filename is allowed (no document)
    await ed.call("set_properties", scene=S["scene"], entity=S["doc"], values={"uidocument": {"filename": ""}})
    ed.check_eq((await doc_values(ed, S["doc"]))["filename"], "")
    await ed.call("set_properties", scene=S["scene"], entity=S["doc"], values={"uidocument": {"filename": DOC}})
    ed.check_eq((await doc_values(ed, S["doc"]))["filename"], DOC)


async def test_save_close_reopen(ed: Editor):
    await ed.call("set_properties", scene=S["scene"], entity=S["doc"], values={"uidocument": {"sortOrder": 7, "enabled": False}})
    saved = await ed.call("save_scene", scene=S["scene"], path="scenes/HtmlUiDoc")
    path = saved["scene"]["path"]
    ed.check_eq(path, "scenes/HtmlUiDoc.scene")

    nodes = [n for n in scene_entities(ed.project / path) if "uidocument" in n["components"]]
    ed.check_eq(len(nodes), 1, "one uidocument in the saved scene")
    stored = nodes[0]["components"]["uidocument"]
    ed.check_eq(stored.get("filename"), "ui/basic.html", f"YAML stores the asset-relative path: {stored}")
    ed.check_eq(stored.get("sortOrder"), 7)
    ed.check_eq(stored.get("enabled"), False)

    await ed.call("close_scene", scene=S["scene"])
    await ed.call("open_scene", scene=path)
    found = await ed.call("find_entities", scene=S["scene"], name="Menu")
    ed.check_eq(found["count"], 1, "Menu after reopen")
    S["doc"] = found["entities"][0]["id"]
    v = await doc_values(ed, S["doc"])
    ed.check_eq(v["filename"], DOC)
    ed.check_eq(v["sortOrder"], 7)
    ed.check_eq(v["enabled"], False)
    await ed.call("set_properties", scene=S["scene"], entity=S["doc"], values={"uidocument": {"sortOrder": 2, "enabled": True}})


async def test_copy_paste_and_duplicate(ed: Editor):
    await ed.call("copy_entities", scene=S["scene"], entity=S["doc"])
    out = await ed.call("paste_entities", scene=S["scene"])
    pasted = out["created"][0]["id"]
    v = await doc_values(ed, pasted)
    ed.check_eq((v["filename"], v["sortOrder"], v["enabled"]), (DOC, 2, True), "pasted values")

    # into another scene (another registry)
    out = await ed.call("paste_entities", scene="Main Scene")
    other = out["created"][0]["id"]
    v = await doc_values(ed, other, scene="Main Scene")
    ed.check_eq((v["filename"], v["sortOrder"]), (DOC, 2), "pasted into Main Scene")
    await ed.call("undo", scene="Main Scene")

    dup = await ed.call("duplicate_entities", scene=S["scene"], entity=S["doc"])
    copy = dup["created"][0]["id"] if isinstance(dup, dict) and "created" in dup else dup[0]["id"]
    v = await doc_values(ed, copy)
    ed.check_eq((v["filename"], v["sortOrder"], v["enabled"]), (DOC, 2, True), "duplicated values")
    await ed.call("delete_entities", scene=S["scene"], entities=[pasted, copy])


async def test_copy_paste_component(ed: Editor):
    target = (await ed.call("create_entity", scene=S["scene"], type="empty", name="Target"))["entity_id"]
    await ed.call("copy_component", scene=S["scene"], entity=S["doc"], component="uidocument")
    await ed.call("paste_component", scene=S["scene"], entity=target, as_new=True)
    v = await doc_values(ed, target)
    ed.check_eq((v["filename"], v["sortOrder"]), (DOC, 2), "pasted as new")

    # Paste Component Values over an existing one
    await ed.call("set_properties", scene=S["scene"], entity=target, values={"uidocument": {"sortOrder": 9, "filename": ""}})
    await ed.call("paste_component", scene=S["scene"], entity=target)
    v = await doc_values(ed, target)
    ed.check_eq((v["filename"], v["sortOrder"]), (DOC, 2), "pasted values")
    await ed.call("delete_entities", scene=S["scene"], entities=[target])


async def test_prefab_round_trip(ed: Editor):
    made = await ed.call("create_prefab", scene=S["scene"], entity=S["doc"], path="prefabs/Menu.prefab")
    ed.check_eq(made["prefabs"][0]["path"], "prefabs/Menu.prefab")
    text = (ed.project / "prefabs/Menu.prefab").read_text()
    ed.check("uidocument" in text and "ui/basic.html" in text, f"prefab file lacks the component: {text[:600]}")

    inst = await ed.call("instantiate_prefab", scene=S["scene"], prefab="prefabs/Menu.prefab", name="Menu 2")
    second = inst["instance"]["id"]
    v = await doc_values(ed, second)
    ed.check_eq((v["filename"], v["sortOrder"], v["enabled"]), (DOC, 2, True), "instance values")

    # an override on the instance, then reverted
    await ed.call("set_properties", scene=S["scene"], entity=second, values={"uidocument": {"sortOrder": 5}})
    info = await ed.call("get_prefab_info", scene=S["scene"], entity=second)
    ed.check("sortOrder" in str(info.get("overrides")), f"sortOrder override not listed: {info.get('overrides')}")
    await ed.call("prefab_overrides", action="revert", scene=S["scene"], entity=second, component="uidocument",
                  properties=["sortOrder"])
    ed.check_eq((await doc_values(ed, second))["sortOrder"], 2, "reverted override")
    await ed.call("delete_entities", scene=S["scene"], entities=[second])


async def test_remove_and_undo(ed: Editor):
    await ed.call("remove_component", scene=S["scene"], entity=S["doc"], component="uidocument")
    got = await ed.call("get_entity", scene=S["scene"], entity=S["doc"], detail="summary")
    ed.check("uidocument" not in str(got.get("components", "")).lower(), f"still has the component: {got}")
    await ed.call("undo", scene=S["scene"])
    v = await doc_values(ed, S["doc"])
    ed.check_eq((v["filename"], v["sortOrder"]), (DOC, 2), "remove undone")


async def test_console(ed: Editor):
    # nothing so far logged an error (documents load in the editor's edit mode too)
    errors = await console_errors(ed)
    ed.check(not errors, f"unexpected Console errors: {errors}")

    # a missing file: one error naming it, no crash
    await ed.call("set_properties", scene=S["scene"], entity=S["doc"], values={"uidocument": {"filename": "assets/ui/missing.html"}})
    await ed.call("select_scene", scene=S["scene"])
    await ed.screenshot(view="game")
    errors = await console_errors(ed)
    ed.check(any("ui/missing.html" in e and "[HTML UI]" in e for e in errors), f"no error naming the missing file: {errors}")
    ed.check(all("ui/missing.html" in e or "Unable to open" in e for e in errors), f"other errors: {errors}")
    await ed.check_alive()
    await ed.call("set_properties", scene=S["scene"], entity=S["doc"], values={"uidocument": {"filename": DOC}})
    await ed.screenshot(view="game")
    errors = await console_errors(ed)
    ed.check(not errors, f"errors after fixing the filename: {errors}")
