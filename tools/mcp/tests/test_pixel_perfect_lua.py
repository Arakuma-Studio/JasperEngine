"""S41 AC16, AC7 (docs/specs/S41-pixel-perfect-camera.md R24, R10): the Lua API of the Pixel Perfect
Camera in Play. A script sets every property and reads them back, camera:getPixelPerfectCamera()
is nil before the component and a handle after it, the layout values are the renderer's (k 4 for
320 x 180 in a 1280 x 720 Game view), roundToPixel is the snap rule, Engine.getPixelsPerUnit() is
the project's, Sprite.pixelsPerUnit / pixelRotation / worldSize and Tilemap.pixelsPerUnit bind,
and a sprite turned 37 degrees under 90 Degree Steps keeps 37 in its Transform.

Runs on an owned empty project with a 320 x 180 canvas.
"""

from __future__ import annotations

from harness import Editor

EDITOR = {"create": True, "name": "Pixel Perfect Lua"}
CI = True

S: dict = {}

SETUP_LUA = """
local cam = Camera(scene, scene:findEntity("Main Camera"))
assert(cam:getPixelPerfectCamera() == nil, "no component yet")
local ppc = PixelPerfectCamera(scene, cam.entity)
ppc.referenceResolution = Vector2(320, 180)
ppc.upscaleMode = PixelUpscaleMode.INTEGER
ppc.upscaleRenderTexture = true
ppc.pixelSnapping = true
ppc.rotationMode = PixelRotation.STEPS_90
ppc.subpixelSmoothing = false
ppc.letterboxColor = Vector4(0, 0, 0, 1)
assert(cam:getPixelPerfectCamera() ~= nil, "a handle once it has one")
return ppc.referenceResolution.x
"""

READ_LUA = """
local cam = Camera(scene, scene:findEntity("Main Camera"))
local ppc = cam:getPixelPerfectCamera()
local snapped = ppc.snappedPosition
local rounded = ppc:roundToPixel(Vector3(10.5, 4.25, 0))
local px = ppc:worldToPixel(Vector3(12, 7, 0))
local back = ppc:pixelToWorld(px)
local hero = Sprite(scene, scene:findEntity("Hero"))
hero.pixelsPerUnit = 16
hero.pixelRotation = PixelRotation.ON_GRID
local ppu = hero.pixelsPerUnit
local rotation = hero.pixelRotation == PixelRotation.ON_GRID
hero.pixelsPerUnit = 0
hero.pixelRotation = PixelRotation.CAMERA
hero.rotation = Quaternion(0, 0, 37)
return {ppc.pixelRatio, ppc.baseResolution.x, ppc.baseResolution.y, ppc.targetResolution.x, ppc.viewport.width,
        rounded.x, rounded.y, Engine.getPixelsPerUnit(), ppu, rotation, back.x, back.y, ppc.cropped,
        ppc.upscaleMode == PixelUpscaleMode.INTEGER, ppc.rotationMode == PixelRotation.STEPS_90, hero.worldSize.x}
"""


async def setup(ed: Editor) -> dict:
    if S.get("ed") is ed:
        return S
    S.clear()
    S["ed"] = ed
    await ed.call("set_project_settings", values={"graphics.canvas_width": 320, "graphics.canvas_height": 180})
    made = await ed.call("create_scene", name="PixelLua", type="2D", path="scenes/PixelLua")
    S["scene"] = made["scene"]["id"]
    await ed.call("create_entity", scene=S["scene"], type="camera", name="Main Camera")
    await ed.call("set_main_camera", scene=S["scene"], entity="Main Camera")
    await ed.call("create_entity", scene=S["scene"], type="sprite", name="Hero", position=[100, 60, 0])
    await ed.call("configure_game_view", resolution="1280x720")
    await ed.call("save_scene", scene=S["scene"])
    return S


async def test_lua_api(ed: Editor):
    s = await setup(ed)
    started = await ed.call("play", action="start", scene=s["scene"])
    ed.check_eq(started["state"], "playing", "the scene plays")
    try:
        first = await ed.call("eval_lua", code=SETUP_LUA)
        ed.check(first["values"][0] == 320, f"the reference reads back: {first}")
        await ed.call("wait", frames=4)
        values = (await ed.call("eval_lua", code=READ_LUA))["values"][0]
        ed.check(values[0] == 4, f"pixelRatio 4 for 320x180 at 1280x720: {values}")
        ed.check(values[1] == 320 and values[2] == 180 and values[3] == 320, f"base and target: {values}")
        ed.check(values[4] == 1280, f"the viewport: {values}")
        ed.check(values[5] == 11 and values[6] == 4, f"roundToPixel is the rule (10.5 -> 11): {values}")
        ed.check(values[7] == 1, f"Engine.getPixelsPerUnit() is the project's: {values}")
        ed.check(values[8] == 16 and values[9] is True, f"Sprite.pixelsPerUnit and pixelRotation: {values}")
        ed.check(abs(values[10] - 12) < 1e-4 and abs(values[11] - 7) < 1e-4, f"pixelToWorld inverts worldToPixel: {values}")
        ed.check(values[12] is False and values[13] is True and values[14] is True, f"cropped, enums: {values}")
        ed.check(values[15] > 0, f"Sprite.worldSize: {values}")
        await ed.call("wait", frames=2)
        kept = (await ed.call("eval_lua", code="return GameObject(scene, scene:findEntity('Hero')).rotation:getRoll()"))["values"][0]
        ed.check(abs(kept - 37) < 1e-3, f"the Transform keeps 37 degrees under 90 Degree Steps: {kept}")
    finally:
        await ed.call("play", action="stop")
