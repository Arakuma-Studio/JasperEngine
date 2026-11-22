# Dialogue graphs

Branching conversations as node graphs, played by a **Dialogue Runner** that hands each line and
its choices to whatever view the game has (docs/specs/dialogue-graphs.md). Agents:
`jasper://docs/dialogue`.

## The asset

A dialogue is a `.dialogue` file: a graph of kind `dialogue` (the generic graph model, so its
unknown keys survive, its edits are undoable ops and it hot-reloads). Make one with Project window
**Create > Dialogue** (an entry `start` wired to an empty line) or
`edit_graph {path, create: {kind: "dialogue"}}`. Its metadata holds `namespace` (its String Table
namespace, from the file name, kept on rename) and `source_language`; its GUID is in the file.
Open it in **Window > Dialogue**.

| Node | Parameters | Outputs |
|---|---|---|
| Entry | `name`, `arguments` (the message arguments `start` passes, for the lint) | `next` |
| Line | `speaker`, `text`, `variants` `[{id, text}]`, `variant_mode` (`random`, `sequence`, `once`), `mood`, `tags`, `voice`, `options` `[{id, text, condition, action, style, once, when_false}]`, `action`, `max_chars` | `next`, one per option (`o1`, `o2` ...) |
| Branch | `condition` | `true`, `false` |
| Set | `assign`: `$gold = $gold - 10; $met = true` | `next` |
| Action | `action` (handler calls separated by `;`), `wait` | `next` |
| Jump | `dialogue` (another file, or empty for this one), `entry` (an entry's name or a node id) | none |
| End | `result` (what `onEnd` gets) | none |

An unconnected output ends the conversation. A line's `next` is followed by `advance()`, and also
when every option is hidden. Option and variant ids are never reused (`next_sub_id`), so wires and
translations survive reorders. A line's `action` runs when it is shown.

## Expressions

Conditions, `set` nodes and actions use one small language, compiled once when the dialogue loads:

- literals (`3`, `"text"`, `true`, `false`), variables `$gold`, message arguments `{bribe}`;
- `not`, `and`, `or`, `==` `!=` `<` `<=` `>` `>=`, `+ - * /`, parentheses;
- `visited("n12")`, `visits("n12")`, `random()`, `chance(0.25)`;
- handler calls `quest.done("intro")`, the shorthand `quest.done:intro` (one string argument), or
  `quest.done` with none;
- in `set` and action fields only: `$x = e`, `$x += e`, `$x -= e` and calls, separated by `;`.

Types are dynamic: comparing different types is false, arithmetic on a non-number stops the
expression (logged once per node; a condition is then false). A parse error names its column.

## The runner

```lua
local runner = DialogueRunner(self.scene, self.entity)
runner.onLine = function(r, line) MyView.show(line.speakerName, line.text, line.options) end
runner.onEnd = function(r, result) MyView.hide() end
runner:start("greet", {player = PlayerName})
```

`start(entry, args)` runs entry, set, branch, action and jump nodes until a line (`onLine`, state
`line`) or the end. `choose(i)` picks among `line.options` (1-based, after their conditions:
`hide` options are left out, `disable` ones come with `enabled = false`, a `once` option chosen
before is gone); `advance()` goes on from a line without enabled options; `stop()` ends without a
result. After an end node, an unconnected output or `stop`, `onEnd(runner, result)` fires and the
state is `ended`. More than 256 nodes without a line in one step is a `dialogue-loop` error that
stops the runner.

A line is `{node, key, text, speaker, speakerName, mood, tags, voice, variant, options}`, each option
`{index, id, key, text, enabled, style, tags}`. Empty text with options is a choice menu.

The **Dialogue Runner** component (Add Component > Gameplay): Dialogue, Start Entry, Auto Start
(starts on Play or when instantiated, after every script's `init` of that frame, so the handlers
those scripts register exist), Seed (0: from the GameObject's id) and View. Its Inspector shows,
in Play, the state, the current node and Advance and option buttons. It has no update: it acts only
in calls and events.

### Variants

`random` draws from the runner's own stream (never the same variant twice in a row), `sequence`
cycles by visit count, `once` shows each once then the last. The stream is seeded and integer-only:
the same seed and choices give the same transcript on every platform, and `run_dialogue` predicts
Play.

## Handlers

Game systems plug in by prefix, game-wide:

```lua
Dialogue.onCondition("shop", function(runner, verb, args)
    if verb == "canAfford" then return Dialogue.variables.get("gold") >= args[1] end
end)
Dialogue.onAction("shop", function(runner, verb, args)
    if verb == "open" then ShopUI.open(args[1]); return "wait" end   -- runner:resume() when the shop closes
end)
```

One handler per prefix (a second replaces the first, with a warning); `Dialogue.removeHandler(prefix)`.
An unregistered prefix warns once per prefix per run, skips the action and makes the condition
false. Handlers run on the main thread, in the order the actions are written, and are cleared at
Stop. Project Settings > Dialogue's Action and Condition Prefixes are for the editor's completion
and lint only.

## Variables and saving

`$name` reads the game-wide store `Dialogue.variables` (`get`, `set`, `onChanged`). A dialogue's own
variables (the graph's) and Project Settings > Dialogue > Variables give defaults, written on the
first read. Visits are counted per `<dialogue GUID>#<node>`. `Dialogue.variables.toTable()` and
`fromTable(t)` are the store with its visits; `resetVisits()` forgets the visits. Editor Play
starts with an empty store.

To keep them in a save slot ([saved-data.md](saved-data.md)), one call each way:

```lua
Dialogue.variables.save("slot1")                -- SaveGame.write of toTable() as namespace "dialogue"
local ok, info = Dialogue.variables.load("slot1")   -- SaveGame.read of it, then fromTable
```

Both take SaveGame's options (`namespace`, `onComplete`, `meta`...) and return what SaveGame
returns; the slot's other namespaces are untouched, and the slot's migrations run as for any read.
`load` changes nothing when it fails (`not_found`, or `corrupt` for a namespace that is not a
dialogue state) and fires `onChanged` for what it changes. Nothing uses SaveGame until you call
them. From C++: `dialogue::SaveHooks::save(slot)` and `load(slot)` (no Lua migrations), or
`capture()` / `restore(text)` for the state as JSON text.

## Localisation

Every line, variant and option text is an entry of the dialogue's own String Table: keys
`<ns>.<node>`, `<ns>.<node>.<variant>`, `<ns>.<node>.<option>`. The source text stays in the node;
translations live in `<file>.<lang>.translation` beside it (`edit_string_table {path:
"<file>.dialogue"}`, the Localization Tables window). Editing a source text turns its
translations `needs_review` in the same undo step. Texts are ICU messages: `{gold}` reads `$gold`,
`start` and `runner:setArg(name, value)` arguments win over variables. A speaker shows the text of
`speakers.<speaker>` when a String Table of namespace `speakers` has it. An export cooks the
dialogue's texts like any table (`l10n/<ns>.<lang>.strbin`).

## Views

The runner emits events; any UI can show them. The `dialogue` HTML UI template
(`create_ui_document {template: "dialogue"}`) is one ready view: it plays its GameObject's runner
(adding one with a demo conversation when there is none), types each line, shows the choices and
plays a line's voice through `Audio`.

## Hot reload

Editing a `.dialogue` (or its translations) while playing recompiles it: a running conversation
moves to the new version at once while its current node exists (a shown line whose text or options
changed is sent to `onLine` again), else it stops with a warning naming the node. A language
change sends the current line again too.

## The editor

Window > Dialogue draws each node as a card (a line's speaker, text in the window's Language,
mood and tags, a badge per translation: complete, missing, review). Its toolbar has Language,
Entry and Play; the Preview pane plays the dialogue without Play (handler calls never run:
Conditions sets each prefix true, false or ask; Variables sets the start values; Back takes a step
back), and the canvas marks the walked path. The Inspector edits a line's fields and options
(each edit one undo step); the Problems strip shows the lint with fixes (remove an extra wire,
declare a variable, declare a prefix).

The lint (`validate_graph`): `no-entry`, `duplicate-entry`, `fan-out`, `bad-expression`,
`jump-target`, `bad-message` (errors); `unreachable`, `no-exit`, `unknown-action-prefix`,
`unknown-condition-prefix`, `unknown-variable`, `unknown-argument`, `empty-line` (warnings);
`duplicate-option-text` (info).

## Agents

`run_dialogue {path, choices, conditions, variables}` walks a dialogue headless;
`run_dialogue {path, explore: true}` walks every path and reports endings, no-exit loops and lines
never shown; `run_dialogue {entity}` drives a runner in Play.

## Builds

The module `dialogue` (`JASPERENGINE_DIALOGUE`) is in every prebuilt player. An export that has no
`.dialogue`, no Dialogue Runner and no script naming `Dialogue` leaves it out and ships no
`.dialogue` file; without it the Lua names exist and log once that the module is not compiled.
An export ships each `.dialogue` cooked, at the same path: its compiled program as bytes
(`dialogue::Program::cook`), which the game reads without parsing JSON or compiling (a 500-node
dialogue in well under a millisecond). A file that does not load ships as it is, so the game logs
the editor's error. A game still reads a JSON `.dialogue`, so earlier exports keep working.
