# Scene integrity: one file per scene, references that mean what they say, Play views that follow the game (design)

Date 2026-11-04. Branch `dc/scene-integrity`. Program: Doriax catch-up, ledger `docs/doriax-catchup/LEDGER.md`
(feature 5). Dossier: `dc-work/dossiers/scene-integrity.json` (21 items) and the `scene-integrity` section of
`EXTRA-FINDINGS.md`. Every finding below is from reading `main` at `b4f7816ea`; nothing was built or run for this
spec. Line numbers are hints.

## 1. Goals

1. **One file per scene, one scene per file, one id per scene file.** No save path (Save dialog, Save All, MCP
   `save_scene`, the core save) writes a scene over the file of another scene of the project; `project.yaml` never
   lists a file twice and a duplicate entry loads once; a copied `.scene` (Project window, `move_asset copy`,
   Duplicate of the `project-window` feature, a copied folder) gets its own scene id and its own scene name; an id
   clash that still reaches the loader (a file copied outside the editor, a merge) is resolved the same way on every
   load and is saved away.
2. **Scenes stay inside the project.** The Save dialog refuses names that leave the chosen folder; the core save
   refuses a new path outside the project; opening a scene file from outside the project is refused with what to do.
   A scene an old project lists outside keeps loading and saving in place, with a warning.
3. **A GameObject id is used only in the scene it belongs to.** Every drop target of the `entity` drag payload
   checks the payload's scene; same-scene targets refuse a child scene's GameObject; cross-scene references
   (Inspector GameObject reference fields, the picker, drops into a script file) are offered and linked across the
   scenes that load together (the scene, its ancestors and their descendants), and nowhere else.
4. **The tracked GameObject list stays in parent-before-child order** after model import, create and duplicate, and
   the generated scene code creates parents first whatever the list says.
5. **Play never crashes or lies in the editor's other views.** A stopped scene shown beside a running game never
   updates a cameraless inline child; when the game loads another scene, the Scene view shows the loaded scene from
   the editor camera, with picking; a game that captures the cursor hands it back to the editor on Esc (Unity).
6. **Saving tells the truth.** Save All saves never-saved scenes too (asking for each file), save dialogs report
   Save/Cancel/failure to whoever waits, a dialog's closures are released when it ends, a project switch closes the
   old project's dialogs, every scene save failure alerts once with the reason, Ctrl+S during Play says why nothing
   happened.
7. **Edits on closed scenes are refused, not lost** (`set_child_scene` on a closed parent); `move_entity` refuses
   the one move that would report success and change nothing; multi-delete undo puts GameObjects without a Transform
   back where they were.

## 2. Non-goals

- Doriax's Detach Camera (F8, e5d7aad14): Jasper's Scene view already draws the running game from the editor
  camera with tools and picking (`editor/App.cpp:4136-4142`, `editor/render/SceneRender.cpp:721-722`). Only its
  cursor hand-back is taken (goal 5).
- A stand-in camera for a cameraless 3D scene loaded during Play (f8be9029f's main part): rejected, see decision 12.
- A Save As menu for titled scenes in the editor UI (MCP has one: `save_scene path=` on a saved scene). Not in the
  mandate.
- Remapping references between scenes copied together (a copied folder's parent scene keeps pointing at the
  original child scene): rejected, decision 4.
- Scene names following file renames: out of scope (only copies are renamed, decision 5).
- Doriax's symmetric `move_entity` refusal (2eea20daa): refuted for Jasper (decision 15).
- Doriax 7d06780c4's generator chain sort as the only fix: superseded upstream by 42aafbbb3; we keep the list sorted
  and sort a copy in the generator as a second line (decision 10).

## 3. Users and stories

- Human: has scenes `Level1` and `Level2` open, presses Ctrl+S on the untitled `Level2`, types `Level1` in the Save
  Scene dialog. Today the dialog warns "will be replaced", Save overwrites `Level1.scene` and both scenes now save
  into one file. After: the name field turns red with "Level1.scene is the file of scene "Level1": choose another
  name", Save is disabled.
- Human: duplicates `Level1.scene` in the Project window to start `Level1 1.scene`. Today the copy has `Level1`'s
  scene id and name; whichever loads second gets a random id on every load, so a child-scene reference to it breaks
  on reopen, and two scenes are named `Level1` (scene names must be unique, they name the generated code). After:
  the copy is scene "Level1 1" with its own id; undo/redo of the copy gives the same id again.
- Human: drags a GameObject shown under an inline child scene in the Hierarchy onto a GameObject of the parent scene.
  Today the parent scene's GameObject with the same number is reparented (ids start at the same value in every
  scene). After: the drop is refused with the tooltip "GameObject belongs to child scene "HUD"".
- Human: a script on a GameObject of child scene `HUD` needs the player of parent scene `Level`. Today the Inspector
  picker lists only `HUD` and its children. After: it lists `Level`, its other child scenes and theirs, by scene.
- Human: imports a skinned character (glTF) and exports the game in the same session. Today the generated scene
  code creates the bones before their parents, so a moving character smears in the export. After: parents first.
- Human: plays the menu scene; a button loads `Level`. Today the Scene view keeps showing the menu (retired) through
  the editor camera with `Level` squeezed into the game rectangle through the game camera. After: the Scene view
  shows `Level` from the editor camera; clicking in it selects `Level`'s GameObjects.
- Human: plays a mouse-look game, wants to inspect something in the Scene view. Today the cursor stays captured until
  Pause. After: Esc releases it (the game still gets the Escape key), clicking the Game view captures it again.
- Human: a project's first scene was never saved; Save All saves nothing and the scene is gone on reopen. After:
  Save All asks where to save it; when it is the first saved scene, it becomes the start scene.
- Agent: `set_child_scene scene=Closed child=HUD` returns ok today and the attachment is lost when `Closed` opens.
  After: `"Scene "Closed" is not open: open_scene it first (child scenes are edited on an open scene)"`.
- Agent: `move_entity entity=Player after=Skybox` returns "Moved" today and nothing moves. After: refused with the
  rule and the alternatives.
- Agent: `move_asset path=a.scene to=b.scene copy=true` reports the copy's scene id; `list_scenes` never shows two
  scenes with one id or one path.

## 4. Findings

Each finding: Jasper evidence (path:line at `b4f7816ea`), the Doriax reference, the dossier verdict.

### 4.1 Save over another scene's file (854022674, must, confirmed)

- The Save Scene dialog only warns: `editor/window/dialog/SceneSaveDialog.cpp:111-119` (`fileExists` then "will be
  replaced"), and its callback assigns `sceneProject->filepath = relPath` before saving
  (`editor/App.cpp:5722`), then `saveSceneToPathAsync` (`editor/Project.cpp:7500-7558`) and `saveSceneFile`
  (`editor/Project.cpp:7153-7310`) write with no owner check.
- Only the non-interactive APIs refuse, and only on an exact lexical match: `createSceneNow`
  (`editor/Project.cpp:4960`), `saveSceneNow` (`:5178-5181`), MCP Save As (`editor/mcp/tools/SceneTools.cpp:398-402`)
  through `findSceneByPath` (`editor/Project.cpp:4850-4862`, `lexically_normal` equality: `Level1.scene` and
  `level1.scene` are two files to it, one file on Windows and macOS).
- `encodeProject` appends every scene with a path, no dedupe of loaded scenes (`editor/Stream.cpp:3005-3010`; only
  unresolved entries are deduped, `:2985-3001`), and `decodeProject` loads every entry (`editor/Stream.cpp:3725-3770`).
  Two in-memory scenes on one file then overwrite each other on every save, for good.

### 4.2 Scenes outside the project (2235f4b50, 0c387ef45; should/could, confirmed)

- `saveSceneFile` checks only `ec || relPath.empty()` (`editor/Project.cpp:7185-7189`): `../x.scene` passes.
- The dialog's name field is unfiltered (`SceneSaveDialog.cpp:99`), `canSave` is `!fileName.empty()` (`:114`); the
  extension check is `find(".scene") == npos` (`:106`), so `a.scene.bak` is taken as a scene name.
- The dialog callback calls the throwing `std::filesystem::create_directories` (`editor/App.cpp:5713`) before any
  validation: a path whose folder is a file throws out of the ImGui frame (inferred).
- `loadScene(isNewScene)` accepts a file outside the project (`editor/Project.cpp:4324-4332`: `fs::relative` of an
  outside file is `../...`, not an error). Only `resolveScenePath` refuses (`:4920-4923`), on the MCP route.
- Exports already stop on a scene that cannot be saved (`editor/Exporter.cpp:2281-2282`, `:2391-2392`).

### 4.3 Copied scenes share an id (e0a38fe33, should, confirmed)

- `AssetOps::copy` (`editor/asset/AssetOps.cpp:189-221`) copies and imports (new GUID through `copyGuids`), and
  touches no scene id; `CopyFileCmd` (`editor/command/type/CopyFileCmd.cpp:253`) and MCP `move_asset copy`
  (`editor/mcp/tools/AssetTools.cpp:1660ff`) go through it. A replace-in-place copy (`CopyFileCmd.cpp:218-240`)
  writes the source's id into the target file.
- The loader renumbers a clash with `allocateSceneId()` (random, `editor/Project.cpp:4443-4449`, `:8581-8596`) and
  does not mark the scene modified: a new random id on every load; child-scene references are by id
  (`editor/Stream.cpp:4123-4136`), so a reference to the renumbered scene breaks on each reopen, and when the copy
  loads first the original is the one renumbered.
- The copy keeps the original's `name` while scene names must be unique (`SceneTools.cpp:535-539`, they name the
  generated code); `SceneManager.loadScene("Level1")` becomes ambiguous (inferred).
- MCP Save As (`SceneTools.cpp:389-415`) moves a scene to a new file and leaves the old file with the same id: a later
  `open_scene` of the old file is a clash too.

### 4.4 Child-scene GameObjects at same-scene drop targets (5c6a3d709, should, confirmed; c8368d371, could)

The child-scene row's drag (`editor/window/HierarchyWindow.cpp:1779-1798`) carries `entitySceneId` and a YAML tail it
re-encodes every frame (no `ImGuiCond_Once`, c8368d371). Targets of the `entity` payload:

