# S31 — Agent tooling: `call` CLI, MCP tool conventions, contributor AGENTS.md/CLAUDE.md

Wave 3 for the CLI. **The conventions part (§6.4–6.6: tool groups, budget gate, docs/coverage
gates) and the contributor docs land in wave 1, with S02**, because S03–S30 all add MCP tools and
must follow them from their first commit. Owner area: `editor/cli/` (new `CallCommand.*`,
`McpClient.*`), `editor/mcp/McpRegistry.*`, `editor/mcp/McpServer.cpp`, `tools/mcp/`, repo root
`AGENTS.md`, `CLAUDE.md`, `.claude/rules/`.

## 1. Goal

Any MCP tool of the editor becomes callable from a shell: `jasperengine-editor call <tool>
[--flags]`, with flags, `--help` and validation derived from the tool's own JSON Schema, stable
exit codes and a machine-readable envelope. CI scripts, hooks, humans and agents without an MCP
client then get the full API. Separately, agents working on the engine itself get a repo-level
`AGENTS.md`/`CLAUDE.md` that says how to build, test, run an isolated editor and land a change.
Every other spec adds tools, so S31 also fixes the conventions (naming, schemas, tool groups,
character budget, generated docs, test coverage) that keep the tool list usable as it grows from
140 to roughly 240 tools.

## 2. Scope

In scope:
- Subcommands `call`, `batch`, `list-tools` and `editors` of the editor binary.
- Connection discovery for running editors (an instance registry), attach over our Streamable
  HTTP with token, and a headless stdio spawn when no editor serves the project.
- The registry offline: building the tool list without starting the editor.
- Tool groups (lazy listing), `tools/list_changed`, and the budget, docs and coverage gates.
- The contributor docs: root `AGENTS.md`, root `CLAUDE.md`, path-scoped `.claude/rules/*.md`,
  plus a drift check for them.

Out of scope:
- Their external Node MCP server, raw TCP debug socket, `ge raw` and `list_debug_methods`
  (09-ai-mcp-cli.md §6). Every capability here is a typed tool.
- The Node SEA binary. Our CLI is the editor binary itself.
- An approval gate, permission modes, agent undo labels and an in-editor assistant panel
  (09 §5 items 3, 4 and 10). They are not specced here. S24 owns undo names.
- Per-project `AGENTS.md`/`CLAUDE.md` content (`Generator::writeAgentsFile`). It stays as it
  is, apart from one line about `call` (R24).
- Mobile and web. The CLI runs on Linux, Windows and macOS, where the editor runs.

## 3. Current state

- **Subcommands.** `editor/main.cpp:20-70` dispatches `export`, `shaders`, `scene-dump`,
  `check`, `ui-support`, `ui-effects`, `import-ui`, `benchmark` and `mcp` before
  `Backend::init`, then `--mcp-port N`. Any other argument goes to
  `CommandLine::runHelpCommand` (`editor/cli/CommandLine.cpp:2521`). Path arguments are made
  absolute once through `CommandLine::absoluteArgPath`/`projectArgPath`
  (`editor/cli/CommandLine.h`, `test_cli_paths.py`). **No subcommand calls an MCP tool.** The
  only generic client is the Python test harness (`tools/mcp/harness.py`).
- **Transports.**
  - stdio: `jasperengine-editor mcp --project|--create [--headless] [--http-port N]`
    (`editor/mcp/McpLaunch.h`). It starts Xvfb with `-displayfd` on Linux
    (`McpLaunch.cpp:150-195`). When stdin closes, the editor quits without prompts, so unsaved
    edits are dropped (`McpServer.cpp:245-251`).
  - HTTP (`McpTransportHttp.{h,cpp}`):
    - binds 127.0.0.1 only;
    - rejects a non-local Origin or Host, where an empty Origin is accepted
      (`isLocalOrigin`, `:317-318`; check at `:525`);
    - checks an optional bearer token read from settings at start (`:447`, check at `:534`);
    - issues `Mcp-Session-Id` at initialize;
    - takes one JSON-RPC message per POST, with no batches (`:568`);
    - answers in SSE only when the request carries a progressToken;
    - answers `GET /mcp` with 405, so it has no server-initiated stream.
  - Settings: `mcp.http_port` (default 47615) and `mcp.http_token` live in `settings.yaml`
    (`AppSettings.cpp:76,261-262`). That file sits in the working directory, else in the
    per-user config directory; `mcp` prefers the user directory when the working directory
    has no `settings.yaml` (`AppSettings.cpp:81-107`).
  - **Nothing records which editors are running or on which port.**
- **Registry.**
  - `ToolDef{name,title,description,inputSchema,outputSchema,annotations,allowedWhilePlaying,activity,handler}`
    is at `McpRegistry.h:41-51`. Its registration functions take only the registry
    (`tools/McpTools.h`), and `McpServer::registerAllTools` calls them (`McpServer.cpp:91-108`).
  - `initialize` advertises `tools.listChanged: false` (`McpServer.cpp:445`).
  - Schema builders: `McpSchema.{h,cpp}`. Refs are `type:[integer,string]`, vectors are
    fixed-length number arrays (`McpSchema.cpp:75-121`), and `object()` defaults to
    `additionalProperties:false`.
  - A result is `content[text, image…]` plus `structuredContent` plus `isError`
    (`McpCall.cpp:21-41`). Jobs return `job_id` and a `status` of
    running|succeeded|failed|cancelled (`McpJobs.cpp:12-15,120-123`).
  - The `batch` tool takes `calls[{tool, arguments}]`, at most 200, with `undo_group`
    (`CoreTools.cpp:32,394-416`).
