# S91 — Effect authoring: the Effect window, filmstrips, and the missing VFX features on S20

**Wave / Depends on.** Authoring wave (spec-first). Extends S20 on `main`, after **BF23**. M5's
sound events need S37 to land first. Independent of S90. Owner area:
`editor/window/ParticleEffectWindow.*` (new), `editor/mcp/tools/ParticleTools.cpp`,
`engine/core/particles/`, `engine/core/subsystem/RenderSystemParticles.cpp`,
`engine/shaderlib/library/`, `docs/mcp/guide/particles.md`.

**Basis of this spec.** The current state below comes from reading the code at `b4f7816ea`. Nothing
was built or run. Effort figures are estimates.

## 1. Goal

S20 gave us a large, deterministic CPU particle system, but authoring an effect is still slow for a
person and partly blind for an agent. This spec adds the authoring experience that makes Effekseer
fast, on our runtime: a dedicated window, editing that holds the current frame, timing bars, and a
deterministic filmstrip with verdicts shared by humans and agents.

It also fills the real feature gaps: drawing sub-emitter children, distortion, a ring renderer,
triggers, cheap dynamic parameters and sprite-sheet baking.

**Not a goal: running Effekseer.** The owner decided that neither Effekseer's effects nor its editor
are required, so no Effekseer code, shader, format or sample is taken. Only ideas are.

## 2. Scope

**In scope:** the Effect window; `preview_particle_effect`; the listed runtime features, each with
Inspector UI, Lua where it is runtime behaviour, MCP and tests.

**Out of scope**

- A second particle runtime, an `.efkefc` importer, or GPU particles (compute, excluded by
  `S20:43`).
- A node or graph VFX editor. `S20:40` excludes it and this spec keeps that: Effekseer's model is a
  tree of emitters with time-driven parameters, which does not need a dataflow graph.
- **`.particles` schema v2 with several emitters in one file** (D1). Revisit if the tree view proves
  insufficient.
- Effekseer's frame-based time units, its custom equation language, its IronPython scripting, one
  panel per module, and binary project files.
- Lit trails, and a track renderer in Effekseer's form. Trails and the Line and Trail Renderer
  components already cover most uses.

## 3. Current state

- **Runtime.** 23 Unity-named modules, 8 billboard modes, mesh particles, ribbons, trails,
  collisions, 5 force-field kinds, soft particles, distance LOD and a deterministic fixed tick.
- **Sub-emitters look undrawn.** `drawParticles` packs only `state.runtime`
  (`RenderSystemParticles.cpp:759,802`), GPU objects exist once per root state
  (`ParticleSystemState.h:43,136`), and only tests call `child()`. Their tests check counts and
  events, never pixels. Absence of the path is confirmed by reading. Invisibility at run time is
  inferred, so R1 starts with a capture.
- **Composition.** One `.particles` file holds one emitter (`ParticleEffect.h:535-586`). Effects
  compose through a GameObject hierarchy or through sub-emitters that reference other files.
- **Editor.** A module Inspector, Curve and Gradient windows, a Scene view overlay with scrubbing,
  an asset preview (`ParticlePreviewRender`) and a Unity importer. A `.particles` double-click opens
  as text. Selecting a parent without its own system previews nothing unless the overlay is set to
  All.
- **Agents.** 12 MCP tools. Authoring by validated patch works. `capture_particles` mutates the live
  system, needs an entity and takes one frame per call (`ParticleTools.cpp:542-559`). Stats omit
  `stateHash` and children. Triggers are Lua-only.
- **Silent failures.** A misspelled patch field is dropped and its warning lost
  (`ParticleEffectCodec.cpp:501-504`, `ParticleTools.cpp:305-308`). `.particles` is untracked by
  `McpAutoSync` (`McpAutoSync.cpp:165-190`), and `read_file` and `write_file` refuse it.
- **Dynamic parameters.** `exposed[]` works, but each change recompiles the effect
  (`ParticleSystem.cpp:315-322`, `ParticleSubsystem.cpp:186`).
