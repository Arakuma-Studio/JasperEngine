"""Asset transport, editing and undo with small, authored project files."""

from harness import Editor
import pngdiff

EDITOR = {"create": True, "name": "Asset Contracts"}
CI = True


def image_file(ed: Editor, relative: str, width: int = 32, height: int = 16):
    path = ed.project / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(pngdiff.encode(width, height, bytes([255, 128, 0, 255]) * width * height))
    return path


async def test_list_and_info(ed: Editor):
    image = "assets/listing/nested/swatch.png"
    image_file(ed, image)
    (ed.project / "assets/listing/readme.txt").write_text("not an image", encoding="utf-8")
    listed = await ed.call("list_assets", directory="assets/listing", type="texture", details=True)
    ed.check_eq([entry["path"] for entry in listed["entries"]], [image], "type filter excludes the text file")
    ed.check_eq((listed["entries"][0]["width"], listed["entries"][0]["height"]), (32, 16))
    matched = await ed.call("list_assets", directory="assets/listing", pattern="assets/**/swatch*.png")
    ed.check_eq([entry["path"] for entry in matched["entries"]], [image], "glob matches project-relative paths")
    shallow = await ed.call("list_assets", directory="assets/listing", recursive=False)
    ed.check(image not in {entry["path"] for entry in shallow["entries"]}, "nonrecursive listing excludes nested files")
    info = await ed.call("get_asset_info", path=image)
    ed.check_eq((info["width"], info["height"]), (32, 16))
    preview = await ed.call_raw("view_asset", {"path": image, "max_size": 16})
    ed.check(not preview.is_error and preview.images, f"PNG preview: {preview.text}")
    ed.check_eq(preview.images[0][1], "image/png")
    width, height, pixels = pngdiff.decode(preview.images[0][0])
    ed.check_eq((width, height), (16, 8), "the actual image respects max_size and aspect ratio")
    ed.check_eq(tuple(pixels[:4]), (255, 128, 0, 255), "preview preserves the supplied image colour")


async def test_file_ops_with_undo(ed: Editor):
    source = ed.project / "assets/original.txt"
    destination = ed.project / "assets/moved/renamed.txt"
    source.parent.mkdir(parents=True, exist_ok=True)
    source.write_bytes(b"asset payload\n")
    destination.parent.mkdir(parents=True, exist_ok=True)
    await ed.call("move_asset", path="assets/original.txt", to="assets/moved/renamed.txt")
    ed.check(not source.exists() and destination.read_bytes() == b"asset payload\n", "move and rename preserve bytes")
    await ed.call("undo", scope="project")
    ed.check(source.read_bytes() == b"asset payload\n" and not destination.exists(), "one project undo restores the move and rename")
    await ed.call("delete_asset", paths=["assets/original.txt"])
    ed.check(not source.exists(), "deletion removes the asset")
    await ed.call("undo", scope="project")
    ed.check_eq(source.read_bytes(), b"asset payload\n", "undo restores the trashed asset")
    before = (ed.project / "project.yaml").read_bytes()
    refused = await ed.call_raw("delete_asset", {"paths": ["project.yaml"]})
    ed.check(refused.is_error, "deletion of protected project data is refused")
    ed.check_eq((ed.project / "project.yaml").read_bytes(), before, "protected project data is untouched")


async def test_import_and_placeholder(ed: Editor):
    path = "assets/placeholder.png"
    result = await ed.call_raw("generate_placeholder_art", {"path": path, "width": 32, "height": 16,
                                                           "shape": "circle", "color": "#ff8000"})
    ed.check(not result.is_error, result.text)
    width, height, pixels = pngdiff.decode((ed.project / path).read_bytes())
    ed.check_eq((width, height), (32, 16), "saved PNG uses the requested dimensions")
    centre = (8 * width + 16) * 4
    ed.check_eq(tuple(pixels[centre:centre + 4]), (255, 128, 0, 255), "circle centre has the requested colour")
    ed.check_eq(pixels[3], 0, "a circle leaves its distant corner transparent")


async def test_text_files(ed: Editor):
    path = "notes/probe.txt"
    original = "one\ncafé\nthree\n"
    await ed.call("write_file", path=path, content=original)
    read = await ed.call_raw("read_file", {"path": path, "start_line": 2, "max_lines": 1})
    ed.check(not read.is_error, read.text)
    ed.check_eq(read.structured["lines"], 1)
    ed.check_eq(read.text.partition("\n")[2], "café\n", "one-based line range preserves UTF-8")
    await ed.call("write_file", path=path, edits=[{"old_text": "café", "new_text": "tea"}])
    ed.check_eq((ed.project / path).read_text(encoding="utf-8"), "one\ntea\nthree\n")
    refused = await ed.call_raw("write_file", {"path": path, "edits": [{"old_text": "missing", "new_text": "x"}]})
    ed.check(refused.is_error, "a replacement must match existing file contents")
    ed.check_eq((ed.project / path).read_text(encoding="utf-8"), "one\ntea\nthree\n", "failed edit leaves bytes intact")
    await ed.call("undo", scope="project")
    ed.check_eq((ed.project / path).read_text(encoding="utf-8"), original, "failed edit adds no undo entry")


