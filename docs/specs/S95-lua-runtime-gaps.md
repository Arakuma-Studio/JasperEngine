# S95 — Lua runtime gaps from the JasperArcade port

**Status: implemented** on `feature/s95-lua-runtime-gaps` (WP-1..WP-5). WP-6 (String Tables, S10)
is not in this branch: it is one design with `game-ui-localization.md` (its D-15), which ships it
(Deviations, WP-6). What the code does where it differs from this text is in "Deviations" at the
end.

Program "arcade engine gaps" (S92-S98). Source: the port's ledger,
`JasperArcade-Ported/docs/port/ENGINE_LIMITATIONS.md`, section "Scripting / runtime API (Lua)",
items S1-S32 (S33 belongs to S92). The port was made against `7f48d7e8d`; every claim below was
checked against `82ccd7c6a` (this branch's base). Paths are relative to the repository root;
`PORT/` is `/var/lib/orquester/workspaces/jaspersito/JasperArcade-Ported/`.

**Identifiers.** `S1`-`S33` are the ledger's item ids and nothing else. This spec's requirements
are `REQ-n`, its criteria `AC-n`, its decisions `D-n` and its work packages `WP-n`, so they never
clash with the ledger or with other specs' `R`/`S` numbers.

Labels: **verified** = run; **inferred** = from reading the cited code; **unverified** = cannot
be settled by reading, so an AC settles it first.

## 1. Goal

A Unity developer who ports a game should find the runtime behaving as Unity does. They should
not need a framework of workarounds. The port needed eleven game-side modules (`PORT/scripts/shared/`
`Timer`, `Rot`, `SceneFlow`, `Save`, `SceneReset`, `LateUpdate`, `App`, `ui/UIState`, `UIKit`,
`Loc`, data modules). Since the port, main has closed most of the gaps. S95 closes the rest:

- Math calls that crash, or that silently do the wrong thing.
- Transforms moved late in a frame and drawn one frame late; world poses that are a frame stale.
- Defaults that differ by entry point: cameras, canvas size, fixed step, mouse as touch.
- Android HTML UI documents rebuilt from their files on every return from the background.
- No data assets (ScriptableObject) and no string tables (localization).

## 2. Scope

**In scope**
- Every ledger item still open (§3), each with editor UI, a Lua API, MCP and docs where it is user-facing.
- Pinning tests for items fixed on main that have no test of the ported game's scenario
  (S16, S18, S21, S31).

**Out of scope**
- S33 (the clang argument leak) and the LuaBridge overload-dispatch patch
  (`PORT/docs/port/engine-patches/luabridge-overload-dispatch.patch`): both are **S92**.
- `System.openURL` and the safe area are **S93** (they exist already: `CoreClassesLua.cpp:4058`).
- Music that carries on across scene loads, crossfades and the audio part of S5 beyond
  DontDestroyOnLoad: **S97**.
- The physics step policy (the cap on steps per frame, P18): **S96**. S95 owns only where the
  fixed-step value comes from (REQ-13).
- A C++ API for data assets and string tables. These are Lua features; C++ games read the JSON
  (D-12).
- Nothing from the README do-not-copy list is involved. No OpenEngine code is used.

## 3. Triage

Counts: **16 FIXED, 3 PARTIAL, 13 MISSING** (two of them unverified: S27, S28), 0 GAME-SIDE,
0 RETRACTED. Commits are on main after `7f48d7e8d`.

| Ledger | Verdict | Evidence (current main) | What S95 does |
|---|---|---|---|
| S1 | MISSING | The Lua constructor list puts `(w,x,y,z)` before `(x,y,z,RotationOrder)` (`engine/core/script/binding/MathClassesLua.cpp:778-787`). Enums are plain integers in Lua (`LuaBridgeAddon.h:219`), so four numbers always pick the raw form. The guide still documents the Euler form (`docs/mcp/guide/lua-api.md` §3.2) | REQ-1: `Quaternion.euler`, `Quaternion.angleAxis`; the broken form is removed |
| S2 | FIXED | `a9e0a4efd` (engine coroutines, `self:invoke*`, Time); `Timer.*` (lua-api §2.13, §2.17; `ScriptScheduler.cpp:2309-2312`) | — |
| S3 | FIXED | `a9e0a4efd`: `Time.unscaledDeltaTime` (`CoroutineClassesLua.cpp:74`), `Engine.unscaledDeltatime` (`CoreClassesLua.cpp:2406`) | — |
| S4 | FIXED | `002d124f8`: `onEnable`/`onDisable` (`ScriptMessages.cpp:94-95`; lua-api §1.1) | — |
| S5 | FIXED | `7be98af28`: `DontDestroyOnLoad`, `persistSingleton`, and voices that play on through the move (`ScriptClassesLua.cpp:509-548`; lua-api §2.8.1) | music policy → S97 |
| S6 | FIXED | `03727c3f1`: a load in Play retires the scenes it replaces, so the current scene is built anew (`editor/Project.cpp:11138-11141`). Tested by `tools/mcp/tests/test_lifecycle.py:84-92` | — |
| S7 | MISSING | `CameraComponent.useTarget` defaults to true (`engine/core/component/CameraComponent.h:106`). `Camera(scene)` keeps that (`engine/core/object/Camera.cpp:14-24`), while the editor's create sets false (`editor/command/type/CreateEntityCmd.cpp:878`) | REQ-11 |
| S8 | PARTIAL | `Engine.callTouchInMouseEvent` exists (`Engine.cpp:172, 798`). The editor has a per-user Game view "Simulate Touch" (`editor/settings/SettingsRows.cpp:4507`). Exports have no project setting | REQ-12 |
| S9 | MISSING | Only a doc recipe: a JSON file or a Lua module (lua-api §2.4.2 "Data assets"; `docs/unity-glossary.md:292`) | REQ-20 to REQ-24: Data Assets |
| S10 | PARTIAL | Project Settings > Localization (languages, default) and the language matching exist, but only Title Art uses them (`engine/core/util/TitleArtLanguage.h:7-67`; `docs/unity-glossary.md:189`). No string tables | REQ-25 to REQ-30: String Tables |
| S11 | FIXED | `b870f5e96`: `System.vibrate([ms])`, `System.hapticFeedback` (`CoreClassesLua.cpp:4078`); Android, iOS and web (`navigator.vibrate`, `JasperEngineWeb.cpp:1672`) | — |
| S12 | PARTIAL | The canvas is never 0 by reading: `setCanvasSize` sets it before the view exists (`Engine.cpp:588-600`, since `d1a4a0d34`). Players call it before `init` (`engine/player/PlayerMain.cpp:314`, `editor/Generator.cpp:2549`). But `init` sees the preferred size, not the fitted layout, and the guide still says "can be 0" (`lua-api.md:519, 3673`) | REQ-14 |
| S13 | MISSING | `RegisterEngineEvent`/`RegisterEvent` capture the method value as an upvalue (`engine/core/script/LuaBinding.cpp:566-570`). `Event:addListener(self, "m")` already looks the method up at call time (`ScriptEvent.cpp:295`) | REQ-7 |
| S14 | MISSING | The fixed step is `Engine::updateTime` only (`Engine.cpp:862`). There is no Project Settings value (no `time.*` key in `SettingsRows.cpp`). Play start does not reset it (`editor/Project.cpp:6398-6420` resets the time scale, not the step), so a step leaks from one Play into the next | REQ-13 |
| S15 | MISSING | Created and imported names are unique across the whole scene: `CreateEntityCmd.cpp:133`, `ImportPrefabCmd.cpp:87` and `ProjectPrefabReplace.cpp:903` pass every scene entity to `ProjectUtils::makeUniqueEntityName` (`editor/util/ProjectUtils.cpp:903`) | REQ-4 |
| S16 | FIXED | `c4c5f0161`: `getChildren`, `getChild`, `findChild("a/b")` (`GameObjectClassesLua.cpp:1520-1523`). `Scene:findEntity(name, parent)` filters by ancestry (`EntityRegistry.cpp:741-745`), and LuaBridge checks overload arity (`LuaBridge.h:6937-6941`). The port's wrong result is unverified; it was likely S15's renames | AC-5 pins it |
| S17 | MISSING | `GameObject::getWorldPosition` calls `getComponent<Transform>` (`engine/core/object/GameObject.cpp:118-121`), which logs and throws on an entity without a Transform (`engine/core/ecs/ComponentArray.h:893-894`) | REQ-3 |
| S18 | FIXED | `03727c3f1`, as S6: a scene entered again is built from its file (from the Play-start snapshot if it is the played scene). Inferred for transforms: the test covers scripts only | AC-6 pins it |
| S19 | FIXED | `b870f5e96`: `System.isEditor()` (`CoreClassesLua.cpp:4075`, `System.cpp:263`); glossary `Application.isEditor` (`unity-glossary.md:207`) | — |
| S20 | FIXED | `0d9c3f74f`: `Log.info` (`CoreClassesLua.cpp:3592`) | — |
| S21 | FIXED | `03727c3f1`, as S18: the old runtime scene is retired, and its spawned objects with it (inferred) | AC-6 pins it |
| S22 | FIXED | `abf99cc5b`: a key that is not an XML name is stored as `<entry key="...">` (`engine/core/io/UserSettingsStore.cpp:25, 144-146`). Tested by `editor/tests/UserSettingsTest.cpp:76` | — |
| S23 | FIXED | `e31681d96`: `UIDocumentComponent` is in the events cleared on tag removal (`engine/core/component/ComponentEvents.h:62`, `Scene.cpp:153-158`). `002d124f8`: documents start after `init`/`onEnable` (lua-api §1.1, load order) | — |
| S24 | MISSING | Frame order: `onUpdate`, `scenes[i]->update` (where `RenderSystem::update` propagates transforms and builds camera matrices: `RenderSystem.cpp:17621, 17769`), PixelFX, timers, coroutines, `onPostUpdate` (`Engine.cpp:2683-2704`), then the draw. So a move in `onPostUpdate`, in a timer or in a coroutine is drawn a frame late (inferred) | REQ-5 |
| S25 | FIXED | `abf99cc5b`: `FileUtils::writeFileAtomic` (`UserSettingsStore.cpp:159`). Tested by `engine/tests/core/io/UserSettingsTest.cpp:48` (`AtomicOnCrash`) | — |
| S26 | MISSING | `getWorldPosition/Rotation/Scale` return the cached `transform.world*` (`GameObject.cpp:118-121, 143-146, 168-171`), which is refreshed only by the propagation in `RenderSystem::update` | REQ-6 |
| S27 | MISSING (unverified) | The chunk runs once per instance (lua-api §1.1, "Instance creation"), so a class-table field set at top level should be per instance. The port saw a stale field, root cause unknown (`PORT/scripts/shared/Framework.lua:34-48`) | REQ-8: test first, fix if red |
| S28 | MISSING (unverified) | The message is logged once per renderer (`engine/core/htmlui/RmlRenderer.cpp:1823-1828`). No test plays and stops with a document and checks the Console | REQ-19: test first, fix if red |
| S29 | FIXED | `002d124f8`: `UnregisterEngineEvent`/`UnregisterEvent` (`LuaBinding.cpp:768`; lua-api §1.3) | — |
| S30 | MISSING | Android: `APP_CMD_LOW_MEMORY` with no window calls `killContext()` (`engine/platform/android/NativeEngine.cpp:1726-1735`). That leads to `Engine::systemViewDestroyed` and `scenes[i]->destroy()` (`Engine.cpp:2892-2928`), then `UIDocumentSystem::destroy()`, which deletes the RmlUi context and marks every document for reload (`UIDocumentSystem.cpp:67-95`). After the return, `scriptLoadDocuments` loads them again and fires `onLoad` with no `onUnload` (`UIDocumentSystemScript.cpp:451-469`). Web context loss only sets a flag (`JasperEngineWeb.cpp:1299-1303`). The first frame after a resume gets the whole absence as its delta, up to 0.25 s: `clampNextDelta` is set only on a hold (`Engine.cpp:1368-1372`) | REQ-15 to REQ-18 |
| S31 | FIXED | `03727c3f1`, as S18 (inferred for particles) | AC-6 pins it |
| S32 | MISSING | `Quaternion(const Vector3*)` and `fromAxes(const Vector3*)` are bound (`MathClassesLua.cpp:783, 804`) and read three vectors from one pointer (`engine/core/math/Quaternion.cpp:185-194`). So `Quaternion(v3)` reads past the userdata, and `Quaternion(nil)` passes a null pointer (inferred). The `__tostring` crash on a metatable is unverified | REQ-2 |

## 4. Current state (open items only; the evidence is in §3)

- **Math (S1, S32).** Only these two bindings take a raw math pointer
  (`grep "const Vector[234]\*" engine/core/script/binding`). Every other bad argument becomes a
  LuaBridge error. The port's `PORT/docs/port/engine-patches/wrong_calls.lua` is a sweep of right
  and wrong calls; it skips the two crashing calls.
- **Missing Transform (S17).** Actions, tweens and the scene's system entities have no Transform.
  Every `GameObject` transform member calls `getComponent<Transform>`, so on those entities a Lua
  script gets a C++ exception turned into an error, plus an engine error line.
- **Frame order (S24, S26).** Transform propagation is one linear walk with a `needUpdate` flag per
  Transform (`engine/core/subsystem/TransformPropagation.h:20-54`). A pure per-Transform compute
  already exists (`computeWorldTransform`, `TransformPropagation.h:60+`). Nothing re-propagates
  after `RenderSystem::update`. The guide maps Unity's `LateUpdate` to `onPostUpdate`
  (`unity-glossary.md:108`), which promises same-frame drawing. The code does not deliver it.
- **Handler binding (S13).** Hot reload replays every registration against the new method
  (`LuaBinding.cpp:667`, `lua_reload::recordRegistration`). Assigning `self.onUpdate = other` at
  runtime has no effect.
- **Defaults (S7, S8, S12, S14).**
  - The canvas, scaling, pixels per unit and input deadzone reach exports through `game.json`
    (`engine/player/PlayerMain.cpp:308-330`, written by `editor/ExporterPrebuilt.cpp:643`) and
    through the generated `main` (`editor/Generator.cpp:2549`). No time setting does.
  - `editor/Project.cpp:11794-11803` saves and restores `maxDeltatime` only around a Step.
- **Scene names (S15).** `UniqueEntityNames` and `makeUniqueEntityName` (`ProjectUtils.cpp:848-930`)
  take a set of names: the callers decide its scope. Duplicate (`DuplicateEntityCmd.cpp:128-147`)
  builds it from every name in the scene.
- **View loss (S30).**
  - `Scene::destroy` is the only teardown (`Scene.cpp:1109-1113`). Every system's `destroy` frees
    GPU objects, and `UIDocumentSystem` also frees CPU state.
  - RmlUi can release GPU resources and keep the DOM: `Rml::ReleaseTextures` and
    `Rml::ReleaseCompiledGeometry` (`engine/libs/rmlui/Include/RmlUi/Core/Core.h:157, 165`).
  - Element handles are freed in `scriptContextDestroying` (`UIDocumentSystemScript.cpp:488-504`).
  - The MCP `send_input` app events (pause, resume, low_memory, hide, show) cannot reproduce a
    view loss.
- **Data and text (S9, S10).**
  - Script `properties` are parsed in an isolated Lua state and shown in the Inspector
    (lua-api §1.2). There is no list type.
  - S87 owns the language candidates, the matching and `TitleArt.setLanguage`/`onLanguageChanged`
    (`TitleArtClassesLua.cpp:318, 418`). The Game view can simulate a language in Play.

## 5. Reference

- No OpenEngine code applies.
- The bar is Unity's documented behaviour: `Quaternion.Euler`/`AngleAxis`, `LateUpdate`,
  `transform.position` (always current), `Time.fixedDeltaTime` and Project Settings > Time, the
  Input System's touch simulation, `ScriptableObject` with `CreateAssetMenu`, the Localization
  package's String Tables and Smart Strings, and `OnApplicationPause`.
- The port's workarounds show the API shape games want:
  - `PORT/scripts/shared/Rot.lua` (Euler in Unity's order).
  - `LateUpdate.lua` (the same-frame late hook).
  - `SceneReset.lua` (editor re-entry).
  - `App.lua` and `ui/UIState.lua` (background, DOM replay).
  - `Loc.lua` (`{0}` placeholders, missing key returns the key).
  - `Save.lua` (key escaping, now unneeded).
  - `Framework.lua` (`mouseAsTouch`, the fixed-step reset).
- Take their behaviour, not their code: those are game modules.

## 6. Requirements

### Functional — Lua API correctness (S1, S15, S16, S17, S27, S32)

- **REQ-1 Euler and angle-axis factories.**
  - Add the statics `Quaternion.euler(x, y, z [, order])` (degrees in default units; the default
    order is that of `Quaternion(x, y, z)`) and `Quaternion.angleAxis(angle, axis)`.
  - Remove the Lua constructor `(x, y, z, RotationOrder)`. It never ran: the raw form always won.
  - Lua-api §3.2 gets a table that names the `RotationOrder` equal to Unity's `Quaternion.Euler`
    composition (Z, then X, then Y). A test proves it.
- **REQ-2 No crash from a wrong math call.**
  - Remove the `const Vector3*` forms of the `Quaternion` constructor and of `fromAxes` from Lua.
    The three-vector form stays.
  - Every math constructor and operator raises a Lua error on nil, a wrong type or a wrong count.
    It must never read out of bounds or crash.
  - `tostring` of any bound class's metatable returns a string.
  - Lint: no Lua binding takes a raw pointer to a math type (a check in `tools/lua_api/tests`).
- **REQ-3 Entities without a Transform.**
  - `getWorldPosition`, `getWorldRotation`, `getWorldScale`, `position`, `rotation`, `scale` and
    `parent` return `nil` with no engine error.
  - The transform setters raise `GameObject '<name>' has no Transform`.
- **REQ-4 Names unique among siblings.**
  - Applies to create (editor, MCP `create_entity`, Lua `GameObject(scene)` named later),
    duplicate, paste, prefab import and prefab replace.
  - A name is numbered (`Blue 2`) only when a sibling under the same parent (or the scene root)
    has it.
  - Rename stays free (Unity).
- **REQ-8 Top-level class fields (S27).**
  - A test settles S27: a top-level `M.LIMIT = 5` and `M.cache = {}` are read in `init` by two
    instances of the same file and by an instance created later by `GameObject.instantiate`.
  - If it is red, find the cause and fix it so each instance sees its own chunk's values.
  - If it is green, record S27 as not reproduced and remove the warning from the guide, if any.

### Functional — frame order and transforms (S13, S24, S26)

- **REQ-5 Late transform pass.**
  - After `onPostUpdate` and before the draw, every running scene whose Transforms changed since
    its `RenderSystem::update` re-runs the transform stage for what changed. The stage is
    propagation plus everything in `RenderSystem::update` that reads world matrices after it: skin
    bones, lattice followers, cameras' view, projection and frustum, the audio listener pose, and
    sprite-batch bounds.
  - So a move made in `onPostUpdate`, a timer, a coroutine, a PixelFX callback or a transition
    callback is drawn in the same frame.
  - `onPostUpdate` stays the `LateUpdate` mapping. No new event name.
- **REQ-6 Fresh world pose on demand.**
  - `getWorldPosition/Rotation/Scale` and `modelMatrix` return the current pose: when the
    Transform or an ancestor is dirty, the pose is composed from the ancestors' local values.
  - This writes no state: no flag is cleared, so propagation is unchanged.
  - MCP `get_runtime_entity` reports the same values.
  - `go:updateTransform()` stays, for C++ callers.
- **REQ-7 Handlers looked up at call time.**
  - Subscriptions made with a method name (`RegisterEngineEvent`, `RegisterEvent`) call
    `self[methodName]` at each call, as `Event:addListener(self, "m")` and `self:invoke("m")`
    already do.
  - Re-assigning the method takes effect at the next call.
  - A value that is not a function skips the call and warns once per subscription.

### Functional — runtime defaults (S7, S8, S12, S14)

- **REQ-11 Camera default.**
  - `Camera(scene)` (Lua and C++) starts in rotation mode (`useTarget` false), as the editor does.
  - `setTarget` and assigning `target` turn target mode on (`Camera.cpp:285-292`).
  - Add a read-write `useTarget` to the Lua `Camera`.
  - Stored scenes are unaffected (the component default stays).
- **REQ-12 Simulate Touch From Mouse.**
  - New setting: Project Settings > Input > Simulate Touch From Mouse (`input.simulate_touch_from_mouse`,
    default false).
  - It sets `Engine.callTouchInMouseEvent` at start in desktop and web exports and in Play.
  - The editor's per-user Game view preference also turns it on in Play (either one is enough).
- **REQ-13 Time settings.**
  - New settings: Project Settings > Time > Fixed Timestep (`time.fixed_timestep`, default 1/60 s)
    and Maximum Allowed Timestep (`time.maximum_delta_time`, default 0.25 s).
  - Exports apply them before `init` (game.json and the generated `main`). Play start applies them,
    and Play stop restores them.
  - New per-scene setting: Fixed Timestep (`fixed_timestep`, 0 = the project's). It applies per D-7.
  - `Time.fixedDeltaTime` still sets the value at runtime.
- **REQ-14 Canvas in `init`.**
  - `Engine.canvasWidth/Height` are never 0.
  - Before the first scene's `init`, the engine computes the layout from the screen size when the
    platform knows it (desktop window, web canvas, iOS view), so `init` sees the size of the first
    frame.
  - Otherwise (Android before the window) `init` sees the preferred size, and `onCanvasChanged`
    fires before the first `onUpdate` if it differs.
  - Remove guide pitfall 19 and the "can be 0" note.

### Functional — app lifecycle and HTML UI (S28, S30)

- **REQ-15 A view loss keeps the UI.**
  - `Engine::systemViewDestroyed` and `systemViewLoaded` release and restore GPU resources only.
    Every scene system keeps its CPU state.
  - `UIDocumentSystem` keeps its RmlUi context, documents, DOM, element handles, listeners, data
    models, scroll positions and `UIState` set by scripts. It releases its textures, compiled
    geometry and render targets, and remakes them on demand.
  - No `onLoad` or `onUnload` fires for a view loss.
  - This covers Android (`APP_CMD_TERM_WINDOW` and `LOW_MEMORY` without a window, and
    `EGL_CONTEXT_LOST`) and the web (`webglcontextlost` and `restored`). The web path becomes a
    real release and restore (today it only sets a flag).
- **REQ-16 A real reload fires `onUnload` first.** Any document reload that rebuilds the DOM
  (file change, `needReload`) fires `onUnload` before the new `onLoad`. The S17 live patch reloads
  stay as they are.
- **REQ-17 Resume delta.**
  - The first frame after a resume (`applyPause(false)`) gets its delta clamped, as after a hold
    (`clampNextDelta`, `Engine.cpp:2477-2481`).
  - The frame that restores the view does too. No game clock jumps by the time spent in the
    background.
- **REQ-18 Agents can cause a view loss.**
  - MCP `send_input` `app` actions gain `view_lost` and `view_restored`.
  - In editor Play they run the running scenes' GPU release and restore (not the editor's renderer).
  - `get_play_state` `platform.lifecycle` counts them.
- **REQ-19 No "outside a frame" error at Play start or stop (S28).**
  - A test plays and stops a scene with an HTML document, three times, and reads the Console.
  - If the error appears, make the edit-time document's offscreen render happen inside a frame, or
    skip it.

### Functional — Data Assets (S9)

- **REQ-20 Data classes.**
  - A Lua module with the annotation `---@dataasset` on its first lines is a data class. Its
    `properties` table (§1.2 entries) is the schema, and its functions are methods of every asset
    of the class.
  - The `properties` types gain `list` with `of = <type>`, for data classes and scripts alike.
    Entity references are refused in a data class.
- **REQ-21 The `.dataasset` file.**
  - JSON `{version: 1, class, values}` with a GUID in its `.meta` (S04).
  - Assets > Create > Data Asset > <Class> makes one.
  - The Inspector edits it with property rows, undoable, with list add, remove and reorder.
  - Renaming a field in the class keeps unknown values in the file. The check reports them.
- **REQ-22 References.**
  - A script or data-class property typed with a data class name (or
    `type = "asset", assetType = "dataasset"`) holds a `.dataasset`. At runtime it is the asset's
    table.
  - The same asset is one shared table per run.
  - Changes made at runtime are never written back to the file (unlike Unity).
- **REQ-23 Lua.**
  - `DataAsset.load(path)` returns the shared table (or `nil, message`).
  - `DataAsset.loadAll(className)` returns every exported asset of a class.
  - `DataAsset.create(className)` returns a new table with the defaults.
  - `DataAsset.classOf(t)` and `DataAsset.pathOf(t)`.
- **REQ-24 MCP and export.**
  - New tool `edit_data_asset` (actions `create`, `get`, `set`, `list`; `set` is one undo step and
    returns validation errors).
  - `get_asset_info` shows the class and values.
  - Exports cook every `.dataasset` and its class.

### Functional — String Tables (S10)

- **REQ-25 Tables.**
  - A `.stringtable` asset (JSON v1, GUID): keys, and per key a value for each project language.
  - A value is a string, or plural forms (`zero`, `one`, `two`, `few`, `many`, `other`).
  - Languages are Project Settings > Localization's (S87).
- **REQ-26 Lookup.**
  - `Localization.get(key [, args])` formats `{0}`, `{name}` and plural selection from the `count`
    argument (or `{0}`), using CLDR cardinal rules for the project's languages.
  - Fallback order: the current language, its base language, the project default, then the key
    itself (warned once per key).
  - `"Table:key"` addresses one table. A bare key searches all tables, and a key defined in two
    tables is a check error.
- **REQ-27 One language.**
  - `Localization.language` (rw) is the game's language. It is the same value as
    `TitleArt.getLanguage/setLanguage`, so titles and strings switch together.
  - `Localization.onLanguageChanged` is the same event as `TitleArt.onLanguageChanged`.
  - Other Lua: `Localization.languages` (ro, the project's) and `Localization.has(key)`.
  - The start language follows S87's candidate order.
  - Project Settings > Localization > Remember Language Choice (`localization.remember_language`,
    default true) stores a chosen language in UserSettings and applies it at the next start.
- **REQ-28 Bound text.**
  - The component Text gets Localization Key (`localizationKey`). Its text follows the key and the
    language.
  - HTML UI elements with `data-loc="key"` get their text set at load and on every language
    change. `el:setLocalized(key [, args])` does the same from Lua.
- **REQ-29 Editor.**
  - Window > Localization > String Tables: a grid of keys × languages with search, add and remove,
    missing cells marked, undoable edits, and CSV and XLIFF import and export (S87's code).
  - The Text Inspector has a key picker.
  - The Game view's simulated language (S87) applies to strings.
- **REQ-30 MCP and export.**
  - New tool `edit_string_table` (actions `create`, `get` with missing keys per language, `set`,
    `remove`, `import`, `export`).
  - Exports cook the tables with the project languages only.

### Platform & backends

- **REQ-31** Everything above works on Windows, Linux, macOS, Android, iOS and HTML5, and on all
  five graphics backends.
  - REQ-15's release and restore are needed on Android and the web. They must also run correctly
    on the others: Metal and D3D11 never lose the view during play, and an explicit call
    (REQ-18, tests) still works.
- **REQ-32** No feature here needs compute, SSBOs or anything else on the forbidden list.

### Performance

- **REQ-33**
  - With nothing moved after the scenes' update, REQ-5 costs at most one counter compare per
    scene.
  - With one camera moved in `onPostUpdate` in a scene of 10 000 static Transforms, the late pass
    takes ≤ 10% of that scene's main transform pass (bench).
- **REQ-34** REQ-7 adds ≤ 5% to the time of 1 000 `onUpdate` dispatches (`LuaBindingBenchTest`).
- **REQ-35** REQ-6 leaves the clean path at one walk of the ancestor flags. A dirty chain of depth
  d costs d TRS compositions. No allocation either way.
- **REQ-36** Unused Data Assets and String Tables cost nothing: no load, no per-frame work.
  `Localization.get` is one hash lookup plus the formatting.

### Data & compatibility

- **REQ-37** The new project and scene keys are optional, with defaults, and need no version bump.
  `.dataasset` and `.stringtable` start at version 1. No stored key changes meaning:
  - S7 changes only the constructor, not the component default.
  - S15 changes only new names.

### Editor UX, Agent UX, docs

- **REQ-38** Every user-facing item gets the following:
  - Editor UI with Unity names and undo.
  - Lua stubs (`engine/core/script/binding/stubs/` fragments or the extractor's OVERRIDES).
  - `lua-api.md` and `unity-glossary.md` rows (Quaternion.Euler, Camera, Time settings,
    ScriptableObject, Localization, Simulate Touch).
  - `docs/scripting.md` sections for Data Assets and String Tables.
  - The MCP guide and an eval case for each new tool text (`tools/mcp/eval/cases.py`).
  - `tool_budget.py --check` passes after the budget is regenerated.

## 7. Design constraints & decisions

- **D-1 Math.**
  - The factories are statics, so no overload dispatch can misread them (S92's R66 costs do not
    apply).
  - `Quaternion(w, x, y, z)` stays as it is.
  - The removed constructor forms never worked or never were safe, so their removal needs no
    migration.
  - The guide's §3.2 "`Quaternion(x, y, z, RotationOrder.XYZ)`" row is replaced by `Quaternion.euler`.
- **D-2 Missing Transform.** The members are `lua_CFunction`s or return `std::optional`. They test
  with `findComponent<Transform>`. No exception path.
- **D-3 Late pass, not a moved event.** Unity runs `LateUpdate` after animation, and so does
  `onPostUpdate` (the scenes' update evaluates animations). Moving the event earlier would break
  that.
  - So `RenderSystem` exposes its transform stage as one function and runs it a second time when
    needed.
  - "Changed" is the Transform array's change tick from the S07 change tracking (`markChangedAt` in
    `TransformPropagation.h:49`), compared with the tick the main pass ended at. No array walk is
    needed to find out.
- **D-4 Pure on-demand pose.** Write a free function
  `computeWorldPose(EntityRegistry&, Entity) -> {Vector3, Quaternion, Vector3, Matrix4}` next to
  `computeWorldTransform`.
  - It walks up to the first clean ancestor (or the root), then composes downwards into locals.
  - It must give bit-identical results to propagation: same `composeTRS` and `affineMultiply`
    order, UI pivot shift included.
- **D-5 Call-time lookup.**
  - The closure keeps the method name as an upvalue instead of the function. Each call does one
    `lua_getfield` (interned string) on `self`.
  - Hot reload's replay stays, since tags and guards still need it, but it no longer has to swap
    functions.
- **D-6 Camera.** Only the constructor changes. An old script that called `Camera(scene)` and then
  `setTarget(...)` still aims, because `setTarget` turns target mode on. A script that relied on
  look-at at the origin without setting a target changes. The guide's migration note says so.
- **D-7 Fixed step across loads.** This stays close to Unity, where a script's
  `Time.fixedDeltaTime` survives loads and nothing is per scene, and it also fixes the port's case.
  - A `SceneManager.loadScene` applies the incoming main scene's override when it has one.
  - When it has none and the previous main scene had one, the project value comes back.
  - Otherwise the current value stays.
  - Added layers and child scenes never change it.
- **D-8 Touch simulation.** One project setting, applied once at start. It does not replace the
  per-user editor preference, which is for testing.
- **D-9 Canvas.** `computeViewLayout` already exists (`Engine.cpp:1821`). The players call it with
  the window size when they have one, before `systemStartProject`. No new API.
- **D-10 Sibling scope.** The callers build the name set from the target parent's children (or
  the roots). `UniqueEntityNames` stays as it is. Hierarchy rename and MCP `rename` keep the name
  as typed.
- **D-11 View loss.**
  - `SubSystem` gains `virtual void releaseGpu()` and `virtual void restoreGpu()`. Their defaults
    call `destroy()` and `load()`, so every other system behaves as today.
  - `Scene::releaseGpu/restoreGpu` call them. `Engine::systemViewDestroyed/Loaded` call the scene
    versions instead of `destroy/load`. Real teardown (scene unload, shutdown) still calls
    `destroy`.
  - `UIDocumentSystem` overrides both. It keeps the `HtmlUIContext` and releases through
    `Rml::ReleaseTextures`, `Rml::ReleaseCompiledGeometry`, its texture-mode targets and
    `RmlRenderer`'s GPU state, which are remade at the next render.
  - Android keeps dropping the EGL context on `LOW_MEMORY` with no window: the GPU memory goes
    back, and the UI's CPU state stays.
  - The black frames while textures reload stay (out of scope; see §10).
- **D-12 Data Assets.**
  - Data Assets are Lua tables: the class module is the table's `__index`, and values decode from
    JSON with references resolved (asset refs by GUID, other data assets lazily).
  - Loading is lazy: first reference or `load`.
  - The isolated parser that reads script `properties` reads data classes too. `list` is added
    there once, for both.
  - C++ games read the cooked JSON (`asset://`) themselves. No C++ API in v1.
- **D-13 String Tables.**
  - `TitleArtLanguage` stays the one place for tags, matching and the global language.
    `Localization` is a thin owner of tables over it.
  - The S87 CSV and XLIFF code is reused for import and export.
  - Plural rules: a compiled CLDR cardinal table for the languages S87 accepts. An unknown language
    uses `one` and `other`.
  - The HTML UI part lives in its own new file under `engine/core/htmlui/` with one call site in
    document load, to stay out of S93's files.

API sketch (Lua):

```lua
local q = Quaternion.euler(10, 45, 0)                    -- REQ-1
local r = Quaternion.angleAxis(90, Vector3(0, 1, 0))
RegisterEngineEvent(self, "onPostUpdate")                -- REQ-5: drawn this frame
local p = self.target:getWorldPosition()                 -- REQ-6: current, never a frame late
local cam = Camera(self.scene); cam.useTarget = false    -- REQ-11 (already false)
local items = DataAsset.loadAll("ItemData")               -- REQ-23
Localization.language = "es"                              -- REQ-27 (titles follow)
local s = Localization.get("hud.coins", { count = 3 })   -- "3 monedas"
```

MCP:
- `send_input` `{type: "app", action: "view_lost" | "view_restored"}`.
- `set_project_settings` keys `time.fixed_timestep`, `time.maximum_delta_time`,
  `input.simulate_touch_from_mouse`, `localization.remember_language`.
- `set_scene_settings` `fixed_timestep`.
- New tools `edit_data_asset {action, path, class?, values?}` and
  `edit_string_table {action, path, keys?, entries?, format?, file?}`.

## 8. Acceptance criteria

"Red" is the run that fails without the change. Unit tests go where `docs/testing.md` "Where a
test goes" says. MCP tests are `CI = True`.

| AC | Verifies | Check (red control) |
|---|---|---|
| AC-1 | REQ-1 | `LuaMathBindingTest.EulerFactoryMatchesConstructor`: `Quaternion.euler(x,y,z)` equals `Quaternion(x,y,z)` over a grid of angles. With the order the guide names, it equals `angleAxis(y,Y)*angleAxis(x,X)*angleAxis(z,Z)` (Unity's). `angleAxis` equals `Quaternion(a, axis)`. Red: `Quaternion.euler` is nil |
| AC-2 | REQ-2 | `LuaMathBindingTest.WrongCallsRaise`: the `wrong_calls.lua` sweep (every math constructor and operator × a pool of nil, numbers, strings, tables, vectors, matrices, GameObjects) under `pcall`. Every call returns or raises; the process lives; under ASan no report. `Quaternion(nil)`, `Quaternion(Vector3())` and `tostring(debug.getmetatable(Vector3()))` are in the pool. Red: the first two crash or read out of bounds (ASan) before |
| AC-3 | REQ-2 | `tools/lua_api/tests`: a check fails on a binding that takes `const Vector*`, `Matrix*` or `Quaternion*`. Red: it flags `MathClassesLua.cpp:783, 804` before the fix |
| AC-4 | REQ-3 | `LuaHierarchyTest.NoTransformGivesNil`: on an Action entity, every getter is nil and `EXPECT_NO_ENGINE_ERRORS()`; a setter raises the message. Red: an error is logged and Lua raises |
| AC-5 | REQ-4, S16 | `editor/tests/command/EntityNamesTest.cpp`: create "Blue" under A and under B (both stay "Blue"); a second under A becomes "Blue 2"; duplicate and prefab import number among siblings only. `test_entity_names.py::test_names_unique_among_siblings` (new file) through `create_entity`. `LuaHierarchyTest.FindEntityUnderParent`: `scene:findEntity("Blue", B)` is B's child and `B:findChild("Blue")` agrees. Red: the second "Blue" is "Blue 2" before |
| AC-6 | S6, S18, S21, S31 pinning | `test_lifecycle.py::test_reentry_rebuilds_scene`: in Play, A moves an object, hides one, instantiates three, emits particles. Load B, load A again: transforms and active flags equal the saved ones, the instances are gone, the particle count is 0. Then the same with `loadScene(current)`. Expected green (main fixed it); if red, it is a bug to fix in WP-1 |
| AC-7 | REQ-8 | `ScriptLifecycleTest.TopLevelClassFieldsPerInstance` (editor tests, ScriptTestHarness). The result decides S27: red → fix, and the test stays; green → the ledger item is closed as not reproduced, and Deviations records it |
| AC-8 | REQ-5 | `editor/tests/engine/render/LateTransformPassTest.cpp`: a camera follows a target. The target moves in `onUpdate`, the camera in `onPostUpdate`, in a timer and in a coroutine. After each frame, the camera's view matrix (and a child's `modelMatrix`) match the poses set that frame. Red: one frame behind before |
| AC-9 | REQ-33 | Bench `LateTransformPassBench` in `engine/tests/core/bench`, thresholds in `tools/perf/bench-thresholds.json`: idle late pass ≤ 1 µs per scene, and the one-camera case ≤ 10% of the main pass |
| AC-10 | REQ-6 | `LuaHierarchyTest.WorldPoseFreshAfterParentMove`: move the parent in `onUpdate`, read the child's world position, rotation, scale and `modelMatrix` the same frame. They equal the values after propagation, bit for bit. Red: last frame's values |
| AC-11 | REQ-7 | `LuaReloadTest.HandlerLookedUpAtCallTime`: replace `self.onUpdate` after registering; the next frame calls the new one. Set it to nil: no call, one warning. Bench `LuaBindingBenchTest` dispatch within REQ-34. Red: the old function runs |
| AC-12 | REQ-11 | `LuaHierarchyTest.CameraUseTargetSwitchesToRotation` (see Deviations, Lua compatibility): `Camera(scene).useTarget == true` as before; `useTarget = false` makes the view follow the Transform rotation; `setTarget` turns it on. Red: `useTarget` is nil before |
| AC-13 | REQ-12 | `test_input.py::test_simulate_touch_from_mouse`: with the setting on, a `send_input` mouse click gives `onTouchStart` in Play. The prebuilt-player `game.json` carries it (unit test of `ExporterPrebuilt`). Red: no touch |
| AC-14 | REQ-13, D-7 | `EngineTimeSettingsTest` (editor tests): project 1/50 applied at Play start. A script sets 1/120; Play stop restores; the next Play starts at 1/50. Scene C's override of 1/400 applies on load; a load from C to the menu (no override) restores 1/50; a load from the menu to D (no override) keeps a script value. MCP `test_time_settings.py` (new) sets and reads both keys. Red: the step leaks across Plays before |
| AC-15 | REQ-14 | `EngineCanvasTest.InitSeesFirstLayout`: FITHEIGHT, preferred 1920×1080, screen 2400×1080 known before start. The script's `init` reads 2400×1080. Without a screen size it reads 1920×1080, and `onCanvasChanged` precedes the first `onUpdate`. Red: `init` reads 1920×1080 with a known screen |
| AC-16 | REQ-15, REQ-18 | `engine/tests/htmlui/TestViewLoss.cpp`: load a document. A script sets a text, adds a class and a listener, and scrolls. Then `Scene::releaseGpu`, `restoreGpu` and one frame. The element handle is the same and valid, the text, class, scroll and listener (a dispatched click) are kept, `onLoad` count is 1, `onUnload` is 0, and it renders the same pixels (golden compare). `test_app_lifecycle.py::test_view_loss_keeps_ui` through `send_input` `view_lost`/`view_restored`. Red: the DOM is back at the file's state and `onLoad` fires twice |
| AC-17 | REQ-15 (device) | `tools/android-emulator-test.sh` gains a step that runs `am send-trim-memory <pkg> HIDDEN` after Home (or the equivalent path). The test project's script logs a counter kept only in the DOM, and the expect file requires it after the return, with no second `[HTML UI] SVG plugin initialised`. Red: the counter is reset before. Web: manual, recorded in Progress. In Chrome, `WEBGL_lose_context.loseContext()`/`restoreContext()` from the console keeps the UI |
| AC-18 | REQ-16 | `TestDocumentReload` gains `ReloadFiresUnloadFirst`: changing the file fires `onUnload`, then `onLoad`. Red: no `onUnload` |
| AC-19 | REQ-17 | `EngineFrameTest.ResumeClampsDelta`: pause, advance the clock 5 s, resume. The first frame's `Time.deltaTime` ≤ the fixed step. Red: 0.25 |
| AC-20 | REQ-19 | `test_app_lifecycle.py::test_play_cycle_no_outside_frame_error`: a scene with a document, three play start/stop cycles, `read_console` has no "outside a frame". The result decides S28 (red → fix, as for AC-7) |
| AC-21 | REQ-20 to REQ-24 | Unit: `DataAssetTest` (parse class, list type, unknown fields kept, refs by GUID, shared table, `create` defaults, no write-back after Play). MCP: `test_data_assets.py` (create class and asset, set values, undo, a script property typed with the class, Play reads it, `loadAll`). Export: prebuilt player smoke reads an asset. Eval case for `edit_data_asset`. Red: the tool is missing |
| AC-22 | REQ-25 to REQ-30 | Unit: `LocalizationTest` (fallback chain, `{0}`/`{name}`, plural rules for en, es, ru, ar, ja; `Table:key`; duplicate-key check; language shared with TitleArt; Remember Language Choice through UserSettings). htmlui: `data-loc` updates on a language change. MCP: `test_localization.py` (create, set, missing per language, CSV round trip, Play with the Game view's simulated language). Eval case for `edit_string_table`. Screenshots of the String Tables window from `tools/editor-shot.sh`. Red: no `Localization` |
| AC-23 | REQ-38 | `tools/lua_api/tests/test_lua_api.py` (doc test of the guide's Lua blocks), `test_docs.py`, `tool_budget.py --check`, `gen_tool_docs.py` with no diff |

## 9. Dependencies & conflicts

- **S92** (LuaBridge overload dispatch, S33).
  - WP-1 edits `MathClassesLua.cpp` constructors; S92 patches `engine/libs/luabridge3/LuaBridge.h`
    and may reorder overloads in the same file.
  - Rule: WP-1 rebases on S92's patch if it landed first, and AC-2 runs on the patched LuaBridge.
  - The crashes in S32 are ours either way: they are in our bindings, not in dispatch.
- **S93** (HTML UI).
  - WP-4 edits `engine/core/subsystem/UIDocumentSystem*.cpp`, `engine/core/htmlui/RmlRenderer.*`
    and `HtmlUIContext`.
  - WP-6 adds `data-loc` in a new file.
  - S93 owns the HTML UI guide pages; WP-6 adds one short section there, last to merge.
- **S96** (physics). It owns step caps and P18. WP-3's Time settings only set
  `Engine::updateTime`/`maxDeltatime`, and S96 reads them.
- **S97** (audio). It owns music across loads. Nothing here touches audio, except the listener
  pose in REQ-5's stage, which is a read.
- **S98** (editor and MCP). `create_entity` naming (WP-1) and new tools (WP-5, WP-6) touch
  `tools/mcp/budget.json`, `docs/mcp/TOOLS.md` and `cases.py`: regenerate after each merge.
- **S87** (title localisation): WP-6 builds on `TitleArtLanguage`, without moving or renaming it.
- **Hotspots inside S95.**
  - `Engine.cpp`: WP-2 edits the update loop (2683-2705), WP-3 `setCanvasSize` and start
    (588-600, 2075-2141), WP-4 the view functions and `applyPause` (2156-2211, 2892-2928, 3027-3061).
    These are separate functions, so merges are textual only.
  - `lua-api.md`: each package edits only its sections, and the pitfall list is append-only (WP-3
    removes item 19).
  - `stubs/`: new fragments per package (`DataAsset.lua`, `Localization.lua`); WP-1 uses the
    extractor's OVERRIDES for `Quaternion`.
  - `editor/settings/SettingsRows.cpp`, `ProjectSettingsWindow.cpp`, `editor/Stream.cpp`: WP-3 and
    WP-6 add separate rows and keys.

## 10. Risks & defaults

- **S27, S28 may not reproduce.** Default: the test lands either way, and the item closes as
  "not reproduced on main", with the run recorded in Deviations.
- **The late pass misses a consumer of world matrices.**
  - Default: the stage function is the only place that reads them in `update`, and AC-8 covers
    camera, child and skin.
  - A consumer found later moves into the stage.
- **Bit-identical world poses (D-4).** If FMA contraction differs between the two paths, the
  tolerance is 0 ULP for the position on GCC and Clang with the project's flags. If MSVC differs,
  AC-10 compares within 1 ULP there and records it.
- **Camera default change (D-6)** can move a script-made camera that relied on look-at at the
  origin. Default: accepted, with a guide migration line. Scene data is untouched.
- **View loss on drivers that keep the context.** Release and restore cost a texture reload.
  Default: accepted; it only happens when the platform drops the view.
- **Black frames while textures reload after an Android return** remain. Default: out of scope
  here. A follow-up could keep a snapshot of the last frame, which S30's ledger mentions.
- **`list` in script properties** changes the Inspector rows for scripts too. Default: new type
  only; existing types are untouched.
- **Plural rules data size.** Default: the cardinal rules only (no ordinals), for S87's language
  list, ≤ 4 KB.

## 11. Work packages

Six packages. Each has its own worktree and branch off this spec's branch, and they merge into one
integration branch. Order of landing: WP-1, WP-2, WP-3 (small, independent), then WP-4, WP-5, WP-6.
None waits for another, except that WP-6's Text component key goes after WP-5's `list` parser if
both touch `ScriptProperty.cpp` (WP-6 does not need it, so they stay disjoint).

| WP | Items | Size | Owns | Shared hotspots and how to avoid conflicts | Depends on |
|---|---|---|---|---|---|
| **WP-1 Lua API correctness** | S1, S15, S16, S17, S27, S32 (REQ-1 to REQ-4, REQ-8), AC-1 to AC-7 | M | `MathClassesLua.cpp`, `engine/core/math/Quaternion.*` (only if needed), the `GameObject` transform members in `GameObjectClassesLua.cpp` and `GameObject.cpp` (getters only: `getWorld*` stays for WP-2), `editor/util/ProjectUtils.*`, `CreateEntityCmd.cpp`, `DuplicateEntityCmd.cpp`, `ImportPrefabCmd.cpp`, `ProjectPrefabReplace.cpp`, paste, `engine/tests/core/script/LuaMathBindingTest.cpp`, `LuaHierarchyTest.cpp`, `editor/tests/command/EntityNamesTest.cpp`, `test_lifecycle.py` (new test), `tools/lua_api/tests` (lint) | `GameObject.cpp`: WP-1 changes the `nil` handling in the transform getters; WP-2 changes what `getWorld*` compute. WP-1 lands first, and WP-2 builds on its `findComponent` form. S92 on `MathClassesLua.cpp` (§9) | S92's patch, if it is first |
| **WP-2 Frame order and transforms** | S13, S24, S26 (REQ-5 to REQ-7, REQ-33 to REQ-35), AC-8 to AC-11 | L | `RenderSystem.cpp` (extract the transform stage), `TransformPropagation.h` (`computeWorldPose`), `GameObject.cpp` `getWorld*`/`modelMatrix`, `LuaBinding.cpp` `luaRegisterEventImpl` and `LuaReload.cpp` replay, the update loop in `Engine.cpp` (2683-2705), MCP `get_runtime_entity` world values, benches | `Engine.cpp` (only the update loop), `lua-api.md` §1.3 and §4.2 | WP-1 (getters' form) |
| **WP-3 Runtime defaults** | S7, S8, S12, S14 (REQ-11 to REQ-14), AC-12 to AC-15 | M | `Camera.cpp`/`.h` and the Camera binding rows, `Engine.cpp` `setCanvasSize`/start, `engine/player/PlayerMain.cpp`, `engine/project/main.cpp`, `editor/Generator.cpp` (main text), `editor/ExporterPrebuilt.cpp` (game.json), `editor/Project.cpp` `resetEngineConfigs` and the Play start/stop, the scene-settings stream and `set_scene_settings`, `SceneManager.cpp` (D-7 on load), Project Settings > Time and Input rows | `SettingsRows.cpp`, `ProjectSettingsWindow.cpp` and `Stream.cpp` with WP-6 (separate rows); `Engine.cpp` (separate functions) | — |
| **WP-4 App lifecycle and HTML UI** | S28, S30 (REQ-15 to REQ-19), AC-16 to AC-20 | L | `SubSystem.h` (two virtuals), `Scene.cpp` `releaseGpu/restoreGpu`, `UIDocumentSystem*.cpp`, `RmlRenderer.*`, `HtmlUIContext.*`, `Engine.cpp` view functions and `applyPause`, `NativeEngine.cpp` (the calls only), `JasperEngineWeb.cpp` context callbacks, `editor/mcp/tools/PlayTools.cpp` (`send_input` app actions), `tools/android-emulator-test.sh`, `engine/tests/htmlui/TestViewLoss.cpp`, `test_app_lifecycle.py` | S93 on the UI files (§9): agree on order before starting. `Engine.cpp` (separate functions) | — |
| **WP-5 Data Assets** | S9 (REQ-20 to REQ-24), AC-21 | L | New `engine/core/script/DataAsset.*` and `binding/DataAssetClassesLua.cpp`, the `list` type in `ScriptProperty.cpp` and the property parser, the editor asset type (create menu, Inspector, `.meta`, export cook), `editor/mcp/tools/DataAssetTools.cpp`, `stubs/DataAsset.lua`, `docs/scripting.md` section, eval case | `ScriptProperty.cpp` (WP-5 only); `LuaBinding.cpp` registration list (one line); `budget.json` and `TOOLS.md` regenerated | — |
| **WP-6 String Tables** | S10 (REQ-25 to REQ-30), AC-22 | L | New `engine/core/localization/*`, `binding/LocalizationClassesLua.cpp`, `engine/core/htmlui/HtmlUILocalization.cpp`, the Text component key (component, stream, reflection, Inspector row), the String Tables window, `editor/mcp/tools/LocalizationTools.cpp`, `stubs/Localization.lua`, Project Settings > Localization row, eval case | `TitleArtLanguage.*` (read and one shared global; no rename); `SettingsRows.cpp` with WP-3; S93's HTML UI guide page (one section, last) | — |

Per package, before merging:
- `tools/check-changed`.
- The lints of its area (`check_raw_writes.py` for WP-5 and WP-6 writers, which use AtomicFile;
  `check_stream_keys.py` for new keys).
- `test_lua_api.py`.
- `tool_budget.py --check` after the tool docs are regenerated.
- Builds use `JOBS=6`, and the test suites are filtered to the package's tests (program rule).

## Deviations

### Integration

- **Cooked scene format.** `main` reads and writes 1.6 (S96); the landing order's ledger gives 1.7
  to S98, 1.8 to 1.11 to #3, #10, #149 and #163, and this spec 1.12 (WP-5: script list properties,
  their element types a tail after a Script component's scripts) and 1.13 (WP-3: the scene's Fixed
  Timestep, the last value of the settings record), contrary to REQ-37's "no version bump": the
  cooked format is positional, so a new value is a new minor (the YAML scene and project keys stay
  optional, with defaults, unversioned). The reader takes 1.5 to 1.13.
- **Compile builds read the settings as data (S98).** The scene's Fixed Timestep reaches a Compile
  build in its cooked scene (no generated `scene->setFixedTimestep` line), and Project Settings >
  Time and Input > Simulate Touch From Mouse in `game.json` (`cook::makeGameConfig`), which
  `player::runGame` applies before the first scene, as in the prebuilt player.
- **The LuaBridge workaround shrank once S92 landed.** WP-1 left `engine/libs/luabridge3/LuaBridge.h`
  untouched (S92 owns its patch) and hardened LuaBridge's class tables from `LuaBinding.cpp`
  (`hardenClassTables`). With S92's fix inside LuaBridge on `main`, its nil-operand wrapper of every
  binary metamethod is gone: it hid S92's overload sets (`LuaBindingBench.OverloadMisses` failed).
  The empty metatable on the class tables stays: without it `tostring(debug.getmetatable(Vector3()))`
  raises "Vector3 expected" (no crash since S92), and REQ-2 asks for a string (`LuaMath.WrongCallsRaise`).
- **AC-2 under ASan found a Debug-only abort.** The ASan unit build is Debug, and there
  `Vector2`/`Vector4` division by a zero scalar asserted (the sweep divides by 0 from Lua). Release
  already gave infinities, and `Vector3` never asserted, so the asserts are gone: every vector
  divides by zero as Unity does.

### WP-1 Lua API correctness

- **Lua compatibility (lead decision at the main merge).** Existing Lua forms keep working as
  they did; the new forms sit beside them. REQ-1's removal of the `(x, y, z, RotationOrder)`
  constructor is undone: it is registered again after `(w, x, y, z)`, so four numbers still pick
  the raw form, as on every release (the guide documents `Quaternion.euler` for an order). REQ-11's
  constructor change is undone: `Camera(scene)` (Lua and C++) starts in target mode as before, and
  the new read-write `useTarget` is the way to rotation mode (`cam.useTarget = false`, as the
  editor's GameObject > Camera). AC-12 is `LuaHierarchyTest.CameraUseTargetSwitchesToRotation`.
  REQ-3 (nil instead of an engine error and a raise) and REQ-6 (current world poses) stay: no call
  that worked before gives another result, except a world getter read before the transform pass,
  which now gives the pose `updateTransform()` would have given.
- **D-2's form.** Only the Lua bindings changed: they test `findComponent<Transform>` and return
  `nil` (getters) or raise (setters, `parent`/`setParent`). The C++ `GameObject` getters keep their
  behaviour for C++ callers. `localMatrix`, `modelMatrix` and `normalMatrix` follow the getters'
  rule too. `setPosition` is one function (`x, y [, z = 0]`) instead of two overloads.
- **REQ-2 beyond the spec's list.** The `Matrix3`/`Matrix4` accessors `row`, `column`, `get`, `set`,
  `setRow` and `setColumn` check their 0-based index and raise. A `RotationOrder` argument that is
  not one raises (in `Quaternion.euler`, `fromEulerAngles` and `getEulerAngles`). The class-table
  metatable fix and the nil-operand guard are the LuaBridge workaround above. AC-3's lint flagged
  `MathClassesLua.cpp:778` and `:803` on the base (the spec's numbers were 783 and 804).
- **REQ-4, paste.** Paste numbered nothing before; it now numbers among siblings, like Duplicate.
- **S27 not reproduced.** `ScriptLifecycleTest.TopLevelClassFieldsPerInstance` is green on the
  base: each instance runs its own chunk. The test stays as a guard.
- **AC-6's test file** is `test_scene_reentry.py` (CI), not `test_lifecycle.py`, which is outside
  the CI suite. It was green on the base, as expected.

### WP-3 Runtime defaults

- **D-9's API.** The platforms call a new `Engine::setStartScreenSize(width, height)` before the
  project starts (Windows, Linux and macOS windows, the web canvas, the Apple view) and the engine
  lays out from it; the players do not call `computeViewLayout` themselves. Android sets nothing,
  so `init` reads the preferred size there and `onCanvasChanged` comes when the view loads, before
  the first `onUpdate`.
- **The project step** is set through `Engine::setProjectUpdateTime(double)` (1/60 stays the exact
  default) and a scene's through `applySceneUpdateTime`. Play stop resets the step, the maximum
  delta and touch simulation to the engine defaults; the next Play start applies the project's.
- **Lua `scene.fixedTimestep`** (read-write) was added beside the Inspector row and
  `set_scene_settings fixed_timestep`.
- **The scene's Fixed Timestep row** has no step buttons and names its fallback (0 = the project's)
  to fit the Inspector.
- **Window states.** A maximized or fullscreen window's start size is not known before the window
  exists: `init` reads the preferred size there and `onCanvasChanged` follows. On macOS the start
  size is in points, so the aspect ratio is right and the retina pixel size arrives through
  `onCanvasChanged`.
- **Not compiled here:** the Windows, macOS and iOS edits of the start size (no build target for them
  on the build hosts); they were reviewed by reading only.
- **No undo for the new project settings:** Project Settings > Time and Input apply on OK/Apply and
  `set_project_settings` stays not undoable, as for the existing project settings; the per-scene
  Fixed Timestep is undoable.

### WP-5 Data Assets

- **Unknown values are reported** by `edit_data_asset get` (`unknown_values`) and listed in the
  Inspector, where they can be removed; the `check` tool does not report them (REQ-21).
- **Lists.** A list holds elements of one type; there are no lists of lists. In Lua an unset
  reference inside a list is `false`, so the array keeps its length.
- **The class index.** Exports write `je-dataassets.json` (class to assets) for
  `DataAsset.loadAll`; editor Play builds the index from the project.
- **Data classes leave Add Script**: a `---@dataasset` module is not offered as a component.
- **`AssetType::DataAsset = 39`** (after S98's Mesh 35, #149's 36 and #163's 37 and 38, the landing order's ledger).
- **References load with the asset.** References to other Data Assets load together with the asset
  that holds them (cycles handled), not lazily per field: a lazy proxy would break `pairs` and `#list`.
- **Stored type name** is `data_asset`; `assetType = "dataasset"` is accepted as an alias.
- **Lists are for Lua scripts and data classes only**, not C++ `DPROPERTY` fields.
- **Cooked format:** element types are written after the scripts, so content cooked at 1.5 to 1.11 still reads.

### WP-6 String Tables

- **Moved to `game-ui-localization.md` (user decision: one design).** S95's WP-6 and that spec
  built the same feature; their merged design is that spec's D-15, which takes from WP-6 the
  positional `{0}` arguments, a writable `Localization.language`, the stored names
  `localizationKey` and `localization.remember_language`, `setLocalized` on Text and HTML
  elements, edits applied at once, keyed text in Edit mode and the `check` tool `strings`, and
  drops the rest (the one-file format, `{form: text}` plurals, `data-loc`, `"Table:key"`, the
  cross-table duplicate check, the `.meta` GUID). This branch drops its WP-6 implementation
  (REQ-25 to REQ-30, AC-22) so that one lands; S10 closes with it. `AssetType::StringTable` is
  that spec's (37), and WP-5's `DataAsset` is 39.

### WP-2 Frame order and transforms

- **REQ-33's idle cost is two compares, not one.** The late pass skips a scene when neither the
  Transform array's nor the Camera array's newest change is past the tick its update ended at
  (camera settings set in `onPostUpdate` count too). With the Camera Follow check and the system
  lookup, `je-bench-late-transforms` measures 0.006 µs a scene, 0.14 µs with a follow camera
  whose target did not move late (1.2 µs when it did: the follower stepped again).
- **A main camera moved late leaves the MVPs to its draw.** Every object's MVP depends on the
  view, so the late pass sets `mainViewMVPStale`: the camera's pass makes them (the multi-camera
  path, `perCameraMVP`), or the next update when no draw comes. The late pass itself never walks
  the static Transforms; the bench's ratio (0.02 of the main pass) leaves that draw-time work out.
  "The main transform pass" in the bench is `RenderSystem::update` of the static scene.
- **What the late pass covers.** Propagation, sprite batches, skins (Model skin bindings and
  Bone components), lattice followers, cameras (view, projection, frustum, Pixel Perfect snap,
  mirrors, sky), bounds, MVPs of moved objects, instanced and points sorting, terrain cuts, lights
  and shadow cameras, Camera Follow, the 3D listener and the 3D sources' positions (their velocity
  stays the update's, so Doppler measures from frame to frame). It does not re-run UI layout: a
  RectTransform's anchors or size changed after the scenes' update lay out at the next frame.
- **Camera Follow in the late pass.** A follower whose target moved after the update steps again
  from where the frame's step started, with its delta, toward the target's pose now
  (`CameraControlSystem::updateLateFollowers`), so a damped follow ends the frame where it would
  had the target moved in `onUpdate`; a transform-mode Camera Shaker on it shakes around the new
  placement. `computeWorldNow` keeps its parent chain on the stack (no allocation a frame).
- **More changes are stamped (`Changed<Transform>`)**: a reparenting, the children a destroyed
  parent hands to its own parent, and the lattice followers. A system that sets `needUpdate`
  without a stamp after `RenderSystem` in the scene update is still drawn at the next frame.
- **REQ-34 needed a compensating change.** The name lookup alone cost +12% on 1 000 handlers;
  the guard's four keys are now upvalues pushed by value, and the dispatch is +3.0% (216.5 ns to
  223.0 ns a handler, median of 7, one box).
- **REQ-6 scope.** `getWorldPosition/Rotation/Scale` and `modelMatrix` are current; `normalMatrix`
  stays the last pass's. `get_runtime_entity` lists `worldPosition`, `worldRotation` and
  `worldScale` (the Transform's world fields are derived and were not listed before); there is no
  matrix value type in its listing, so `modelMatrix` is not added.
- **Hot reload** still drops a registration whose method is gone after the reload (with its
  warning), as before; a method assigned at runtime after that is not picked up.

### WP-4 (app lifecycle and HTML UI)

- **D-11, `Rml::ReleaseCompiledGeometry` is not called.** `RmlRenderer` keeps every compiled
  geometry's vertices on the CPU and uploads them again after `releaseGpuResources`, so the handles
  stay valid; releasing them would make RmlUi drop geometry it does not build again for unchanged
  elements. AC-16 shows the frame after the return equals the one before.
- **REQ-15, CPU-only systems.** `ActionSystem`, `AnimatorSystem` and `NavigationSystem` hold no GPU
  objects, and their `destroy` stopped actions and tweens, reset state machines and dropped baked
  surfaces: their `releaseGpu`/`restoreGpu` do nothing. Every other system keeps D-11's default
  (`destroy`/`load`), as before.
- **Scenes-only release (REQ-18).** `Engine::releaseScenesGpu`/`restoreScenesGpu` run the systems'
  release with the view marked lost, as the platforms do. RenderSystem keeps its shared default
  textures during such a release (they still serve the other scenes): one condition in
  `RenderSystem::destroy`.
- **REQ-18, the game while the view is lost.** `view_lost` pauses a running app first (a device
  loses the view only in the background) and `view_restored` resumes it; meanwhile the editor
  neither updates nor draws the running game, and a Game view screenshot fails naming
  `view_restored`. `get_play_state.platform.lifecycle` is `{view_lost, view_restored, view}`.
- **Web.** `webglcontextlost` now prevents its default (without it the context never comes back),
  no frame runs while the context is lost, and the restored context gets its extensions enabled
  again before `systemViewLoaded`.
- **AC-16.** The pixel check compares the frame after the return with the frame before it (within
  2 levels), not a stored golden. The scenes-only route and a texture-mode document have their own
  tests (`ViewLossTest.*`).
- **AC-17.** The device step is `tools/android-emulator-test.sh --view-loss` (Home,
  `am send-trim-memory <pkg> COMPLETE`, the launcher) with `--once 'SVG plugin initialised'`, on the
  new test project `tools/test-projects/app-lifecycle`. The web check stays manual (in Chrome,
  `WEBGL_lose_context` from the console, the steps in the project's `main.cpp`): the web gate's
  chrome-headless-shell stops answering after any WebGL context loss of the page, even with the
  engine's handler reduced to setting a flag (bisected), so no gate is committed.
- **AC-19.** The suite is `EngineFrameGpu` (it draws frames: GPU suites end in `Gpu`), with
  `ViewRestoreClampsDelta` beside `ResumeClampsDelta`.
- **REQ-16 held on main.** A rebuild (file change, `needReload`) already fired `onUnload` before
  `onLoad`; the only DOM rebuild without `onUnload` was the view loss REQ-15 removes.
  `UIDocumentReloadTest.ReloadFiresUnloadFirst` pins it (the reload paths it runs are unchanged here).
- **S28 not reproduced.** `test_app_lifecycle.py::test_play_cycle_no_outside_frame_error` passes on
  the editor built before this package and after it: three Play cycles with a document log no
  "outside a frame" error.
