# Doriax catch-up program: ledger

Owner request (2026-11-04): implement in JasperEngine every fix and improvement worth taking from Doriax (the engine
JasperEngine was forked from, at `29348d7b4` on 2026-09-29), with the best UX for human users (editor, Inspector,
menus, undo) and AI agents (MCP tools, the agent guide, docs, Lua stubs). Product decisions are delegated to the lead
agent ("be braver"): what most users and agents expect of a game engine, Unity names and behaviour first.

## Goals and requirements

- Take every Doriax bug fix that applies to JasperEngine, every place Doriax does something better, and the new
  Doriax features worth having; fix the defects of our own that the comparison found.
- Each feature has a spec (goals, requirements, design, tests, acceptance) in
  `docs/superpowers/specs/2026-11-04-<feature>-design.md`. No separate implementation plans.
- Each feature is implemented on its own branch `dc/<feature>`, in its own worktree
  (`/var/lib/orquester/workspaces/jaspersito/dc-wt/<feature>`), by its own agent.
- A feature is done when it is complete and correct: every acceptance criterion met, built, tested (red-before-green
  for fixes), reviewed, docs and MCP docs regenerated. Then it is merged into `main` together with `origin/main`, the
  merged tree is built and tested, and `main` is pushed.
- Project rules hold (CLAUDE.md, AGENTS.md): all five graphics backends and all export targets; disabled costs
  nothing; editor UI with undo, Lua API, MCP tools; old projects load (except the owner decisions below).

## Owner decisions (2026-11-04)

- Built-in AdMob is retired entirely. No compatibility: no project uses it (new engine).
- Windows moves to the UTF-8 code page, without a Windows test first. Old projects with ANSI-encoded YAML are not
  supported (none exist).
- No rented machines: everything builds and runs on this host (8 cores, 15 GB RAM, 32 GB swap added), in Docker
  (`tools/build.sh`), installed for this program.

## Sources

- Comparison (2026-11-04): all 319 Doriax commits since the fork triaged by 22 agents, actionable findings
  re-checked by 17 skeptic agents; ledger of every commit with evidence and verdicts in
  `/var/lib/orquester/workspaces/jaspersito/dc-work/analysis/ledger-verified.json` (outside the repo; summary in
  [COMPARISON.md](COMPARISON.md)).
- Doriax read-only clone: `/var/lib/orquester/workspaces/jaspersito/dc-work/analysis/doriax` (`3fb49fbba`).

## Lead decisions across specs (binding on every branch)

- **Escape in Play** (runtime-core and scene-integrity both touch it): Unity's behaviour. A focused Game view
  delivers Escape to the game, and if the game captured the cursor, the same press also releases the capture.
