# Graph Editor

Window > Assets > Graph Editor edits a graph file of any kind the editor registers (the graph
model of `engine/core/graph`, S23). A kind with a window of its own, such as Procedural Material
(S90), opens in that window, which is built on this one. Double-click a graph file in the Project
window to open it.

## The canvas

- **Ports and wires.** Inputs sit on a node's left edge and outputs on its right, coloured by
  type. Drag from a port to another to wire them: only ports whose types fit, in the other
  direction, on another node, and (in an acyclic kind) without closing a loop take the wire. An
  input that takes one wire loses the one it had.
- **Add a node.** Drop a wire on empty canvas: the search lists only the nodes with a port that
  fits, and the node you pick is wired with that port under the cursor. Right click on empty
  canvas opens the same search, unfiltered. Type to rank by name, category and aliases; once you
  type, each value of a node's enum parameters is an entry of its own ("Blend: Multiply").
  Up, Down and Enter pick from the keyboard; Esc closes it.
- **Select and move.** Click, Shift / Ctrl click, or drag a box. Drag nodes to move them; a press
  on a group's title moves its nodes. **F** frames the selection (all nodes when none is
  selected). Middle drag or Alt + drag pans; the wheel zooms. Zoomed out, ports and their labels
  fade while node images stay.
- **Esc** during any drag (nodes, a box, a wire) puts everything back and adds no undo step.
- **Arrange** lays every node out left to right by what feeds what, as one undo step. Nodes a
  file gives no position (an agent's, written without one) are always drawn laid out, and keep
  no position until you move them or Arrange.

## Parameters

The selected node's parameters show in the Inspector: numbers, toggles, enums, colours (the Color
window), gradients (the Gradient Editor, up to 32 keys in a graph) and curves (the Curve window).
Right click a parameter's name to copy or paste its value as JSON text.

## Copy, Paste, Duplicate, Delete

Ctrl+C copies the selected nodes and the wires between them as JSON text, so a graph fragment can
be pasted into another graph of the same kind, a text file or a message. Ctrl+V pastes at the
mouse with new ids; Ctrl+D duplicates; Delete removes the selected nodes (with their wires) or the
selected wire.

## Problems

The strip above the canvas lists the graph's issues (the model's checks and the kind's own). A
click selects the node; **Fix** applies an issue's fix when it is a single edit, such as removing
a wire to a node that is gone.

## Undo and files

Every edit is written at once as one project undo step (no Save), like every asset window. A
change made outside the window (`edit_graph`, another program, an undo) shows at once; a file that
stops parsing keeps the last good graph on screen with the error in the Problems strip.
