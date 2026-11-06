# Undo

Every edit in the editor is an undo step with a name: Edit ▸ Undo says what it undoes ("Undo
Move Player", "Undo Rename File a.png → b.png"). The design and acceptance record is
`docs/specs/S24-undo.md`.

## One timeline

Ctrl+Z (Cmd+Z) and Ctrl+Y (Cmd+Shift+Z on macOS) always walk one timeline: the selected
scene's steps and the asset steps of the Project window (rename, move, copy, delete, create
folder or material, file writes, a `.material` edited in the Inspector) in the order you made them. Which window had focus no longer
matters: rename a file, click into the Scene view, and Ctrl+Z still undoes the rename. Only a
text field you are typing in and the Code editor keep their own undo.

- Steps of other scenes stay in those scenes: switching scene tabs changes nothing.
- A new step frees what you could redo, in that scene and in the asset steps.
- Steps made together (an agent's batch over several scenes and an asset) undo together. When
  one part is under newer edits of its history, it stays, and the Console says so ("Batch step
  partly undone: Scene 'Level2' has newer edits").
- Closing a scene frees its steps.
- Undoing a step that is in the saved file (done before the last save) leaves the scene modified,
  and an applied prefab with unsaved changes: the next save writes the undone state. A save also
  ends the step before it, so the next edit is a step of its own.

## Drags

A drag is one step: the move, rotate and scale gizmos and every other Scene view handle, an
Inspector slider, field drag or colour picker, a terrain stroke, a key or block drag in the
Animation window. **Esc** (or Ctrl+Z) while the mouse is still down puts everything back as it
was, the selection and the unsaved mark included, and adds no step. Opening a menu or a popup,
or switching to another application, ends the drag as it is. A node drag in the Animator is one
step too, and Esc puts it back; Ctrl+Z during it undoes the step before it. An edit that changes nothing (a
click on a field, a drag back to where it started) is no step either.

## Selection

Selecting GameObjects is a step of its own, as in Unity: Ctrl+Z gives the previous selection
back. Selection changes of one click (or one box select) are one step, and nothing is recorded
while you have something to redo, so clicking around after an undo never loses the redo.
Undoing any edit restores the selection it had: undoing a Delete selects the GameObjects again.
Editor Settings ▸ Undo ▸ Record Selection in Undo turns the selection steps off.

## Undo History

Window ▸ General ▸ Undo History (next to the Console) lists the timeline from "(Start)" to the
newest step, with an icon (scene, asset, selection, agent, Play review), the name and how long
ago. Click a row to jump there; "(Start)" undoes everything; Back and Forward step once. Redo
rows are dimmed. The tooltip shows the time, the history, who made the step, its size and its
type. While the scene plays, a divider marks where Play began and the steps before it are
disabled. Filter by name, show the selection steps or the other scenes' histories (read-only),
and Clear History… the timeline. The footer shows the memory the histories hold, an estimate.

## Limits

Editor Settings ▸ Undo:

| Setting | Default | |
|---|---|---|
| Max Undo Steps | 1000 | Steps each scene (and the asset history) keeps, 50 to 10000 |
| Undo Memory Limit (MB) | 512 | All histories together, 64 to 8192, 0 = unlimited |
| Record Selection in Undo | on | Selection changes as steps |
| Review Play Mode Changes | on | The Play Mode Changes window on Stop |

The oldest steps go first, from whichever scene holds them. The steps of Play mode, of a drag
in progress and of an agent's batch in progress wait until those end. Big steps (a large delete,
a terrain stroke) are freed on a background thread.

## Play Mode Changes

Stop puts the scene back as it was before Play, as in Unity. With Review Play Mode Changes on,
the edits you made while playing are listed in Window ▸ General ▸ Play Mode Changes (it opens on
Stop): GameObject ▸ component ▸ property, with the edit value, the value at Stop and the step it
came from. Keep Selected or Keep All applies them to the restored scene as one step, "Apply Play
Mode Changes (N)", that undoes back to the edit values (and on a prefab instance records the
overrides there). Discard, or closing the window, drops them; so does playing the scene again.

- What a script changes is not a step. To keep it, use the Inspector component menu ▸ Keep
  Component Values After Play, or the Hierarchy menu ▸ Keep Values After Play (all components),
  while playing: at Stop those components' values are listed too, with the keys that differ.
- GameObjects a script made can't be kept ("Created by a script during Play"). GameObjects you
  made (or duplicated, pasted) while playing are kept with their children and ids.

## Agents

An MCP agent's edits are its own steps, named after it ("Claude Code: Set Intensity (Sun)"),
one per tool call and history; the Undo History shows them with a robot icon. The agent tools
are described in the agent guide (`jasper://guide`, section Undo) and `docs/mcp/TOOLS.md`:
`get_undo_history`, `undo` / `redo` with `scope: "timeline"`, `to_id` and `expect_top_id`, `batch`
with `undo_group` and `undo_name`, and `get_play_changes` / `apply_play_changes` /
`discard_play_changes`.

## Tests

`editor/tests/command/` (the `CommandHistoryTests`, `UndoTimelineTests`, `PrefabUndoTests`,
`PlayChangeReviewTests` and `CommandHistoryBench` suites of `jasperengine-editor-tests`),
`tools/mcp/tests/test_undo.py` (agent semantics, CI) and `tools/mcp/tests/test_undo_ui.py`: Esc in a
gizmo, Inspector and terrain drag, the Undo History panel and the Play Mode Changes window driven
with real mouse and keys (xdotool in the harness's Xvfb), with screenshots.

## Command line

`jasperengine-editor scene-dump --ops <file>` runs its `undo` and `redo` ops through the editor's
own history (merges, no-op drops, the Play floor); the `history` op prints the steps as
`id name` lines.

## For contributors

Every class deriving `Command` names itself: `describe()` and `typeName()` are pure virtual, so a
new command doesn't compile without a name. Use the words of `command/CommandNames.h` ("Set
<Property> (<subject>)", "N Objects"). A command holding YAML, clips, files or pixels reports
`memoryBytes()`; one whose destructor only frees memory may return `releaseOffThread()`. Value
edits implement `isNoOp()` and `collectKeepCommands()` (Play Mode Changes). A drag opens
`CommandHistory::beginInteractive()` and commits on release; new tools (splines, particles,
animation) do the same.
