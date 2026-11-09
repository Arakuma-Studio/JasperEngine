# BF23 — Fixes found while auditing particles, materials, the editor and the MCP tools

**Wave / Depends on.** Lands before S90 and S91, which build on it. Depends on nothing unmerged.
Branch `fix/bf23-authoring-audit`, one commit per fix, in the order of §5.

**Basis of this spec.** Every item was found by reading the code at `b4f7816ea`, and its cited lines
were re-read when writing this spec. **Nothing was built or run.** "Absent in the code" is a fact
from reading. What a user would see as a result is inferred until the item's red test fails.

## 1. Goal

Fix what is already wrong before building on it: a runtime feature that looks invisible, agent edits
that fail silently, and editor behaviour that contradicts our own docs. Each fix is useful alone.

**The rule for every item.** Write the test first and watch it fail on `main`. If it passes, the
audit was wrong: record that under Deviations with the evidence and drop the item, rather than
changing the code or the test.

## 2. Scope

**In scope:** the 28 items in §3, and recording the gaps in §4.

**Out of scope:** new features, which belong to S90 and S91. No refactor beyond what a fix needs.

**Dropped from the audit after re-reading**

- `docs/testing.md:513-515` was reported as contradicting itself about macOS. It does not: GPU
  suites can run on macOS, and CI chooses to run only the CPU ones.

## 3. Items

Priority: **P1** wrong behaviour users or agents hit, **P2** real but narrow, **P3** cleanup.

### A. Runtime

- **F1 (P1). Sub-emitter children look undrawn.** `drawParticles` packs only `state.runtime`
  (`RenderSystemParticles.cpp:759,802`), GPU objects exist per root state
  (`ParticleSystemState.h:43,136`), the pack code never reads children, and `child()` is called only
  from tests. Their tests check counts and events, never pixels.
  - **Red:** a scene with a Death sub-emitter, captured at a set time, shows no child pixels. Add it
    to `ParticlesGpuTest`.
  - **Fix:** walk children in a deterministic order, each with its own GPU state and renderer
    settings, allocating that state only when a child exists.
  - **Guard:** a system with no sub-emitters allocates nothing new, measured on `ref_100k`.
  - **Note for release notes:** existing effects with sub-emitters will start to show them, so they
    may look different, and they now cost what they always should have.
- **F2 (P1). Stats leave out children.** `stats.live = r.count()` (`ParticleSubsystem.cpp:426`) and
  the world total (`ParticleWorld.cpp:166`) count only `channels.count` (`EmitterRuntime.h:201`).
  - **Red:** `get_particle_stats` reports 0 while only children are alive.
  - **Fix:** count recursively. Check that the budget share (`ParticleWorld.cpp:60-79`) counts them
    too, and fix it if not.
- **F3 (P2). The world budget cannot be set.** `S20` R15 calls it a project setting, but `setBudget`
  (`ParticleWorld.h:64`) has no caller.
  - **Red:** a project setting of 1,000 leaves `lastStats.budget` at its default.
  - **Fix:** a Project Settings row through the S25 registry, read at startup in games and the
    editor. The default stays `defaultWorldBudget()`, so old projects behave the same.
- **F4 (P2). "Generate Lighting Data" does nothing.** `generateLightingData` is stored and compiled
  (`ParticleModules.cpp:586`, `ParticleCompiler.cpp:1278`), and `lightingData` has no reader
  (`ParticleProgram.h:449` is its only other mention).
  - **Fix (decided by the owner):** keep the field loading, because unknown keys are errors in this codec and old
    files must load, but remove it from the Inspector and the docs. Lit trails stay out of scope, as
    in S91.
  - **Red:** the Inspector row exists and toggling it changes no pixel or program field that is
    read.

### B. Agents: failures that say nothing