- **Budget and docs.** There are 140 tools at about 99.8K characters, against a target of under
  ~100K (`docs/mcp/README.md:265-267`).
  - `tools/mcp/tool_budget.py` only prints sizes. The hard cap is a constant in an unrelated
    test: `TOTAL_LIMIT = 99800 + 837` (`tools/mcp/tests/test_html_ui_tools.py:56,375`).
  - `gen_tool_docs.py` groups tools by a hard-coded `MODULES` list of source files and
    already honours `_meta["jasper/module"]` (`gen_tool_docs.py:1-45`). `docs/mcp/TOOLS.md`
    says "jasperengine-editor unknown" in its header.
  - `DESIGN.md:197-206` says "target well under 150 tools" and "every tool has a test",
    but no gate enforces either.
  - Other specs already budget their own deltas: S14 ≤2,400, S15 ≤7,000 for 7 tools, S18
    ≤6,000. Together the ~28 streams will roughly double the total. **Without groups, the
    ~100K default listing cannot hold.**
  - S15 §6 uses camelCase parameters (`minFrameMs`, `captureId`), but every existing tool uses
    snake_case (`job_id`, `undo_group`, `max_width`).
- **Agent docs.** `Generator::writeAgentsFile` (`editor/Generator.cpp:1402-1519`) writes the
  per-project `AGENTS.md` once, plus a `CLAUDE.md` containing `@AGENTS.md`. It is served as
  `jasper://project/AGENTS.md` (`tools/DocsTools.cpp:492`).
  - The repository has no root `AGENTS.md`, `CLAUDE.md` or `.claude/`. Contributor knowledge
    is spread over `docs/mcp/README.md` §"Developing JasperEngine itself" (lines 155-169) and
    §"Testing the server", the `tools/build.sh` header, the `harness.py` docstring, and the
    `smoke-test.sh`/`editor-shot.sh` headers.

## 4. Reference (OpenEngine, MIT; design port, no code copied)

| File | Take | Improve / differ |
|---|---|---|
| `mcp/src/cli.ts` | One registry feeding both front-ends; global flags; `list`; `help <tool>`; `--dry-run` prints the params without calling; kebab or snake tool names | Globals must come **before** the tool name, so they never collide with a tool parameter. Not taken: their `raw` |
| `mcp/src/cli/args.ts` | Flags derived from the published JSON Schema, not from internals; typed conversion **before** validation; strict booleans (never coerce `"false"`); `a,b,c` lists; `k=v` maps; a lone `{…}` passed through | Our refs are `[integer,string]`: numeric tokens become integers and a JSON-quoted token stays a string. `@file` values. No `field:field` object-array sugar (JSON only) |
| `mcp/src/cli/output.ts` | Images written to files and never printed as base64; one-line `--json` envelope; the endpoint that answered named on stderr | Our exit codes: 4 means internal, and a timeout is 3 |
| `mcp/src/cli/raw.ts` | `batch` reads ndjson from stdin over one connection, with responses paired | `--atomic` maps onto our MCP `batch` with `undo_group` |
| `mcp/src/cli/help.ts` | Per-tool help from the schema (required, enum choices, item types) | — |
| `AGENTS.md`, `CLAUDE.md` | Structure only: a map, build, test, land; values, debugging, architecture, style, discipline, reporting; `@AGENTS.md` include; path-scoped rules | Our own text. Not taken: their workbench, AB-LOCK, Windows `C:/` rules, coordination lanes, and the "never name other engines" rule |
| `mcp/src/assistant.ts` (`servedTools`) | A served-tool subset, where an unknown name fails at startup | Named groups toggled per session, not one env list |

Licence: MIT. Nothing is copied verbatim, so `NOTICE.md` needs no entry. If an implementer does
copy code, add the notice as the README requires.

## 5. Requirements

### Functional — `call`

- **R1** `jasperengine-editor call [globals] <tool> [tool-args]`.
  - The tool name may be snake_case or kebab-case.
  - Globals: `--url U`, `--port N`, `--token T`, `--project DIR`, `--timeout S` (default 120),
    `--json`, `--structured`, `--quiet`, `--dry-run`, `--image-dir D` (default `.`) and
    `--wait-job`.
  - Everything after the tool name belongs to the tool, except `--help`/`-h`.
  - An unknown global is a usage error.
- **R2** Tool arguments are either one positional JSON object (`'{…}'`, `@file.json` or `@-`
  for stdin) or flags. Mixing the two is a usage error. A flag is `--<key>` or `--<key>=v`,
  where `<key>` is the property name with `_`→`-`; the snake_case spelling is also accepted.
  Values convert from the property schema as follows:
  - `string`: verbatim. `integer`: strict decimal (`1.5` is rejected). `number`: a finite
    float.
  - `boolean`: tri-state. Omitted means not sent. `--k` means true. `--k=V` or `--k V` takes V
    from true/false/1/0/yes/no/on/off, case-insensitive. `--no-k` means false. Any other value
    is a usage error.
  - `enum`: must be one of the values. On a mismatch the error lists them.
  - `type:[integer,string]` (refs): `^-?[0-9]+$` becomes an integer. A token starting with `"`
    is parsed as a JSON string, so `'"42"'` is the name 42. Anything else is a string.
  - Arrays of scalars: `1,2,3`, a JSON array, or repeated flags that append.
    `minItems`/`maxItems` are enforced; `--position 1,2,3` makes a `vec3`.
  - Free-form objects (`anyObject`, or `additionalProperties` not false): JSON, or repeated
    `K=V`. V is parsed as JSON when it is valid JSON, else kept as a string
    (`--values transform='{"position":[0,1,0]}'`).
  - Objects with declared properties, arrays of objects, `anyOf` and `any`: JSON only, or
    `@file`. For `any`, a token that is not JSON is sent as a string.
  - Any value accepts `@path` (file contents, as JSON or text by the rule above).