async def test_texture_settings(ed: Editor):
    """set_texture_settings changes the sampling of a texture a property holds and names what it
    refuses (an unknown property, a mipmap filter for magnification)."""
    image_file(ed, "assets/textures/pixel.png", 8, 8)
    scene = (await ed.call("create_scene", name="TextureSettings", type="3D"))["scene"]["id"]
    tex = (await ed.call("create_entity", scene=scene, type="box", name="Pixel"))["entity_id"]
    prop = "submeshes[0].material.baseColorTexture"
    await ed.call("set_properties", scene=scene, entity=tex, values={"mesh": {prop: "assets/textures/pixel.png"}})
    ts = await ed.call("set_texture_settings", scene=scene, entity=tex, component="mesh", property=prop,
                       mag_filter="nearest", wrap_u="clamp_to_edge")
    ed.check_eq(ts["property"], prop)
    await ed.check_error("set_texture_settings", scene=scene, entity=tex, component="mesh", property="nope", contains="texture")
    await ed.check_error("set_texture_settings", scene=scene, entity=tex, component="mesh", property=prop,
                         mag_filter="linear_mipmap_linear", contains="nearest or linear")


async def test_skybox_cube_faces(ed: Editor):
    """A six-face cube map reads back in the order it is written (front, back, left, right, up,
    down), and a write of what was read keeps it. The faces are square images of one size (a cube
    map's faces must be: the engine refuses the load otherwise, and the Skybox's texture then reads
    null), read back once the sky has loaded."""
    faces = ["front", "back", "left", "right", "up", "down"]
    paths = [f"assets/cubefaces/{face}.png" for face in faces]
    for path in paths:
        image_file(ed, path, 8, 8)
    scene = (await ed.call("create_scene", name="CubeFaces", type="3D"))["scene"]["id"]
    sky = (await ed.call("create_entity", scene=scene, type="skybox", name="FacesSky"))["entity_id"]
    await ed.call("set_properties", scene=scene, entity=sky, values={"skybox": {"texture": {"cube": paths}}})
    await ed.call("wait", frames=10)
    texture = (await ed.call("get_entity", scene=scene, entity=sky, components=["skybox"]))["components"]["skybox"]["texture"]
    ed.check(isinstance(texture, dict), f"a cube map value: {texture!r}")
    ed.check_eq(texture.get("cube"), paths, "faces read back in the written order")

    await ed.call("set_properties", scene=scene, entity=sky, values={"skybox": {"texture": texture}})
    await ed.call("wait", frames=10)
    again = (await ed.call("get_entity", scene=scene, entity=sky, components=["skybox"]))["components"]["skybox"]["texture"]
    ed.check(isinstance(again, dict), f"the re-written cube map still loads: {again!r}")
    ed.check_eq(again.get("cube"), paths, "the read value round-trips")


async def test_move_asset_copy_without_to_duplicates_beside(ed: Editor):
    image_file(ed, "assets/dup/hero.png")
    await ed.call("sync_files", paths=["assets/dup/hero.png"])
    first = await ed.call("move_asset", path="assets/dup/hero.png", copy=True)
    ed.check_eq(first["to"], "assets/dup/hero 1.png", "Unity's Duplicate name")
    second = await ed.call("move_asset", path="assets/dup/hero.png", copy=True)
    ed.check_eq(second["to"], "assets/dup/hero 2.png")
    ed.check((ed.project / "assets/dup/hero 1.png").exists() and (ed.project / "assets/dup/hero 2.png").exists(), "both copies")
    ed.check(first.get("guid") and first["guid"] != second.get("guid"), "each copy its own GUID")
    await ed.check_error("move_asset", path="assets/dup/hero.png", contains="to is required")


async def test_copy_into_itself_says_copy(ed: Editor):
    image_file(ed, "assets/self/inner/a.png")
    await ed.call("sync_files", paths=["assets/self/inner/a.png"])
    msg = await ed.check_error("move_asset", path="assets/self", to="assets/self/inner/self", copy=True)
    ed.check_in("Can't copy 'self' into itself", msg)
    msg = await ed.check_error("move_asset", path="assets/self", to="assets/self/inner/self")
    ed.check_in("Can't move 'self' into itself", msg)
