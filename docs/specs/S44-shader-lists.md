# S44 — Shader lists are complete for closed scenes

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
