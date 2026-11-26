# Dialogue Graphs: a `.dialogue` graph kind, a DialogueRunner with prefix-dispatched actions and conditions, localised lines, a Dialogue window and headless walks for agents

Sibling of `game-ui-localization.md`: every line and option text is an entry of a String Table
whose owner is the `.dialogue` file (that spec's R2, D3, D5). Builds on S23's graph data model and
S90's Graph Editor. Every `file:line` of section 3 was read at `97d450661`; nothing was built or
run for this spec, so budgets are targets the implementer measures, and behaviour no test covers yet
is marked *(inferred)*.

## 1. Goal

Writers and agents author branching conversations as node graphs in the editor, with lines,
text variants, choices with conditions and actions, branches, variables and jumps, and the game
plays them through a **DialogueRunner** that hands each line and its choices to whatever UI the game
has. Game systems plug in by registering action and condition handlers under a key prefix
(`quest.`, `shop.`, `tutorial.`), so a dialogue can start a quest, check an inventory or open a shop
without the dialogue system knowing about any of them. Text goes through localisation keys from the
first line, conversations hot-reload while playing, and an agent can walk or exhaustively explore a
dialogue headless and lint it for dead ends, unreachable nodes and unknown handlers. It fits RPGs,
adventures, visual novels, shopkeepers and tutorials alike.

## 2. Scope

**In scope**
- Graph kind `dialogue` (`.dialogue`, registered in core so players load it) with node types
  `entry`, `line` (speaker, text, variants, mood, tags, voice, options), `branch`, `set`, `action`,
  `jump`, `end`; a small expression language for conditions and assignments; kind validation (lint).
- Runtime (`engine/core/dialogue/`, player module `JASPERENGINE_DIALOGUE`): load and compile a
  graph, `DialogueRunner` component and Lua handle, global handler registries by prefix, a variable
  and visit store with save/restore hooks, seeded variant choice, waiting actions, hot reload in Play.
- Localisation: line, variant and option texts are table entries with stable keys; speakers' display
  names through a table; message arguments from variables.
- Editor: Window > Dialogue (a Graph Editor kind window with node bodies showing text, an Inspector
  for nodes and options, a Preview pane that plays the dialogue with editable variables and condition
  overrides), Create > Dialogue, Project Settings > Dialogue (declared prefixes and global variables),
  the DialogueRunner Inspector, the `dialogue` HTML UI template driven by a runner.
- MCP: `run_dialogue` (group `graphs`), the kind's lint through `validate_graph`, edits through
  `edit_graph`, texts and translations through `edit_string_table`; docs and the agent guide.

**Out of scope (decided)**
- A scripting language for dialogue (Yarn, Ink syntax import/export). The graph and the expression
  language are the format; an importer can come later as a separate spec.
- Lip sync, voice-over recording pipelines, per-language voice clips (localised assets are out of
  scope in `game-ui-localization.md`); a line carries one optional voice asset.
- Camera direction, character animation or "barks" systems: lines carry `mood` and `tags` that game
  code maps to animations; the engine interprets neither.
- A mandatory UI. The runner emits events; the `dialogue` template is one ready view, not a
  requirement.
- Multiplayer session management (who is talking to whom). A runner is a component; a server may run
  one per player and send `{key, args}` per `game-ui-localization.md` D9.
- Visual-scripting gameplay graphs (README do-not-copy list keeps OpenEngine's prototype out; this
  kind is narrative flow only, its logic limited to the expression language and handlers).

## 3. Current state

**The graph data model names dialogue but no kind exists.**
- `engine/core/graph/Graph.h:7` describes the generic model as "for dataflow tools (shader, VFX,
  dialogue, tooling graphs)". It has nodes with typed ports, dynamic ports stored per node, edges,
  variables (`name, type, defaultValue, exposed, group`, `Graph.h:80-87`), groups, metadata, a
  never-reused id counter (`nextId`, `:107`), deterministic save, and undoable ops whose inverse
  restores the file byte for byte (`:189-211`). Unknown keys, node types and kinds survive a save.
- `GraphKindDesc` (`engine/core/graph/GraphRegistry.h:61-76`) gives a kind its node types, port
  types, version and migrations, `acyclic`, `strictParams` and a `validate` hook; `ParamDesc` kinds
  include `string`, `enum`, `asset` and `any` (`:25-36`); `NodeTypeDesc::dynamicPorts` computes a
  node's ports from its params (`:46-47`).
- Registered kinds: `noise` (`engine/core/noise/NoiseNodes.cpp:271`, registered once in
  `NoiseCompiler.cpp:1010`) and the editor-only procedural material
  (`editor/procmat/ProceduralMaterialKind.cpp:410`, `ProceduralMaterialRegistration.cpp:32`), plus
  test kinds (`editor/mcp/tools/GraphTools.cpp:42-85`). No dialogue kind, node or runner exists
  (searched `dialog` in `engine/` and `editor/`: only `Graph.h`, the HTML UI example and an unrelated
  audio test).
- Cycle detection runs only for `acyclic` kinds (`engine/core/graph/Graph.cpp:673-704`); a
  conversation loops back to a hub, so the dialogue kind is not acyclic.
- A runtime graph precedent exists: Lua `NoiseGraph.load(path)` reads the JSON through `FileData`
  and `graph::load`, then validates (`engine/core/script/binding/NoiseClassesLua.cpp:310-338`);
  `.noisegraph` files ship only when the `noise` module is used
  (`editor/Exporter.cpp:4326-4331`).