| Target | File:line | Today |
|---|---|---|
| Hierarchy row (reparent/reorder) | `HierarchyWindow.cpp:1804-1826` → `getMovableDraggedEntities:146-163` | id used in the selected scene: wrong GameObject moved |
| Hierarchy empty space (to root) | `HierarchyWindow.cpp:3234-3252` | same |
| Project window (make prefab) | `editor/window/ProjectWindow.cpp:4135-4139` → `saveEntityFile:3541` → `createPrefab` on the selected scene | prefab from the child's YAML linked to an unrelated parent-scene GameObject |
| Inspector GameObject field (`unsigned int`) | `editor/window/InspectorWindow.cpp:5587-5604` | `isEntityCreated` in the owning scene only |
| Inspector camera texture | `InspectorWindow.cpp:1492-1497` | checks scene (ok) |
| Particles collision planes | `editor/window/InspectorParticles.cpp:943-951` | checks scene (ok) |
| Animation tracks | `editor/window/AnimationWindow.cpp:2838-2845` | checks scene (ok) |
| Inspector GameObject reference (`EntityReference`) | `InspectorWindow.cpp:5784-5804` | any loaded scene, related or not |
| Code editor drop | `editor/window/CodeEditor.cpp:1656-1666` | see 4.5 |
| Bookmarks, Pose Animation, VAT Baker, Sprite Normals | `BookmarksWindow.cpp:161`, `PoseAnimationWindow.cpp:1036`, `VertexAnimationBakerWindow.cpp:1614`, `SpriteNormalsWindow.cpp:2575` | use the payload's scene (ok) |

Only the Project window reads the YAML tail (`ProjectWindow.cpp:3543-3544`); every other target reads the header.

### 4.5 Picker and script drops across scenes (5821f79e1, should, confirmed)

- The GameObject picker lists the owning scene and its descendants only (`InspectorWindow.cpp:2651-2666`): a child
  scene's script cannot pick a parent or sibling scene's GameObject, while the runtime resolves any loaded scene's id
  (`engine/core/script/LuaBinding.cpp:1688-1691`).
- A drop into a Lua script links only in the selected scene, on the first matching instance, then returns
  (`CodeEditor.cpp:1081-1110`); C++ the same (`:1114-1117` and on). The closed-scene fallback (`:1003-1006`) is
  practically unreachable (payloads always carry a scene id, `HierarchyWindow.cpp:1498`, `:1796`).

### 4.6 Model import and list order (42aafbbb3 must; 7d06780c4 skip; confirmed)

- `ModelLoadCmd::finalizeLoad` appends glTF nodes in `std::map` index order (`editor/command/type/ModelLoadCmd.cpp:
  431-444`, the map at `ProjectUtils.cpp:413-425`) and `restoreArrangement` returns before its sort when nothing is
  parked (`:355`): the tracked list is child-first.
- `CreateEntityCmd` places children under the parent in the Transform array but `push_back`s them in the list
  (`CreateEntityCmd.cpp:942-960`); `DuplicateEntityCmd` places top-level copies after their sources and leaves their
  descendants at the end (`DuplicateEntityCmd.cpp:97`, `:150-175`).
- The generated scene code emits blocks in list order (`editor/Factory.cpp:2721-2723`, `:2832-2834`) and sets
  `transform.parent` directly (`:1136`); `writeSceneSource` passes the live list (`Project.cpp:7385`); exports save
  open scenes to regenerate (`Exporter.cpp:2240-2281`); the cooker walks the list too (`editor/cook/SceneCooker.cpp:
  141`). `TransformPropagation.h:24-50` updates a child before its parent and `RenderSystem.cpp:15161` clears
  `needUpdate` on both, so a node that stops moving keeps a pose from a stale parent (corrected evidence, inferred).
- Generated sources are recognised by `Factory::sceneSourceHeader` ("scene source 3", `Factory.cpp:2665`,
  checked by `Generator::hasSceneSource`, `Generator.cpp:2331-2336`).

### 4.7 Cameraless inline child beside a running game (482ff693e, should, confirmed, uncertain)

