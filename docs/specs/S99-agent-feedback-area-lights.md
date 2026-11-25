# S99 — Agent feedback loop, contributor checks, area lights and IES profiles

**Depends on.** The MCP server (`editor/mcp/`), McpJobs (`editor/mcp/McpJobs.h`), `screenshot`, `build_project`
(`editor/mcp/tools/BuildTools.cpp`), `tools/check_changed.py`, the Lit shading path (`engine/shaderlib/`,
`engine/renders/`, `engine/core/render/`), S11 photometric units, S57 screen-tile light lists, `LightComponent`,
`SkyboxComponent`, `SceneSettings`.
**Reference.** Crucible (`github.com/codepawl/crucible`, MIT, commit `bc46b028`), read for ideas only: no code is copied.
The comparison is summarised in Section 4.

## 1. Goal

Agents working in JasperEngine should be told what went wrong without having to ask, with a fix they can apply. That means:
a suggestion when they mistype a name, advice instead of raw exceptions, findings on every screenshot, the size of every build, and
cost estimates on long jobs. Contributors get one check command that keeps going, prints a fix per failing step and proves its own
red control. Rendering gains the two light types every Unity user expects and we lack: area lights (Rectangle, Disc) and IES
profiles. It also gains two zero-cost lighting authoring aids: the sun extracted from an HDRI, and a Gradient ambient source.

## 2. Scope

In scope, as workstreams:

| ID | Workstream | Parts |
| --- | --- | --- |
| W1 | Names and faults in MCP errors | "did you mean" everywhere, internal faults turned into advice |
| W2 | Screenshot findings | exposure, empty frame, subject framing, missing textures, z-fighting; Scene view Frame Checks overlay |
| W3 | Build size for agents | sizes in `build_project`, a download budget and its warning |
| W4 | Job estimates and reports | estimate on submit, measured report and fallbacks on completion, per-project history |
| W5 | MCP call log | opt-in argument-free JSONL log, `call_profile.py` summary, per-tool counts in the MCP Server window |
| W6 | check-changed output and red control | per-step logs, summary with fix lines, keep-going, `--json`, area lints, `--red-control` |
| W7 | HDRI sun and gradient ambient | Extract Sun from a skybox HDRI, Remove Sun Disc, Environment Lighting Source: Gradient |
| W8 | Area lights and IES profiles | Light Type Area (Rectangle, Disc), tiered shading, shadows, IES Profile asset on spot and point lights |

Out of scope: baked GI, lightmaps and probe volumes; light linking; area lights in 2D; textured (image) area lights; reversed-Z;
an HDR scene target (S13 stands); a review grid or offscreen multi-shot `screenshot`; a Scene Audit window; result-size gates;
PR-body checks. Nothing here touches the do-not-copy list in `README.md`.

## 3. Current state

- **MCP errors.** `McpValues.cpp` already uses `engine/core/util/NearestName.h` for some unknown keys. Unknown tool names, unknown
  `jasper://docs` areas, resource URIs and enum values get no suggestion. `McpServer.cpp` (catch chain near line 1546) rewrites only
  `Json::type_error`. Every other `std::exception`'s `what()` reaches the agent verbatim, for example `vector::_M_range_check`.
- **Screenshots.** `screenshot` warns only when the image is a single flat colour.
- **Builds.** The exporter writes `build-report.json` with sizes, and `BuildReportWindow` shows them, but `finishRun` in
  `BuildTools.cpp` does not put them in the `build_project` result. There is no download budget.
- **Jobs.** `McpJob` reports progress, but no job gives an estimate before it runs or compares the time taken against one.
  Downgrades taken during a job (for example a software encoder used instead of a hardware one) are not listed.
- **Call log.** The MCP Server window keeps recent calls in memory only. `tools/mcp/call_profile.py` reads only the harness's
  own logs.
- **check-changed.** `tools/check_changed.py` stops at the first failing step, prints up to about 20 KB of raw output inline, and
  writes no per-step logs or JSON. AGENTS.md asks contributors to remember five area lints that it does not run. The red control
  the Evidence rules demand is done by hand.
- **Lighting.** `LightComponent::LightType` is `DIRECTIONAL, POINT, SPOT`. There are no area lights and no IES profiles. Ambient
  is `SceneSettings::globalIllumColor`/`globalIllumIntensity` plus the sky. Per-object SH (`objectLights.shR/G/B`) already carries
  the lights beyond `maxLightsPerObject`. An HDRI skybox lights the scene only through IBL, so its sun casts no shadow.

## 4. Reference (Crucible, ideas only)