- **Spec drift.** The S20 deviation "S27's curve widget has not landed" is stale,
  `generateLightingData` has no effect, the world budget has no project setting (`setBudget` has no
  callers), and mobile performance rows are unmeasured.

## 4. Reference: Effekseer at `b5700082b` (MIT, ideas only)

Treat a clone as untrusted: read it, never run it. Worth studying: the F-curve panel, re-simulation
to the same frame on an edit with the same seed, inline value modes, and its recorder's outputs. A
usage sample of 1,687 of its nodes found easing on 47% of scale values, and F-curves on under 1.1%
of position, rotation and scale. So inline modes do most of the work, and easing matters more than a
better curve editor.

Weaknesses not to copy: "Spawn Rate" that is really an interval, a burst trick using a rate of
0.0001, "Gauss" that is a uniform range, single-node selection, no gizmos, a visibility eye that
changes the export, a CLI that exits 0 on failure and exports the default effect when input is
missing, and a recorder that is GUI-only.

`LICENSE_TOOL` lists bundled parts not under MIT, and its F-curve widget has no licence header.
Nothing is vendored.

## 5. Requirements

### Functional: fixes first

BF23 delivers these before this spec starts: drawing sub-emitter children (F1), stats that include
them (F2), the world budget setting (F3), the `generateLightingData` field (F4), patch diagnostics
(F10), `.particles` as a tracked file (F11), and seeds and `state_hash` in reports (F13). If F1's
red test shows that children do draw, correct §3 here.

- **R1 to R5.** Reserved, delivered by BF23 as above.

### Functional: runtime features

- **R6. Easing mode.** `MinMaxCurve` gains a mode `Ease`: start, end, and a preset
  (`Curve::fromEase`). It is stored as an ordinary curve plus its preset name, and old files load
  unchanged.
- **R7. Distortion.** A `library:particles` distortion feature using scene colour, with a golden on
  each tested backend. It works where scene colour exists, and the docs state that it holds opaque
  objects and sky only (`heat_haze.frag:6`).
- **R8. Ring renderer.** `RenderMode::Ring` generates one mesh per renderer from outer radius, inner
  radius, arc and segments, and draws through the existing Mesh path. Per-particle size, rotation
  and colour work as they do on any mesh. The value is appended to the enum, so old files keep their
  meaning. No new shader variant is needed when unused.
- **R9. Trigger actions.** An effect declares named triggers that start, stop or pause an emitter,
  or fire sub-emitters. Add an MCP action. Triggers also work with zero live particles, which they
  do not today.
- **R10. Dynamic parameters without recompiling.** Exposed parameters bound to numeric fields update
  a per-state override table read by the kernels. A structural change still recompiles. Effects with
  no exposed parameters pay nothing.
- **R11. Follow parent.** A sub-emitter entry gains continuous inherit for position, and optionally
  rotation and scale, which trails attached to a moving parent need. S20's R10 has only one-shot
  inherit at spawn.
- **R12. Sprite-sheet bake.** **Bake Sprite Sheet** renders an effect in a private scene to one PNG
  with alpha and a chosen grid, then sets up a Texture Sheet Animation material. One undo step and a
  job, following `bake_vertex_animation`.
- **R13. Sound events.** A sub-emitter, trigger or burst can name a `.soundevent` (S37). Needs S37
  on `main`.

### Platform and backends

- **R14.** All of the above works on all five backends and six targets, with no compute, SSBO,
  bindless, indirect draw or 64-bit atomic. Distortion has a defined result where scene colour is
  off: draw without distortion, and warn in the checks.
- **R15.** A disabled feature costs nothing. New module fields default off, and a kernel branch is
  taken only when the effect's compiled program includes it. Particles are not one of S34's
  strippable modules as far as read, so new runtime code must stay small: AC16 records the size.

### Performance

- **R16.** `docs/perf/particles.md` budgets still hold on `ref_100k` and `trails_10k`, recorded
  before and after. R10's override read adds no per-particle branch for effects without exposed
  parameters.
- **R17.** An edit while paused re-simulates to the same frame within the existing 4 ms slice
  budget, using checkpoints, and shows progress rather than blocking if it takes longer.

