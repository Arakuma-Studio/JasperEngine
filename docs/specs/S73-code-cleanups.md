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

## Deviations
- **R1, more rows than the hand table.** Every hand row is generated unchanged (`AssetRefSchemaFrozen.EveryRowOfTheHandTable`), but two reflected
  references the hand table had missed are rows now, since a reference field in a table is all a row takes: `VolumetricCloudsComponent.weatherMap` and
  `AnimationPlayerComponent.layers[].mask`. The spec's "same results" assumed the hand table was complete; leaving them out would have taken an opt-out
  flag on real references. Moved weather maps and masks are now rewritten, found by Find References and listed by Missing Assets.
- **R1, what a field may say.** Types and `.asset` decide most rows. `AssetKind` gains `NavMesh`, `NavGrid`, `ParticleEffect` and `ModelOrPrefab` (the
  fields that said nothing or `Any`), which also fills in `x-asset-kind` for them in the reflection document and changes three lines of
  `locators_expected.txt`. New field metadata: `.assetForm` (a shape the type does not tell: Animation Player clip entries, the Prefab component's
  root, scripts, inline effects), `.assetMoved` (what a moved file does beyond `updateFlags`: Audio Source `loaded`, Terrain `heightMapLoaded`),
  `.assetCount` (a mesh's slots past `numSubmeshes`). `ComponentDesc::legacyTextureKeys` holds the Terrain's old keys. A new shape is the one thing that
  still needs code in `AssetRefSchema.cpp`.
- **R1, a moved file reloads through `Catalog::updateEntity`.** The walk runs the field's `updateFlags` and the component's `edited`, which does slightly
  more than the hand walk's per-component code in a few cases: a texture of one submesh marks every submesh, a moved height map also rebuilds the
  foliage, a probe's capture revision also rises. `AssetRefSchemaFlags.*` passed on the hand walk and passes on the generated one.
- **R1, later programs.** S51's fonts are `FontArray` references and need nothing here. S49's particle ramps (R43) belong in
  `ParticleModules::visitAssets`, which the inline effect rows already walk, not in a new row.
- **R2, the goldens are not all 500 samples in full.** That would be tens of MB. Each file keeps the first 8 in full and a digest of all 500, so a change past
  the 8th fails without showing its text. `RandomValues` now draws the same values on every compiler and per field, so adding a field adds its key to the
  data and moves no other line (`JASPER_TEST_UPDATE_GOLDEN=1` rewrites them). The decode side compares the encoded text of the decoded value, not every saved
  value bitwise; a saved field is in that text.
- **R2, the benchmark keeps frozen numbers**: the hand codecs' last medians on server B (encode 3557 ms, decode 802 ms for 10,000 GameObjects, RelWithDebInfo).
