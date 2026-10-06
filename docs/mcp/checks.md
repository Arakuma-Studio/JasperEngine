# Project checks: `jasperengine-editor check`

The checks the MCP tools run (`check_script`, `check_shader`, `compile_scripts`) are also a CLI
subcommand, so an agent that edits files with its own tools, a shell, a CI job or an agent hook
gets the same findings without an editor running. Both sides call the same code
(`editor/util/ProjectChecks.{h,cpp}`).

```
jasperengine-editor check --project <dir|project.yaml> [--lua] [--shaders] [--cpp] [--all]
                          [--files <paths...>] [--format text|json] [--quiet] [--hook]
```

| Option | |
|---|---|
| `--lua` | Every `.lua` file of the project: syntax (compiled, not run), then the script's top level run in an isolated Lua state to read its `properties`, as the editor does (see below). |
| `--shaders` | Every shader fork: the forks the scenes, prefabs and scene settings use, and every `.vert`/`.frag` pair no scene uses yet (its type guessed from the fragment shader). Compiled with the engine's shader compiler for the editor's backend, exactly as `check_shader` does, includes resolved as the engine resolves them. |
| `--cpp` | The C++ scripts: generated sources, CMake configure and build, as Play and `compile_scripts` do (same build directory, `.jasperengine/build`, so it is incremental). Slow the first time (a configure plus a full build; about 25 s for one script here), then a few seconds to tens of seconds. |
| `--all` | All three. Without `--lua/--shaders/--cpp/--all`: `--lua --shaders` (C++ only when asked, since it builds). |
| `--files <paths...>` | Only these files: absolute, relative to the current directory, or relative to the project. Lua files are checked one by one; a `.vert`/`.frag` checks its fork; a `.glsl` checks every fork (any can include it); a C++ file (`.h .hpp .hh .hxx .cpp .cc .cxx .c .inl`) builds the scripts when `--cpp` is given and is ignored otherwise. Files outside the project, in hidden or `build` directories, deleted, or of other kinds are skipped; when nothing is left, the command returns at once (no engine start). |
| `--format json` | One JSON object on stdout (below). Default `text`. |
| `-q`, `--quiet` | No summary line on stderr. |
| `--hook` | Agent hook mode (below). |

## Output

Text: one line per problem on stdout, compiler style, paths relative to the project directory,
sorted by tool, path and line:

```
scripts/Enemy.lua:12: error: unexpected symbol near '='
scripts/Boss.lua:3: error: runtime error at load: attempt to call a nil value (global 'spwan')
scripts/Boss.lua: error: property 'speed' (float): default must be a number, got string
shaders/Water.frag:229: error: 'wobble' : undeclared identifier
shaders/inc/noise.glsl:14: error: 'hash3' : no matching overloaded function found
scripts/Spinner.cpp:31:16: error: 'undefinedThing' was not declared in this scope
cpp: error: the C++ build failed without a file:line diagnostic (CMake configuration or linking?); last output: ...
```

`path:line:col: severity: message`, with an unknown column or line left out (Lua and glslang give
no column); a problem without a file starts with the tool name. Then a summary on stderr (unless
`--quiet`): `check: 1 error, 0 warnings (lua 7 files, shaders 1 fork) in 0.22s`. Engine and compiler
logs are discarded; when a C++ build fails without a file:line, its last 30 output lines follow on
stderr.

JSON (`--format json`):

```json
{
 "ok": false,
 "errors": 1,
 "warnings": 0,
 "checked": {"lua": 7, "shaders": 1},
 "findings": [
  {"tool": "lua", "path": "scripts/Enemy.lua", "line": 12, "col": 0, "severity": "error",
   "message": "unexpected symbol near '='"}
 ],
 "seconds": 0.22
}
```

`tool` is `lua`, `shader` or `cpp`; `path` is project-relative (absolute for a file outside the
project, e.g. a system header warning; `engine/shaderlib/<include>` for an engine shader include;
`""` when there is no file); `line`/`col` are 1-based, 0 when unknown; `severity` is `error` or
`warning`. `checked` counts what ran: Lua files, shader forks, C++ scripts.

Exit code: **0** no errors (warnings allowed), **1** errors, **2** usage error (bad option, no
`project.yaml`, the project fails to load).

The same findings come back from MCP: `check_script` and `check_shader` return `findings` in this
format, and `compile_scripts` returns the same diagnostics as `errors`/`warnings`
(`{file, line, column, message}`).

## What the Lua check runs

The editor reads a Lua script's properties by running the script's top level (it returns the
script table with its `properties`). `check` does the same, so a script that fails at load
(`runtime error at load: ...`) or declares a property default it can't read is an error. In the
editor (`check_script`, `sync_files`) that runs in the editor's Lua state, as it always did. The
CLI runs every script in a fresh, isolated `lua_State` instead, so checking a file can't affect
anything:

