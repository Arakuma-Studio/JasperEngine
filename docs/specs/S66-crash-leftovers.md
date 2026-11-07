# S66 — Crash leftovers clean themselves up

**Depends on.** S03 (file locks), S27 (Hub, templates), BF21. Touches `editor/hub/HubPaths.cpp`, `editor/hub/TemplateCatalog.cpp`.

## 1. Goal
A crash during a project import or a template copy leaves no junk that a user has to find. Today `<name>.importing` and
`templates/.staging/<id>.importing` stay on disk for ever and force the next import to a numbered name.

## 2. Requirements
- R1. Each staging folder gets an owner marker (pid plus process start time, or an S03 lock held while it is in use).
- R2. On Hub start and before each import or template operation, staging folders whose owner is dead are removed. A folder
  with a live owner (a second editor at work) is never touched.
- R3. Only folders with our marker are removed automatically. A folder matching the old name pattern but without a marker
  (from an older version) is shown in the Hub as a leftover with Remove and Show in Folder buttons, because it may hold a
  user's data; the MCP result lists it.
- R4. Removal failures are logged and retried next time, never fatal.
- R5. The sweep touches only the known parent folders and does no deep scan, so Hub start stays within budget.

## 3. Acceptance
- AC1. Red-then-green test: kill -9 mid-import and mid-template-copy through a test hook; the next start removes both, and the
  imported name is unnumbered.
- AC2. Live-owner test with two processes. Marker-less folder test: kept and reported.
- AC3. `HubTest` extended; `tools/check-changed` green. Docs mention it.

## Deviations
- **The goal's "numbered name" is the staging folder's.** A project's own folder never took a number; the
  next import's staging folder did (`<name>.importing2`). AC1 checks the staging name and the project's. The
  old template path in the goal said `templates/.staging/<id>`; the code's is `<id>.importing`.
- **R1 marker: the S03 lock, beside the folder.** `<folder>.lock`, not inside: `git clone` needs an empty folder
  and a whole folder moves to be the project. No pid check: the lock survives pid reuse. Its text (pid, start
  time) is for people, and tells our marker from a file of the user's.
- **A volume without file locks gets no marker** (network shares, S03 R10). The copy goes on with a Console warning; a
  crash there leaves a folder treated as marker-less. Automatic removal needs proof of a dead owner.
- **R5 known parents** are the recent projects' parents, the Hub's last location, `templates/.staging`, plus a small
  journal (`<settings dir>/hub-staging/`, one file per running copy), which the spec did not ask for: without it a kill
  during an import into a folder the settings do not know would not be found by the next start (AC1). A start without
  the Hub reads only the journal. All of it runs off the main thread, so R38's 150 ms is not at stake.
- **Added `clear_project_leftovers`** (group `editor_tools`): R3 only asks that the MCP result lists a leftover, but
  agents otherwise could not remove what the Hub's button removes.
- **Remove asks first**, inline: the folder may hold a user's files and the removal is permanent.
- `import_project` stays a blocking call rather than a job, and so does the new tool, as S27 made it.
