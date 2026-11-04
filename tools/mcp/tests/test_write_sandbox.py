"""The MCP write sandbox resolves symbolic links (docs/mcp/DESIGN.md, Addressing): a tool that
writes, creates, deletes or moves a file refuses a path whose link-resolved location is outside the
project (isError naming the path, where the link leads and the fix), and writes beneath the project
when it happens (editor/util/SafeWrite.h); links inside the project work; reads keep following links
(an assets folder linked in from a shared library); the project opened through a link works. A
refused undo (a generated file's folder became a link out of the project) keeps the history where
it is and says so."""

import os
import shutil
import tempfile
from pathlib import Path

from harness import Editor, skip, symlinks_permitted
import pngdiff

EDITOR = {"create": True, "name": "Write Sandbox"}
CI = True

OUT: dict = {}


def outside_dir() -> Path:
    if "dir" not in OUT:
        OUT["dir"] = Path(tempfile.mkdtemp(prefix="jasper-sandbox-outside-"))
    return OUT["dir"]


def require_links() -> None:
    if not symlinks_permitted():
        skip("this process may not create symbolic links")


def png(width: int = 32, height: int = 32) -> bytes:
    # an opaque disc on transparency (something for the normal generator to shade)
    pixels = bytearray()
    for y in range(height):
        for x in range(width):
            inside = (x - width / 2 + 0.5) ** 2 + (y - height / 2 + 0.5) ** 2 <= (width / 2 - 3) ** 2
            pixels += bytes([200, 120, 60, 255]) if inside else bytes([0, 0, 0, 0])
    return pngdiff.encode(width, height, bytes(pixels))


def outside_listing() -> list[str]:
    return sorted(str(p.relative_to(outside_dir())) for p in outside_dir().rglob("*"))


async def refused(ed: Editor, tool: str, args: dict, what: str) -> str:
    r = await ed.call_raw(tool, args)
    ed.check(r.is_error, f"{what}: {tool} {args} must be refused: {r.text[:400]}")
    ed.check("resolves outside the project" in r.text, f"{what}: the error says it resolves outside the project: {r.text[:400]}")
    ed.check("link" in r.text, f"{what}: the error names the link and the fix: {r.text[:400]}")
    return r.text


async def test_writes_through_a_link_out_of_the_project_are_refused(ed: Editor):
    require_links()
    outside = outside_dir()
    (outside / "theirs.txt").write_text("theirs\n", encoding="utf-8")
    assets = ed.project / "assets"
    assets.mkdir(parents=True, exist_ok=True)
    os.symlink(outside, assets / "out")
    os.symlink(outside / "theirs.txt", assets / "file_out.txt")
    os.symlink(outside / "nothing.txt", assets / "dangling_out.txt")
    (assets / "mine.txt").write_text("mine\n", encoding="utf-8")
    before = outside_listing()
    text = await refused(ed, "write_file", {"path": "assets/out/x.lua", "content": "print(1)\n"}, "a folder link out")
    ed.check(str(outside.resolve()) in text or outside.resolve().as_posix() in text, f"the error says where the link leads: {text[:400]}")
    await refused(ed, "write_file", {"path": "assets/file_out.txt", "content": "ours\n"}, "a file link out")
    await refused(ed, "write_file", {"path": "assets/dangling_out.txt", "content": "ours\n"}, "a dangling link out")
    await refused(ed, "create_folder", {"path": "assets/out/made"}, "create_folder")
    await refused(ed, "move_asset", {"path": "assets/mine.txt", "to": "assets/out/mine.txt"}, "move_asset to")
    await refused(ed, "move_asset", {"path": "assets/out/theirs.txt", "to": "assets/theirs.txt"}, "move_asset from")
    await refused(ed, "delete_asset", {"paths": ["assets/out/theirs.txt"]}, "delete_asset")
    await refused(ed, "generate_placeholder_art", {"path": "assets/out/p.png", "width": 8, "height": 8}, "generate_placeholder_art")
    await refused(ed, "create_material", {"path": "assets/out/m.material"}, "create_material")
    ed.check_eq(outside_listing(), before, "nothing written, moved or deleted outside")
    ed.check_eq((outside / "theirs.txt").read_text(encoding="utf-8"), "theirs\n", "their file is untouched")
    ed.check_eq((assets / "mine.txt").read_text(encoding="utf-8"), "mine\n", "the source of the refused move stays")
    # deleting the link itself is fine (the entry, not its target, goes to the trash), and undoable
    await ed.call("delete_asset", paths=["assets/file_out.txt"])
    ed.check(not os.path.lexists(assets / "file_out.txt"), "the link went to the trash")
    ed.check_eq((outside / "theirs.txt").read_text(encoding="utf-8"), "theirs\n", "its target is untouched")
    await ed.call("undo", scope="project")
    ed.check(os.path.islink(assets / "file_out.txt"), "undo brings the link back")


