# Prefabs: contracts

Developer notes for the prefab system (`editor/project/ProjectPrefab*.cpp`,
`editor/StreamPrefab.cpp`, `editor/util/Prefab.*`, `editor/util/AssetDatabase.*`).
Sections 1-5: files, identity, GUIDs, Missing Prefab. Sections 6-11: the real root,
the override model v2 and its APIs. Section 12: editing instances the Unity way (edits
are overrides, Apply pushes them), Play mode, and the Inspector's override UI. Sections 13-15:
Replace Prefab, the Hierarchy rows, the Scene view drop, thumbnails and the Prefabs dialog.
Section 16: Prefab Mode (`editor/PrefabStage.*`). Section 17: nested prefabs in assets,
stripped objects (references into nested prefabs), the layered import and the baked export
process. Section 19: Prefab Variants (`editor/project/ProjectPrefabVariant.cpp`).
`tools/prefab-fixtures.py` makes test projects, `jasperengine-editor scene-dump --project
<dir> [--save] [--ops <file>]` dumps what loading them gives (and runs prefab operations
headless). Section 18: prefabs at runtime (Instantiate, Destroy, Prefab script properties,
the build's prefab list).
Read this before changing how prefabs are identified, stored or loaded.

## 1. Prefab file format (version 3)

```yaml
type: Prefab
version: 3
guid: 9c1d0f3e5b7a42c8a1e06d2f4b8c7a19   # the asset's GUID, 32 lowercase hex digits
nextEntity: 1008                          # the id the next new GameObject of the prefab takes
root: 1001                                # the prefab's root GameObject (section 6)
members:                                  # the root first; GameObjects without a Transform may follow
  - type: Entity
    entity: 1001                          # registry entity id = the GameObject's file id
    name: Crate
    components: {...}
    children:
      - type: Entity
        entity: 1004                      # a nested prefab instance: its root (its GameObjects
        name: Wheel                       # are the nested prefab's), its components as this
        components: {prefab: {name, path, guid}, transform: {...}, ...}   # prefab has them,
        prefabInstance: {...}             # what this prefab overrides of it (section 7)
        children:                         # and the GameObjects this prefab adds to it (17)
          - type: Entity
            entity: 1010
            addedTo: 1012                 # below that nested GameObject (a stripped id)
            childIndex: 1                 # at that place among its children
            name: Flag
            components: {...}
stripped:                                 # GameObjects of nested instances it refers to (17)
  - entity: 1012
    source: [1004, 2003]                  # nested root here, ..., the GameObject's id inside
```

- The header is always written (`Project::encodePrefabNode`). `stripped` only when the
  prefab has some.
- Version 2 had the header but the old wrapper-root model; version 1 (no header) is
  the root entity node itself (`type: Entity ...`) or `{type: Prefab, members: [...]}`.
  The project-load scan inserts a `guid:` line into version 1 files and changes nothing
  else (`AssetDatabase::writeGuid`). `Prefab::readHeader` reads every form.
- **Upgrade (one-shot, on load)**: `Prefab::loadRegistry` reads a version 1/2 file into the
  real-root model in memory (section 6): its top-level GameObjects go below a new root
  GameObject equivalent to the old per-instance wrapper (named after the file stem, a
  default Transform when any top-level GameObject had one) which takes the id
  `nextEntity` (deterministic: the same on every load until written). Its nested roots get
  `PrefabInstanceData::legacy` (their own values were wrappers' too, section 9).
  `Prefab::needsUpgrade` is set: the file is written as version 3 the next time a scene
  using it is saved (`saveSceneFile`), even with no change; the scene is not marked
  modified by the upgrade.

## 2. Identity contract: registry ids are file ids, never reused

A prefab registry entity id is the persistent per-object id of a prefab GameObject
(Unity's fileID). Scenes, closed ones included, key everything by it:
the `prefabInstance` block's `overrides[].registryEntity`, `removedGameObjects`,
`addedGameObjects[].parentRegistryEntity`, `memberIds`, `nested[].prefabRootRegistryEntity`
(and the older keys `prefabOverrides`, `prefabRemovedGameObjects`, `prefabLocalEntities`).
So an id, once handed out, is never given to another GameObject of that prefab.

- `Prefab::loadRegistry(node)` is the only way a prefab file becomes a registry. It
  decodes the file into `registry`/`registryEntities` and raises the registry's id counter
  (`EntityRegistry::setLastEntity`) to `max(nextEntity - 1, highest id in the file,
  counter before the reload)`. Used by `importPrefab`, `loadStandalonePrefab`
  (`setStandalonePrefabs`), `repackPrefabInstances`, `ImportPrefabCmd` (temporary
  registry) and the shader variant collector. A new load site must use it too.
- New GameObjects of a prefab take `registry->createUserEntity()`, which counts up from
  that counter (monotonic, so above the high-water mark). Ids are only re-created with
  `recreateEntity` for the GameObject that had them (undo/redo, removal reverts).
- Deleting never lowers the counter; `Prefab::getNextEntity()` (written as `nextEntity`)
  is the counter + 1.
- Not covered for legacy files: an id deleted before the upgrade cannot be known, so the
  first version 2 save starts from `highest id + 1`.

## 3. Asset GUIDs: `editor/util/AssetDatabase.{h,cpp}`

`Project::getAssetDatabase()` maps GUID <-> project-relative path for every asset of the
project: the general contract (all types, `.meta` sidecars, former GUIDs, sub-assets, the
dependency index, Missing Assets, `AssetOps`) is `docs/specs/S04-asset-identity.md`; this
section is what prefabs add to it. A prefab's GUID lives in the file (top-level `guid:` key,
and `formerGuids:` right after it); the database is the index, rebuilt by a scan when the
project opens, and cached in `.jasperengine/assetdb/index.json` (a deletable cache).

- **Scan on project load** (`AssetDatabase::open`, before any scene or prefab is read):
  every tracked file gets a GUID. A legacy file gets `legacyGuid(path)` (SHA-1 of its
  relative path: every machine upgrading the same project writes the same GUID, and a
  reference written before GUIDs, by path only, still finds the file after it moves). A
  GUID found in two files stays with the file the cache says had it, else the older one;
  the other file gets a new random GUID written into it.
- **External changes** come from the file watcher (`Project::applyFileChanges`, through the
  hot reload, docs/hot-reload.md). A batch asks for a walk of the disk (`AssetDatabase::scanDisk`
  on a Background job, applied by `refresh(scan)` on the main thread; no poll), so S04's rules
  decide what happened: a move that carried its meta or its in-file GUID keeps the GUID, a file
  renamed without its meta adopts the orphan meta (R5), a copy gets a new GUID, an edit is
  Modified. While a scene plays they wait, and a full **Refresh**
  (`Project::refreshAssetDatabase(true)`: Assets > Refresh, `sync_files` full, a tool that
  answers from the database) runs when Play stops. The refresh: A new file with an unknown
  GUID is added; with the GUID of a file that is gone, it is that file moved; with the GUID of
  a file that is still there, it is a copy and gets a new GUID. A gone file is removed. A file
  edited so that its GUID line disappeared gets it back.
- **A read that finds a tracked prefab file gone** (`loadPrefabAsset`, the import making a
  Missing Prefab instance from that, before a walk noticed) makes the database forget it at once
  (`forgetGonePrefabFile`); the removal is applied by the next refresh, unless the file is back by
  then (the same GUID at the same path: nothing went). So the file put back is new to the
  database, an Added that relinks the instance, whether or not a walk fell between the delete and
  the put-back (one watcher batch for both, a file moved out and back with its stamp).
- **Editor file operations** keep it up to date, all through `AssetOps`
  (`editor/asset/AssetOps.cpp`: `RenameFileCmd`, `CopyFileCmd`, `DeleteFileCmd`, MCP
  `move_asset` / `delete_asset`), which hands the database's changes to
  `Project::onAssetFilesChanged` (`applyAssetChanges` below for the prefab files):
  - rename / move -> `AssetDatabase::moveAssets` (GUIDs kept; a file renamed away from
    `.prefab` is removed, one renamed to it is imported),
  - delete (to the trash) -> `removeAssets`; undo -> `importAssets` (the file keeps its GUID),
  - copy / paste / import from outside -> `importAssets(path, copy=true, &copyGuids)`: a
    copy gets a new GUID unless its GUID is unknown (an import from another project keeps
    it), and a redo gives it the same GUID again; undo -> `removeAssets`,
  - Create Prefab (`createPrefab`, redo keeps the GUID via `CreatePrefabCmd`) ->
    `registerAsset`; every `savePrefabToDisk` re-registers the file.
- Changes reach the rest of the editor through `Project::applyAssetChanges`: Moved ->
  `remapPrefabPathsInMemory` (prefab assets, scene and prefab `PrefabComponent`s, every
  scene's prefab list, the standalone list), Removed -> Missing Prefab conversion
  (section 4), Added -> `relinkMissingPrefabs`. Closed scenes whose prefab list is
  affected get their generated source written again (`invalidateClosedSceneSources`).

### References

| Where | Stored as |
|---|---|
| prefab instance root, nested prefab root inside a prefab file | `components.prefab: {name, path, guid}` (`PrefabComponent::guid`) |
| scene file `prefabs:` (the build's prefab list) | `[{path, guid}]` (plain path strings read too); Missing Prefabs stay listed, the build skips files that are not there |
| `project.yaml` `standalonePrefabs:` | `[{path, guid}]` (plain path strings read too) |
| `prefabInstance.nested[]` (and the older nested entries of `prefabOverrides`, `prefabLocalEntities`, `prefabRemovedGameObjects`) | `prefabRootRegistryEntity` names the nested root (`prefabPath` is only informative; entries without a registry entity, from the oldest files, match by path) |

The path is a hint. `Project::resolvePrefabPath(guid, pathHint)`:
1. a GUID resolves to its file, or to nothing (Missing, even if some file sits at the
   path: Unity semantics); a file at the hint carrying that GUID but not indexed yet is
   indexed on the spot;
2. no GUID (written before GUIDs): the file whose GUID is `legacyGuid(pathHint)`, else
   the file at the path.

`Project::resolvePrefabReference(PrefabComponent&)` applies it in place (updates `path`,
fills a missing `guid`). `importPrefab` resolves the root's `PrefabComponent` before
anything else; when a scene's own reference resolved to a new path the loaded scene is
marked modified (`SceneProject::prefabRefsChanged`). Paths inside prefab files are fixed in
memory only (`resolveRegistryPrefabRefs`); the files get the new hint when saved anyway.
Renaming or moving a prefab rewrites nothing on disk but the project's standalone list.

Exported games: generated code sets `PrefabComponent::{name, path, guid}` on spawned
roots, and PrefabManager registers every prefab of the build with its id, name and GUID;
Prefab script properties find theirs by GUID (section 18).

## 4. Missing Prefab

A prefab instance root whose asset cannot be found (no file has its GUID, or the file
cannot be read) is a Missing Prefab instance: the root stays with its own components, its
GameObjects are not there, and everything the scene stored for the instance is kept and
written back unchanged.

- Data: `SceneProject::missingPrefabs[root]` = `MissingPrefabInstance{guid, path, data,
  memberIds, mainCamera}`. `data` holds the instance keys relative to that root:
  `prefabInstance` (or the older `prefabOverrides`, `prefabLocalEntities`,
  `prefabRemovedGameObjects`, read by `Stream::decodePrefabInstanceData`) and, for a root
  read from a scene file, every other key of its node except the GameObject keys (`type`,
  `entity`, `name`, `active`, `tag`, `layer`, `components`, `children`;
  `Stream::isGameObjectKey`) - keys a newer format adds are kept without knowing them.
- Made by: `importPrefab` (resolution or load failure, nested ones included),
  `Stream::decodePrefabInstance` (replaces it with the root's whole node), and
  `convertPrefabInstancesToMissing` (the asset was deleted: each live instance in the
  loaded scenes is encoded with `Stream::encodePrefabInstanceData`, its GameObjects
  (members, added GameObjects) destroyed, its root kept, the local ids recorded).
- Written by: `Stream::encodeMissingPrefab` (any plain-encoded root: scene save,
  copy/paste, duplicate, delete-undo recovery: the data keys go back next to it), and
  `encodePrefabInstance` for a missing prefab nested in a live instance (its whole block as
  the nested entry, at any depth).
- Read by: `Project::getMissingPrefab(sceneId, entity)` (validated: the entity exists and
  still has a `PrefabComponent` with the record's GUID, and is not an instance again),
  `isMissingPrefab`. UI: red row and tooltip in the Hierarchy, "Missing Prefab" bar with
  the last known path in the Inspector.
- Relink: `relinkMissingPrefabs` (after a file appears: undo of a delete, a copy, a
  refresh) re-imports each record whose reference resolves again with its data, the
  members taking back their old local ids (`importPrefab(..., preferredIds)`), so an undo
  of the delete gives back the same entities. Loading a scene relinks by itself.
- Deleting a `.prefab` no longer destroys its instances: they become Missing Prefab
  instances, and a standalone prefab stays listed (a project load drops a listed prefab
  whose file is gone).
- Children added in the scene below a missing root are saved as its `children` (top-level
  root) or as added GameObjects of its nested entry (nested root) and come back as added
  GameObjects of the instance once relinked.
- Export: a missing top-level root is exported as a plain GameObject; a missing nested
  prefab is built as its root alone (`Factory::createPrefab` skips its build call).

## 5. Missing Prefab integration

- New per-instance keys written next to a top-level instance root are preserved for
  Missing Prefabs automatically. A missing prefab *nested* in a live instance keeps its
  whole `prefabInstance` block (recursive), see `encodeLiveInstance` in StreamPrefab.cpp.
- `importPrefab(..., instanceData, entityRemap, preferredIds, assetLayers)`: the root's
  `PrefabComponent` decides the prefab; `preferredIds` (`PrefabMemberIds`: instance root ->
  registry id -> local id) is the hook for persisted member ids, over the data's own
  `memberIds`.
- Anything creating a prefab instance root must set `PrefabComponent::guid`
  (`Project::getPrefabGuid(path)`).

## 6. Real single root (Unity's model)

A prefab asset has exactly one root GameObject (`Prefab::root`, `root:` in the file), part
of the asset: its name, components and children. An instance's root **is** that GameObject
instantiated (`Prefab::Instance::rootEntity` <-> `rootRegistryEntity`). `Instance::members`
are the instance's other GameObjects (the root is not one; `Instance::getRegistryEntity` /
`getLocalEntity` and `Prefab::getObjectRegistryEntity` / `getLocalEntity` include it,
`Prefab::getRegistryEntity(sceneId, entity)` does not). GameObjects without a Transform (an
action's virtual children) can sit next to the root at top level, as members.

**Default overrides** (Unity's): an instance root's name, Transform `position` and
`rotation`, and its RectTransform placement (`width height anchorPoint* anchorOffset*
positionOffset pivot anchorPreset usingAnchors`) are always the instance's own: never
listed as overrides, never applied, never propagated (`Project::isPrefabRootDefaultOverride`).
Everything else on the root is asset data, overridable like on any GameObject.

- The instance root's node in a scene file is written in full (name, state, components,
  PrefabComponent): its values for the default overrides and for its overrides are read
  from there, and a Missing Prefab root keeps all of it.
- **Import** (`importPrefab`): the prefab root's state and components are applied to the
  instance root (a component it lacks is added, the PrefabComponent stays), then it takes
  back from its own node the default overrides and its overridden properties, keeps its
  added components and loses its removed ones; components it had that are neither the
  prefab root's nor recorded as added are removed (the asset dropped them). Its name is
  kept (set to the prefab root's when empty). The prefab's other GameObjects are created
  below it.
- **Create Prefab** (`createPrefab`, `CreatePrefabCmd`): the selected GameObject becomes the
  prefab's root and the instance's root (no wrapper): it gets the PrefabComponent, its
  descendants become the members. Several selected roots make one prefab each (Unity's way
  when several GameObjects are dragged into the Project window, also for the Hierarchy's
  "Create Prefab" on a multi-selection): `ProjectWindow::saveEntityFile` splits the
  selection, a GameObject without a Transform (an action) going with the root it acts on;
  `CreatePrefabCmd` creates them as one undo step. A selected prefab instance root is
  unpacked first and becomes an original prefab (Unity's "Original Prefab"; nested
  instances inside stay nested; undo repacks it). A GameObject of an instance (a member)
  is refused (unpack the instance first). Prefab instances nested in the selection keep
  everything the scene had for them, as the new prefab's: their overrides at every depth
  become its asset-level data, the GameObjects added to them its added GameObjects, the
  references into them its stripped objects (section 17, `moveNestedInstancesToAsset`;
  undo puts their scene data back). The same happens when a prefab instance is put into a
  prefab (`addEntityToPrefab`, `AddEntityToPrefabCmd` undoes it the same way).
- **Drop a prefab** (`ImportPrefabCmd`): the new root has only its PrefabComponent and the
  prefab root's name (made unique); the import gives it the prefab root's components, at the
  prefab root's position (Unity); dropped on a parent it keeps that local placement. Dropped
  on a GameObject of an instance it is a GameObject added to that instance (Unity); in
  Prefab Mode (`isEditingPrefabAsset`) it is nested in that instance's prefab.
- **Editing the root**: like any GameObject of the instance (section 12): an edit records
  an override, but for the default overrides above, which stay the instance's own.
- **Upgrade of old instances**: an instance written before (no `prefabInstance` block) has
  a root whose components and state were the per-instance wrapper's: `importPrefab` diffs
  them against the prefab root (`diffAgainstRoot`): a component the root lacks is removed,
  one the prefab root lacks is added, Transform/RectTransform by property (scale, ...),
  other differing components whole, active/tag/layer as state overrides. Nothing changes
  visibly; the scene is written in the new format on its next save.

## 7. Override model v2

### In memory

`Prefab::Instance::overrides`: local entity (root or member) -> `PrefabObjectOverrides`:

| field | meaning |
|---|---|
| `properties` | `map<ComponentType, set<string>>`: overridden properties by the Catalog property names PropertyCmd uses (`position`, `color`, `submeshes[0].material`, `tiles[3].name`). `"*"` (`WHOLE_COMPONENT`) = every property of the component |
| `state` | bits `StateName`, `StateActive`, `StateTag`, `StateLayer` |
| `addedComponents` | components the instance added to the GameObject |
| `removedComponents` | components of the prefab GameObject the instance removed |

- **Granularity**: one Catalog property. An indexed name is one element (applied when the
  element exists); compound properties (`scripts`, `actions`, `foliageLayers`,
  `shaderUniforms`, materials) are whole properties (Catalog::copyPropertyValue copies them
  whole; a Script property copies the whole ScriptComponent). Array sizes are not property
  overrides: a change of size goes through a whole-component change (`"*"`). Files written
  before property overrides, an edit that rewrites a whole component (a mesh changed, a
  model loaded or cleared) and a reference pointing outside the instance record `"*"`.
- **Which level owns an override** (`Project::findPrefabObject`, `PrefabObjectRef`): the
  instance the GameObject is a member of, else the instance it is the root of. So a
  nested instance root's overrides belong to the instance holding it (where it is a
  member); only a top-level instance root owns overrides as a root.
- Removed GameObjects: `Instance::removed` (unchanged); added GameObjects: the scene
  GameObjects below the root or a member that are not members (unchanged).
- `Prefab::nestedInstances`: per nested root registry entity, the **asset-level** data of
  that nested instance (what this prefab overrides of it): a `PrefabInstanceData`
  (section 8), written as the nested root's `prefabInstance` block in the prefab file. Its
  values' references are in **this prefab's** ids (its registry entities and stripped
  ids, section 17), at any depth of `nested`.

### `PrefabInstanceData` (the serialized form, `editor/util/Prefab.h`)

Parsed by `Stream::decodePrefabInstanceData(node)` from a node holding a
`prefabInstance` block (or the older keys), written by `Stream::encodePrefabInstanceBlock`.
`objects` (registry entity -> overrides + YAML `values`), `removedGameObjects`,
`addedGameObjects` (entity nodes + `parentRegistryEntity` + `childIndex`), `memberIds`
(registry -> local), `nested` (the same per nested instance, by `nestedRoot` = the nested
root's registry entity in this prefab; to any depth), `legacy`.

## 8. Scene file schema (`prefabInstance`)

```yaml
- type: Entity
  entity: 12                       # the instance root, written in full
  name: Crate (2)
  components:
    prefab: {name: Crate, path: prefabs/Crate.prefab, guid: 9c1d...}
    transform: {position: [3, 0, 0], ...}
  prefabInstance:
    version: 3
    memberIds: [[1002, 13], [1003, 14], [1004, 15]]   # registry entity -> local id, members
    overrides:
      - registryEntity: 1001       # the root: values are the root node's own (above)
        properties: {transform: [scale]}
        addedComponents: [light]
      - registryEntity: 1002
        properties: {sprite: [color], mesh: ['*']}
        state: [active, tag]       # name / active / tag / layer
        active: false              # the state values
        tag: Enemy
        components: {sprite: {...}, mesh: {...}}   # values: whole components (only the listed
                                                    # properties are read), added components'
        removedComponents: [light]
    removedGameObjects: [1003]
    addedGameObjects:
      - parentRegistryEntity: 1002 # a registry entity (1001 or 0 for the root)
        childIndex: 1              # among all the parent's children
        type: Entity
        entity: 20
        name: Flag
        components: {...}
    nested:                        # the instances nested in this one, recursively
      - prefabRootRegistryEntity: 1004
        prefabPath: prefabs/Wheel.prefab           # informative
        memberIds: [[2002, 16]]
        overrides: [...]
        removedGameObjects: [...]
        addedGameObjects: [...]
        nested: [...]
```

- `nested` entries hold the same keys to any depth; a Missing Prefab nested at any level
  keeps its block as loaded (GameObjects added below its root since go after its own).
- **Prefab files**: the nested root's node carries the same block (without `version`,
  `memberIds` and `addedGameObjects`): what that prefab overrides of the nested instance.
  The nested root's own values in the prefab registry play the root node's part (its
  default overrides and overridden properties are read from it). The GameObjects a prefab
  adds to a nested instance are its own registry entities, the nested root's `children`
  (section 17).
- A Missing Prefab record made by the import (a nested one, a cycle) holds
  `{prefabInstance: {..., legacyRoot: true}}` for data read from the older keys.
- **Legacy reader** (no `prefabInstance` key): `prefabOverrides` entries
  `{registryEntity, components}` -> every listed component `"*"`, its YAML the values;
  entries tagged `prefabPath`/`prefabRootRegistryEntity` go to that nested entry (the oldest,
  without the id, match by path); `prefabRemovedGameObjects` (own, tagged, and `nested`
  chains to any depth); `prefabLocalEntities` -> added GameObjects (`parentRegistryEntity`
  0 = the root, tagged ones to the nested entry); `legacy` set (the root's own values were
  the wrapper's, section 6). Old scenes load identically (verified by `scene-dump`).
- **Member ids**: members are created with their `memberIds` (where free), so references
  from scene GameObjects to instance members survive a reload. Without saved ids (older
  files, new members) a member takes its registry id when free and not one the scene file
  being read holds anywhere (`SceneProject::reservedEntityIds`, filled by
  `decodeSceneProjectEntities` from every id of the file, blocks included), else a new id
  above them. This fixes the old collision where a member grabbed an id a GameObject
  further down the file had, which then got another id and broke references to it.

## 9. Import: layers and validation

`importPrefab(sceneProject, entities, path, root, needSave, instanceData, entityRemap,
preferredIds, assetLayers)` builds the instance in two phases (section 17 has the details):

1. **The GameObjects**: the prefab root's components on the root, the registry's
   GameObjects below it (what any layer removes left out: `assetLayers` and `instanceData`
   removals), then each nested root imported (recursively, as a *deferred* import: GameObjects
   only), then the GameObjects this prefab adds to that nested instance. Then this level's
   references, its registry ids and stripped ids, become local; the nested roots keep their
   prefab's values.
2. **The values, in layers**: the prefab registry (phase 1); each prefab's asset-level data
   for the instances nested in it (`Prefab::nestedInstances`), applied by that prefab's own
   import once everything below it exists, inner prefabs first, so an outer prefab's
   override wins, its values' references made local through that prefab's map; then, by the
   outermost import only, `instanceData` (the scene's data, values in local ids) for the
   whole tree, the only layer recorded in `Instance::overrides` / `removed`, and the
   GameObjects the scene added to it (`decodeSceneAddedGameObjects`).

`assetLayers` (innermost first: the containing prefab's `nestedInstances[root]`, then the next
one's `findNested` chain) tell a nested import what the levels above remove. A nested root
imported on its own later (`instantiatePrefabBranch` of a reverted removal) is built as a
deferred import too, then its holder's data and the levels above are applied as above
(`applyHolderData`, `applyLayersAbove`); a relinked Missing Prefab nested in a live instance
comes back with that instance re-imported (`relinkMissingPrefabs`). Every entry is validated
against the GameObject it names (registry entity -> this instance's local entity): a
property or whole-component override needs the component on that GameObject, a removed
component the prefab GameObject's component, an added one only adds when absent (when the
prefab has it now, the instance's values become a `"*"` override); a GameObject the prefab
lost or a property that no longer exists is dropped and logged, never applied to whatever
else holds that id. A legacy nested root (prefab file older than v3) is converted once by
`nestedAssetLayer`: its values in the containing registry diffed against the nested prefab's
root become that prefab's asset-level overrides (written on its next save); the copies of
the nested prefab's GameObjects such files held below it are dropped when read.

**The Transform order.** Every GameObject the import makes below one already in the scene is
decoded by `Stream::decodeEntity`, which appends its Transform at the end of the array and then
moves its branch to the end of its parent's (`ProjectUtils::keepInParentBranch`): a re-import
(an apply, Add to Prefab and its undo, Replace Prefab, a relinked Missing Prefab, a reverted
removal) leaves every branch in one run, which the moves, the encoders and `placeAtChildIndex`
read; the save refuses a file that would still leave a GameObject out (`docs/data-safety.md`).
**The scene's added GameObjects** (`decodeSceneAddedGameObjects`) are decoded with the
import's held paths set aside: they are the scene's, so an instance of this prefab among them
(below a GameObject of this instance) is no cycle, and imports as the scene's own instances do.

## 10. APIs

On `Project` (`ProjectPrefabOverrideModel.cpp` unless noted); `sceneId, entity` is any
GameObject of an instance, resolved to its owning level.

| API | |
|---|---|
| `findPrefabObject(sceneId, entity, PrefabObjectRef&)` | owning level: path, prefab, instance, local/registry entity, isRoot |
| `isPrefabRootDefaultOverride(type, property)` (static) | Unity's default overrides of a root |
| `isPrefabInstanceRoot(sceneId, entity)` | a top-level or nested instance root |
| `isComponentOverridden`, `getOverriddenProperties` (`"*"` = whole), `getPrefabObjectOverrides` | queries |
| `getPrefabSourceValue(sceneId, entity, type, property, PrefabSourceValue&)` | the value without the instance's own override (prefab + containing prefabs' layers), in a scratch registry with a `PropertyData`, references in local ids |
| `getPrefabSourceComponent(...)` | the same, whole component YAML |
| `getPrefabApplyTargets(sceneId, entity)` | innermost first: for a nested instance root its own prefab, then the owning prefab, then each prefab holding that instance, outwards (apply-target choice; the first is Unity's "Apply to Prefab", the others "Apply as Override in Prefab") |
| `getPrefabSourceState(sceneId, entity, name, active, tag, layer)` | the source of name / active / tag / layer (comparison view, revert) |
| `canApplyPrefabOverrides(reason)`, `canApplyPrefabOverride(sceneId, entity, type, properties, target, reason)` | Apply allowed: not in Play mode, `target` one of the apply targets, no reference outside what `target` holds |
| `recordPropertyOverride` / `clearPropertyOverride` / `revertPropertyOverride` / `applyPropertyOverride(..., target)` | property overrides (empty list = whole component) |
| `recordStateOverride` / `revertStateOverride` / `applyStateOverride` | name/active/tag/layer bits |
| `recordAddedComponent` / `revertAddedComponent` / `applyAddedComponent` | added components |
| `recordRemovedComponent` / `revertRemovedComponent` / `applyRemovedComponent` | removed components |
| `refreshPrefabInstances(path, reloadFromDisk)` | re-imports every open instance (top-level and nested, all loaded scenes) from the current registry (or the file), keeping overrides, member ids, added/removed GameObjects, the selection and undo validity (same ids) |
| `reimportPrefabInstance(sceneProject, root, rootNode)` | rebuilds one outermost instance from a root node |
| `capturePrefabUndoState(sceneId, entity, targets)` / `restorePrefabUndoState` | undo snapshots: the outermost instance, and for targets the assets (and the prefabs nesting them) plus every instance using them |
| `PrefabOverrideCmd` (`command/type/`) | any primitive above as one undo step |
| `recordPrefabEdit`, `recordPrefabComponentEdit`, `recordPrefabStateEdit`, `recordPrefabComponentAdded` / `Removed` | instance edits (section 12) |
| `getPrefabEditOverrides` / `setPrefabEditOverrides` | a GameObject's overrides as they are / put back (edit undo, `PrefabEditUndo`) |
| `getPrefabOverrideEntries(sceneId, instanceRoot)` | every override of an instance and the ones nested in it, by GameObject in hierarchy order (`PrefabOverrideEntry`: property, state, added/removed component, added/removed GameObject) |
| `isEditingPrefabAsset(sceneId, entity)` | Prefab Mode's GameObjects: their edits are no overrides |
| `capturePlayPrefabState` / `restorePlayPrefabState`, `restorePlaySnapshot` | Play mode (section 12) |
| `loadPrefabAsset(path)` | the asset, read when not loaded |
| `getPrefabAssetLayers(sceneId, instanceRoot)` | the `assetLayers` of a (nested) instance, innermost first: pass them when importing a nested root on its own (relink does) |
| `nestedAssetLayerFor`, `applyTargetLayer`, `recordNestedRootAssetOverride` | asset-level nested data (section 17) |
| `makePrefabReference`, `remapReferencesToPrefab`, `encodeComponentForPrefab`, `componentReferencesOutside`, `getPrefabReferenceMap`, `findHolderInstanceRoot` | references held by a prefab, stripped objects included (section 17) |
| `moveNestedInstancesToAsset`, `promoteNestedAssetData`, `bakePrefabsForExport` | nested instances into an asset, out of it (unpack), the export (section 17) |

Apply to the owning prefab writes the registry (refused for a reference outside the
instance and the instances nested in it; references into nested instances become stripped
objects), clears the override and refreshes the prefab's instances; apply to an outer
prefab records the value in that prefab's asset-level data for the nested instance
(`Prefab::nestedInstances`, nested chain), references in that prefab's ids
(`encodeComponentForPrefab`), and refreshes it. External edits: the asset
database reports `Modified` for a prefab file changed on disk; a loaded prefab without
unsaved changes is re-read and refreshed (`applyAssetChanges`).

Apply to the prefab of a nested instance root (`isNestedRootApplyTarget`, the first apply
target of such a root) writes onto that prefab's root GameObject (its default overrides
left out) and refreshes it. Every apply primitive refuses in Play mode.

## 11. Export and Play

- `build_prefab_X(scene, root, members)` makes `root` the prefab's root GameObject: a bare
  root (PrefabManager) takes its name, state, PrefabComponent and every component; a root
  that already has a PrefabComponent (a scene instance, written with its own components
  first) only gets the components it lacks. Its source is generated from the prefab
  **baked** (section 17): one instance of it made by the editor's import, so every nested
  prefab at any depth with what each level overrides of it, the GameObjects each adds, and
  every reference into nested prefabs (stripped objects, camera links) come out as the
  editor shows them. `members` gets every GameObject, nested ones included, in the order
  Generator counts them (`Factory::getPrefabMemberEntities` of the prefab, then each nested
  prefab's, recursively); a GameObject a layer removed keeps its slot as `NULL_ENTITY`.
- Scene code: per member override, removed components are removed, added ones added,
  overridden ones assigned whole from the scene's values (exact: the editor's values are
  the prefab's plus the overrides), overridden state set; the root's removed components are
  removed after its build. References to members, nested ones included, go through the
  members vector.
- Play (`registerPrefabManager`): the factory makes the bare root a copy of a template
  imported once per prefab (section 18), as the export's `build_prefab` builds it.

## 12. Editing instances (Unity semantics)

**An edit on a GameObject of a prefab instance (its root included) is an override of that
instance only.** The prefab asset changes through Apply (section 10's primitives, the
Inspector's menus and Overrides dropdown) or in Prefab Mode; other instances and other
scenes never see an instance edit. There is no "edit shared" path any more
(`prefabPropertyChanged`, `prefabNameChanged`, `prefabStateChanged`,
`addComponentToPrefab`, `removeComponentFromPrefab`, `applyComponentOverride` and
"Override Prefab Component" are gone).

| edit | override recorded (`ProjectPrefabInstanceEdit.cpp`) |
|---|---|
| a property (PropertyCmd, MultiPropertyCmd, gizmos: ObjectTransformCmd records only what moved) | the property (`recordPrefabEdit`) |
| a whole component rewritten: Reset, Paste Component Values (ComponentValuesCmd) | the properties that now differ (`recordPrefabComponentEdit`, `diffComponentProperties`; `"*"` when no single property holds the change) |
| mesh changed (MeshChangeCmd), model loaded (ModelLoadCmd) or cleared (ModelClearCmd) | `"*"` on the Mesh / Model; the model's old nodes that are prefab GameObjects become removed GameObject overrides, its new ones added GameObjects |
| material linked / unlinked, animation track key created, terrain map created or deleted (its path) | that property |
| terrain map painted (TerrainMapPatchCmd, the edit window's snapshots) | none: the map file is an asset (Unity's TerrainData); every loaded terrain using it is refreshed (`TerrainMapUtils::refreshSharing`) |
| name, active, tag, layer (EntityNameCmd, EntityStateCmd) | the state bit (`recordPrefabStateEdit`) |
| component added (AddComponentCmd) | an added component; one of the prefab's this instance had removed comes back as a `"*"` override |
| component removed (RemoveComponentCmd) | a removed component; an added one is simply gone. What the component owned (an Action's or Animation's GameObjects) leaves this instance only (removed GameObject overrides) |

- **Restructuring an instance is refused** (Unity: "Children of a Prefab instance cannot be
  rearranged. Open the Prefab to restructure it."): a member of an instance (a GameObject
  of its prefab, a nested instance root included) cannot be reordered among its siblings,
  reparented, or moved out of the instance, by any path: `MoveEntityOrderCmd` (Hierarchy
  drag and drop, the AI's reparent / move actions, Reset Mesh Parenting) checks
  `MoveEntityOrderCmd::getPrefabRestructureBlock` and fails with the message; the
  Hierarchy refuses the whole drop and shows Unity's "Cannot restructure Prefab instance"
  dialog with **Open Prefab** (Prefab Mode in context of the instance holding it,
  `openPrefabFromHierarchy`), the AI actions return the message. Nothing reaches the asset
  in that case. Free: an instance root and an added
  GameObject (moved below an instance's GameObject it is an added GameObject of it, its
  sibling place saved with it; moved out it is a plain GameObject again), and a Prefab
  Mode stage's own GameObjects (`isEditingPrefabAsset`: an asset edit). Deleting a member
  stays a removed GameObject override. `scene-dump --ops` `move {entity, target,
  insertion}` runs MoveEntityOrderCmd.
- Not overrides: an instance root's default overrides (section 6), a component the
  instance added (it is the instance's already), GameObjects added to the instance, edits in
  Prefab Mode (`isEditingPrefabAsset`). A value set back equal to the prefab's stays
  overridden until Revert (Unity).
- **Undo**: each command keeps the GameObjects' overrides from before its first run
  (`command/PrefabEditUndo.h`: `capture` before the edit, `restore` after the undo,
  `mergeOlder` when commands merge, e.g. a drag), so one undo step puts values and
  override state back together; a redo records again. Multi-selection edits are one step.
- **Apply / Revert** run as `PrefabOverrideCmd` (one undo step each, snapshots of the
  instances and the assets); several at once (multi-selection, Revert All, Apply All) as
  one `PrefabOverridesCmd` (`PrefabOverridesCmd::revertAll` / `applyAll`: added GameObjects,
  removed GameObjects, then state and components; Apply All goes to the instance's prefab,
  the outermost, so nested instances' overrides become its overrides of them).
- **Play mode**: edits while playing record overrides like at edit time, and Apply is
  refused (`canApplyPrefabOverrides`).
  Play's snapshot of the played scene takes its instances' bookkeeping with it
  (`capturePlayPrefabState`: every prefab's `instances[scene]` and the Missing Prefab
  records); Stop's `restorePlaySnapshot` puts entities, components and that bookkeeping back,
  so nothing done while playing reaches the asset, another scene or the scene after Stop.
- **Prefab Mode (section 16)**: `Project::isEditingPrefabAsset(sceneId, entity)` says
  which GameObjects' edits are asset edits (a prefab stage's own GameObjects); the record
  calls ignore those. GameObjects of the instances nested in a stage are not: their edits
  record overrides, which the stage saves as the asset's overrides of them. The Prefab
  bar's Open button opens the instance's prefab in context (`openPrefabAsset`).

### Inspector (`editor/window/InspectorPrefabBar.cpp`)

- An overridden value: bold label (the UI font's bold, `App::getBoldFont`) and Unity's blue
  bar in the left margin; right click on it: "Apply as Override in Prefab 'Outer'" ...
  "Apply to Prefab 'Inner'" (the apply targets, outermost first) and "Revert", for every
  selected GameObject overriding it. `propertyRow` names its property for the label
  (`prefabRowProperty`); a custom row sets `prefabRowProperty` before `propertyHeader`.
  An override of `submeshes[0].material` marks that row and the rows inside it.
- A component: blue bar on its header when some property is overridden, plus a "+" badge
  when the instance added it; its menu (right click or kebab) has the same Apply / Revert
  entries for the whole component (added and removed ones included). A removed component
  shows after the others as a struck-through "Name (Removed)" entry whose menu applies or
  reverts the removal. Components following the prefab are drawn like any other.
- The GameObject header: name, active, tag and layer bold with the bar when overridden,
  right click for Apply / Revert.
- The Prefab bar: "Prefab", Open, Select, Overrides. The Overrides dropdown: a tree by
  GameObject of every override (GameObject state, modified / added / removed components,
  added and removed GameObjects); the selected one shows a Prefab Source | Override
  comparison, property by property, with its Revert and Apply (a menu of targets for nested
  ones); Revert All / Apply All below.

### Checking it headless

`jasperengine-editor scene-dump --ops` runs, besides section 10's ops: `set` (a real
PropertyCmd per GameObject, `also` for a multi-selection), `setName`, `setState`,
`addComponent` / `removeComponent`, `delete`, `revertComponent`, `revertAll` / `applyAll`,
`canApply` (prints whether and the targets), `playStart` / `playStop` (Play's snapshot and
Stop's restore without running the game), `undo` / `redo`. The dump lists each
GameObject's `overrides`. `tools/prefab-instance-edit-test.py` runs on an owned, asset-free `legacy` fixture
(tools/prefab-fixtures.py): an explicit override remains explicit when its value equals
the base; the asset file stays the same until Apply; Apply
to the inner prefab or as an override in the outer one reaches exactly the instances it
should; Revert, undo and redo restore values and overrides; edits while playing are gone
after `playStop`; overrides survive a save and a reload.

## 13. Replace Prefab (Unity's Replace Prefab Asset)

`editor/project/ProjectPrefabReplace.cpp`, `command/type/ReplacePrefabCmd.*`.

- **Entry points**: Hierarchy, Prefab > Replace... (a popup: the project's prefabs with
  their thumbnails and a search box, "Match Overrides" By Name / By Hierarchy / Root only,
  "Keep Overrides", "Rename Root"); a `.prefab` dropped from the Project window on an
  instance root row with **Alt** held (Alt+Shift: overrides cleared); a Missing Prefab
  row's Prefab > Replace... (relinks it). The whole selection when the row is part of it.
- **Which roots** (`Project::canReplacePrefab`): an outermost instance root, an added
  prefab instance's root, or a Missing Prefab root that is not nested in a live instance;
  never a nested instance root (it belongs to its prefab, as in Unity); not while playing.
- **What it does** (Unity's `ReplacePrefabAssetOfPrefabInstance` with
  `PrefabReplacingSettings`): the root GameObject stays (its id, parent, sibling place and
  its default overrides: name, position, rotation, RectTransform placement) and becomes an
  instance of the new prefab. Its instance data (`prefabInstance`, section 8) is carried
  from the old prefab's registry entities to the new one's by a key
  (`settings.match`): **ByName** the name path from the root (`Body/Wheel#1`: the second
  sibling named Wheel; `~name` for top-level GameObjects without a Transform),
  **ByHierarchy** the sibling index path, **None** the root alone.
  - property overrides: kept for the components the matched GameObject has (or the
    instance adds to it); removed-component overrides when it has the component; state
    and added components always (an added one the new prefab has becomes a `"*"` override,
    section 9);
  - removed GameObjects: kept when matched;
  - added GameObjects: below their matched parent, else below the root (logged);
  - nested instances: the same nested asset (by GUID) keeps its data as it is, another
    one is matched one level down the same way;
  - member ids follow the keys, so references from the scene to a matched GameObject stay
    (an unmatched one's GameObject is gone, as in Unity);
  - Keep Overrides off (Unity's `ClearAllNonDefaultOverrides`): only the default
    overrides stay; added GameObjects go too.
  Everything dropped is logged as a warning, with a summary line per root.
- **Rename Root** (`changeRootNameToAssetName`): a root still named after its old prefab's
  root ("Crate", "Crate (2)") takes the new prefab root's name, made unique.
- **Missing Prefab roots**: the old asset cannot be read, so only the root's entry matches
  (the overridden object that is not in `memberIds`, else the one without values); the
  GameObjects the scene added below it stay and become added GameObjects.
- **Steps** (each a `Project` API): `capturePrefabInstanceState` (the root's node, as
  `Stream::encodeEntity` writes it, and the member ids), `buildReplacedPrefabNode` (the
  same node, PrefabComponent swapped, data and ids remapped, the dropped list in a
  `PrefabReplaceReport`), `rebuildPrefabInstance` (the instance unimported keeping its
  root, or the Missing record cleared; the root's name, state and components, its
  PrefabComponent included, from the node; `importPrefab` with the node's data and the
  ids; a result that is Missing keeps the whole node and gets its children back).
  `ReplacePrefabCmd` replaces any number of roots as one undo step; undo rebuilds each from
  its captured node, exactly (same ids: `scene-dump` before and after undo compare equal
  with `--ids`).
- `UnpackMissingPrefabCmd`: Unpack on a Missing Prefab: the root becomes a plain
  GameObject (its PrefabComponent and record go); undo makes it Missing again.
- `scene-dump --ops` has `replace {entity, prefab, match, keepOverrides, renameRoot}`,
  `unpackMissing {entity}`, `preview {prefab}`, `usage`, `close {scene}`;
  `tools/prefab-fixtures.py replace <dir>` makes a project with prefabs to replace the
  legacy fixture's instances with.

## 14. Hierarchy rows and the Scene view drop

`editor/window/HierarchyPrefabRows.cpp` (`HierarchyWindow::markPrefabRows`, called after
each tree rebuild), `SceneWindow::updatePrefabDrag`.

- **Added GameObjects** (Unity's added GameObject override): a GameObject with a
  Transform below an instance's root or member that is no member of any instance gets a
  green "+" on its icon; below a Missing Prefab root the "+" is red. Removed GameObjects
  are not in the scene, so not listed (as in Unity).
- **Instance roots**: blue cube and blue name, members blue; a ">" button at the right end
  of the row opens the prefab: `HierarchyWindow::openPrefabFromHierarchy(path)` is the one
  entry point for Prefab Mode (`openPrefabAsset`). `isPrefabVariantAsset(path)` says a row is an instance root of a Prefab
  Variant: it gets the variant badge (section 19).
- **Missing Prefab roots**: red, "(Missing Prefab)" after the name, the last path and the
  GUID in the tooltip; their Prefab submenu: the last path and GUID, Copy GUID, Select
  Missing Asset Folder, Replace... (relink), Unpack (`UnpackMissingPrefabCmd`), Remove
  (delete).
- **Drop in the Scene view**: a `.prefab` dragged over a Scene view shows an instance
  following the pointer (an `ImportPrefabCmd` run outside the undo history): in 3D on what
  the pointer is over (the preview left out of the raycast,
  `Project::findObjectByRayIgnoring`; a terrain at its surface, another GameObject at the
  ray's hit on its bounds), else on the ground plane (y = 0), else at the camera's target;
  in 2D and UI at the pointer's world position (z = 0 plane), the prefab keeping its own z.
  The prefab root's rotation and scale stay. Leaving the view or ending the drag elsewhere
  takes the preview away; the drop replaces it with `ImportPrefabCmd`s
  (`setDropPosition`), one undo step, the new root selected. Not while playing. A drop on
  a Hierarchy row works as before.

## 15. Prefab thumbnails and the Prefabs dialog

- **Thumbnails** (`ProjectWindow::processPrefabThumbnails`, `render/preview/PrefabRender`):
  a `.prefab` in the Project window gets a rendered thumbnail, built on the main thread
  (the instance comes from the project's prefab data: `Project::instantiatePrefabPreview`
  imports it, nested prefabs and their overrides included, into a scene of its own,
  `SceneProject::prefabPreview`, and forgets it: no instance list, Hierarchy or Missing
  Prefab warning sees it). Framed like model thumbnails in perspective; a 2D or UI prefab
  (sprites, tilemaps, polygons, UI, no 3D mesh) orthographic. At least 3 frames, more while
  resources still build (60 at most), one prefab at a time. Cached as a PNG in the thumbs
  directory (named from the file's path, size and time) with a `<png>.deps` stamp: the
  relative path, size and time of the prefab file and of every prefab it nests. A
  different stamp re-renders it: when the folder is listed and on the Project window's
  1 Hz check of the current folder (a prefab saved in place, a nested prefab changed).
- **Prefabs dialog** (Assets > Prefabs..., `PrefabsWindow`): every prefab file with its
  path, GUID (click copies), instance count, and where: per scene "Scene (n + m nested)"
  (closed scenes marked; their counts read from the scene file), "in X" for the prefab
  files nesting it, and the build checkbox as before (a prefab a scene lists is always
  built). A Missing Prefabs section lists referenced GUIDs no file has, with their last
  path and Missing instances per scene. The counts come from
  `Project::collectPrefabUsage`: loaded scenes from their instances (an instance whose
  root is a member of another is "nested"), closed scenes from their file (each instance
  root node, then the nested prefabs of its asset minus the removed GameObjects of the
  scene's and the assets' layers, recursively, and the added GameObjects at every level),
  keyed by GUID (a reference without one by the GUID its path was upgraded to).

## 16. Prefab Mode (prefab stages)

Unity's Prefab Mode. `editor/PrefabStage.cpp` (Project), `PrefabStage` in `Project.h`, the
Scene view bar in `window/PrefabStageBar.cpp`, the Hierarchy's part in
`window/HierarchyPrefabMode.cpp`.

**A stage is a scene.** `PrefabStage::sceneProject` is a `SceneProject` kept outside
`Project::scenes` (so nothing that lists, saves or builds scenes sees it) that `getScene(id)`
answers to like any scene: the Hierarchy, the Inspector, the Scene view, the commands, the
undo history (`CommandHandle::get(stage id)`, one per stage) and the selection work on it
unchanged. Loops that must reach every GameObject being edited (script refreshes, asset and
shader path changes, terrain maps, tools) use `getScenesAndPrefabStages()`.

- **Content**: the asset as `encodePrefabNode` gives it (the in-memory asset, upgraded),
  decoded by `Stream::decodeSceneProjectEntities(..., reservedBelow = nextEntity)`. The
  prefab's GameObjects keep their registry ids: **a stage entity id is the GameObject's
  file id**, and new GameObjects take ids above the asset's `nextEntity` (the counter starts
  there; ids under it are reserved for the members of the nested instances, which take ids
  above too), so the identity contract of section 2 holds without any id mapping.
- **Nested prefabs** are real instances in the stage, imported with the asset's
  `prefabInstance` block of their root as their *instance data* (a nested root without a
  block gets an empty v3 one first: without it the import reads it as a pre-v3 root). So
  what is edited on them in the stage is recorded as their overrides, and on save those
  are written back as the asset's block for that nested root: the asset-level override
  (section 7, `Prefab::nestedInstances`). Unity's semantics: editing a nested instance in
  Prefab Mode overrides it in the outer asset, the inner asset is unchanged.
- **Save** (`savePrefabStage`): the stage's top-level GameObjects (root first) encoded
  like a scene (`Stream::encodeEntity` with the stage), the scene-only keys of the blocks
  dropped (`version`, `memberIds`), wrapped in the v3 header (`nextEntity` = max(asset's,
  stage counter + 1)); then `Prefab::loadRegistry` (the registry is the stage), references
  outside the asset's own GameObjects cleared (into nested members: B's stripped proxies),
  `savePrefabToDisk`, prefabs nested in the stage that it changed saved too, and
  `refreshPrefabInstances(path)`: every loaded instance (scenes and other stages, nested
  ones included) follows, keeping its overrides; scenes not loaded read the file when they
  load. The generated sources of the scenes using it are cleared (written again by
  `updateGeneratedSources`) and the prefab sources rewritten; shader keys and the export
  read the saved asset (`collectRequiredShaderKeys` reads the registry/file; the exporter
  saves unsaved stages first).
- **Single root**: a GameObject that ends up at the top of a stage (created there, dropped,
  pasted, duplicated root) goes below the root (`fixPrefabStageRoots`, each frame and on
  save; Unity does the same). The root cannot be deleted (`DeleteEntityCmd` refuses).
- **Undo** is the stage's own history (Ctrl+Z with the stage shown). Deleting a nested
  instance and undoing it gives it back where it was: `DeleteEntityCmd::undo` puts back the
  saved local Transform values after re-parenting (an instance root's world matrix is the
  prefab root's until the next update, so keeping its world placement moved it; this
  affected scenes too).
- **No cycles**: an instance of the stage's prefab, or of any prefab holding it or one of the
  stages below it in the stack, is refused in a stage (`canInstantiatePrefabIn`, checked by
  `ImportPrefabCmd` and `importPrefab`).
- **Auto Save** (Unity's toggle, on by default, `Project::prefabAutoSave`): each command on
  a stage (`CommandHistory::onSceneModified` -> `notePrefabStageChange`, which also marks
  the stage modified: an undo past a save differs from the file again) arms a save that
  runs 350 ms later once no edit is in progress (mouse button down, an active widget, text
  input). Off, `Save` (bar, Hierarchy header menu, Ctrl+S: `saveScene(stage)` saves the
  stage) writes it, and leaving a modified stage asks **Save / Discard Changes / Cancel**
  (`closePrefabStages(depth, done, choice)`; `registerSaveDiscardAlert`). Unsaved stages
  count as unsaved changes for quitting, switching projects and Save All
  (`hasScenesUnsavedChanges`, `saveAllScenes`).
- **Outside changes**: once a second a stage compares the asset (`encodePrefabNode`) with
  what it last read or wrote: an unmodified stage reloads (its undo history cleared), a
  modified one keeps its version and warns (saving writes it over the other).
- **Stack (breadcrumbs)**: `prefabStages`, bottom first; the top one is shown in the one
  "Prefab Mode" tab (`###PrefabMode`, docked with the scene tabs). Opening from an instance
  inside a stage pushes; opening from the Project window or from a scene replaces the stack
  (each modified stage asks first); the breadcrumbs (`Scenes > Outer > Inner`), the back
  arrows (Scene view bar, Hierarchy header) and the Hierarchy header menu pop to a depth.
  Leaving selects the stage below, or the scene Prefab Mode was entered from
  (`returnSceneId`, `getSceneBehindPrefabStage`: also Play's, the build's start scene and
  the workspace's selected scene).
- **Environment**: a 3D stage gets the new-scene look as system entities (never listed,
  picked or saved): a directional light, and a skybox in isolation. The scene type is the
  context's, else that of a loaded scene using the prefab, else UI for a root with a
  RectTransform/UI component (the canvas is the environment), 2D for 2D components, 3D.
- **In Context** (from an instance; Unity's default from the Hierarchy arrow and the
  Inspector): the context scene (a scene, or the stage below) is the Engine's main scene
  under the stage (`SceneRender::setUnderlay`; the stage is a layer that keeps the colour),
  seen through a system camera that follows the stage's editor camera mapped by the
  instance root's world placement over the prefab root's, so the prefab stands where the
  instance is; the instance itself is hidden while drawn (`begin/endPrefabStageDraw`). The
  context is not pickable. **Context: Normal / Gray / Hidden** (default Gray: a translucent
  veil layer, `PrefabContextOverlay`). A running or closed context leaves the stage in
  isolation.
- **Play**: Play runs the scene behind Prefab Mode; the stage stays open and editable
  (Unity). Its saves update the loaded scenes that are stopped; the playing scene gets the
  asset when it stops (its snapshot is re-imported from the asset).
- **Entry points**: Project window double-click / Open Prefab on a `.prefab` (isolation;
  "Open as Text" keeps the YAML reachable), Hierarchy instance arrow and Prefab > Open Asset
  in Context / in Isolation, the Inspector's prefab bar through
  `EditorHost::openPrefabAsset(path, sceneId, instanceRoot, inContext)`.
  `Project::isEditingPrefabAsset(sceneProject)` tells the override machinery it is in a
  stage. Headless: `scene-dump` ops `openPrefab`, `closePrefab`, `savePrefab`, `autoSave`,
  `tickPrefab`, `undoStage`/`redoStage`, `setCmd`, `create`, `createChild`, `deleteEntity`
  (`stage: true` addresses the shown stage); the dump lists the stages.
- **Nested data** (section 17): on load the GameObjects the asset adds to its nested
  instances become those instances' added GameObjects in the stage
  (`prefabNodeToStage`) and its stripped references resolve to the nested GameObjects
  (`resolveStageStrippedReferences`); on save GameObjects added below a nested instance's
  GameObjects become the asset's (`addedObjects`, stripped parents), and references into
  nested instances become stripped objects, the ones the asset had keeping their ids
  (`stageToPrefabAsset`).

## 17. Nested prefabs in assets, stripped objects, the baked export (B)

### What a prefab holds of the instances nested in it

A nested prefab instance is, in its holding prefab X, its root (a registry entity with a
`PrefabComponent`) and three kinds of asset-level data, at any depth:

| | in memory | in the file |
|---|---|---|
| overrides of its GameObjects (properties, state, added / removed components), removed GameObjects, the same for the instances nested in it (`nested`) | `Prefab::nestedInstances[nestedRoot]` (`PrefabInstanceData`) | the nested root's `prefabInstance` block |
| GameObjects X adds to it | registry entities of X, children of the nested root; `Prefab::addedObjects[top]` = the nested GameObject they go below (a stripped id, NULL for the nested root) and their place among its children (`childIndex`, -1 last) | the nested root's `children`, the top of each with `addedTo` / `childIndex` |
| references from X's values into it | stripped ids (below) | `stripped:` |

Values of that data, at any depth, are in **X's ids**: X's registry entities and X's
stripped ids. The nested root's own values (its default overrides, the properties X
overrides of the nested prefab's root, the components X adds to it) are the nested root's
registry entity's.

### Stripped objects (Unity's)

`Prefab::stripped`: stripped id -> `source`, the way down: the nested root's registry entity
in X, then the registry entity of the next nested root in that prefab, ..., then the
GameObject's registry entity in the innermost prefab (a GameObject of an instance nested
in a nested instance is reached without the middle prefab knowing). A stripped id is never
a registry entity: it comes from the same never-reused counter (`Prefab::allocateId`,
`nextEntity` counts them). Any reference a value of X holds can be one: an entity property
(action target, joint bodies, bone / model links, UI links), a script's entity reference, a
camera texture link (`camera|<id>`; written even unbound). The file lists the ones whose
first step is still a nested root of X.

- `makePrefabReference(sceneId, instanceRoot, localTarget)`: the id X (the prefab of the
  instance rooted at instanceRoot) uses for a scene GameObject: its registry entity, else
  the stripped id of the way down to it (made, X modified), else NULL_ENTITY (outside: no
  prefab can hold it). `remapReferencesToPrefab` does it for a component's references in
  place (false when one was outside, now None); `encodeComponentForPrefab` gives the
  component's YAML as X stores it; `componentReferencesOutside` checks first (the
  instance's tree: `collectInstanceTreeObjects`). Apply paths, Create Prefab and Add to
  Prefab use these.
- Resolution: `getPrefabReferenceMap(sceneId, instanceRoot)` (registry entities and
  stripped ids -> local); the import builds the same map once the
  nested instances exist (`addStrippedToMap`, `resolveStrippedObject`). A stripped object
  whose GameObject is not there (removed by a layer, a Missing Prefab) resolves to nothing:
  the reference is None.

### Import (section 9)

`importPrefab` phase 1 (`prepareBranch` leaves out a nested root's children) decodes this
prefab's GameObjects, then for each nested root in order: imports it with
`nextImportDeferred` (GameObjects only), then decodes the GameObjects X adds to it below
their resolved parent at their `childIndex` (nested roots among them join the queue). Then
the map with stripped objects; every reference of X's GameObjects but its nested roots is
made local (`remapEntityProperties`, anything unmapped None). Then X applies its data to
each nested instance (`applyHolderData`): its root takes, from X's nested root entity
(references mapped by X's map), the default overrides and the properties / components X
overrides; its other GameObjects and deeper nested instances take the entries' values,
references mapped by X's map. A deferred import returns there; the outermost then applies
the scene's data to the whole tree (`applySceneData`, recorded) and decodes the scene's
added GameObjects (`decodeSceneAddedGameObjects`).

### Into an asset: Create Prefab, Add to Prefab

`createPrefab` and `addEntityToPrefab` encode the selection with the project (a prefab
instance in it is its root and its block; Hierarchy payloads, single and multiple, do the
same), decode it into the registry, then `moveNestedInstancesToAsset` for its nested roots:

1. the scene GameObjects added below each nested instance's GameObjects (at any depth, not a
   GameObject of any instance) become X's registry entities below the nested root, with
   `addedTo` (a stripped id made from the scene parent) and `childIndex`; the scene
   GameObjects become members of X's instance; prefab instances among them are nested roots
   of X too and go the same way;
2. what the scene's data overrides of each (`Instance::overrides` / `removed` of the nested
   instance and the ones in it, a nested root's root overrides as its root entry) becomes
   `nestedInstances[nestedRoot]`, values encoded with references in X's ids; a component
   referring outside stays a scene override (and out of the asset);
3. the scene's data of those instances is cleared (the import makes the same values from
   the asset).

Then every reference of X's registry is made X's (`remapReferencesToPrefab`); components of
X's instance referring outside its tree become whole-component scene overrides
(`overrideOutsideReferences`). Add to Prefab then re-imports every instance of X
(`refreshPrefabInstances`; ids kept). Undo: `CreatePrefabCmd` and `AddEntityToPrefabCmd`
re-import the nested instances from their nodes taken before.

Apply of an added GameObject (`AddEntityToPrefabCmd(..., target)`,
`addEntityToPrefab(..., target)`): to the prefab whose GameObject its parent is (empty
target), or to a prefab holding that instance (`getPrefabApplyTargets(parent)`; the
Overrides dropdown offers them, Apply All uses the instance's own prefab):
`addEntityToOuterPrefab` makes it a GameObject that prefab adds to its nested instance
(below the nested root, `addedTo` the parent's stripped id, at its `childIndex`).
Neither takes a GameObject whose branch holds an instance of the target prefab or of a prefab
holding it (nested instances, a variant's bases, at any depth: `canAddToPrefab`): the prefab
would contain itself. Add to Prefab refuses it ("... so the prefab would contain itself"), and
Apply All leaves it an override (counted in `remaining`).

### Out of an asset: Unpack

`unpackPrefabInstances` calls `promoteNestedAssetData` before dropping an instance: what its
prefab (down its chain) overrides of the instances nested in it becomes their scene
overrides and removed GameObjects (a nested root's root entry and what the instance held
of that root become the root's own, it is a top-level instance now); the GameObjects the
prefab added to them are added GameObjects of theirs by themselves. `repackPrefabInstances`
puts their previous state back (`PrefabUnpackRecord::nestedBefore`).

### Source values of a nested root

`buildPrefabSource` / `getPrefabSourceValue` / `getPrefabSourceState` of a nested instance
root (owned by the instance holding it): the nested prefab's root's value, the holding
prefab's overridden properties and default overrides from its nested root entity, then the
layers above, never the holding prefab's whole stored copy (stale after the nested prefab
changed).

### Export: baked prefabs

`Project::bakePrefabsForExport` imports each loaded prefab once into a scratch scene (its
own scene id, dropped right after) and lists its GameObjects in build order
(`BakedPrefab::slots`: the prefab's members, then each nested root's prefab's slots,
recursively; a GameObject a layer removed keeps an empty slot). `Factory::createPrefab`
generates `build_prefab_*` from it: every GameObject created, named, its state, its
components (references through the slot variables), nested roots keep their
`PrefabComponent`, the hierarchy in Transform order, camera texture links into and out of
nested prefabs. Generator's scene side counts the same slots (`getPrefabMemberEntities`
includes the GameObjects a prefab adds to its nested instances). Play imports with the
editor (`registerPrefabManager`), the same code the bake uses.

### Prefab Mode

A stage (section 16) is a scene holding the asset's GameObjects with their file ids and its
nested prefabs as instances whose scene data is the asset's nested data. `prefabNodeToStage`
turns the GameObjects the asset adds to a nested instance into that instance's added
GameObjects (`addedGameObjects` of its block or of the nested entry `addedTo` leads to, at
`childIndex`, ids kept) before the stage is decoded; `resolveStageStrippedReferences` then
maps stripped ids to the nested GameObjects (only those ids: everything else keeps its id).
On save, after `loadRegistry` of the stage's encoding, `stageToPrefabAsset` makes the added
GameObjects of the nested instances (at any depth) the asset's again (`addedObjects`,
stripped parents; prefab instances among them nested roots with their data) and every
reference the asset's (its GameObjects', stripped objects for the nested instances' with the
ids the asset had, None outside), in its registry and in its nested data's values.

### Variants

Everything here is keyed by a registry entity of the holding prefab that roots an instance
of another prefab: a nested root (`Prefab::isNestedRoot`), or the root of a Prefab Variant
(its base). `Prefab::isInstanceRoot` is either; section 19 builds Variants on it.

### Tests

These scripts author their own project and scene files; they need no sample game or assets.
Set `JASPER_EDITOR` to an editor command, or use the default Linux install from `tools/build.sh linux`.

`tools/prefab-fixtures.py nested <dir>` writes a project of plain GameObjects four levels
deep and `<dir>.ops/build.yaml`, the scene-dump ops making Leaf, Branch (holding Leaf), Tree
and Forest bottom-up with overrides of every kind, added / removed GameObjects and
references (camera link, action targets, joint bodies) at every level, a fresh instance of
each compared with what it was made from (`prefab-dump-compare.py --subtree=A:B`), scene
references into deep members and between instances, a multi-selection of instances made
prefabs (unpacked to originals), an instance put into a prefab, and `bake` (the generated
`build_prefab_*` sources). A Lua script (`Watcher.lua`) with an entity property points into
nested prefabs from Branch and Tree and logs its target when the game starts (`WATCH ...` in
an exported game's log). Scene-dump ops added for this: `createPrefabs`, `addToPrefab`,
`create`, `setRef`, `setStateLocal`, `addLocalComponent` / `removeLocalComponent`, `spawn`
(Play's factory: a bare root imported), `bake`; `@/x` in a path is next to the ops file.

`tools/prefab-nested-test.py [work-dir]` runs all of it and
checks: a fresh instance of each level equals what it was made from (by
`prefab-dump-compare.py --subtree`), the references at every level, a reload equal with
member ids, a second save byte for byte the same, Play's spawn equal to an instantiated
instance, Apply of a reference into a nested prefab to the owning and to an outer prefab,
undo / redo of Create Prefab holding an instance, a nested prefab going missing and coming
back, Revert of a nested root after its prefab changed, an added GameObject applied to an
outer prefab, Prefab Mode (open and save changes nothing; a GameObject added below a nested
GameObject and a reference into a nested prefab are kept).
`tools/smoke-test.sh <work-dir>/project` exports, builds and runs that project.

## 18. Runtime: Instantiate, Destroy, Prefab properties

Unity's runtime API. Engine: `engine/core/manager/{ObjectManager,PrefabManager,PrefabAsset}`,
`engine/core/registry/EntityCloner`; Lua in `GameObjectClassesLua.cpp` / `CoreClassesLua.cpp`.
Editor: `editor/project/ProjectPrefabRuntime.cpp`, `Generator` (main.cpp, `scene_scripts.cpp`).

### API (Lua and C++ alike)

| | |
|---|---|
| `GameObject.instantiate(original)` | `original`: a `PrefabAsset`, a prefab path/name/GUID string, a prefab id, a GameObject (any handle) or a Lua script (`self`, whose copy's script of the same class is returned) |
| `... (original, parent [, instantiateInWorldSpace])` | below `parent` at the original's local placement, or at its world placement |
| `... (original, position, rotation [, parent])` | at that world position and rotation |
| `... (prefab, scene [, position, rotation])` | a prefab into a given scene (default: the active scene, `ObjectManager::getActiveScene`: the scene whose scripts are starting, else `Engine::getScene`) |
| `GameObject.destroy(object [, delay])` | the GameObject and its children, after this frame's update (before the draw), or `delay` seconds of game time later |
| `GameObject.destroyImmediate(object)` | now |
| `PrefabAsset.load(path or id)`, `.valid`, `.id`, `.guid`, `.path`, `.name` | a reference to a prefab of the build |
| `PrefabManager.findPrefab(name)`, `getPrefabNames()`, `prefabCount` | the registry |

C++: `GameObject::instantiate(...)` (the same overloads, returns a `GameObject` whose entity is
`NULL_ENTITY` on failure), `GameObject::destroy`, `GameObject::destroyImmediate`;
`ObjectManager` has the scene/entity forms. `createPrefab` / `destroyPrefab` are gone
(clean break): `GameObject.instantiate(PrefabAsset.load(name), parent)` replaces them.

- The new root is named `<name>(Clone)` (Unity). A prefab's copy carries its
  `PrefabComponent` but is not a tracked prefab instance (Unity: a runtime clone is not
  connected to its prefab): Stop removes it like any GameObject the game made.
- Placement: a prefab's root keeps its own position/rotation/scale (no parent), or that
  local placement below a parent, or that as world placement with `instantiateInWorldSpace`.
  A copied GameObject goes where the original is in the world (no parent), keeps its local
  placement below a parent, or its world placement with `instantiateInWorldSpace`. World
  values are computed from the Transforms as they are (not last frame's matrices).
- Scripts: every new GameObject's scripts start before `instantiate` returns: Lua instances
  are made, their properties set (entity references resolved), then `init()` of each;
  C++ scripts are constructed and their properties bound (`initEntityScripts`, below).
  `destroy` calls Lua `onDestroy()` (new, Unity's OnDestroy; only for GameObjects
  destroyed this way, not when a scene unloads) then releases the instances and deletes
  C++ scripts (`cleanupEntityScripts`). The GameObjects due at the end of a frame are
  destroyed together, per scene (`EntityRegistry::destroyEntities`: every removal
  callback, then each component array compacted once); `destroyImmediate` compacts once
  per call. A Lua script file is compiled once per Play/run
  (`LuaBinding` chunk cache); its chunk still runs per instance (module-level locals stay
  per instance, as before).

### Copying (`EntityCloner`)

`collectHierarchy(registry, root)`: the root, its Transform descendants (the branch after
it in the Transform array), and the GameObjects without a Transform that belong to them
(actions targeting one of them, a model's `animations`, an animation's tracks), to any
depth; foliage a terrain streams in is left out. `clone(source, entities, target,
targetRoot)` copies them, across registries too (Play copies from a template scene):

- Components are copied whole, then what the engine made of them at runtime is reset
  (`prepare<T>`): GPU objects (renders, shaders, buffers' render handles, sky env maps,
  bones buffers), physics bodies/shapes/joints, sound voices and samples, a camera's
  framebuffer (a new one; textures showing the original camera's show the copy's when
  both are copied), text atlases, `loaded` flags, runtime buffers (particles, render
  instances), event subscriptions (`onClick`, `onStart`, ...). Actions restart (a Running
  one gets `startTrigger`, as a loaded scene does). A `Playing` sound plays again (play on
  awake).
- External buffers pointing into a model's file data stay pointed when that model is
  copied too (the copy shares the parsed file), else take a copy of the bytes.
- Entity references in components (Transform parent, action target, model mappings and
  skins, bone model, UI parts, joints, collider sources, layout boxes, animation tracks)
  and in script properties (`EntityReference` with no scene id): a copied GameObject goes
  to its copy; another one is kept within one registry and cleared across registries.
- Script properties take the original's current values first (`syncFromMember`, Unity
  copies the current serialized fields); instances, member pointers and Lua refs are
  cleared on the copy.
- Ids: user ids count up, so what a factory made is every id above the counter before.

### Prefab script properties

`ScriptPropertyType::Prefab`, value a `PrefabAsset{guid, path}` (the variant's last type).

- Declared in Lua as `{ name = "enemy", type = "Prefab" }` (or `"PrefabAsset"`), in C++ as
  `DPROPERTY("Enemy") PrefabAsset enemy;` (or `DPROPERTY("Enemy", Prefab)`). No default
  value: None. Lua sees a `PrefabAsset` or nil; C++ the member (`syncToMember`).
- Scene and prefab files: `value: {guid, path}` (empty map for None); generated code
  `PrefabAsset("<guid>", "<path>")`.
- Inspector (script properties section): the prefab's name, a drop target for a `.prefab`
  from the Project window, a button back to None; Missing when its GUID resolves to no file.
  One `PropertyCmd<PrefabAsset>` per GameObject (undo).
- Resolution at runtime: by GUID in `PrefabManager` (registered with GUIDs), the path only
  when the property has no GUID or the registry entry has none.

### The build's prefabs and their ids (`collectAllPrefabs`)

One list for Play (`registerPrefabManager`) and the export (`Generator::configure` ->
`main.cpp`): the prefabs scenes list (`sceneProject.prefabs`: instances, Missing Prefabs,
and, through `updateScenePrefabs` -> `addScriptPrefabReferences`, the prefabs the scene's
Prefab script properties reference, saved in the scene file's `prefabs:` so closed scenes
keep them), the open scenes' script references as they are now, the standalone list, then
everything those prefabs nest or their scripts reference, to any depth; files that exist
only. Sorted by path: the id is the position + 1, the same in Play and the export,
whatever is loaded. `loadBuildPrefabs()` (from `writeSceneSource` and Play) loads them all
so `writePrefabSources` writes their sources; `appendPrefabCppScripts` adds the C++ scripts
of those prefabs to the build (a prefab only scripts instantiate can carry C++ scripts
no scene holds).

### Play: templates

`registerPrefabManager` registers, per prefab of the list, a factory that copies a
template: on first use the prefab is imported (`importPrefab`, with nesting and
asset-level overrides) into a hidden `SceneProject` (`playTemplateProject`, a `Scene` no
window shows nor the Engine runs, so its scripts never start), its instance tracking
dropped, and the GameObjects kept (`playPrefabTemplates`). Each instantiate is then an
`EntityCloner::clone` from it: no YAML per instance. Cleared at Stop
(`clearPlayPrefabTemplates`) and when Play starts.

### Export

`main.cpp`: `PrefabManager::registerPrefab(id, name, guid, create_prefab_<name>)` per
prefab, then `ObjectManager::setCppScriptHooks(initEntityScripts, cleanupEntityScripts)`.
`scene_scripts.cpp` has, next to `initScripts` / `cleanupScripts`, those two functions
(`extern "C"`; Play's script library exports them and `Conector::connect` hands them to
the engine, `disconnect` takes them back). Factories build each instance in generated
code: no file reading at runtime.

### Timings

`tools/fixtures/prefab-runtime` (Enemy: 4 GameObjects with a nested prefab, a Lua script
with 3 properties), Linux container capped at 2 CPUs, Release, `os.clock()` in the test
(`[PRT] ...` lines), over several runs:

| | exported game | editor Play |
|---|---|---|
| 1000 `instantiate` of the prefab (scripts started) | 30-44 ms (generated code) | 26-41 ms (template copies) |
| 1000 copies of live instances | 43-62 ms | 50-66 ms |
| 1000 `destroyImmediate`, oldest first | 115-193 ms (each compacts the arrays' tail; 15 ms newest first) | 140-187 ms |
| the frame destroying 1001 with `destroy` | 32-57 ms | 94-125 ms (editor frame) |

The test (`tools/smoke-test.sh tools/fixtures/prefab-runtime`, and Play in the editor)
also checks names, placements, parenting, references into the copies, script values,
scripts on copies (Lua and C++), delayed destroy, a scene GameObject destroyed and back
after Stop, and the prefab ids.

## 19. Prefab Variants (E)

Unity's Prefab Variants: a prefab made of an instance of another prefab (its *base*) plus
overrides of any kind and GameObjects of its own. Base edits reach every variant and every
variant instance except where a level above overrides them; a variant of a variant is a
chain of any length. Code: `editor/project/ProjectPrefabVariant.cpp` (levels, chains,
creation); the rest is the nested-prefab model of section 17, with the root as the nested
root, made level-aware.

### Model and file schema

A variant file is a version 3 prefab whose root member carries a `prefab` component naming
its base (`name`, `path`, `guid`; the GUID wins, the path is a hint). That is all that marks
it (`Prefab::isVariant`, `Prefab::getBaseComponent`). `Prefab::isInstanceRoot(e)` is
`isNestedRoot(e)` or the root of a variant. Everything else is section 17 keyed by the root:

- The root member: the root's own components (a registry entity of the variant), and
  `prefabInstance` = the variant's overrides of its base (`nestedInstances[root]`:
  properties, added / removed components, removed GameObjects, state, keyed by the *base's*
  registry ids).
- `children` of the root: GameObjects the variant adds, each with `addedTo` (a stripped id
  of a base GameObject) and `childIndex` (section 17).
- `stripped`: references into the base, `source` a way starting at the root:
  `[V.root, b]` for a GameObject b of the base, `[V2.root, V1.root, b]` through a chain,
  `[V.root, nestedRoot, x]` into a prefab nested in the base.
- A variant never lists its base's members. A variant made in the Project window is the
  root alone (its components copied from the base's root, an empty `prefabInstance`).

Example (`Ship Variant.prefab` of `Ship.prefab`, a Flag added under Ship's Wing):

```yaml
root: 1001
members:
  - entity: 1001
    name: Ship
    components: {transform: ..., sprite: ..., prefab: {name: Ship, path: prefabs/Ship.prefab, guid: 9a0d...}}
    children:
      - {entity: 1002, name: Flag, components: ..., addedTo: 1003, childIndex: 0}
    prefabInstance:
      overrides:
        - {registryEntity: 1001, properties: {transform: [scale]}}
stripped:
  - {entity: 1003, source: [1001, 1003]}
```

Bases are read without importing:

- `getPrefabFileBase(path)` reads the `prefab` component of a file's root (cached by time
  and size, at most one stat a second).
- `getVariantBasePath`, `getPrefabBasePath` (a loaded asset), `isPrefabVariant`.
- `getPrefabVariantChain(path, &broken)` lists the variant first and the original last;
  `broken` is set when a base is missing or the chain cycles.
- `findPrefabVariantsOf(path)` lists the direct variants.

Cycles:

- Import refuses a variant whose chain is broken or holds a prefab being imported up the
  stack; the instance becomes a Missing Prefab.
- Prefab Mode refuses an instance holding the stage's asset: `canInstantiatePrefabIn`
  follows the variant's `prefab` root component like any nested one.
- The creation paths only ever name an existing base.

### Instance levels (in memory) and the scene schema

The root GameObject of a variant instance roots one `Prefab::Instance` per chain level,
outermost first: V2 (the prefab its scene `PrefabComponent` names), then V1, then the
original.

- `findInstanceAt(sceneId, root, path)`, `getInstanceLevels(sceneId, root)`
  (`PrefabInstanceLevel {path, prefab, instance}`), and `instancePathAt`.
- `getNestedInstanceLevels(prefab, instance)` lists the nested instances of an instance as
  `NestedInstanceLevel {registryEntity, localRoot, path, instance, variantBase}`, with a
  variant's base level marked by `variantBase`.
- `findPrefabHolder` (`PrefabHolderLink`) gives the level holding another one: the variant
  over a base level at the same root, else the instance the root is a member of. Overrides,
  stripped references and Apply climb these links.

The scene's `prefabInstance` block writes the base levels as nested entries keyed by the
variant's root (`prefabRootRegistryEntity` = the root's registry id), so one root has a
chain of nested blocks:

```yaml
prefabInstance:
  version: 3
  memberIds: [[1002, 1033]]                  # Ship Variant 2's own members
  nested:
    - prefabRootRegistryEntity: 1001         # its root: the base level, Ship Variant
      prefabPath: prefabs/Ship Variant.prefab
      memberIds: [[1002, 1023]]
      nested:
        - prefabRootRegistryEntity: 1001     # and its base, Ship
          prefabPath: prefabs/Ship.prefab
          memberIds: [[1002, 1002], [1003, 1003], ...]
          nested:
            - {prefabRootRegistryEntity: 1007, prefabPath: prefabs/Engine.prefab, memberIds: ...}
```

`PrefabMemberIds` (preferred ids on import, Missing Prefab data) keys a base level with
`prefabLevelKey(root, depth)` = `(depth << 27) | (root & 0x07FFFFFF)`, so the levels at one
root never collide. As for nested prefabs, the scene overrides of every level stay in the
one scene layer of the outermost instance (section 7).

### Import

`importPrefab` of a variant:

1. It checks the chain. A missing base or a cycle makes the whole instance a Missing
   Prefab (section 4), with its block and member ids kept verbatim.
2. It makes the root from its own member, skips decoding the root's registry children,
   and imports the base first onto the same root. Three thread-locals pass state to the
   base import: `nextImportPath` (the base path), `nextImportLevel` (its depth) and
   `nextImportRootKept` (root components and state the levels above add, remove or keep, so
   the base import does not drop them).
3. It decodes its added GameObjects onto the base's members by stripped ids.
4. `applyHolderData(..., level)` applies the variant's own overrides.
5. The scene layer of the outermost instance goes last (`applyLayersAbove` per level).

Instances of a variant are instances like any other. Everything in sections 7-14 that takes
"the instance" now takes a level (default `{}` = the outermost).

### Create

From the Hierarchy (an instance root > Prefab > *Create Variant*), or by dragging an
instance root into the Project window, which asks "Create a new original Prefab or a variant
of this Prefab?" (*Original Prefab* / *Prefab Variant* / *Cancel*; a variant is named
"<name> Variant"):

- `canCreatePrefabVariant` requires a live outermost (or added) instance root: not a Missing
  Prefab, not inside another instance.
- `createPrefabVariant(sceneId, root, filepath)` (from `CreatePrefabCmd` with
  `setVariant(i, true)`):
  1. Builds the variant's root from the scene root (its components, `prefab` = the base).
  2. Pushes the variant level at the root and names the variant in the scene's
     `PrefabComponent`.
  3. Moves every override of the instance and its added GameObjects into the asset
     (`moveNestedInstancesToAsset` with `{root -> prefab.root}`).
  4. Remaps references to the asset; references outside it stay scene overrides
     (`overrideOutsideReferences`).
  5. Registers the GUID and saves.

  Nothing in the scene changes (checked by dump). Undo is `removePrefabVariantInstance`: it
  deletes the file, restores the scene node and reimports the instance as it was.

From the Project window (a prefab > Create > *Prefab Variant*): `createPrefabVariantAsset(base,
path)` through `CreatePrefabVariantAssetCmd` ("<base> Variant.prefab"; undo deletes the file).

### Apply, Revert, Unpack

- **Apply targets** (`getPrefabApplyTargets`), innermost first:
  1. The levels at the GameObject's instance root, from the original out to each variant.
  2. The prefab owning the GameObject in an outer instance.
  3. The holders.

  The Inspector labels them Unity's way (`applyTargetLabel`):
  - "Apply to Prefab 'X'" for the innermost original.
  - "Apply to Base Prefab 'X'" (or "Base Prefab Variant") when a variant over it holds the
    instance.
  - "Apply to Prefab Variant 'X'" for the instance's own variant.
  - "Apply as Override in Prefab (Variant) 'X'" for the outer ones.
- **Applying to a lower level** writes there and, as in Unity, clears the same property's
  overrides in every level between it and the instance (`clearIntermediateOverrides`).
  After "Apply to Base Prefab", the variant no longer overrides the value.
- **Removed and added GameObjects and components** apply to any level of the chain
  (`applyPrefabRemovalTo`, `ApplyPrefabRemovalCmd` with a target).
- **Revert** goes back to what the level below gives: in a variant instance, the variant's
  value; in the variant's Prefab Mode (its root an instance of the base), the base value.
- **Unpack** follows Unity and removes one level.
  - A variant instance becomes an instance of the base. The root's `PrefabComponent` is
    switched to the base, and the variant's data (its overrides, added GameObjects and
    stripped references) is promoted into the scene layer
    (`promoteNestedAssetData(..., includeBase=true)`), so nothing visible changes.
  - *Unpack Completely* removes every level, leaving plain GameObjects.
  - Both undo (`PrefabNestedState::path`).

### Prefab Mode on a variant

- The stage's root is an instance of the base (`PrefabStage` detects the scene type over the
  whole chain).
- The variant's overrides show as instance overrides (bold, in the Overrides dropdown), and
  its added GameObjects as added ones. Apply and Revert target the base.
- Saving writes the variant file through `stageToPrefabAsset` (section 16): the root's
  `prefabInstance` and its stripped references.
- When the base is missing, the stage root is a Missing Prefab instance of the base, and
  saving keeps the variant's data (unresolved stripped ids are kept).

### Missing base, renames, GUIDs

- Bases are found by GUID first. Renames and moves, made in the editor or with the project
  closed, need no rewrite: scene files and variant files resolve by GUID (a stale `path`
  hint is only a fallback).
- A deleted base makes every instance of its variants a Missing Prefab with its data kept
  (the scene block and member ids by level key; the variant file is untouched).
- Putting the base back restores everything with the same ids.

### Replace, copy, duplicate, Play

- **Replace Prefab** (section 13) across a chain: `remapChainInstance` matches GameObjects
  by keys over the whole effective tree (`ChainNode` = (level, entity)). Replacing base ↔
  variant ↔ variant of a variant keeps matched GameObjects and their overrides.
- **Duplicate and copy / paste** (`EntityClipboard`, `PasteEntitiesCmd`) and **Delete and
  undo** (`DeleteEntityCmd`) snapshot every level.
- **Play spawning** and the runtime import path take a variant like any prefab:
  `GameObject.instantiate` (section 18) of a variant builds its whole chain, both in Play
  (the template made by `importPrefab`) and in the export (`build_prefab_*`). The build's
  prefab list (`collectAllPrefabs`) follows a variant's root `prefab` component to its
  bases.

### Export (bake)

`build_prefab_*` of a variant appends, in order:

1. Its own members: the root and its added GameObjects.
2. Its base's subtree, in the base's `build_prefab_*` order (recursively for a chain). A
   member removed by a level is kept as a `NULL_ENTITY` slot.
3. The nested subtrees.

In the Generator and the Factory:

- `subtreeSize` includes the base.
- A variant instance's `PrefabInstanceInfo` carries `baseLevel` and `baseSize`. `baseLevel`
  is the base level's info, attached to the variant's info (recursively) rather than listed
  at the top level.
- `Factory::createScene` maps the base members and emits the overrides of every level.
- Shader key collection (`collectRequiredShaderKeys`) instantiates any prefab with an
  instance root (nested or variant) to see its effective materials.

### UI

- **Badge** (`UIUtils::drawPrefabVariantBadge`, a dark round mark with an arrow):
  - drawn over the prefab icon in the Project window (list, tile and classic views);
  - drawn on Hierarchy rows of variant instance roots
    (`HierarchyPrefabRows::isPrefabVariantAsset`);
  - its tooltips name the base.
- **Inspector prefab bar**: "Prefab Variant" (with the badge) instead of "Prefab", and a
  "Base" row where the base is selectable ("(a variant of X)" further up the chain).
- **Hierarchy Prefab menu**: *Select Base* and *Create Variant*.
- **Project context menu**: Create > *Prefab Variant*, and *Select Base*.
- **Prefabs dialog**:
  - "Variant of X" with *Select Base* (red when the base is missing);
  - "Variants: ..." on a base;
  - the usage counts a variant instance for the variant, and as nested for its bases
    (`PrefabUsage::base`, `baseMissing`, `variants`).
- **Prefab Mode breadcrumb**: the badge and "Prefab Variant of X".
- **Thumbnails**: `instantiatePrefabPreview` imports the chain.

### CLI ops and tests

`scene-dump` ops:

- `createVariant` (entity, path) and `createVariantAsset` (prefab, path).
- `variantInfo` prints `chain <path>: <V> ... <original>` and the direct variants.
- `levels` prints the levels at an entity.
- `unpack` (`completely: true` for Unpack Completely).
- `applyRemovals` / `revertRemovals` (with `target`).
- `renameFile`, `deleteFile`, `duplicate`.
- `copyPaste` (it creates a temporary ImGui context when headless).
- `usage` also prints bases and variants.

Fixtures (`tools/prefab-fixtures.py`):

- `variant <dir>` writes Engine and Ship (Engine nested), plus the build ops that make Ship
  Variant and Ship Variant 2 from instances with overrides of every kind at both levels.
- `variant_export <dir>` writes a minimal 2D project with untextured Badge, Badge Red (a
  variant) and Badge Green (a variant of it), for the smoke test. A
  `BadgeSpawner.lua` instantiates Badge Red and Badge Green at runtime by path, checks
  their GameObjects and colours, and logs `[VARIANT] PASS` (or `[ERROR] [VARIANT] ...`,
  which fails the smoke test).

`tools/prefab-variant-test.py [work-dir]` checks:

- Creation changes nothing, and fresh instances equal what they were made from at every
  level.
- Base edits propagate except where overridden.
- Apply targets and labels of chains; Apply to the base clears intermediate overrides.
- Revert in a variant's Prefab Mode.
- Unpack (one level and completely), with undo.
- Removed and added GameObjects applied to a variant or to the base.
- Duplicate, copy / paste, Play spawn, and Replace between a prefab and its variants.
- Undo / redo of Create Variant, Apply All and Revert All.
- Save and reload, byte for byte.
- The base and the variants renamed and moved, with the scene open or closed.
- The base deleted (Missing Prefab with its data kept; the Prefab Mode save keeps the file)
  and put back.
- A cycle of bases edited in by hand.

`tools/smoke-test.sh` on the `variant_export` project exports, builds and runs it. The
game's screenshot matches the editor's Game view, plus the two runtime copies.