- **F5 (P1). Graph typos are written without complaint.** `validate` loops over declared parameters
  only, so a misspelled key is never reported (`Graph.cpp:541-556`). Unknown node types and
  out-of-range values are warnings (`:537,:549,:553`), and `edit_graph` refuses only new errors
  (`GraphTools.cpp:197-209`).
  - **Fix:** a `GraphKindDesc::strictParams` flag, default off, so kinds keep S23's forward
    compatibility at file level. For a strict kind, these are errors on edit. `load` never refuses.
  - **Red:** on a strict test kind, `setParam` with a misspelled name succeeds.
- **F6 (P1). `edit_graph` does not return new ids** (`GraphTools.cpp:202`), so `addNode` then
  `connect` in one batch forces the agent to supply an id, which triggers F7.
  - **Fix:** return `added: [{op_index, id}]`. **Red:** the reply has no such field.
- **F7 (P1). Supplying an id skips defaults.** Parameter defaults and dynamic ports apply only when
  `fresh` (`Graph.cpp:693-711`).
  - **Fix:** apply them always, except on the undo path, which passes an explicit restore flag and
    keeps its exact restore.
  - **Red:** `addNode` with an id has no default parameters. Undo must still restore
    byte-identically.
- **F8 (P2). Hand-written edges without ids collide.** A missing id parses as `""`
  (`Graph.cpp:300-308`), and two of them are a `duplicate-id` error (`:519-522`).
  - **Fix:** `load` allocates missing edge ids. **Red:** a file with two id-less edges fails
    validation.
- **F9 (P2). Issues have no suggestion.** `graph::Issue` gains `suggestion`, as
  `ProjectChecks::Finding` has, with a did-you-mean for F5. **Red:** the F5 reply has no suggestion.
- **F10 (P1). A misspelled particle field in a patch vanishes.** The codec warns and continues
  (`ParticleEffectCodec.cpp:501-504`). `editEffectTool` collects `diagnostics` and never passes them
  to `replyEffect` (`ParticleTools.cpp:305-308`).
  - **Fix:** return the patch's diagnostics, and refuse an unknown field in a patch, with a
    did-you-mean and the file untouched. Reading an old file keeps warning.
  - **Red:** a patch with a misspelled field replies as a success with no mention of it.
- **F11 (P1). `.particles` is second-class on disk.** `isTrackedFile` omits it
  (`McpAutoSync.cpp:165-180`), so a direct edit reports no `external_changes.problems`, and
  `isTextFile` (`ScriptTools.cpp:140-146`) refuses it for `read_file` and `write_file`.
  - **Fix:** add `.particles` to both, and let a graph kind's `fileExtension` count as tracked and
    text, so S90 needs no further edit. **Red:** both refusals, in `test_particles_editor.py`.
- **F12 (P2). `GraphEditCmd` only writes the file** (`GraphEditCmd.cpp:15-29`). An open window or
  pool cannot react, unlike `writeFilesUndoable`.
  - **Fix:** an after-change hook, called on execute and undo. **Red:** a registered listener is not
    called on undo.
- **F13 (P1). Particle runs cannot be reproduced from their reports.** `capture_particles` takes and
  reports no seed, and a seedless effect re-randomizes. `stateHash` exists (`EmitterRuntime.h:227`)
  but no particle tool returns it.
  - **Fix:** report the seed in use and `state_hash` in `get_particle_stats` and
    `capture_particles`.
  - **Red:** neither field is present.
- **F14 (P2). `focus_window` cannot name the Animator in its schema.** The code handles `"animator"`
  (`ViewTools.cpp:1497`) and the enum omits it (`:2025-2027`). Whether the call is refused is not
  yet checked.
  - **Fix:** add it, and a test that every handled name is in the enum. Budget: a few characters in
    core.
- **F15 (P3). A nearly flat screenshot passes as not flat.** `isFlatPng` needs exact equality
  (`ViewTools.cpp:302-316`), so noise of 1/255 hides a blank frame.
  - **Fix:** a small tolerance. **Red:** an almost-flat PNG returns false.
