# Animation Clips: the T0 contract (header index and decisions)

Companion of `2026-10-24-animation-clips-design.md` (section 9 binding, section 8 = tasks). T0 created every
public header below with compiling stubs (`// TODO(T<n>)` names the owning task). Later tasks implement them and
may only **add** to these headers. Each header starts with a "Contract" block (purpose, owner, thread-safety,
invariants): that block is the authority, this page is the index.

## Engine (`engine/core/`)

| Header | What | Implements |
|---|---|---|
| `animation/AnimationClip.h` | `ClipChannel`, `ClipWrapMode`, `ClipWarning`; value form `ClipData` (`ClipTrackData`, `ClipKey`, `ClipEventData`, `ClipSource`); immutable runtime `AnimationClip` (contiguous typed arrays, `Track` pointers) | T1a (`build`, `toData`) |
| `animation/AnimationClipIO.h` | `anim::parseClip` (result: data, warnings with JSON paths, error line:col), `writeClip` (canonical), `normalize`, Unity YAML detection + refusal text, GUID helpers (`generateClipGuid`, `readClipGuid`, `replaceClipGuid`), enum names | T1a |
| `animation/KeyframeSampling.h` | `anim::locateKey` + value samplers over const arrays with a `KeyCursor` (ActionSystem's entity tracks move onto them), `wrapClipTime`, `trackTime`, `sampleTrack`, `sampleClipTracks`, `collectCrossedEvents` | T1a (samplers), T1b (wrap/events) |
| `animation/ClipBinding.h` | path grammar (`parsePath`/`formatPath`, `#N`), `resolvePath`, `relativePath`, duplicate-sibling detection, `descendantPaths` | T1b |
| `animation/AnimationEvent.h` | `AnimationEvent {functionName, args (variant number/string/bool), state, time}` + accessors (implemented) | T1b (firing, Lua) |
| `animation/AnimationPlayerRuntime.h` | `anim::AnimationPlayerRuntime` (states, queue, rest map, pending callbacks), `ClipBindings`, `ChannelRest`, `ClipPreviewSession`, `AnimationStateInfo` (read-only view) | T1b |
| `animation/ModelSubClips.h` | sub-clip references `model#clip`, names, node paths, `buildSubClip` / `buildModelSubClip`, `listModelClips` | T1c |
| `pool/AnimationClipPool.h` | `get`/`getOrLoad`, `getGeneration`, `revision`, `invalidate`, `invalidateModel`, `clear`, `clearUnused` | T1a (+ T1c sub-clips) |
| `component/AnimationPlayerComponent.h` | authored `enabled, clips[{clip, guid, name, wrap}], defaultClip, playAutomatically, speed, defaultFadeTime (0.25)`; runtime `needsLoad, started, runtime`; `onEvent(const AnimationEvent&)`, `onFinish(const std::string&)` | T1b runtime, T2a editor plumbing |
| `object/AnimationPlayer.h` | Lua/C++ wrapper `AnimationPlayer` (play, crossFade, blend, queue, stop, rewind, sample, isPlaying, getState, clip list, settings, callbacks) and the `AnimationState` handle | T1b |
| `subsystem/ActionSystem.h` | public `previewPlayer(root, PlayerPreview, ClipPreviewSession&)`, static `sampleClip`, `resetPlayers`; private `updatePlayers`, `dispatchPlayerCallbacks` (already called from `update()`) | T1b (`ActionSystemPlayers.cpp`) |
| `registry/EntityRegistry.h` | `hierarchyRevision()` (returns 0) | T1b (bumps) |

Registration: `EntityRegistry.cpp` registers `AnimationPlayerComponent` (60th component of 64), `JasperEngine.h`
includes the new headers. `ComponentType::AnimationPlayerComponent` follows `PoseAnimatorComponent` and is
`LastComponentType`; Catalog has its name (`AnimationPlayerComponent` / `animationplayer`), display name
"Animation Player", icon (film) and component id. Not listed in Add Component, no properties, no Stream/Factory
support yet (T2a): `add_component` refuses it like any unlisted type.

## Editor (`editor/`)

| Header | What | Implements |
|---|---|---|
| `animation/AnimationClipDocument.h` | per-path shared document (PoseAnimationDocument clone), commit → `WriteFilesCmd` on the project history, merge keys, reload/poll, listeners, read-only sub-clip documents, `clip()` runtime form per revision | T2b |
| `animation/ClipValidation.h` | `ClipValidationReport` (per track `TrackBindingStatus` + nearest paths, per event `EventHandlerStatus`), `validate` (pure), `validateInScene`, `scriptFunctions`, `nearestPaths`, `trackLabel` | T2d |
| `animation/ClipConverters.h` | `convertEntityAnimation`, `extractModelSubClip`, `planUseAnimationPlayer`, `bakeChannels`, `uniqueClipPath`, `defaultClipDirectory` (pure: data and plans) | T2d |
| `command/type/CreateClipCmd.h` | the one scene-history composite (R4): writes files, runs caller-built scene commands, undo deletes/restores files only if unchanged | T2d |
| `animation/ClipRecordHooks.h` | Record listeners (`claims`/`record`, drag merge keys) and chained `beforeChannelWrite`/`afterChannelWrite` (replaces `poseInspectorHooks().beforeTransformWrite`; T0 did not migrate the Pose window) | T3a registry, T3c call sites |
| `animation/PreviewOwner.h` | one edit-mode pose owner per model root (`acquire` stops overlapping owners first) | T3a |
| `animation/AnimationClipInspectorHooks.h` | Inspector buttons: openClip, openEntityAnimation, createNewClip, convertToClip, extractSubClip, useAnimationPlayer, addToAnimationPlayer | T2c calls; T2d/T3 set |
| `animation/AnimationClipController.h` | clip mode without UI: bind root, open clip/state, preview/transport, `sampledBoundTransforms`, keying, Record, App integration; `shared(project)` for the window and MCP | T3a |
| `animation/AnimationClipAppHooks.h` | App wiring: stopTransientPreviews, ownsUndo, takesKeys, sceneClosed, dropClips, windowTestHook | T3a / T2b / T2d / T3b set |

App wiring done in T0 (`editor/App.{h,cpp}`): `stopTransientPreviews` calls the hook; a new
`LastFocusedWindow::AnimationClip` (set when `ownsUndo` says so) routes Ctrl/Cmd+Z/Y and Edit > Undo/Redo to the
project history (`undoGoesToProjectHistory()`); `takesKeys` mutes the main toolbar's letter shortcuts; scene close
calls `sceneClosed`; `App::dispatchClipDrop(sceneId, target, paths)` is the drop entry point for the Scene view and
Hierarchy (T2b calls it). With every hook unset, behaviour is unchanged.

## Build and tests

- Engine sources in `engine/CMakeLists.txt` (+ Xcode via `tools/xcode_sync_engine.py`, `--check` clean). Editor
  sources in the root `CMakeLists.txt`, including empty placeholder TUs for later tasks:
  `window/AnimationWindowClipSheet.cpp`, `AnimationWindowClipEvents.cpp` (T3b), `AnimationWindowClipCurves.cpp`
  (T3c), `window/InspectorWindowClips.cpp` (T2c, optional), `mcp/tools/AnimationClipTools.cpp` (T4, optional).
- `jasperengine-anim-tests` (`engine/tests/anim`, option `JASPERENGINE_ANIM_TESTS`, globbed `*.cpp`, defines
  `ANIM_FIXTURES_DIR` and `ANIM_POSE_FIXTURE_MODELS_DIR`); smoke tests `AnimContractSmokeTest.cpp`.
  Editor smoke tests `editor/tests/AnimationClipContractTest.cpp` (globbed `*Test.cpp`).

## Decisions where the spec left room

1. **ClipData and AnimationClip both exist.** Editors need a value type to copy, change and commit (keys as
   structs with a flattened float value, so copy/paste/delete/drag are uniform across channels); the runtime needs
   immutable contiguous arrays shared through the pool. `AnimationClip::build(ClipData)` is the only bridge (plus
   `toData`). The parser returns ClipData; the sub-clip builder produces ClipData; the document caches `clip()`.
2. **One wrap enum with `Default`.** `ClipWrapMode::Default` exists only for player entries (R14); a clip's wrap is
   never Default.
3. **Players live in ActionSystem**, implemented in a separate TU `subsystem/ActionSystemPlayers.cpp`, so they
   reach the private pose-blend accumulators directly (R1 rest term) without a new SubSystem or an interface; the
   call points in `update()` are already wired (stubs are no-ops).
4. **Player control goes through the `AnimationPlayer` object** (as PoseAnimator): Lua, Model integration, MCP
   and the Inspector all call it; nothing outside engine player code writes the runtime structs.
5. **Edit-mode preview state is a caller-owned `ClipPreviewSession`** passed to `previewPlayer`: rest = the
   editor's snapshot (`setRest` on scene edits, R6), and a preview can play any clip (document revision,
   target-less preview model) on any root, with or without a player component.
