# Forge and meshoptimizer program: ledger

Owner request (2026-11-04): analyse The Forge (ConfettiFX) and meshoptimizer (zeux), implement what fits
JasperEngine with the best UX for human users and AI agents, add LOD cross-fade, and merge and push when done.
Product decisions are delegated to the lead ("be braver").

## Owner decisions

- Existing projects get cooked models automatically, not only new ones.
- New projects have mesh LOD on by default.
- LOD cross-fade is required.
- Licence and NOTICE rules are removed: private engine. The notices tool stays only to stage credits into
  shipped builds.
- No rented machines: everything builds on this host in Docker, one build at a time.

## Specs

| Spec | Stream (branch) | Covers |
|---|---|---|
| S53 | `feat/s41-loader`, `feat/s41-importer` | glTF loader normalise step, meshoptimizer v1.3, Model Importer and cook, Audit window, Build Report, `meshes` MCP group |
| S54 | `feat/s42-lod` | LOD defaults, LOD builder fixes, baked levels, dithered cross-fade, shadow LOD, Draw Modes, `capture_model_lods` |
| S55 | `feat/s43-draws` | automatic GPU instancing of meshes, constants once per pass, job-built visible lists, shadow geometry, stream-budget fix, mesh bench gate |
| S56 | `feat/s44-device` | device tiers from a GPU database, thermal and memory response, upload pacing |
| S57 | `feat/s45-lighting` | screen-tile light lists, capsule shadows, contact shadows, weighted blended OIT, hair lobe |

Branch names keep the stream names they started with. Specs were first written as S41-S45 and renumbered
to S53-S57 because the pixel-art program had already taken S41-S52.

Research reports are in this directory: `forge-*.md`, `meshopt-*.md`, `jasper-*.md`,
`visibility-feasibility.md` and `lod-crossfade.md`.

## Splits with the other programs

With the Doriax catch-up (`docs/doriax-catchup/LEDGER.md`):

- `draw-perf` owns the opaque state sort, the redundant pipeline and binding tracker, and cull before sort.
  S55 batches instances over that order and does not sort.
- `sprite-batching` owns dynamic batching (sprites, single-submesh meshes of up to 64 vertices with the
  built-in shader, UI). S55's GPU instancing takes the remaining mesh draws: dynamic batching first, and a
  draw is never both.
- `mesh-memory` reshapes `MeshComponent`. S53, S54 and S55 keep their component changes additive, and
  whichever lands second merges onto the first.
- `render-correctness` (flat normals for meshes without normals, foliage fades, light-loop lint) composes
  with S53 Normals Calculate, S54 LOD fades and S57's tiled light loop.
- Shader variant bits and the shader cache version follow that ledger's rule: the next free bit at landing,
  in landing order, and one cache bump per branch over its `main`.
- The agent guide is restructured only by `agent-authoring`. S53-S57 add at most one-line pointers, and
  their detail goes to `docs/mcp/guide/meshes.md`, `device.md` and the rendering page.

With the follow-ups program (`FOLLOWUPS-LEDGER.md`, specs S61-S73):

- S65 changes how `MeshSystem` loads glTF and OBJ on a game's loading thread. S53's normalise step runs in
  the parse, before either path, so it serves both. Whoever lands second merges onto the first.
- S72 cuts header fan-out across `editor/` and `engine/core/`: a merge hotspot, so merge `main` often.

With the pixel-art program (`PIXEL-ART-LEDGER.md`): there is no shared feature. Its S52 sorting and sprite
parameters sit inside `sprite-batching`, not S55.

## Landing

All five specs landed on `origin/main` together. The owner chose to push without the final full suite (2026-11-05). What ran:
each stream's named tests, red controls, WERROR linux, web and Android builds on its branch, and a WERROR linux build of
the merged tree before its last three merges (S53's Read/Write fixes, S56's tier variants, S54's fade bindings), each built
and tested on its branch. Not run on the merged tree: the full unit and MCP suites, the Vulkan and HTML UI suites, the
fixture smokes, the live MCP eval (cases written), and the regeneration of `docs/mcp/TOOLS.md` and the other generated files.
The MCP tests that need `fixture-unity` were never run (it isn't on this host).

## Status

`impl` = being implemented, `merged` = on `origin/main`.

| Spec | Status |
|---|---|
| S53 | on `origin/main`: loader, importer, Read/Write release (R11), exported game from je-cooked (AC11), load bench 13.7 to 4.4 ms (R28), Medium golden (AC7) |
| S54 | on `origin/main` (WERROR linux, Vulkan, its tests pass; web and android before the S57 merge, rerun at the final gate) |
| S55 | on `origin/main`. R9: draws 7.0x (target 10x, not met: 200 unique meshes), shadow triangles -84 % with LOD (met) / -18.7 % from S55 alone, render CPU -8.3 % on llvmpipe (not met, not measurable here) |
| S56 | on `origin/main` (linux, web, android built; its tests and MCP files pass) |
| S57 | on `origin/main` (WERROR linux, web, android; its tests pass). GPU costs measured on llvmpipe only |