- **F16 (P3). A thumbnail can be cached from a stand-in.** After 600 waits the thumbnail path saves
  anyway (`ProjectWindow.cpp:604-617`).
  - **Fix:** on giving up, do not write it, and leave it to retry on the next change.
  - **Red:** a material that never finishes loading gets a thumbnail file.

### C. Editor

- **F17 (P1). The Animator window is drawn twice per frame.** `animatorWindow->show()` appears at
  `App.cpp:2815` and `:2819`, and `show()` has no guard. It looks like a merge artifact. ImGui
  appends to the same window, so the canvas item id is duplicated (inferred).
  - **Fix:** delete the second call. **Red:** count `show()` per frame in `AnimatorWindowTest`.
- **F18 (P2). Box and multi-select are lost in the Animator.** The widget supports them, but the
  window rebuilds `node.selected` from one `Selection` each frame (`AnimatorWindow.cpp:1420,1440`)
  and never reads `selectedNodes`.
  - **Fix:** `Selection` holds several states. A multi-drag is one undo step, and Delete removes
    them all as one step.
  - **Red:** with `ui_probe`, box-selecting two states leaves one or none selected on the next
    frame.
- **F19 (P2). Esc does not cancel a node drag,** against `docs/undo.md:29`. Only `linkSource` is
  cleared (`GraphCanvas.cpp:220`).
  - **Fix:** Esc restores the drag start, ends the box, and adds no undo step. **Red:** in
    `GraphCanvasTest`.
- **F20 (P3). Audio Mixer is missing from `forEachPanel`** (`App.cpp:5557-5578`), so layout presets
  do not record it.
  - **Red:** a preset does not restore its visibility.
- **F21 (P3). The palette's window list omits Animator and Audio Mixer**
  (`SearchPalette.cpp:71-78`), against S25 R38. It is a hand-kept list that can drift again.
  - **Fix:** add them, and a test comparing it with the `window.*` catalog commands, with a named
    exclusion list.
- **F22 (P3). Layout and focus leftovers.** The Animator is docked twice (`App.cpp:2068,2070`). Two
  windows share dock order 7 (`:2108-2109`). An `else if (animatorWindow->isFocused())` can never
  run (`:2721`). Remove them, with no behaviour change except a stable tab order.
- **F23 (P2). Double-clicking a `.material` opens text.** `openFile` has no material branch
  (`ProjectWindow.cpp:4675` onward). Unity selects it and shows it in the Inspector, and our
  Inspector already can.
  - **Fix (decided by the owner):** the double-click focuses the Inspector, and **Open as Text** stays in the
    context menu (`:2039`).
  - `.particles` gets its window in S91. Until then it behaves like `.material`.
- **F24 (P2). `.exr` is cookable but has no identity.** It is in `isCookableTexture`
  (`TextureCookQueue.cpp:69`) and absent from the Texture row of the GUID table
  (`AssetTypes.cpp:28-29`).
  - **Fix:** add it. An existing project gains sidecars on its next scan, and old references still
    resolve.
  - **Red:** an `.exr` gets no GUID.

### D. Docs

- **F25 (P2). "GL 3.3 core" is wrong.** `AGENTS.md` (Invariants), `.claude/rules/rendering.md` and `docs/specs/README.md` say 3.3. The code targets GLSL 410 (`ShaderBuildTypes.h:59`,
  `ShaderPool.cpp:31`) and refuses to start below a 4.1 context (`BackendLinux.cpp:2633`,
  `BackendWin.cpp:753`).
  - **Fix:** say GL 4.1 core, then run `tools/check_agent_docs.py`.
  - **Check first:** that no export target still builds for 3.3. If one does, record it here,
    because the fix then changes.
- **F26 (P3). Stale spec text.** `S20:620-621` ("S27's curve widget has not landed";
  `InspectorParticles.cpp` uses `CurveField`). `S23:48` (`editor/widget/GraphCanvas`; it lives in
  `editor/window/widget/`). `S11:69` ("Variant space is full", from before the 64-bit widening).
  Correct them with a dated note, and do not rewrite the history around them.