- Every error names the fix (`mcp-input-errors.mjs`, `mcp-union-errors.mjs`); internal crashes become fixed advice
  (`agent-errors.js`).
- Opt-in call log without arguments (`mcp-call-log.mjs`).
- Frame findings on previews (`frame-checks.js`, `frame-diagnostics.js`, `coplanar-overlap.js`).
- Render-job estimates corrected by the last measured jobs, and a report of measured against estimated with fallbacks
  (`docs/agent-render-jobs.md`).
- A preflight that keeps going and prints one line per check with a fix (`scripts/preflight.mjs`), and an automatic red
  control (`scripts/test-mutation-check.mjs`).
- Area lights: a representative point at low cost (`light-shapes.js`). IES: an LM-63 parse (`light-profile.js`). HDRI sun
  extraction (`environment-*.js`). Ground and gradient ambient (`ground-bounce.js`).
- What we improve on: area lights get LTC on High and shadows. IES keeps the full 2D profile, not a 16-sample 1D average.
  Findings use our renderer bounds and the triangle BVH. The red control reruns our exactly selected tests, not a basename grep.

## 5. Requirements

### W1 — Names and faults

- R1. Unknown tool name, unknown argument key (top level and nested closed objects), unknown enum value, unknown
  `jasper://docs/<area>` and unknown resource URI each produce an error naming at most one nearest valid name ("did you mean
  `local_position`?"). The match goes through `NearestName.h`. It also catches a camelCase/snake_case confusion of a valid name
  (`localPosition` → `local_position`) and a missing or extra `_`.
- R2. When no candidate is close enough, the error lists the valid names, or the call that lists them (`get_tool_help`, the docs
  index), never nothing.
- R3. `std::logic_error` and its family (`out_of_range`, `invalid_argument`, `length_error`, `domain_error`),
  `std::bad_alloc`, `std::bad_function_call`, `Json::out_of_range`/`Json::invalid_iterator` and unknown exceptions thrown out of
  a handler are internal faults. The agent gets: `internal error in <tool> (not caused by your arguments); details in editor.log
  as fault <id>. Check the state with <a read tool of the same group, or editor_status>, retry once, and report it if it
  repeats.` The full `what()`, the tool and the id are logged at error level.
- R4. Handlers that throw a `logic_error`-family exception with a message meant for the agent are converted to the normal
  error path (`runtime_error` or an error result) first, so no intended message is lost. An audit of `editor/mcp/tools/` finds
  them; the result is recorded in Deviations.
- R5. A table-driven test covers each error class in R1–R3 and asserts the text names a next action.

### W2 — Screenshot findings

- R6. Every `screenshot` result of a Scene or Game view carries `structuredContent.findings`. It holds at most 5 entries, the
  most severe first, each `{kind, severity, message, fix, entities?}`. When there are findings, a `next` line names the first fix.
  No new parameter is added.
- R7. The kinds:
  - `exposure_clipped`: at least 20 % of pixels with luminance at or above 0.98. The message names the brightest contributing
    light where one is known.
  - `exposure_dark`: mean luminance under 0.04 with at least 80 % of pixels under 0.02, unless the camera's clear colour is
    itself that dark and nothing is drawn.
  - `empty_frame`: no renderer's world bounds intersect the view frustum.
  - `subject_out_of_frame`: the selection (or, with none, the union of visible renderers) is more than 50 % outside the frustum
    by projected bounds area. The fix gives the call that frames it.
  - `subject_too_small`: the selection covers less than 1 % of the image.
  - `texture_missing`: a visible renderer's material references a missing asset (the S04 missing-asset tracker).
  - `z_fighting`: two different visible renderers have coplanar, same-facing, overlapping triangles within 1 mm. Candidate
    pairs come from overlapping world bounds; each pair is checked against plane buckets built from the triangle BVH.
- R8. The findings cost at most 10 ms on the fixture project's main scene (Release, the build machine) and never exceed a
  30 ms hard budget. Past it, the remaining checks are skipped and one `info` finding says which.
- R9. The Scene view's overlay menu gains **Frame Checks** (off by default, saved per user). It draws the current findings as
  a small warning list at the view's corner, refreshed at most twice a second. Toggling it is a view setting and needs no undo.

### W3 — Build size for agents

- R10. The `build_project` result (and CLI `build --json`) includes `build_bytes`, `build_categories` (bytes per category, as
  the build report has them), `top_files` (the 5 largest, path and bytes) and `build_report` (path). A web build also has
  `download_bytes_gzip`, a gzip-9 estimate of what a browser downloads, labelled an estimate.
- R11. Project Settings gains **Download Budget (MB)** per family: Web, Mobile, Desktop. It is stored as optional fields with
  default 0 (off), so no version bump. A build over its family's budget succeeds, carries the warning `W-BUILD-OVER-BUDGET`
  (budget, actual, the three largest categories), and gets a `next` naming the tools that inspect the largest category.
- R12. The Build Report window shows the budget and actual size, coloured when over budget.

### W4 — Job estimates and reports

- R13. `record_game`, `build_project`, `bake_impostors` and `export_title` jobs return `estimate` at submit:
  `{seconds, bytes?, basis: "model"|"history", confidence: "low"|"medium"|"high"}`. The model is per kind (frames × resolution,
  file count, impostor count × resolution, layers × size).
- R14. On completion the result has `report`: `{seconds, estimated_seconds, ratio, fallbacks: [{step, from, to, reason}]}`.
  Each of these jobs records the downgrades it can take, for example encoder, method (prebuilt to compiled), texture format,
  resolution clamp.
- R15. History: the last 16 completions per kind are stored in `.jasperengine/user/job-history.json` through `AtomicFile`. The
  estimate uses the median measured/model ratio, clamped to [0.25, 4]. A corrupt or missing file starts empty, never fails a job.
- R16. The editor's progress bar and `get_job` show the remaining time when an estimate exists.

### W5 — MCP call log

- R17. Opt-in by `JASPER_MCP_LOG=<path>` or the MCP Server window's **Log Calls To File** setting (saved per user). Each call
  appends one JSON line: `ts, tool, group, ms, ok, error_kind, result_bytes, client`. Arguments, paths, entity names and result
  content are never written.
- R18. The writer is append-only, buffered and flushed on a background thread, and rotates to `<path>.1` past 16 MB. A write
  failure disables the log for the session with one console warning. It is a raw write, so it gets an entry in
  `tools/raw_writes_allowlist.txt` with the reason.
- R19. `tools/mcp/call_profile.py` summarises such a log: calls, error rate, p50 and p95 ms, and mean result bytes per tool,
  sorted by calls.
- R20. The MCP Server window shows per-tool counts and error rates for the session next to the recent-calls list.

### W6 — check-changed output and red control

- R21. Each step's full output goes to `build/check-changed/<step>.log`. The terminal shows, per failing step, the parsed
  failures (gtest, build, pytest and lint parsers, reusing `tools/ci/report.py` where it already parses them), at most 40 lines,
  then the log path.
- R22. The run ends with one line per step, `ok|FAIL|skip|cached  <step>  <seconds>`. Each FAIL line carries a `fix:` line
  with the exact rerun command (for example `UNIT_TEST_ARGS=--gtest_filter=X tools/build.sh unit-tests`).
- R23. Independent steps keep going after a failure. A step whose build failed is reported as `skip` with the reason, not run.
  The exit code is non-zero when any step failed.
- R24. `--json` prints the plan, the step results, log paths and fix commands as one JSON document.
- R25. The area lints (`check_raw_writes.py`, `check_stream_keys.py`, `check_event_clears.py`, `check_text_encoding.py`,
  `tools/ci/check-hardcoded-shortcuts.py`) run by path, before the builds. AGENTS.md and `docs/testing.md` drop their "lints
  check-changed does not run" lists and say check-changed runs them.
- R26. `--red-control`: refuses a dirty tree, or a leftover from an interrupted run, naming the fix. It first runs the normal
  selection, or uses its cached pass. It then saves the branch patch over non-test paths (everything outside `engine/tests`,
  `editor/tests`, `tools/mcp/tests`, `tools/fixtures`, goldens and `docs/`) to `build/red-control/patch.diff` and reverse-applies
  it. It rebuilds and reruns the same selected tests, then restores in a `finally`. Afterwards it verifies the tree is
  byte-identical to before (status and diff hash). It prints which tests failed without the change: a test that fails to build
  counts as red and is labelled `red (build)`. It exits 1 if none went red. The result is never cached.
- R27. A run killed mid-revert leaves a marker. The next invocation refuses to run and prints the one command that restores the
  tree (`tools/check-changed --red-control --restore`).

### W7 — HDRI sun and gradient ambient

- R28. **Extract Sun** (Skybox Inspector button, MCP and CLI): analyses the skybox's HDR image on the CPU. It finds the
  brightest 3×3 block of a downsampled equirect, then the luminance-weighted centroid within 10° of it. It refuses skies whose
  peak is under 20× the mean luminance ("no distinct sun"). It creates the scene's main Directional Light, or aligns the existing
  one, setting its direction, colour and intensity (lux when the light uses physical units). It is one undo step.
- R29. **Remove Sun Disc**: an HDR texture import setting (optional field, default off). The cook clamps the pixels within
  the found disc (up to 5°) to the average of the ring around it, so IBL does not count the sun twice. Extract Sun offers it as
  an option, and both edits form one undo step.
- R30. **Environment Lighting › Source** gains **Gradient**, with Sky Color, Equator Color and Ground Color (Unity names).
  Existing scenes and a missing key keep today's behaviour as the default source. The gradient is projected to SH and added to
  the per-object SH the shader already reads, so no shader, variant or uniform is added.
- R31. Lua: `scene.ambientMode`, `scene.ambientSkyColor`, `scene.ambientEquatorColor`, `scene.ambientGroundColor`. MCP: the
  existing scene-settings or lighting tool gains the fields and an `extract_sun` action. Its result has direction, colour,
  intensity, the peak/mean ratio and the light's entity id.

### W8 — Area lights and IES profiles

- R32. `LightType::AREA` is appended (stored enum, append only). An Area Light has **Shape** (Rectangle, Disc), **Width** and
  **Height** (Rectangle) or **Radius** (Disc), all optional fields with defaults (1 m × 1 m, 0.5 m), and **Range**. It emits from
  one side along the light's forward axis, like the spot light, and the shape scales with Width/Height, not the transform scale.
- R33. Shading by Lit Quality:
  - Standard and lower: a most-representative-point approximation with energy normalisation (`intensity·A/(d²+A/π)` style),
    stable at contact.
  - High: LTC (Heitz et al. 2016) for diffuse and GGX specular, with the two 64×64 LUTs as RGBA16F textures (RGBA8 with scale
    where 16F is not filterable).
  - Both are evaluated in the tiled (S57) and per-object paths. On GLES3 the per-object path uses the light-records texture,
    never extra uniforms.
- R34. Units: Intensity in Nits when the light uses physical units, otherwise engine units, converted on the CPU as other lights
  are (`util/LightPhotometry.h`).
- R35. Shadows: an Area Light with Shadows on renders one perspective shadow along its forward axis (120° field of view) into
  the existing shadow atlas. It uses PCSS with the light size taken from the shape's half extent where the tier has PCSS, and PCF
  otherwise. Receivers outside the 120° cone are lit unshadowed.
- R36. **IES Profile**: `.ies` files (LM-63-1995 and -2002, `TILT=NONE`) import as an asset with a GUID in `.meta`. `TILT=INCLUDE`
  or a tilt file is refused with an error naming it. The profile is resampled to a fixed 2D grid (vertical × horizontal angle,
  using the file's symmetry) and normalised to a peak of 1. Profiles share one atlas texture, one row band per profile.
- R37. Spot and Point lights gain an optional **IES Profile** field, oriented by the light's rotation. The light's Intensity
  stays its peak, and the IES asset Inspector shows the file's peak candela. The asset Inspector draws a polar plot of the
  vertical profile and the file's metadata (manufacturer, lamp, lumens).
- R38. Editor:
  - The Light Inspector shows Type Area with Shape, Width/Height or Radius.
  - GameObject › Light gains **Area Light**.
  - The Scene view gizmo draws the rectangle or disc and its normal, with resize handles. Every edit is undoable.
- R39. Lua: `LightType.AREA`, `light.areaShape`, `light.areaWidth`, `light.areaHeight`, `light.areaRadius`, `light.iesProfile`.
  MCP: through the existing component tools (`set_properties`, `add_component`) and asset import. The guide's lighting page
  documents the fields.

### Platform and backends (all workstreams)

- R40. W2's checks and the W7 analysis are editor-only CPU work. W7's gradient and W8 run on GL 4.1, GLES3/WebGL2, Metal,
  D3D11 and Vulkan, with no compute, SSBO or integer textures. LUTs and the IES atlas are filterable float or RGBA8 textures.
- R41. A scene with no Area Light and no IES Profile compiles the same shader variants, binds no new textures, uploads no new
  uniforms, and renders the same pixels as before. The existing goldens are unchanged.
- R42. Shader variant lists (S64) include the area-light and IES variants a scene uses, so players and web builds draw them.

### Data and compatibility

- R43. Every new stored field is optional with a default, so no version bump and no migration. Old projects load and draw
  unchanged. `LightType::AREA` is appended. An old editor reading a new scene keeps the light, as with any unknown enum value it
  already tolerates (the behaviour is recorded).
- R44. New persistent writes (job history) go through `AtomicFile`. The call log is the only raw write, and it is allowlisted.

### Docs

- R45. `docs/mcp/guide/` describes findings, estimates, the download budget, the call log, area lights, IES and Extract Sun where
  an agent decides. `docs/mcp/TOOLS.md` is regenerated. `tools/mcp/budget.json` is respected (trade bytes or lower it). Each new
  piece of tool text has a case in `tools/mcp/eval/cases.py` that fails without it.

## 6. Design constraints and decisions

- No new MCP tool. Everything extends `screenshot`, `build_project`, job results, the scene-settings/lighting tool, component
  tools and asset import. Findings and reports are result fields, which the schema budget does not count. Any new parameter is
  traded inside the budget.
- Findings are warnings, never errors: a screenshot with findings still succeeds.
- The red control reverts in place and rebuilds incrementally (two builds), not in a fresh worktree (a full build).
- The LTC LUT data: use data whose licence is verified permissive, recorded in `NOTICE.md`. If none is clear, fit our own with a
  generator under `tools/` and add it to AGENTS.md's Generated files table.
- Test IES files are written by us (synthetic profiles), never copied from vendors.
- Area lights are not "baked only" as in Unity's built-in pipeline: they are realtime.

## 7. Acceptance criteria

- AC1 (W1). Table test in an existing MCP test file: misspelled tool, argument, nested key, enum, docs area and camelCase key
  each return the right single suggestion. An injected `std::out_of_range` and `bad_alloc` in a test-only tool return the fault
  advice, and `editor.log` holds the detail and id. Red on main.
- AC2 (W2). MCP test on a scratch scene that triggers each finding kind (an over-bright light, a black scene, a camera facing
  away, a selected object off-screen, a tiny object, a material with a missing texture, two coplanar quads), plus a clean scene
  with no findings. A gtest of the z-fighting plane-bucket check, and a timing report through `perfSlack()`.
- AC3 (W3). MCP test: a web and a desktop build return the size fields. A 0.001 MB budget gives `W-BUILD-OVER-BUDGET` with a
  `next`.
- AC4 (W4). gtest of each kind's model and the history median and clamp. MCP test: `record_game` returns `estimate` then
  `report`. A corrupt history file is tolerated.
- AC5 (W5). MCP test with `JASPER_MCP_LOG`: lines have exactly the fields, no argument text (a secret-looking argument is
  absent), and rotation works at a small test cap. A pytest of `call_profile.py` on a fixture log.
- AC6 (W6). pytest of `check_changed.py`: summary lines, fix lines, keep-going and skip on a failed build, `--json` schema, lint
  selection by path, and red control on a temporary git repo (red when the fix is reverted, exit 1 when the test does not pin the
  fix, restore after a simulated kill, byte-identical tree).
- AC7 (W7). gtest: Extract Sun on synthetic equirects (known sun direction within 1°; a no-sun sky refused). Gradient SH
  projection unit test. MCP test of `extract_sun` creating and aligning a light, with undo. A golden of a gradient-lit scene.
- AC8 (W8). gtests: LM-63 parser (both versions, symmetries, `TILT=INCLUDE` refused); representative-point and LTC energy
  against a numeric integral within 5 %. Goldens: Rectangle and Disc on Standard and High, a shadowed area light, and an IES spot.
  These run on the llvmpipe and GLES3 lanes. MCP test sets an Area Light and an IES profile via `set_properties`. All existing
  goldens unchanged (R41).
- AC9. Editor screenshots (from our own editor on a scratch project) of the Light Inspector (Area, IES), the area-light gizmo,
  the IES asset Inspector, Frame Checks, Extract Sun and the Build Report budget line.
- AC10. `tools/check-changed` green on the merged branch, `tool_budget.py --check` green, generators rerun, and `Check` green on
  the PR.

## 8. Dependencies and conflicts

- Merge hotspots:
  - `McpServer.cpp`: W1 catch chain, W5 call hook.
  - `docs/mcp/TOOLS.md`, `tools/mcp/budget.json`, `tools/mcp/eval/cases.py`: regenerate or merge last.
  - The light shaders and `LightComponent`: W7 SH, W8.
  - `BuildTools.cpp`: W3 sizes, W4 estimates.
  - The Project Settings window: W3 budget.
  - AGENTS.md and `docs/testing.md`: W6.
- W4's `build_project` estimate builds on W3's result fields when both land.

## 9. Risks and defaults

- **Findings that teach agents to ignore them.** Default: conservative thresholds pinned by tests; z-fighting needs a 1 mm
  overlap of real triangles.
- **Red control in a shared worktree.** Default: refuse when the tree is dirty, and document that it reverts in place.
- **LTC LUT licence.** Default: our own fit if the licence is unclear.
- **GLES3 uniform limits.** Default: light records through the texture path.
- **IES point lights in the per-object path.** Default: same atlas lookup, one fetch per light only where a profile is set.

## Deviations

- **W6: what the red control keeps.** Besides the paths R26 lists, it keeps the tools' own tests and their data
  (`tools/**/test_*.py`, `tools/tests/`, `tools/*/tests/`, `tools/*/fixtures/`), so a change of a tool's Python has a
  red control too. It reruns the steps that run tests (unit, HTML UI, MCP, the tools' tests, navigation OFF) and the builds
  they need; the area lints, header gates, the modules-off build and the web and Android builds are not tests and are not
  rerun.
- **W6: `--keep-going` is gone.** R23 makes keeping going the default, so the flag had nothing left to do.
- **W6: AC6's tests** extend `tools/test_check_changed.py`, which is stdlib `unittest` (pytest collects it too), because
  CI's lint lane and check-changed run it with `python3 -I` and no packages.
- **W2: out of frame without a selection.** The union of every visible renderer is mostly outside
  the frustum in any outdoor or first-person view (the ground, a terrain or a room around the eye
  always is), which would teach agents to ignore the finding. With nothing selected,
  `subject_out_of_frame` measures the union of the visible renderers whose projected bounds are in
  front of the eye and no larger than the image; the selected case is as R7 states.
- **W2: z-fighting candidates.** Meshes (`MeshComponent` leaves of the picking BVH) only: skinned
  and morphed meshes (their bind-pose faces are not what is drawn), instances of one renderer and
  terrains are not paired, and a mesh whose triangle BVH is still building is skipped. The plane
  buckets also carry a grid cell across the plane's dominant axis, so two dense parallel meshes
  are not compared face by face; same-facing means normals within 0.8 degrees.
- **W2: texture_missing referrers.** A visible renderer matches a missing-asset group through the
  `.material` files its submeshes link and through references its own scene file holds for it
  (a missing texture or material); references inside a prefab file are not mapped to its
  instances' GameObjects.
- **W2: severities and the budget finding.** `severity` is `warning` or `info`. The budget's info
  finding has kind `checks_skipped` and is kept when the list is cut to 5. Findings are computed for
  the `screenshot` tool, not for the images other tools attach with their `screenshot` argument.
### W1 — Names and faults

- **R1, nested keys and enum values are suggested, not refused up front.** A first version refused an unknown key
  of any closed object below the top level before the handler ran: 74 tests of the MCP CI suite failed, because
  listed schemas stay lean (`edit_pose_animation`'s `ops` items declare only `op`, closed, and the handler reads the
  rest). Enums are the same: handlers take aliases their enums do not list (`create_entity` type `box` for `cube`,
  `sun` for `directional_light`). So the registry refuses only unknown top-level arguments, as before. For a key of
  a closed object, or a string outside its enum (not even in another case or with other underscores), it adds the
  nearest declared name to the handler's own error, right after the handler's mention of it
  (`McpRegistry::suggestNearestNames`); a handler that ignores an unknown nested key still ignores it. A component
  property's enum (`McpValues::enumFromJson`) suggests one itself. AC1's nested-key case is `place_with_physics`
  `target`, whose handler refuses unknown keys.
- **R1, the matcher.** `NearestName.h` gains `NearestIdentifier` (a JasperEngine addition at its end, recorded in
  `NOTICE.md`): a fold-equal candidate (case, `_` and `-` ignored) first, then `NearestName`, then `NearestName` of
  the folded names. The CLI's `closeMatches` puts its answer first, so `call` suggests what the server does.
- **R2, no `get_tool_help`.** That tool does not exist; an unknown tool's error names `tool_groups`, which lists
  every tool by group. A `tools/call` of an unknown tool stays a JSON-RPC error (-32602), with the suggestion in its
  message; inside `batch` it is the step's error result with the same text.
- **R4, audit of `editor/mcp/tools/` (and `McpValues`, `McpRefs`).** No handler throws a `logic_error`-family
  exception with a message meant for the agent. The two engine helpers that do (`applyInputActionsOp`'s
  `invalid_argument`, `Catalog::getComponentType`'s) are caught by every MCP caller (`edit_input_actions`, `McpRefs`,
  `EcsTools`). Agent text that could reach `std::stoi`/`stoul`/`stoull` and throw `invalid_argument` or
  `out_of_range` (not the agent's message, and wrongly called "not caused by your arguments" now) is bounded
  first: `read_console` `since`, tileset and `AssetTools` hex colours, the F-key names of `BuildTools`,
  `submeshes[i]` and path indices in `McpValues`, the scene id of `jasper://scene-view/{scene}`. The remaining
  `out_of_range` throws (ComponentArray, Tilemap, LayoutGroup, Model, Text) are internal faults.
- **R3, the fault path.** Continuations (`waitUntil`, `afterFrames`) and the off-main-thread tool share the
  handler's catch (`McpServer::failFromException`). The read tool named is the failing tool's group's get_/list_/find_
  tool sharing most of its noun words, else that group's first one, else `editor_status`. The test-only tool is
  `selftest_echo` with a `throw` argument (the existing `JASPER_MCP_SELFTEST_GROUP` group).
- **Download Budget measure (W3).** A web build is held to `download_bytes_gzip`; a Mobile or Desktop build to
  `build_bytes`, the build report's files as they are measured before packaging (the report does not measure an APK, AAB or
  store download). The rows are labelled **Download Budget MB (Web|Mobile|Desktop)** under Player › Publishing Settings,
  keys `player.download_budget_mb.<family>`; S53's per-scene `quality.render_budgets.<family>.download_mb` (cooked model
  files a scene loads) is unrelated and unchanged.
- **W8, R33 Standard diffuse:** the representative direction and the energy come from the shape's
  vector form factor (4 edges for a Rectangle, 8 for a Disc, each a rational fit instead of an
  acos), clipped by the receiver's horizon as a sphere of the same vector in closed form, not from
  a closest or centre point: those were 10-30 % off at one to two light sizes, the form factor is
  exact while the light is whole above the horizon. The specular is the representative point with
  Karis's energy normalisation and the `A / (d^2 + A / pi)` irradiance, as written. Measured
  (`AreaLightMath.*`): the form factor and the LTC diffuse within 5 % where the horizon leaves the
  light whole and 15 % where it cuts it; the LTC GGX within 5 %; the representative point's
  specular energy within 5 % in the point-light limit and 0.6-1.3 of the integral for a 1 m light
  at 2-4 m.
- **W8, R33 LUTs:** the LTC tables are `selfshadow/ltc_code`'s fit (BSD-2-Clause with a request to
  cite the paper, verified and recorded in `NOTICE.md`), converted bit for bit by
  `tools/gen_ltc_lut.py`; no fit of our own. They are RGBA16F only, sharing one texture
  (`u_lightProfiles`) and sampler with the IES bands: RGBA16F filters on all five backends (GLES 3.0
  and WebGL2 core), so the RGBA8 form has no device to run on.
- **W8, R33 Disc:** a Disc is a regular polygon of its area: an octagon for the diffuse on every tier,
  sixteen sides for High's LTC specular (an octagon showed in sharp reflections).
- **W8, R33 lobes:** a material with extended lobes (clear coat, sheen, ...) lights them from the
  diffuse's representative direction on every tier; LTC is the base GGX's on High.
- **W8, R33 other shaders:** `USE_AREA_LIGHTS` and `USE_IES` are the built-in Lit's (and its
  forks') defines. Library shaders (water, custom library shaders) and vertex lights draw an Area
  Light as a spot of its front hemisphere at its centre (its records keep that reading) and ignore
  IES Profiles. Contact shadows do not march towards an Area Light.
- **W8, R36 refusal:** an `.ies` file is imported as a copy with a `.meta` GUID; a `TILT=INCLUDE`, tilt
  file or non-Type-C profile is refused where it is used: `set_properties` fails naming it, the
  asset Inspector shows the reason and the renderer warns once and draws the light without it.
- **W8, R37 orientation:** the horizontal angle 0 lies along the light's right (local X), 90 along its
  up (local Y).
- **W8 batching:** a submesh whose variant takes `USE_AREA_LIGHTS` or `USE_IES` is not batched by
  automatic instancing (as any builtin:lit define but the High Lit Quality).
- **W8, R43 old editors:** an editor without `area` reads the type as an unknown enum value: the
  value is kept with the component (`StreamPreserved`, "kept by S03") and the light loads as
  Directional, the enum's fallback; inferred from the code, not run.
- **W8, R40 bind limits:** a variant the light profiles texture would take past 16 fragment textures
  or samplers (on GLES3, a material with every extended lobe and its maps, shadows, IBL and the spot
  masks left out already) is built without it (`LIGHT_PROFILES_OFF`, a `light-profiles-left-out`
  build line): its IES lights draw with their cones and its Area Lights with the representative
  point. Elsewhere the profiles share the spot masks' sampler.
### W4 — Job estimates and reports

- **R16's progress bar is the footer's.** No editor progress bar showed MCP jobs, so the footer (the
  bar along the editor's bottom, with the Console line and the FPS) gains one per running job with
  an estimate (at most two): its progress, or the share of the estimate elapsed when the job reports
  none (record_game), and "N s left" from `jobRemainingSeconds` (the estimate early, the progress
  late). Its tooltip names the basis and confidence. (`StatusBar` is not used: the footer is drawn
  over it.) `get_job` carries `estimate` and `remaining_seconds` in `structuredContent`; its text
  is unchanged.
- **The fallbacks are the downgrades the code can take, no more.** record_game: another H.264 encoder
  than libx264, fps or size clamped to the Recorder's limits, the size clamped to the GPU's maximum
  texture size or to even sizes for 4:2:0, audio dropped (started mid-Play, or the deterministic
  driver failed), frames recorded without waiting for what does not settle (S62). build_project:
  Auto compiled instead of using the prebuilt player (with the reasons), a bigger player variant
  than the one that fits, textures R6 left uncompressed (per family, with a count). A texture's
  Max Size clamp is the user's setting, not a downgrade, and is not listed. bake_impostors: view
  passes that could not be read back (the only downgrade its code has). export_title: the
  `output_downscaled` warnings (an output over its max_size).
- **History kinds are finer than the four tools.** Times differ by minutes across platforms and
  methods, so the kinds are `record_game:<format>`, `build_project:<platform>:prebuilt|compiled`
  (and `:source`), `bake_impostors`, `export_title:<format>` and `export_title:bake:dry_run`. A
  build that compiled against a prebuilt estimate records its measure against the compiled model.
  Only successes are recorded. `estimate.bytes` is given where a model of the output exists
  (recordings, impostor atlases, baked title PNGs, a prebuilt build's player plus assets).
- **export_title format glb is not a job** (it answers in one call), so it has no estimate.
- **W7: Unity's names, `ambientMode` Color and Gradient (the lead's decision, shared with S94).** The
  source is `SceneSettings::ambientMode` (`AmbientMode::COLOR` default, value 0, and `GRADIENT`, 1;
  stored `ambientMode: color|gradient`, append only), with `ambientSkyColor`, `ambientEquatorColor` and
  `ambientGroundColor`: the keys S94 (#19) uses, so whichever lands second keeps the first one's
  instead of its own. Today's shading (read in `mesh.frag`) is the flat Ambient Color, which the
  built-in Lit with a sky image swaps for the image's diffuse ambient (IBL); library shaders, vertex
  lighting and particles always take the colour. That stays **Color**: no Skybox value is added, as a
  Skybox source that dropped the colour where there is no sky image, or a Color source that dropped a
  sky image's diffuse ambient, would need the IBL variant without its specular, a shader change R30
  rules out; either can be appended later. The Inspector shows Ambient Color and Intensity under Color
  (its tooltip says Lit takes a sky image instead) and the three colours under Gradient.
- **W7: the Gradient's colours are used as they are, with Unity's defaults.** As Unity's Gradient (its
  Intensity Multiplier is the Skybox source's), Intensity does not scale them, and they default to
  Unity's sRGB (54, 58, 66), (29, 32, 34) and (12, 11, 9).
- **W7: Gradient under a sky image.** The gradient replaces the sky's diffuse ambient: the meshes bind a
  black irradiance map while the scene has lights (with none, Lit takes no SH and keeps the sky's
  diffuse ambient), and the frame's flat ambient Lit subtracts is 0, so Lit adds the object's whole SH.
  The sky's prefiltered map still gives reflections; the SH's specular term Lit adds for the folded
  lights then includes the gradient too, a small extra ambient specular. Particles and the clouds take
  the gradient's average.
- **W7: cooked scenes.** The ambientMode and the gradient are appended to the scene settings record as format
  1.10 (`CookedScene.h` `FORMAT_MINOR`, the next free minor after S96's 1.6, S98's 1.7, S78's 1.8 and S92's 1.9);
  an older record reads as Color with the default gradient. With S98 every build loads the cooked scene, so
  compiled builds take them from there (no generated scene code). Numbers taken at landing: shader cache
  `v70`, `AssetType::IesProfile = 36`, the variant hash's environment bits 36 and 37.
- **W7: Remove Sun Disc per image.** The cook sees one image at a time: a cross finds the disc across its
  six faces, but a six-file sky finds it in each face file on its own, so a disc split by a face edge
  is removed in each face where that face's part is distinct (20x its mean). An image without a
  distinct sun cooks unchanged, with a note in its cook record. Extract Sun sets the setting on every
  image of the sky.
- **W7: the CLI** is `jasperengine-editor call set_scene_settings` with `extract_sun`, as every MCP tool
  (no subcommand of its own).