**Editing infrastructure exists.**
- Five MCP tools, group `graphs`: `list_graph_kinds`, `list_graph_node_types`, `get_graph`,
  `validate_graph` (generic codes plus "the kind's own"), `edit_graph` (ops as one undo step,
  refused when they add errors, `create {kind}`) (`GraphTools.cpp:415-480`).
- Window > Graph Editor edits any kind; a kind's own window subclasses `GraphEditorWindow` and adds
  node thumbnails, a preview pane, toolbar extras and problem rows (contract at
  `editor/window/GraphEditorWindow.h:4-40`); `graphEditorHooks().kindWindows` routes a kind's files to
  its window (`editor/graph/GraphEditorHooks.h:26-30`). Edits are `GraphEditCmd` file snapshots
  (`editor/command/type/GraphEditCmd.h:21-26`); `CommandGroup` makes several commands one step
  (`editor/command/CommandGroup.h:15-35`).
- Graph files get the editor's own writes and external changes through hot reload
  (`editor/reload/handlers/Graphs.cpp:4-32`; `editor/tests/reload/HotReloadServiceTest.cpp:296-330`).

**Dialogue today is a Lua table in an HTML UI example.** `create_ui_document {template:
"dialogue"}` instantiates `docs/mcp/guide/html-ui-examples/dialogue/` (`HtmlUiExamplesTest.cpp:232`):
`DialogueUI.lua` hard-codes the conversation as a `CONVERSATION` table (`:14-24`) with `say(speaker,
text, choices)`, typing at 40 characters per second of game time; golden
`engine/tests/htmlui/golden/examples/dialogue.png`. Its texts are English literals.

**No runtime expression evaluator.** The only expression parser is the editor's numeric field
parser (`editor/util/NumericExpression.h:45`), not linked into players.

**Asset types** end at `TitleArt = 34` (`engine/core/asset/AssetType.h`); siblings claim values too.

## 4. Reference

| Source | Pattern studied | Take / improve |
|---|---|---|
| Craftsman `src/city_npc_dialog_server.cpp` (header comment, `Session`, `DState`) | A node = text (one of its variants at random), mood, gesture, options; each option has `next`, `action`, `cond`, `style`; actions and conditions are dispatched by **key prefix** to handlers other systems register (`onAction("job.", ...)`, `onCondition`); a session per player holds node, args and enabled options; text resolved per player language | Take: prefix dispatch, options with next/condition/action/style, variants, mood/tags, a session object. Improve: per-variant stable keys, seeded choice instead of a global RNG, conditions as expressions over variables plus handlers |
| Craftsman `src/city_jobs_bind_server.cpp:172-190` | Another module registers `job.` handlers and parses `verb:arg` | Take: `prefix.verb:arg` shorthand and handlers receiving verb + args |
| Craftsman `tools/city/npc/*.py` → `city_npc_content.inc` | Content authored in Python, compiled into the binary | Anti-pattern: no editor authoring, no hot reload, a rebuild per typo. Dialogues here are assets loaded at runtime |
| Yarn Spinner for Unity (docs only, MIT) | `DialogueRunner`, line and options views, commands and functions registered from code, line IDs for localisation, visited() | Names: **DialogueRunner**, `visited`. No syntax, no code |
| Ink (docs only, MIT) | Once-only versus sticky choices, visit counts, variables observed from the game | Take the `once` option flag and visit counts as concepts |
| Unity | No built-in dialogue system; Graph Toolkit and UI Toolkit are the closest | Unity-like naming for the component and Inspector rows |

**IP rule.** The Craftsman tree is a port reconstructed from a retail binary. Ideas only: no code,
data, dialogue content, assets or game-specific mechanics (pedestrians, jobs, police) are copied;
every feature here is genre-agnostic. Yarn Spinner and Ink are cited for names and concepts only.

## 5. Requirements

### Functional: the asset and its nodes

- **R1 Kind.** `dialogue`, extension `.dialogue`, `kindVersion 1`, not acyclic, `strictParams`,
  registered by core at start (always in the editor; in players with the module). Port types:
  `flow` (outputs fan out to at most one edge, checked by lint R11; inputs `multi`).
  `metadata.namespace`: the String Table namespace of its texts (S87 rule; made from the file stem
  at creation, kept on rename); `metadata.source_language` (default: project default).
- **R2 Nodes** (params; ports):
  - `entry {name}` → `next`. Names unique in the file; `start(name)` begins there.
  - `line {speaker, text, variants, variant_mode, mood, tags, voice, options, action}` → `next` (also
    the fallback when every option is hidden), and one `o<id>` output per option (dynamic ports).
    `variants`: `[{id, text}]`; `variant_mode`: `random` (default) | `sequence` | `once` (each
    variant once, then the last); `options`: `[{id, text, condition, action, style, once,
    when_false}]` with `when_false` `hide` (default) | `disable`; `style` is a free tag for the view
    (`""`, `accept`, `danger`, `exit`, …); `action` on the line runs when it is shown; `voice` an
    audio asset or Sound Event (S37).
  - `branch {condition}` → `true`, `false`.
  - `set {assign}`: `"$gold = $gold - 10; $met = true"` → `next`.
  - `action {action, wait}` → `next`. `action` is one or more handler calls separated by `;`.
  - `jump {dialogue?, entry}` (another file by asset reference, or this one) → none.
  - `end {result}` → none; `result` reaches `onEnd`.
  An unconnected `next`, option or `false` output ends the conversation (no node needed). Option and
  variant ids are per node (`o1`, `v1`, …), never reused (the node keeps `next_sub_id`).