- **F27 (P3). `docs/mcp/guide/shaders.md:510`** says "a mesh or points shader" can read the scene,
  and particles can too.
- **F28 (P3). Old recommendations in `docs/openengine-analysis/`.** `00:91`, `01:252` and `05:443`
  recommend node libraries that S21 D14 rejected. `05:321` predates S23's graph model. `01:251`
  promises fragment `SURFACE_*` hooks that do not exist. These are historical, so add one note at
  the top of each file pointing to the decision. Do not edit the analysis.

## 4. Gaps to record, not fix here

These are real, but each needs money, a device or a decision. BF23 records them in a new Known gaps section of `docs/testing.md` and changes nothing else.

| Gap | Evidence | What it would take |
|---|---|---|
| No native compile check for generated HLSL and MSL | There is no CI, and `tools/check-changed` has no `fxc`, `dxc` or `metal` step | A check someone runs on a Windows and a macOS machine |
| Nothing runs on Windows or macOS unless a person does: there is no CI | `AGENTS.md`, "There is no CI" | A machine for each, or accepting it |
| Particle goldens run on glcore and Vulkan only | `S20:638-639` | Devices for Metal, D3D11 and GLES3 |
| Mobile particle performance rows are unmeasured | `docs/perf/particles.md:129-136` | A person with reference phones |
| S20's "Web: est. X ms" hint was never built | `S20:573`, no match in `InspectorParticles.cpp` | Delivered by S91's `cost` table |
| The agent guide has about 110 bytes free, and S35, S90 and S91 all want some | `budget.json:25-30`; estimated, not measured | The second to land moves its own detail out, per `conventions.md:209-213`. Measure with `tool_budget.py --groups all` |

## 5. Order and effort

Person-days, estimates at ±50%.

| Group | Items | Effort |
|---|---|---|
| 1. Quick wins | F17, F14, F22, F25–F28 | 1–2 |
| 2. Agent failures | F5–F13 | 5–7 |
| 3. Runtime | F1, F2, then F3, F4 | 5–8 |
| 4. Editor behaviour | F18, F19, F20, F21, F23, F24 | 4–6 |
| 5. Small checks | F15, F16 | 1 |

About 3–5 weeks in total. Stop-and-report rule: F1's red test comes first in group 3, because its
result changes S91's first milestone.

## 6. Acceptance

- **AC1.** Each item has a test named in its commit, and the commit message says which run was red.
  F22 and the docs items have no test, and say so.
- **AC2.** `tools/check-changed` passes per commit on a build server. The full suite passes before
  merge.
- **AC3.** F1 has a golden on glcore and Vulkan. F18 and F19 have screenshots from an isolated
  editor.
- **AC4.** `ref_100k` and `trails_10k` stay within `docs/perf/particles.md` after F1 and F2, before
  and after numbers recorded.
- **AC5.** `tools/mcp/gen_tool_docs.py` and `tool_budget.py --check` pass after F6, F13 and F14.
  Core stays within budget.
- **AC6.** Old projects and every file in `tools/fixtures/particles` load unchanged after F4, F5 and
  F10.

## 7. Dependencies and conflicts

- **Blocks:** S90's graph tools (F5–F9, F11, F12) and S91's first milestone (F1–F4, F10, F11, F13).
- **Merge hotspots:** `App.cpp`, `Graph.cpp`, `GraphTools.cpp`, `ParticleTools.cpp`,
  `RenderSystemParticles.cpp`.
- **F5 changes behaviour only for kinds that opt in.** The test kind and its tests keep working as
  they are.

## 8. Risks and defaults

1. **F1 may not reproduce.** Default: if children do draw, keep the pixel test, close the item and
   correct S91.
2. **F1 can make existing effects heavier,** because children will really render. Default: they
   count against the world budget, and the release note says so.
