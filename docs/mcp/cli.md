# Calling MCP tools from a shell: `jasperengine-editor call`

Every MCP tool of the editor (see [TOOLS.md](TOOLS.md)) runs from a shell, a CI script, a hook
or an agent without an MCP client:

```sh
jasperengine-editor call --project ~/games/demo create-entity --name Box --type box
jasperengine-editor call --port 47615 set_properties --entity Player/Gun --values transform='{"position":[0,1,0]}'
jasperengine-editor call --json --wait-job --port 47615 build_project '{"mode":"desktop","build_type":"Release"}'
jasperengine-editor call screenshot --view game --image-dir build/shots
printf '%s\n' '{"tool":"create_scene","arguments":{"name":"A"}}' '{"tool":"save_scene","arguments":{"path":"scenes/A"}}' \
  | jasperengine-editor batch --project ~/games/demo --atomic
```

The command line is the editor binary itself. `call`, `batch`, `list-tools` and `editors`
never start the editor's window or backend in their own process, and never write
`settings.yaml` or `editor.log` in the working directory.

## Subcommands

| Command | What it does |
|---|---|
| `call [options] <tool> [tool arguments]` | one tool call |
| `call [options] <tool> --help` | the tool's parameters, generated from its schema, with two examples |
| `call --help` | the options and every tool name, by group |
| `batch [options] [--continue] [--atomic] < calls.ndjson` | several calls over one session |
| `list-tools [options] [--json] [--group G\|all]` | name, group, annotations (`r` read-only, `d` destructive, `i` idempotent), size; `--json`: the definitions |
| `editors [--json]` | the running editors that serve MCP over HTTP: pid, url, project, version, started |

## Options

Options go **before** the tool name; everything after it belongs to the tool (except
`--help`), so a tool parameter never collides with an option.

| Option | |
|---|---|
| `--url U` | `http://127.0.0.1:<port>/mcp` of a running editor |
| `--port N` | the port of a running editor (started with `--mcp-port N`, or MCP enabled in Editor Settings) |
| `--token T` | bearer token; else `JASPER_MCP_TOKEN`; else `mcp.http_token` of that editor's `settings.yaml` |
| `--project DIR` | the running editor that has DIR open, else a one-shot headless editor on DIR |
| `--timeout S` | seconds per call (default 120); then the CLI cancels the call and exits 3 |
| `--json` | exactly one JSON envelope line per call on stdout |
| `--structured` | print `structuredContent` (indented JSON) instead of the text |
| `--quiet` | no progress or status lines on stderr |
| `--dry-run` | convert and validate, print `{"tool", "arguments"}`, contact no editor |
| `--image-dir D` | where images go (default `.`) |
| `--wait-job` | when the call started a job (`job_id`, status `running`), poll `get_job` every 500 ms until it ends |

## Which editor answers

1. `--url`, else `--port` (127.0.0.1), else `JASPER_MCP_URL`.
2. `--project DIR`: the running editor whose project is DIR (paths compared canonically).
   If none has it open, the CLI starts one (below). It never attaches to an editor of another
   project when `--project` is given.
3. No target: the only running editor. None: exit 3 ("pass --project to start one"). More
   than one: exit 2, listing them.

Running editors are found through the instance registry: an editor whose HTTP transport is on
writes `<user-config-dir>/mcp/instances/<pid>.json` (`~/.config/jasperengine` on Linux,
`~/Library/Application Support/JasperEngine` on macOS, `%APPDATA%\JasperEngine` on Windows)
with its pid, url, port, project, working directory, settings path, version and start time,
mode 0600, and removes it when HTTP stops or it quits. Readers delete the files of processes
that are gone. **The token is never in that file.** The CLI sends no `Origin` header and
`Host: 127.0.0.1:<port>`.

### One-shot editors (`--project` with no editor open)

