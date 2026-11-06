# Scripting: the script surface humans and agents reach for works the way a Unity user expects (design)

Date 2026-11-04. Branch `dc/scripting`. Program: Doriax catch-up, ledger `docs/doriax-catchup/LEDGER.md` (#9).
Doriax references (read-only, `dc-work/analysis/doriax`): 9844a94a7, 86722b85d, 077bfa735, 3d7a91e4e, 04501b15b,
755972f92, cc550567a. Line numbers below were re-read on `main` (b4f7816ea); treat them as hints.

## 1. Goals

1. **Add Script works for Lua.** The Inspector's Add Script button attaches a real script (Lua or C++) chosen from
   the project's scripts, a new one, or a file; dropping a script file on the Inspector or on a GameObject in the
   Hierarchy attaches it. No empty, file-less entries are created any more.
2. **Math that a Unity user types works.** `AABB:getCorners()` / `OBB:getCorners()` return all 8 corners;
   `Vector2/3/4` get `moveTowards`, `lerp` (clamped, Unity's `Lerp`) and `lerpUnclamped`, in C++ and Lua.
3. **The Inspector shows the script's real properties.** A script that drops its whole `properties` table loses its
   fields; a scene opened after its scripts changed on disk shows the current fields; a live edit in the code
   editor reaches Lua entries under a custom Lua root.
4. **A Model's Mesh API reaches the parts it is made of.** On a multi-node model (geometry on child "part"
   entities) `setColor`, `setMaterial`, `castShadows`, `setMorphWeight` ... change what is drawn; the editor says
   where the geometry is; part mesh settings survive a model rebuild.
5. **Curated Lua bindings.** The subsystem methods a game script needs (14 of them), C++ boolean defaults honoured
   from Lua (and Unity's "forces wake the body"), and the field gaps with the most gameplay value, led by
   hierarchy access (`parent`, `setParent`, `childCount`, `getChild`, `getChildren`, `findChild`).
6. **C++ completion offers only what C++ has.** Lua-only statics (`Engine.deltatime`) and Lua-only methods are not
   offered after `Engine::`; `search_engine_api` says they are Lua only.

## 2. Non-goals

- Binding raw component classes to Lua (`TransformComponent`, `CameraComponent`... as Doriax 04501b15b did): writes
  would bypass `setTrackedMember` / `markChanged` and the dirty flags the object wrappers set (ECSClassesLua.cpp
  comment at the CanvasGroup block, `:772`). The wrappers stay the Lua surface; gaps are filled there.
- Removing the empty root `MeshComponent` of multi-node models (the second half of cc550567a): see 5.9.
- Terrain foliage from Lua (no foliage Lua exists; a separate feature), Bone bind-pose read-back, keyframe/tween
  track read-back, SpriteAnimation internals, Joint3D legacy-parameter read-back (Joint3D already exposes the
  modern fields): low gameplay value, listed in 5.10 with reasons.
- The rest of Unity's `Vector3` statics (`Angle`, `SignedAngle`, `ClampMagnitude`, `ProjectOnPlane`, `SmoothDamp`):
  a follow-up note in the ledger, not this feature.
- New MCP tools. Agents already have `attach_script`, `list_scripts`, `set_properties`, `eval_lua`; this feature
  changes their results only where noted (warnings, `cpp_callable`).

## 3. Users and stories

- *A Unity user* selects an enemy, clicks Add Script, types "pat", picks `Patrol (Lua)`: the script is attached with
  its properties, one undo step. They drag `Health.lua` from the Project window onto another GameObject in the
  Hierarchy: attached. They never see an empty "Unnamed Script [C++]" entry.
- *A designer* deletes the `properties` table from a script while the editor is closed, reopens the scene: the
  Inspector no longer shows the old fields.
- *A gameplay programmer* writes `model:setColor(1, 0, 0)` on a hit and `model:setMorphWeight("Blink", 1)` on a
  character imported from a multi-mesh glTF: the character tints and blinks (today: nothing visible changes).
- *A scripter* writes `local p = self.gameObject.parent`, `for _, c in ipairs(go:getChildren()) do`,
  `go:findChild("Arm/Hand")`, `pos = pos:moveTowards(target, speed * dt)`, `body:applyForceToCenter(f)` and gets
  Unity's behaviour (the last one wakes a sleeping body).
- *A C++ script author* types `Engine::` and is offered `getDeltatime()`, not `deltatime`.
- *An agent* calls `set_properties` on a Model root's `mesh` component and is told the geometry is on 3 parts (with
  their ids); it calls `search_engine_api query=deltatime` and sees `cpp_callable: false`; `attach_script` behaves
  exactly as today (it now shares the Inspector's code).

## 4. Findings (what is wrong or missing today)

F1 (9844a94a7, confirmed). Inspector Add Script adds an empty C++ entry with no Lua choice
(`editor/window/InspectorWindow.cpp:11383-11397`); the entry's Source File picker then rejects `.lua` for it
(`:11740-11745`); a Lua entry's edit popup says "Class Name" (`:11648-11649`) and normalises it to a C++ identifier
(`:11836-11840`) although a Lua entry's name is its file name (MCP `makeEntry`,
`editor/mcp/tools/ScriptTools.cpp:640-654`). A full Lua path exists only through Add Component > New Script
(`InspectorWindow.cpp:2283`, `:2297-2357`, `ScriptCreateDialog.cpp:420-437`). No script drop on the Inspector or
the Hierarchy exists (`HierarchyWindow.cpp:1931-2034` accepts scenes, prefabs, models, clips, physics materials).

F2 (86722b85d, 077bfa735, confirmed). `AABB:getCorners` / `OBB:getCorners` are bound to the C++
`const Vector3* getCorners()` (`engine/core/script/binding/MathClassesLua.cpp:865`, `:949`); LuaBridge pushes the
pointer as one object, so Lua gets the first corner only, pointing into the box's cache (`mCorners`) whose lifetime
is the box's. A null or infinite AABB hits `assert(mBoxType == BOXTYPE_FINITE)` (`engine/core/math/AABB.cpp:201`).
No other bound method returns a pointer to an array of values (checked over `lua-api.json`). `Vector3::moveTowards`
and `Vector3::lerp` exist (`Vector3.h:87,90`) but are unbound; `Vector2` and `Vector4` have neither. `lerp` does not
clamp `t` (Unity's `Lerp` does). Solved already: number-first `*`/`/` (`MathClassesLua.cpp:1051-1059`), `Vector3`
component-wise `__div` (`:535-539`).

F3 (3d7a91e4e, confirmed with corrections).
- `parseLuaPropertiesTable` clears `entry.properties` only inside `lua_istable` (`editor/util/ProjectUtils.cpp:1592`),
  so a script that drops its `properties` table keeps the old fields: the parsed copy
  (`editor/Project.cpp:9153`) still holds them and the merge (`:9161`, `mergeScriptProperties` `:238`) sees no
  change.
- The code editor's refresh matches Lua entries by comparing the Lua-root-relative `scriptEntry.path` with a
  project-relative path (`editor/window/CodeEditor.cpp:727-728`): with a Lua root other than `.`
  (`editor/Project.h:1485`) live edits and code-editor reloads skip Lua entries. The file watcher resolves correctly
  (`editor/reload/handlers/Scripts.cpp:33-35`, `entryPropertyFile`).
- No refresh at scene open: `updateAllScriptsProperties` (`Project.cpp:9107`) runs only when Play starts
  (`:9420`). A script edited while the editor was closed shows stale fields until Play.
- Every refresh runs the chunk in a new isolated Lua state per entity (`ProjectUtils.cpp:1522-1533`): N entities
  sharing one script run it N times.
- Hazard: the live refresh and the watcher refresh apply with `PropertyCmd::execute()` and no undo
  (`CodeEditor.cpp:735-737`, `Scripts.cpp:149-151`).

F4 (04501b15b, partial; curated here). Public subsystem methods with no Lua route: `ActionSystem::getDuration`,
`getFrameDuration` (`ActionSystem.h:355-356`); `AudioSystem::stopSceneSounds` (`AudioSystem.h:193`);
`MeshSystem::hasPendingAsyncModelLoads`, `isAsyncModelLoadPending(Entity, string)`, `cancelAsyncModelLoad`,
`cancelAsyncModelLoads` (`MeshSystem.h:350-357`); `PhysicsSystem::clearSubscriptions`, `removeSubscriptionsByTag`
(`PhysicsSystem.h:449,465`); `RenderSystem::isAllLoaded`, `getLoadCount` (`RenderSystem.h:2764-2765`);
`UISystem::parentToLayoutSpace`, `layoutToParentSpace`, `hasPendingImages`, `resetButtonStates`
(`UISystem.h:327-328,359,365`). Bindings at `ECSClassesLua.cpp:427-628`. Covered already: `createWall`,
`hasInstancedMesh`. Not applicable: `particleActionReset`.

F5 (04501b15b default bools; widened). LuaBridge reads a missing boolean as `false` (`Stack<bool>::get`,
`engine/libs/luabridge3/LuaBridge.h:3876-3879`), so a C++ default of `true` is lost from Lua:
`ActionSystem::actionStart(entity, newRun = true)` (`ActionSystem.h:268`; bound `ECSClassesLua.cpp:428`) and
`MeshSystem::loadGLTF(..., changeRootTransform = true, ...)` (`MeshSystem.h:268`; bound `:509`). These are the only
two direct bindings with a `= true` bool default (grep over the bound methods of `lua-api.json`). The same trap
without a C++ default: `Rigidbody2D::applyForce/applyForceToCenter/applyTorque/applyLinearImpulse/
applyLinearImpulseToCenter/applyAngularImpulse(..., bool wake)` (`Rigidbody2D.h:228` ff.; bound
`GameObjectClassesLua.cpp:4525-4530`): `body:applyForceToCenter(f)` passes `wake = false`, and Box2D ignores a force
on a sleeping body. Unity's `AddForce` always wakes. Stubs mark these trailing booleans optional
(`tools/lua_api/parser.py:3324-3328`), which reads as "has a default".

F6 (04501b15b field gaps, curated). No Lua route to a GameObject's parent or children (no `parent`, child count or
child access in `GameObjectClassesLua.cpp:1207-1228`; only `addChild`/`removeChild`/`removeParent`); C++
`GameObject` has none either (`GameObject.h:62-68`). Also missing: `Light` world direction (C++ computes
`LightComponent::worldDirection`, `RenderSystemShadows.cpp:625`, no getter; Camera has one, `Camera.cpp:349`),
`Mesh` vertex count (`MeshComponent::vertexCount`, `RenderSystem.cpp:6160`), `Model` file name and animation count,
`Rigidbody` allowed-DOF read-back and `overrideMassProperties` (`RigidbodyComponent.h:129,174`; only setters bound).

F7 (755972f92). Static properties bound through an accessor or a lambda (`Engine.deltatime` =
`&Engine::getDeltatime`, `CoreClassesLua.cpp:2205`; `Input.KEY_SPACE` = a lambda over the macro `D_KEY_SPACE`,
`:3424`) become kind `Constant` in the completion header (`tools/lua_api/suggestions.py:63-64`); the C++ code editor
hides only `Property` (`editor/window/widget/SemanticSuggestions.cpp:193`) and boosts `Constant` after `::`
(`:257`), so it offers `Engine::deltatime`, which does not compile. Same for Lua methods bound to lambdas with no C++
member of that name. `search_engine_api` (`editor/mcp/EngineApiContext.cpp:340-370`) marks C++-only methods
(`lua_callable: false`) but not the reverse.

F8 (cc550567a, confirmed, uncertain on reach). A glTF with more than one mesh node loads its geometry into child
entities (`MeshSystem.cpp:5242-5243`, `:5447-5495`, `ModelComponent::meshNodesMapping`); the root keeps an empty
`MeshComponent` treated as loaded (`RenderSystem.cpp:6163-6174`). Every `Mesh` wrapper method reads
`getComponent<MeshComponent>()` of its own entity (59 sites in `engine/core/object/Mesh.cpp`), and
`Model::setMorphWeight/getMorphWeight` write the root's `morphWeights` (`Model.cpp:429-466`), so on such a model the
documented calls (`docs/mcp/guide/lua-api.md:1704-1835`) change nothing drawn. Submesh-indexed methods have no bounds
check (`Mesh.cpp:184-187` `setMaterial(submesh)`, `getMaterial`, `setPrimitiveType`...). Morph names are one
model-wide table of per-mesh indices (`MeshSystem.cpp:6042`) while each part keeps its own `morphTargetNames`.
The Inspector and `set_properties` edit the root's empty mesh without a word. `ModelLoadCmd` carries morph weights
and submesh overrides across a rebuild (`editor/command/type/ModelLoadCmd.cpp:216-270`, `:472`, `:653`) but not the
parts' mesh-level settings (cast shadows, custom shader...).

## 5. Product decisions

5.1 **Add Script is Unity's script picker.** The button opens a searchable popup: the project's scripts (Lua modules
under the Lua root, C++ script classes found in the script directories), then "New Script..." and "Browse...". It
never adds an empty entry. Reason: Unity has no empty script slot; an empty entry only invites a broken state.
Add Component's search also lists project scripts (Unity's Add Component > Scripts). Dropping a `.lua`, `.h` or
`.cpp` on the Inspector or on a Hierarchy row attaches it (Unity drag-a-script-onto-a-GameObject).

