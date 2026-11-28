"""Sprite Flip X and Size, and GameObject:setScale(x, y, z), over MCP and Lua
(docs/specs/S94-rendering-anim-gaps.md F29-F31; AC29 and AC30's MCP and Lua side): set_properties
writes flipX and size in one undo step, the quad's bounds follow Size (world units) and Flip X
mirrors them about the pivot, Lua's Sprite.flipX / Sprite.size drive both in Play, and
go:setScale(1, 2, 3) sets a non-uniform scale (an arity error before)."""

from harness import Editor

EDITOR = {"create": True, "name": "Sprite Size"}
CI = True

S: dict = {}


async def setup(ed: Editor) -> dict:
    if S.get("ed") is ed:
        return S
    S.clear()
    S["ed"] = ed
    made = await ed.call("create_scene", name="Sprites", type="2D", path="scenes/Sprites")
    S["scene"] = made["scene"]["id"]
    S["path"] = "scenes/Sprites.scene"
    await ed.call("create_entity", scene=S["scene"], type="sprite", name="Hero", position=[0, 0, 0],
                  values={"sprite": {"pivotPreset": "bottom_left", "width": 16, "height": 8}})
    await ed.call("save_scene", scene=S["scene"], path=S["path"])
    return S


async def sprite_values(ed: Editor, s: dict) -> dict:
    got = await ed.call("get_entity", scene=s["scene"], entity="Hero", components=["sprite"])
    return got["components"]["sprite"]


def near(a: float, b: float, tolerance: float = 1e-4) -> bool:
    return abs(a - b) <= tolerance


async def test_flip_and_size_properties(ed: Editor):
    s = await setup(ed)
    sprite = await sprite_values(ed, s)
    ed.check_eq((sprite["flipX"], sprite["size"]), (False, [0, 0]), f"defaults: {sprite}")
    await ed.check_error("set_properties", scene=s["scene"], entity="Hero", values={"sprite": {"size": [-1, 1]}}, contains="0")
    await ed.call("set_properties", scene=s["scene"], entity="Hero", values={"sprite": {"flipX": True, "size": [1.5, 0.75]}})
    sprite = await sprite_values(ed, s)
    ed.check_eq((sprite["flipX"], sprite["size"]), (True, [1.5, 0.75]), f"read back: {sprite}")
    await ed.call("undo", scene=s["scene"])
    sprite = await sprite_values(ed, s)
    ed.check_eq((sprite["flipX"], sprite["size"]), (False, [0, 0]), "one undo step")


async def test_lua_flip_size_and_scale(ed: Editor):
    s = await setup(ed)
    await ed.call("play", action="start", scene=s["scene"], fixed_fps=60, auto_pause=True)
    try:
        await ed.call("wait", frames=2)
        await ed.call("eval_lua", code="local h = object('Hero', 'Sprite') h.size = Vector2(1.5, 0.75) h.flipX = true")
        await ed.call("wait", frames=2)
        values = (await ed.call("eval_lua", code="local h = object('Hero', 'Sprite') local b = h:getAABB() "
                                                 "return h.flipX, h.size.x, h.size.y, h.worldSize.x, b.minimum.x, b.maximum.x, b.maximum.y"))["values"]
        ed.check_eq(values[:3], [True, 1.5, 0.75], f"Sprite.flipX and Sprite.size read back: {values}")
        ed.check(near(values[3], 1.5), f"worldSize is the Size: {values}")
        # bottom-left pivot: the quad spans [0, 1.5] x [0, 0.75], mirrored about the pivot to [-1.5, 0]
        ed.check(near(values[4], -1.5) and near(values[5], 0.0) and near(values[6], 0.75), f"the bounds mirrored about the pivot: {values}")

        values = (await ed.call("eval_lua", code="local h = object('Hero') h:setScale(1, 2, 3) local a = h.scale "
                                                 "h:setScale(4) return a.x, a.y, a.z, h.scale.y"))["values"]
        ed.check_eq(values, [1, 2, 3, 4], "setScale(x, y, z) and setScale(s)")
    finally:
        await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)
