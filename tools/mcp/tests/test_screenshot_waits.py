"""A screenshot never shows a stand-in for a texture still loading.

A mesh whose texture is still loading draws without it (the texture before, or a stand-in). The editor's
screenshot (and the player's waited capture, and Engine::areScenesLoaded, the ready line) wait
until no texture is loading; past the call's timeout_seconds the screenshot fails and names the
loads (it used to shoot whatever was there after 6 s). JASPER_TEXTURE_LOAD_DELAY_MS holds every
async texture decode of this editor that long.
"""

from __future__ import annotations

import time

import pngdiff
from harness import Editor

CI = True
DELAY_MS = 10000
EDITOR = {"create": True, "name": "Screenshot Waits", "env": {"JASPER_TEXTURE_LOAD_DELAY_MS": str(DELAY_MS)}}
TIMEOUT = 600

DISPLAY = {"show_grid_3d": False, "hide_light_icons": True, "hide_selection_outline": True,
           "hide_camera_view": True, "hide_sound_icons": True}
S: dict = {}


def write_image(ed: Editor, name: str, rgb: tuple) -> str:
    path = f"assets/textures/{name}.png"
    (ed.project / "assets" / "textures").mkdir(parents=True, exist_ok=True)
    (ed.project / path).write_bytes(pngdiff.encode(32, 32, bytes([*rgb, 255]) * 32 * 32))
    return path


def share_near(img, rgb: tuple, tol: int = 60) -> float:
    """The share of the image's centre quarter in the hue of `rgb` (its strongest channel well
    above the others, whatever the lighting)"""
    width, height, _ = img
    w, h, rgba = pngdiff.crop(img, width * 3 // 8, height * 3 // 8, width // 4, height // 4)
    strong = max(range(3), key=lambda c: rgb[c])
    hits = sum(1 for i in range(0, len(rgba), 4)
               if all(rgba[i + strong] >= rgba[i + c] + tol for c in range(3) if c != strong))
    return hits / max(1, w * h)


async def shoot(ed: Editor, view: str, timeout_seconds: float = 120):
    r = await ed.call_raw("screenshot", {"view": view, "scene": S["scene"], "max_width": 256, "timeout_seconds": timeout_seconds},
                          timeout=300)
    ed.check(not r.is_error and r.images, f"the {view} screenshot: {r.text[:600]}")
    return pngdiff.decode(r.images[0][0]), r.images[0][0]


IMAGES = {"warm-red": (230, 30, 30), "slow-green": (30, 200, 30), "slow-blue": (30, 30, 230), "slow-red": (230, 30, 30)}


async def set_texture(ed: Editor, name: str) -> None:
    """The box's base colour texture, from Lua as a game sets it: an image cooked in the setup and
    never loaded before, so its load (in the background) is held DELAY_MS"""
    await ed.call("eval_lua", code=f"object('Box', 'Mesh'):setTexture('assets/textures/{name}.png') return true",
                  allow_edit_mode=True)


async def test_setup(ed: Editor):
    """A box both views look at, drawn once with a texture (its shaders made): from then on a new
    texture is the only thing a screenshot can wait for"""
    S["scene"] = (await ed.call("create_scene", name="SlowTextures", type="3D", path="scenes/SlowTextures"))["scene"]["id"]
    S["box"] = (await ed.call("create_entity", scene=S["scene"], type="box", name="Box"))["entity_id"]
    camera = (await ed.call("create_entity", scene=S["scene"], type="camera", name="Camera"))["entity_id"]
    await ed.call("set_main_camera", scene=S["scene"], entity=camera)
    await ed.call("set_transform", scene=S["scene"], entity=camera, position=[0.4, 0.5, 1.6])
    await ed.call("set_properties", scene=S["scene"], entity=camera, values={"camera": {"useTarget": True, "target": [0, 0, 0]}})
    await ed.call("select_entities", scene=S["scene"], entities=[])
    await ed.call("set_scene_camera", scene=S["scene"], position=[0.4, 0.5, 1.6], target=[0, 0, 0], display=DISPLAY)
    # every image written and cooked now: no file event, hot reload or cook swap-in later reads
    # one of them besides the load under test
    paths = [write_image(ed, name, rgb) for name, rgb in IMAGES.items()]
    await ed.call("sync_files")
    cooked = await ed.call("reimport_textures", paths=paths, platforms=["desktop"], wait=True, timeout_s=600)
    ed.check(not cooked["timed_out"], f"the images cooked: {cooked}")
    await set_texture(ed, "warm-red")
    # (polled: this part must pass on an editor whose screenshots do not wait for textures too)
    for view in ("scene", "game"):
        deadline = time.monotonic() + 120
        share = 0.0
        while share <= 0.5 and time.monotonic() < deadline:
            img, _ = await shoot(ed, view)
            share = share_near(img, (230, 30, 30))
            if share <= 0.5:
                await ed.call("wait", frames=10)
        ed.check(share > 0.5, f"the {view} view shows the box's first texture ({share:.0%} red)")


async def test_scene_screenshot_waits_for_a_slow_texture(ed: Editor):
    await set_texture(ed, "slow-green")
    img, png = await shoot(ed, "scene")
    (ed.scratch.root / "slow-green.png").write_bytes(png)
    share = share_near(img, (30, 200, 30))
    # held 10 s, longer than the 6 s a screenshot used to wait before it shot what was there (the
    # texture before, or a stand-in)
    ed.check(share > 0.5, f"the Scene view shows the new green texture ({share:.0%} of the centre green)")


async def test_game_view_screenshot_waits_for_a_slow_texture(ed: Editor):
    await set_texture(ed, "slow-blue")
    img, png = await shoot(ed, "game")
    (ed.scratch.root / "slow-blue.png").write_bytes(png)
    share = share_near(img, (30, 30, 230))
    ed.check(share > 0.5, f"the Game view shows the new blue texture ({share:.0%} of the centre blue)")


async def test_timeout_names_the_pending_texture(ed: Editor):
    await set_texture(ed, "slow-red")
    text = await ed.call("screenshot", view="scene", scene=S["scene"], max_width=256, timeout_seconds=1, expect_error=True)
    ed.check("still loading" in text and "texture load" in text and "slow-red.png" in text,
             f"the error names the texture load: {text}")
    # it then loads, and a screenshot shows it
    img, _ = await shoot(ed, "scene")
    share = share_near(img, (230, 30, 30))
    ed.check(share > 0.5, f"the Scene view shows the red texture after all ({share:.0%} of the centre red)")
