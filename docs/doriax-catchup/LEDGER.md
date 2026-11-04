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
- **Mesh shader variant bits:** bit 36 is draw-perf's directional-only Lit variant (`Dlo`); bits 37 and 38 are
  water-boats' exclusion and wake variants. Other new variant or update-flag bits take the next free bit on their
  branch; the lead renumbers at merge if two branches collide, with a test pinning each assignment.
- **Shader cache version** (`editor/App.cpp`): each branch that changes shaders bumps it by one over its `main`;
  the lead re-bumps at merge so every merged change gets its own version.
- **Capacities:** mesh-memory removes the fixed component capacities, so export-pipeline does not implement
  Doriax `b1df76163` (capacity counting for script-only prefabs). Merge order: mesh-memory before draw-perf before
  sprite-batching; water-reflections before water-boats; unicode-text before ui-components (character input).
- **Agent guide budget** (`editor/mcp/McpInstructions.cpp`, 4,096-character client cut found by agent-authoring):
  only agent-authoring restructures the guide. Every other feature puts agent guidance in `docs/mcp/guide/<area>.md`
  and adds at most a one-line pointer, trimming elsewhere if needed.
- **Pixel-art program** (S41-S52, `PIXEL-ART-LEDGER.md`, pushed to `origin/main` from another machine): S52 (Sprite
  instancing: batched sprites, Sorting Layers) owns sprite batching. Our feature 13 is rescoped to `ui-batching`:
  component UI quads (S52 leaves them out) and content-shared buffers for non-sprite meshes, implemented after S52
  lands. The lean MCP rules merged with it (`fe3e90b97`: guide at most 8,192 bytes, eval cases for new text) override
  our specs; agent-authoring revises its guide plan against them.
- **Generated files** (`docs/mcp/TOOLS.md`, `tools/mcp/budget.json` measurements, `catalog_snapshot.json`, Lua
  stubs): each branch regenerates them after its last merge of `main`; at merge the lead regenerates again.

## Features

Status: `spec` (being written) / `specced` / `impl` (being implemented) / `review` / `merged` (on origin/main).

| # | Feature (branch `dc/<name>`) | Covers | Status |
|---|---|---|---|
| 1 | `unicode-text` | UTF-8 code page on Windows, console/log encoding, saved-path slashes, Apple UTF-8 paths, emoji input, CJK editor fonts | impl |
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
| 13 | `ui-batching` (was `sprite-batching`) | batching of component UI quads and buffers shared by content for non-sprite meshes; sprite batching itself is pixel-art S52's | specced (rescope pending) |
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
  water-reflections), ui-batching (after pixel-art S52).