- no `io`, `debug`, `dofile`, `loadfile`, native modules or binary chunks; `os` has only `time`,
  `clock`, `date` and `difftime`; `print` prints nothing;
- `require` loads the project's Lua modules from the project's Lua directory, as the engine does;
- the engine's globals (`Vector3`, `Engine`, `RegisterEngineEvent`, ...) are inert stand-ins (callable,
  indexable, returning stand-ins), so top-level engine calls run but do nothing; names that are
  not engine globals are `nil`, so `undefinedThing()` fails as in the editor;
- 256 MB of memory and about 100 million VM instructions (an endless loop at load is reported at
  its line).

So the CLI and `check_script` agree on syntax errors, load errors that don't depend on the engine's
state, and property declarations; a top level that depends on real engine values (rare: it runs
before any scene) can differ.

## Examples

```sh
# everything but C++ (fast: about 0.2-0.5 s for the TappyPlane fixture)
jasperengine-editor check --project .

# after editing files
jasperengine-editor check --project . --files scripts/Enemy.lua shaders/Water.frag

# CI: all checks, machine-readable
jasperengine-editor check --project . --all --format json > check.json
```

## Agent hooks

### Claude Code

`.claude/settings.json` of the project (tested with Claude Code 2.1.285, `claude -p`): after
every Edit/Write the hook runs the checks on the edited file; on errors Claude gets them as hook
feedback and fixes them.

```json
{
  "hooks": {
    "PostToolUse": [
      {
        "matcher": "Edit|Write|MultiEdit|NotebookEdit",
        "hooks": [
          { "type": "command", "command": "jasperengine-editor check --project \"$CLAUDE_PROJECT_DIR\" --hook", "timeout": 120 }
        ]
      }
    ]
  }
}
```

A PostToolUse hook gets the call as JSON on stdin (`tool_input.file_path` is the edited file,
absolute); Claude Code shows the model the hook's stderr only when it exits with **2**. `--hook`
does exactly that: it reads the JSON, checks the file(s), prints the problems on stderr and exits
2 when there are errors, 0 otherwise (also for files it doesn't check), 1 when it can't run (bad
stdin, no project), which Claude Code shows the user only. Add `--cpp` to also build the C++
scripts after a C++ edit (seconds to tens of seconds per edit). The edit itself is never undone.
In a real run, Claude received:

```
PostToolUse:Edit hook blocking error from command: "jasperengine-editor check --project "$CLAUDE_PROJECT_DIR" --hook": ...: scripts/GameController.lua:35: error: unexpected symbol near '='
```

Without `--hook` (e.g. on a system with `jq`), the equivalent is:

```sh
f=$(jq -r '.tool_input.file_path // empty'); [ -z "$f" ] || jasperengine-editor check --project "$CLAUDE_PROJECT_DIR" --files "$f" --quiet 1>&2 || exit 2
```

### Codex

Codex CLI (0.159) has the same hooks (`codex features list`: `hooks stable`), in
`<repo>/.codex/hooks.json` or `~/.codex/hooks.json` (or `[[hooks.PostToolUse]]` in `config.toml`):

```json
{
  "hooks": {
    "PostToolUse": [
      {
        "matcher": "Edit|Write",
        "hooks": [
          { "type": "command", "command": "jasperengine-editor check --project . --hook", "timeout": 120 }
        ]
      }
    ]
  }
}
```

Codex edits files with `apply_patch`: the hook's stdin has no `file_path`, the paths are in the
patch text (`tool_input.command`, `*** Update File: <path>` / `*** Add File:` / `*** Move to:`
lines), which `--hook` reads. Exit 2 shows stderr to the model, like Claude Code. Codex runs a
hook only once it is trusted (review it with `/hooks` in the interactive CLI; project hooks also
need a trusted project) and skips untrusted hooks silently. `CLAUDE_PROJECT_DIR` is not set: the
hook runs in the session's directory, hence `--project .`. The `--hook` parsing of the Codex input
is covered by the tests (with the stdin Codex sends); a live Codex session with this hook was not
run.

## CI

```yaml
- name: Check scripts and shaders
  run: jasperengine-editor check --project . --all
```

The CLI needs no display (it doesn't open a window) and `--cpp` needs CMake and a C++ compiler
like Play. It reads the project; `--shaders`/`--cpp` load it as the editor does (which may refresh
`.jasperengine/assetdb.yaml`), and `--cpp` writes the generated sources and the build under
`.jasperengine/` and regenerates `CMakeLists.txt`, as Play does. Don't run `--cpp` while the editor
builds the same project.