`loadChildSceneInline` builds no SceneRender (`Project.cpp:5328-5385`), so the child's camera stays `NULL_ENTITY`
until `Scene::load()` (`engine/core/Scene.cpp:922-925`) runs from `Engine::addSceneLayer` in `SceneRender::activate`
(`SceneRender.cpp:646-650`). Beside a running game a stopped scene never activates: `renderSceneBesideGame`
(`App.cpp:4164-4175`) calls `childScene->scene->update(dt)` on a child loaded during Play (the Hierarchy's "Show
Contents" is enabled for a stopped scene's row, `HierarchyWindow.cpp:2125-2133`), reaching
`scene->getComponent<CameraComponent>(scene->getCamera())`, which throws (`MeshSystem.cpp:6982` sprite flip,
`UISystem.cpp:1537`, `:1552`, `:1609` and `MeshSystem.cpp:3728`, `:4309`, `:7064`, `:7081`;
`ComponentArray.h:873-878`); no catch up the stack (`SystemScheduler.cpp:369-382`). Path traced by reading; needs the
repro test of section 10.

### 4.8 Scene view after the game loads another scene (1e6ef8baa, should, confirmed, uncertain)

`drawSceneView` always draws `this->scene` (the played scene) from the editor camera as the base, and runtime layers
through their own cameras in the game rectangle (`SceneRender.cpp:737-748`). After a load replaces the played scene
(`Project::isPlayedSceneReplaced`, `Project.cpp:10586-10597`), only the Game view swaps its main scene
(`App.cpp:4081-4089`); the Scene view shows the retired scene with the loaded level in the game rect, and picking
(`Project::getSceneViewLayers`, `Project.cpp:7853-7915`) follows the same layout. The edit-path activation at
`App.cpp:3936-3944` has no replacement handling either; by reading it does not run during a session
(`App.cpp:3878-3883` continues first), to be confirmed by the pause/step test. Prefab Mode In Context already mirrors
the view camera into another scene (`editor/PrefabStage.cpp:1135-1180`): the mechanism to reuse.

### 4.9 Captured cursor during Play (e5d7aad14 cursor part, could, confirmed, uncertain)

The cursor goes back to the editor only while the session is paused or loading:
`Backend::setMouseControlSuspended(playSessionActive && !project->isMainScenePlaying())` (`editor/EditorFrame.cpp:
122-123`; `BackendMac.mm:1024-1025`); `setMouseMode(CAPTURED)` hides and locks the pointer while not suspended
(`BackendLinux.cpp:2990-2995`, `BackendWin.cpp:1052-1058`, `BackendMac.mm:1179`). `PlayInput::forward` already
unlocks the game's pointer lock when the Game view loses focus (`editor/window/PlayInput.cpp:36-41`), but a captured
OS pointer cannot reach another panel to move the focus.

### 4.10 Child scenes edited on a closed parent (385cff970 closed-parent part, should, confirmed)

`setChildSceneTool` never tests `parent->scene` (`SceneTools.cpp:669-728`, unlike `setMainCameraTool:741-746`);
`AddChildSceneCmd` reaches `Project::addChildScene` (`Project.cpp:5414-5443`), which sets `isModified` on a scene no
save writes (`Project.cpp:5153`, `:7047`), and opening runs `childScenes.clear()` from the file
(`Stream.cpp:4123`). Same for detach and `start_active`.

### 4.11 Save All, dialog callbacks, start scene (fe5e52774, could, confirmed)

- `App::saveAllFunc` (`App.cpp:263-287`) → `Project::saveAllScenes` (`Project.cpp:7562-7582`) collects only
  `hasSceneUnsavedChanges`; `saveSceneListSequentially` re-checks it (`:7111`). An untouched never-saved scene
  (every new scene starts unmodified, `Project.h:233`; a project with no scene gets one, `Project.cpp:6883`) is never
  saved and `project.yaml` leaves out scenes with no file (`Stream.cpp:3005-3008`).
- Plain Save is disabled for an untouched scene of a normal project (`AppCommands.cpp:112-117`), even an untitled one.
- `registerSaveSceneDialog(uint32_t, std::function<void()>)` (`EditorHost.h:97`, `App.h:589`): the callback runs only
  on success (`App.cpp:5728-5730`); Cancel (`:5744-5751`), a failed save and a save that did not start (`:5737-5741`)
  run nothing, so `saveScene`'s `finishWithChildren(true)` (`Project.cpp:7083-7085`) never reports a cancel. Project
  dialog the same (`EditorHost.h:120`, `App.cpp:5763-5797`).
- No start scene is set when the first scene gets a file; fallbacks exist at export (`ExportWindow.cpp:84-88`,
  `ExporterPrebuilt.cpp:581`) but not in the benchmark lookup (`App.cpp:3027`).

### 4.12 Dialog lifetime and project switch (c790bc97b, 5a8c7e98d; could, confirmed / uncertain)

- The save-chain cycle is already solved (`Project.cpp:7093-7097`, weak self reference).
- `SceneSaveDialog` and `ProjectSaveDialog` run `m_onSave`/`m_onCancel` without clearing them
  (`SceneSaveDialog.cpp:133-152`, `ProjectSaveDialog.cpp:91-108`); `close()` only clears `m_isOpen`
  (`SceneSaveDialog.h:35`, `ProjectSaveDialog.h:32`), so `App::exit` (`App.cpp:5918-5921`) leaves
  `saveDialogInProgress` true (set at `:5700`, cleared only by the callbacks).
- `prepareForProjectSwitch` (`App.cpp:4861-4906`) touches neither dialog nor `saveDialogQueue`; the queue is drained
  every frame (`:2806`) and looks scenes up by id in whatever project is open (`:5668-5690`). Reachable through MCP
  `open_project` (`ProjectTools.cpp:545-556`) and native menus (inferred).

### 4.13 Save failures that only log (ecfffde83, could)

Write failures alert with the reason (`Project.cpp:7230-7246`), and an incomplete encode alerts (`:7295-7303`);
other exceptions only log (`:7304-7306`), the background catch only logs (`:7543-7545`), a busy/absent scene in
`saveSceneToPathAsync` only warns (`:7503-7512`), `saveProjectFile`'s refusals only log (`:6468`, `:6477`). `saveFunc`
passes no callback (`App.cpp:259`), so those failures show only in the Console.

### 4.14 Ctrl+S during Play (f5fb1f18c, could)

`file.save`/`file.saveAll` are disabled while playing (`AppCommands.cpp:112-119`) and the dispatcher never runs a
disabled command (`ShortcutDispatcher.cpp:52`): the key does nothing visible. `CommandDef::disabledReason` exists
(`CommandCatalog.h:68-69`) but only menus show it.

### 4.15 Cameraless 3D runtime scenes and a stale main camera (f8be9029f, could, verdict open)

`getSceneCamera` returns `mainCamera` when the entity exists, without checking it still has a Camera
(`Project.cpp:4174-4182`); removing the Camera component leaves `mainCamera` set (nothing in
`RemoveComponentCmd.cpp`; only a destroyed entity clears it, `ProjectUtils.cpp:1405-1407`).
`prepareRuntimeScene` calls `setCamera` with whatever came back (`Project.cpp:4218-4219`); for a runtime clone of a
3D scene without a camera that is `NULL_ENTITY` (3D scenes have no editor default camera, `Project.cpp:1436-1438`),
which logs "Invalid camera entity" (`Scene.cpp:227-240`) before `Scene::load` gives it the engine's default UI camera.
An export does the same (`Factory.cpp:2905-2919` emits `setCamera` only for a camera it has), so editor and export
agree today.

### 4.16 `move_entity` with a Transform-less anchor (2eea20daa, refuted; EXTRA-FINDINGS)

Jasper keeps a GameObject without a Transform in its gap (`ProjectUtils.cpp:985-1069`), and
`tools/mcp/tests/test_scene_order.py` relies on moving Skybox/Fog/Loose after Transform GameObjects. Only the
reverse is wrong: a Transform source with a Transform-less `before`/`after` anchor takes the list-only branch
(`ProjectUtils.cpp:1144-1160`), `MoveEntityOrderCmd::execute` ignores that (`MoveEntityOrderCmd.cpp:70-110`) and
`move_entity` reports "Moved" (`EntityTools.cpp:888-985`) while the next sort undoes it.

### 4.17 Multi-delete undo of Transform-less GameObjects (ff477db58 residue; EXTRA-FINDINGS)

`DeleteEntityCmd` records a Transform-less GameObject's full-list index at construction (`DeleteEntityCmd.cpp:
194-198`), restores in the caller's order (`:138-152`, `:738`), and applies that index while earlier GameObjects of
the same delete are still absent (`:798-803`). `{Light, A, Sky, B}` deleting `[Sky, Light]` undoes to
`{Light, A, B, Sky}`; `{T1, T2, S1, S2}` deleting `[S2, S1, T1]` swaps the skies (the first Skybox is the sky).
Nothing in it calls `keepUntransformedPlaces` (`ProjectUtils.cpp:1071`), which `MoveEntityOrderCmd::undo`
(`:125`) and `RemoveComponentCmd` (`:187`) use.

## 5. Product decisions

1. **The file decides who a scene is; the save refuses to share it.** One check, `Project::findOtherSceneUsingFile`,
   guards every write (the core `saveSceneFile`, so no route can skip it) and the dialog shows it live. Reason: the
   damage is silent and permanent (two scenes overwriting one file); Unity never lets two open scenes share an asset.
2. **Path equality follows the project's file system.** Compare project-relative, lexically normal, generic paths,
   case-folded when the project's volume is case-insensitive (`AssetDatabase`'s `detectCaseInsensitive`, already
   per project), plus `fs::equivalent` when both files exist (links). Reason: Windows and macOS users hit
   `Level1`/`level1`; a fixed per-OS rule is wrong on case-sensitive APFS or a Linux mount of NTFS.
3. **Copies get a new scene id at copy time, chosen once per command** (redo gives the same id, as `copyGuids` does
   for GUIDs). A replace-in-place copy keeps the **target's** id (it keeps its GUID and identity already). Reason:
   Doriax's fix; the id is the scene's identity for child-scene references, the build list and generated code.
4. **References between scenes copied together are not remapped.** A copied folder's parent scene keeps its child
   scenes (the originals). Reason: every other reference in a copied folder (materials, prefabs, clips by GUID)
   already keeps pointing at the originals; one rule for all assets beats a scene-only exception. Documented.
5. **A copy is renamed after its file** (`Level1 1.scene` → scene "Level1 1"; the next free name if taken) when the
   source's name equals the source's file stem; a source with a hand-given name keeps it with a numbered suffix.
   Reason: scene names must be unique (generated code, `SceneManager.loadScene(name)`); Unity names scenes by file.
6. **An id clash at load is resolved deterministically and saved away.** The scene listed first in `project.yaml`
   (or already open) keeps the id; the other gets a replacement derived from its file's GUID (stable across loads,
   probed past taken ids), is marked modified when its data is loaded, and the Console names both files. Nothing is
   written on open. Reason: today's random renumber breaks references on every reopen; opening a project must not
   write files (VCS-locked files, `Project::setSelectedSceneId` comment).
7. **New scene paths stay inside the project; old outside scenes keep working.** The dialog refuses `/`, `\`, `..`,
   absolute names and names invalid on Windows (portable projects); the core refuses a *new* path outside the project;
   `open_scene`/drops of an outside file are refused with "copy it into the project first". A scene an old project
   lists outside loads with a warning and saves in place. Reason: Unity has no scene outside Assets; old projects
   must load and must not become unsavable.
8. **The Save dialog sets nothing until the save succeeded.** `saveSceneFile` already assigns `filepath` on success;
   the App callback stops assigning it and creating folders. Reason: a failed save must not leave a scene pointing at
   a file it never wrote.
9. **Child-scene drags are reference drags.** The payload carries a `referenceOnly` flag; same-scene targets refuse
   it with a tooltip naming the scene; the Project window refuses it ("Open "HUD" to make a prefab of its
   GameObjects"). All `entity` payloads become header-only; the Project window encodes the selection on drop.
   Reason: one rule for every target instead of per-target guesses; the YAML tail was read by one target and paid by
   every drag frame (c8368d371).
10. **"Scenes loaded together" = related scenes**: the scene, every ancestor through child-scene links, and every
    descendant of those (Doriax's `getRelatedScenes`). The picker lists them grouped by scene; GameObject reference
    drops accept them only (tooltip otherwise: "not loaded with this scene"); a drop into a script links the property
    on every instance of that script in related scenes (not only the first). Reason: these are the scenes the
    runtime can resolve together; Unity forbids cross-scene references, Jasper allows them where they can work.
11. **The tracked list is kept in Transform order by every command that adds GameObjects, and the generator sorts a
    copy anyway**; the scene source header becomes "scene source 4" so sources written by older editors are
    regenerated. Reason: an invariant enforced at the producers, with the export protected even if a future command
    forgets.
12. **No stand-in camera for a cameraless 3D runtime scene.** Editor Play and export both draw it through the engine
    default; the editor stops calling `setCamera` with nothing and warns once per Play: `Scene "X" has no camera: add
    a Camera or set its main camera (the game draws it through the default camera)` (Unity's "No cameras rendering").
    The stale main camera is fixed: anything that reads `mainCamera` requires a Camera component. Reason: an
    editor-only stand-in would make Play differ from the export.
13. **An inline child gets its camera when it is loaded, and the engine never throws for a missing camera.** Loading
    a child inline prepares it for drawing (`Scene::load` once); `MeshSystem`/`UISystem` skip camera-dependent work
    (sprite/UI flip, sprite build) while a scene has no camera and retry next frame. Reason: the crash is a class
    (any cameraless update), not one call site; Doriax hid the child instead, which loses the view.
14. **Scene view after a scene load shows the loaded scene from the editor camera**, through a mirror of the editor
    camera placed in the loaded scene (Prefab Mode In Context's mechanism, shared), with picking and selection; the
    Game view is untouched. Reason: multi-level games are the norm; the editor camera is the Scene view's contract.
15. **Esc releases a captured cursor; clicking the Game view captures it again** (Unity). The Escape key is still
    forwarded to the game. A command "Release Game Cursor" (`playmode.releaseCursor`, no default key) gives menus and
    agents (`run_command`) the same. The cursor is also handed back whenever the Game view is not focused. Reason:
    Unity's behaviour; Pause should not be the only way out.
16. **`move_entity` refuses only a Transform source next to a Transform-less anchor**, at the command
    (`MoveEntityOrderCmd`) and with an MCP message that names the alternatives (`parent`/`index`, or an anchor with a
    Transform). The Hierarchy keeps refusing both directions (stricter UI). Reason: the skeptic's narrow rule;
    `test_scene_order.py` keeps passing.
17. **`set_child_scene` refuses a parent whose scene data is not loaded** (`scene == nullptr`). An inline-loaded
    child counts as loaded. It may run during Play for `loaded_inline` on a parent that is not running (what the
    Hierarchy's Show Contents does); attach/detach/start_active stay refused during Play. Reason: refusing beats a
    success that is lost; agents get what humans have.
18. **Save All saves never-saved scenes too** (after the modified ones, one dialog each); Ctrl+S on an untitled scene
    opens the dialog even when untouched; the save-before-quit/switch flow keeps saving only scenes with unsaved
    changes (an untouched untitled scene holds nothing to lose). MCP `save_scene all=true` lists untitled scenes in
    `skipped` whether touched or not. Reason: Unity's Save on an Untitled scene asks; quitting does not ask about an
    empty untitled scene.
19. **Dialog callbacks take a `bool`**: true = saved, false = cancelled, failed, did not start, or dropped by a
    project switch. A cancelled dialog stops a Save All chain (and the quit or switch that waited on it). Reason:
    callers cannot do the right thing without knowing.
20. **The first scene to get a file becomes the start scene** when the project has none (or names a scene that no
    longer exists), logged in the Console. Reason: Doriax's `ensureStartScene`; the export already falls back to it,
    the benchmark lookup does not.
21. **Every scene-save failure alerts once, with the reason** (non-interactive callers get it as their error, as
    today through `alertUser`'s depth). Reason: a Console-only failure is a silent data loss for a human.
22. **A shortcut whose command is disabled shows why in the status bar** when the command has a `disabledReason`
    (generic in the dispatcher); Save and Save All get "Stop Play to save (changes made in Play mode are not
    kept)". Reason: Doriax's alert interrupts testing; a transient status line informs without blocking, and the fix
    helps every command with a reason.
23. **A project switch cancels the old project's save dialogs** (each waiting callback gets false) and waits for a
    save in flight to finish. Reason: a dialog of project A over project B can only do harm.

## 6. Design

### 6.1 Scene file ownership (editor/Project.{h,cpp}, editor/Stream.cpp)

New in `Project` (public; const where shown):

```cpp
// The project-relative key two paths of one file share: lexically normal, generic, case-folded on a
// case-insensitive project volume (AssetDatabase::isCaseInsensitive)
std::string sceneFileKey(const fs::path& path) const;
// The scene (not `exceptSceneId`, not a Play copy, not a prefab stage) saved at `path`; also matches
// through fs::equivalent when both files exist. NULL_PROJECT_SCENE when none.
uint32_t findOtherSceneUsingFile(const fs::path& path, uint32_t exceptSceneId) const;
// "" when `path` may become this scene's file, else the reason a human or agent reads
std::string sceneSavePathProblem(uint32_t sceneId, const fs::path& path) const;
```

- `AssetDatabase` gets `bool isCaseInsensitive() const` (the flag exists, `AssetDatabase.h:255`). Case folding uses
  the same folding the database uses for its own keys; if there is none, ASCII folding plus `FileUtils`'s UTF-8 lower
  (coordinate with `unicode-text`).
- `findSceneByPath` is reimplemented on `sceneFileKey` (its callers `createSceneNow`, `saveSceneNow`, `openSceneNow`
  keep their behaviour, now case-aware).
- `sceneSavePathProblem` returns, in this order: outside the project ("Scenes are saved inside the project folder:
  <path> is outside it") unless `path` is the scene's current file (decision 7); not a `.scene` extension; owned by
  another scene ("<rel> is the file of scene "<name>": choose another name"); a directory exists there.
- `saveSceneFile` (`Project.cpp:7153`) calls `sceneSavePathProblem(sceneProject->id, fullPath)` before anything is
  written (before prefab saves and encode); a problem logs an error, alerts (decision 21) and returns `false` with
  `SaveResult::Failed`. This is the guard every route passes (dialog, Save All, autosave does not save scene files,
  MCP Save As, `saveSceneNow`, `writeSceneToPath`).
- The MCP Save As branch (`SceneTools.cpp:398-402`) and `saveSceneNow`/`createSceneNow` switch to
  `sceneSavePathProblem` so their messages match the core's.
- `Stream::encodeProject` (`Stream.cpp:3005-3010`): skip a scene whose `sceneFileKey` was already appended this
  encode (a `std::unordered_set<std::string>`), with `Out::warning` naming both scenes (only reachable through a bug
  or an old session).
- `Stream::decodeProject` (`Stream.cpp:3725-3770`): before `loadScene`, skip an entry whose key matches an earlier
  entry (`Out::warning("project.yaml lists %s twice: the second entry is ignored (the next save writes it once)")`);
  `listPosition` still advances so unresolved positions stay right.

### 6.2 Scene ids: copies and clashes

- New `editor/project/SceneFileIdentity.{h,cpp}` (namespace `jasperengine::editor::sceneid`):
  - `uint32_t replacementId(const Guid& fileGuid, const fs::path& relative, const std::function<bool(uint32_t)>&
    taken)`: FNV-1a 32 of the GUID text (of the relative path when the GUID is null), mapped to `[1, UINT32_MAX]`,
    then `+1` probing while `taken`. Deterministic for one file in one project.
  - `bool readSceneHeader(const fs::path& file, uint32_t& id, std::string& name)`: reads the root `id` and `name`
    through `DocumentFiles::read(..., DocumentKind::Scene)`.
  - `bool rewriteSceneHeader(const fs::path& file, std::optional<uint32_t> id, std::optional<std::string> name,
    std::string* error)`: line edit of the root (indent 0) `id:` and `name:` lines, modelled on
    `AssetDatabase::writeInFileIdentityOnce` (`AssetDatabase.cpp:517-600`): keeps line endings, leaves nested `id:`
    keys (`childScenes: - id:`) alone, writes through `assetio::rewriteFile` with a read stamp (a concurrent change
    fails it). The name is YAML-quoted with the same emitter rule the encoder uses (unicode safe).
- `AssetOps::copy` (`AssetOps.cpp:189`) gains a `CopyIdentity*` parameter replacing `copyGuids`:
  `struct CopyIdentity { std::map<std::string, std::string> guids; std::map<std::string, uint32_t> sceneIds; };`.
  After the files are copied and imported, for every `.scene` in the copy (the file, or a recursive walk of a copied
  folder, `Util::isSceneFile`): the recorded id for its relative path if any (redo), else `allocateSceneId()` (random,
  also checked against ids read from the other copies of this command); the name per decision 5 (unique against
  `project.getScenes()` and the other copies). `rewriteSceneHeader`; a failure fails the copy and removes it, as a
  failed file copy does today.
- `CopyFileCmd` (`CopyFileCmd.cpp:253`) passes its `CopyIdentity` member; the replace-in-place branch
  (`:218-240`) reads the target's id and name before `replaceFile` and writes them back after it (decision 3).
  `AssetTools.cpp` `move_asset copy=true` reports `copied_scenes: [{path, scene_id, name}]` in `structuredContent`
  and its description gains "a copied scene gets its own scene id and name".
- Loader (`Project::loadScene`, `Project.cpp:4440-4450`): a clash takes
  `sceneid::replacementId(assetDatabase.getAssetGuid(rel), rel, [this](uint32_t id){ return getScene(id); })`
  instead of `allocateSceneId()`; `targetScene->isModified = loadSceneData` for a renumbered scene (the save writes
  the id); the warning becomes `Scene file "%s" carries id %u, which "%s" uses: it gets id %u (stable; save it to
  keep it). References to %u still mean "%s".`. `NULL_PROJECT_SCENE` ids (no `id` key) take the same function.
  `loadUntitledScene` keeps `allocateSceneId()` (no file).
- `createRuntimeCloneFromNode` already forces `runtime->id = source->id` (`Project.cpp:3389`): unchanged.

### 6.3 Scenes inside the project

- `SceneSaveDialog` (`editor/window/dialog/SceneSaveDialog.{h,cpp}`):
  - `static std::string nameProblem(const fs::path& projectPath, const fs::path& folder, const std::string& name,
    std::function<std::string(const fs::path&)> ownerProblem)`: empty name; `/`, `\`, `..`, a drive or root; a
    character or name invalid on Windows (`<>:"|?*`, control characters, trailing dot or space, `CON`/`PRN`/...);
    the extension check uses `fs::path(name).extension() == ".scene"` (appended otherwise, `a.scene.bak` →
    `a.scene.bak.scene` is shown so the user sees it); then `ownerProblem(fullPath)` (the App passes
    `project.sceneSavePathProblem(sceneId, …)`).
  - The dialog shows the problem in `Theme::Colors::ErrorText` under the name and disables Save; an existing file no
    scene owns keeps the orange "will be replaced" line. `UIProbe::item("sceneSave.name" / "sceneSave.save" /
    "sceneSave.problem")` for tests.
  - `open()` takes the scene id it saves (for the owner check).
- `App::processNextSaveDialog` (`App.cpp:5668-5800`): the Save callback no longer calls `create_directories` or
  assigns `filepath`; it calls `project.saveSceneToPathAsync(sceneId, fullPath, …)`, which creates the folder with
  `FileUtils::createDirectories` (non-throwing) inside the save after validation.
- `Project::loadScene` with `isNewScene` refuses a file outside the project
  (`Out::error` + `alertUser("Scene Outside the Project", "<file> is outside the project folder. Copy it into the
  project (drag it into the Project window) and open the copy.")`); `decodeProject` loads an outside entry of an old
  project (it calls `loadScene(isNewScene=true)` too: a `bool allowOutside` parameter, true only from
  `decodeProject`) and warns once per scene `Scene "%s" is outside the project folder (%s): it is saved in place;
  move it into the project to keep the project self-contained.`.

### 6.4 Entity drag payloads (editor/util/EntityPayload.h, the drop targets)

```cpp
struct EntityPayload {
    Entity entity;
    Entity parent;
    size_t order;
    bool hasTransform;
    uint32_t entitySceneId = 0;   // the scene the id belongs to (always set by the Hierarchy)
    bool referenceOnly = false;   // a child scene's GameObject: usable as a reference only
};  // nothing follows any more

// The scene a dropped GameObject belongs to (0 resolves to `fallbackSceneId`)
uint32_t payloadSceneId(const EntityPayload&, uint32_t fallbackSceneId);
// "" when a same-scene target may take it, else the tooltip ("GameObject belongs to child scene \"HUD\"")
std::string sameSceneDropProblem(const Project&, const EntityPayload&, uint32_t targetSceneId);
// "" when a GameObject reference of `ownerSceneId` may point at it, else the tooltip
std::string referenceDropProblem(const Project&, const EntityPayload&, uint32_t ownerSceneId);
```

(implemented in `editor/util/EntityPayload.cpp`, new; the header keeps `MaterialPayload` as is.)

- `HierarchyWindow::buildEntityPayload` (`:1456`) and the child-scene source (`:1779-1798`) emit header-only payloads
  with `ImGuiCond_Once`; the child-scene one sets `referenceOnly`. The pose suspension (`PoseEditSession::
  suspendActiveFor`) moves to the Project window's drop.
- `HierarchyWindow::encodeDraggedSelection(uint32_t sceneId, Entity dragged) -> std::string` (the YAML the payload
  used to carry; `saveNodeAsPrefab` uses it too). `ProjectWindow` (`:4135-4139`) accepts with
  `ImGuiDragDropFlags_AcceptBeforeDelivery`, shows `sameSceneDropProblem` or the reference message, and on delivery
  calls `saveEntityFile(dir, sceneId, yaml)` (signature changed from raw payload bytes) with the payload's scene,
  so `createPrefab` links the GameObject in its own scene.
- Same-scene targets use `sameSceneDropProblem` and show its text as the tooltip: Hierarchy row (`:1804-1826`) and
  root drop (`:3234-3252`), Inspector GameObject field (`InspectorWindow.cpp:5587`), and the three already guarded
  (camera texture, particles, Animation tracks) switch to the helper for one wording.
- `InspectorWindow.cpp:5784` (GameObject reference) uses `referenceDropProblem`.
- Cross-scene targets that take any scene (Bookmarks, Pose, VAT Baker, Sprite Normals) use `payloadSceneId`.
- Grep gate: `rg 'AcceptDragDropPayload\("entity"' editor` lists exactly the audited targets (the test of 10.2
  greps the count so a new target is reviewed).

### 6.5 Related scenes, picker and script drops

- `Project::getRelatedScenes(uint32_t sceneId) const -> std::vector<uint32_t>`: ancestors by walking every scene's
  `childScenes` (cycle-safe with a visited set, cycles exist in hand-edited projects, `ProjectSaveTest.cpp:274`),
  then `collectInvolvedScenes` of each ancestor, deduplicated, the scene itself first, then ancestors nearest first,
  then the rest in project order.
- Picker (`InspectorWindow.cpp:2646-2666`): `includeChildScenes` becomes `includeRelatedScenes`; loaded related
  scenes are listed under a scene header row (`ICON_FA_LAYER_GROUP "  Level (parent)"`, "(child)", "(sibling)");
  related scenes not loaded show one dim row "Level2 (not open)". The stored scene id stays 0 for the owning scene.
- `CodeEditor::insertLuaEntityProperty`/`insertCppEntityProperty` (`CodeEditor.cpp:1003`, `:1081-1117`): the source
  scene is `payloadSceneId(...)` (no fallback to the selected scene), the property is linked on every instance of the
  script in every loaded related scene (stored id 0 in the source scene, the source id elsewhere), through the same
  direct `PropertyCmd::execute` as today (the text undo re-parses the properties, `:1605-1608`); scenes changed are
  marked modified.

### 6.6 Transform order of the tracked list

- `ModelLoadCmd::finalizeLoad` (`:431-444`): after appending, `ProjectUtils::sortEntitiesByTransformOrder(scene,
  sceneProject->entities)` (stable, keeps Transform-less GameObjects in their gaps), before `restoreArrangement`.
- `CreateEntityCmd` (`:956-960`): same sort after the push_backs when the new GameObject has a parent or children.
- `DuplicateEntityCmd` (`:150-175`): the sort after the duplicate-after-source pass (descendant copies then join
  their branch).
- `Factory::createScene` (`Factory.cpp:2667`): sorts a local copy of `entities` with
  `sortEntitiesByTransformOrder(scene, copy)` before emitting; `SceneCooker` (`SceneCooker.cpp:141`) the same.
  `sceneSourceHeader` → "scene source 4".
- `ProjectUtils::isTransformOrdered(EntityRegistry*, const std::vector<Entity>&)` (true when sorting would change
  nothing) for tests and a debug assertion in `writeSceneSource` (`#ifndef NDEBUG`: `Out::warning` naming the scene
  once).

### 6.7 Inline children beside a running game; cameraless updates

- `Project::loadChildSceneInline` (`Project.cpp:5328`): after `loadSceneProjectData`, when the Engine's view is loaded
  and `childScene->scene->getCamera() == NULL_ENTITY`, call `childScene->scene->load()` (what `Engine::includeScene`
  does for a layer; creates the default camera and loads the systems). The stopped path keeps working (activate's
  `addSceneLayer` finds it loaded).
- `App::renderSceneBesideGame` (`App.cpp:4153-4175`) and the stopped path (`:3993-4006`) share
  `App::updateInlineChildScene(SceneProject& child, double dt)`: beside a game the child is updated by
  `scene->update(dt)` as today, but only after the camera check above (a child somehow without a camera is loaded
  first).
- Engine hardening (`engine/core/subsystem/MeshSystem.cpp`, `UISystem.cpp`): every
  `scene->getComponent<CameraComponent>(scene->getCamera())` listed in 4.7 becomes
  `scene->findComponent<CameraComponent>(scene->getCamera())`; when null, the flip/size work is skipped and the
  sprite/polygon/text keeps its `needUpdate*` flag (built next frame). `RenderSystem.cpp:14102` and
  `Scene.cpp:895` get the same null check. No behaviour change with a camera.

### 6.8 Scene view after a scene load (editor/render, editor/Project.cpp, editor/App.cpp)

- New `editor/render/ViewCameraMirror.{h,cpp}` extracted from `PrefabStage.cpp:1135-1180`:
  `Entity ensure(Scene* target, Entity& slot, const char* name)` (system entity with Transform + Camera),
  `void copy(Scene* from, Entity fromCamera, Scene* to, Entity toCamera, const Matrix4& toTarget)` (type, clips,
  yfov, aspect, near/far, useTarget, transparentSort, world pose through `toTarget`; `autoResize = false`).
  `preparePrefabStageView` uses it (no behaviour change).
- `Project` gains, for the active session, `Scene* getReplacingMainScene(uint32_t playedSceneId)` (the
  `SceneManager::getCurrentSceneId()` scene when `isPlayedSceneReplaced`, else null) and the mirror slot
  `Entity sceneViewMirror` on the `PlaySession` with the runtime scene it lives in; released (entity destroyed if the
  scene is alive) when the loaded scene is retired or at Stop.
- `SceneRender::drawSceneView(width, height, layerScenes, gameLayers, Scene* replacingBase = nullptr, Entity
  replacingCamera = NULL_ENTITY)`: with a replacing base, the base layer is `{replacingBase, replacingCamera, full,
  editorOverlays=true, ...}` (the Scene view's own toggles), `this->scene` is not drawn, and `replacingBase` is skipped
  in the game-layer loop. Editor overlays (tools layer, UI layer gizmos) are drawn as today.
- `App::renderRunningScene` (`App.cpp:4136-4142`): when `getReplacingMainScene` is non-null, mirror the editor camera
  (`sceneRender->getCamera()`, identity `toTarget`) into the loaded scene before `drawSceneView`.
- `Project::getSceneViewLayers` (`Project.cpp:7853`): when replaced, the first pickable layer is the loaded scene with
  the mirror camera's view over the full rect (`makeLayerView`), the played scene's base layer is marked not pickable
  (`SceneViewLayer::pickable = false`, new field, default true), the other runtime layers as today. Picking, the
  selection outline and the transform gizmos of a selected loaded-scene GameObject follow from the layer
  (`ScenePicker.cpp:261`); the implementer checks how the selection overlay of a runtime-layer GameObject is drawn
  today and routes it through the mirror camera.
- Hierarchy: the played scene's root row gets the tooltip `Not running: the game loaded "Level"` and its rows are
  dimmed while replaced (`HierarchyWindow.cpp` root node draw).
- MCP: `get_play_state` gains `scene_view_scene_id` (the scene the Scene view draws as its base during Play).

### 6.9 Cursor hand-back (editor/window/PlayInput.{h,cpp}, editor/EditorFrame.cpp, editor/backend/BackendMac.mm)

- `PlayInput` keeps `bool cursorReleased` and exposes `bool wantsEditorCursor() const` = `cursorReleased ||
  !gameViewFocused` (from the last `forward`).
- In `forward`: Escape pressed while `focused` and `Engine::getMouseMode()` is CAPTURED or CONFINED → `cursorReleased
  = true`, `Engine::systemPointerLockChanged(false)`; the key is still forwarded (`:263`). A mouse button pressed over
  the Game view → `cursorReleased = false` (and the existing focus rule locks the pointer again). Play start/stop and
  a mouse mode change to NORMAL reset it.
- `EditorFrame::run` (`:122-123`) and `BackendMac.mm:1024-1025`: `setMouseControlSuspended(playSessionActive &&
  (!isMainScenePlaying() || playInput.wantsEditorCursor()))`. Windows and Linux reach the same through
  `EditorFrame`; macOS through its own loop. The macOS and Windows edits are made by reading the existing pattern
  (they cannot be built here).
- `DefaultCommands.cpp`: `b.add("playmode.releaseCursor", "Release Game Cursor", C::Global)` (agent safe, enabled
  while a session captures the cursor), runs the same release.
- `get_play_state` `input` gains `cursor: {mode: "normal"|"hidden"|"confined"|"captured", released_to_editor: bool}`.
- Status bar while released: "Cursor released: click the Game view to give it back to the game" (transient, 4 s).

### 6.10 Child scenes on closed parents (editor/mcp/tools/SceneTools.cpp, editor/window/HierarchyWindow.cpp)

- `Project::childScenesEditable(uint32_t sceneId, std::string& why) const`: false with "Scene "X" is not open: open
  it first (open_scene) to change its child scenes" when `scene == nullptr`; false for a prefab stage (existing
  message).
- `setChildSceneTool`: calls it before any command; `allowedWhilePlaying = true` with an in-handler refusal of
  attach/detach/start_active while a session runs ("Child scenes cannot change during Play"), and of `loaded_inline` on
  a parent that is running or a Play copy. Description adds "The parent must be open."
- The Hierarchy's child-scene menu items (Start Active, Remove) are disabled with the same `why` as tooltip (they act
  on open scenes today; this keeps one rule).