5.2 **A Lua entry's name is its file name**, shown read-only as "Module" in the edit popup; picking another source
file renames it. Reason: the runtime and `attach_script` already treat it so; an edited name silently breaks
`getScript("Name")`. Existing entries whose name differs keep it (shown, not editable).

5.3 **One attach path for humans and agents.** `makeEntry`/`attachEntry` move from `ScriptTools.cpp` into
`editor/util/ScriptAttach.{h,cpp}`; `attach_script`, the Inspector, the Hierarchy drop and the New Script dialog all
call it. Reason: the same validation (Lua root, header/source pairs, class detection, duplicate refusal) and the
same undo shape everywhere.

5.4 **Unity's `Lerp` semantics in both languages.** `lerp` clamps `t` to [0, 1]; `lerpUnclamped` does not.
`moveTowards` never overshoots (negative delta moves away, as Unity). Engine callers that interpolate
(`PhysicsSystem.cpp:955`, `NavigationSystem.cpp:2406,2409,2644`) switch to `lerpUnclamped` so their results stay
bit-identical. Reason: Unity-first; C++ scripts port from C# too. `a:lerp(b, t)` and `Vector3.lerp(a, b, t)` are
the same Lua call, so both of Unity's spellings work.

5.5 **`getCorners()` returns copies**: a 1-based table of 8 `Vector3`, element `i` = `getCorner(i - 1)` (the
`CornerEnum` value + 1); an empty table for a null or infinite box. Reason: a pointer into the box's cache is one
corner and a lifetime hazard; an assert on a script-reachable call aborts a debug editor.

5.6 **A refresh that cannot be undone never deletes on a guess.** A *saved* script that runs and returns a table
without `properties` has no properties: its fields go (watcher, scene open, Play). A *live buffer* in the code
editor without a `properties` table keeps the fields until the file is saved (the user may be cutting and
pasting the table). Load errors, run errors and a non-table result keep the last fields everywhere (and report
the problem as today). `properties = {}` clears in both. Reason: the live path has no undo (F3 hazard).

