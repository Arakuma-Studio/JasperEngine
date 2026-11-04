"""AC2 of docs/specs/S64-shader-lists.md on the default fixture, its scenes as they are and all but the
first closed: get_build_info, generate_shaders and an export name the same shaders. The small project
of test_shader_lists.py is the CI side of it; the fixture is not in git, so this is MCP_SUITE=all."""

from __future__ import annotations

from harness import Editor
from test_shader_lists import check_lists_match

TIMEOUT = 1800


async def test_fixture_matches_an_export():
    async with Editor(label="fixture") as ed:
        await check_lists_match(ed)