async def test_links_inside_the_project_work(ed: Editor):
    require_links()
    assets = ed.project / "assets"
    (assets / "real").mkdir(parents=True, exist_ok=True)
    os.symlink(assets / "real", assets / "in")
    await ed.call("write_file", path="assets/in/a.txt", content="through\n")
    ed.check_eq((assets / "real" / "a.txt").read_text(encoding="utf-8"), "through\n", "written at the link's real place")
    await ed.call("create_folder", path="assets/in/sub")
    ed.check((assets / "real" / "sub").is_dir(), "the folder made at the real place")
    (assets / "move_me.txt").write_text("moved\n", encoding="utf-8")
    await ed.call("move_asset", path="assets/move_me.txt", to="assets/in/sub/moved.txt")
    ed.check_eq((assets / "real" / "sub" / "moved.txt").read_text(encoding="utf-8"), "moved\n", "moved through the inside link")
    await ed.call("undo", scope="project")
    ed.check((assets / "move_me.txt").is_file(), "undo moves it back")
    # a file link inside: written through (the link stays a link)
    (assets / "real" / "target.txt").write_text("old\n", encoding="utf-8")
    os.symlink(assets / "real" / "target.txt", assets / "alias.txt")
    await ed.call("write_file", path="assets/alias.txt", content="new\n")
    ed.check(os.path.islink(assets / "alias.txt"), "the link stays")
    ed.check_eq((assets / "real" / "target.txt").read_text(encoding="utf-8"), "new\n", "its target got the bytes")
    await ed.call("undo", scope="project")
    ed.check_eq((assets / "real" / "target.txt").read_text(encoding="utf-8"), "old\n", "undo puts the old bytes back through it")
    await ed.call("delete_asset", paths=["assets/in/a.txt"])
    ed.check(not (assets / "real" / "a.txt").exists(), "deleted through the inside link")


async def test_reads_still_follow_links_out_of_the_project(ed: Editor):
    require_links()
    library = Path(tempfile.mkdtemp(prefix="jasper-sandbox-library-"))
    OUT["library"] = library
    (library / "lib.txt").write_text("shared\n", encoding="utf-8")
    (library / "tile.png").write_bytes(png(16, 16))
    os.symlink(library, ed.project / "assets" / "shared")
    read = await ed.call_raw("read_file", {"path": "assets/shared/lib.txt"})
    ed.check(not read.is_error, f"a read through a link out of the project works: {read.text[:300]}")
    ed.check_in("shared", read.text)
    listed = await ed.call("list_assets", directory="assets/shared")
    ed.check_in("assets/shared/tile.png", [e["path"] for e in listed["entries"]], "listing through the link")
    info = await ed.call("get_asset_info", path="assets/shared/tile.png")
    ed.check_eq((info["width"], info["height"]), (16, 16), "asset info through the link")
    # ... but writing into the linked library is refused
    await refused(ed, "write_file", {"path": "assets/shared/lib.txt", "content": "ours\n"}, "a write into the linked library")
    ed.check_eq((library / "lib.txt").read_text(encoding="utf-8"), "shared\n", "the library is untouched")


async def test_a_refused_undo_keeps_the_history_and_says_why(ed: Editor):
    require_links()
    ed.require_tools("generate_sprite_normals")
    outside = outside_dir()
    sprites = ed.project / "assets" / "sprites"
    (sprites / "gen").mkdir(parents=True, exist_ok=True)
    (sprites / "disc.png").write_bytes(png())
    await ed.call("sync_files")
    r = await ed.call_raw("generate_sprite_normals", {"source": "assets/sprites/disc.png", "output": "assets/sprites/gen/disc_normal.png"},
                          timeout=600)
    ed.check(not r.is_error, f"generated: {r.text[:400]}")
    made = sprites / "gen" / "disc_normal.png"
    ed.check(made.is_file(), "the normal map is written")
    # the folder becomes a link out of the project, a file of theirs behind it
    shutil.rmtree(sprites / "gen")
    os.symlink(outside, sprites / "gen")
    (outside / "disc_normal.png").write_bytes(b"theirs")
    undo = await ed.call_raw("undo", {"scope": "project"})
    ed.check(undo.is_error, f"the undo is refused: {undo.text[:400]}")
    ed.check_in("Remove the link, then undo again", undo.text, "the fix")
    ed.check_in("disc_normal.png", undo.text, "the file")
    ed.check_eq(undo.structured.get("steps"), 0, "nothing undone")
    ed.check(undo.structured.get("refused") is True and "refusal" in undo.structured, f"refused: {undo.structured}")
    ed.check_eq((outside / "disc_normal.png").read_bytes(), b"theirs", "nothing removed through the link")
    again = await ed.call_raw("undo", {"scope": "project"})
    ed.check(again.is_error, "still refused (the history did not move past it)")
    # the link gone (a real folder with the file again): the same step undoes
    os.remove(sprites / "gen")
    (sprites / "gen").mkdir()
    (sprites / "gen" / "disc_normal.png").write_bytes(b"generated")
    done = await ed.call_raw("undo", {"scope": "project"})
    ed.check(not done.is_error and done.structured.get("steps") == 1, f"undone once the link is gone: {done.text[:300]}")
    ed.check(not (sprites / "gen" / "disc_normal.png").exists(), "the undo removed the generated file")


