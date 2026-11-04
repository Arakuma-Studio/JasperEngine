"""AC2 and AC3 of docs/specs/S64-shader-lists.md on the default fixture, its scenes as they are and all
but the first closed: get_build_info, generate_shaders and an export name the same shaders, and the
unchanged project then answers from the cache. test_shader_lists.py is the CI side of it, on a small
project; the fixture is not in git, so this is MCP_SUITE=all."""

from __future__ import annotations

from harness import Editor, over_budget
from shader_lists import check_lists_match

TIMEOUT = 1800


async def test_fixture_matches_an_export():
    async with Editor(label="fixture") as ed:
        await check_lists_match(ed)

        times = []
        for _ in range(10):
            info = await ed.call("get_build_info")
            ed.check_eq(info["shader_list_scenes_read"], 0, "an unchanged project reads no scene")
            times.append(info["shader_list_ms"])
        over = over_budget("shader list of the fixture", {"best of 10": min(times), "worst of 10": max(times)}, 50, "ms")
        ed.check(not over, f"over the budget: {over}")
