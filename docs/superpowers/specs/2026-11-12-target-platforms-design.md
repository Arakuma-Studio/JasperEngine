# Target Platforms: which platforms a game ships on (design)

Date 2026-11-12. Branch `docs/ci-target-platforms-design`. Companion design: `2026-11-12-ci-design.md`. Build
that one first: this one lands through its PR workflow. It starts once CI phase 2 (`AGENTS.md` "CI owns the full
suite") has landed. If it is built before CI phase 4 (the merge queue), AC7's merge-queue half is replaced by
whatever full-suite rule `AGENTS.md` states at that moment.

Every `path:line` below was read on `main` at `82ccd7c6a`. "Inferred" marks what comes from reading code, not a
run.

## 1. Intent

A game ships on some platforms, not all six. The owner's own games are Android and iOS only; another game may be
PC only. Today a project does not record its target platforms:

- **No setting.** `project.yaml` has no such key, and the `export:` map holds only method, pack, strip and
  backends (`editor/Stream.cpp:3022-3052`).
- **The editor guesses.** The Build Settings window opens on the host platform (`ExportWindow.cpp:100-105`), and
  `build_project` with no mode builds for this computer (`BuildTools.cpp:789`).
- **Agents guess too.** An agent in a mobile-only game cannot tell that, so it tests, budgets and exports for
  Linux because that is the machine it runs on.

After this:

- The project says where it ships.
- The editor, its MCP tools and the game's agents default to those platforms.
- An agent proves a change on the game's own platforms, and nothing else.

## 2. Goals

1. **G1 — one setting.**
   - Project Settings > Player > Publishing Settings > **Target Platforms**: a non-empty subset of Windows,
     Linux, macOS, Android, iOS and Web.
   - Stored in `project.yaml`; registry key `player.target_platforms`.
   - Undoable from the editor UI.
2. **G2 — the Build Settings window follows it.**
   - Each platform row has a **Target** checkbox that edits the same setting.
   - Non-target rows are dimmed.
   - The window opens on the host platform if it is a target, else on the first target.
   - Selecting a non-target platform shows a notice with an **Add to Target Platforms** button. Building it is
     still allowed.
3. **G3 — MCP follows it.**
   - `get_project_settings` / `set_project_settings` read and write it through the registry row. No new tool.
   - `get_build_info` reports `target_platforms`, a `target` flag per platform, and `default_platform`.
   - `build_project` with no `mode`, or `mode: player` with no `platform`, builds the default platform. A
     source export with no `backends` and no saved selection gets the target platforms' backend presets.
   - A build of a non-target platform succeeds and says the platform is not a target, naming the key.
4. **G4 — the game's agents follow it.**
   - The generated game `AGENTS.md` gets a "Target platforms" section: test, budget and export for the target
     platforms, and how to prove a change works on them.
   - The same facts reach projects whose `AGENTS.md` already exists, through `get_build_info` and
     `build_project` (G3).
5. **G5 — old projects load unchanged.**
   - A project without the key targets all six, so everything defaults as today.
   - No version bump and no migration.

## 3. Non-goals

- **Narrowing what an export contains.**
  - A source export still cooks every texture family and carries every workspace
    (`Exporter::exportPlatformNames`, `Exporter.cpp:2999-3004`).
  - Cooking only for the targets would make source exports smaller and faster. But a source project built later
    for a platform that was not a target would be missing textures. That needs its own design.
- **Hiding or reordering anything.**
  - Non-target platforms stay listed in Build Settings and in the Player page's per-platform tabs, in their
    usual order.
  - Dimming the per-platform Player tabs is a possible follow-up, not part of this.
- **A runtime Lua API.**
  - A shipped game never reads its project's target list; a game asks where it runs through `Engine.platform`,
    which exists (`CoreClassesLua.cpp:2398`).
  - The rule "a runtime feature gets a Lua API" does not apply: this is an editor and project setting.
  - If the editor's own Lua scripting exposes registry settings generically, the row comes with it.
- **Per-platform budgets, checks or `check --project` filtering by target.** `check_render_budgets` and the
  others keep their behaviour. Follow-up candidates.
- **Game CI.** The owner decided it is the game author's business.

## 4. Constraints

- **`AGENTS.md` invariants:**
  - editor UI with Unity-like names and undo;
  - MCP tools that drive and check it;
  - old projects load;
  - an optional new field gets a default and no version bump;
  - every persistent write goes through `AtomicFile` (the existing project save does).
- **The MCP text rules:**
  - new tool text needs a failing eval case in `tools/mcp/eval/cases.py`;
  - `tool_budget.py --check`. `build_project` is at its limit, 1,972 bytes (`tools/mcp/budget.json:138`), so
    its description change must be net zero or shorter.
- **The game `AGENTS.md` size cap:** 5,000 bytes (`tools/mcp/tests/test_generators.py`, `AGENTS_MD_BYTES`).
  Today's file is about 4,200 bytes plus paths, so the new section gets about 600 bytes.
- **Rule placement:** a rule an agent must see goes where it decides. That means the result of
  `build_project` and `get_build_info` first, the description second, and the guide last.

## 5. Design

### 5.1 Model and storage

- **`Project`:**
  - holds `uint8_t targetPlatforms`, a bit per `editor::BuildPlatform` (`ShippingTypes.h:17`), defaulting to all
    six;
  - gets `getTargetPlatforms()`, `isTargetPlatform(BuildPlatform)`, `setTargetPlatforms(uint8_t)`, and a static
    `allTargetPlatforms`.
- **YAML:** `export.targetPlatforms`, a sequence of `buildPlatformName` strings in `BuildPlatform` order:
  `windows, linux, macos, android, ios, web`.
  - **Encode** writes the key only when the set is not all six. That is the existing "non-default only"
    convention, so an untouched project's file does not change.
  - **Decode:**
    - names are read with `parseBuildPlatform`, which already accepts `osx` and `mac`;
    - unknown names are dropped with a load warning naming the value;
    - a list that is empty after that, or not a sequence, gives all six plus a warning.
  - No root key is added: it nests in `export:`, so `StreamSchema::knownRootKeys` is unchanged.
- **No migration.** Absence means all six, which is exactly today's behaviour.

### 5.2 Registry row: one source for UI, MCP and undo

`editor/settings/SettingsRows.cpp` gets row `player.target_platforms`, modelled on `player.allowed_orientations`
(`:1191-1222`):

- `type = "list"`, `values = {"windows","linux","macos","android","ios","web"}`.
- `get` returns the list in canonical order.
- `set` rejects an empty list or an unknown name with
  `must be a non-empty list of windows, linux, macos, android, ios, web`.
- **Description:** "Target Platforms: the platforms this game ships on; Build Settings, build_project and
  get_build_info default to them. Non-empty."
- **Table entry** (`:4243+`): `{"player.target_platforms", "Player", "Publishing Settings", "Target Platforms",
  "platforms targets ship export mobile desktop android ios web windows linux macos"}`.

Every UI edit goes through `ProjectSettingsValuesCmd` with this one key. It is the existing generic undoable
command (`editor/command/type/ProjectSettingsValuesCmd.h`), applies through the row's `set`, and saves
`project.yaml`. Command name: "Set Target Platforms". `set_project_settings` stays not undoable, as for every
setting (its contract).

### 5.3 Project Settings: Player > Publishing Settings

- **Placement:** first row of the section, above Build Method.
- **Look:** one line of six checkboxes, each with the platform's icon and name (the icons `ExportWindow` already
  draws, `platformIcon`).
