"""W4 asset tools: listing/info/preview, file operations with undo, import, placeholder art,
materials (create/update/link), shader forks with compile errors, text file read/write."""

import tempfile
from pathlib import Path

from harness import Editor, find_key

TIMEOUT = 600

PNG_MAGIC = b"\x89PNG\r\n\x1a\n"


async def make_entity(ed: Editor, name: str, type_: str = "box"):
    made = await ed.call("create_entity", name=name, type=type_)
    eid = find_key(made, "entity_id", "id")
    ed.check(eid is not None, f"create_entity returned no id: {made!r}")
    return eid


async def test_list_and_info(ed: Editor):
    ed.require_tools("list_assets", "get_asset_info")
    r = await ed.call("list_assets", type="texture", details=True)
    paths = {e["path"]: e for e in r["entries"]}
    ed.check_in("assets/sprites/plane-sheet.png", paths, "textures listed")
    ed.check(paths["assets/sprites/plane-sheet.png"].get("width", 0) > 0, "image dims with details")
    ed.check(all(e["type"] == "texture" for e in r["entries"]), "type filter")

    r = await ed.call("list_assets", pattern="assets/**/rock*.png")
    ed.check(r["entries"] and all("rock" in e["path"] for e in r["entries"]), f"glob: {r['entries']!r}")
    r = await ed.call("list_assets", directory="assets", recursive=False)
    ed.check(any(e["type"] == "folder" for e in r["entries"]), "non-recursive listing shows folders")

    info = await ed.call("get_asset_info", path="assets/sprites/rock.png")
    ed.check(info["width"] > 0 and info["height"] > 0, f"info dims: {info!r}")
    await ed.call("get_asset_info", path="assets/nope.png", expect_error=True)
    await ed.call("get_asset_info", path="../outside.png", expect_error=True)


async def test_view_asset(ed: Editor):
    ed.require_tools("view_asset")
    r = await ed.call_raw("view_asset", {"path": "assets/sprites/background.png", "max_size": 128})
    ed.check(not r.is_error, r.text)
    ed.check(r.images, "view_asset returned no image")
    data, mime = r.images[0]
    ed.check_eq(mime, "image/png", "mime")
    ed.check(data.startswith(PNG_MAGIC), "not a PNG")
    ed.check(max(r.structured["preview_width"], r.structured["preview_height"]) <= 128, "max_size respected")


async def test_file_ops_with_undo(ed: Editor):
    ed.require_tools("create_folder", "move_asset", "delete_asset", "undo", "redo")
    root = ed.project
    await ed.call("create_folder", path="assets/w4tmp")
    ed.check((root / "assets/w4tmp").is_dir(), "folder created")

    await ed.call("move_asset", path="assets/sprites/rock.png", to="assets/w4tmp/rock.png", copy=True)
    ed.check((root / "assets/w4tmp/rock.png").exists() and (root / "assets/sprites/rock.png").exists(), "copy")

    await ed.call("move_asset", path="assets/w4tmp/rock.png", to="assets/w4tmp/stone.png")
    ed.check((root / "assets/w4tmp/stone.png").exists() and not (root / "assets/w4tmp/rock.png").exists(), "rename")
    await ed.call("undo", scope="project")
    ed.check((root / "assets/w4tmp/rock.png").exists(), "undo rename")
    await ed.call("redo", scope="project")
    ed.check((root / "assets/w4tmp/stone.png").exists(), "redo rename")

    await ed.call("create_folder", path="assets/w4tmp/sub")
    await ed.call("move_asset", path="assets/w4tmp/stone.png", to="assets/w4tmp/sub/pebble.png")
    ed.check((root / "assets/w4tmp/sub/pebble.png").exists(), "move to another folder with a new name")
    await ed.call("undo", scope="project")
    ed.check((root / "assets/w4tmp/stone.png").exists(), "undo move+rename is one step")

    await ed.call("move_asset", path="assets/w4tmp/stone.png", to="assets/sprites/rock.png", expect_error=True)

    await ed.call("delete_asset", paths=["assets/w4tmp/stone.png"])
    ed.check(not (root / "assets/w4tmp/stone.png").exists(), "deleted")
    await ed.call("undo", scope="project")
    ed.check((root / "assets/w4tmp/stone.png").exists(), "undo delete restores from trash")
    await ed.call("delete_asset", paths=["project.yaml"], expect_error=True)


