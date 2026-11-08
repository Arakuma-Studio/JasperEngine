# MCP tool conventions, groups and budget

Every MCP tool of the editor follows these rules. They keep the tool list usable as it grows
(140 tools before the OpenEngine-port specs, roughly 240 after): an agent pays for every listed
tool definition in every session, and it can only use what it can understand from the name,
description and schema. Gates in `tools/mcp/tests/` enforce the rules marked *(gated)*.

This page is stable: specs add tools against it. A change to a rule goes through S31's owner
and updates this page and the gates together.

## Checklist for a new tool

1. **Prefer extending a tool.** A parameter on an existing tool beats a new tool.
2. **Name** it `verb_noun` in snake_case: `get_`, `list_`, `set_`, `create_`, `delete_`, and
   `edit_<thing>` for a tool that runs several operations. Names are unique. *(gated)*
3. **Group** it: set `ToolDef::group` (or the group of its module row in `buildToolRegistry`,
   `editor/mcp/tools/McpTools.cpp`) to your spec's group. Only `core` tools are always listed.
   *(gated)*
4. **Schema** from the `McpSchema` builders only, snake_case keys, closed objects. *(gated)*
5. **Annotations and activity** accurate; `allowedWhilePlaying` decided on purpose.
6. **Result**: a text summary plus `structuredContent`; longer than 1 s becomes a job; images
   as `ToolResult` images.
7. **Budget**: the tool fits its limit and its group fits `tools/mcp/budget.json`. *(gated)*
8. **Test** it in `tools/mcp/tests/` (and assert its group is hidden by default and listed after
   `tool_groups` enables it), then regenerate `docs/mcp/TOOLS.md`. *(gated)*
9. **Guidance** for agents goes in `docs/mcp/guide/<area>.md`; the agent guide gets at most a
   line in its area's digest ([The agent guide](#the-agent-guide)). *(gated)*

## Names and parameters

- Tool names: snake_case `verb_noun`, globally unique (`create_entity`, `list_shaders`,
  `edit_colliders`). The names registered before this rule keep their names
  (`PREDATING_NAMES` in `test_tool_conventions.py`), and so do upstream's absorbed tools
  (`UPSTREAM_NAMES`: `audio_mixer` and its sibling `audio_session`, `native_plugins`, `user_settings`); every new name starts with a verb of its
  `VERBS` list. Add a verb there only when none of the listed ones fits.
- Parameter keys: snake_case at every level (`job_id`, `undo_group`, `max_width`,
  `min_frame_ms`, `capture_id`). No camelCase, ever, including keys of nested objects.
- References use the shared shapes: a scene is `sceneRef`, a GameObject `entityRef`, a list of
  them `entities`; their conventions are explained once in the server instructions
  (Addressing), so their schemas carry no description unless they differ from it (for example
  "Default: the only terrain.").

## Schemas

Build schemas with `editor/mcp/McpSchema.h` only:

| Value | Builder |
|---|---|
| object with declared keys | `object({...}, {required...})`: `additionalProperties: false` |
| free-form object (a property map) | `anyObject(desc)`: `additionalProperties: true` |
| closed set of strings | `stringEnum(desc, {...})` |
| number / integer with a range | `number(desc, min, max)`, `integer(desc, min, max)` |
| scene / GameObject / list of GameObjects | `sceneRef()`, `entityRef()`, `entities()` |
| vectors, colours, rotations | `vec2/vec3/vec4`, `color`, `quat` |
| a default | `withDefault(schema, value)` (the handler applies it) |

The keyword subset is `type` (a JSON type or a list of them), `enum`, `required`,
`properties`, `additionalProperties` (a boolean), `items`, `minItems`, `maxItems`, `minimum`,
`maximum`, `anyOf`, plus the annotations `description` and `default`. Nothing else: no
`oneOf`, `const`, `pattern`, `$ref`, `format`. The CLI (`jasperengine-editor call`) converts
and validates arguments against exactly this subset, and `test_tool_conventions.py` fails on
any other keyword.