- **R3** Before sending, the CLI validates the arguments against the schema subset our builders
  emit: `type` (including arrays of types), `enum`, `required`, `properties`,
  `additionalProperties`, `items`, `minItems`, `maxItems`, `minimum`, `maximum` and `anyOf`.
  - An unknown flag fails with exit 2, a did-you-mean (edit distance ≤ 2) and the valid flags.
  - The server's own validation remains authoritative.
- **R4** `call <tool> --help` prints the following, all generated from the schema:
  - title, description and annotations (read-only/destructive/idempotent);
  - group, and whether the tool is allowed while playing;
  - one line per parameter: flag, type, required, default, enum, item shape, description;
  - two generated examples.

  `call --help` and `call` with no tool print usage and the tool names grouped by group.
- **R5** `--dry-run` resolves, converts and validates, prints
  `{"tool":…, "arguments":{…}}`, contacts no editor and exits 0.
- **R6** Output.
  - Plain mode prints the result's text blocks to stdout, with `--structured` printing
    `structuredContent` as indented JSON instead. Each image is written to
    `<image-dir>/<tool>-<n>.<ext>` (no clobbering: `-<n>` increments), and its path goes to
    stderr. Base64 is never printed.
  - `--json` prints exactly one line:
    `{"ok":bool,"exit_code":n,"code":null|"tool_error"|"usage"|"connection"|"internal","tool":s,"text":s,"structured":obj|null,"images":[{"path","mime_type","bytes"}],"error":s|null,"editor":{"url"|null,"pid","project","spawned":bool,"version"}|null}`.
  - Progress notifications (a progressToken is always sent) go to stderr unless `--quiet`.
- **R7** Exit codes: **0** ok; **1** the tool answered `isError` (refusals such as
  busy/play/dialog included); **2** usage (bad flag, value, unknown tool, ambiguous editor);
  **3** connection (refused, 401/403/404 session, timeout, spawn failure, `project_locked`);
  **4** internal (malformed JSON-RPC, protocol version mismatch, an unexpected exception).
  - On a timeout the CLI sends `notifications/cancelled` before it exits with 3.
- **R8** `--wait-job`: when `structuredContent.job_id` is present and its status is `running`,
  the CLI polls `get_job {job_id}` every 500 ms until the status is terminal and prints the
  final job.
  - The exit code is 0 for `succeeded` and 1 for `failed` or `cancelled`.
  - Ctrl-C sends `cancel_job` and then exits 3.

### Functional — `batch`, `list-tools`, `editors`

- **R9** `jasperengine-editor batch [globals] [--continue] [--atomic] < file.ndjson`.
  - One `{"tool":…,"arguments":{…}}` per line; blank lines and `#` lines are skipped.
  - All lines run in order over one session, and one R6 envelope line per call goes to
    stdout. Execution stops at the first failure unless `--continue` is given.
  - `--atomic` sends the lines as one MCP `batch` call with `undo_group:true` (at most 200
    lines, else exit 2).
  - The exit code is the highest per-call code. A malformed line is exit 2 before anything
    runs.
- **R10** `list-tools [globals] [--json] [--group G|all]` prints name, group, annotations and
  size in characters. `--json` prints the full tool definitions.
- **R11** `editors [--json]` lists live instances from the registry (R13): pid, url, project,
  version and started.

### Functional — connection