6. **`onFinish` carries the state name** (`FunctionSubscribe<void(const std::string&)>`), `onEvent` the
   `AnimationEvent` (R23).
7. **One composite command (`CreateClipCmd`)** for every file + scene action instead of one class per action;
   callers build the scene sub-commands.
8. **App hooks are a struct of std::functions** (`AnimationClipAppHooks`, PoseInspectorHooks style), so App.cpp
   needs no edit when the clip tasks land, and nothing in App includes window code for clips.
9. **No ViewTools change in T0.** A test-hook entry for `focus_window {window: "animation"}` needs a new schema
   property, which costs tool-definition budget (T4's, R28). `AnimationClipAppHooks::windowTestHook` is declared;
   T4 adds the `animation` property and the call in ViewTools when T3b sets the hook.
10. **Catalog plumbing is names and ids only**; `findComponents`, properties, Stream, Factory, Add Component,
    ProjectUtils add/remove and PropertyCmd stay T2a's.
11. **Not done in T0 (owned later):** `Engine::clearPools` hooks (T1a), `SceneResources.clips` (T1a/T2a),
    `ModelComponent.animationEntities` (T1c), Lua bindings and stubs, ScriptEvents, EntityCloner (T1b),
    `Util::isAnimationClipFile` and Project window plumbing (T2b).