### 6.11 Save flows (editor/App.{h,cpp}, editor/EditorHost.{h,cpp}, editor/Project.cpp, dialogs)

- `EditorHost::registerSaveSceneDialog(uint32_t sceneId, std::function<void(bool)> callback = nullptr)` and
  `registerProjectSaveDialog(std::function<void(bool)> callback = nullptr)`; `App` overrides and the headless
  `EditorHost` (`EditorHost.cpp:130`, `:148`: call `callback(false)` at once, nothing can answer). Callers:
  `App.cpp:280` (`callback(saved)`), `App.cpp:876` (ignore), `Project.cpp:6433` (`saveProject`: the old
  `std::function<void()>` runs only on true), `Project.cpp:7071` (degraded Save As: none), `Project.cpp:7083`
  (`finishWithChildren(saved)`).
- `processNextSaveDialog`: every exit path calls `completion(bool)` exactly once: save finished (its `success`),
  save did not start (false), Cancel (false), scene gone (false). `SaveDialogQueueItem::callback` becomes
  `std::function<void(bool)>`.
- `SceneSaveDialog`/`ProjectSaveDialog`: `m_onSave`/`m_onCancel` are taken with `std::exchange` before running (a
  re-entrant `open()` from inside them is safe and nothing is retained); `close()` = cancel now: if open, take
  `m_onCancel`, clear both, run it. `void abandon()`: clears both without running (for the project switch).
