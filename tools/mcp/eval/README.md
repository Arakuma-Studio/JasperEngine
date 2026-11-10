# MCP text eval

Does the editor's MCP text (tool names, descriptions, schemas, results, errors and the agent
guide) make agents act correctly at the moment they decide? Each case in `cases.py` is one
decision point taken from a real failure (`bad`), a real good behaviour to protect (`guard`), or
ordinary work that must not pull in the wrong tool (`control`). Checks are deterministic: calls
and must-not-calls with partial argument match, order, call caps, the last call, shell and
reply regexes, file edits. No model judges.

The eval is not part of CI (it costs subscription quota); CI runs `test_grade.py` only.

## Run

1. Record the surface from a build of this tree (new text needs a rebuild first):

   ```sh
   tools/build.sh linux
   tools/build.sh exec linux dbus-run-session -- uv run --no-project --with-requirements tools/mcp/requirements.txt \
       python tools/mcp/eval/dump_surface.py --out build/eval-surface
   # dbus-run-session, as the MCP tests run: without a session bus the editor exits at start (NFD_Init failed)
   mv build/eval-surface ~/je-eval/surface-<name>       # keep evidence outside the repository
   python3 tools/mcp/eval/measure.py ~/je-eval/surface-<name>/surface.json   # model-visible bytes
   ```

2. Run the cases on Orquester-managed Claude accounts (never an API key, never the system
   login; there is no default account):

   ```sh
   python3 tools/mcp/eval/run.py --appdir /var/lib/orquester --account jasperinuwu,jaspinggg \
       --surface ~/je-eval/surface-<name> --model sonnet --out ~/je-eval/runs/<name> \
       [--baseline ~/je-eval/runs/<base>/results.json] [-k case_id] [--samples 4]
   ```

   On an Orquester VM the account homes hold no login: ask for them with the
   `box_lend_accounts` tool (the owner approves a card) and pass the `appdir` it returns; call
   `box_return_accounts` only once the run has ended. Pick accounts with weekly quota left
   (`get_usage`). The runner spreads samples across the accounts given and records the account
   in every result row.

3. Read what the agent did before trusting a failure: `<out>/<case>/s<n>/stream.jsonl` (the
   transcript), `calls.jsonl` (what the stub was asked), `result.json`. A call the case did not
   mock shows up in `unmocked`: most early failures are eval bugs (a missing mock, a wrong
   result shape, a check stricter than the rule).

Default: 2 samples per case, a third when they disagree; a case fails when half or more of its
samples fail. Decide a wording change on `--samples 4` or more, against a baseline run on the
same model. `--tool-search off` sends every schema up front, as clients without tool search do;
`--patch PATCH.json` (`{"instructions": ..., "tools": {name: {"description": ...}}}`) tries a
wording on the recorded surface before you change the editor.

## Isolation

Each sample runs `claude -p` in a throwaway git copy of a new project, with
`CLAUDE_CONFIG_DIR=<account home>`, no API-key variables, `--setting-sources ""`,
`--strict-mcp-config` (only the stub), `--disable-slash-commands`, subagents and web tools off,
and a PreToolUse hook (`sim_hook.py`) that answers the case's canned shell commands, runs only
read-only commands inside the project and keeps file tools inside it. The stub
(`stub_server.py`) serves the recorded tool list and guide, applies the registry's argument
checks and answers from the case's mocks; `tool_groups` is stateful. The session files a sample
leaves in the account home are deleted. `result.json` reports `isolation` problems (other MCP
servers, plugins, skills) seen in the session's init event.

## Add a case

A case is a dict in `cases.py`: `id`, `kind` (bad, guard, control), `what_happened`,
`text_at_issue` (the current text, or "none"), `source` (the transcript, commit or doc it comes
from, paraphrased: no user data), `prompt`, optional `history` (prior turns, written as a
resumable session), `mocks` (results in the server's real shapes: `{"recorded": key}` reuses a
result `dump_surface.py` recorded; `"after": tool` answers only once that tool succeeded, so a
read after a write shows the write; `"after_error": tool`, once it returned an error), `playing`, `shell`, `files`, `groups_enabled`,
`max_turns`, and `checks` (see `grade.py`). `world.py` answers what a case does not mock: one
scene "Main" with a camera, ground, player, enemy and crate. A call nobody mocked fails with
"eval stub: no canned result" and is listed in the sample's `unmocked`: mock it before trusting
the sample. Check the decision, not more: a check stricter than the rule is an eval bug. New tool
text needs a case that fails without it.
