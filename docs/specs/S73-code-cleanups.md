# S73 — Code cleanups: generated AssetRefSchema, frozen parity data, dead code, a counter

**Depends on.** S08, S04, S32. Touches `editor/asset/AssetRefSchema.*`, `editor/reflect/`, `editor/tests/reflect/legacy/`, `editor/Stream.h`,
`engine/core/animation/UpstreamAnimFormats.cpp`, `engine/renders/sokol/SokolAsyncCreate.cpp`.

## 1. Goal
Finish four loose ends so the code says one thing in one place.

## 2. Requirements
- R1. `AssetRefSchema` is generated from reflection tables (as its header comment and S08 promised): adding an asset-reference field to a
  component's reflection is all a contributor does. Same results as the hand table, proven by a test that checks the generated table
  against a frozen copy of today's entries. `AssetRefSchema.CoversAllFields` becomes unnecessary or stays as a check. Non-component
  references (materials, project.yaml) stay in a small explicit list.
- R2. Delete the legacy oracle `editor/tests/reflect/legacy/`, without losing what it guarded: first freeze its encode output for the
  parity cases as golden YAML under the tests, so `ReflectParity.*` compares against data. The codec benchmark (S08 R17) keeps a frozen
  number or goes. Update `Stream.h` and `EditorTests.cmake`.
- R3. Remove dead `keptText` (`UpstreamAnimFormats.cpp:124`) and whatever becomes unused with it. A compile with `-Wunused` shows nothing new.
- R4. `createdShaders` no longer counts one too many after a clear with a queued make (`SokolAsyncCreate.cpp`), with a red test.

## 3. Acceptance
- AC1. Red test for R4; R1's frozen-table test; R2's goldens pass on main's code before deletion and after.
- AC2. Editor test binary is smaller or equal; `tools/check-changed` green; docs of S08 updated (Deviations).
