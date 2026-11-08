# Doriax catch-up follow-ups (2026-11-07)

The owner asked for these follow-ups of the 19 catch-up features to be done. Each workstream has its own agent,
worktree `/var/lib/orquester/workspaces/jaspersito/dc-wt/fu-<name>` and branch `fu/<name>`, made from `main`
(4aecb677d, which includes origin's navigation and pixel-art landings). The lead reviews, merges and pushes.

## Brief

[AGENT-BRIEF.md](AGENT-BRIEF.md) applies in full, with these changes:

- **Boxes:** four 16-core boxes A to D (`dc-work/BOXES.md`). Use `rbox auto '<round script>'` for every round,
  `rbox auto --fetch` for results. Local `slot` is the fallback only. Nine agents share four boxes, so keep each
  round short and complete, and don't run a job that answers no question.
- **Your item list is your spec.** Read the original spec (`docs/superpowers/specs/2026-11-04-<feature>-design.md`,
  especially its Deviations), the feature's LEDGER.md entry, and the code. When an item is a "possible improvement",
  build it if it's right for users on every backend and target; if your measurements show it's wrong, don't build it,
  and say why with the numbers. Record decisions in a `## Follow-ups (2026-11-07)` section appended to the original
  spec.
- **Shared registries:** pixel-art (S41) takes MESH bit 45. Add any new shader property bit after the highest one on
  `main`, and bump the shader cache version once. The lead renumbers both at landing when two workstreams collide,
  so say in your report which you took.
- **Don't touch** the submesh culling in `RenderSystem.cpp` (S55 `localBounds` stride) or
  `engine/core/render/VisibleList.cpp` until another program's `fix/submesh-drop` lands on `origin/main`
  (`git fetch origin` and look for it in `git log origin/main`).
- **Windows, macOS and iOS can't be compiled here.** If an item needs one of them, say exactly what must be run where.
  The lead batches those into one rented Windows or Mac check at the end.
- Final report: at most 300 words, as AGENT-BRIEF.md's Definition of done says.

## Workstreams

| workstream | items | status |
|---|---|---|
| water-uniforms | The 16-light water variant needs 245 to 248 fragment uniform vectors; GLES3/WebGL2 allows 224. 30 base water variants are over the uniform-block budget. Both came with water-reflections. | in progress |
| water-tests | Particle custom blocks get frame time, not the scene clock (`RenderSystemParticles`). Regression tests for the four water-reflections fixes that have none: per-pass terrain view, HUD refraction, MVP rewrites, target sizing. | merged (train 1) |
| pcss-gles | PCSS soft shadows on GLES3/WebGL2, using the texture units draw-perf's directional-only variant frees (draw-perf spec §13). | in progress |
| mesh-memory | Compile the Android heap-trim branch (`tools/build.sh android`). Check the MSVC size limits. Explain the 421-pixel difference in the early clone test. Save about 1.6 KB per submesh in S55's warm tables, only after `fix/submesh-drop` lands. | merged (train 1) |
| ssr | Run SSR before transparents and UI (shared with water-reflections' colour-pass order). Handle display-encoded colour under a tone curve. Store a higher-precision shading normal in the G-buffer; all 4 render targets are used, so repack. | in progress |
| lints-keys | The LOD lint misses local per-fragment variables (render-correctness). The event-clear lint doesn't cover events outside components and Engine (runtime-core). F11 clashes with macOS Show Desktop (project-window). | merged (train 2) |
| agent-authoring | An FBX foliage layer draws 0 instances in the headless editor (a foliage-loader question). The generated project AGENTS.md sits near its 5,000-byte cap (5,153 was measured once). | in progress |
| platform-build | macOS CMake builds copy no plugin bundles. Carried-plugin lists include `.meta` files (mobile-shipping). `/MP` isn't capped, so Visual Studio can run N×N compilers (build-toolchain). | merged (train 1) |
| code-editor-git | Subversion scene diffs, and the `diff_scene`/`diff_file` defaults flagged in the spec. Prebuilt players compare only the major format version. | merged (train 1) |

## Log

- Train 1 (2026-11-08): fix/main-shared-data, water-tests, mesh-memory, platform-build, code-editor-git. Check on box D:
  WERROR linux, core and editor units (branch tests plus shared-data suites, `ReflectLocators.*` and `Stream.*` added), 11
  MCP files, lints, test_grade, tool budget, TOOLS.md check, web compile: all green after the SpriteComponent ceiling
  (S44's tag playback grew it to 320 bytes; ceiling 384). mesh-memory's S55 table item stays open until
  `fix/submesh-drop` lands. Still to run on real hardware: MSVC Debug/Release `MeshMemory.*`, a Visual Studio export timed
  both ways, a Mac export with signed frameworks (commands in the specs' Follow-ups).
- Train 2 (2026-11-08): lints-keys alone, on box D: WERROR linux, core and editor units (its tests plus shared-data),
  8 MCP files, lints, test_grade, tool budget, TOOLS.md check, web compile: all green. Not run: macOS (`BackendMac.mm`).