- `App::cancelSaveDialogs()`: moves the queue out, `abandon()`s both dialogs, sets `saveDialogInProgress = false`,
  then runs each moved item's callback with false. Called from `processProjectChange` before `change()` and from
  `exit()` (replacing `sceneSaveDialog.close()` at `:5918`). `processProjectChange` defers (keeps the pending change)
  while `project.isAnySceneSaving()`.
- `Project::sceneNeedsSave(uint32_t id) const` = `hasSceneUnsavedChanges(id) || (scene loaded && filepath empty &&
  !isPlayRuntimeScene(id) && !isPrefabStage(id))`. `saveSceneListSequentially` takes a
  `std::function<bool(uint32_t)> needsSave` (Doriax's signature), `saveModifiedChildScenes` passes
  `hasSceneUnsavedChanges`. `saveAllScenes(callback, bool includeUntitled)`: modified scenes first (prefab stages
  first, as today), then untitled scenes when `includeUntitled`, each through `saveScene` (which opens the dialog).
  The menu's Save All passes true; `saveAllAndProject` (quit/switch) passes false. The refused-degraded continuation
  (`Project.cpp:7099-7106`) is kept.
- `AppCommands.cpp:112-117`: `file.save` is also enabled when the selected scene is loaded and has no file.
  `disabledReason` for `file.save`/`file.saveAll`: while playing "Stop Play to save (changes made in Play mode are not
  kept)", while a save dialog is open "A Save dialog is open".
- `Project::ensureStartScene()` (decision 20), called on the main thread after a successful save in
  `saveSceneToPath` and in `saveSceneToPathAsync`'s completion before `saveProject()`.
- Alerts (decision 21): `saveSceneFile`'s `catch (const std::exception&)` and the background catch
  (`Project.cpp:7543-7545`) alert `Couldn't save "<file>": <what>. Your changes are still in the editor.` through
  `enqueueMainThreadTask` + `alertUser`; `saveSceneToPathAsync`'s busy refusal alerts "Wait for <scene> to finish
  (saving or Play is starting) and save again"; `saveProjectFile`'s two log-only refusals (`:6468`, `:6477`) alert
  with their text. `alertUser` stays log-only inside non-interactive scopes.
