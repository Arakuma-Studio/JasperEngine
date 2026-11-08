"""S42 AC12 (docs/specs/S42-pixel-art-mode.md R1, R25): Pixel Art Mode. With the mode off (the red control)
an imported PNG gets no pixelArt, a new 2D scene has no camera of its own and nothing is logged. With
it on, an imported PNG gets pixelArt: true written with its type, a new 2D scene has a Main Camera with
a Pixel Perfect Camera, the import's check logs one Console warning naming a finding, and Play start
logs one summary line.
"""

from __future__ import annotations

from harness import Editor
import pngdiff

CI = True
TIMEOUT = 900
EDITOR = {"create": True, "name": "Pixel Art Mode"}


def png(color: tuple[int, int, int]) -> bytes:
    return pngdiff.encode(8, 8, bytes((*color, 255)) * 64)


async def meta_text(ed: Editor, path: str) -> str:
    for _ in range(200):
        meta = ed.project / f"{path}.meta"
        if meta.exists() and "textureImporter" in meta.read_text():
            return meta.read_text()
        await ed.call("wait", frames=10)
    return (ed.project / f"{path}.meta").read_text() if (ed.project / f"{path}.meta").exists() else ""


async def summaries(ed: Editor, since: int) -> list[str]:
    got = await ed.call("read_console", since=since, limit=500)
    return [e["message"] for e in got.get("entries", []) if e["message"].startswith("Pixel Art Check:")]


async def cursor(ed: Editor) -> int:
    return (await ed.call("read_console", tail=True, limit=1)).get("next_cursor", 0)


async def import_png(ed: Editor, path: str) -> None:
    (ed.project / path).parent.mkdir(parents=True, exist_ok=True)
    (ed.project / path).write_bytes(png((200, 40, 40)))
    await ed.call("sync_files", paths=[path], full=False)


async def test_mode_off_does_nothing(ed: Editor):
    await ed.call("get_project_summary")
    since = await cursor(ed)
    await import_png(ed, "sprites/off.png")
    text = await meta_text(ed, "sprites/off.png")
    ed.check("pixelArt" not in text, f"the mode off writes no pixelArt: {text}")
    made = await ed.call("create_scene", name="Off", type="2D")
    tree = await ed.call("get_scene_tree", scene=made["scene"]["id"], format="json")
    ed.check("Main Camera" not in str(tree), "no camera added with the mode off")
    await ed.call("wait", frames=120)
    ed.check_eq(await summaries(ed, since), [], "nothing logged with the mode off")


async def test_mode_on(ed: Editor):
    # the mode alone: the project keeps its letterbox canvas, so pixel_art_mode warns
    await ed.call("set_project_settings", values={"pixel_art.enabled": True})
    since = await cursor(ed)
    await import_png(ed, "sprites/on.png")
    text = await meta_text(ed, "sprites/on.png")
    ed.check("pixelArt: true" in text and "type: sprite" in text, f"the first import writes pixelArt: true: {text}")
    made = await ed.call("create_scene", name="On", type="2D", path="scenes/On")
    scene = made["scene"]["id"]
    camera = await ed.call("get_entity", scene=scene, entity="Main Camera")
    ed.check("pixelperfectcamera" in camera["components"], f"a new 2D scene has a Pixel Perfect Camera: {list(camera['components'])}")
    await ed.call("save_scene", scene=scene)
    lines = []
    for _ in range(100):
        lines = await summaries(ed, since)
        if lines:
            break
        await ed.call("wait", frames=10)
    ed.check_eq(len(lines), 1, f"one Console line for the import batch: {lines}")
    ed.check(lines and ("FAIL" in lines[0] or "WARN" in lines[0]) and " - " in lines[0], f"it names a finding: {lines}")

    since = await cursor(ed)
    await ed.call("play", action="start", scene=scene)
    try:
        lines = []
        for _ in range(100):
            lines = await summaries(ed, since)
            if lines:
                break
            await ed.call("wait", frames=10)
        ed.check_eq(len(lines), 1, f"Play start logs one summary line: {lines}")
    finally:
        await ed.call("play", action="stop")
    await ed.call("set_project_settings", values={"pixel_art.enabled": False})
