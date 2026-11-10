"""S46 AC17: fix_pixel_art and lint_pixel_art are hidden until tool_groups enables pixel_art (docs/mcp/conventions.md)."""

from __future__ import annotations

from harness import Editor

CI = True
EDITOR = {"create": True, "name": "PixelArtGroups"}


async def test_cleanup_tools_hidden_until_enabled(ed: Editor):
    default = {t.name for t in await ed.tools(groups=None)}
    ed.check("fix_pixel_art" not in default, "fix_pixel_art is hidden in a new session")
    ed.check("lint_pixel_art" not in default, "lint_pixel_art is hidden in a new session")
    everything = {t.name for t in await ed.tools()}
    ed.check("fix_pixel_art" in everything and "lint_pixel_art" in everything, "both are registered")
    await ed.call("tool_groups", enable=["pixel_art"])
    listed = {t.name for t in await ed.tools(groups=None)}
    ed.check("fix_pixel_art" in listed and "lint_pixel_art" in listed, "listed after tool_groups enables pixel_art")
