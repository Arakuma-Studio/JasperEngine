# S69 — Every modal dialog can be seen and answered by an agent

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
- R8. Follow AGENTS.md "MCP text": new tool text needs a case in `tools/mcp/eval/cases.py` that fails without it; stay within `tools/mcp/budget.json`; regenerate `docs/mcp/TOOLS.md`; put guidance in `docs/mcp/guide/`.

## 3. Acceptance
- AC1. A test that opens each modal it can drive (table of all sites in the test: driven or stated why not), checks `editor_status`
  and answers it. At least the About, Export, Preferences, Scenes, Prefabs, ScriptCreate, ShaderFork, TextureSlicer dialogs.
- AC2. Ported `test_editor_modals.py`, failing on main first. Unit tests for `EditorModal`.
- AC3. The lint has no exceptions other than `EditorModal.cpp` itself.
- AC4. Screenshots of two dialogs, unchanged against main. `tools/check-changed` green.

## Deviations
- **Choices are not limited to ok/yes/no/cancel.** `respond_dialog.choice` is a string, one of the dialog's
  `choices`: a button with no such meaning has a word of its own (`build_and_run`, a VCS dialog's label). Without
  this Build And Run and the VCS dialogs could not be answered.
- **A native picker in a windowed editor is reported, not answered (R5).** NFD offers no way to close its dialog from
  outside. `editor_status` says it is there, from the transport thread (the main thread is inside the dialog);
  `respond_dialog` says only a human can answer it. An agent that must drive pickers uses `--headless`, where every
  picker waits for `values.path`. No caller of `FileDialogs` changed.
- **While a picker waits, only `editor_status` and `respond_dialog` run.** The main thread is held as a native dialog
  holds it, so a read tool cannot run; the transport refuses it at once, naming the answer, instead of hanging. Calls
  already queued or waiting for frames when it opens get the same refusal, the call that opened it replies at once
  (a client that sends one call at a time could never answer otherwise), and an edit's continuation resumes afterwards.
- **Role::Progress removes the "edit waits for Loading" path of McpServer::execute** (R4, "as in the reference"). The
  wait (39befe24c) ran only when a call reached `execute` from the tick after the windows, the one moment ImGui
  reported the modal; a Loading modal is now not a dialog anywhere, so edits run under it as they did on the common path.
- **Fields are declared with `EditorModal::field` (and `pathField`, `enumField`) as well as the widget wrappers**, for
  values whose widget edits something else (radio buttons, tree selections, custom path displays). Lists a human clicks
  (Choose Asset, Replace Prefab) are an `enum` or a `path` field naming the row.
- **`EditorDialogInfo` moved to `EditorModal.h`** (so its unit tests need no `App.h`).
- **A save takes a moment and can go on to another dialog.** `editor_status.playing` is true while a scene is `saving`
  (as before S69), and in a temporary project Save All goes from Save Scene to Save Project once the save ends, so a reply
  with no `dialog` does not mean none follows.
- **The Loading popup no longer closes a dialog.** ImGui keeps one
  popup per level, so when builds started `OpenPopup("Loading")` replaced whatever was open, and a dialog opened once
  was gone for good: a person or agent who asked for it while a project's shaders were starting saw nothing happen. The
  progress now waits while another popup is open and comes up once it closes; a dialog asked for during builds opens as
  before, and the progress returns after it. A person sees the dialog stay where it used to vanish.
- **A refusal's instructions are in the error's text, not in `next`.** Eval `dialog_refusal_answers_the_box_first`,
  sonnet, 2 samples: for an `isError` result Claude Code showed the model the text and not `structuredContent`, so a `next` there
  was never read and both samples failed. A picker's message needs no instructions either: with them stripped from the mock,
  `dialog_answers_a_file_picker` passed 4 of 4 on the schema and fields alone, so they were cut.
