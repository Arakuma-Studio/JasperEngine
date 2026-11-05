# S68 — Linked folders behave like normal folders: exports ship them, and one file has one GUID

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
- D7 (`dc/export-pipeline`, 2026-11-04-export-pipeline-design.md, not landed when this was written). Its §6.1 `exportfiles::walk` and
  its §7 rule 2 ("a link inside assets to a folder outside is not followed ... nothing changes") contradict R1: whoever lands
  second makes `copyAssets`, `copyLua` and `copyCustomShaderSources` walk through links (`Exporter::walkTree`, or the same rules in
  `exportfiles::walk`), and `test_linked_folders_export.py` fails if they stop. The header and script-root walks of
  `copyCppScripts` and `copyGenerated` stay unfollowed for both (C++ is out of R1). Its other rules hold through a link and are not done
  here: a failed copy fails the export (`copyOrFail` goes in `walkTree`'s visitors, whose copies stay unchecked as on main), an unreadable
  folder is `W-UNREADABLE-FOLDER`, an earlier export is left out (`isExportFolder` goes in the visitors' directory step, which runs for
  linked folders too, so a link to one is left out as well). Adapted to its rule "the walkers skip the export's own folder":
  `walkTree` leaves out the output folder when a link leads to it or into it, which R2 needs anyway, since it would ship itself without end.
- D8 (`dc/project-window`, 2026-11-04-project-window-design.md, not landed when this was written). Its asset-root change refuses a source or destination
  through a link and does not walk linked folders: a project with a folder linked out of it, whose moves would go through it,
  is refused with its message. That stands: it is the same line as D1, an operation that moves files stays out of linked
  targets, and nothing here needs it to change. Its `AssetOps` refusals sit before the rename; `AssetDatabase::moveAssets` and
  `removeAssets` treat a link's own name as the link (not what it leads to) after it.
- D9 (MCP text, AGENTS.md "MCP text"). `list_missing_assets`' description is unchanged; `read_only_linked` is a result key, and its text names the assets, only when any
  exist. Eval: `guard_read_only_linked` and `guard_linked_folder_rules` pass with the text and fail without it (surfaces of this branch and of the control editor). The first
  run of the read-only case changed the tool: agents said "the check doesn't say which asset", as Claude Code shows them the text of this result, so the text names them.
- D10 (R7's reach, found by `test_root_attacks.py`). Identity follows a link out of the project only at, below or on the way to the assets or Lua root (a Lua root at the project
  root counts only when the assets root is the project root too, as it is the default and says nothing of what was linked). A root pinned at load that leads elsewhere now (`safewrite::pinTrustedRoot`)
  is not scanned and not written through, by `AssetDatabase::writeIdentity` and by every walk. Exports walk the assets and Lua roots as R1 says, and the project root for shader sources, so a link beside the roots ships only those. An export whose root leads elsewhere now fails with an error (it would ship whatever is there). The watcher and the listings still follow such a root: they write nothing, and an agent can see the link.
- D11 (a root that is a link when the project opens). `openAssetDatabase` gave the database the root's resolved path ("../../outside"), so a linked assets root was not the assets directory: its data files (`.txt`, `.json`, whose type holds only there) were no assets, while textures, which hold anywhere, were. That rule decides what is an asset, not what may be written: `writeIdentity`'s check applies to every type. `normalizeToProjectRelative` now keeps the spelling below the project.
