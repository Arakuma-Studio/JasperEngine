# S41 — Window-scoped commands: a key acts on the panel that has focus, and every panel key is rebindable

**Depends on.** S25 (`CommandCatalog`, `ShortcutDispatcher`, contexts), S27 (curve editor, colour picker). Touches
`editor/AppCommands.cpp`, `editor/shortcuts/`, `editor/widget/CurveEditor.cpp`, `editor/widget/ColorPickerWindow.cpp`.

## 1. Goal
A shortcut does what the focused panel says, and only that. Pressing Delete while the Curve editor has focus removes
curve keys, never the GameObjects selected in the Hierarchy. F frames the curve, not the Scene view. Today this is
inferred from reading the code and could lose a user's work. The curve and colour keys that bypass the catalog become
real commands, so a user can rebind them like every other key.

## 2. Scope
In: every window and widget that handles Delete, Backspace, F, Ctrl+C/V/X/D or similar on its own while a global
command with the same key exists. The five keys now marked `shortcut-lint: allow` in `CurveEditor.cpp` and
`ColorPickerWindow.cpp`. Out: new shortcuts, changing default keys.

## 3. Current state
`AppCommands.cpp:151` enables `edit.delete` with `sceneKeysAnd(notAssetPanel)`; `notAssetPanel` only knows the Project
and Jobs windows. `CurveEditor.cpp:528-540` reads F, Delete/Backspace, Ctrl+C and Ctrl+V directly with
`ImGui::IsKeyPressed`, so both the widget and the global command can fire in one frame. `AnimatorWindow` and
`InputActionsWindow` already route `edit.delete` through `canDelete()`/`removeSelection()`.

## 4. Requirements
- R1. Start with a red test that reproduces the bug: scene entity selected, curve editor focused, Delete pressed through
  the real dispatcher. If it does not reproduce, record that in Deviations; R2-R7 still apply.
- R2. One general mechanism: a focused panel claims the edit commands (delete, duplicate, frame, copy, paste, cut) with a
  handler. While a claim is active the command goes to the panel and nowhere else, whether it came from a key, the Edit
  menu or the command palette. No per-panel special cases in `notAssetPanel`.
- R3. The Edit menu items show the claimed meaning: enabled or disabled by the panel's handler.
- R4. Audit every `IsKeyPressed` of those keys under `editor/`. Each either goes through the claim or carries a stated
  reason in its lint comment. The audit's table goes in this spec's Deviations section.
- R5. The curve editor's F, Delete/Backspace, Copy, Paste and the colour picker's eyedropper key become catalog commands
  in new contexts, listed and rebindable in Shortcuts, with the same defaults. The lint allow-list entries go away.
- R6. Text fields keep their keys: a focused input still receives Delete, Ctrl+C and so on.
- R7. Curve edits stay undoable as named commands; the behaviour of Scene, Hierarchy, Animator and Input Actions is
  otherwise unchanged.

## 5. Acceptance
- AC1. Red-then-green unit or MCP test for R1 (and the same for F and Ctrl+C/V).
- AC2. A test per claim-holding panel: its Delete affects it, not the scene.
- AC3. A rebind test: bind curve frame to another key through the settings path; the old key no longer frames, the new one
  does, and a saved settings file reloads it.
- AC4. `tools/check-changed` green, `test_search_organisation_ui.py` green, shortcut lint has no entry for those keys.
- AC5. `docs/` shortcut list and MCP guide updated if tools change.

## 6. Risks & defaults
A panel focused but with nothing selected: Delete does nothing and does not fall through to the scene (Unity behaves so).