async def test_import_and_placeholder(ed: Editor):
    ed.require_tools("import_files", "generate_placeholder_art", "view_asset")
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / "notes.txt"
        src.write_text("hello")
        await ed.call("import_files", sources=[str(src)], to_dir="assets/imported")
        ed.check_eq((ed.project / "assets/imported/notes.txt").read_text(), "hello", "imported")
        await ed.call("import_files", sources=[str(src)], to_dir="assets/imported", expect_error=True)
    await ed.call("undo", scope="project")
    ed.check(not (ed.project / "assets/imported/notes.txt").exists(), "undo import")

    r = await ed.call_raw("generate_placeholder_art", {"path": "assets/sprites/ball.png", "width": 48, "height": 32,
                                                       "shape": "circle", "color": "#ff8000", "outline_width": 2})
    ed.check(not r.is_error, r.text)
    data = (ed.project / "assets/sprites/ball.png").read_bytes()
    ed.check(data.startswith(PNG_MAGIC), "placeholder is a PNG")
    info = await ed.call("get_asset_info", path="assets/sprites/ball.png")
    ed.check_eq((info["width"], info["height"]), (48, 32), "placeholder size")
    ed.check(r.images, "placeholder preview")
    await ed.call("generate_placeholder_art", path="assets/sprites/ball.png", shape="star", overwrite=True, expect_error=True)


async def test_text_files(ed: Editor):
    ed.require_tools("read_file", "write_file")
    await ed.call("write_file", path="notes/todo.md", content="one\ntwo\nthree\n")
    r = await ed.call_raw("read_file", {"path": "notes/todo.md", "start_line": 2, "max_lines": 1})
    ed.check("two" in r.text and "three" not in r.text.split(":", 1)[1], r.text)
    await ed.call("write_file", path="notes/todo.md", edits=[{"old_text": "two", "new_text": "TWO"}])
    ed.check("TWO" in (ed.project / "notes/todo.md").read_text(), "edit applied")
    await ed.call("write_file", path="notes/todo.md", edits=[{"old_text": "missing", "new_text": "x"}], expect_error=True)
    await ed.call("undo", scope="project")
    ed.check("two" in (ed.project / "notes/todo.md").read_text(), "undo edit")
    await ed.call("read_file", path="assets/sprites/rock.png", expect_error=True)  # binary


async def test_materials(ed: Editor):
    ed.require_tools("create_material", "update_material", "link_material", "get_asset_info")
    eid = await make_entity(ed, "MatBox")
    r = await ed.call("create_material", path="assets/materials/Red", values={"baseColorFactor": [1, 0, 0, 1], "roughnessFactor": 0.3},
                      entity=eid)
    ed.check_eq(r["path"], "assets/materials/Red.material", "extension added")
    ed.check(r.get("linked"), f"linked: {r!r}")
    info = await ed.call("get_asset_info", path="assets/materials/Red.material")
    ed.check_eq(info["material"]["baseColorFactor"], [1, 0, 0, 1], "stored color")
    ed.check(any(u["entity"]["id"] == eid for u in info["linked_by"]), f"linked_by: {info['linked_by']!r}")

    await ed.call("update_material", path="assets/materials/Red.material", values={"metallicFactor": 0.0, "alphaMode": "BLEND"})
    info = await ed.call("get_asset_info", path="assets/materials/Red.material")
    ed.check_eq(info["material"]["alphaMode"], "BLEND", "alpha mode updated")
    await ed.call("update_material", path="assets/materials/Red.material", values={"bogus": 1}, expect_error=True)
    await ed.call("update_material", path="assets/materials/Red.material", values={"baseColorTexture": "assets/sprites/rock.png"})

    await ed.call("link_material", entity=eid, material=None)
    info = await ed.call("get_asset_info", path="assets/materials/Red.material")
    ed.check(not any(u["entity"]["id"] == eid for u in info["linked_by"]), "unlinked")
    await ed.call("link_material", entity=eid, material="assets/materials/Red.material")
    info = await ed.call("get_asset_info", path="assets/materials/Red.material")
    ed.check(any(u["entity"]["id"] == eid for u in info["linked_by"]), "relinked")

    thumb = await ed.call_raw("view_asset", {"path": "assets/materials/Red.material"}, timeout=60)
    ed.check(not thumb.is_error and thumb.images, f"material thumbnail: {thumb.text}")