### Editor UX (human)

Window name **Particle Effect**, under **Window > Visual Effects**. Double-clicking a `.particles`
opens it. Selecting a GameObject with child systems and choosing **Open in Particle Effect** opens
the subtree, as Unity's Particle Effect panel does for a selection. Add the row to
`docs/unity-glossary.md`.

- **R18. Outline.** The tree of the subtree's systems and their sub-emitters, with a preview-only
  eye that never changes the asset or the export.
- **R19. Timing bars.** One row per system or sub-emitter, showing start, emission and particle
  lifetime, analytically, with a draggable playhead. The same timing is available to MCP.
- **R20. Frame-locked editing.** An edit while playing or paused re-simulates to the same time with
  the same seed. The seed changes only on an explicit reroll or, if enabled, on each loop. A loop
  range can be set.
- **R21. Preview Stage.** Background (dark, light, checker, or the current scene's sky), an optional
  moving emitter, Overdraw view, and stats. Pause, speed and step.
- **R22. Inspector.** Non-default values marked; Reset per field and per module; a help link per
  module; copy and paste of a module, with Paste Values. Module fields stay in the single table
  (`ParticleModules.cpp`) that drives the codec and the UI.
- **R23. Curve window.** Playhead, paste at the playhead, align, smooth and sharpen keys.
- **R24. Filmstrip and Checks.** The same filmstrip and rows as the agent's tool, with a Fix button
  when `fix_hint` is a single edit. A Record button makes sequences and sprite sheets without a
  modal or a native save dialog (R12).
- **R25. Exposed Parameters panel,** with sliders and trigger buttons, driving the preview live.

Undo: each gesture is one named step, a no-op makes no step, and Esc during a drag restores
everything. Effect edits use `ParticleEffectAssetCmd`.

### Agent UX (MCP, Lua, CLI)

- **R26. `preview_particle_effect`** (read-only unless `save_path`). Takes `path`, or `entity` for
  its whole subtree; `seed`, `times`, `frames` (0 to 24, default 6, 0 for metrics only), `seconds`,
  `view`, `background`, `overdraw_map`, `platform`, `fail_on`, `reference`, `save_path`, `wait`,
  `timeout_seconds`. It runs in a private scene, framed once from the bounds of the whole run. It is
  a job past 1 s.
- **R27. Metrics.** `count` (peak, curve at about 32 samples, emitted), `bounds`, `overdraw` (CPU
  analytic by default, deterministic; `overdraw_map` adds a GPU heat row and acts as a cross-check),
  `visibility`, `temporal`, `cost`, and `scene_context` listing what was not simulated (colliders,
  force fields, world budget).
- **R28. Checks.** S35's row shape and anti-gaming rules. `capacity_hit` (fail when live particles
  reach Max Particles and emission is silently limited), `overdraw_budget`, `invisible`,
  `blown_out`, `pop`, `never_ends`, `platform_budget`.
- **R29. Honesty about cost.** `cost.method` is `estimate_from_bench_table`, never a live
  measurement on the machine running the tool, and every result says "estimate, not measured on a
  device". The guide tells agents never to claim otherwise.
- **R30. Small extensions.** `edit_particle_effect` gains `dry_run`. `get_particle_effect` gains
  `module_fields`, returning fields, defaults and ranges from the same table the Inspector uses.
- **R31. Tree-wide control.** Play, seek and capture accept a subtree, so a multi-system effect can
  be sought as a whole.
- **R32. Lua.** Bind R9's trigger actions, R10's parameters, R6's easing mode and R8's ring
  settings. Windows and previews have no Lua.
- **R33. CLI.** `jasperengine-editor check --particles` reports the checks at default seed, in
  compiler style, and exits non-zero on `fail_on`.

### Data and compatibility

- **R34.** No schema break. New fields have defaults, and `kEffectSchemaVersion` stays 1 unless a
  field's meaning changes. Because unknown keys are errors today (`ParticleEffectCodec.h`), an older
  engine refuses a newer file. Add the first real upgrade step to the codec only if a change needs
  it.