- **R12** Resolution order:
  1. `--url`;
  2. `--port` (127.0.0.1);
  3. `JASPER_MCP_URL` (shared with the harness);
  4. `--project DIR`: the live instance whose canonical project path equals DIR, else
     **spawn**;
  5. no target: exactly one live instance is used. With zero instances: exit 3 ("no running
     editor; pass --project to start one"). With more than one: exit 2, listing them.

  The CLI never attaches to an instance of a different project when `--project` is given.
- **R13** Instance registry.
  - When its HTTP transport starts, the editor writes
    `<user-config-dir>/mcp/instances/<pid>.json`:
    `{pid, url, port, project, cwd, settings_path, token_required, version, started}`. The
    write is atomic (S03), with mode 0600 on POSIX.
  - The editor deletes the file when HTTP stops or the editor quits. Readers ignore and delete
    entries whose pid is not alive.
  - The token is never stored in this file. The CLI takes it from `--token`, else
    `JASPER_MCP_TOKEN`, else `mcp.http_token` in `settings_path`.
  - The CLI sends no `Origin` header and `Host: 127.0.0.1:<port>`.
- **R14** Spawn.
  - The CLI runs its own executable as `mcp --project DIR --headless` with pipes on stdio, in
    a scratch working directory holding an empty `settings.yaml`, so it never shares or
    rewrites the user's settings.
  - It waits up to `JASPER_CALL_SPAWN_TIMEOUT` seconds (default 600; the first open compiles
    shaders) for `initialize`.
  - It closes stdin at the end, which makes the editor quit (`McpServer.cpp:245`), and kills
    only the pid it spawned, and only after 30 s.
  - If the project is locked by another editor (S03 R10 `project_locked`), it exits 3 and
    names that pid.
  - Edits are not saved implicitly. Help text and docs say so; save in the same batch.
- **R15** The CLI subcommands never initialise the backend, open a window, or write
  `settings.yaml`/`editor.log` in the working directory.

### Functional — registry offline and live

- **R16** A free function `buildToolRegistry(McpRegistry&)` (moved out of
  `McpServer::registerAllTools`) is side-effect free and needs no `App`. `--help`,
  `--dry-run` and `list-tools` without a connection target use it. When connected, the CLI
  uses the live `tools/list` (all groups, R18). If the server version differs from its own, it
  warns on stderr.

### Agent UX — tool groups and conventions (wave 1)

- **R17** `ToolDef` gains `std::string group` (empty means `core`) and `std::string module`.
  `tools/list` emits `_meta: {"jasper/group", "jasper/module"}`. A table in
  `editor/mcp/McpToolGroups.cpp` defines each group: name, one-line description and budget.
  Registering a tool with an unknown group aborts at startup in debug builds and fails the
  coverage test.
- **R18** Listing.
  - `tools/list` returns `core` plus the session's enabled groups.
  - With `params._meta["jasper/groups"]` set to `"all"` or a list, it returns those instead.
    The CLI, harness, `gen_tool_docs.py` and `tool_budget.py --groups all` use this.
  - `tools/call` and `batch` **ignore groups**: every registered tool is callable. So a client
    that misses `list_changed` can reach a group tool through `batch`.
- **R19** Enabling groups.
  - The `mcp --tool-groups a,b|all` flag and the settings key `mcp.tool_groups` (the default
    for HTTP sessions) choose the groups. An unknown name fails startup with exit 2.
  - A new core tool `tool_groups {enable?: string[], disable?: string[]}` (activity Control,
    idempotent) returns `groups[{name, description, tools, chars, enabled}]` and sends
    `notifications/tools/list_changed` to that session. On stdio the notification goes out at
    once. On HTTP it goes inside the call's SSE response when the client accepts
    `text/event-stream`; otherwise the result text says to re-list tools or use `batch`.
  - `initialize` advertises `tools.listChanged: true`.
  - `McpInstructions.cpp` gains one generated line that names the groups and `tool_groups`.
    The guide stays ≤ 16,384 bytes.
- **R20** Conventions that every spec's tools must meet. Each is gated by
  `test_tool_conventions.py`, except the description rule:
  - Tool names are snake_case `verb_noun` (`get_`/`list_`/`set_`/`create_`/`delete_`, with
    `edit_<thing>` for multi-op tools) and are globally unique.
  - Parameter keys are snake_case. **S15's camelCase parameters must be renamed** (for
    example `min_frame_ms`, `capture_id`).
  - Schemas use the `McpSchema` builders only: refs through `sceneRef`/`entityRef`/`entities`,
    vectors through `vec2/3/4`, colours through `color`, `stringEnum` for closed sets,
    `withDefault` for defaults, `additionalProperties:false`. The only keywords allowed are
    the R3 subset.
  - Annotations and `activity` are accurate, and `allowedWhilePlaying` is decided explicitly.
  - Results carry a text summary plus `structuredContent`. Operations longer than 1 s become
    `McpJobs` jobs. Images go out as `ToolResult` images.
  - Descriptions are ≤ 400 characters, and shared conventions go in the instructions or
    `docs/mcp/guide/*.md` (manual review rule, listed in the rules file).
  - Extending an existing tool with a parameter is preferred over adding a new tool.
- **R21** Budget gate. `tools/mcp/budget.json` is the single source of the limits; it
  replaces `TOTAL_LIMIT` and `LIMITS` in `test_html_ui_tools.py`. A tool's size is measured
  as in `tool_budget.py` (name + description + compact inputSchema).
  - Default listing (`core` + `tool_groups`): ≤ 100,000 characters and ≤ 150 tools.
  - Each group: ≤ 15,000 characters and ≤ 12 tools.
  - Each tool: ≤ 1,500 characters, unless an `exceptions` entry gives `{limit, reason}`.
    Today's oversize tools are grandfathered at their current size + 5%.
  - New tools of S03–S30 go into a non-core group unless their spec moves them into core
    **and** trims core by the same amount (budget.json records the trade).
  - `tool_budget.py --check budget.json` exits 1 and names every violation.
    `tools/mcp/tests/test_tool_budget.py` runs it.
- **R22** Docs gate.
  - `gen_tool_docs.py` takes the section from `_meta["jasper/module"]` (`MODULES` stays only
    as a fallback, so new specs do not edit it) and prints the group per tool.
  - `--check` exits 1 when `docs/mcp/TOOLS.md` is stale.
  - The header shows the real editor version.
- **R23** Coverage gate. `test_tool_coverage.py` fails when a registered tool name appears in no
  `tools/mcp/tests/*.py` and is not listed in `tools/mcp/tool_coverage_exempt.txt` (name and
  reason).

### Agent docs (wave 1)

- **R24** Root `AGENTS.md`, ≤ 120 lines and written in our own words. It opens with a scope
  line: "For engine contributors. Inside a game project (a directory with `project.yaml`),
  follow that project's AGENTS.md instead." Sections:
  - **Map:** `CLAUDE.md`, `.claude/rules/`, `docs/specs/`, `docs/mcp/DESIGN.md`,
    `docs/testing.md` (S02), `NOTICE.md`.
  - **What lives where:** `engine/core`, `engine/renders`, `engine/libs`, `engine/project`
    (template), `editor/` (`mcp/`, `cli/`, `htmlui/`, `window/`, `command/`), `tools/`,
    `docker/`.
  - **Build:** `tools/build.sh linux|web|android|…` in Docker, `SKIP_ENGINE=1`, `JOBS`/`CPUS`.
    Build where your brief says, never on a machine whose editor a person is using.
  - **Run an editor:** always your own, from a scratch copy of a project (never
    `build/fixture-unity` itself), started from its own working directory holding a
    `settings.yaml`. Use `mcp --headless` over stdio, or `--mcp-port <a port you checked is
    free>`. Record the pid; stop only that pid. **Never kill an editor you did not launch**, and
    never use `pkill`/`killall`. Drive it through MCP or `call`, not xdotool on a real display.
  - **Test:** gtest targets (S02), `tools/mcp/run-tests.sh -k … -j …`, `tools/smoke-test.sh`,
    `tools/editor-shot.sh`. Report declared = ran + skipped.
  - **Evidence:** a behavioural claim needs a test that was red without the change and green
    with it, and you say which run was red. Visible changes ship screenshots from an isolated
    editor. Evidence goes outside the repository.
  - **Worktrees:** one `git worktree` per change, a branch per purpose, never in another
    agent's or person's checkout.
  - **Land:** merge main and rebuild the merged tree, plus the regeneration duties
    (`gen_tool_docs.py`, `tool_budget.py --check`, `generate_lua_stubs.py`,
    `generate_api_suggestions.py` when bindings change).
  - **Adding an MCP tool:** the R20–R23 checklist in 8 lines.