Objects: an object with declared `properties` states `additionalProperties: false` (or `true`
for a free-form object whose properties only name its common keys, `schema::anyObject` plus
`properties`). The top level is closed by the registry, which rejects unknown arguments
listing the valid ones and leaves `additionalProperties` out of the listing to save
characters.

A schema must not depend on project state: the CLI builds the registry offline
(`buildToolRegistry`, no `App`) and `test_call_cli.py` compares it with the live listing.
Put state-dependent choices in the description or in the handler's error message.

## Descriptions and results

- A description says what the tool does, what it returns and the common mistakes, in at most
  400 characters (review rule, not gated). Shared conventions (addressing, value shapes, undo,
  `parent` and `max_width` defaults) go in the server instructions (`McpInstructions.cpp`) or
  in `docs/mcp/guide/*.md`, never repeated per tool.
- A result has a text summary (`ToolResult::text`) and `structuredContent` with the data. A hint the agent must
  see goes in `next` in `structuredContent` on a success and in the text on an error: a client like Claude Code shows the
  model only that part of each.
- Work that can take longer than about 1 s runs as an `McpJobs` job: the call returns
  `job_id` and `status` (`running|succeeded|failed|cancelled`), polled with `get_job`.
- Images are `ToolResult` images (base64 in MCP, files in the CLI), never base64 in text.
- Annotations (`readOnly`, `destructive`, `idempotent`) and `activity` (the busy policy) are
  accurate; `allowedWhilePlaying` is set on purpose for every tool that edits.

## Tool groups

`ToolDef::group` names a group of `editor/mcp/McpToolGroups.cpp` (empty = `core`). tools/list
returns `core` plus the groups the session enabled; hiding is only about context:
**`tools/call` and `batch` reach every registered tool**, listed or not, so a client that
misses `notifications/tools/list_changed` can still call a group tool by name.

| Group | Specs |
|---|---|
| `core` | the tools before S31 (but the terrain ones), plus `tool_groups` |
| `assets_pipeline` | S04, S05, S14 |
| `data_safety` | S03 (`get_load_report`, `list_scene_backups`, `discard_scene_backup`) |
| `diagnostics` | S06 (`get_job_system_stats`, `set_job_system_config`, `get_memory_stats`), S07, S09 (`get_log_config`, `set_log_level`, `wait_for_log`, `list_timers`), S28 (`get_editor_health`, the one tool answered off the main thread: `ToolDef::offMainThread`; `get_crash_reports`, `dismiss_crash_notice`, `set_hang_watchdog`, `debug_crash`) |
| `editor_ux` | S24 (`get_undo_history`, `get_play_changes`, `apply_play_changes`, `discard_play_changes`), S08 (`get_inspector_layout`), S25 (`search_editor`, `run_command`, `list_shortcuts`, `set_shortcut`, `edit_layout_presets`, `edit_asset_labels`, `edit_saved_searches`, `edit_bookmarks`), S26, `find_ui_item` (UI tests), upstream's ui-scaling-text / safe-area `configure_game_view` (out of core to fit the budget) |
| `profiling` | S15 |
| `device` | S56 (`get_device_info`, `set_device_simulation`; the tier table through `set_project_settings`) |
| `rendering_fx` | S11 (`get_material_variant`, `set_light_unit`), S12, S13 |
| `meshes` | S53 (model import and cook, render budgets), S54 (`capture_model_lods`) |
| `splines` | S18 |
| `terrain` | S31: `sculpt_terrain`, `set_terrain_maps`, `sample_terrain` (out of core to fit the budget) |
| `voxels` | T6 |
| `navigation` | S19 |
| `particles` | S20, upstream's `import_unity_particles` (out of core to fit the budget) |
| `animation` | S21 |
| `gameplay` | S22 |
| `physics` | upstream's physics-3d: `create_ragdoll`, `edit_physics_material` (out of core to fit the budget) |
| `audio` | upstream's audio-mixer: `audio_mixer` (out of core to fit the budget) |
| `editor_tools` | S27, S66 (`clear_project_leftovers`) |
| `vcs` | S29 |
| `html_ui` | S17 (`edit_ui_view`) |
| `scripting` | S23 (`reload_scripts`, `get_script_status`, `set_script_settings`, `list_editor_menus`, `run_editor_menu`, `eval_expression`, the graph tools), upstream's `user_settings` (PlayerPrefs) |
| `shipping` | S30, upstream's native-bridge `native_plugins` (out of core to fit the budget) |