- **R35.** `RenderMode` values are append-only.

## 6. Design constraints and decisions

- **D1. The tree is a view, not a new format.** Effect units stay a `.particles` file plus its
  sub-emitter files, or a GameObject subtree saved as a prefab. That is Unity's model, and a v2 file
  holding a tree would cost 5–8 weeks and duplicate two working composition mechanisms. The cost is
  that an agent patches each file rather than one document, which R31 and R26's `entity` soften. If
  v2 is ever built, emitters must be a keyed map, since an RFC 7386 merge patch replaces arrays
  whole.
- **D2. A new tool rather than extending `capture_particles`,** against the "prefer extending" rule.
  Its mutation of the live system, its reading of a view with whatever else the view shows, and its
  required entity and time cannot change by adding a parameter. It stays for "this effect, in this
  scene, from this camera".
- **D3. `ParticlePreviewRender` is the base,** given a stepped mode. `ParticleEffectOverlay` needs a
  `SceneProject`, so it cannot host the window's preview. `PoseSheetRenderer` is the sheet pattern.
- **D4. Private scenes, offscreen.** Views render only while shown, and an idle editor does not
  draw, so the tool requests its own frames and never depends on a tab being visible.
- **D5. One check library,** `editor/util/QaChecks`, shared with S90 and S35.

**Budget** (characters, drafted; `tool_budget.py` replaces them): `preview_particle_effect` about
1,500, the 12th and last slot in `particles`; `dry_run` +29; `module_fields` +96. Core +0. A later
tool in this group needs a `budget.json` entry.

## 7. Acceptance criteria

| AC | Check | Verified by |
|---|---|---|
| AC1 | A parent with a Death sub-emitter shows child pixels at a set time. **Red on `main`** | `ParticlesGpuTest`, golden on glcore and Vulkan |
| AC2 | A system with no sub-emitters allocates nothing new and packs in the same time within noise | Allocation hook, `ref_100k` bench |
| AC3 | A misspelled patch field returns a refusal with did-you-mean, and the file is unchanged | `test_particles_editor.py`, with red control |
| AC4 | A direct edit to a `.particles` with a bad field shows in `external_changes.problems` | MCP test |
| AC5 | `state_hash` is equal across two seeded runs, and differs when the seed differs or a child's seed changes | MCP test |
| AC6 | An `Ease` value in a file written by this version evaluates as its curve, and every file in `tools/fixtures/particles` still loads | Unit test |
| AC7 | Distortion, Ring, and Follow parent each have a golden on the tested backends | `ParticlesGpuTest` |
| AC8 | A trigger fires with zero live particles, and start, stop and pause each work from MCP and Lua | Unit test, MCP test, doc test |
| AC9 | Changing an exposed numeric parameter per frame triggers no effect recompile, and a structural change triggers one | Unit test counting `compile` |
| AC10 | A sprite-sheet bake is one undo step, and undo removes the files. The output's alpha is correct on a test effect | MCP test with an independent Python oracle |
| AC11 | The same request gives the same filmstrip bytes on one device, `capacity_hit` fails on a burst over Max Particles, and `invisible` fails on an empty effect | MCP test |
| AC12 | Analytic overdraw agrees with the `overdraw_map` heat row within a stated tolerance on a known effect | MCP test |
| AC13 | `preview_particle_effect` leaves the live scene and undo stack unchanged | MCP test |
| AC14 | The eye never changes the file. An edit while paused holds the time and seed. Esc cancels a drag with no undo step | `test_particle_effect_window.py` (`ui_probe`), and screenshots from an isolated editor |
| AC15 | The tool's checks and the window's Checks rows match on one effect | Editor test calling both paths |
| AC16 | `docs/perf/particles.md` budgets and `check_web_particles.py` still pass, and player size growth is recorded | `tools/perf/`, S34 size check |

## 8. Dependencies and conflicts

- **Merge hotspots:** `RenderSystemParticles.cpp`, `EmitterRuntime.*`, `ParticleEffectCodec.cpp`,
  `InspectorParticles.cpp`, `App.cpp`, `tools/mcp/budget.json`.