- **R25** Root `CLAUDE.md`: `@AGENTS.md`, then ≤ 80 lines covering:
  - engineering values: the platform rules and do-not-copy list from `docs/specs/README.md`,
    and a cost of zero when a feature is disabled;
  - debugging instruments: `call`, `read_console`, `screenshot`, sanitizer presets (S02),
    `editor.log` and harness scratch dirs (`JASPER_KEEP_SCRATCH=1`);
  - architecture on one screen;
  - code style: C++20, 4-space indent, `jasperengine::` namespaces, the `(c) Eduardo Doria and
    contributors` / SPDX MIT header, comments that say why;
  - working discipline: label claims verified, inferred or assumed;
  - reporting format.
- **R26** `.claude/rules/` with `paths:` front matter:
  - `mcp.md` for `editor/mcp/**`, `tools/mcp/**`: the R20–R23 conventions and harness usage;
  - `rendering.md` for `engine/renders/**`, `engine/shaders/**`, `engine/shaderlib/**`,
    `engine/core/render/**`: the five backends, no compute/SSBO, goldens;
  - `scripting.md` for `engine/core/script/**`, `editor/**/Lua*`: regenerating the stubs and
    suggestions;
  - `htmlui.md` for `engine/core/htmlui/**`, `editor/htmlui/**`: the suites and the support
    tables.
- **R27** `tools/check_agent_docs.py` (plain Python, no editor) fails when any of these holds:
  - a backticked repo path in `AGENTS.md`, `CLAUDE.md` or `.claude/rules/*.md` does not
    exist;
  - a `tools/build.sh <target>` named there is not in its usage;
  - `CLAUDE.md` does not start with `@AGENTS.md`;
  - a rules file has no `paths:`.

  CI runs it (S02).
- **R28** Separation. `Generator::writeAgentsFile` output is unchanged except for one added
  sentence pointing at `jasperengine-editor call`. The repo files are hand-written and never
  generated, and nothing in the editor reads them.

### Performance

- **R29** `call --help`, `--dry-run` and `list-tools` take ≤ 300 ms wall time on the Linux
  build machine. CLI overhead for an attached call is ≤ 50 ms over the tool's own time.
  `batch` reuses one session.

## 6. Design constraints & decisions

**6.1 Files.**
- `editor/cli/CallCommand.{h,cpp}`: argv parsing, R2 conversion, help and output.
- `editor/cli/McpClient.{h,cpp}`: an HTTP/1.1 client over loopback sockets. It reuses the
  socket helpers factored out of `McpTransportHttp.cpp` and parses SSE. It also has a stdio
  child-process client: `posix_spawn` with pipes, and `CreateProcessW` with pipes on Windows.
- `editor/cli/SchemaArgs.{h,cpp}`: classification, conversion and validation (R2, R3). Pure
  functions, unit-tested.
- `editor/mcp/McpInstances.{h,cpp}`: the instance registry (R13).
- `editor/mcp/McpToolGroups.{h,cpp}`: the group table.

No new third-party dependency (no libcurl).

**6.2 API sketch.**
```cpp
namespace jasperengine::editor::cli {
  enum class ArgKind { String, Integer, Number, Boolean, Enum, Ref, ScalarArray, FreeObject, Json };
  struct ArgSpec { std::string key, flag; ArgKind kind; bool required; Json schema; };
  std::vector<ArgSpec> argSpecs(const Json& inputSchema);
  // throws UsageError{message} (exit 2)
  Json parseToolArgs(const Json& inputSchema, std::span<const std::string> tokens);
  std::vector<std::string> validate(const Json& schema, const Json& value);  // error paths
  enum Exit { Ok = 0, ToolError = 1, Usage = 2, Connection = 3, Internal = 4 };
}
namespace jasperengine::editor::mcp {
  void buildToolRegistry(McpRegistry& registry);   // no App, no I/O
  struct InstanceInfo { int pid; std::string url, project, cwd, settingsPath, version; bool tokenRequired; };
  std::vector<InstanceInfo> liveInstances();       // prunes dead entries
}
```

**6.3 Examples** (the help text and `docs/mcp/cli.md` use these):
```sh
jasperengine-editor call --project ~/games/demo create-entity --name Box --type box
jasperengine-editor call --port 47615 set_properties --entity Player/Gun --values transform='{"position":[0,1,0]}'
jasperengine-editor call --json --wait-job --port 47615 build_project '{"mode":"desktop","build_type":"Release"}'
jasperengine-editor call screenshot --view game --image-dir build/shots
printf '%s\n' '{"tool":"create_scene","arguments":{"name":"A"}}' '{"tool":"save_scene","arguments":{"path":"scenes/A"}}' \
  | jasperengine-editor batch --project ~/games/demo --atomic
```