- **Behaviour:**
  - It applies at once and is undoable, like the window's other apply-at-once rows (precedent: the note at
    `ProjectSettingsWindow.cpp:4308`), not on OK.
  - The last checked box is disabled, with the tooltip "A game needs at least one target platform."
  - The reset button restores all six.
- **Tooltip:** "The platforms this game ships on. Build Settings and the MCP tools default to them; you can
  still build any platform."

```text
Publishing Settings
  Target Platforms   [ ] 🪟 Windows  [ ] 🐧 Linux  [ ] 🍎 macOS  [x] 🤖 Android  [x]  iOS  [ ] 🌐 Web   ⟲
  Build Method       Auto ▾
  …
```

### 5.4 Build Settings window

The platform list (`ExportWindow::drawPlatformList`, `ExportWindow.cpp:612-653`) gains a checkbox column at the
row's left edge:

- **The checkbox:** it toggles the target set through the same command, with the tooltip "Target platform:
  this game ships here."
  - The Custom row has no checkbox; it is not a platform.
  - The last target's checkbox is disabled, as in 5.3.
- **Row text:** non-target rows draw their icon and name with `ImGuiCol_TextDisabled`. Order and selection
  behaviour are unchanged.
- **Opening platform:** `Project::defaultBuildPlatform()` (5.5). Replaces `hostPlatform()` at `:100-105`. The
  in-memory "last selected" platform stays within a project, but is forgotten when another project opens
  (`m_platformInitialized`, `ExportWindow.cpp:68,101`, is never reset today), so a mobile-only project never
  opens on the platform a previous project used.
- **Notice:** selecting a non-target platform shows, at the top of the right pane:
  "*Linux is not a target platform of this game.* **[Add to Target Platforms]**". Building it is still allowed.