- **R3 Keys.** Texts are entries of the dialogue's own String Table: line `<ns>.<node>`, variant
  `<ns>.<node>.<vid>`, option `<ns>.<node>.<oid>`, so translations survive edits, reordering and
  moving nodes. The source text stays in the node params (`game-ui-localization.md` D3); translations
  live in `<file>.<lang>.translation`. Speakers' display names: key `speakers.<speaker>` when a table
  of namespace `speakers` has it, else the speaker as written.
- **R4 Expressions** (conditions, `set`, option conditions; compiled once at load to a postfix
  program):
  - literals (numbers, `"strings"`, `true`, `false`), variables `$name`, `not`, `and`, `or`, `==`
    `!=` `<` `<=` `>` `>=`, `+ - * /`, parentheses;
  - built-ins `visited("n12")`, `visits("n12")`, `random()` (the runner's stream),
    `chance(0.25)`;
  - handler calls `prefix.verb(arg, …)` and the shorthand `prefix.verb:arg` (one string argument);
  - assignments `$name = expr`, `+=`, `-=`, separated by `;` (only in `set`).
  A parse error names the column; types are dynamic (bool, number, string); comparing different
  types is false, arithmetic on a non-number is an evaluation error (logged once per node, the
  condition is false).
- **R5 Variables.** `$name` reads the runner's store: the game-wide `Dialogue.variables` (bool,
  number, string). Graph `variables` (S23's model) and Project Settings > Dialogue's declared
  variables give defaults written to the store on first read when absent. Visit counts are kept per
  `<dialogue guid>#<node id>`. The store has `get`, `set`, `onChanged(name, value)`, and
  `toTable()`/`fromTable(t)` for saving (`savegame.md` calls them).
- **R6 Message arguments.** Line and option texts are ICU messages (`game-ui-localization.md` R3):
  `{gold}` reads `$gold`; `runner:start(entry, args)` adds per-conversation arguments that win over
  variables; a handler can set more with `runner:setArg(name, value)`.

### Functional: the runner

- **R7 Flow.** `start(entry?)` (default `start`, else the entry with the lowest id) runs `set`,
  `branch`, `action` and `jump` nodes without presenting until it reaches a line or the end; a line
  fires `onLine(runner, line)` and waits: `advance()` follows `next` (or the fallback), `choose(i)`
  (1-based, among the presented options) follows the option, running its action first. `stop()`
  ends without `result`. After `end`, an unconnected output or `stop`, `onEnd(runner, result)` fires
  and `isRunning` is false. A runner runs at most 256 non-presenting nodes per step; more is an
  error (`dialogue-loop`, the node logged) and stops it.
- **R8 Line payload** (Lua table / C++ struct): `node`, `key`, `text` (formatted in the game
  language), `speaker`, `speakerName`, `mood`, `tags`, `voice`, `variant` (id or nil), and `options`:
  `[{index, id, key, text, enabled, style, tags}]` after conditions, `hide` options removed, `once`
  options already chosen removed. Empty `text` with options = a choice menu without a line.
- **R9 Handlers.** `Dialogue.onAction(prefix, fn(runner, verb, args))` and
  `Dialogue.onCondition(prefix, fn(runner, verb, args) -> value)` register game-wide handlers (one
  per prefix; a second registration replaces and warns). `args` is the list of evaluated arguments.
  An action returning `"wait"` pauses the runner (`state == "waiting"`) until `runner:resume()`; an
  unknown prefix warns once per prefix per run, skips the action and makes the condition false.
  Handlers run on the main thread, in the order the actions are written.
- **R10 Variants.** `random` draws from the runner's splitmix64 stream (seed: component `seed`,
  0 = from the entity id; never the same variant twice in a row when there are 2+); `sequence`
  cycles by visit count; `once` shows each once, then the last. Same seed and choices →
  identical transcripts on every platform.
- **R11 Lint** (the kind's `validate`, so `validate_graph`, the window's Problems strip and the
  export check all show it): `no-entry` (Error), `duplicate-entry` (Error), `fan-out` (a flow output
  with 2+ edges, Error), `bad-expression` (Error, column), `jump-target` (missing file or entry,
  Error), `unreachable` (no path from any entry, counting jumps from other dialogues of the project,
  Warning), `no-exit` (a set of nodes from which no end is reachable and no option leads out,
  Warning), `unknown-action-prefix` / `unknown-condition-prefix` (when Project Settings declare
  prefixes and this one is not declared, Warning), `unknown-variable` (not declared in the graph or
  project, Warning), `unknown-argument` (`{x}` in a text that no variable or declared entry argument
  names, Warning), `empty-line` (no text, no options, Warning), `bad-message` (ICU parse error,
  Error), `duplicate-option-text` (Info). Fixes for the window: remove edge (fan-out), declare
  variable, declare prefix.
- **R12 Hot reload in Play.** A changed `.dialogue` (or its translations) recompiles; a running
  runner switches at its next step when its current node still exists (a presented line is re-sent
  with `onLine` when its text or options changed), else stops with a warning naming the node.
- **R13 Component.** `DialogueRunner` (Add Component > Gameplay > Dialogue Runner): `dialogue`
  (asset), `startEntry` (`""` = default), `autoStart` (false; starts on Play/instantiate), `seed`
  (0), `view` (a GameObject with a UI Document; `""` = none). Events: `onStart`, `onLine`, `onEnd`,
  `onVariableChanged` (the store's, filtered to this runner's dialogue). Read-only in Play:
  `isRunning`, `state` (`idle` | `line` | `waiting` | `ended`), `currentNode`.