**6.4 Groups.**
- The 140 existing tools plus `tool_groups` stay in `core`. S31 moves nothing out, so existing
  clients see no change.
- The initial groups each spec registers into:
  - S15 `profiling`;
  - S18 `splines`;
  - S19 `navigation`;
  - S20 `particles`;
  - S21 `animation`;
  - S22 `gameplay`;
  - S29 `vcs`;
  - S12/S13 `rendering_fx`;
  - S27 `editor_tools`;
  - S04/S05/S14 `assets_pipeline`;
  - S06/S07/S09/S28 `diagnostics`;
  - S24/S25/S26 `editor_ux`.
- A spec may define another group name in `McpToolGroups.cpp`.

**6.5 Listing is the only thing groups govern.** Hiding a tool saves context but never makes
it uncallable. This keeps the CLI, the harness and `batch` simple, and a test never needs
enabling first, though each spec's tests include one assertion that its group's tools are hidden by default
and listed after `tool_groups` enables them (the AC10 pattern).

**6.6 Help on the wire.** `call --help` while connected uses the live schema. A parameter that
is a component's property map (`set_properties.values`, `add_component`) also prints the
component names from `jasper://reflection` when S08 has landed; otherwise it points to
`list_component_types`.

**6.7 Spawn vs attach.** Attach is preferred because it is fast and shares state with the
human's editor. Spawn is one-shot. For many calls, start an editor with `--mcp-port` and use
`--port`, or use `batch`. There is no daemon or keep-alive mode.

## 7. Acceptance criteria

Unit tests live in `jasperengine-editor-tests` (S02), under `editor/tests/cli/`. MCP tests live
in `tools/mcp/tests/`.

| AC | Verification |
|---|---|
| AC1 (R2) | `CallArgsTests.*`: for each kind, a table of token → JSON → error. Includes `--flag=false` → false; `--no-flag`; `--flag maybe` → UsageError; `'"42"'` → "42"; `42` → 42 for a ref; `1,2` for vec3 → UsageError; `K=V` JSON or string; `@file`; JSON plus flags → UsageError. |
| AC2 (R3, R20) | `SchemaValidateTests.*` covers every keyword of the subset. `test_tool_conventions.py`: every live tool schema uses only the subset, snake_case names and keys, `additionalProperties:false` on objects with declared properties, and every tool's `_meta` group is in the group table. |
| AC3 (R4, R16) | `test_call_cli.py::test_help_offline`: `call create_entity --help` with no editor running exits 0, lists every inputSchema property of the live tool and both examples. `test_offline_registry_matches_live`: offline `list-tools --json` equals live `tools/list` (all groups) for a `--create` project, byte-for-byte per tool. |
| AC4 (R5, R15) | `test_dry_run`: prints the exact arguments, the editor's activity log shows no call, the working directory stays empty (no settings.yaml or editor.log), and the run takes ≤ 300 ms (R29). |
| AC5 (R6, R7) | `test_call_cli.py` against a harness editor attached over `--port` with a token. `create_entity` → 0 and the `--json` envelope parses with `ok:true`. `delete_entities` of a missing entity → 1, `code:"tool_error"`. Bad enum → 2. Wrong token → 3. Port with nothing listening → 3. `screenshot` → 0 with a PNG file whose path is in `images[0].path` and no base64 on stdout. A fake server returning malformed JSON-RPC (`fake_server.py --broken`) → 4. |
| AC6 (R7 timeout, R8) | `--timeout 1 wait --seconds 5` → 3, and the server logs the call as cancelled. `build_project --wait-job` on the fixture → 0 with status succeeded (marked slow: `JASPER_SLOW_BUILD=1`). Ctrl-C (SIGINT) during the job sends `cancel_job`. |
| AC7 (R9) | `test_batch_cli`: 3 lines, the second failing → 2 envelopes printed, exit 1; with `--continue` → 3 envelopes. `--atomic` → one `undo` reverts all lines. 201 lines with `--atomic` → 2. |
| AC8 (R12, R13) | `test_discovery`: two harness editors with HTTP on different free ports and different projects. `call --project A editor_status` reaches A (pid matches). With no target → 2, listing both. After editor A is SIGKILLed, its stale file is pruned by `editors`. The instance file has no token and mode 0600. Blocked `Origin: http://evil` sent by the test directly → 403 (regression). |
| AC9 (R14) | `test_spawn`: `call --project <scratch copy> get_scene_tree` with no editor running → 0. Afterwards no child process remains, the user-config `settings.yaml` mtime is unchanged, and the scratch working directory was used. With the project open in a harness editor without HTTP → 3 and the message names its pid (needs S03 R10). |
| AC10 (R17-R19) | `test_tool_groups.py`: a test-only group `selftest` registered under `JASPER_MCP_SELFTEST_GROUP=1`. Its tool is absent from the default `tools/list` and still callable directly and via `batch`. `tool_groups {enable:["selftest"]}` → a `list_changed` notification received on stdio and inside the SSE stream over HTTP, after which `tools/list` contains it. `--tool-groups nope` → exit 2. The instructions mention `tool_groups` and stay ≤ 16,384 bytes. |
| AC11 (R21) | `test_tool_budget.py` runs `tool_budget.py --check tools/mcp/budget.json`: default listing ≤ 100,000 chars and ≤ 150 tools, each group within its limit. A red control: an inflated description injected through `JASPER_MCP_SELFTEST_GROUP` makes the check exit 1 and name the tool. `test_html_ui_tools.py::test_budget` reads its limits from budget.json. |
| AC12 (R22, R23) | `gen_tool_docs.py --check` exits 0 on main, and exits 1 after a description edit without regeneration (red control in `test_docs.py`). The TOOLS.md header shows the version. `test_tool_coverage.py` exits 0, and a temporarily unexempted tool name makes it fail. |
| AC13 (R24-R27) | `tools/check_agent_docs.py` exits 0 and runs in CI; a fake path added to a scratch copy of AGENTS.md makes it exit 1. Manual check: AGENTS.md ≤ 120 lines and CLAUDE.md ≤ 80 lines after the include, every R24 section present, no text copied from OpenEngine (a reviewer diff against their AGENTS.md/CLAUDE.md shows no shared sentence longer than 8 words). |
| AC14 (R28) | `test_generators.py`: a project created by `create_project` has the generated AGENTS.md (first line `# JasperEngine Project Context`) containing `jasperengine-editor call`, and `jasper://project/AGENTS.md` returns it, not the repo file. |
| AC15 (platforms) | The Windows and macOS CI jobs (S02) run `CallArgsTests`, `SchemaValidateTests`, `call --help` and `--dry-run`. Linux runs everything above. |