3. **F10 refuses patches that used to "succeed".** Those patches were silently losing data, so this
   is intended.

## Deviations

- **F25, the check first (from reading the build and context code, 2026-11-04): no export target
  builds for GL 3.3, so the fix stands.** The glcore backend compiles GLSL 410 only
  (`ShaderVariantCompiler.cpp`: `args.version = 410`; `ShaderPool.cpp` includes `glsl410.h` and
  `glsl300es.h` only), and nothing in `engine/`, `editor/` or `tools/` selects GLSL 330. Every
  desktop context asks for 4.1 core: games on Linux (`JasperEngineLinux.cpp`), Windows
  (`JasperEngineWin.cpp`) and macOS (`MacViewGL.mm`, `NSOpenGLProfileVersion4_1Core`), the editor's
  `BackendLinux.cpp` and `BackendWin.cpp`, and the tests' `HeadlessContext.cpp`. Web and Android use
  GLES3 (`glsl300es`), iOS uses Metal. The one version 3 context attribute is the Linux GLES3 3.2
  test context. The text now says 4.1 in `AGENTS.md` and `docs/specs/README.md`; after the merge of
  `integ/mats-fx`, `.claude/rules/rendering.md` names no version. **Left as they are, outside those files:** the runtime label
  `"OpenGL 3.3 core"` (`SokolSystem.cpp:836`), pinned by `tools/ci/test_player_cli.py:157` and
  repeated in `docs/profiling.md:109` and a comment in `SystemRender.h:140`, still names the wrong
  version and should change together as a small follow-up. The `GL 3.3` mentions about RGB565
  (`engine/libs/sokol/PATCHES.md`, `TextureFallback.cpp:121`, `TextureFallbackTest.cpp:244`),
  `docs/textures.md:177` and `TestRenderHalfFloatTargets.cpp:176`, and the requirements in S20 and
  S21, were not re-checked or edited. No test: docs only.
- **F17's red is an MCP test, not `AnimatorWindowTest`.** The unit binary has no `App`, whose frame
  has the double call, and a count in `AnimatorWindowTest` would only count that test's own calls.
  `test_animator_selection.py` reads `begins` from the UI probe, which reports top-level windows
  begun more than once a frame (ImGui's `BeginCount`). The red run reported `{'Animator': 2}`.
- **Section 4's guide row is measured, not the estimate above:** 6,241 of 8,192 bytes, 1,951 left with
  1,500 of headroom required, so 451 bytes free rather than about 110 (`tool_budget.py --check`).
- **F21 adds more than the two windows named.** The comparison with the catalog also found Pose
  Animation, Vertex Animation Baker, Color Picker, Recorder, Navigation, Texture Synthesis, Sprite
  Normal Map and Physics Placement missing; they are added too. The named exclusions are the
  layouts' actions, Detachable Windows and the version control windows.
- **F18 goes a little further in the canvas and the menus.** A plain click on one of several selected
  nodes now leaves that one, or a multi-selection could not be narrowed by clicking. A right click on a
  link selects it, or the link menu's Delete would remove the selected states. A right click on a
  selected node makes it the Inspector's and the menu's node. Copy still copies that one node.
- **F19 also restores the selection,** as `docs/undo.md` says Esc does, not only the positions.
- **F23's premise did not hold: the Inspector had no `.material` view** ("our Inspector already can").
  Owner's ruling: it must be editable, with undo, reusing `InspectorMaterialPbr`. It is: the Inspector
  draws the rows of a submesh's material (moved into `drawSubmeshMaterialRows`, so Advanced Surface,
  Surface, Extra pass and presets are the same rows) on a mesh in a scene of its own that no scene list
  holds. An edit reaches the linked submeshes as it is made and the file when the gesture ends, as one
  project undo step through `update_material`'s write path (`Project::materialFileText`). The rows'
  own commands go to that scene's history (`CommandHandle::Redirect`), so they never enter the open
  scene's undo. Open as Text was prefab-only at the cited line, so `.material` and `.particles` get
  the context menu item.
