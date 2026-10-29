#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""The navigation step of tools/smoke-test.sh (docs/specs/S19-navigation.md AC28): a new project's
3D scene with a floor and a block, a NavMesh Surface baked through bake_navigation, a path query
that must be complete and walk around the block. Exits 1 on any failure.

    python3 tools/mcp/nav_smoke.py        (JASPER_EDITOR picks the editor, as for the MCP tests)
"""

import asyncio
import sys

from harness import Editor


async def main() -> int:
    async with Editor(create=True) as ed:
        made = await ed.call("create_scene", name="NavSmoke", type="3D", path="scenes/NavSmoke")
        scene = made["scene"]["id"]
        floor = await ed.call("create_entity", scene=scene, type="plane", name="Floor", scale=[2, 1, 2])
        await ed.call("create_entity", scene=scene, type="cube", name="Block", position=[0, 1, 0], scale=[2, 2, 6])
        await ed.call("add_component", scene=scene, entity=floor["entity_id"], component="navmeshsurface")
        baked = await ed.call("bake_navigation", scene=scene, mode="full")
        polys = baked["polys"]
        path = await ed.call("query_navigation", scene=scene, op="path", **{"from": [-8, 0, 0]}, to=[8, 0, 0])
        if path["status"] != "complete" or path["length"] <= 16.0 or len(path["corners"]) < 3:
            print(f"FAIL: navigation path {path}")
            return 1
        print(f"ok: navigation baked ({polys} polys), path around the block {path['length']:.2f} m")
        return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