## 8. Dependencies & conflicts

- **Depends on:** S01 (C++20, `std::span`); S02 (`jasperengine-editor-tests`, CI wiring,
  `docs/testing.md` linked from AGENTS.md); S03 (atomic writes for instance files,
  `editor.lock`/`project_locked` for R14 and AC9). It uses S08's `jasper://reflection` when
  present (6.6).
- **Blocks or constrains:** every spec adding tools (S03–S30) must use R17 groups and pass
  R20–R23. Their own budget ACs (S14, S15, S18) become checks against their group's
  budget.json entry. **S15 renames its camelCase parameters** (R20). S04, S06, S07, S09, S17
  and S19 reference `call`; they need nothing beyond R1–R9.
- **Merge hotspots:**
  - `editor/main.cpp` (subcommand dispatch, also S30);
  - `editor/cli/CommandLine.cpp` help (S14, S30);
  - `editor/mcp/McpServer.cpp` (`registerAllTools` → `buildToolRegistry`, capabilities; S03,
    S24);
  - `editor/mcp/tools/McpTools.h` (one line per new register function, from every spec);
  - `editor/mcp/McpRegistry.h` (`ToolDef` fields);
  - `McpInstructions.cpp`;
  - `tools/mcp/gen_tool_docs.py`;
  - `tools/mcp/tests/test_html_ui_tools.py` (limits move out);
  - `docs/mcp/README.md`.

  Land R17–R23 first, as one small commit, so later streams only append group-table rows and
  budget.json entries.

## 9. Risks & defaults

- **Client support for `list_changed` varies (biggest risk).** Default: group tools are
  always callable through `batch` (R18). `mcp --tool-groups` and `mcp.tool_groups` let a user
  pre-enable groups. The instructions name the fallback. If agents measurably fail to find
  group tools, a spec may promote a tool to core by trimming core (R21 trade).
- **The offline registry drifts from the live one** (a schema computed from project state).
  Default: AC3 compares them on an empty project, and a tool whose schema depends on project
  state must build it from compiled-in data or move that detail into the description.
- **The wave-3 slot comes too late for the conventions.** Default: the conventions (R17–R23)
  and the contributor docs (R24–R28) land in wave 1, and the CLI (R1–R16) can follow later.
- **The token lives in a same-user-readable `settings.yaml`.** This is accepted: the same-user
  boundary is unchanged, and the instance file never contains the token.
- **Spawn mode drops unsaved edits on exit.** Accepted and documented. `--atomic` batches end
  with a save in the docs' examples.
- **Repo `CLAUDE.md` is inherited by user projects under the checkout** (Claude Code walks
  parent directories). Default: the R24 scope line, plus a rule never to create test projects
  inside the repository (scratch directories only).
- **Global flags only before the tool name** may surprise users of their CLI. This is
  intentional, because it is collision-free; `call <tool> --port 1` is treated as the tool
  parameter `port` or rejected with a hint.

## Deviations (implementation)

