# Undo for agents

Served as `jasper://docs/undo`. The user documentation of undo is `docs/undo.md`; this page is
what an agent needs to undo its own work without touching a human's.

## Steps and names

- Every edit result carries `undo` {id, name}: the step your call made, named
  "<your client name>: Set Intensity (Sun)". One call makes one step per history it edits.
- A `batch` with `undo_group` is one linked step, named by `undo_name`.
- An edit that changes nothing makes no step (no `undo` in its result).

## undo and redo

- `undo`/`redo` with scope "timeline" are exactly the human's Ctrl+Z/Ctrl+Y: the scene's steps
  and the asset steps in one order. The default scope "scene" will switch to it; "project" is
  the asset steps alone.
- To undo only your own last call, pass `expect_top_id` = that call's `undo.id`: it refuses,
  changing nothing, when someone edited since.
- `get_undo_history` lists the steps, newest first; `to_id` on `undo`/`redo` jumps to one.
- A step can refuse (`refused: true`): undoing or redoing it now would make a child scene
  cycle. The console names the scenes.
- While a Physics Placement gesture or settle runs in the Scene view, `undo` cancels it instead
  (`cancelled_gesture: true`, nothing undone) and `redo` is refused until it ends.

## Play mode

Edits made while playing are rolled back by `play` stop and then offered back:
`get_play_changes`, then `apply_play_changes` (one step) or `discard_play_changes`.
`play` stop with `keep_changes` all|none decides at once.
