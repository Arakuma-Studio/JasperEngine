# Dialogue (agents)

Conversations are `.dialogue` graphs (kind `dialogue`), played by a **Dialogue Runner**
component; never a Lua table of lines. Tools (group `graphs`): `edit_graph`, `validate_graph`,
`list_graph_node_types {kind: "dialogue"}`, `run_dialogue`. Reference: `docs/dialogue.md`.

## Write a branching conversation

1. `tool_groups {enable: ["graphs"]}`.
2. `edit_graph {path: "assets/dialogue/guard.dialogue", create: {kind: "dialogue"}, ops: [...]}`: the namespace
   comes from the file name. Nodes: `entry {name}`, `line {speaker, text, options, variants}`,
   `branch {condition}`, `set {assign}`, `action {action}`, `jump {dialogue?, entry}`, `end {result}`.
   A line's options are `[{id: "o1", text, condition?, action?, once?, when_false?}]`, each an output port
   named by its id; set `next_sub_id` past the ids you use. Wire with `connect {from: {node, port}, to: {node, port: "in"}}`;
   an unconnected output ends the conversation.
3. Expressions: `$gold >= 10 and not visited("n3")`, handler calls `quest.done:intro` or `shop.canAfford(5)`;
   `set` and actions take `$gold -= 10; quest.start:intro`.
4. `validate_graph` lists the lint (no-exit, unreachable, unknown-variable ...). Declare variables in the
   graph (`addVariable`) or `set_project_settings {"dialogue.variables": [...]}`.

## Walk it

- `run_dialogue {path, choices: [1, 2]}` plays it without Play; unlisted handler conditions are false
  and listed in `unknown_calls`: pass `conditions: {"quest.done": true}` (or `"quest.*"`) for the other branch.
- Every ending reachable? `run_dialogue {path, explore: true}`: endings with the choices leading there,
  `no_exit_loops`, `lines_never_shown`. Do not reason over `get_graph` for this.
- In Play, `run_dialogue {entity, choices, advance}` drives the live runner with the real handlers.

## Register a handler (Lua, in a script's init)

`Dialogue.onAction("shop", function(runner, verb, args) ... return "wait" end)` (resume with
`runner:resume()`), `Dialogue.onCondition("quest", function(runner, verb, args) return true end)`.

## Translate it

The texts are the dialogue's own String Table (keys `<ns>.<node>`, `<ns>.<node>.<option>`):
`edit_string_table {path: "assets/dialogue/guard.dialogue", ops: [{op: "add_language", language: "de"},
{op: "set", key: "n2", language: "de", text: "Halt!"}]}`. Edit source texts with `edit_graph` setParam.