A spec may add a group: one row in `McpToolGroups.cpp` (name, one-line description), and a
`budget.json` `groups` entry only if it needs other limits than the defaults. Registering a tool
into a group the table lacks aborts debug builds and fails `test_tool_conventions.py`.

Choosing groups:

- the `tool_groups {enable?: [...], disable?: [...]}` core tool, per session; it returns every
  group with its tools, size and state, and sends `notifications/tools/list_changed` (stdio at
  once; HTTP inside the call's SSE response when the client accepts `text/event-stream`,
  otherwise the result says to re-list);
- `jasperengine-editor mcp --tool-groups a,b|all` for the sessions of that editor (an unknown
  name exits 2);
- the settings key `mcp.tool_groups` (`a,b`, `all`, or a list), the default for every session
  when the flag is absent;
- `tools/list` with `params._meta["jasper/groups"]` set to `"all"` or a list lists those
  groups instead of the session's: the CLI, the test harness (`Editor.tools()` lists every
  group by default), `gen_tool_docs.py` and `tool_budget.py --check` do this.

Each tool carries `_meta: {"jasper/group", "jasper/module"}` in tools/list. The module (the
docs section) comes from the module table of `buildToolRegistry`.

### Adding a module or a tool to a group

```cpp
// editor/mcp/tools/McpTools.h
void registerProfilingTools(McpRegistry& registry);

// editor/mcp/tools/McpTools.cpp, TOOL_MODULES (order = tools/list order)
{registerProfilingTools, "Profiling and render diagnostics", "profiling"},

// a tool of a core module that belongs to a group sets it itself
tool.group = "profiling";
```

The test of the group asserts (as `test_tool_groups.py` does for `selftest`):

```python
default = {t.name for t in await ed.tools(groups=None)}
ed.check("get_frame_stats" not in default, "hidden until profiling is enabled")
await ed.call("tool_groups", enable=["profiling"])
ed.check("get_frame_stats" in {t.name for t in await ed.tools(groups=None)}, "listed once enabled")
```

## Budget

`tools/mcp/budget.json` is the single source of the limits. A tool's size is
`len(name) + len(description) + len(compact JSON of its listed inputSchema)` in characters
(`tool_budget.py`, `McpRegistry::toolChars`).

| Limit | Value |
|---|---|
| default listing (`core`) | ≤ 99,000 characters and ≤ 150 tools |
| each other group | ≤ 15,000 characters and ≤ 12 tools (`group_default`), unless `groups` has an entry |
| each tool | ≤ 1,500 characters (`tool_default`), unless `exceptions` gives `{limit, reason}` |

- The tools over 1,500 characters when S31 set the limits are grandfathered at their size then
  plus 5 %; the HTML UI specs' caps are `exceptions` entries too (lower than the default).
- New tools of S03–S30 go into a non-core group. A spec that moves a tool into core trims core
  by the same amount and records it in `core_trades`
  (`{"tool", "spec", "chars", "trimmed": "what core gave up"}`); the default listing must still
  fit.
- A spec's own budget ("S15: ≤ 7,000 characters for its tools") is its group's `groups` entry.
- Gate: `tool_budget.py --check tools/mcp/budget.json` exits 1 naming every violation (also a
  stale `exceptions` or `groups` entry); `test_tool_budget.py` runs it, with a red control.

