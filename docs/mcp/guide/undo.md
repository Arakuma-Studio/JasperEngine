# Undo for agents

Every edit an agent makes through MCP is an undo step of the editor, on the same timeline as
the human's Ctrl+Z (`docs/undo.md` describes the editor side).

## Your steps

- Every edit result carries `undo` {id, name}: your step, named "<your client name>: Set
  Intensity (Sun)". One step per call and history.
- A `batch` with `undo_group` is one linked step, named by `undo_name`.
- An edit that changes nothing makes no step.

## Undoing

- `undo` / `redo` with scope "timeline" are exactly the human's Ctrl+Z / Ctrl+Y: the scene's
  steps and the asset steps in one order. The default scope "scene" will switch to it.
- To undo only your own last call, pass `expect_top_id` = that `undo.id`: it refuses, changing
  nothing, when someone edited since.
- `get_undo_history` lists the steps; `to_id` jumps to one.

## Play mode

Edits made while playing are rolled back by play stop and then offered: `get_play_changes`,
then `apply_play_changes` (one step) or `discard_play_changes`. `play` stop with
`keep_changes` all|none decides at once.
