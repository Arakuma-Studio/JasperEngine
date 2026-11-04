---
paths:
  - "engine/core/htmlui/**"
  - "editor/htmlui/**"
---

# HTML/CSS UI

- Suites: `tools/build.sh htmlui-tests` (engine, goldens on glcore and Vulkan) and
  `tools/build.sh htmlui-editor-tests` (the editor's importers against a local fixture server),
  plus the `tools/mcp/tests/test_html_ui_*.py` MCP tests.
- After changing CSS or element support, regenerate the support tables and keep
  `jasperengine-editor check --ui` reporting every construct the engine drops.
- The contracts are in `docs/html-ui/CONTRACTS.md`.
