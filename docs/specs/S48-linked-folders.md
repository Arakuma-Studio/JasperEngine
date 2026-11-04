# S48 — Linked folders behave like normal folders: exports ship them, and one file has one GUID

**Depends on.** S04 (GUIDs in `.meta`, `AssetIndex`, `AssetOps`), S03, S05. Reference (read only, adapt to S04, no copying of its
`AssetDatabase`): sound-layer `docs/superpowers/specs/2026-10-19-sound-layer-cleanup-design.md` §2.A1 and §5.A.1,
`2026-10-19-sound-layer-cleanup-d-design.md`, and tests `test_cleanup_group_a.py`, `test_cleanup_group_d.py`, in `je-wt/snd-ro`.

## 1. Goal
A folder or file link inside `assets/` (symlink, junction, mount point) works everywhere the way a real folder does, as in
Unity. Today an asset in a linked folder works in the editor but is missing from the exported game; and the same file reached
through two paths can be taken for a copy and given a new GUID, which breaks references.

## 2. Requirements
Exports (A-1):
- R1. Every export mode (source, desktop, web, Android, iOS, prebuilt) ships every file the editor reaches through a linked
  folder, at the path the editor reads it by. Includes assets, Lua and custom shader sources.
- R2. A link leading to the project's own folder ships under both names, without collision. A linked file ships inside the
  export tree, never outside it. Link cycles terminate with a warning.
- R3. Export size budget unaffected for projects without links.
Identity (D-1, D-2):
- R4. Two paths are compared by file identity (`std::filesystem::equivalent`), so a file reached by a link or hard link is never a
  copy and gets no new GUID. A real copy still gets a new GUID.
- R5. One asset, one path: a GUID's path is the file's real path when inside the project. Lookups, references and instantiation
  through any path resolve to the same asset; stored hints are the real path.
- R6. A folder moved and linked at its old name is a move: references survive.
- R7. A link leading out of the project is a normal project folder (the decision for this engine, as Unity does): its assets
  are indexed by their project path, with `.meta` beside the file; a read-only target is reported in the Missing panel rather
  than failing. The scan handles cycles and is bounded.
- R8. A project without links behaves exactly as before: no format, cache or tool change. Watcher (S05) sees changes through links.

## 3. Acceptance
- AC1. Ports of the two reference test files, adapted to `.meta`, each failing on main first.
- AC2. An exported desktop game and a web game that really load assets from a linked folder (slow test allowed, opt-in via the
  existing mechanism; run once and report).
- AC3. Unit tests for R4-R7 in `AssetIdentityTest`. A symlink on Linux and macOS; junction code compiled by careful reading for
  Windows (no Windows runner available), stated as such.
- AC4. `tools/check-changed`; docs on links in the asset guide.

## Deviations

- D1 (R7 against the write sandbox). A link out of the project is a project folder for the database, the
  lists, references, the watcher and exports. The MCP write tools keep refusing a path that resolves
  through one, and `search_files` keeps not reading through one (`test_write_sandbox.py`,
  `test_trusted_roots.py`, `test_checks.py`): that sandbox is a security boundary of its own, not a
  scan's limit, and R7 does not ask to widen it. The database's own metas are written beside the files;
  `test_write_sandbox.py`'s listing of the outside folder leaves them out for that reason.
- D2 (R5, sidecar hard links with their own metas). Two hard-linked sidecar files that each already have
  a meta with a different GUID stay two assets: an old project may reference both GUIDs and neither is
  a copy to fix. Same GUID, or no meta on one: one asset, as R4 says.
- D3 (out-of-project names). A second link to a folder outside the project is an alias of the first name in
  path order, inside or outside what R5 names. When a smaller name appears later the assets move to it
  (a `Moved` change: references keep their GUIDs, the hints follow).
- D4 (cooked textures). A texture the cooks replace ships cooked at its real path and uncooked under a link's
  name (R2's "under both names"): the runtime loads both, the cook is keyed by the real path.
- D5 (AC2, web). The web half has two steps (docs/testing.md, "Linked folders in a web game"): no image holds
  both an editor and Emscripten, so the editor exports the source in the Linux image and the web image builds and
  gates it.
- D6 (AC3). Junctions and mount points on Windows go through `watch::isLink` (`file_type::junction` on
  MSVC) and `std::filesystem::canonical`/`safewrite::resolvedPath`; read, not run: no Windows runner.