- **F11 (section B, bf23-agent).** `write_file` also checks what it takes, as it does for clips and
  masks: an invalid `.particles` or a file that is not a graph is refused, and warnings or issues come
  back in its result. Without it `write_file` could have broken every Particle System reading the file.
- **F13 (section B, bf23-agent).** Reporting was not enough. An `edit_particle_effect {seed}` on a
  running system went through the parameter-only rebind and kept the old seed, so a reported seed
  could not be pinned to reproduce a run. A changed pinned seed now starts the run over. The golden
  `tools/mcp/tests/golden/particles/capture_t2.png` was rewritten for it: its test patches `seed: 11`,
  which never took effect before this fix.
- **F16 (section B, bf23-agent).** The give-up path cannot be reached from a project on demand. A texture
  that fails to load is not pending (`Texture::empty()`, `MaterialRender::isPending`), so its material
  already gets a truthful thumbnail, checked with a material naming a missing file. The audit's example
  of a texture that never loads is really a load slower than 600 frames, or a variant that never builds.
  The MCP test holds that state with `JASPER_TEST_MATERIAL_THUMBNAIL_STUCK`, and its red run was that
  seam with the give-up branch disabled.
Section A (F1 to F4), 2026-11-04.

- **F1 reproduced.** On the base of this spec, `ParticlesGpu.DeathSubEmitterChildrenDraw` found the
  children alive in the simulation and 0 of their pixels on screen, so S91 §3 stands as written.
- **F1, wider than `drawParticles`.** Drawing the children needed four more things they had never
  needed: the system's bounds cover them (culling and draw order read those bounds), their
  interpolation factor follows the parent's in the same frame (it was a frame late), scene-input
  readers among them register (a soft child), and `ShaderVariantCollector` ships their variants
  (an export without them could not draw a child with another Renderer).
- **F1, the HTML UI `<particle-system>` element** draws the children too, each with its own
  Renderer's texture and blend, after the root in the effect's order (review round 1).
- **F1, edits reach live parents** (review round 1). An edit of a parent's sub-emitter list (an
  entry's effect, or entries added or removed) binds the children again; an entry's other fields
  are read when it fires and keep the particles. An edit of a child's or grandchild's file reaches
  every live parent: a parameter keeps the particles, a structure starts the parent over.
- **F1, draw order** (review round 1). In the transparent queue each child is its own item, with
  its own Sort Priority and distance; a UI particle system still draws its tree in the effect's order.
- **F2, wider than the two lines named.** `getParticleCount` (Lua's `particleCount`, and what an
  Action's controller waits for) counts children too, since a controller stopped while children
  were alive. In the budget share, three things were wrong and are fixed: children were not in what
  a system wants; a child was capped by its parent's Max Particles (each emitter is now held to its
  own); and sibling entries each saw the whole remainder, so they could overspend it.
  Review round 1 added two: what a system wants counts what its children will need before they
  exist (its parents times each entry's Emit Count, up to the child's Max Particles), and a root is
  held to what its children leave of the tree's budget.
- **F3.** The key is `particles.world_budget`, on the Graphics page, 0 for the platform's default. It
  reaches compiled games through the generated `main.cpp` and prebuilt players through
  `PlayerConfig`. A running world follows it at once; `ParticleWorld::setBudget` still overrides it
  for one world. No Lua API: it is a project setting, read at startup, like `physics.max_bodies`.
  (`physics.max_bodies` and its two siblings do not reach prebuilt players: seen, not changed here.)
- **F4.** The field keeps loading and saving, is hidden in the Inspector, and leaves the Lua stubs
  (`tools/gen_particle_stubs.py`), the only place the docs listed it. `ps.trails.generateLightingData`
  still answers, because the Lua view reads the same field table. `TrailParams::lightingData` had no
  reader and is gone.
