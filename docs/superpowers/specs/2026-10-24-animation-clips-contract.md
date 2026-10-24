# Animation Clips: the T0 contract (header index, ownership and decisions)

Companion of `2026-10-24-animation-clips-design.md` (section 9 binding, section 8 = tasks). T0 created every
public header below with compiling stubs (`// TODO(T<n>)` names the owning task) and implemented the pure helpers
listed as "done". Later tasks implement the rest and may only **add** to these headers. Each header starts with a
"Contract" block (purpose, owner, thread-safety, invariants): that block is the authority, this page is the index.
Revised after review round 1 (`/tmp/clip-research/reports/T0-review.md`).

Namespaces: types in public signatures live in `jasperengine` (`AnimationClip`, `ClipData`, `AnimationEvent`,
`AnimationStateInfo`, the component types); `jasperengine::anim` is the internal function and runtime namespace.
`JasperEngine.h` exports the public headers only (not `AnimationPlayerRuntime.h` / `ModelSubClips.h`).

## Engine (`engine/core/`)

| Header | What | Done in T0 | Implements the rest |
|---|---|---|---|
| `animation/ClipTypes.h` | `ClipChannel` (enum order = canonical track order), `ClipWrapMode`, `ClipWarning` (no heavy includes) | all | — |
| `animation/AnimationClip.h` | value form `ClipData` (`ClipTrackData`, `ClipKey`, `ClipEventData`, `ClipSource`); immutable `AnimationClip`; `quaternionFromXYZW/ToXYZW` | ClipData helpers, quaternion helpers | T1a (`build`, `toData`) |
| `animation/AnimationClipIO.h` | `parseClip`, `writeClip` (canonical), `normalize`, Unity YAML, GUIDs, enum names | enum names, `generateClipGuid`, `isClipGuid`, `readClipGuid`, `replaceClipGuid`, `isUnityYamlClip` | T1a |
| `animation/KeyframeSampling.h` | `locateKey` + samplers with `KeyCursor`, `wrapClipTime`, `trackTime`, `sampleTrack`, `sampleClipTracks`, `collectCrossedEvents` (includeStart covers backward starts at length) | — | T1a (samplers), T1b (wrap/events) |
| `animation/ClipBinding.h` | path grammar, `resolvePath`, `relativePath`, duplicates, `descendantPaths`; `readChannelValue` / `writeChannelValue` (the one channel value reader/writer) | channel value read/write | T1b (paths) |
| `animation/AnimationEvent.h` | `AnimationEvent` + accessors + `numberArg/stringArg/boolArg` (never build args from `const char*`/int) | all | T1b (firing, Lua `push_value`) |
| `animation/AnimationPlayerRuntime.h` | runtime structs (state `cycles`, rest indexing), `ClipPreviewSession` (`written`, `previousTimes`), `defaultStateName` / `playerStateNames` | state names, session helpers | T1b |
| `animation/ModelSubClips.h` | references (canonical form = name), names, node paths, builders | reference parse/format, `isModelFilePath` | T1c |
| `pool/AnimationClipPool.h` | key form (asset-relative or absolute, sub-clip alias table name/index → index), generations, concurrency rules | — | T1a (+ T1c sub-clips) |
| `component/AnimationPlayerComponent.h` | authored fields, runtime pointer, `onEvent`, `onFinish(state)` (includes ClipTypes.h, forward-declares AnimationEvent) | all | T1b runtime, T2a plumbing |
| `object/AnimationPlayer.h` | `AnimationPlayer` object (constructor adds the component; Lua `getAnimationPlayer` must return nil when absent), `AnimationState`, `AnimationStateInfo` | constructor, authored-field accessors (clip list, settings, callbacks) | T1b (playback, states, Lua) |
| `subsystem/ActionSystem.h` | `previewPlayer` (precise semantics: state time + wrap, authored entries via the pool, rest = session), `restorePlayerPreview`, `sampleClip`, `resetPlayers`; private `updatePlayers` / `dispatchPlayerCallbacks` (wired in `update()`) | `restorePlayerPreview`, `sampleClip` forward | T1b |
| `registry/EntityRegistry.h` | `hierarchyRevision()` (0) | — | T1b |

## Editor (`editor/`)

| Header | What | Done in T0 | Implements the rest |
|---|---|---|---|
| `animation/AnimationClipDocument.h` | per-path document; pool calls take `absolutePath()`; `ClipUser` + `findClipUsers` | `clip()`, listeners | T2b (tests after T1a) |
| `animation/ClipValidation.h` | binding/handler report | `ok()`, `trackLabel`, Unchecked report | T2d |
| `animation/ClipConverters.h` | convert, extract, use-player plan, bake, paths | — | T2d |
| `command/type/CreateClipCmd.h` | the R4 composite (files-written hook too) | constructor, `affectsStructure` | T2d |
| `animation/ClipRecordHooks.h` | record listeners, drag keys, channel write chains; `channelsOf` / `before/afterComponentWrite` | `channelsOf`, the component helpers, every command call site | T3a (registries), T3c (record sources, Pose window move, morph rows) |
| `animation/PreviewOwner.h` | one pose owner per root; token 0 is never valid | stub grants valid tokens | T3a (registry), T2c (Inspector action preview acquires) |
| `animation/AnimationClipInspectorHooks.h` | Inspector buttons; Project window double-click uses `openClip(project, 0, NULL_ENTITY, path)` | — | T2c calls; T2d/T3 set |
| `animation/AnimationClipController.h` | clip mode without UI; drag API (`beginKeyDrag/updateKeyDrag/endKeyDrag`), refs returned by edits, `findKey`, `release` | `findKey`, `shared` / `release` | T3a |
| `animation/AnimationClipAppHooks.h` | App/ViewTools wiring hooks + `ClipUndoFocus` | — (every call site wired) | setters per the table below |