```text
 Platforms                 │  Android
 [ ] 🪟 Windows   (dim)     │  Build Type   Player ▾
 [ ] 🍎 macOS     (dim)     │  …
 [ ] 🐧 Linux     (dim)     │
 [x] 🤖 Android  ◀ selected│
 [x]  iOS                  │
 [ ] 🌐 Web       (dim)     │
 ──────────────────         │
     Custom Graphics APIs   │
```

### 5.5 MCP

**`get_build_info`** (`BuildTools.cpp`, `platforms.<name>` at `:1355-1379`; `source.platforms` and
`source.backends` at `:1328-1332`):

- adds `target: true|false` per platform;
- adds top-level `target_platforms` (the list) and `default_platform`.

`default_platform` comes from one function, `Project::defaultBuildPlatform()`: the host if it is a target, else
the first target in the Build Settings list's order (Windows, macOS, Linux, Android, iOS, Web,
`ExportWindow.h:42-50`), so the window and the tools agree. Build Settings and `build_project` use it too.
Reporting it lets tests check the default without building. `source.backends`, when no `backends` were saved,
becomes the union of the target platforms' presets instead of the host's (`:903`, `:1328`).

**`build_project`** (`:785-800`):

- **When `mode` is omitted:** desktop if the host is a target (today's behaviour), else `player` for
  `default_platform`.
- **`mode: player` with no `platform`:** `default_platform` instead of the host (`:789`).
- **`mode: source` with no `backends` and no saved selection:** the target platforms' backend presets (`:903`).
- **An explicit `platform`, `desktop`, `web` or `backends`:** unchanged.
- **A non-target platform** builds as asked. The success result's `structuredContent.next` carries the note
  "<platform> is not a target (player.target_platforms: android, ios)". That is where an agent reads it as it
  decides (`AGENTS.md` MCP text).
- **Schema and description:** the default is the schema's `withDefault(stringEnum(...), "desktop")`
  (`:2368`) and `args.value("mode", "desktop")` (`:785`); the description says "desktop/web: this
  computer/web". The schema default goes (the handler decides), and the description gains "omitted: the
  project's default platform (get_build_info)". `tool_budget.py` counts both. Measure the tool's size first: 1,972
  is its limit in `budget.json:138`, not its measured size. Stay within it, cutting elsewhere in the same text if
  needed, and lower `budget.json` if it shrinks.

**The eval case** (`tools/mcp/eval/cases.py`):

- Setup: a project with `player.target_platforms = [android, ios]`, and the task "make sure the game still
  builds after this change".
- Pass: the agent builds Android, or reads `default_platform`/`target_platforms` before choosing.
- Fail: it builds for the host.
- Without this design's text and results, the case fails. Run on Orquester-managed accounts per `AGENTS.md`.

### 5.6 The generated game `AGENTS.md`

**A new section** in `Generator::writeAgentsFile` (`editor/Generator.cpp:1707-1783`), about 550 bytes:

```markdown
## Target platforms

This game ships on Project Settings > Player > Target Platforms (`player.target_platforms`;
`get_build_info` lists them). Test, budget and export for those only; ask before adding one.
Prove a change: `check --project .`, `play` it (`read_console`, `screenshot`), then
`build_project` for each target this computer can build. Name any you could not (iOS needs a Mac).
```

**Two fixes in the same function,** both found while reading it:

- The "Physics backends" line says "Project Settings > Build > 2D/3D Physics", but no Build page exists. The
  switches are on the Physics page (3D, `ProjectSettingsWindow.cpp:2907`) and the Physics 2D page
  (`:3196-3203`).
- The source URL names the old owner. This PR creates the repository-URL constant of the CI design's §12.6
  (one header, e.g. `editor/RepositoryUrl.h`, `https://github.com/Arakuma-Studio/JasperEngine`), and uses it
  for both URLs `Generator.cpp` writes (the project `CMakeLists.txt` comment at `:1270` and `AGENTS.md` at
  `:1751`). The other 11 files stay for CI phase 6.

**Existing projects:** the file is written once, when missing (`:1711-1720`), and that stays. Existing projects
learn about targets from G3's results, not from their `AGENTS.md`.

### 5.7 Docs

- **`docs/mcp/guide`:** a line only if the eval case needs it (MCP text rules: results first). Editor rebuild
  needed to show.
- **User-facing docs:** the Build Settings and Player settings docs under `docs/editor/` get the setting, with
  the screenshots from §7.
- **Generated:** `docs/mcp/TOOLS.md` regenerates (`tools/mcp/gen_tool_docs.py`).

## 6. Tests

Each test fails without the change. Each red run is named in the PR.

| Test | Where | Checks |
|---|---|---|
| `StreamProjectTargetPlatforms.MissingKeyTargetsAll` | `editor/tests/stream/` | an old `project.yaml` without the key → all six; re-encoding writes no `targetPlatforms` |
| `StreamProjectTargetPlatforms.RoundTripsCanonical` | same | `[ios, android]` → stored and re-read as `[android, ios]` |
| `StreamProjectTargetPlatforms.UnknownAndEmptyFallBack` | same | `[android, playstation]` → android plus a warning; `[]` → all plus a warning |
| `TargetPlatformsCmd.UndoRedo` | `editor/tests/command/` | `ProjectSettingsValuesCmd` on the key: execute, undo, redo, and project.yaml after each |
| `SettingsRegistry.CoversMcpKeys` | existing | passes with the new row (label, type, description, get, set) |
| `test_target_platforms.py` (CI = True, `EDITOR["create"]`) | `tools/mcp/tests/` | `set_project_settings` with an empty list → error naming the fix; set `[android, ios]` → `get_build_info` `target` flags, `target_platforms` and `default_platform == "android"` on a Linux host; `build_project` of a non-target platform succeeds and its `next` names the key (a web player build through a fake installed prebuilt runtime, as `test_build_method.py:246` does: the Linux image has no Emscripten); `mode: player` with no platform picks `default_platform`; reload keeps the value |
| `test_generators.py::test_project_agents_file` | existing, extended | the section names `player.target_platforms`; the size stays ≤ 5,000 bytes; the physics line names the Physics pages |
| eval case (5.5) | `tools/mcp/eval/cases.py` | fails before, passes after |
| `test_tool_budget.py` | existing | within budget |

**UI:** the two widgets are checked with screenshots from the implementer's own editor on a scratch copy:

- Project Settings row checked and unchecked;
- Build Settings with dimmed rows and the notice;
- Edit > Undo restoring the set.

They are attached to the PR (visible change ships screenshots). The command and the row are covered by the tests
above, so no UI-automation test is added.

## 7. Development plan (fast, under the CI design's rules)

One PR, from its own worktree:

1. **Model, stream and tests:** 5.1 with its three `StreamProject…` tests, red first.
   `tools/check-changed` per commit.
2. **The row and the command:** 5.2 and `TargetPlatformsCmd.UndoRedo`. `get/set_project_settings` then work
   with no tool change.
3. **`defaultBuildPlatform()`, `get_build_info` and `build_project`:** 5.5 and `test_target_platforms.py`.
   Then the budget check and the eval case.
4. **The two widgets:** 5.3 and 5.4, with the screenshots.
5. **The generator:** 5.6 and its test.
6. **Docs:** 5.7.

Then:

- Open the PR with the template, its "Surfaces" checklist filled:
  - backends: N/A, no rendering;
  - export targets: defaults only;
  - editor UI and undo: yes;
  - Lua: N/A, see §3;
  - MCP: yes;
  - old projects: yes, the test above.
- Watch `Check`, read the report, fix, and stop when green.
- The full MCP suite runs in the merge queue, not locally.

**Speed choices:**

- No new MCP tool: the registry row covers get and set.
- No new command class.
- No migration.
- One function decides the default for all three consumers.

## 8. Acceptance criteria

1. **AC1:** An old project (any committed fixture) opens, saves, and its `project.yaml` is byte-identical.
2. **AC2:** In the editor, both widgets can set and clear targets, cannot clear the last one, and Edit > Undo
   reverts each change. Screenshots.
3. **AC3:** On a Linux host with targets `[android, ios]`:
   - Build Settings opens on Android;
   - `get_build_info.default_platform` is `android`;
   - `build_project` with no `mode` builds Android, or fails only on the missing Android SDK, naming the
     platform.
4. **AC4:** Building a non-target platform succeeds, and its result names the key.
5. **AC5:** A new project's `AGENTS.md` has the section, names the right Physics pages and the new repository
   owner, and is ≤ 5,000 bytes.
6. **AC6:** The eval case fails on `main` and passes on the branch; `tool_budget.py --check` passes.
7. **AC7:** `Check` is green on the PR's last commit, and the merge-queue run is green.

## 9. Calls made in this document

These are the author's calls, not the owner's decisions. Check them while reading.

- The row lives in Player > Publishing Settings. There is no Build page, and Build Method lives there. The
  Build Settings window is the second, more visible place.
- Both widgets apply at once (undoable) instead of on OK.
- The source-export cook is not narrowed (§3).
- No runtime Lua API (§3).
- A non-target build is allowed with a note, never refused.
- When the host is not a target, `build_project` with no mode, or `player` with no platform, builds the default
  platform. A source export with no backends uses the targets' presets. An explicit platform or backends is
  untouched.
- "First target" follows the Build Settings list order, not the storage order.
- Build Settings forgets its selected platform when another project opens.
- This PR creates the repository-URL constant and uses it only in `Generator.cpp`.