async def test_shader_fork_and_errors(ed: Editor):
    ed.require_tools("fork_shader", "write_file", "check_shader", "get_shader_uniforms", "set_shader_uniform")
    eid = await make_entity(ed, "Glow")
    r = await ed.call("fork_shader", entity=eid, directory="shaders")
    frag = r["frag_path"]
    ed.check((ed.project / frag).exists() and (ed.project / r["vert_path"]).exists(), f"fork files: {r!r}")
    ok = await ed.call("check_shader", shader=r["custom_shader"], timeout=300)
    ed.check_eq(ok["failed"], 0, f"fresh fork compiles: {ok!r}")

    source = (ed.project / frag).read_text()
    broken = source.replace("void main()", "void main()\n{ undefined_symbol_w4 = 1.0; }\nvoid unused()", 1)
    ed.check(broken != source, "fragment has a main()")
    w = await ed.call("write_file", path=frag, content=broken, timeout=300)
    checks = w.get("shader_check", [])
    ed.check(checks and not checks[0]["ok"], f"broken shader reported: {w!r}")
    ed.check("undefined_symbol_w4" in checks[0].get("log", ""), "compiler log names the symbol")
    await ed.call("undo", scope="project")
    ok = await ed.call("check_shader", shader=r["custom_shader"], timeout=300)
    ed.check_eq(ok["failed"], 0, "undo restores a compiling shader")

    uni = await ed.call("get_shader_uniforms", entity=eid)
    ed.check_eq(uni["custom_shader"], r["custom_shader"])
    ed.check("members" in uni and "values" in uni, f"uniform listing: {uni!r}")
    set_ = await ed.call("set_shader_uniform", entity=eid, name="amount", value=[0.5, 0.25])
    ed.check_eq(set_["name"], "amount")
    uni = await ed.call("get_shader_uniforms", entity=eid)
    ed.check_eq(uni["values"].get("amount"), [0.5, 0.25, 0, 0])
    await ed.call("undo")
    uni = await ed.call("get_shader_uniforms", entity=eid)
    ed.check("amount" not in uni["values"], "undo removes the uniform value")
    await ed.check_error("set_shader_uniform", entity=eid, name="time", value=1, contains="engine")
    await ed.check_error("fork_shader", entity=eid, contains="already uses")

    tex = await make_entity(ed, "Pixel", "box")
    prop = "submeshes[0].material.baseColorTexture"
    await ed.call("set_properties", entity=tex, values={"mesh": {prop: "assets/sprites/rock.png"}})
    ts = await ed.call("set_texture_settings", entity=tex, component="mesh", property=prop,
                       mag_filter="nearest", wrap_u="clamp_to_edge")
    ed.check_eq(ts["property"], prop)
    await ed.check_error("set_texture_settings", entity=tex, component="mesh", property="nope", contains="texture")
    await ed.check_error("set_texture_settings", entity=tex, component="mesh", property=prop,
                         mag_filter="linear_mipmap_linear", contains="nearest or linear")

    pp = await ed.call("fork_shader", target="post_process", name="W4Post")
    ed.check_eq(pp["new_pass"], True, "post-process pass added")
    await ed.call("fork_shader", target="scene_default", expect_error=True)  # needs shader_type


async def test_skybox_cube_faces(ed: Editor):
    """A six-face cube map reads back in the order it is written (front, back, left, right, up, down)."""
    faces = ["front", "back", "left", "right", "up", "down"]
    folder = ed.project / "assets/cubefaces"
    folder.mkdir(parents=True, exist_ok=True)
    rock = (ed.project / "assets/sprites/rock.png").read_bytes()
    for face in faces:
        (folder / f"{face}.png").write_bytes(rock)
    paths = [f"assets/cubefaces/{face}.png" for face in faces]

    sky = await make_entity(ed, "FacesSky", "skybox")
    await ed.call("set_properties", entity=sky, values={"skybox": {"texture": {"cube": paths}}})
    got = await ed.call("get_entity", entity=sky, components=["skybox"])
    texture = got["components"]["skybox"]["texture"]
    ed.check(isinstance(texture, dict), f"a cube map value: {texture!r}")
    ed.check_eq(texture.get("cube"), paths, "faces read back in the written order")

    # a write of what was read keeps the faces
    await ed.call("set_properties", entity=sky, values={"skybox": {"texture": texture}})
    again = await ed.call("get_entity", entity=sky, components=["skybox"])
    ed.check_eq(again["components"]["skybox"]["texture"].get("cube"), paths, "the read value round-trips")
    await ed.check_alive()


async def test_external_image_reload(ed: Editor):
    """An image replaced on disk is reloaded by the GameObjects using it after sync_files."""
    ed.require_tools("sync_files")
    await ed.call("sync_files")  # baseline
    rock = ed.project / "assets/sprites/rock.png"
    rock.write_bytes((ed.project / "assets/sprites/rock-down.png").read_bytes())
    r = await ed.call("sync_files")
    ed.check_in("assets/sprites/rock.png", r["changed"]["modified"], "image change detected")
    ed.check(r.get("textures_reloaded", 0) >= 1, f"no texture reloaded: {r!r}")
    await ed.check_alive()
    if ed.has_tool("screenshot"):
        await ed.screenshot(view="scene")