- **R14 Default view.** The `dialogue` HTML UI template's `DialogueUI.lua` becomes a view of a
  runner (the component on its GameObject, or its `runner` property): `onLine` drives `say`, a
  choice calls `choose`, Space/click calls `advance`, `voice` plays through `Audio`; its demo
  conversation moves into `assets/dialogue/demo.dialogue` with an `en` table. The first frame is
  identical to today's (golden `examples/dialogue.png` unchanged).

### Platform & backends

- **R15** CPU only; every export target; single-threaded web unchanged; JSON loaded through
  `FileData` like Noise Graphs. Module `JASPERENGINE_DIALOGUE` (default ON in the editor and
  prebuilt variants; S34's detector marks it used when the project has a `.dialogue`, a
  DialogueRunner or a script naming `Dialogue`); `.dialogue` files ship only when the module is used
  (the `shipsForNoise` pattern). Module off: `Dialogue` and `DialogueRunner` Lua names exist and log
  once that the module is not compiled.

### Performance (release; desktop / mobile / web as `game-ui-localization.md` §5)

- **R16 Load**: a 500-node dialogue (2,000 edges, 1,500 option and variant texts) parsed and
  compiled ≤ 3 / 10 / 12 ms; memory ≤ 160 bytes per node plus source texts (translations are in the
  localisation tables).
- **R17 Step**: `advance` through 16 non-presenting nodes to a line with 4 options (2 conditions
  calling a Lua handler) ≤ 0.04 / 0.15 / 0.2 ms excluding the Lua handlers' bodies and the view;
  0 heap allocations in the C++ step besides the payload strings (`DialogueBench.StepAllocations`).
- **R18 Zero cost**: a scene without DialogueRunners adds no per-frame work (the runner has no
  update; it acts only in calls); no registry allocation until the first `onAction`/`onCondition`.

### Editor UX (human)

- **R19 Window > Dialogue** (`window.dialogue`; `.dialogue` files open here from the Project window,
  `focus_window` and `openGraph`): the Graph Editor canvas with dialogue nodes drawn as cards (speaker,
  the text in the window's language, first 3 lines, option rows each with its output port, mood and
  tag chips, a translation badge per shipped language: complete / missing / review), entries and
  ends coloured; toolbar extras: **Language** (any table language or the source), **Entry**,
  **Play** (Preview from the entry).
