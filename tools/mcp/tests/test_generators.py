"""Public CSS completion keywords, independent of generator internals.

The author-facing property names are specified by the HTML UI guide and phase-3
CSS compatibility contract. Their presence in the completion data protects the
historical loss of properties registered by RmlUi loops. Generic generator output
self-comparisons are not regression oracles.
"""

import json
import re

import harness
from harness import TestFailure


async def test_rcss_properties_keep_loop_registered_names():
    # CSS authoring keys: docs/mcp/guide/html-ui.md (supported border, transition,
    # stacking properties), docs/html-ui/phase-3-fork-patches.md.
    # Read only literal key bytes: variable renames, formatting, and comments
    # must not affect this configuration-data check.
    header = (harness.REPO / "editor/window/widget/RcssProperties.h").read_text(encoding="utf-8")
    tokens = re.findall(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"', header, re.S)
    names = {json.loads(token).casefold() for token in tokens if token.startswith('"')}
    required = {"border-top-width", "border-left-width", "border-top-left-radius", "border-bottom-right-radius",
                "will-change", "transition-property"}
    if not required <= names:
        raise TestFailure(f"missing author-facing CSS completion keys: {sorted(required - names)}")