- **Shader variant and update-flag bits:** taken as the next free bit at landing, in landing order, on both
  programs (draw-perf's `Dlo`, water-boats' `Wex`/`Wwk`, ui-components' `UpdateFlags_Button` keep their names); the
  lead renumbers at merge if two branches collide, with a test pinning each assignment.
- **Shader cache version** (`editor/App.cpp`): each branch that changes shaders bumps it by one over its `main`;
  the lead re-bumps at merge so every merged change gets its own version.
- **Capacities:** mesh-memory removes the fixed component capacities, so export-pipeline does not implement
  Doriax `b1df76163` (capacity counting for script-only prefabs). Merge order: mesh-memory before draw-perf before
  sprite-batching; water-reflections before water-boats; unicode-text before ui-components (character input).
- **Agent guide budget** (`editor/mcp/McpInstructions.cpp`, 4,096-character client cut found by agent-authoring):
  only agent-authoring restructures the guide. Every other feature puts agent guidance in `docs/mcp/guide/<area>.md`
  and adds at most a one-line pointer, trimming elsewhere if needed.
- **Pixel-art program** (S41-S52, `PIXEL-ART-LEDGER.md`, pushed to `origin/main` from another machine). Split agreed
  with its lead on 2026-11-04: our `sprite-batching` owns generic dynamic batching (sprites, small meshes, UI quads,
  shared buffers, settled objects, 2D reordering, batching stats in `get_render_stats`); their S52 shrinks to
  Sorting Layers / Order in Layer / Sorting Groups, the Sprite Batch component, and making pixel-art per-sprite
  parameters batch-compatible inside our batcher, landing after us. Our batcher therefore takes a sort key that S52
  extends and a documented per-vertex extension for per-sprite parameters. mesh-memory vs their S44 (sprite frames
  as sheet references): whoever lands second merges onto the first. Asset types 24-27 are theirs (we add none).
  Shader and variant bits on both sides are appended at landing in landing order (the bit numbers in our specs are
  names, not reservations). The lean MCP rules merged with it (`fe3e90b97`: guide at most 8,192 bytes, eval cases
  for new text) override our specs; agent-authoring revises its guide plan against them.
- **Two more programs share `origin/main`.** Forge/meshoptimizer (S53-S57, `docs/forge-meshopt-analysis/LEDGER.md`)
  defers to us: draw-perf owns the opaque state sort, the pipeline/binding tracker and cull-before-sort (their S55
  instances over that order); sprite-batching's dynamic batching goes first and S55's GPU instancing takes the
  remaining mesh draws, never both; S53-S55 keep component changes additive around mesh-memory. Follow-ups (S61-S73,
  `FOLLOWUPS-LEDGER.md`): S68 Linked folders makes the export walk follow links (overrides export-pipeline's rule;
  export-pipeline follows links with a cycle guard), S64 meets export-pipeline's shader keys, S67 meets
  mobile-shipping's packaging, S65 leaves capacities to mesh-memory, S72's header fan-out cut is a merge hotspot.
  Whoever lands second merges onto the first.
- **Generated files** (`docs/mcp/TOOLS.md`, `tools/mcp/budget.json` measurements, `catalog_snapshot.json`, Lua
  stubs): each branch regenerates them after its last merge of `main`; at merge the lead regenerates again.

## Features

Status: `spec` (being written) / `specced` / `impl` (being implemented) / `review` / `merged` (on origin/main).

| # | Feature (branch `dc/<name>`) | Covers | Status |
|---|---|---|---|
| 1 | `unicode-text` | UTF-8 code page on Windows, console/log encoding, saved-path slashes, Apple UTF-8 paths, emoji input, CJK editor fonts | review |
| 2 | `mobile-shipping` | retire AdMob, iOS/macOS privacy manifest, prebuilt exports keep native plugins, Apple build fixes | impl |
| 3 | `export-pipeline` | failed copies, exports inside exports, shader keys, never-saved scenes, capacities, loading scene, D3D11 bytecode, visible build errors, web index/favicon | impl |
| 4 | `build-toolchain` | CLI recent projects, ; and # in titles, exec bits, generated .gitignore, Box2D/Jolt for C++ scripts, /MP, lib-dir defines | impl |
| 5 | `scene-integrity` | save over another scene, scene ids, scenes inside the project, child-scene references, model import order, Play scene view, Save All | impl |
| 6 | `project-window` | Paste target, drag selection, Duplicate, Create scripts, numbered names, texture slot names, asset-folder relocation | impl |
| 7 | `ui-components` | button textures, layout boxes, 9-patch labels, canvas size, Selectable navigation and transitions | specced |
| 8 | `runtime-core` | subscriber exceptions, dead spawn roots, clip speed, hidden children, controller-hit cleanup, preview edits, start offsets | impl |
| 9 | `scripting` | Add Script for Lua, getCorners, property refresh, vector moveTowards/lerp, Model API on parts, subsystem bindings | impl |
| 10 | `render-correctness` | bounds, normals, Points depth, G-buffer variants, UV sets, foliage ring and margins, shader loops, gizmo | impl |
| 11 | `mesh-memory` | MeshComponent memory cut by an order of magnitude without lower capacities | impl |
| 12 | `draw-perf` | redundant GPU state, sort by state, moved-object math, no scans for features that are off | specced |
| 13 | `sprite-batching` | automatic (dynamic) batching of sprites, small meshes and UI quads, 2D reordering, settled objects, content-shared buffers, no 8k-sprite ceiling, batch stats | impl |
| 14 | `ssr-probes` | SSR under local probes and AO, early-out | impl |
| 15 | `water-reflections` | planar reflection, transparents under water, waves, shadows, forks | impl |
| 16 | `water-boats` | exclusion volumes and wakes | specced |
| 17 | `platform-lifecycle` | hidden/minimized games, macOS launch, web audio and keys, Android Gradle | impl |
| 18 | `agent-authoring` | agent guide rules, foliage and blend-map tools, camera, rotations, shader overrides, save holds | impl |
| 19 | `code-editor-git` | VCS markers and diffs in the code editor, footer state, scene default shader uniforms | impl |

## Log

- 2026-11-04: comparison finished and reported (84 must/should items, 80 optional). Owner decisions recorded above.
- 2026-11-04: owner asked to implement everything. Fetched `origin/main` (no new commits; `main` = `b4f7816ea`).
  No toolchain or Docker on this host: installed Docker 29.1.3, added 24 GB swap (32 GB total). Baseline build of
  `main` (`tools/build.sh linux`, then `unit-tests`) started. 19 features defined; 19 spec agents started.
- 2026-11-04: all 19 specs written (606 to 1,019 lines, 11 to 21 acceptance criteria each), with the cross-spec
  decisions above. Builds run through `slot` (2 slots of 4 CPUs). Implementation starts in waves, ordered by the
  merge order above; agents work in `/var/lib/orquester/workspaces/jaspersito/dc-wt/<feature>`.
- 2026-11-04: pushing the specs found 9 new commits on `origin/main` from the parallel pixel-art program (S41-S52
  specs, lean MCP surface). Merged (`59c2a964f`) and pushed. Feature 13 rescoped (see decisions). Wave 1 started
  (12 features): mobile-shipping, unicode-text, export-pipeline, build-toolchain, scene-integrity,
  project-window, runtime-core, scripting, mesh-memory, render-correctness, agent-authoring, code-editor-git.
  Also started, as they depend on nothing in flight: ssr-probes, water-reflections, platform-lifecycle (15 agents).
  Waiting: ui-components (after unicode-text), draw-perf (after mesh-memory), water-boats (after
  water-reflections).
- 2026-11-04: agreed the split with the pixel-art lead (see decisions): feature 13 is `sprite-batching` again, with
  hooks for their narrowed S52, and started now (16 agents) because their S52 waits for it.
- 2026-11-04: `origin/main` moved again (`815791c80`..`297562c95`): AGENTS.md rewritten as the one rule book, the
  GitHub workflows removed (no CI: nothing checks Windows, macOS or iOS code except reading), and the push gate is
  now the full suite for big features (`WERROR=1` linux and unit-tests, the slow MCP suite, area lints). Merged into
  `main` (fast-forward) and pushed; the brief's definition of done follows it, and all 16 implementers were told to
  merge `main` before their next gate. Baseline of `main` on this host: `tools/build.sh linux` passes with 11
  warnings (ODR warnings of the vendored shader compiler's SPIR-V headers, as the pixel-art lead measured).
- 2026-11-04 07:0x: merged 11 more `origin/main` commits (follow-ups program, CI-suite classification) and pushed.
  Recorded the splits with the Forge/meshoptimizer (S53-S57) and follow-ups (S61-S73) programs; told export-pipeline
  (S68 links, S64 shader lists), project-window (S68) and mobile-shipping (S67). Build queue: `slot` became a
  priority queue (mesh-memory first, then unicode-text, water-reflections). Progress: unicode-text, runtime-core,
  render-correctness, water-reflections, scripting, agent-authoring, build-toolchain and mobile-shipping have
  committed work and are in red/green runs; the others have code written and wait for their first build. Nothing
  merged yet.
- 2026-11-04 14:xx: the host disk filled (0 B free): every worktree's full gate leaves about 40 GB of build trees.
  Freed 58 GB (a 29 GB scratch copy, Docker's build cache, an unused 24 GB swap file, the main checkout's baseline
  build); disk rules added to the brief; the lead deletes a feature's build trees once it merges. `slot` reworked
  twice for fairness: slot 1 serves the critical path (mesh-memory, unicode-text, water-reflections), the other
  slot the longest-waiting job; queued jobs resubmitted with their original queue time (`DC_SINCE`). Merged 65
  upstream commits (follow-ups S61, S66, S71, S38 M1) into `main`. Closest to landing: mesh-memory (final gate),
  unicode-text (linux WERROR clean, unit 5676 run / 0 failed), runtime-core (linux WERROR clean).
- 2026-11-04 16:1x: gate corrected (owner's question). The brief had required every branch to run the full unit
  suite and the full slow MCP suite, which AGENTS.md reserves for pushes to `origin/main`: the most expensive work
  ran once per branch on a two-slot host. Per-branch gate is now `tools/check-changed` plus the spec's own tests,
  red controls and lints; the lead runs the full suite once at landing, on a local `main` holding one or several
  ready features. All 16 agents told to trim queued jobs (keeping their queue place). mesh-memory finishes the full
  run it is already hours into; it counts toward its landing.
- 2026-11-04 21:xx: **unicode-text** finished its gate (`check-changed`: Linux, web, Android builds; editor 2898,
  core 2879 with one load flake that passed on rerun; HTML UI 28/28; MCP files; red controls all red). Independent
  review: no blockers or compile errors found by reading the Windows/Apple code; 3 should-fix defects (logger
  re-entry overwriting a repaired record, ANSI re-decode on the UTF-8 code page, crash-report paths on Windows
  before 1903) and 4 minor items, being fixed. Integration branch `dc/integration` created; a trial merge into
  current `main` conflicts only in `tools/mcp/eval/cases.py`. Disk: a janitor now deletes idle worktrees' Android and
  web outputs when free space falls under 25 GB (Android builds leave 25 GB each).
- 2026-11-05: the session's account hit its weekly limit, which stopped every agent mid-run (about a day lost). Found
  the disk full (0 B) and 14 orphaned queued jobs: cancelled them, deleted every worktree's build trees (they are stale
  after 304 upstream commits), raised the shared ccache to 40 GB. Merged `origin/main` (the Forge/meshoptimizer
  program landed S53-S57, including S55's automatic GPU instancing; follow-ups landed S63, S64, S68, S69, S70, S73
  and more). Owner's rule: no full suites anywhere; the brief's gate is now the WERROR editor build, the feature's
  own tests and red controls, filtered affected suites, compile-only web/Android when relevant, and lints. Agents
  revived in batches, closest to landing first.
- 2026-11-06: a second stop (the session account's login was revoked, about 17 hours lost). The slot queue kept
  running the agents' detached jobs meanwhile. Merged 187 more upstream commits (the sound layer S35-S40) into
  `main` and pushed; all 16 agents revived to read their job results, merge `main` once more, finish and report.
- 2026-11-06 18:3x: owner's top priority: mesh-memory, sprite-batching, unicode-text, platform-lifecycle (in that
  order), and permission to rent Hetzner VMs. `slot` now runs only priority order while a priority-1 job waits.
  Rented two Hetzner cpx51 boxes (16 vCPU, 30 GB, 48 h limit, about $0.44/h each): box A (mesh-memory) and box B
  (sprite-batching). `rbox <A|B> '<cmd>'` syncs a worktree's current files to the box and runs the command there
  (one job per box, JOBS=14). unicode-text and platform-lifecycle finish their running local jobs.
