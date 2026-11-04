# Dialogs: what blocks the editor and how to answer it

Any modal window that blocks a human at the editor also shows up to you. `editor_status.dialog` reports
the top-most one, `respond_dialog` answers it. Edit tools are refused while one is open, and the refusal
says what to answer; read tools still run.

## What editor_status.dialog says

```json
{"kind": "modal", "title": "Rename File", "message": "Rename 'a.lua' to: ...",
 "choices": ["ok", "cancel"], "disabled_choices": [],
 "fields": [{"name": "name", "type": "text", "value": "a.lua"}],
 "error": ""}
```

- `kind`: `modal`, `alert` (an editor message with Yes/No/OK), `save_dialog` (Save Scene, Save Project)
  or `file_picker`.
- `choices`: what `choice` takes. `ok`, `yes`, `no` and `cancel` are the usual buttons; the message's
  `[yes = Original Prefab, no = Prefab Variant, cancel = Cancel]` says which button each one is when the
  labels differ. A button without one of those meanings has a word of its own (`build_and_run`).
- `disabled_choices`: buttons greyed out now. New `values` may enable them.
- `fields`: what `values` takes, with the current value. `type` is `text`, `path`, `bool`, `int`, `float` or
  `enum` (with `options`). Only what the dialog draws now is listed: choosing a platform in Build Settings
  changes the fields below it, and the reply to `values` shows them.
- `error`: why the dialog does not take its current values, in its own words.
- `blocks_edits: false`: Project Settings; its OK keeps your edits, so nothing is refused under it.

A window that closes by itself (Loading, Creating Project) is not a dialog: it is not reported and refuses
nothing.

## respond_dialog

- `{"choice": "cancel"}` presses a button, through the button's own code, as a click does.
- `{"values": {"name": "Hero"}}` fills fields and leaves the dialog open; the reply has the new `dialog`.
- `{"choice": "ok", "values": {"name": "Hero"}}` fills the fields, lets the dialog check them for a frame,
  then presses the button.

The reply says what the editor shows afterwards (`dialog`, when something is still open). When the dialog
refuses, the reply is an **error** with the dialog's reason, and the dialog stays open: an error box of its own
that came up on top ("A file with this name already exists": answer it with `ok`, then try again), a reason in
its body, or a button that stayed greyed out. Unknown fields and values of the wrong type are refused before
anything is applied, and the error lists what it takes.

Nested dialogs: the top-most is the one reported and answered; the one under it comes back when it closes.

A box whose only answer is `ok` (an error box) does not stop you: the next edit dismisses it and says so in its
result.

## File pickers

A picker is a call that returns a path, so the editor's main thread waits inside it.

- **In a `--headless` editor** the picker waits for you, whatever opened it. `editor_status.dialog` has
  `kind: file_picker`, `choices: ["ok", "cancel"]` and a `path` field, and the message says what it wants
  (the file types, a folder, where to save). Answer with `{"values": {"path": "..."}}` (absolute, or relative to
  the project), `{"values": {"paths": [...]}}` for the pickers that take several, or `{"choice": "cancel"}`. A path
  that does not exist or is not of the types it takes is refused and it stays open. The call that opened it
  (`run_command assets.import`, the `respond_dialog` of a button) replies right away with what to answer.
- **While it waits nothing else runs**: `editor_status` and `respond_dialog` answer; every other call is refused
  with this explanation and not run. Retry it after answering.
- **In an editor with a window** the picker is the system's own dialog for the human at it. `editor_status` reports it
  (`choices: []`) so you can see why the editor is not answering; only they can close it. To drive pickers on a
  machine with a display, start the editor with `--headless`.
- With no MCP session connected, a headless picker returns as cancelled instead of waiting.

Many pickers have a field that does the same without a picker: `output_directory` in Build Settings,
`script_file` in New Script. The tools (`import_asset`, `save_scene` with a path, `build_project`) need no dialog at
all, which is usually the better way; dialogs are for what a human opened, or for checking what a human sees.