```sh
uv run --no-project --with-requirements tools/mcp/requirements.txt python tools/mcp/tool_budget.py --groups all --top 20
uv run --no-project --with-requirements tools/mcp/requirements.txt python tools/mcp/tool_budget.py --check tools/mcp/budget.json
```

## The agent guide

The guide (`editor/mcp/McpInstructions.cpp`, sent as `initialize.instructions` on every session
and served as `jasper://guide`) is paid for by every session, like the default listing, so it
has a budget too: the `guide` entry of `tools/mcp/budget.json`.

| Limit | Value |
|---|---|
| the whole guide, with its generated Tool groups section | ≤ 8,192 bytes (UTF-8) |
| headroom: what the whole guide must leave free under that limit, for later specs' groups and lines | ≥ 1,500 bytes (`headroom_bytes`), so the guide itself stays ≤ 6,692 |
| each `## ` section (the text before the first one counts as one) | ≤ 1,500 bytes (`section_bytes`), unless `sections` gives `{limit, reason}` |

- The guide holds only what an agent must know before it picks a tool, whatever it works on:
  Unity names, addressing, the loop, undo, code, how to find tools and groups and read the docs.
  An area gets a short **digest** section only for the rules agents broke without it (Lua, C++,
  physics, prefabs, shaders, HTML UI); every other area is one entry of the "Other areas" index
  (its key tool and `jasper://docs/<area>`). A guide line needs a case of `tools/mcp/eval` that
  fails without it (`tools/mcp/eval/README.md`).
- **The client cut.** Clients may keep only the start of the instructions: Claude Code keeps the
  first 4,096 characters and appends "… [truncated]" (`CLAUDE_CODE_MAX_MCP_DESCRIPTION_LENGTH`).
  So the guide puts first what an agent needs before any call (the intro, Addressing, The loop,
  Building scenes and the generated Tool groups line), and the intro tells an agent that cannot see
  the last section ("## Other areas") to read the whole guide with `read_file jasper://guide`
  (`read_file` reads any `jasper://` resource, for clients without MCP resources too). The
  `client_cut` entry of the budget lists those sections; the gate fails when one ends after the
  cut (counted in UTF-16 code units, as JavaScript counts) or the intro lacks the pointer.
- An area's detail lives in `docs/mcp/guide/<area>.md`. A file there is embedded at build time
  and served as `jasper://docs/<area>` (`docs/mcp/guide/README.md` lists them); long ones split
  into `### ` section resources.
- A spec that adds guidance writes or extends its area's doc and, if agents need it up front,
  adds a line to its area's digest (or one new digest section). When the guide or a section
  outgrows its limit, or the guide eats into the headroom, move detail from the digest into
  the doc (every moved fact must already be there, or go there in the same change); never
  raise a limit, shrink the headroom or trim other areas' digests to make room. A tool or parameter name in the guide must exist
  (`test_autosync.py`).
- Gate: `tool_budget.py --check tools/mcp/budget.json` also measures the guide, prints the bytes
  left, and names the section over its limit or the headroom missing (`test_tool_budget.py`,
  with rule tests on made-up guides).

## Docs and test coverage

- `docs/mcp/TOOLS.md` is generated: `tools/mcp/gen_tool_docs.py` (every group, sections from
  `_meta["jasper/module"]`, the group per tool, the editor version in the header).
  `gen_tool_docs.py --check` exits 1 when the file is stale (the version in the header aside);
  `test_docs.py` runs it, with a red control. Regenerate after any tool change.
- Every registered tool appears quoted (`"tool_name"`) in some `tools/mcp/tests/*.py`, or in
  `tools/mcp/tool_coverage_exempt.txt` with a reason (`test_tool_coverage.py`).
- `test_tool_conventions.py` checks names, keys, keywords, closed objects, annotations and
  `_meta` on the live listing of every group.

## The test-only group

`JASPER_MCP_SELFTEST_GROUP=1` registers the group `selftest` with one tool, `selftest_echo`,
for the tests of groups, coverage and budget; `=<n>` (n > 1) pads its description to n
characters (the budget gate's red control). It never exists otherwise.