- **S90** shares `QaChecks` only. **S35** defines the check row. **S37** blocks R13 only:
  `.soundevent` is not implemented on `main` yet.
- `AnimatorWindow`'s double `show()` at `App.cpp:2815,2819` looks like a merge artifact. Touching
  this code may surface it, and it should be checked separately.

## 9. Risks and defaults

1. **The sub-emitter bug may not reproduce.** Default: R1's capture decides, and if children do
   draw, record that here and add the pixel test anyway.
2. **Drawing children may strain budgets** in effects that were never seen at full cost. Default:
   children count against the world budget, and the release note says existing effects may look
   different.
3. **`platform_budget: pass` will be read as "runs on a phone".** Default: R29's label. Device
   sign-off stays manual.
4. **A view-only tree may feel thin** next to Effekseer's single-file effects. Default: ship it, and
   revisit v2 after real use (D1).
5. **Metric gaming.** Default: S35's rules, and `look_at` for taste.
6. **Distortion quality** is limited because scene colour holds opaque objects and sky only.
   Default: documented, not hidden.

## Milestones

Effort in person-weeks, estimates at ±50%.

- **M1.** BF23 merged. AC1 to AC5 are its tests.
- **M2 (2–3).** The Particle Effect window: R17 to R22, R25. AC14.
- **M3 (1–2).** `preview_particle_effect`, metrics, checks, Filmstrip, CLI: R24, R26 to R31, R33.
  AC11 to AC13, AC15.
- **M4 (3–5).** R6 to R10 with their Lua (R32). AC6 to AC9.
- **M5 (2–3).** R11, R12, R13, R23. AC7, AC10.
- **M6 (about 1).** Budgets, docs, guide, final gate. AC16.

## Deviations

- **M3, bake as a tool of its own (R12, §6 Budget).** Bake Sprite Sheet is `bake_particle_sheet`,
  the 13th tool of `particles`, rather than a mode of `preview_particle_effect`: it writes two files
  as one undo step and runs as a job, as `bake_vertex_animation` does, and the preview stays
  read-only but for `save_path`. `tools/mcp/budget.json` gives `particles` 13 tools.
- **M3, D5 adopted by S35 now.** `editor/util/QaChecks` holds the row, the rounds, the threshold
  audit, the guidance and the row's text line; `AudioQa` and `audio_session` use it, so S90 only
  adopts it.
- **M3, overdraw (R27, AC12).** The analytic overdraw counts camera-facing squares of each
  particle's size, with the Renderer's screen-size clamp, over a 64 x 64 grid of the frame;
  stretched billboards, meshes and trails are approximated by that square. The heat row redraws the
  systems as flat additive squares adding 4/255 a layer (it reads up to 63 layers); sub-emitter
  children keep their own look there (their files are not rewritten), so the cross-check is exact
  for effects without sub-emitters. AC12's tolerance is 10% of the analytic peak.
- **M3, cost (R29).** Mobile has no measured row in docs/perf/particles.md; its per-particle costs
  are assumed twice desktop's single-thread ones (S20 R21's budget ratio) and the result says so in
  `cost.basis`. `platform_budget` warns over `particles.qa.cpu_ms_<platform>` and fails over twice.
- **M3, checks (R28).** `pop` is the share of an emitter's live particles (20 or more) dying in one
  tick at alpha 0.25 or more; `blown_out` needs cells and is SKIP with `frames: 0`; `never_ends` is
  SKIP for a looping root. `reference` is shown as the last cell, not compared.
- **M3, AC15.** The Particle Effect window that hosts `ParticleChecksPanel` is M2's (another
  branch), so AC15 is an editor test of the panel's request against the tool's (its source and
  arguments) on one effect; a `ui_probe` test of the hosted panel belongs after both merge.
- **M3, CLI (R33).** `check --particles` is opt-in (and part of `--all`), not in the default set: it
  starts the engine for every effect. `--fail-on warn` reports WARN rows as errors too.
