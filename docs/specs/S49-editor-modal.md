# S49 — Every modal dialog can be seen and answered by an agent

**Depends on.** S31, MCP core tools. Reference: sound-layer `editor/window/EditorModal.{h,cpp}`, `test_editor_modals.py`,
`2026-10-19-sound-layer-cleanup-b-design.md` §2.3, in `je-wt/snd-ro`. Touches 71 `BeginPopupModal` sites in 36 files; merge hotspot, so
commit by group of windows and merge `main` often.

## 1. Goal
Agents and humans are equal users, so any modal that blocks the human also shows up to the agent: `editor_status` reports
its title, message and choices, and `respond_dialog` answers it. Today `App::getOpenDialog` sees the top-most modal but has no
choices or message, so an agent gets stuck on most of them.

## 2. Requirements
- R1. One class, `EditorModal`: `begin`, `message`, `button(label, choice)`, roles (dialog, merges edits, progress), as in the
  reference. All 71 sites use it. A lint in `tools/` fails on a raw `ImGui::BeginPopupModal` outside `EditorModal`.
- R2. `editor_status.dialog` gives kind, title, message, choices, and field names and current values for fields.
- R3. Name prompts and fields are answerable too: `EditorModal::inputText` and friends (checkbox, combo), and `respond_dialog` gains a
  `values` object. An agent can complete a rename prompt or an export dialog, not only cancel it. Validation errors from the dialog
  come back in the reply.
- R4. Edits are refused while a dialog-role modal is open, as today for the others; merge-edits and progress roles behave as in the
  reference. Nested modals: the top-most is the one reported and answered.
- R5. File pickers (native or ImGui) report a clear message and accept `values.path` where a path is the answer.
- R6. Humans see no change: same layout, same keys.
- R7. Headless: a modal opened in a headless editor is still answerable, and a frame is forced if needed.
- R8. Stay within `tools/mcp/budget.json`; regenerate `docs/mcp/TOOLS.md`; put guidance in `docs/mcp/guide/`.

## 3. Acceptance
- AC1. A test that opens each modal it can drive (table of all sites in the test: driven or stated why not), checks `editor_status`
  and answers it. At least the About, Export, Preferences, Scenes, Prefabs, ScriptCreate, ShaderFork, TextureSlicer dialogs.
- AC2. Ported `test_editor_modals.py`, failing on main first. Unit tests for `EditorModal`.
- AC3. The lint has no exceptions other than `EditorModal.cpp` itself.
- AC4. Screenshots of two dialogs, unchanged against main. `tools/check-changed` green.
