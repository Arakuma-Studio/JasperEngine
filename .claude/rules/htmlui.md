---
paths:
  - "engine/core/htmlui/**"
  - "editor/htmlui/**"
---

# HTML/CSS UI

- Suites: `tools/build.sh htmlui-tests` (engine, goldens on glcore and Vulkan) and
  `tools/build.sh htmlui-editor-tests` (the editor's importers against a local fixture server),
  plus the `tools/mcp/tests/test_html_ui_*.py` MCP tests.
- What the engine supports is generated: after changing CSS or element support, regenerate the
  support tables with `tools/mcp/gen_html_ui_support.py` (`docs/mcp/guide/html-ui-support.md`)
  and keep `jasperengine-editor check --ui` reporting every construct the engine drops.
- The design notes and contracts are in `docs/html-ui/`.
