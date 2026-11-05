# S64 — Shader lists are complete for closed scenes

**Depends on.** S32, BF20. Touches `editor/Project.cpp` (`collectStandaloneShaderKeys`, `:4088`), `editor/mcp/tools/BuildTools.cpp`.

## 1. Goal
`generate_shaders`, `get_build_info` and Play's startup list name every shader the project needs, with scenes closed or
open. Exports already load and re-save every scene and are correct; these three read each closed scene's saved keys,
which can be stale (63 listed instead of 71 in one test). A missing key means a stall or a stand-in at runtime.

## 2. Requirements
- R1. Red test: a project with closed scenes whose saved keys are stale; today's list misses shaders that an export ships.
- R2. One source of truth: closed scenes get their keys computed from their current files without GPU work, the same way
  an export gets them. Export and the three consumers share that code and give the same set for the same project.
- R3. Cost: results are cached per scene by file hash, so a project with unchanged scenes answers `get_build_info` in under
  50 ms beyond today's cost. More than 1 s of work runs as a job (MCP conventions); `get_build_info` may return
  `shaders_complete: false` plus how to finish rather than block.
- R4. A scene whose file cannot be read is named in the result, not skipped silently.
- R5. Play startup uses the complete list, so the warm-up does not miss a variant needed by a scene loaded later.

## 3. Acceptance
- AC1. R1 red-then-green.
- AC2. A test comparing `generate_shaders` keys, `get_build_info` keys and the keys an export really builds, equal on the
  fixture and on a test project with several closed scenes.
- AC3. A timing test or measurement for R3. Docs and `docs/mcp/TOOLS.md` regenerated. `tools/check-changed` green.

## Deviations

- **The cache key is more than the file hash.** A closed scene's keys also read the prefabs of every scene, its
  models, the scripts (the variants a script can reach) and the HTML UI files, so a hit needs the file's hash and a
  context hash (`Project::shaderListContext`: stamps of those files, the lists of prefabs, the mobile tier, the editor
  binary). An edit of one of them reads the closed scenes again. Kept in `.jasperengine/shader-lists.yaml` so a new
  session of an unchanged project reads nothing.
- **Play's list follows the overrides.** R2 asks for one set, so Play's startup now also builds what every build
  ships, the additions and the warm-up list, and no longer builds the excluded keys ahead (the editor still compiles
  one on demand when it draws with it).
- **A fix in the source of truth.** `collectRequiredShaderKeys` read a prefab that was not loaded from its file only,
  so a Prefab Variant or a prefab with nested instances gave different keys depending on which scenes were open. It
  now makes an instance of it either way, and forgets what it loaded.
- **`generate_shaders` per backend.** The list is the same; an export still skips the High shadow filters' variants
  for the backends no platform of which has a High tier (`ShadowTiers::cooksHighFilters`), `generate_shaders` builds
  what it is asked to on every backend. AC2 compares OpenGL, where both build them.
- **Shared with `dc/export-pipeline` (its 6.5.1, F6).** That design makes one `Project::collectBuildShaderKeys` for the
  same four copies. This spec uses that name and shape (`void collectBuildShaderKeys(std::set<ShaderKey>& keys) const`)
  so the two merge onto one function, and implements the union only: its `droppedForks` parameter and the dead-fork
  drop (6.5.2), the Play-created objects (6.5.3) stay export-pipeline's. What this spec adds around it: closed scenes
  are refreshed before it runs (`refreshClosedSceneShaderKeys`), and `finishBuildShaderKeys` holds what a build adds and
  removes for every consumer, so export-pipeline's filter of additions and the warm-up list belongs there rather than
  in `resolveShaderKeys`. Its dead-fork drop in `collectBuildShaderKeys` also covers keys read from the cache.
- **How to finish is the result's own fields.** `shaders_complete`, `shaders_pending_scenes` and `shaders_job_id` (and
  a line in the text summary); no `next`, guide section or new description text. The eval case
  `bad_shader_list_incomplete` passed 4/4 with and without a `next` hint, so AGENTS.md "MCP text" says to cut it; what
  it fails without is `shaders_job_id`.
- **The fixture half of AC2 is `test_shader_lists_fixture.py`**, outside the CI list: the fixture is not in git.
- **The cache file is written on `ioWriteChannel`, not on the Background lane.** It ends in an fsync, and
  `docs/jobs-memory.md` sends blocking long writes to that channel, not to a work-stealing worker. The reply never
  waits for it; a lost write only costs reading the scenes again.