- `ShortcutDispatcher`: `void setDisabledHandler(std::function<void(const CommandDef&)>)`; when a chord matches a
  command whose contexts are active and gates pass but `enabled()` is false, the handler runs (once per press) if
  `disabledReason` is set. App sets it to `StatusBar::setTransientMessage(def.name + ": " + reason, 4.0)`.
- MCP `save_scene all=true` (`SceneTools.cpp:346-380`): the loop also visits untitled loaded scenes without changes
  and lists them in `skipped` (`"reason": "never saved: save_scene with a path"`).

### 6.12 Stale main camera; cameraless runtime scenes

- `Project::getSceneCamera` (`:4174`): `mainCamera` only when it is created **and** has a `CameraComponent`.
- `prepareRuntimeScene` (`:4218`): `setCamera` only with a non-null camera; a 3D runtime scene left without one gets
  `Out::warning` once per session per scene (decision 12).
- `Generator`/`Factory` camera: the camera passed to `createScene` comes from `getSceneCamera` (check the call at
  `Generator.cpp:2309` and its caller) so a stale `mainCamera` emits no `setCamera` on a camera-less entity.
- `list_scenes` `main_camera`: null when `getSceneCamera` would not use it; the Inspector's Main Camera field shows
  "None (Camera removed)" in that case.
- `RemoveComponentCmd` keeps `mainCamera` as is (undo restores the component and the main camera works again).

### 6.13 `move_entity` and multi-delete undo

- `MoveEntityOrderCmd::execute` (`:70`): before `moveEntityOrderByTarget`, `if (sourceHasTransform &&
  !targetHasTransform && type != INTO)` warn "Cannot place "%s" next to "%s": GameObjects with a Transform follow the
  hierarchy; one without (a Skybox) keeps its own place" and return false. The command's tested contract for
  Transform-less sources is unchanged.
- `move_entity` (`EntityTools.cpp:888-907`): the same check up front, failing with "...: place it with parent (and
  index), or next to a GameObject with a Transform".
- `DeleteEntityCmd`: `execute` stores `listBefore = sceneProject->entities` (only when the delete includes a
  Transform-less GameObject, else empty: no cost); `undo` (`:719`), after the final `sortTracked()` and the id remap,
  remaps `listBefore` through `entityRemap` and calls `ProjectUtils::keepUntransformedPlaces(scene, listBefore,
  sceneProject->entities)`, then `scene->sortComponentsByEntityOrder<SkyboxComponent>(sceneProject->entities)` (the
  first Skybox is the sky, as `MoveEntityOrderCmd::undo:126` does).

### 6.14 Lua, export, platforms

- Lua: no API change. The engine change (6.7) is internal: a scene updated without a camera waits instead of
  throwing; same result on every backend and export target (CPU code only, no shader or GPU state).
- Export: generated scene code creates parents first (6.6) and regenerates once ("scene source 4"); a stale main
  camera no longer emits a `setCamera` on a camera-less entity (6.12).
- Platforms: the cursor change touches `BackendMac.mm` (cannot be built here: kept compiling by mirroring the
  `EditorFrame` expression), `EditorFrame.cpp` (Linux, Windows). Windows file names in the Save dialog are validated
  on every OS (portable projects). Case-insensitive comparison follows the project volume, not the OS.

## 7. Behaviour rules and edge cases

1. A scene saved to its own file under another spelling (`Level1.scene` vs `level1.scene` on a case-insensitive
   volume) is its own file: allowed (the save writes the scene's recorded spelling).
2. A Play copy and a prefab stage never own a scene file for the check (the copy shares its source's path).
3. A scene whose file was deleted on disk still owns the path while it is in the project: another scene cannot take
   it (remove the scene from the project first).
4. Two old entries for one file in `project.yaml`: the first loads; if the first is unresolvable (missing), the second
   is skipped too (it is the same missing file).
5. Copy of a folder holding `A.scene` and its child `B.scene`: both get new ids and names; the copy of A still
   references the original B (decision 4); `list_scenes` shows no duplicate.
6. Undo of a copy removes the copies; redo recreates them with the same ids and names, even if a scene with that name
   was created meanwhile (then the name is numbered again and the warning says so; the id is reused only if still
   free, else a fresh one, logged).
7. A copy made from outside the project (an external drop into the Project window) keeps its id when no project
   scene uses it (Doriax: links between imported scenes hold), else a new one.
8. Id clash at load between an open scene and a closed listed one: the earlier listed keeps it. Between the project
   list and an `open_scene` of an unlisted file: the listed one keeps it.
9. A renumbered closed scene keeps its replacement id in memory only; its file still says the old id until the scene
   is opened and saved (the warning says so). The replacement is the same at every load.
10. The Save dialog's folder tree only lists folders inside the project; the name decides the rest (no separators).
11. A child-scene payload dropped on the Inspector's GameObject reference field of a scene that does relate to it
    stores the source scene id; on a non-related scene the drop is refused even if that scene is loaded.
12. Picker in a scene with no relations: unchanged list (the scene only).
13. Script drop with no instance of the script in related scenes: the property is inserted in the text, nothing is
    linked (as today when nothing matches).
14. Model import with parked GameObjects: `restoreArrangement` still sorts after re-attaching; the new sort before it
    keeps the list ordered in between.
15. A GameObject without a Transform in the generator's sorted copy keeps its gap (the sort's rule).
16. An inline child loaded while the Engine's view is not loaded (headless start): `load()` waits for the first
    activation, as today.
17. A 2D or UI played scene replaced by a 3D level: the mirror copies the Scene view's camera as it is (orthographic
    for a 2D Scene view); the user can switch the Scene view's projection as usual. A replacing scene that is itself
    replaced again (level → level): the mirror moves to the new loaded scene; the old one is destroyed with its scene.
18. Restart (the played scene loaded again as a fresh copy) counts as replaced: the copy is shown from the editor
    camera.
19. Esc in the Game view with a NORMAL or HIDDEN mouse mode releases nothing (no capture) and the key goes to the game.
20. Window loses OS focus during Play: the cursor is handed back (already, through focus); regained focus does not
    recapture until a click on the Game view.
21. `set_child_scene loaded_inline=true` during Play on a stopped parent loads it inline and its Scene view keeps
    drawing beside the game (no crash).
22. Save All with two untitled scenes and Cancel on the first: the chain stops, the second is not asked, the call
    reports false (a quit/switch waiting on it does not proceed).
23. Save All while a scene is refused as degraded: the others continue, as today.
24. A project switch while an untitled scene's dialog is open: the dialog closes, its callback gets false, the switch
    continues with the unsaved choice the switch flow already made.
25. `ensureStartScene` with a start scene set to a removed scene: replaced by the first saved scene, logged.
26. Ctrl+S in the Code Editor during Play still saves the script (`saveFunc`'s Code branch): `file.save` stays enabled
    for a focused Code Editor with changes while playing.
27. `move_entity` with several entities and `after=<Skybox>` where the first is a Transform GameObject: the whole call
    fails before any move (one undo step, nothing applied).
28. Multi-delete undo when a restored GameObject got a new id (its old id was taken meanwhile): `listBefore` is
    remapped first; an id not in the remap and not alive is skipped by `keepUntransformedPlaces`.

## 8. Performance and cost

- Save checks: one pass over the project's scenes per save (`findOtherSceneUsingFile`), plus `fs::equivalent` only
  when both files exist. Negligible next to the encode.
- Copy: one read and one line-edit write per copied `.scene`; folder copies walk the copy once.
- Load: the clash path runs only on a clash. Project encode/decode dedupe: one hash set of keys.
- Drags: every `entity` drag gets cheaper (header only; no per-frame YAML for child-scene drags; the selection is
  encoded once, on a Project window drop).
- Tracked-list sorts: `O(n log n)` once per model load, create with children, duplicate; the generator sorts a copy
  per scene source write (already `O(n)` emit).
- Scene view mirror: exists only while a played scene is replaced and the Scene view is shown; per frame a copy of a
  few camera fields. No cost otherwise.
- Engine guard: the existing `getComponent` becomes `findComponent` at the same call sites (same lookup, no extra
  work) in update paths that already run.
- Cursor: one boolean per frame. Dispatcher: the disabled lookup runs only when a chord matched nothing.
- Nothing runs in exported games except the engine guard (zero cost with a camera) and the corrected generated code.

## 9. Compatibility

- No scene or project format change. Old projects load: duplicate `project.yaml` entries load once and are written
  once at the next save; outside-the-project scenes keep loading and saving in place with a warning; id clashes get a
  stable replacement instead of a random one (the warning tells to save).
- Generated sources: `sceneSourceHeader` "scene source 3" → "4". `hasSceneSource` then reports every old source
  missing and `writeMissingSceneSources` regenerates them (it already loads closed scenes for that); no user action.
- `EditorHost` callback signatures change (editor-internal API; plugins/editor scripts do not implement `EditorHost`;
  the implementer greps `registerSaveSceneDialog`/`registerProjectSaveDialog` across `editor/` and the editor-script
  bindings to confirm).
- `AssetOps::copy`'s parameter changes from `std::map<...>* copyGuids` to `CopyIdentity*` (callers: `CopyFileCmd`,
  `AssetTools`; `project-window` adds Duplicate on the same function).
- MCP: `move_asset` and `get_play_state` gain fields; `set_child_scene` becomes callable during Play (for
  `loaded_inline` only) and refuses closed parents; `move_entity` refuses one case it used to report as Moved;
  `save_scene all` lists untouched untitled scenes in `skipped`. Regenerate `docs/mcp/TOOLS.md`; descriptions stay
  within `tools/mcp/budget.json` (a few words each).

## 10. Tests

Red controls: each test marked **(red)** fails on `main` and passes with the change; the implementer records the red
run (the test run against the unchanged code, or with the fix reverted) in the commit message and the ledger.

### 10.1 Unit tests (`jasperengine-editor-tests`, `tools/build.sh unit-tests`)

`editor/tests/project/SceneFileIdentityTest.cpp`:
- `SceneFileIdentity.ReplacementIdIsStableAndFree`: same GUID → same id twice; probes past taken ids; never 0.
- `SceneFileIdentity.RewriteTouchesOnlyTheRootIdAndName`: a fixture with `childScenes: - id: 5` and a component field
  `id:` keeps them; CRLF kept; a UTF-8 name round-trips.
- `SceneFileIdentity.FindOtherSceneUsingFileFoldsCaseOnCaseInsensitiveProjects` **(red)**: two `SceneProject`s,
  `Levels/A.scene` and a lookup of `levels/a.scene` with the flag on finds it, off does not
  (`findSceneByPath` today misses it).
- `SceneFileIdentity.SavePathProblems`: outside path refused, own outside path allowed, other scene's file refused,
  directory refused.
- `SceneFileIdentity.RelatedScenesCoverAncestorsAndTheirDescendants` (and a cycle terminates).