- **One contributor manual.** R24's 120 lines and sections and R25's `CLAUDE.md` are replaced:
  `AGENTS.md` holds every repository-wide rule (at most 350 lines, DON'Ts section included) and
  `CLAUDE.md` is exactly the line `@AGENTS.md`. R26's rule files hold only what applies under their
  `paths:`. AC13's sizes follow; R27's `CLAUDE.md` check is now exact.
  `tools/check_agent_docs.py` and `test_agent_docs.py` enforce this shape.
- **No build-server rule.** R24's "build where your brief says" is gone: builds and tests run on
  whatever machine the agent is on.
- **Group limits live in `tools/mcp/budget.json` only.** R17 put a budget in the group table of
  `McpToolGroups.cpp`; R21 makes budget.json the single source of the limits. The table holds
  name and description; budget.json `groups` holds a group's limits when they differ from the
  defaults (`profiling` 7,000/7 for S15, `splines` 6,000 for S18).
- **A `data_safety` group (S03).** §6.4 names no group for S03. Its three tools
  (`get_load_report`, `list_scene_backups`, `discard_scene_backup`) went into `data_safety`, and
  the S06/S09 tools (`get_job_system_stats`, `set_job_system_config`, `get_memory_stats`,
  `get_log_config`, `set_log_level`, `wait_for_log`, `list_timers`) into `diagnostics` and the
  S24 ones (`get_undo_history`, `get_play_changes`, `apply_play_changes`,
  `discard_play_changes`) into `editor_ux`, when
  S31 merged oe-port: those streams had registered them into core and raised the limit inside
  `test_html_ui_tools.py`, which took the default listing over 100,000 characters.
- **The terrain tools moved out of core** (`sculpt_terrain`, `set_terrain_maps`,
  `sample_terrain`, group `terrain`), against §6.4's "S31 moves nothing out": after the oe-port
  merge the core listing measured 101,299 characters even without the S03/S06/S09 tools
  (S03 added `allow_data_loss` to nine tools and `recovery` to `open_scene`, S09 grew
  `read_console`, and `focus_window` gained the preferences and Console views). Trimming
  1,300+ more characters of descriptions would have cost agents information; three niche tools
  in a group cost nothing (callable by name, named in the guide's Tool groups line) and leave
  about 4,000 characters of room. `allow_data_loss` lost its per-tool description (explained
  once in the guide's Data safety section).
- **Core trimmed to fit `tool_groups`.** The 140 core tools measured 100,579 characters before
  S31. Eight long descriptions were reworded and four repeated schema descriptions (`parent`
  "Default: scene root.", `max_width` "0 = full size.", the default component and quaternion
  descriptions) moved to the instructions' Addressing section, as R20 asks for shared
  conventions; the instructions were tightened to keep the guide, with its generated Tool
  groups section, within 16,384 bytes.
- **Schemas brought into the R3 subset:** the `screenshot` option of play/wait/send_input used
  `oneOf` (now `anyOf`), and the `{euler}` branch of `quat` and the `heightmap`/`blendmap`
  objects of `set_terrain_maps` were open objects (now `additionalProperties: false`).
- **`_meta` also carries `jasper/while_playing`**, so `call <tool> --help` against a live editor
  shows the play rule (R4) without the offline registry. `_meta` is not part of the measured size.
- **SSE is chosen lazily over HTTP:** a request is answered as an SSE stream when the client
  accepts `text/event-stream` and a notification (progress, `tools/list_changed`) comes before
  the answer, instead of "a progressToken was sent". Calls without notifications stay plain JSON.
- **`mcp --tool-groups` applies to every session of that editor** (stdio and HTTP), and
  `mcp.tool_groups` is the default when the flag is absent. An unknown name in the flag exits 2;
  in the settings it is a warning, because the GUI editor cannot exit on a stale setting.
- **`gen_tool_docs.py --check` ignores the editor version in the header line**: the version
  (`git describe --always --dirty`) changes with every commit, so comparing it would make the
  file stale on every commit.
- **Unit tests** are `editor/tests/cli/SchemaArgsTest.cpp` with the suites `CallArgs` and
  `SchemaValidate` (S02's `<Subject>Test.cpp` / `TEST(Subject, ...)` naming), not
  `CallArgsTests.*` / `SchemaValidateTests.*`.
- **AC6 uses `generate_shaders`** (a job of a few seconds) for `--wait-job` and Ctrl-C;
  `build_project --wait-job` runs under `JASPER_SLOW_BUILD=1` as specified.
- **`batch` always prints envelopes** (`--json` is implied); a usage error before any call is one
  envelope with `code: "usage"`. With `--atomic`, the batch result's images are attached to the
  last envelope (the MCP `batch` tool does not say which call made them).
- **The CLI's own stderr lines** (the editor that answered, a spawned editor's pid and scratch
  directory, image paths, progress) are suppressed by `--quiet`. A spawned editor's stderr goes
  to `editor.stderr.log` in its scratch directory, which is removed afterwards unless
  `JASPER_KEEP_SCRATCH=1`.
- **Coverage** (R23) counts a tool as tested when a test file names it quoted (`"tool_name"`),
  so a word inside a longer name does not count.
- **The agent guide has a budget in budget.json** (beyond R19's "≤ 16,384 bytes"): after the
  S04 merge oe-port's guide measured 17,194 bytes, because each stream trimmed or grew it ad
  hoc. budget.json's `guide` entry now holds its limits (16,384 bytes in all, 1,500 per `##`
  section, per-section exceptions with a reason), `tool_budget.py --check` gates them, and
  conventions.md ("The agent guide") makes the guide workflow plus one digest per area, with an
  area's detail in `docs/mcp/guide/<area>.md`. The Lua scripts section (3,139 bytes, the largest)
  became a digest; its full text and example script are `jasper://docs/lua-scripting`.
- **S04's tools are in `assets_pipeline`** (§6.4 names S04 there): `resolve_asset`,
  `find_references`, `get_asset_dependencies`, `list_missing_assets`,
  `retarget_asset_references`, `clear_asset_references`, through their module row in
  `buildToolRegistry`. S04 had registered them into core and raised the limit in
  `test_html_ui_tools.py`; its `import_files` and `build_project` parameters stay in core.
- **S10's tools are in `rendering_fx`** (`get_shadow_stats`, `set_shadow_debug`; §6.4 names no
  group for S10, and the group's description now names shadow statistics and debug views), and
  S10's guide paragraph became its own `## Shadows` digest: appended to "Shaders and materials"
  it took that section over the 1,500-byte section limit. S12's tools
  (`list_atmosphere_presets`, `apply_atmosphere_preset`, `get_atmosphere_info`) are in
  `rendering_fx` as §6.4 says, and its guide bullet became an `## Atmosphere` digest the same way;
  so are S13's (`inspect_color_lut`, `get_post_processing_status`, `set_post_processing_debug`),
  with post-processing a `## Post-processing` digest of its own. S10, S12 and S13 had raised
  `TOTAL_LIMIT` in `test_html_ui_tools.py` (S05 too, by 395 characters of core parameters); the
  merges keep budget.json as the only limits.