- **R20 Preview pane** (`hasPreviewPane`): plays the dialogue in the editor without Play: current
  line with speaker and text, option buttons (disabled ones greyed with their condition), Advance,
  Restart, Back (undo the last step of the preview), a **Variables** table (editable values, the
  store's defaults), **Conditions** overrides per unknown or handler prefix (true / false / ask),
  actions listed as they would fire (never run); the canvas highlights the current node and the
  visited path.
- **R21 Inspector** for the selected node (`drawSelection`): Speaker (with the project's speaker
  list), Text (multi-line, ICU checked, character count against `max_chars` when set), Variants
  (list with Add/Remove/drag order, Variant Mode), Mood, Tags, Voice (asset field), Options (list:
  Text, Condition, Action, Style, Once, When False, each with drag reorder that keeps ids and wires),
  Line Action; `branch`/`set`/`action` show their expression fields with completion of variables,
  built-ins and declared prefixes. Each field edit is one undo step; a text edit is one
  `CommandGroup` over the `.dialogue` and, when the source text changed and translations exist, their
  `needs_review` marks.
- **R22 Create.** Project window Create > Dialogue (namespace from the name, an `entry start` and an
  empty `line` connected).
- **R23 Project Settings > Dialogue**: **Action Prefixes** and **Condition Prefixes** (name,
  description, argument hint), **Variables** (name, type, default, description), **Speakers** (id,
  display key); all one undo step each, used for completion and lint.
- **R24 DialogueRunner Inspector**: Dialogue (asset field with Open), Start Entry (dropdown of the
  file's entries), Auto Start, Seed, View; in Play: State, Current Node (click selects it in the
  Dialogue window), and **Advance** / option buttons for manual testing.

### Agent UX (MCP, Lua, CLI)

- **R25 `run_dialogue`** (group `graphs`, ≤ 1,500 characters, read-only in edit mode):
  - `path` (a `.dialogue`): headless in the editor process, no Play needed; `entry`, `choices`
    (1-based picks applied in order at each option menu; `advance` is implied for lines without
    options), `variables` (initial values), `conditions` (`{"quest.done": true}` or `{"quest.*":
    false}` for handler calls; unlisted ones are `false` and reported), `language`, `seed`,
    `max_steps` (200);
  - `explore: true`: every reachable path breadth-first, each unknown condition both ways, up to
    `max_paths` (64): returns node coverage, endings reached with the choices leading there, `no-exit`
    loops found, lines never shown;
  - `entity` (a GameObject with a DialogueRunner) in Play: drives the live runner (`start`,
    `choices`, `advance` n) with the real handlers;
  - result: `transcript` (`[{node, speaker, text, key, options, actions, assigned}]`), `state`
    (`line` | `waiting` | `ended`, the current node, the presented options), `variables` after the
    run, unknown handler calls, and `next` hints (e.g. "conditions quest.done was assumed false; pass
    conditions to take the other branch").
- **R26 Through existing tools**: create and edit with `edit_graph {create: {kind: "dialogue"}}` and
  ops (line and option texts set as params; the tool's handler writes the translation `needs_review`
  marks as R21 does); lint with `validate_graph`; node types with `list_graph_node_types {kind:
  "dialogue"}`; translations with `edit_string_table {path: "<file>.dialogue"}`; runtime state with
  `get_runtime_entity` (DialogueRunner `state`, `currentNode`, `isRunning`).
- **R27 Lua** (`engine/core/script/binding/DialogueClassesLua.cpp`, stubs
  `engine/core/script/binding/stubs/Dialogue.lua`): `Dialogue.onAction`, `onCondition`,
  `removeHandler(prefix)`, `Dialogue.variables` (`get`, `set`, `onChanged`, `toTable`, `fromTable`,
  `resetVisits()`), `DialogueRunner(scene, entity)` with `start(entry?, args?)`, `advance()`,
  `choose(i)`, `resume()`, `stop()`, `setArg(name, value)`, `dialogue` (r/w asset path), `state`,
  `isRunning`, `currentNode`, `currentLine` (the last payload), events `onStart`, `onLine`, `onEnd`,
  `onVariableChanged` (cleared at Stop, `tools/check_event_clears.py`).
- **R28 Docs.** `docs/dialogue.md` (nodes, expressions, runner, handlers, variables and saving,
  localisation keys, views, hot reload), `docs/mcp/guide/dialogue.md` (recipes: write a branching
  conversation with `edit_graph`; walk it with `run_dialogue`; register a handler; translate it),
  `docs/mcp/guide/html-ui.md` example section (the runner-driven template), `lua-api.md` lines,
  `TOOLS.md` regenerated, one "Other areas" index line in the guide.
- **R29 Eval.** `tools/mcp/eval/cases.py`: `guard_dialogue_graph_not_lua_table` (asked for an NPC
  conversation, the agent creates a `.dialogue` with `edit_graph` instead of a Lua table) and
  `guard_dialogue_run_explore` (asked whether every ending is reachable, it calls `run_dialogue
  {explore: true}` instead of reasoning over `get_graph`).

### Data & compatibility

- **R30** `.dialogue` is a graph file (`formatVersion 1`, kind `dialogue`, `kindVersion 1`): the
  model's unknown-key preservation and migrations apply. GUID in-file: a top-level `guid` the model
  keeps as an unknown key, as S90's `.procmat` does (`editor/procmat/ProceduralMaterialKind.cpp:454`).
  `AssetType::Dialogue` (38 at the time of writing, after the String Table value; an
  explicit value, the next free one at merge); Lua name `dialogue`.
- **R31** New component, new project settings (`dialogue.action_prefixes`,
  `dialogue.condition_prefixes`, `dialogue.variables`, `dialogue.speakers`, all default empty): no
  version bump, no migration.
- **R32** The `dialogue` template change keeps old instantiated copies working (they are project
  files with their own Lua table); only new instantiations get the runner version.

## 6. Design constraints & decisions

- **D1 A graph kind, not a script format.** Reuses S23/S90's model, ops, undo, canvas, MCP tools,
  hot reload and validation; content is an asset that ships as data, never compiled into code (the
  Craftsman `.inc` anti-pattern).
- **D2 Options live on the line.** One card = what the player sees at once (Craftsman's node shape),
  so a 3-choice beat is one node, not four; dynamic ports keyed by option id keep wires through
  reorders. `branch` covers logic between lines.
- **D3 Prefix dispatch, game-wide.** Handlers are registered per prefix by the systems that own
  them (quests, shops, tutorials), not per runner, so any dialogue can use any system; the runner is
  passed to the handler so it can read its GameObject. Declared prefixes in Project Settings are for
  editor completion and lint only; the runtime never needs them.
- **D4 Expressions are tiny and safe.** No Lua inside dialogue files: a file cannot run arbitrary
  code, lint can check it, and the headless walker can evaluate it with overrides. Anything richer is
  a handler.
- **D5 Source text in the node.** Writers and agents read and edit real text in the graph (`get_graph`
  shows it); keys are derived from never-reused ids (R3) and translations live beside the file
  (`game-ui-localization.md` D3, D5: the cook writes the dialogue's source and translations to
  `l10n/<ns>.<lang>.strbin` like any table, and the runner looks texts up through `Localization`).
- **D6 Runtime layer.** `engine/core/dialogue/`: `DialogueKind` (registration, shared by editor and
  players), `DialogueProgram` (compiled nodes, postfix expressions, string ids), `DialogueRunner`
  (state machine), `DialogueVariables`, `DialogueHandlers`; component
  `engine/core/component/DialogueRunnerComponent.h`; subsystem only for hot reload notification
  (no per-frame system). Editor: `editor/dialogue/` (window, Inspector rows, preview session, walker
  shared with `run_dialogue`).
- **D7 Determinism.** One splitmix64 stream per runner (`seed`), consumed only by `random`
  variants, `random()` and `chance()`; the headless walker and the live runner share the program and
  the step function, so `run_dialogue` predicts Play.

```cpp
// engine/core/dialogue/DialogueRunner.h
namespace jasperengine::dialogue {
    using Value = std::variant<std::monostate, bool, double, std::string>;
    struct Option { int index = 0; std::string id, key, text, style; std::vector<std::string> tags; bool enabled = true; };
    struct Line {
        std::string node, key, text, speaker, speakerName, mood, variant;
        std::vector<std::string> tags; AssetRef voice; std::vector<Option> options;
    };
    enum class RunnerState { Idle, Line, Waiting, Ended };
    using ActionFn = std::function<bool /*wait*/(Runner&, std::string_view verb, std::span<const Value> args)>;
    using ConditionFn = std::function<Value(Runner&, std::string_view verb, std::span<const Value> args)>;

    class JASPERENGINE_API Handlers {          // game-wide, main thread
    public:
        static void onAction(const std::string& prefix, ActionFn fn);
        static void onCondition(const std::string& prefix, ConditionFn fn);
        static void remove(const std::string& prefix);
        static void clear();                   // at Stop
    };

    class JASPERENGINE_API Runner {
    public:
        bool setDialogue(std::shared_ptr<const Program> program);
        bool start(std::string_view entry = {}, const LocalizationArgs& args = {});
        bool advance();
        bool choose(int index);                // 1-based among presented options
        bool resume();
        void stop();
        void setArg(std::string name, LocalizationValue value);
        RunnerState getState() const;
        const Line* getCurrentLine() const;
        void setSeed(uint64_t seed);
        // the walker and run_dialogue: condition overrides instead of handlers
        void setConditionOverride(std::function<std::optional<Value>(std::string_view call)> fn);
        FunctionSubscribe<void(Runner&)> onStart;
        FunctionSubscribe<void(Runner&, const Line&)> onLine;
        FunctionSubscribe<void(Runner&, const std::string& result)> onEnd;
    };
}
```

```lua
-- a shop system registers once
Dialogue.onCondition("shop", function(runner, verb, args)
    if verb == "canAfford" then return Inventory.gold >= args[1] end
end)
Dialogue.onAction("shop", function(runner, verb, args)
    if verb == "open" then ShopUI.open(args[1]); return "wait" end   -- resume() when the shop closes
end)
-- an NPC starts its conversation
local runner = DialogueRunner(self.scene, self.entity)
runner.onLine = function(r, line) MyView.show(line.speakerName, line.text, line.options) end
runner:start("greet", { player = PlayerName })
```

Node in the file (S23 model; texts are source text, keys derived):

```json
{"id": "n7", "type": "line", "position": [420, 80], "params": {
   "speaker": "guard", "text": "Halt! {player}, the gate is closed after dark.", "mood": "stern",
   "variants": [{"id": "v1", "text": "You again? The gate stays shut."}], "variant_mode": "sequence",
   "options": [
     {"id": "o1", "text": "Here, take {bribe} coins.", "condition": "shop.canAfford({bribe})",
      "action": "shop.pay({bribe}); $guard_bribed = true", "style": "accept", "when_false": "disable"},
     {"id": "o2", "text": "I'll come back tomorrow.", "style": "exit"}],
   "next_sub_id": 3}}
```

Option `action` strings may mix handler calls and assignments (one parser, R4); `{bribe}` inside
an expression is the argument value, inside a text it is an ICU argument.

MCP sketch (`run_dialogue`, group `graphs`): `{path?, entity?, entry?, choices?: [int], advance?:
int, variables?: object, conditions?: object, language?: string, seed?: int, explore?: bool,
max_steps?: int (1..2000), max_paths?: int (1..512)}`; exactly one of `path` and `entity` (the
handler checks; error names the fix).

## 7. Acceptance criteria

- **AC1 Expressions.** `engine/tests/core/dialogue/DialogueExpressionTest.cpp`:
  `DialogueExpression.EvaluatesPrecedenceAndTypes`, `ShorthandAndCallFormsAreEqual`,
  `ParseErrorsNameTheColumn`, `AssignmentsOnlyInSet`, `MalformedInputNeverCrashes` (1,000 mutated
  expressions from a seed under ASan). Red: no parser.
- **AC2 Flow.** `engine/tests/core/dialogue/DialogueRunnerTest.cpp`: `WalksEntryLinesBranchesSetsJumpsEnd`
  (a fixture covering every node type, transcript compared with a committed expectation),
  `UnconnectedOutputEnds`, `FallbackNextWhenAllOptionsHidden`, `LoopGuardStopsAt256`,
  `JumpAcrossFiles`.
- **AC3 Handlers.** `DialogueRunnerTest.ActionsAndConditionsDispatchByPrefix`,
  `WaitPausesUntilResume`, `UnknownPrefixWarnsOnceAndIsFalse`, `HandlersClearedAtStop`.
- **AC4 Variants and options.** `DialogueRunnerTest.RandomVariantsAreSeededAndNeverRepeat`
  (10,000 steps, two seeds, identical transcripts per seed across runs; red: no seeding),
  `SequenceAndOnceModes`, `OnceOptionsDisappear`, `WhenFalseDisableKeepsTheOptionDisabled`.
- **AC5 Variables.** `DialogueVariablesTest.DefaultsFillOnFirstRead`, `VisitCountsPerNode`,
  `ToTableFromTableRoundTrip`, `OnChangedFires`.
- **AC6 Lint.** `engine/tests/core/dialogue/DialogueKindTest.cpp`: one fixture per R11 code asserts
  exactly that code at that node (`NoExitLoopIsReported`, `UnreachableCountsJumpsFromOtherFiles`,
  `FanOutIsAnError`, `UnknownPrefixOnlyWhenPrefixesDeclared`, …); `KeysAreStableAcrossEdits`
  (reorder options, move nodes, delete a variant: other keys unchanged).
- **AC7 Localised lines.** `DialogueRunnerTest.LinesUseTranslationsAndArguments` (de translation
  shown, `{player}` from start args, `{gold}` from variables, plural) and
  `SourceEditMarksTranslationsForReview` (editor test `editor/tests/dialogue/DialogueEditTest.cpp`,
  one undo step restores both files byte for byte).
- **AC8 Lua.** `engine/tests/core/script/DialogueLuaTest.cpp`: `DialogueLua.ApiMatchesStubs`,
  `RunnerEventsAndChoose`, `WaitReturnValuePauses`.
- **AC9 Hot reload.** `editor/tests/reload/HotReloadServiceTest.cpp`:
  `DialogueEditInPlayKeepsTheRunner` (text of the presented line edited → `onLine` re-sent; its node
  deleted → runner stops with the warning).
- **AC10 Editor.** `editor/tests/dialogue/DialogueWindowTest.cpp`: `OpensDialogueFilesInItsWindow`,
  `InspectorFieldEditIsOneUndoStep`, `OptionReorderKeepsIdsAndWires`, `PreviewWalksWithOverrides`
  (the preview never calls a handler), `ProblemsStripFixDeclaresVariable`.
- **AC11 MCP.** `tools/mcp/tests/test_dialogue.py` (`CI = True`, own project):
  `test_create_and_edit_with_edit_graph`, `test_validate_graph_dialogue_lint` (seeded defects each
  reported once), `test_run_dialogue_walk_choices_and_conditions`,
  `test_run_dialogue_explore_finds_endings_and_dead_loops` (red: tool missing),
  `test_run_dialogue_live_runner_in_play` (a Lua handler logs; `read_console` sees it),
  `test_dialogue_template_runs_a_runner` (`create_ui_document {template: "dialogue"}`, Play,
  `send_input` Space, `inspect_ui` shows the second line).
- **AC12 Golden.** `HtmlUiExamplesTest` and the htmlui golden `examples/dialogue.png` unchanged by
  the template change (R14).
- **AC13 Performance.** `engine/tests/core/dialogue/DialogueBench.cpp`: `DialogueBench.Load500`,
  `Step`, `StepAllocations` (`[bench]` lines; budgets R16-R17 through `perfSlack()`; allocations
  counted with `ThreadAllocationCount` on Linux).
- **AC14 Module.** A core build with `JASPERENGINE_DIALOGUE=OFF` (preset added to
  `docs/testing.md`'s Build options) compiles and `DialogueStubTest.LogsOnceWhenNotCompiled` passes;
  `ModuleUsageTest.DetectsDialogue`; an export without dialogues ships no `.dialogue` file.
- **AC15 Shipped game.** `test_dialogue.py::test_export_runs_dialogue` (`JASPER_SLOW_BUILD=1`):
  prebuilt Linux export of the template project runs headless with a script that advances twice and
  prints the transcript, equal to `run_dialogue`'s for the same seed (D7).
- **AC16 Docs, budget, eval.** `gen_tool_docs.py --check`; `tool_budget.py --check` (`graphs` group
  within defaults with `run_dialogue`); R29 cases fail with the new text removed;
  `tools/check_event_clears.py` passes.

## 8. Dependencies & conflicts

- **Depends on** `game-ui-localization.md` (owner tables, keys, ICU, `Localization` lookup,
  cooking; implement its P1 runtime first, or land dialogue with source-text lookup and switch when
  it lands), S23 (graph model), S90 (Graph Editor window base, problems strip), S37 (Sound Event
  voice), S34 (module detection), S17 (template list), S31 (MCP conventions).
- **Used by** `savegame.md` (`Dialogue.variables:toTable/fromTable` and visit counts in save
  slots), `cancellable-prioritised-events.md` (runner events may adopt its subscription priority;
  the API above uses plain `FunctionSubscribe` until then), `input-device-awareness.md` (the default
  view's prompts), `text-entry-gamepad-cursor-back-button.md` (gamepad navigation of option buttons
  and Back = advance), `project-test-runner.md` (tests can call the walker), `dev-console-for-agents.md`
  (a `dialogue` console command can start or advance a runner).
- **Merge hotspots**: `engine/core/asset/AssetType.h` and `editor/asset/AssetTypes.cpp` (values
  claimed by siblings), `editor/mcp/tools/GraphTools.cpp` (new tool beside the graph tools),
  `editor/graph/GraphEditorHooks.h` (kind window), `editor/shortcuts/DefaultCommands.cpp` (Window
  menu; `docs/editor/keyboard-shortcuts.md` regenerated), `editor/players/ModuleUsage.h`,
  `editor/Exporter.cpp` (ships-for-module), the HTML UI example files and template list,
  `engine/CMakeLists.txt` (module option), Lua binding registration.

## 9. Risks & defaults

- *Large conversations on one canvas get unreadable.* Default: S23 groups as chapters, entries as
  bookmarks in the toolbar's Entry menu, Frame Selected; `jump` across files for long games.
- *Expressions grow into a language.* Default: R4 is closed; requests beyond it become handlers.
- *Handler registration order versus `autoStart`.* A runner that starts before a system registers
  its prefix sees unknown prefixes. Default: `autoStart` runs after every script's `init` of that
  frame (end of the first update), documented.
- *Translations orphaned by node deletion.* Deleting a node leaves its keys in translation files;
  the Localization Tables window lists them as "unused" with a Remove action; export drops unused
  keys from cooked files.
- *Writers want inline text with markup (bold, colours).* Default: plain text plus `mood`/`tags`;
  the view styles whole lines; inline markup is a later decision for the whole localisation system.
- *Edit-mode walker cannot know handler results.* Default: unlisted conditions are false and listed
  in the result, `explore` tries both; the live mode uses real handlers.

## Deviations

- **D-1 Assignments in actions (R4).** Assignments are statements: they are allowed in `set` and in
  every action field (a line's or option's `action`, an action node), as the option example of §6
  mixes them (`shop.pay({bribe}); $guard_bribed = true`), and refused only in conditions (branch,
  option conditions). `AssignmentsOnlyInSet` checks the refusal in conditions.
- **D-2 Runner events (D6 sketch).** The core `dialogue::Runner` has plain callbacks (`onStart`,
  `onLine`, `onEnd`, `onVariableChanged`), so the headless walker owns them; the `FunctionSubscribe`
  events Lua and C++ scripts subscribe to are the Dialogue Runner component's (listed in
  `ComponentsWithEvents`, cleared at Stop like every component event). `Dialogue.variables.onChanged`
  is a static `FunctionSubscribe` cleared by `Engine::clearAllSubscriptions`.
- **D-3 Entry arguments and jump targets (R2, R11).** `entry` has an `arguments` parameter (the names
  `start` passes, for `unknown-argument`). A jump's `entry` may name an entry or a node id, so a jump
  from another file into the middle of a dialogue is what makes a node reachable without a path from
  its entries (`UnreachableCountsJumpsFromOtherFiles`). A line also has `max_chars` (R21's count).
- **D-4 The template's demo (R14).** The demo conversation ships with the template's other assets, so
  `create_ui_document` puts it at `<stem>_assets/demo.dialogue` beside the document (not
  `assets/dialogue/`), its namespace rewritten to `<stem>_demo` so two documents from the template do
  not collide; its English texts are the dialogue's own table. The view plays its GameObject's Dialogue
  Runner (adding one with the demo when the GameObject has none); there is no separate `runner`
  script property. On the last line the "continue" marker shows (the runner cannot tell that `next` is
  unconnected); the first frame and the golden are as before.
- **D-5 Hot reload timing (R12).** A running runner moves to the reloaded program when the reload
  happens (the editor applies file changes between frames), which is its next step in practice.
- **D-6 A line's action and "wait".** A line's `action` runs when it is shown; a handler returning
  "wait" there is ignored (the line waits for advance or a choice anyway).
- **D-7 Speakers (R3, R23).** At runtime a speaker's name is `speakers.<speaker>` when a table has
  it; Project Settings > Dialogue > Speakers' display key is for the editor (completion), the runtime
  never reads project settings but the variables' defaults (`je-dialogue.json`, written by the export
  when variables are declared).
- **D-8 Explore and no-exit (R11, R25).** `run_dialogue {explore}` tells menus apart by node,
  variables and the condition values assumed on the way (visit counts are left out, so a loop that
  only counts visits is a loop); the lint's `no-exit` reports one warning per group of trapped
  nodes at its lowest id, entries left out.
- **D-9 Graph model (S23).** `setParam` on a node of a dynamic-port type now keeps its stored ports in
  step with the params they are computed from (and its inverse carries the old ports back exactly), so
  an option added by `edit_graph` gets its port. `GraphEditorWindow` gained two small hooks for a
  kind's window: `drawKindNodeInspector` and `Problem::fixAction` (a fix that is no graph edit).
- **D-10 Bench file (AC13).** The benchmarks are `engine/tests/core/dialogue/DialogueBenchTest.cpp`
  (the core target globs `*Test.cpp`), suite `DialogueBench`.
- **D-11 Add Component.** There was no Gameplay category: it is added with the Dialogue Runner.
- **D-12 Module off (AC14).** `DialogueStubTest` binds the stubs on a Lua state of its own, so it runs
  in every core build; the module-off engine builds with `tools/check-physics-off.sh dialogue-off`.
- **D-13 Reordering in the Inspector (R21).** Variants and options reorder with Up and Down buttons
  (each one undo step; ids and wires kept) rather than by dragging the rows.
- **D-14 Create > Dialogue (R22).** The item sits in the Project window's Create menu directly (not in
  the catalog's `assets.create.*` commands).
- **D-15 Load budget (R16, AC13).** A game loads the dialogue the export cooks in place of its JSON,
  at the same path: the compiled program as bytes (`Program::cook`), which `Programs::load` reads
  through `Program::fromCooked` without parsing or compiling (every string reference, node index and
  range checked, and each expression's stack simulated, so a damaged file is refused, never run;
  `DialogueRunnerTest.CookedFilePlaysAsItsSource`, `CookedFileRefusesDamage`). On box A
  (RelWithDebInfo, `JASPER_TEST_PERF_SLACK=1`, six runs) the 500-node dialogue loads in
  0.23-0.28 ms, inside the 3 ms desktop budget; the editor keeps the JSON, whose parse into the
  generic graph model (`graph::load` on nlohmann JSON, 370 KB of text) and compile take about 43 ms
  and 5 ms there. `DialogueBench.Load500` prints both and enforces R16 on the cooked load. A cooked
  file names its format version (a game refuses another one: "export the game again"), and a game
  still reads a JSON `.dialogue`, so earlier exports keep working; a file that does not compile ships
  as it is, so the game logs the editor's error. A compiled program holds 134 bytes per node (its
  record and its share of strings and expressions) plus 52 bytes per option and 16 per variant
  record, beside the source texts; the test checks the 160-byte node budget on the former.
- **D-16 Saving in one call (R5, savegame.md).** Beside `toTable()`/`fromTable(t)`,
  `Dialogue.variables.save(slot, options?)` and `load(slot, options?)` write and read the store as
  the SaveGame namespace `dialogue` (through `SaveGame.write`/`read`, so migrations, callbacks and
  errors are SaveGame's); C++ has `dialogue::SaveHooks::save/load`. `fromTable` now refuses a table
  with neither `variables` nor `visits`, so loading another namespace cannot empty the store.
- **D-17 run_dialogue's explore sentence (R29).** `run_dialogue`'s description no longer says what
  `explore: true` reports: `guard_dialogue_run_explore`, sharpened to ask for lines nobody sees with
  `validate_graph` and `get_graph` in reach, passed 4/4 without the sentence (the parameter's name
  decides), so R29's "fail with the new text removed" cannot hold and AGENTS.md cuts the text. The
  case stays as a guard; `docs/dialogue.md` still documents `explore`.