`editor/tests/stream/StreamProjectScenesTest.cpp`:
- `StreamProjectScenes.EncodeListsEachSceneFileOnce` **(red)**: a `Project` with two `SceneProject`s on one path
  encodes one `scenes` entry.

`editor/tests/GeneratedScenesTest.cpp`:
- `GeneratedScenes.ParentsAreCreatedBeforeChildren` **(red)**: a scene whose list is `[child, parent]` generates the
  parent's Transform block first.

`editor/tests/stream/SceneOrderTest.cpp`:
- `SceneOrder.IsTransformOrderedSeesAChildFirstList`.

`editor/tests/window/SaveDialogsTest.cpp` (no ImGui frame needed: `nameProblem` and the callback plumbing):
- `SceneSaveDialog.NameProblems` **(red: `canSave` accepts them today)**: `../x`, `a/b`, `/abs`, `C:x`, `con`,
  `a?b`, `name.` refused; `a.scene.bak` gets `.scene`.
- `SceneSaveDialog.CloseRunsCancelOnceAndReleasesTheClosures` **(red)**: callbacks capturing a `shared_ptr`;
  `close()` runs onCancel once and the use count drops back to 1; `abandon()` runs nothing and releases.
- `ProjectSaveDialog.CloseRunsCancelOnceAndReleasesTheClosures` **(red)**.

`editor/tests/shortcuts/ShortcutDispatcherTest.cpp`:
- `ShortcutDispatcher.ADisabledCommandsChordReportsWhy` **(red)**: handler called once with the def; not called
  without `disabledReason`, nor when a context or gate blocks.

`editor/tests/engine/subsystem/CameralessUpdateTest.cpp`:
- `CameralessUpdate.SpriteWaitsForACamera` **(red: throws today)**: a Scene with a `SpriteComponent`
  (`automaticFlipY`, `needUpdateSprite`) and no camera; `update(0)` does not throw, the flag stays set; after
  `setCamera`, the next update builds it.
- `CameralessUpdate.UIPolygonWaitsForACamera` **(red)**: the same for `PolygonComponent` with `automaticFlipY`.

### 10.2 MCP tests (`tools/mcp/tests/`, `CI = True`, own scratch project)

`tools/mcp/tests/test_scene_integrity.py` (new):
- `test_duplicate_project_entry_loads_once` **(red)**: write `project.yaml` listing `a.scene` twice, open; `list_scenes`
  has one scene with that path; the Console has the warning; `save_project` writes it once.
- `test_copied_scene_gets_its_own_id_and_name` **(red)**: `move_asset copy=true a.scene → b.scene`; the file's root
  `id` differs from a's, `name` is "b"; `copied_scenes` reports it; `open_scene b.scene` gives a distinct id; undo
  then redo gives the same id and name; copying a folder with two scenes gives two new ids; a replace-in-place copy
  keeps the target's id.
- `test_id_clash_on_load_is_stable` **(red)**: byte-copy `a.scene` to `c.scene` outside the editor, `open_scene
  c.scene`: id differs from a's, c is modified, the warning names both; close and reopen the project twice without
  saving c: the same replacement id each time (today random); after saving c the file holds it.
- `test_outside_scene` **(red for the old-project half: today saved in place silently; for open: today added)**:
  `open_scene` of a scene file outside the project fails with the copy-it-in message; a project whose `project.yaml`
  lists `../shared/s.scene` opens, warns, `save_scene` writes it in place.
- `test_set_child_scene_closed_parent` **(red)**: closed parent → error naming open_scene; the parent's file is
  unchanged; during Play attach is refused, `loaded_inline` on a stopped parent works.
- `test_inline_child_beside_running_game` **(red: editor terminates today)**: scenes P (child C with a sprite,
  `automaticFlipY`) and Q; open both, play Q, `select_scene P`, `set_child_scene scene=P child=C loaded_inline=true`,
  run 30 frames (`play step` is for the played scene: wait on `get_play_state` frames); the editor answers, the session
  is still running, no exception in `read_console`, `screenshot view=scene` of P is not empty.
- `test_scene_view_follows_replaced_scene` **(red)**: Menu scene with a Lua script loading `Level` on start; Level
  has a red cube at the origin; play Menu, wait until `get_play_state` shows Level running; `set_scene_camera` to
  look at the origin; `pick_at` the Scene view centre returns Level's cube (runtime scene id); `get_play_state`
  `scene_view_scene_id` is Level's runtime id; `sample_view_color` at the centre is red-dominant. Pause, step, and
  pick again (covers the activation path of 4.8).
- `test_cameraless_3d_runtime_scene_warns_once`: a 3D scene without a camera as a child; play; one warning, no
  "Invalid camera entity" error.
- `test_stale_main_camera` **(red)**: remove the Camera component of the main camera GameObject; `list_scenes`
  `main_camera` is null; play: no "Invalid camera entity" error; undo restores it.
- `test_save_all_lists_untitled_scenes`: `save_scene all=true` with an untouched untitled scene lists it in
  `skipped` **(red)**.
- `test_entity_payload_targets_are_audited`: greps `editor/` for `AcceptDragDropPayload("entity"` and compares with the
  list of 4.4 (a new target fails until it is added to the list and to `EntityPayload` rules).

`tools/mcp/tests/test_scene_order.py` (extend):
- `test_move_transform_next_to_transformless_is_refused` **(red: reports Moved today)**: `move_entity Player
  after=Skybox` fails with the message; the scene file is byte-identical; the existing Skybox/Fog/Loose moves still
  pass.
- `test_multi_delete_undo_keeps_untransformed_places` **(red)**: `{Light, A, Sky, B}`, `delete_entities [Sky,
  Light]`, undo: `get_scene_tree` order and the saved file equal the original; and `{T1, T2, S1, S2}` deleting
  `[S2, S1, T1]` keeps S1 as the sky.
- `test_model_import_generates_parents_first` **(red)**: load a multi-node glTF fixture (the one of
  `test_project_save.py::test_model_load_undo_keeps_parent`), save; in the generated scene source, every node's
  Transform block comes after its parent's.

UI tests (Xvfb harness, `ui_probe: True`, xdotool as `test_animator_window.py`), `tools/mcp/tests/test_scene_integrity_ui.py`:
- `test_save_dialog_refuses_another_scenes_file` **(red)**: scenes A (saved) and untitled B; `run_command file.save`
  on B; type `A` into `sceneSave.name`; `sceneSave.save` is disabled and `sceneSave.problem` names A; type `../x`:
  disabled; `respond_dialog cancel`; `a.scene` unchanged (sha1).
- `test_save_all_asks_for_untitled_scenes` **(red)**: untouched untitled scene, `run_command file.saveAll`;
  `respond_dialog` reports a `save_dialog`; cancel; no file written.
- `test_project_switch_closes_save_dialogs` **(red)**: open the untitled scene's dialog, `open_project` another
  scratch project; after the switch no dialog is open and `editor_status` shows no save in progress.
- `test_child_scene_drag_is_reference_only` **(red)**: P with inline child C; drag C's GameObject (same numeric id as
  a P GameObject) onto that P row: P's tree unchanged and the tooltip shows; drag it onto the Project window: no
  `.prefab` file appears.
- `test_entity_picker_lists_related_scenes` **(red)**: a script property on a C GameObject; open its picker; the probe
  lists P's GameObjects under "P (parent)".
- `test_escape_releases_captured_cursor` **(red)**: a game whose script sets `Engine.mouseMode = MouseMode.CAPTURED`
  (`docs/mcp/guide/input.md`); click the Game view; `get_play_state` `cursor.mode` captured,
  `released_to_editor` false; press Escape: `released_to_editor` true and the game's Lua saw the Escape key (a global
  it sets); click the Game view: false again; `run_command playmode.releaseCursor` releases too.

### 10.3 Gates

`tools/check-changed` on the branch; then `tools/mcp/gen_tool_docs.py`, `tools/mcp/tool_budget.py --check
tools/mcp/budget.json`; `tools/check-cxx-subset.py`. The full suite at landing per AGENTS.md.

## 11. Acceptance criteria

1. No route writes a scene over another project scene's file: dialog (disabled with the reason), Save All, MCP,
   `saveSceneFile` (refusal with alert); case variants are caught on case-insensitive volumes
   (`SceneFileIdentity.FindOther...`, `test_save_dialog_refuses_another_scenes_file`).
2. `project.yaml` never lists a file twice after a save, and a duplicate entry loads one scene
   (`StreamProjectScenes.EncodeListsEachSceneFileOnce`, `test_duplicate_project_entry_loads_once`).
3. A copied scene has a new id and a unique name, stable across undo/redo; a replace-in-place copy keeps the target's
   id; `move_asset` reports them (`test_copied_scene_gets_its_own_id_and_name`).
4. An id clash at load gives the same replacement id on every load, marks the loaded scene modified and names both
   files (`test_id_clash_on_load_is_stable`).
5. The Save dialog refuses names that leave the folder or are invalid on Windows; a failed dialog save leaves the
   scene's path unchanged; `open_scene` of an outside file is refused; an old project's outside scene loads and saves
   in place with a warning (`SceneSaveDialog.NameProblems`, `test_outside_scene`).
6. Every `entity` drop target applies the payload's scene: same-scene targets refuse child-scene GameObjects with a
   tooltip, the Project window refuses them, GameObject reference fields accept related scenes only; the target list
   is audited by a test (`test_child_scene_drag_is_reference_only`, `test_entity_payload_targets_are_audited`).
7. The picker lists related scenes' GameObjects grouped by scene; a script drop links every instance in related scenes
   (`test_entity_picker_lists_related_scenes`, `SceneFileIdentity.RelatedScenes...`).
8. Entity drags carry no YAML; a Project window drop still makes the same prefab as before (existing prefab tests
   green).
9. After a model import, create with children and duplicate, the tracked list is Transform-ordered; generated scene
   code creates parents first; the header reads "scene source 4" (`GeneratedScenes.ParentsAreCreatedBeforeChildren`,
   `test_model_import_generates_parents_first`).
10. Loading a child inline beside a running game does not crash and its contents draw; no camera-dependent engine
    path throws without a camera (`test_inline_child_beside_running_game`, `CameralessUpdate.*`).
11. After the game loads another scene, the Scene view draws that scene from the editor camera and `pick_at` selects
    its GameObjects, also after Pause and Step; the Game view is unchanged (`test_scene_view_follows_replaced_scene`;
    a screenshot of before/after kept with the evidence).
12. Esc releases a captured cursor and still reaches the game; a click on the Game view captures it again;
    `playmode.releaseCursor` does the same (`test_escape_releases_captured_cursor`).
13. `set_child_scene` on a closed parent fails with the open_scene hint and changes nothing
    (`test_set_child_scene_closed_parent`).