5.7 **Scene open refreshes script properties** of every opened scene and prefab stage, through a per-pass cache (one
isolated Lua state per pass, each distinct file run once), and does not mark the scene modified (like the linked
material sync at `Project.cpp:4390-4422`). Reason: Unity shows current fields after a domain reload without
dirtying the scene; the changed values are written with the next save.

5.8 **A Model's Mesh API addresses the model, not its empty root.** When the entity has a `ModelComponent` whose
`meshNodesMapping` names live part entities (a "parts model"):
- mesh-level setters (cast/receive shadows, receive lights/IBL, static shadow caster, culling mode, winding order,
  LOD, transparency, custom shaders, reflection probes, shadow billboard, shader uniforms) write every part;
  getters read the first part (lowest glTF node index);
- unindexed submesh calls (`setTexture`, `setColor`, `setAlpha`, `material`, `setNormalTexture(path)`,
  `primitiveType`, `faceCulling`, `castShadowsWithTexture`) act on submesh 0 *of every part*; getters read the first
  part's submesh 0 (the same "submesh 0" rule a single mesh has, applied per part, which is what tints a whole
  character);
- indexed calls (`setMaterial(i, m)`, `getMaterial(i)`, `setNormalTexture(i, p)`...) number submeshes across the
  parts in node-index order (part A's submeshes, then part B's), `getNumSubmeshes()` is the sum, an index past
  the end logs an error and does nothing (getters return the default value);
- `getAABB()`/`getVerticesAABB()` merge the parts in the model's space, `getWorldAABB()` in world space;
  `vertexCount` sums; `load()` loads every part;
- morph weights by name go to every part that has a target of that name (per-part `morphTargetNames`), by index to
  every part with at least `index + 1` targets; getters return the first match;
- instancing (`createInstancedMesh`, `addInstance`...) and `setAsMirror` on a parts model log one error and do
  nothing: instance or mirror a part.
Every indexed `Mesh` method gets a bounds check, parts model or not. Reason: makes the documented API true for the
most common imported models; "first submesh of each part" matches the single-mesh rule users already know.