async def test_a_copy_out_of_a_linked_library_is_a_read(ed: Editor):
    require_links()
    library = OUT.get("library")
    ed.check(library is not None and (library / "lib.txt").is_file(), "the library of the read test")
    await ed.call("move_asset", path="assets/shared/lib.txt", to="assets/copied_lib.txt", copy=True)
    ed.check_eq((ed.project / "assets" / "copied_lib.txt").read_text(encoding="utf-8"), "shared\n", "copied out of the linked library")
    ed.check((library / "lib.txt").is_file(), "the library keeps its file")
    # ... but moving it (an entry inside the linked library) is refused
    await refused(ed, "move_asset", {"path": "assets/shared/lib.txt", "to": "assets/moved_lib.txt"}, "a move out of the linked library")
    await ed.call("undo", scope="project")
    ed.check(not (ed.project / "assets" / "copied_lib.txt").exists(), "undo removes the copy")


async def test_an_undo_through_a_folder_swapped_for_a_link_is_refused(ed: Editor):
    require_links()
    outside = outside_dir()
    assets = ed.project / "assets"
    (assets / "parent").mkdir(parents=True, exist_ok=True)
    await ed.call("create_folder", path="assets/parent/made")
    (assets / "parent" / "made" / "mine.txt").write_text("mine\n", encoding="utf-8")
    # the parent becomes a link to a folder of theirs holding a "made" of its own
    shutil.rmtree(assets / "parent")
    (outside / "made").mkdir(exist_ok=True)
    (outside / "made" / "theirs.txt").write_text("theirs\n", encoding="utf-8")
    os.symlink(outside, assets / "parent")
    undo = await ed.call_raw("undo", {"scope": "project"})
    ed.check(undo.is_error, f"the undo is refused: {undo.text[:400]}")
    ed.check_in("Remove the link, then undo again", undo.text, "the fix")
    ed.check_eq(undo.structured.get("steps"), 0, "nothing undone")
    ed.check_eq((outside / "made" / "theirs.txt").read_text(encoding="utf-8"), "theirs\n", "nothing of theirs removed")
    # a real folder again: the same step undoes
    os.remove(assets / "parent")
    (assets / "parent" / "made").mkdir(parents=True)
    done = await ed.call_raw("undo", {"scope": "project"})
    ed.check(not done.is_error and done.structured.get("steps") == 1, f"undone once the link is gone: {done.text[:300]}")
    ed.check(not (assets / "parent" / "made").exists(), "the folder it made is gone")
    shutil.rmtree(outside / "made", ignore_errors=True)


async def test_the_project_opened_through_a_link_works(ed: Editor):
    # last: switches the editor to the project's linked spelling
    require_links()
    original = ed.project.resolve()
    via = ed.scratch.root / "via-link"
    if os.path.lexists(via):
        os.remove(via)
    os.symlink(original, via)
    try:
        await ed.call("open_project", path=str(via), unsaved="discard")
        await ed.call("write_file", path="assets/via/written.txt", content="via\n")
        ed.check_eq((original / "assets" / "via" / "written.txt").read_text(encoding="utf-8"), "via\n", "written through the project's link")
        await ed.call("create_folder", path="assets/via/folder")
        ed.check((original / "assets" / "via" / "folder").is_dir(), "a folder made")
        await ed.call("move_asset", path="assets/via/written.txt", to="assets/via/folder/written.txt")
        ed.check((original / "assets" / "via" / "folder" / "written.txt").is_file(), "moved")
        await ed.call("delete_asset", paths=["assets/via/folder/written.txt"])
        ed.check(not (original / "assets" / "via" / "folder" / "written.txt").exists(), "deleted")
        await ed.call("undo", scope="project")
        ed.check((original / "assets" / "via" / "folder" / "written.txt").is_file(), "undo restores it")
        if symlinks_permitted():
            os.symlink(outside_dir(), original / "assets" / "via" / "away")
            await refused(ed, "write_file", {"path": "assets/via/away/x.txt", "content": "x\n"}, "a link out, the project through a link")
    finally:
        for key in ("dir", "library"):
            if key in OUT:
                shutil.rmtree(OUT[key], ignore_errors=True)