14. Save All asks for each never-saved scene after the modified ones; Cancel stops the chain and reports false; Ctrl+S
    on an untitled scene opens the dialog; the first saved scene becomes the start scene when none is set
    (`test_save_all_asks_for_untitled_scenes`, `test_save_all_lists_untitled_scenes`).
15. Save dialogs run exactly one of their callbacks once and keep no closure afterwards; `close()` acts as Cancel; a
    project switch closes them and answers their callers with false (`SceneSaveDialog.Close...`,
    `test_project_switch_closes_save_dialogs`).
16. Every scene-save failure path alerts with the reason in the editor and returns the reason to MCP callers (reviewed
    path by path against 4.13; one MCP check: a save into a path whose folder is a file fails with the reason).
17. Ctrl+S or Ctrl+Shift+S during Play shows "Stop Play to save ..." in the status bar
    (`ShortcutDispatcher.ADisabledCommandsChordReportsWhy`).
18. A removed Camera no longer counts as the main camera; Play of a cameraless 3D scene logs one warning and no error
    (`test_stale_main_camera`, `test_cameraless_3d_runtime_scene_warns_once`).
19. `move_entity` refuses a Transform GameObject next to a Transform-less anchor; the existing order tests pass
    (`test_scene_order.py`).
20. Multi-delete undo restores the exact list order including Transform-less GameObjects and the active sky
    (`test_multi_delete_undo_keeps_untransformed_places`).
21. Docs updated: `docs/data-safety.md` (section "Scene files and identity": one file per scene, copies, clashes,
    outside scenes, Save All), `docs/mcp/guide/data-safety.md` (the same for agents, `move_asset copy`,
    `save_scene all` skipped), `docs/editor/scene-view.md` (Scene view after a scene load; Esc releases the cursor),
    `docs/mcp/guide/scene-view.md` (`scene_view_scene_id`, `cursor`), `docs/editor/keyboard-shortcuts.md` (Esc in the
    Game view, Release Game Cursor), `docs/mcp/guide/ecs.md` (the `move_entity` anchor rule); `docs/mcp/TOOLS.md` regenerated; the agent guide digest is unchanged (no budget used).

## 12. Files likely touched

- Core save and identity: `editor/Project.{h,cpp}`, `editor/project/SceneFileIdentity.{h,cpp}` (new),
  `editor/project/ProjectAssets.cpp` (only if `prepareDocumentIdentity` needs the case key), `editor/Stream.cpp`
  (encode/decode project), `editor/util/AssetDatabase.{h,cpp}` (`isCaseInsensitive`), `editor/asset/AssetOps.{h,cpp}`,
  `editor/command/type/CopyFileCmd.{h,cpp}`.
- Dialogs and save flows: `editor/App.{h,cpp}` (save functions, `processNextSaveDialog`, `cancelSaveDialogs`,
  `processProjectChange`, `exit`, `renderRunningScene`, `renderSceneBesideGame`), `editor/EditorHost.{h,cpp}`,
  `editor/AppCommands.cpp`, `editor/window/dialog/SceneSaveDialog.{h,cpp}`, `ProjectSaveDialog.{h,cpp}`,
  `editor/shortcuts/ShortcutDispatcher.{h,cpp}`, `editor/shortcuts/DefaultCommands.cpp`.
- Payloads and references: `editor/util/EntityPayload.{h,cpp}`, `editor/window/HierarchyWindow.{h,cpp}`,
  `editor/window/ProjectWindow.{h,cpp}` (`saveEntityFile` signature, drop), `editor/window/InspectorWindow.cpp`,
  `InspectorParticles.cpp`, `AnimationWindow.cpp`, `BookmarksWindow.cpp`, `PoseAnimationWindow.cpp`,
  `VertexAnimationBakerWindow.cpp`, `SpriteNormalsWindow.cpp`, `editor/window/CodeEditor.cpp`,
  `editor/project/ProjectPrefabEdit.cpp` (if `createPrefab` needs the scene id).
- Order: `editor/command/type/ModelLoadCmd.cpp`, `CreateEntityCmd.cpp`, `DuplicateEntityCmd.cpp`,
  `DeleteEntityCmd.{h,cpp}`, `MoveEntityOrderCmd.cpp`, `editor/util/ProjectUtils.{h,cpp}`, `editor/Factory.cpp`,
  `editor/cook/SceneCooker.cpp`.
- Play views: `editor/render/SceneRender.{h,cpp}`, `editor/render/ViewCameraMirror.{h,cpp}` (new),
  `editor/PrefabStage.cpp`, `editor/window/PlayInput.{h,cpp}`, `editor/EditorFrame.cpp`, `editor/backend/BackendMac.mm`,
  `editor/picking/ScenePicker.cpp` (if it reads `pickable`).
- Engine: `engine/core/subsystem/MeshSystem.cpp`, `engine/core/subsystem/UISystem.cpp`,
  `engine/core/subsystem/RenderSystem.cpp` (one line), `engine/core/Scene.cpp` (one line).
- MCP: `editor/mcp/tools/SceneTools.cpp`, `EntityTools.cpp`, `AssetTools.cpp`, `PlayTools.cpp`; `docs/mcp/TOOLS.md`.
- Tests: section 10 files. Docs: section 11.21.

## 13. Cross-feature dependencies

- `project-window`: shares `editor/asset/AssetOps` and `CopyFileCmd` (Duplicate, numbered names, paste targets) and
  `ProjectWindow.cpp` (the entity drop). Decision: scene ids and names are assigned inside `AssetOps::copy`
  (`CopyIdentity`), so Duplicate and Paste get them for free; whoever lands second merges the `copyGuids` →
  `CopyIdentity` change. The numbered-name helper of `project-window` ("Level1 1") is the one decision 5 names after.
- `export-pipeline`: Factory/Generator (scene source header 4, parents first; they also touch shader keys and
  never-saved scene sources), the start scene (`ensureStartScene` vs their loading/start scene rules: one function,
  agree on it), save failures that fail an export.
- `unicode-text`: path keys and case folding of UTF-8 names, forward slashes in saved paths, Windows-invalid names in
  the Save dialog; `SceneFileIdentity` name quoting.
- `agent-authoring`: "MCP edits held during a human save" touches the save state (`saveDialogInProgress`,
  `CommandHistory::beforeMutation`, App save functions): coordinate the dialog-callback change.
- `platform-lifecycle`: `EditorFrame.cpp`, backends (`setMouseControlSuspended`, minimize handling).
- `ui-components` (UISystem) and `sprite-batching`/`mesh-memory`/`draw-perf` (MeshSystem): the camera guards are
  small hunks in files they rewrite; land them early or reapply.
- `scripting` and `code-editor-git`: `InspectorWindow.cpp` (Add Script, picker) and `CodeEditor.cpp` (drop handling
  next to their gutter work).
- `runtime-core`: none functional; both touch Play stop paths in `Project.cpp` (mirror release at Stop).
- `render-correctness`: `SceneRender.cpp` (orthographic camera gizmo) near `drawSceneView`.

Hot files for merge planning: `editor/Project.cpp`, `editor/App.cpp`, `editor/Stream.cpp`,
`editor/window/HierarchyWindow.cpp`, `editor/window/InspectorWindow.cpp`, `editor/window/ProjectWindow.cpp`,
`editor/window/CodeEditor.cpp`, `editor/render/SceneRender.cpp`, `editor/asset/AssetOps.cpp`,
`editor/command/type/CopyFileCmd.cpp`, `editor/Factory.cpp`, `engine/core/subsystem/MeshSystem.cpp`,
`engine/core/subsystem/UISystem.cpp`, `editor/mcp/tools/SceneTools.cpp`.

## Deviations

Recorded during the implementation (branch `dc/scene-integrity`).

1. **Multi-delete undo (4.17) was mostly fixed already.** `DeleteEntityCmd` keeps `trackedBeforeDelete`
   and puts the whole tracked list back on undo (an older fix the comparison's trace missed), so the
   list order of `{Light, A, Sky, B}` comes back. What remained wrong is the Skybox component array: the
   skies are restored in restore order, so the scene's sky (the first Skybox) could swap. The undo now
   sorts the Skybox array by the restored list; no `keepUntransformedPlaces` pass is needed.
2. **`AssetOps::copy` keeps its `copyGuids` parameter and gains `copyScenes`** (a map of `SceneCopy {id,
   name}` by copy path) instead of a `CopyIdentity` struct replacing it: the same redo behaviour with
   less churn in the signature `project-window` also calls. `AssetOps::Result::copiedScenes` reports what
   each copy got.
3. **`move_entity parent/index` counts `index` among the siblings with a Transform** when the moved
   GameObject has one, so `parent: null, index: 0` never anchors on a Skybox (which the command now
   refuses). Documented in `docs/mcp/guide/ecs.md`.
4. **No `ProjectUtils::isTransformOrdered` and no debug-only warning in `writeSceneSource`**: the
   generator and the cooker sort their own copy of the list, so an unsorted list can no longer reach an
   export, and the tests compare the list with its sorted copy directly.
5. **Inspector "Main Camera" field**: there is none (the main camera is set from the Hierarchy's context
   menu and MCP); `list_scenes` reports `main_camera` null when the Camera component is gone, and
   `getSceneCamera` ignores it.
6. **Hierarchy child-scene menu items for closed parents**: the Hierarchy only shows open scenes and
   inline-loaded child scenes, whose data is loaded, so the closed-parent refusal lives in
   `set_child_scene` alone.
7. **The picker UI test** is replaced by the unit test of `Project::getRelatedScenes` and the drop rules
   (`SceneIntegrityTest.RelatedScenesAndDropRules`): driving the picker popup of a script property with
   real input would test ImGui more than the list it is built from.
8. **Agent guide**: no digest line (lead decision: only agent-authoring restructures the guide). The
   changed tool texts (`set_child_scene`'s error and description, `move_entity`'s refusal) have eval cases
   `bad_child_scene_on_closed_parent` and `bad_move_next_to_skybox` in `tools/mcp/eval/cases.py`.
9. **Escape** follows the lead decision across specs: the focused Game view delivers Escape to the game
   (runtime-core) and the same press releases a captured or confined cursor (`PlayInput::forward`).
10. **The project-switch test** is not an MCP test: since S69, every MCP edit (`open_project` included) is
    refused while a modal dialog is open, so only a native menu (macOS) can switch projects under an open
    Save dialog. `App::cancelSaveDialogs` stays (run before every switch and at exit); its parts, the
    dialogs' `abandon()` and close-as-Cancel, are unit-tested.
11. **The Save dialog tests use `respond_dialog`** (S69's EditorModal fields `name` and `folder`)
    instead of real input: the refusal reaches agents as `EditorModal::error`, so it is the same check.

