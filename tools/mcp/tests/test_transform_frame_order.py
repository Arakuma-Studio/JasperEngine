"""World poses read between frames (S95 REQ-6): get_runtime_entity reports a child's world position,
rotation and scale as its script's getWorld*() reads them, current after its parent moved, before
any frame propagated the move."""

import harness
from harness import Editor

CI = True
EDITOR = {"create": True, "name": "FrameOrder"}


def vec(value) -> list:
    if isinstance(value, dict):
        return [value[k] for k in ("x", "y", "z")]
    return list(value)[:3]


def close(a: list, b: list, eps: float = 1e-4) -> bool:
    return len(a) == len(b) and all(abs(x - y) <= eps for x, y in zip(a, b))


async def test_runtime_entity_world_pose_is_current(ed: Editor):
    parent = (await ed.call("create_entity", name="Mover", type="empty"))["entity_id"]
    await ed.call("create_entity", name="Rider", type="empty", parent=parent, position=[1.0, 2.0, 3.0])
    await ed.call("play", action="start", fixed_fps=60, auto_pause=True)
    try:
        await ed.call("wait", frames=1)
        # no frame runs between these calls (auto_pause): only the on-demand pose sees the move
        seen = (await ed.call("eval_lua", code="object('Mover').position = Vector3(10, 0, -5) "
                                               "local p = object('Rider'):getWorldPosition() return p.x, p.y, p.z"))["values"]
        ed.check(close(seen, [11.0, 2.0, -2.0]), f"getWorldPosition after the parent moved: {seen}")
        live = await ed.call("get_runtime_entity", entity="Mover/Rider", components=["transform"], include_script_state=False)
        world = vec(harness.find_key(live, "worldPosition"))
        ed.check(close(world, seen), f"get_runtime_entity worldPosition {world} equals the script's {seen}")
        ed.check(harness.find_key(live, "worldRotation") is not None and harness.find_key(live, "worldScale") is not None,
                 f"world rotation and scale listed: {live}")
    finally:
        await ed.call("play", action="stop", auto_pause=False, fixed_fps=0)
    await ed.check_alive()
