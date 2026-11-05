# Dialogs: what blocks the editor and how to answer it

`editor_status.dialog` reports the top-most modal; `respond_dialog` answers it. Edits are refused while one
is open and the refusal says what to answer. Nested: the one under it comes back when it closes.

## What dialog holds

- `kind`: `modal`, `alert`, `save_dialog` or `file_picker`.
- `choices`: what `choice` takes. The message's `[yes = Original Prefab, no = Prefab Variant]` says which
  button each word is. A button with no such meaning has a word of its own (`build_and_run`).
- `disabled_choices`: greyed out now; new `values` may enable them.
- `fields`: what `values` takes, with `name`, `type` (`text`, `path`, `bool`, `int`, `float`, `enum` with
  `options`) and `value`. Only what the dialog draws now: a platform changes the fields below it, so set it first.
- `error`: why it does not take its current values.
- `blocks_edits: false`: Project Settings; edits run under it.

Loading and Creating Project close by themselves: they are not reported and refuse nothing.

## Answering

- `{"choice": "cancel"}` presses a button.
- `{"values": {"name": "Hero"}}` fills fields; the reply has the new `dialog`.
- `{"choice": "ok", "values": {"name": "Hero"}}` fills them, then presses.

A refusal is an error that says what to do, and the dialog stays open: a box on top (answer it with `ok`, then
answer again), a reason in its body, or a button still greyed out. A box whose only choice is `ok` is dismissed by
your next edit, which says so.

## File pickers

`kind: file_picker` holds the editor: only `editor_status` and `respond_dialog` run until it is answered; other calls
are refused unrun, so retry them after.

- `choices: ["ok", "cancel"]`: answer `{"values": {"path": "..."}}` (absolute or relative to the project;
  `paths` where the message says so) or `{"choice": "cancel"}`. A wrong path is refused and it stays open. This is
  every picker of a `--headless` editor.
- `choices: []`: the system's dialog in an editor with a window; only the human there can close it.