5.9 **The root keeps its empty MeshComponent** (Doriax's root removal is rejected for now). Reason: Jasper code
expects it (`CreateEntityCmd.cpp:794`, `ModelLoadCmd.cpp:584`, `RenderSystem.cpp:6164`, CpuSkin, picking,
serialization), and removing it needs a scene migration with risk out of proportion to the gain once the API
delegates. Instead the editor is honest: the root's Mesh section on a parts model shows "Geometry is on N parts"
with a **Select Parts** button (selects them, so the existing multi-selection Inspector edits them as one undo
step) and disables its mesh-level fields; `set_properties` on that `mesh` component succeeds as today but returns a
warning naming the parts. Part mesh-level settings survive a `ModelLoadCmd` rebuild, matched like morph weights.

5.10 **Curated bindings** (5.10 table in 6.4). Taken: the 14 subsystem methods of F4 plus `RenderSystem.getLoadCount`;
hierarchy access; `Light.worldDirection`; `Mesh.vertexCount`; `Model.filename`, `getAnimationCount()`;
`Rigidbody.getAllowedDOFs()`, `overrideMassProperties` (read-only). Rejected: raw component classes (2), Bone
`index/bindPosition/bindRotation/bindScale/offsetMatrix` (animation internals; the pose APIs cover the uses), tween
start/end read-back and track `values/inTangents/outTangents` (author in clips), SpriteAnimation `frameIndex/
framesSize` (the Animation Player replaces it), Camera `customViewMatrix/invertCulling` (render internals with
editor UI), InstancedMesh `numVisible`, LayoutGroup `numBoxes`, `Text.loaded`, `Points.loaded`, `Skybox.loaded`
(diagnostics, `RenderSystem.isAllLoaded` answers "loaded"), Terrain foliage (its own feature), Joint3D legacy
read-back (modern properties exist).

5.11 **Omitted booleans keep their C++ meaning.** Bindings whose C++ default is `true` take `std::optional<bool>`
and apply `value_or(true)`; `Rigidbody2D` force and impulse methods default `wake` to true (Unity). A test over
`lua-api.json` fails when a new direct binding has a `bool x = true` default (6.4.3). Stubs and docs say
"default true" for these.

5.12 **Lua-only symbols are marked once, by the API parser** (`luaOnly` in `lua-api.json` and the completion header):
a static property is C++-visible only when bound to a static data member of the same name or to
`constantCopy<T, T::NAME>` with that name; a method only when the C++ class (or a base) has a method of that name.
The C++ code editor skips `luaOnly` symbols; `search_engine_api` returns `cpp_callable: false` and a note naming the
C++ accessor when it can (`deltatime` -> `Engine::getDeltatime()`). Kinds stay as they are (no fixture churn).

## 6. Design

### 6.1 Editor: Add Script (F1)

**`editor/util/ScriptAttach.{h,cpp}`** (new, namespace `jasperengine::editor`):

```cpp
struct ScriptCandidate {            // one attachable script of the project
    ScriptType type;
    std::string name;               // Lua: file stem; C++: class name
    std::filesystem::path file;     // absolute .lua, or the .h of a C++ script
    std::filesystem::path source;   // C++: the .cpp beside it
    bool subclass = false;          // C++ class deriving an engine object class (not ScriptBase)
};
class ScriptAttach {
public:
    // makeEntry of ScriptTools.cpp:635, unchanged in behaviour (error text included)
    static bool makeEntry(Project*, const fs::path& file, const std::string& className, ScriptEntry& out,
                          std::vector<std::string>& warnings, std::string& error);
    // attachEntry of :708 for several entities: one MultiPropertyCmd (AddComponentCmd where the Script
    // component is missing, then the scripts PropertyCmd), setNoMerge, on the scene's CommandHandle.
    // Entities that already have the file are skipped and named in `skipped`; fails only if none is left.
    static bool attach(Project*, SceneProject*, const std::vector<Entity>&, const ScriptEntry&, long long index,
                       std::vector<Entity>& skipped, std::string& error);
    // Lua modules under getLuaPath(); headers under getScriptDirs() whose class ScriptParser finds
    // (findScriptClassName + inheritsScriptBase: true = script class, false = subclass); sorted by name
    static std::vector<ScriptCandidate> findProjectScripts(Project*);
};
```

`ScriptTools.cpp` `attachScriptTool` calls `ScriptAttach::makeEntry` + `attach` (one entity) and keeps its result
text and JSON. `openNewScriptDialog`'s callback (`InspectorWindow.cpp:2311-2356`) builds the entry and calls
`attach` (it gains the duplicate refusal it lacks).

**Inspector `drawScriptComponent`** (`InspectorWindow.cpp:11383-11397`): the button opens popup
`script_add_menu`:
- a search field (focused on open; fuzzy match as Add Component's `search::normalize`);
- rows `ICON_FA_FILE_CODE Name  [Lua]` / `[C++]`, tooltip = project-relative path; a row already attached to every
  selected entity is disabled ("Already attached"); a C++ subclass row whose base class the first selected entity
  is not is disabled with the reason; Enter / click attaches to all selected entities through `ScriptAttach::attach`,
  then `project->updateScriptProperties` fills the properties (already inside `attach`);
- separator, "New Script..." (opens `ScriptCreateDialog` in New mode, current behaviour of Add Component > New
  Script), "Browse..." (opens it in Existing Script mode: `ScriptCreateDialog::open` gains
  `bool startWithExisting = false`).
The candidate list is computed once when the popup opens (`findProjectScripts`), never per frame. In Play the
button is disabled with the tooltip "Stop Play to add scripts" (as `attach_script`, `allowedWhilePlaying = false`).
`UIProbe::item("inspector.addScript")`, `"inspector.addScript.row.<name>"` for the UI test.

**Add Component dialog** (`editor/window/dialog/ComponentAddDialog.{h,cpp}`): `open(...)` gains
`std::function<std::vector<ScriptCandidate>()> scripts` and `std::function<void(const ScriptCandidate&)> onScript`.
In the Scripts category and in search results, script rows follow the Script component row (`Row::Script`, label
`Name (Lua script)` / `(C++ script)`), fetched once per open.

**Drops.** Inspector: a drop target over the component list area (`BeginDragDropTargetCustom` on the window's content
rect after the components, and on the Script component header) accepting `resource_files` whose first path is a
script (`Util::isLuaFile || isHeaderFile || isSourceFile`): attaches to the selected entities. Hierarchy
(`HierarchyWindow.cpp`, after the physics material branch `:2017-2031`): a script file onto a GameObject row
attaches to that GameObject (one entity), focus stays. Both report failures with `Out::warning` (the
`ScriptAttach` error text), like the physics material drop.

**Edit Script popup** (`:11640-11660`): for `ScriptType::LUA` the first row is "Module" with the name read-only
(`ImGuiInputTextFlags_ReadOnly`, disabled text colour); choosing a new Source File sets `className` to the new
file's stem; the tooltip of the pen button is "Edit script details: source file" for Lua, unchanged for C++.
`Factory::toIdentifier` applies to C++ names only. An entry with neither path nor header (old scenes) shows
"No file: pick one or remove the entry" in warning colour under its header.

### 6.2 Engine: math (F2)

- `engine/core/math/Vector2.{h,cpp}`, `Vector4.{h,cpp}`: `moveTowards(const T& target, float maxDistanceDelta) const`
  (as `Vector3.cpp:103-109`), `lerp(const T& target, float t) const` (t clamped to [0,1]),
  `lerpUnclamped(const T& target, float t) const`. `Vector3`: `lerp` clamps, `lerpUnclamped` added. Engine callers
  of 5.4 switch to `lerpUnclamped`; `Vector3Test.cpp:141-143` keeps passing (t in [0,1]).
- `MathClassesLua.cpp`: bind `moveTowards`, `lerp`, `lerpUnclamped` on Vector2/3/4 (member pointers; no overloads,
  so no Emscripten ordering issue). `getCorners` for AABB and OBB becomes
  `[] (const AABB& self) -> std::vector<Vector3>`: empty when `!self.isFinite()`; else 8 values from
  `self.getCorner(static_cast<AABB::CornerEnum>(i))` for i in 0..7 (no use of the `mCorners` cache, no assert).
  OBB: empty when `isNull() || isInfinite()`. LuaBridge pushes `std::vector` as a 1-based table.
- C++ `AABB::getCorners()` keeps its contract (finite boxes; engine callers pass finite ones).

### 6.3 Editor: script property refresh (F3)

- `ProjectUtils::LuaPropertiesResult::Status` gains `NoPropertiesTable` (the chunk ran and returned a table without
  a `properties` field) and `NotATable`. `runLuaScriptProperties` (`ProjectUtils.cpp:1535-1557`) sets them; when the
  result is `NoPropertiesTable` it clears `entry.properties`. `parseLuaPropertiesTable` (`:1588`) is unchanged
  (clears inside the table branch).
- `loadLuaScriptProperties(entry, path)` (saved file) returns the status; `loadLuaScriptPropertiesFromString(entry,
  content, chunk)` (live buffer) restores the previous properties when the status is `NoPropertiesTable`
  (decision 5.6).
- `Project::scriptPropertyFile(const ScriptEntry&) const` (moved from `Scripts.cpp:33-41` `entryPropertyFile`,
  returns the absolute, `lexically_normal` Lua file or C++ header) and
  `static bool scriptEntryUsesFile(const Project&, const ScriptEntry&, const fs::path& absolute)` (property file
  or C++ source). `CodeEditor::updateScriptPropertiesForPath` (`CodeEditor.cpp:709-741`) uses
  `scriptEntryUsesFile(resolveFilepath(rel))`; `Project::updateScriptProperties`' in-memory comparison
  (`Project.cpp:9136`, `:9155`) compares `lexically_normal` absolute paths. `Scripts.cpp` uses the moved helper.
- `editor/util/ScriptPropertyCache.{h,cpp}` (new): for one refresh pass, owns one isolated Lua state
  (`ProjectChecks::newIsolatedLuaState`) and maps absolute file -> (status, parsed properties) for Lua, and header ->
  parsed DPROPERTY list for C++. `Project::updateScriptProperties(..., ScriptPropertyCache* cache = nullptr)`;
  `updateAllScriptsProperties(sceneId, cache)` creates one when none is given. Watcher refresh (`Scripts.cpp`
  `reloadScripts`), scene open and Play preparation each use one cache per pass. A test hook
  (`ScriptPropertyCache::runsForTest()`) counts chunk runs.
- Scene open: in `Project::loadScene` when `opened` (after the linked-material sync, `Project.cpp:4390-4422`), call
  `updateAllScriptsProperties(targetScene->id, &cache)` and restore `targetScene->isModified` to its value before
  the call (5.7). Prefab stages: the same call where a prefab stage is opened (the implementer finds the stage open
  path; `getScenesAndPrefabStages` lists them).
- Lua property errors at open are reported once per file through `Out::error` (as today's Play path).

### 6.4 Engine: Lua bindings

6.4.1 **Subsystems** (`ECSClassesLua.cpp:427-628`):

| Class | Added | Notes |
|---|---|---|
| ActionSystem | `getDuration(entity)` (`overload<Entity>`), `getFrameDuration(frame)`; `actionStart(entity [, newRun])` wrapper | newRun default true (5.11) |
| AudioSystem | `stopSceneSounds()` (instance: `scene:getAudioSystem():stopSceneSounds()`) | the scene's sounds only; `AudioSystem.stopAll` stays global |
| MeshSystem | `hasPendingAsyncModelLoads()`, `isAsyncModelLoadPending(entity, path)` (`overload<Entity, const std::string&>`), `cancelAsyncModelLoad(entity, path)`, `cancelAsyncModelLoads()`; `loadGLTF(entity, path [, async [, skipEntities [, changeRootTransform [, bakeSceneRoot]]]])` wrapper | changeRootTransform default true |
| PhysicsSystem | `clearSubscriptions()`, `removeSubscriptionsByTag(substring)` | doc: removes every script's physics callbacks of the scene, like `Engine.clearAllSubscriptions` |
| RenderSystem | `isAllLoaded()`, `getLoadCount()` -> `loaded, total` (two results, `lua_CFunction`-free lambda returning `std::tuple<size_t, size_t>`) | for loading screens |
| UISystem | `parentToLayoutSpace(entity, v2)`, `layoutToParentSpace(entity, v2)`, `hasPendingImages()`, `resetButtonStates()` | |

6.4.2 **Rigidbody2D** (`GameObjectClassesLua.cpp:4525-4530`): the six force/impulse methods become lambdas with a
trailing `std::optional<bool> wake` and `wake.value_or(true)`.

6.4.3 **Guard.** `tools/lua_api/extract.py` `_sig` writes `cpp.params[i].default` (the parser's `CppParam.default`,
`parser.py:877-906`) for direct bindings. `tools/lua_api/tests/test_lua_api.py::test_no_bool_true_default_lost`
fails for any method overload with a `cpp` signature whose bool parameter defaults to `true` (allowlist empty).
For `std::optional<bool>` lambdas the stubs already show `boolean?`; the docs say the default.

6.4.4 **Hierarchy** (C++ first, then bound in `GameObjectClassesLua.cpp` next to `addChild`):

```cpp
// GameObject.h, Unity's Transform hierarchy members
std::optional<GameObject> getParent() const;               // none for a root or a Transform-less object
void setParent(const EntityHandle* parent, bool worldPositionStays = true);   // nullptr: to the root
size_t getChildCount() const;                              // direct children with a Transform
std::optional<GameObject> getChild(size_t index) const;    // 0-based, Hierarchy order; none out of range
std::vector<GameObject> getChildren() const;               // direct children, Hierarchy order
std::optional<GameObject> findChild(const std::string& path) const;  // "Arm/Hand": names under this one
```

- `setParent` refreshes the child's and the new parent's world matrices first (the transform update the editor's
  `updateTransform()` performs) so a same-frame move is honoured, then
  `scene->addEntityChild(parent, entity, worldPositionStays)` (`EntityRegistry.cpp:932-980`: `changeTransform`
  keeps the world placement). Refused with `Log::error` and no change: a parent in another scene, the object itself
  or one of its descendants (`isParentOf`), a parent without a Transform.
- Children: the Transform array keeps a branch contiguous (`findBranchLastIndex`); direct children are the entries in
  `(index(this), findBranchLastIndex(this)]` whose `parent == entity`. `findChild` walks one name segment at a time
  with `EntityRegistry::findEntity(name, parent)` (`EntityRegistry.h:269`); an empty segment fails.
- Lua: `parent` (rw: read -> `GameObject` or nil; write = `setParent(p, true)`, nil to the root), `setParent(p|nil
  [, worldPositionStays])` (default true), `childCount` (ro), `getChild(i)`, `getChildren()` (table),
  `findChild(path)`. Pushed as `GameObject` (scripts wrap the class they need, as today).

6.4.5 **Read-backs**: `Light::getWorldDirection()` (returns `LightComponent::worldDirection`, as Camera's) -> Lua
`worldDirection` (ro); `Mesh::getVertexCount()` (parts summed, 5.8) -> `vertexCount` (ro); `Model::getFilename()`
-> `filename` (ro), `Model::getAnimationCount()`; `Rigidbody::getAllowedDOFs(bool& tx, ...)` -> Lua
`getAllowedDOFs()` returning six booleans in `setAllowedDOFs` order, `Rigidbody::isOverrideMassProperties()` ->
`overrideMassProperties` (ro).

### 6.5 Engine: Model parts (F8)

- `MeshSystem::getModelParts(Entity model) const` -> `std::vector<Entity>`: the `meshNodesMapping` entities that
  exist and have a `MeshComponent`, in node-index order (`std::map` order). Empty when the entity has no
  `ModelComponent` or the model has no parts.
- `Mesh` (private helpers, `Mesh.h`):
  `std::vector<MeshComponent*> targetMeshes()` (the parts, else its own; at most one `findComponent<ModelComponent>`
  signature test per call on non-model meshes), `MeshComponent& firstMesh() const`,
  `MeshComponent* submeshOwner(unsigned int& submesh) const` (model-wide index -> part and local index, or
  `nullptr`), and `void markMeshChanged(Entity)` (the `markChanged<MeshComponent>` each setter does today, on the
  part's entity). Every setter in `Mesh.cpp` is rewritten over these helpers per 5.8; every submesh access goes
  through one bounds-checked `Submesh* submeshAt(MeshComponent&, unsigned int)` (the single place mesh-memory's
  storage change touches).
- `getAABB/getVerticesAABB`: merge each part's box transformed by the part's local matrix relative to the model
  (parent chain up to the model entity; parts moved out of the model use `inverse(model world) * part world`);
  `getWorldAABB`: merge the parts' `worldAABB`.
- `Model::setMorphWeight/getMorphWeight` (name and index) per 5.8, over `MeshComponent::morphTargetNames`; the
  single-mesh path (no parts) is today's code. `morphWeightsSet` / `morphTargetCount` are maintained per part as
  `Model.cpp:452-460` does.
- Instancing and mirror methods on a parts model: `Log::error("... a model made of parts: call it on a part")`
  once per entity (a static set), no change.

### 6.6 Editor: Model parts honesty and reload (F8)

- Inspector Mesh section (`InspectorWindow.cpp`, where `MeshComponent` is drawn for an entity with
  `ModelComponent`): when `MeshSystem::getModelParts` is non-empty and the root mesh has `numSubmeshes == 0`, an info
  row "Geometry is on N parts" and a "Select Parts" button (`project->replaceSelectedEntities` with the parts, the
  usual selection undo); the mesh-level fields are drawn disabled with tooltip "Edit the parts".
- `set_properties` (`editor/mcp/tools/ComponentTools.cpp:446-505`): when a change targets `MeshComponent` of such an
  entity, add a warning "This model's geometry is on N parts (<entity refs>): set the mesh on them, or use the
  Model API" (text + `warnings`).
- `ModelLoadCmd`: `recordPartMeshSettings`/`restorePartMeshSettings`, beside `recordMorphWeights`
  (`ModelLoadCmd.cpp:216-270`), keyed by `NodeRef` (index and name, `findModelNode`), carrying the mesh-level fields
  of 5.8 (`castShadows`, `receiveShadows`, `staticShadowCaster`, `receiveLights`, `receiveIBL`, `cullingMode`,
  `windingOrder`, `lodEnabled`, `lodBias`, `transparent`, `autoTransparency`, `customShader`,
  `customDepthShader`, `renderInReflectionProbes`, `shadowsBillboard`, shader uniforms). Undo restores the old
  parts as today.

### 6.7 Tooling: Lua-only marking (F7)

- `parser.py` `property_member` (`:3409-3480`): set `m.cpp_visible` for static properties: true when the getter
  resolves as `data` (a static data member) whose name equals the property name, or is `constantCopy<T, T::NAME>`
  with `NAME` equal to it. `extract.py` writes `"luaOnly": true` on static properties without it, and on methods
  whose name is in neither the C++ class nor a C++ base (`c.cpp_class.methods`, walking bases), operators excluded.
- `suggestions.py` emits a fifth field `bool luaOnly` in `EngineAPISymbol` (default `false` in the struct so
  hand-written symbols compile).
- `CustomTextEditor::addEngineAPISuggestions` (`editor/window/widget/CustomTextEditor.cpp:400-440`): in C++ mode skip
  `sym.luaOnly`.
- `EngineApiContext` search results: `luaOnly` symbols get `"cpp_callable": false` and `"cpp_note"`: "Lua only. C++:
  <Class>::get<Name>() / is<Name>()" when such a method exists on the class, else "Lua only."

### 6.8 Export and platforms

All of this is platform-independent C++ (engine, editor) and Python (tooling); no graphics backend or OS code is
touched. LuaBridge lambdas with `std::optional` and `std::vector` returns are already used on every target
(`HtmlUIClassesLua.cpp:1353-1392`), including Emscripten. Exported games get the new bindings through the normal
engine build; the editor-only parts (Inspector, cache, completion) do not ship.

### 6.9 Docs

- `docs/mcp/guide/lua-api.md`: 3.1 (moveTowards/lerp/lerpUnclamped row; `lerp` clamps), 3.3 (getCorners table,
  empty for null/infinite), 2.9 subsystems (a short table of the 6.4.1 methods), 4.2 GameObject hierarchy rows,
  4.3/4.8 parts rule (one paragraph: what "every part", "first part", model-wide submesh index mean), 4.10
  `worldDirection`, 4.13 `wake` default true, 4.14 `getAllowedDOFs`, `overrideMassProperties`; 6 Pitfalls: "an
  omitted boolean is false unless documented"; ```lua run blocks for hierarchy, lerp, getCorners (they run in
  `test_lua_api_json.py`).
- `docs/mcp/guide/lua-scripting.md` / `editor-integration.md`: Add Script picker, drops, Module name.
- `docs/unity-glossary.md`: rows for `transform.parent` / `SetParent` / `childCount` / `GetChild` / `Find`,
  `Vector3.Lerp` / `LerpUnclamped` / `MoveTowards`, `Bounds` corners, Add Component > Scripts, drag a script onto a
  GameObject, `AddForce` wakes.
- `docs/mcp/guide/cpp-scripting.md`: the C++ hierarchy methods; completion shows C++ names only.
- `docs/scripting.md`: property refresh rules (5.6, 5.7).
- Agent guide (`McpInstructions.cpp`): no change (details live in the area files).

## 7. Behaviour rules and edge cases

- Add Script with several GameObjects selected: attaches where missing, skips (and names) those that have it; one
  undo step. A prefab instance that refuses the change: the `ScriptAttach` error, nothing attached.
- A Lua file outside the Lua root dropped or browsed: refused with the `makeEntry` message ("move the file there
  first"). A `.cpp` without a header beside it: refused (same text as `attach_script`).
- Popup search with no match: "No script matches" and the New/Browse rows stay.
- A script deleted between popup open and click: `makeEntry` fails, warning, popup stays.
- Live buffer that deletes one property (not the table): removed as today (5.6 keeps today's per-property rule).
- A script used by 200 entities refreshes with one chunk run per pass.
- `getCorners` on an AABB built from min > max: finite by type, corners returned as the box says.
- `lerp` with NaN `t`: clamped result is NaN-propagating as `std::clamp` gives (documented as undefined).
- `moveTowards` with zero distance returns the target.
- `setParent` to the current parent: no change (`setEntityChild` early return), but `worldPositionStays` is moot.
- `getChild` on a Transform-less object: nil; `childCount` 0.
- Parts model whose parts were all deleted by the user: `getModelParts` is empty, the API falls back to the root
  mesh (today's behaviour).
- A part moved under another GameObject (allowed, `ModelLoadCmd` "movedParts"): still a part.
- `setMorphWeight("X")` where no part has X: logs "non-existent morph weight" as today.
- Rigidbody2D `applyForce(f, p, false)` keeps today's explicit no-wake behaviour.
- `PhysicsSystem.clearSubscriptions` during a contact callback: as `FunctionSubscribe::clear` is today (runtime-core
  owns its safety).

## 8. Performance and cost

- Runtime: `Mesh` setters on non-model meshes pay one component-signature test (`findComponent<ModelComponent>`)
  more per call; nothing per frame. Parts models pay a loop over parts per call (typically 2-10).
- `getCorners` allocates a table and 8 `Vector3` userdata per call: documented "cache it in a hot loop".
- `getChildren` is O(branch size); `findChild` O(segments x children).
- New bindings add a few hundred bytes to the Lua registration; no per-frame work.
- Editor: scene open runs each distinct script file once (one isolated state per pass) instead of zero today and N
  per Play today; Play preparation becomes cheaper with the cache. The Add Script candidate list scans the Lua root
  and script directories once per popup open (a few ms for hundreds of files), never per frame.
- Completion: one more bool per symbol in a static table.

## 9. Compatibility

- No file format change, no version bump, no migration. `ScriptEntry` and `ModelComponent` are unchanged.
- Old scenes with empty C++ entries load and show the "No file" hint; Lua entries whose name differs from the file
  stem keep their name.
- Behaviour changes (documented in lua-api.md and the ledger): `Vector3::lerp` clamps in C++ (C++ scripts that
  extrapolated use `lerpUnclamped`); from Lua an omitted `newRun`, `changeRootTransform` and `wake` now mean true;
  Mesh API calls on parts models now change the parts (they did nothing).
- Opening a scene no longer leaves stale script fields; the scene is not marked modified by that refresh.

## 10. Tests

Red controls are marked **R** (fail on `main`, pass with the change); the rest guard the new behaviour.

Core (`jasperengine-core-tests`, `tools/build.sh unit-tests`):

| Test | Asserts |
|---|---|
| `engine/tests/core/math/Vector3Test.cpp` `Vector3.LerpClampsAndUnclampedExtrapolates` **R** | `a.lerp(b, 2) == b`, `a.lerp(b, -1) == a`, `a.lerpUnclamped(b, 2) == 2b - a` |
| `Vector2Test.cpp` / `Vector4Test.cpp` `MoveTowardsAndLerp` | no overshoot, exact target at reach, zero length, negative delta moves away, clamp |
| `engine/tests/core/script/LuaMathBindingTest.cpp` (new) `LuaMath.AabbGetCornersReturnsEight` **R** | `#c == 8`, `c[i] == box:getCorner(i - 1)`; the table survives `box = nil; collectgarbage()` |
| `LuaMath.BoxGetCornersEmptyWhenNullOrInfinite` **R** | `#AABB():getCorners() == 0`, infinite AABB and null OBB likewise, no abort |
| `LuaMath.ObbGetCornersReturnsEight` **R** | 8 corners equal `getCorner` |
| `LuaMath.VectorMoveTowardsAndLerp` **R** | Vector2/3/4 member and `Vector3.lerp(a, b, t)` forms; clamp; `lerpUnclamped` |
| `engine/tests/core/script/LuaHierarchyTest.cpp` (new) `LuaHierarchy.ParentChildrenFind` **R** | root `parent == nil`; `childCount`, `getChild(0/1)` in Hierarchy order, `getChild(5) == nil`, `getChildren()` length, `findChild("A/B")`, `findChild("A//B") == nil` |
| `LuaHierarchy.SetParentKeepsWorldOrLocal` | `setParent(p)` keeps world position (moved parent), `setParent(p, false)` keeps local; `parent = nil` to root |
| `LuaHierarchy.SetParentRefusesCycleAndOtherScene` | parent to own child refused (unchanged), other scene refused |
| `engine/tests/core/script/LuaSubsystemBindingTest.cpp` (new) `LuaSubsystems.NewMethodsCallable` **R** | `getDuration` of a 2 s tween == 2; `isAllLoaded()` true and `getLoadCount()` returns two integers on an empty scene; `hasPendingAsyncModelLoads()` false; `parentToLayoutSpace(layoutToParentSpace(p)) == p` on an anchored element; `removeSubscriptionsByTag` removes a tagged `beginContact2D` Lua subscription (2D build) |
| `LuaSubsystems.ActionStartOmittedNewRunIsTrue` **R** | a stopped Animation whose ActionFrame has `started = true`: Lua `actionStart(e)` resets `started` to false, as C++ `actionStart(e)` |

Editor (`jasperengine-editor-tests`):

| Test | Asserts |
|---|---|
| `editor/tests/engine/subsystem/LuaDefaultArgumentsTest.cpp` (new) `LuaDefaults.LoadGltfKeepsChangeRootTransform` **R** | Lua `loadGLTF(e, path)` gives the root the same transform as C++ `loadGLTF(e, path)` on a glTF whose scene root has a transform (fixture built as `GltfMeshOffsetTest.cpp:56`) |
| `LuaDefaults.Rigidbody2DForceWakesByDefault` **R** | a sleeping dynamic body; Lua `applyForceToCenter(Vector2(0, 100))` -> awake and moves after a step; `applyForceToCenter(f, false)` leaves it asleep |
| `editor/tests/engine/object/ModelPartsTest.cpp` (new; two-node glTF written by the test: part A 2 submeshes, morphs `Smile`,`Blink`; part B 1 submesh, morph `Blink`) `ModelParts.MeshLevelSettersReachEveryPart` **R** | `setCastShadows(false)`, `customShader` on the model -> both parts; getter = part A |
| `ModelParts.UnindexedSubmeshCallsActOnEachPartsFirstSubmesh` **R** | `setColor` -> A[0] and B[0], A[1] unchanged |
| `ModelParts.SubmeshIndexSpansParts` **R** | `getNumSubmeshes() == 3`; `setMaterial(2, m)` -> B[0]; `setMaterial(3, m)` logs and changes nothing |
| `ModelParts.MorphWeightsResolvePerPart` **R** | `setMorphWeight("Blink", 1)` -> both parts' own Blink index; `"Smile"` -> A only; `getMorphWeight("Blink") == 1` |
| `ModelParts.BoundsAndVertexCountMergeParts` **R** | `getAABB()` contains both parts' boxes in model space; `vertexCount` = sum |
| `ModelParts.SingleMeshModelUnchanged` | a one-node model: same results as on `main` (control) |
| `ModelParts.InstancingRefusedOnPartsModel` | `createInstancedMesh()` logs once, adds no component |
| `editor/tests/command/ModelLoadPartSettingsTest.cpp` (new) `ModelLoadCmd.PartMeshSettingsSurviveRebuild` **R** | part B `castShadows = false`, `customShader` set; a rebuilding `ModelLoadCmd` (changed node hierarchy) -> kept on the new B; undo -> old parts back |
| `editor/tests/command/ScriptAttachTest.cpp` (new) `ScriptAttach.*` | Lua entry (path Lua-root-relative, name = stem); C++ pair from `.h` or `.cpp`; outside Lua root refused; two entities one undo step; already attached skipped and named; `findProjectScripts` lists a Lua module and a ScriptBase class, flags a subclass |
| `editor/tests/project/ScriptPropertiesRefreshTest.cpp` (new) `ScriptProperties.SavedFileWithoutTableClears` **R** | entry with a property; file rewritten without `properties`; `updateScriptProperties` -> empty, change reported |
| `ScriptProperties.LiveBufferWithoutTableKeeps` | the same content in memory -> properties kept |
| `ScriptProperties.ErrorsKeepLastProperties` | syntax error / runtime error / `return 5` -> kept |
| `ScriptProperties.CustomLuaRootMatchesLiveEdit` **R** | Lua root `lua`, entry `enemy.lua`; `scriptEntryUsesFile(<proj>/lua/enemy.lua)` true; `CodeEditor::updateScriptPropertiesForPath("lua/enemy.lua", buffer)` updates the entry |
| `ScriptProperties.SceneOpenRefreshes` **R** | scene saved with old fields, file changed, `loadScene(opened)` -> new fields, `isModified` false |
| `ScriptProperties.CacheRunsEachFileOnce` | 50 entities sharing one script: `runsForTest() == 1` per pass |
| `editor/tests/window/widget/CustomTextEditorTest.cpp` `CustomTextEditorCompletion.CppHidesLuaOnlyMembers` **R** | C++ mode, after `Engine::`: no `deltatime`, has `getDeltatime`; Lua mode after `Engine.`: has `deltatime` |

Tooling (`tools/lua_api/tests`, pytest in the MCP image):

| Test | Asserts |
|---|---|
| `test_lua_only_marked` **R** | `Engine.deltatime`, `Input.KEY_SPACE` luaOnly; `Vector3.ZERO` not; a lambda-only method (e.g. `GameObject.getParticleSystemsInChildren`) luaOnly; header has the field |
| `test_no_bool_true_default_lost` **R** | no direct binding with a `bool = true` default (fails on `main` listing `actionStart`, `loadGLTF`) |
| `test_suggestions_keep_what_the_regex_parser_knew` | unchanged and green (kinds kept) |

MCP (`tools/mcp/tests`, `tools/build.sh mcp-tests`):

| Test | Asserts |
|---|---|
| `test_inspector_add_script.py` (new, `ui_probe`, harness Xvfb) **R** | click `inspector.addScript`, type "ene", click row `Enemy`; `list_scripts` shows a Lua entry `enemy.lua` with its properties; `undo` removes it; Edit popup shows Module read-only. On `main` the click adds a C++ entry with no path (fails) |
| `test_inspector_add_script.py::test_drop_on_hierarchy` **R** | drag `enemy.lua` from the Project window row probe to a Hierarchy row: attached to that GameObject |
| `test_script_properties_refresh.py` (new) **R** | attach a script with `speed`; rewrite the file without `properties`; next call's `external_changes` lists `removed: ["speed"]`; `list_scripts` shows none. Close the scene, add a property on disk, `open_scene`: `list_scripts` shows it, scene not dirty |
| `test_model_parts.py` (new) **R** | a two-node model: `set_properties mesh.cast_shadows` on the root returns a warning naming both parts; Play, `eval_lua` `Model(scene, root):setColor(1,0,0,1)`, `get_entity` of each part shows submesh 0 tinted |
| `test_docs.py::test_search_engine_api` (extended) **R** | `query=deltatime` -> the Engine static has `cpp_callable: false` and a note naming `Engine::getDeltatime()` |
| `test_lua_api_json.py` | the new lua-api.md run blocks (hierarchy, lerp, getCorners) run in Play |
| `test_scripts*.py` (existing attach_script tests) | unchanged and green after the move to `ScriptAttach` |

## 11. Acceptance criteria

1. The Inspector's Add Script lists the project's Lua and C++ scripts with search, attaches the chosen one to every
   selected GameObject in one undo step, offers New Script... and Browse..., and never creates an empty entry.
2. Dropping a script file on the Inspector or on a Hierarchy GameObject attaches it; invalid files give the
   `attach_script` message.
3. A Lua entry's edit popup shows "Module" read-only; changing its source file renames it.
4. `attach_script` results are unchanged (its existing MCP tests pass) and it shares `ScriptAttach` with the editor.
5. `AABB:getCorners()` / `OBB:getCorners()` return 8 copies in `CornerEnum` order, an empty table for null/infinite
   boxes, and never assert.
6. `Vector2/3/4` have `moveTowards`, `lerp` (clamped) and `lerpUnclamped` in C++ and Lua; physics and navigation
   interpolation results are unchanged (their tests pass).
7. A saved script without a `properties` table has no properties in the Inspector after the watcher refresh, at
   scene open and at Play; a live buffer without the table keeps them until saved.
8. Live edits refresh Lua entries under a custom Lua root.
9. Opening a scene refreshes script properties without marking it modified; each distinct script runs once per pass.
10. On a multi-node model, the Mesh/Model API calls of 5.8 change the parts as specified; indexed calls are bounds
    checked on every mesh; single-mesh models behave as before.
11. The Inspector shows "Geometry is on N parts" with Select Parts on a parts model root; `set_properties` warns;
    part mesh settings survive a model rebuild and undo.
12. The 6.4.1 subsystem methods, the 6.4.4 hierarchy members and the 6.4.5 read-backs exist in Lua (and C++ where
    listed), with stubs regenerated by the build and no `-- unresolved:` for them.
13. From Lua an omitted `newRun`, `changeRootTransform` or Rigidbody2D `wake` behaves as true; the tooling test
    fails on any new direct binding with a `bool = true` default.
14. C++ completion offers no `luaOnly` symbol; `search_engine_api` marks them `cpp_callable: false`.
15. Every **R** test of section 10 was run red on `main` (or on the branch with the fix reverted) and green with it;
    the report names the runs. `tools/check-changed` passes; `gen_tool_docs.py --check` and
    `tool_budget.py --check` pass (no schema change expected).
16. lua-api.md, lua-scripting.md, editor-integration.md, cpp-scripting.md, scripting.md and unity-glossary.md are
    updated; the doc tests pass.

## 12. Files likely touched

Engine: `engine/core/math/Vector2.{h,cpp}`, `Vector3.{h,cpp}`, `Vector4.{h,cpp}`;
`engine/core/object/Mesh.{h,cpp}` (hot), `Model.{h,cpp}`, `GameObject.{h,cpp}`, `Light.{h,cpp}`,
`physics/Rigidbody.{h,cpp}`; `engine/core/subsystem/MeshSystem.{h,cpp}` (`getModelParts` only),
`PhysicsSystem.cpp`, `NavigationSystem.cpp` (`lerpUnclamped` call sites);
`engine/core/script/binding/MathClassesLua.cpp`, `ECSClassesLua.cpp` (hot), `GameObjectClassesLua.cpp` (hot).
Editor: `editor/window/InspectorWindow.{h,cpp}` (hot), `editor/window/HierarchyWindow.cpp` (hot),
`editor/window/dialog/ComponentAddDialog.{h,cpp}`, `editor/window/dialog/ScriptCreateDialog.{h,cpp}`,
`editor/window/CodeEditor.cpp`, `editor/window/widget/CustomTextEditor.cpp`, `editor/util/ProjectUtils.{h,cpp}`,
`editor/util/ScriptAttach.{h,cpp}` (new), `editor/util/ScriptPropertyCache.{h,cpp}` (new), `editor/Project.{h,cpp}`
(hot: `loadScene`, `updateScriptProperties`), `editor/reload/handlers/Scripts.cpp`,
`editor/command/type/ModelLoadCmd.{h,cpp}`, `editor/mcp/tools/ScriptTools.cpp`, `editor/mcp/tools/ComponentTools.cpp`,
`editor/mcp/EngineApiContext.cpp`, `editor/CMakeLists.txt` (new sources).
Tooling: `tools/lua_api/parser.py`, `extract.py`, `suggestions.py`, `tests/test_lua_api.py`.
Tests: the files of section 10. Docs: the files of 6.9.

## 13. Cross-feature dependencies

- `mesh-memory`: rewrites `MeshComponent` submesh storage and touches `Mesh.cpp`, `MeshSystem.cpp`, `ModelLoadCmd`.
  This spec funnels every submesh access in `Mesh.cpp` through `submeshAt` so their change lands in one place;
  whichever merges second rebases onto it. Bounds checks here use `numSubmeshes`, valid for both layouts.
- `runtime-core`: fixes `PhysicsSystem::clearSubscriptions` / `removeSubscriptionsByTag` to include
  `onControllerColliderHit`; the Lua bindings here inherit the fix (no ordering constraint; the Lua test covers a 2D
  event). Their "hidden child kept hidden" change in EntityCloner/ObjectManager does not touch `setEntityChild`.
- `scene-integrity`: touches `InspectorWindow.cpp` (entity-reference drops in script properties),
  `HierarchyWindow.cpp` (drop targets), `CreateEntityCmd`/`ModelLoadCmd` (parent-before-child order on import).
  Merge hotspots; the script drop branch here is self-contained after their branches. Their `ModelLoadCmd` order fix
  and the part-settings carry-over here are independent functions.
- `project-window`: Create > Lua Script / C++ Script in the right-clicked folder may change `ScriptCreateDialog`;
  this spec only adds `startWithExisting` to `open`.
- `code-editor-git`: large `CodeEditor.cpp` changes; this spec changes `updateScriptPropertiesForPath` only.
- `ui-components`: adds Lua for Selectable navigation in `GameObjectClassesLua.cpp` / `ECSClassesLua.cpp`
  (`UISystem` block): binding-file merge hotspots; `resetButtonStates` bound here is the same C++ method their stick
  hand-back work may change.
- `sprite-batching`: `Mesh` setters on Sprites will need to mark a batch dirty; the `targetMeshes`/`markMeshChanged`
  helpers here are the one place to hook.
- `render-correctness`: "mesh bounds from real vertices" changes `MeshComponent::aabb`; the parts AABB merge here
  consumes it (tests compare against the parts' own boxes, so either order passes).
- `agent-authoring`: "MCP edits held during a human save" may wrap `set_properties`; the parts warning here is added
  after the command runs and composes with it.
- `unicode-text`: `ScriptAttach` stores paths with `generic_string` / `FileUtils::pathToUtf8` as `makeEntry` does
  today; their forward-slash rule applies unchanged.
- No dependency on: `export-pipeline`, `build-toolchain`, `mobile-shipping`, `draw-perf`, `ssr-probes`,
  `water-reflections`, `water-boats`, `platform-lifecycle`.

## Deviations

Recorded while implementing on `dc/scripting` (2026-11-04).

1. **`LuaPropertiesResult` gains flags, not statuses.** `returnedTable` and `noPropertiesField` instead of
   `NoPropertiesTable` / `NotATable` status values (6.3): `ProjectChecks::checkLuaScript` (and so MCP
   `check_script`) reports every non-`Ok` status as an error, and a script without properties is not one.
2. **One isolated Lua state per file, not per pass** (6.3): each distinct file still runs once per pass, but in a
   state of its own, so one script's top-level globals never leak into the next one's read.
3. **Editor test files.** The project-backed tests of 10 share one headless project in
   `editor/tests/project/ScriptingEditorTest.cpp` (`ScriptingEditor.*`: the property refresh, `ScriptAttach`,
   and ModelLoadCmd's part settings) instead of `ScriptPropertiesRefreshTest.cpp`, `ScriptAttachTest.cpp` and
   `ModelLoadPartSettingsTest.cpp`; the two-part glTF writer is shared in `editor/tests/support/TwoPartGltf.h`.
4. **The bool-default guard covers single bindings only** (6.4.3): an overload set picks its overload by the
   argument count, so its full-arity form always gets every argument (Lattice `addFollower` is such a set).
   On `main` the guard finds exactly `ActionSystem.actionStart(newRun)` and `MeshSystem.loadGLTF(changeRootTransform)`.
5. **Stubs mark trailing `std::optional` parameters optional** (`parser.py` `to_lua_sig`): the wrappers of 6.4
   show `newRun?`, `wake?`, `worldPositionStays?` with their documented defaults, instead of `boolean|nil`.
6. **`search_engine_api` description unchanged** (lean MCP rules, `fe3e90b97`): the results carry
   `cpp_callable: false` and `cpp_note` (`structuredContent`), no new tool or guide text, so no eval case.
7. **Add Component's script rows** come through `ComponentAddDialog::setScripts(scripts, onScript)` after
   `open()` rather than two more `open()` parameters (its other callers stay as they are).
8. **Drops and picks report through `ScriptAttach::attachFile`** (Console warnings: outside the Lua directory,
   no header beside a `.cpp`, already attached), shared by the Inspector and the Hierarchy.
9. **The root Mesh section of a parts model** draws the note and Select Parts, then the usual fields disabled
   (`drawMeshComponentFields` inside `BeginDisabled`).
10. **The Module row is not UI-tested.** `test_inspector_add_script.py` clicks Add Script, picks a Lua script and
    drops one on the Hierarchy; the edit popup's read-only Module row (5.2) did not open under xdotool in the
    harness (the pen button's click registered no popup), so that row is verified by reading only.
11. **Main's mesh LOD and GPU-instancing settings** (merged after the spec) follow the 5.8 rule: they write
    every part of a model made of parts and survive a ModelLoadCmd rebuild with the other part settings.
