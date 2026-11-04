# S46 — Crash leftovers clean themselves up

**Depends on.** S03 (file locks), S27 (Hub, templates), BF21. Touches `editor/hub/HubPaths.cpp`, `editor/hub/TemplateCatalog.cpp`.

## 1. Goal
A crash during a project import or a template copy leaves no junk that a user has to find. Today `<name>.importing` and
`templates/.staging/<id>` stay on disk for ever and force the next import to a numbered name.

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