## Ownership (who does what; one owner each)

| Item | Where | Owner |
|---|---|---|
| App call sites: stopTransientPreviews (entity + clip mode), undo routing (`LastFocusedWindow::AnimationClip`, `ClipUndoFocus`), toolbar/scene keys (`takesKeys`; Record keeps scene keys but undo), scene close, project close and shutdown (`projectClosed`), Assets > Create > Animation Clip (hidden until `createClipAsset`), `App::dispatchClipDrop` | `editor/App.{h,cpp}` | T0 (done) |
| `AnimationWindow::stopTransientPreviews` (entity mode) | AnimationWindow | T3b (stub by T0) |
| hook setters `stopTransientPreviews`, `ownsUndo`, `takesKeys`, `sceneClosed`, `projectClosed` | AnimationClipController | T3a |
| `createClipAsset`, Project window double-click → `openClip`, `Project::setInspectedAsset` on click, `ProjectWindow::getSelectedPaths` users, `findClipUsers` | ProjectWindow, document | T2b |
| `dropClips` (and calling `App::dispatchClipDrop` from the Scene view/Hierarchy drop targets) | converters / drop targets | T2d sets, T2b calls |
| `windowTestHook` (focus_window `animation`; property and gated call wired by T0, +29 B of tool definitions) | window | T3b sets; T4 counts it in R28 |
| channel write notifications in PropertyCmd, ObjectTransformCmd, ComponentValuesCmd, LatticePointsCmd (execute and undo) | command files | T0 (done); morph-row writes: T3c |
| Inspector action preview acquires PreviewOwner (R7); inspected-object read + clear on scene selection (R30) | InspectorWindow | T2c |
| `ActionSystem::resetPlayers` call when Play stops | `editor/Project.cpp` | T0 (done) |
| ScriptEvents "Animation Player" entry, `LuaFunctionBase::push_value(const AnimationEvent&)`, Lua stubs | engine + `editor/util/ScriptEvents.cpp` | T1b |
| McpValues clip-path validation | `editor/mcp/McpValues.cpp` | T2a (excepted from T4) |
| `Engine::clearPools` hooks, `ModelPool::invalidate` lock | engine | T1a |
| `SceneResources.clips` preload | engine + `collectResourcesInRegistry` | T1a (engine) / T2a (editor fill) |
| `ModelComponent.animationEntities`, layout forcing | engine + ModelLoadCmd | T1c |
| EntityCloner, Engine/Scene callback clearing | engine | T1b |
| Stream/Factory/Catalog properties, ProjectUtils add/remove, PropertyCmd hierarchy switch, ComponentAddDialog | editor | T2a |

## Build and tests

- Engine sources in `engine/CMakeLists.txt` (+ Xcode via `tools/xcode_sync_engine.py`, `--check` clean). Editor
  sources in the root `CMakeLists.txt`, including empty placeholder TUs: `window/AnimationWindowClipSheet.cpp`,
  `AnimationWindowClipEvents.cpp` (T3b), `AnimationWindowClipCurves.cpp` (T3c), `window/InspectorWindowClips.cpp`
  (T2c, optional), `mcp/tools/AnimationClipTools.cpp` (T4, optional).
- `jasperengine-anim-tests` (`engine/tests/anim`, option `JASPERENGINE_ANIM_TESTS`): `AnimContractSmokeTest.cpp`.
  Editor: `editor/tests/AnimationClipContractTest.cpp`. Both test the pure helpers T0 implemented and only the
  API shape of the stubs.

## Decisions where the spec left room

1. **ClipData and AnimationClip both exist** (value form for editing; immutable contiguous runtime form; `build` /
   `toData` bridge).
2. **One wrap enum with `Default`**, only meaningful in player entries.
3. **Players live in ActionSystem** (`ActionSystemPlayers.cpp`), sharing the private pose-blend accumulators.
4. **Player control goes through the `AnimationPlayer` object**; nothing outside engine player code writes the
   runtime structs.
5. **Edit-mode preview state is a caller-owned `ClipPreviewSession`**; `restorePlayerPreview` ends it.
6. **`onFinish` carries the state name**, `onEvent` the `AnimationEvent`.
7. **One composite command (`CreateClipCmd`)** for every file + scene action.
8. **Hooks are structs of std::functions** (PoseInspectorHooks style); every App/ViewTools call site exists now.
9. **One pool key form**: `.anim` paths normalized through `FileData::getSystemPath` (asset-relative or absolute;
   editor passes `absolutePath()`), sub-clips via a per-model alias table to the animation index.
10. **Key indices are per revision**: edits return refs, drags recompute from the drag-start clip.
11. **Catalog plumbing is names and ids only**; properties and serialization are T2a's.
