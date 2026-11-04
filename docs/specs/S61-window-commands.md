# S61 — Window-scoped commands: a key acts on the panel that has focus, and every panel key is rebindable

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

## Deviations

**R1 reproduced.** Red on the unchanged build (`tools/mcp/tests/test_window_commands.py`, real keys under Xvfb): Delete
deleted the selected GameObject (entities 3 -> 2), F moved the Scene view's camera, and Ctrl+C / Ctrl+V with the pointer
over the Scene view pasted a GameObject (3 -> 4). So this was a real loss of work, not only an inference.

**Decisions.**
- The mechanism is `editor/shortcuts/PanelClaims.h`. Beyond the six verbs of R2 it also covers Paste As Child, Select All
  and Rename: left out, they still reached the scene (F2, Ctrl+A, and the menu's Paste As Child) from a claiming panel.
- A panel with its own catalog command for a verb keeps the key on that command: `edit.frameSelected`'s key is quiet while
  the panel has focus, so AC3's "the old key no longer frames" holds even though F is still Frame Selected's binding. The
  Edit menu shows the panel's binding while the claim is active, which also moves macOS's key equivalent with it.
- Holders: Curve, Color Picker, Gradient Editor, Input Actions, Animator, Jobs & Memory, Project, Console (its list),
  Sprite Normal Map, Animation (clip mode), Pose Animation (also a Scene view posing for it). `notAssetPanel` and
  `sceneKeys` are gone: the claims say the same. Consequences: Edit > Delete, Copy, Cut, Paste, Select All and Rename now work
  in the Project window; Delete and F in the Console's list, the Color window and the Gradient Editor no longer reach the scene.
- The Curve window's keys act while it has focus. They used to act under the pointer too (`hovered`), which is how a key
  could reach two panels; `docs/editor/curves.md` says focus now.
- The Scene view's hover keeps its other keys, except `sceneView.deleteMeasure`: under the pointer it no longer takes Delete
  from a claiming panel.
- The Sprite Normal Map window's F became the claim's Frame handler, and with it focused the `tools.*` keys (W, E, G, X...)
  are off: they fired together with that window's own letters.
- `editor_status` gained `ui.curve_window` and `ui.color_picker`: agents and tests need them to check these keys.
- The generated AGENTS.md's Source references says an editor key is rebound with `set_shortcut`: without it agents asked to
  rebind a key went to the editor's source (eval case `guard_curve_keys_rebind`, 0/2 without the sentence, 2/2 with it; a line
  in the guide's Other areas did not move them, 0/2).

**R4 audit.** Every key poll of these keys under `editor/`:

| Where | Keys | Verdict |
|---|---|---|
| `widget/CurveEditor.cpp` | F, Delete, Backspace, Ctrl+C, Ctrl+V | removed: catalog `curve.*`, through the claim |
| `widget/ColorPickerWindow.cpp` | I | removed: catalog `colorPicker.eyedropper` |
| `window/SpriteNormalsWindow.cpp` | F | removed: the claim's Frame handler |
| `window/SpriteNormalsWindow.cpp` | V, X and its other bare letters | kept, lint reason: the tool window's own keys while focused (bare letters, not Ctrl+V/X; `tools.*` are off there) |
| `window/InputActionsWindow.cpp:931` | any | kept, lint reason: Listen captures any key for a game binding |
| `window/dialog/ProjectSettingsWindow.cpp` (Listen) | any | kept, lint reason added: a modal's Listen |
| `window/ProjectWindowSearch.cpp` | letters, digits | kept, lint reason added: type-ahead, no modifiers |
| `window/dialog/ComponentAddDialog.cpp` | Backspace | kept: a local navigation key of a popup, where the dispatcher fires nothing (lint's `LOCAL_KEYS`) |
| `window/widget/CustomTextEditor.cpp` | Ctrl+C/X/V/F/D, Backspace, Delete | kept: the Code Editor's text keys; Editor commands do not fire there (lint `SKIPPED`) |
| `window/widget/SelectableTextView.cpp` | Ctrl+A, Ctrl+C | kept: a text view's selection (lint `SKIPPED`) |
| `window/PlayInput.cpp` | all | kept: forwards keys to the game (lint `SKIPPED`) |
| `shortcuts/ShortcutsWindow.cpp` | any | kept: captures a chord, the dispatcher is held meanwhile (lint `SKIPPED`) |
| `sceneview/SceneViewToolbar.cpp:724`, `window/SceneWindow.cpp` | held V; held W/A/S/D/Q/E | kept: `IsKeyDown` gestures (measure snap, fly), not presses; read-only catalog rows |