The CLI runs itself as `mcp --project DIR --headless` with pipes on stdio, in a scratch working
directory that holds an empty `settings.yaml` (the user's settings are never read or written),
waits up to `JASPER_CALL_SPAWN_TIMEOUT` seconds (default 600: the first open compiles shaders)
for it to answer, and closes its stdin at the end, which makes it quit. Only that process is
killed, and only if it is still there 30 s later. Its stderr is `editor.stderr.log` in the
scratch directory, which is removed afterwards (`JASPER_KEEP_SCRATCH=1` keeps it).

**Edits are not saved implicitly**: what a one-shot editor changed and did not save is gone
when the command ends. Save in the same `batch` (`save_scene`, `save_project`). For many calls,
start an editor with `--mcp-port` and use `--port`, or use `batch`.

If another editor has the project open (the project lock), the CLI exits 3 with
`project_locked` and that editor's pid: attach to it with `--port` instead.

## Tool arguments

Either one JSON object (`'{...}'`, `@file.json`, or `@-` for stdin) or flags, not both. A
flag is `--<key>` or `--<key>=<value>`, the parameter name with `_` written `-` (the snake_case
spelling works too). Values convert from the parameter's schema:

| Schema | Command line |
|---|---|
| string | verbatim |
| integer | strict decimal (`1.5` is an error) |
| number | a finite number |
| boolean | omitted = not sent; `--k` = true; `--k=V` or `--k V` with true/false/1/0/yes/no/on/off; `--no-k` = false |
| enum | one of the values (an error lists them) |
| reference (`integer\|string`: scenes, GameObjects) | digits = an id; `'"42"'` (a JSON string) = the name 42; anything else = a name or path |
| array of scalars | `1,2,3`, a JSON array, or the flag repeated; `--position 1,2,3` is a vec3 |
| free-form object (a property map) | JSON, or `KEY=VALUE` repeated (VALUE is JSON when it parses, else a string) |
| object with declared keys, array of objects, `anyOf`, any | JSON only (or `@file`); for "any", a word that is not JSON is a string |

Any value accepts `@path`: the file's contents, as JSON or text by the rule above.

Before anything is sent, the arguments are validated against the schema (type, enum,
required, properties, additionalProperties, items, minItems, maxItems, minimum, maximum,
anyOf); an unknown flag fails with a did-you-mean and the valid flags. The editor's own
validation stays authoritative.

`--help` and `--dry-run` use the tool list of this build (the registry the editor builds,
without starting it); with a target, `--help` and `list-tools` use the editor's live list.
A version mismatch between the CLI and the editor is a warning on stderr.

## Output

- Plain: the result's text on stdout (a tool error's text on stderr); `--structured` prints
  `structuredContent` instead. Images are written to `<image-dir>/<tool>-<n>.<ext>` (never over
  an existing file) and their paths go to stderr; base64 is never printed.
- `--json`: one line per call:

  ```json
  {"ok": true, "exit_code": 0, "code": null, "tool": "create_entity", "text": "...", "structured": {...},
   "images": [{"path": "screenshot-1.png", "mime_type": "image/png", "bytes": 48213}], "error": null,
   "editor": {"url": "http://127.0.0.1:47615/mcp", "pid": 4242, "project": "/home/me/games/demo", "spawned": false, "version": "v1.4.0"}}
  ```

  `code` is `null`, `"tool_error"`, `"usage"`, `"connection"`, `"internal"` or `"tool_exit"` (the
  tool's own exit code, below; `ok` is true then).
- Progress notifications (the CLI always asks for them) go to stderr unless `--quiet`.

## Exit codes

| Code | Meaning |
|---|---|
| 0 | ok |
| 1 | the tool answered with an error (refusals such as busy, play mode or an open dialog included); a `--wait-job` job that failed or was cancelled |
| 2 | usage: a bad option, flag or value, an unknown tool, several editors and no target |
| 3 | connection: refused, wrong token (401/403), unknown session, timeout (after `notifications/cancelled`), Ctrl-C, the one-shot editor failed to start, `project_locked` |
| 4 | internal: malformed JSON-RPC, an unsupported protocol version, an unexpected error |

A tool can set the exit code of a successful call: an integer `cli_exit` (1 to 125) in its
`structuredContent` (or in a `--wait-job` job's result) becomes the exit code. `apply_palette
--measure-only` sets it from its verdict, as the pixel-art checks do: 0 `clean`, 3 `fixable`, 4
`needs_person` (and 2 for a usage error, as every call). Those numbers are also the CLI's
`connection` and `internal` codes: in a script, tell them apart with `--json`, whose `code` is
`"tool_exit"` for the tool's own code (and `ok` true), or by the verdict on stdout. `batch` does not
stop at a tool's own exit code.

Ctrl-C during `--wait-job` sends `cancel_job` for the job, then exits 3.

## batch

`batch` reads one `{"tool": ..., "arguments": {...}}` per line from stdin (blank lines and `#`
lines are skipped). Every line is parsed and validated first: a malformed line exits 2 before
anything runs. The lines then run in order over one session, with one `--json` envelope per
call on stdout, and stop at the first failure unless `--continue`. `--atomic` sends them as one
MCP `batch` call with `undo_group: true` (one undo step; at most 200 lines, else exit 2). The
exit code is the highest of the calls'.

## Tool groups

`call` and `batch` reach every tool, whatever groups the editor's sessions list
([conventions.md](conventions.md#tool-groups)); `list-tools --group <name>` shows one group.
