# Prefabs: contracts

Developer notes for the prefab system (`editor/project/ProjectPrefab*.cpp`,
`editor/StreamPrefab.cpp`, `editor/util/Prefab.*`, `editor/util/AssetDatabase.*`).
Sections 1-5: files, identity, GUIDs, Missing Prefab (A1). Sections 6-11: the real root,
the override model v2 and its APIs (A2). Section 12: editing instances the Unity way (edits
are overrides, Apply pushes them), Play mode, and the Inspector's override UI (C). `tools/prefab-fixtures.py` makes test projects,
`jasperengine-editor scene-dump --project <dir> [--save]` dumps what loading them gives.
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
        entity: 1004                      # a nested prefab instance: its root only, no children,
        name: Wheel                       # its components as this prefab has them,
        components: {prefab: {name, path, guid}, transform: {...}, ...}
        prefabInstance: {...}             # and what this prefab overrides of it (section 7)
```

- The header is always written (`Project::encodePrefabNode`).
- Version 2 (A1) had the header but the old wrapper-root model; version 1 (no header) is
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

`Project::getAssetDatabase()` maps GUID <-> project-relative path for the GUID-tracked
asset types (`AssetDatabase::isTrackedFile`: `.prefab` now; add a type there, scenes may
follow). The GUID lives in the asset file (top-level `guid:` key); the database is the
index, rebuilt by a scan when the project opens, and cached in
`.jasperengine/assetdb.yaml` (only used to decide which of two files with the same GUID keeps
it).

- **Scan on project load** (`AssetDatabase::open`, before any scene or prefab is read):
  every tracked file gets a GUID. A legacy file gets `legacyGuid(path)` (SHA-1 of its
  relative path: every machine upgrading the same project writes the same GUID, and a
  reference written before GUIDs, by path only, still finds the file after it moves). A
  GUID found in two files stays with the file the cache says had it, else the older one;
  the other file gets a new random GUID written into it.
- **Refresh** (`Project::refreshAssetDatabase`, every 2 s from `App::engineRender`, not
  while playing): external changes. A new file with an unknown GUID is added; with the
  GUID of a file that is gone, it is that file moved; with the GUID of a file that is still
  there, it is a copy and gets a new GUID. A gone file is removed. A file edited so that its
  GUID line disappeared gets it back.
- **Editor file operations** keep it up to date (all through `Project`):
  - rename / move (`RenameFileCmd`, `CopyFileCmd` move, and their undo) ->
    `remapPrefabFilePath` -> `AssetDatabase::moveAssets` (GUIDs kept; a file renamed away
    from `.prefab` is removed, one renamed to it is imported),
  - delete (`DeleteFileCmd`) -> `cleanupPrefabFilePath` -> `removeAssets`; undo ->
    `importPrefabFiles` (the file keeps its GUID),
  - copy / paste / import from outside (`CopyFileCmd` copy) -> `importPrefabFiles(path,
    copy=true, &copyGuids)`: a copy gets a new GUID unless its GUID is unknown (an import
    from another project keeps it), and a redo gives it the same GUID again; undo ->
    `cleanupPrefabFilePath`,
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

Exported games resolve by path: generated code sets `PrefabComponent::{name, path, guid}`
on spawned roots, and PrefabManager registers prefabs by id/name as before.

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

## 5. Notes for later streams (Missing Prefab)

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
  is refused (unpack the instance first). Prefab instances nested in the selection: what
  their scene data overrides becomes the new prefab's asset-level data
  (`moveNestedInstanceDataToAsset`; their added GameObjects stay the scene's; undo puts
  their scene data back). The same happens when a prefab instance is put into a prefab
  (`addEntityToPrefab`).
- **Drop a prefab** (`ImportPrefabCmd`): the new root has only its PrefabComponent and the
  prefab root's name (made unique); the import gives it the prefab root's components, at the
  prefab root's position (Unity); dropped on a parent it keeps that local placement.
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
  (section 8), written as the nested root's `prefabInstance` block in the prefab file.

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
  default overrides and overridden properties are read from it).
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
preferredIds, assetLayers)` builds the instance in layers:

1. the prefab registry (decoded, its references remapped to this instance's ids),
2. `assetLayers`: what the prefabs holding this instance as a nested one override of it,
   innermost first (the containing prefab's `nestedInstances[root]`, then the next one's
   `findNested` chain); not recorded on the instance, **the hook for B**,
3. `instanceData`: the scene's own data, the only layer recorded in `Instance::overrides` /
   `removed`.

Removals of every layer are left out; overrides of a layer apply after the instances
nested in it are built (so they win over the nested prefab). A nested root imported on its
own later (a relinked Missing Prefab, a reverted removal) also keeps from its own values
what the instance holding it overrides of it. Every entry is validated
against the GameObject it names (registry entity -> this instance's local entity): a
property or whole-component override needs the component on that GameObject, a removed
component the prefab GameObject's component, an added one only adds when absent (when the
prefab has it now, the instance's values become a `"*"` override); a GameObject the prefab
lost or a property that no longer exists is dropped and logged, never applied to whatever
else holds that id. Values of the scene layer are in the scene's local ids; values in
asset layers are copied as written (references into nested prefabs: B's stripped proxies).
A legacy nested root (prefab file older than v3) is converted once by `nestedAssetLayer`:
its values in the containing registry diffed against the nested prefab's root become that
prefab's asset-level overrides (written on its next save).

## 10. APIs

On `Project` (`ProjectPrefabOverrideModel.cpp` unless noted); `sceneId, entity` is any
GameObject of an instance, resolved to its owning level.

| API | |
|---|---|
| `findPrefabObject(sceneId, entity, PrefabObjectRef&)` | owning level: path, prefab, instance, local/registry entity, isRoot |
| `isPrefabRootDefaultOverride(type, property)` (static) | Unity's default overrides of a root |
| `isPrefabInstanceRoot(sceneId, entity)` | a top-level or nested instance root |
| `isPropertyOverridden`, `isComponentOverridden`, `getOverriddenProperties` (`"*"` = whole), `getPrefabObjectOverrides` | queries |
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
| `isEditingPrefabAsset(sceneId, entity)` | Prefab Mode's GameObjects (agent D; false until then): their edits are no overrides |
| `capturePlayPrefabState` / `restorePlayPrefabState`, `restorePlaySnapshot` | Play mode (section 12) |
| `loadPrefabAsset(path)` | the asset, read when not loaded |
| `getPrefabAssetLayers(sceneId, instanceRoot)` | the `assetLayers` of a (nested) instance, innermost first: pass them when importing a nested root on its own (relink does) |
| `nestedAssetLayerFor`, `applyTargetLayer`, `recordNestedRootAssetOverride` | asset-level nested data (B) |

Apply to the owning prefab writes the registry (refused for a reference outside the
instance), clears the override and refreshes the prefab's instances; apply to an outer
prefab records the value in that prefab's asset-level data for the nested instance
(`Prefab::nestedInstances`, nested chain) and refreshes it. External edits: the asset
database reports `Modified` for a prefab file changed on disk; a loaded prefab without
unsaved changes is re-read and refreshed (`applyAssetChanges`).

Apply to the prefab of a nested instance root (`isNestedRootApplyTarget`, the first apply
target of such a root) writes onto that prefab's root GameObject (its default overrides
left out) and refreshes it. Every apply primitive refuses in Play mode.
`Prefab::setComponentOverride` / `clearComponentOverride` / `hasComponentOverride` stay as
bookkeeping helpers (whole component, no undo).

## 11. Export and Play

- `build_prefab_X(scene, root, members)` makes `root` the prefab's root GameObject: a bare
  root (PrefabManager) takes its name, state, PrefabComponent and every component; a root
  that already has a PrefabComponent (a scene instance, written with its own components
  first) only gets the components it lacks. Nested prefabs are built first on their roots
  (bare then), then the outer prefab's values over them: position/rotation (default
  overrides), the overridden state and components, removals (`nestedRootOverrides` from
  `Prefab::nestedInstances`). Members are appended as before (root excluded).
- Scene code: per member override, removed components are removed, added ones added,
  overridden ones assigned whole from the scene's values (exact: the editor's values are
  the prefab's plus the overrides), overridden state set; the root's removed components are
  removed after its build.
- Play (`registerPrefabManager`): the factory names a bare root after the prefab root and
  imports onto it (the import adds the root's components), as the export does.

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
  refused (`canApplyPrefabOverrides`; reordering an instance's GameObjects, which edits the
  asset, too).
  Play's snapshot of the played scene takes its instances' bookkeeping with it
  (`capturePlayPrefabState`: every prefab's `instances[scene]` and the Missing Prefab
  records); Stop's `restorePlaySnapshot` puts entities, components and that bookkeeping back,
  so nothing done while playing reaches the asset, another scene or the scene after Stop.
- **Prefab Mode (D)**: `Project::isEditingPrefabAsset(sceneId, entity)` (stub: false) says
  which GameObjects' edits are asset edits; the record calls ignore those. The Prefab bar's
  Open button waits for `App::openPrefabAsset(path)` (`PREFAB-MODE-WIRE` in
  InspectorPrefabBar.cpp: set `prefabModeAvailable`, uncomment the call).

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
GameObject's `overrides`. `tools/prefab-instance-edit-test.py` (FIXTURE=<TappyPlane
fixture>) runs these checks on the `legacy` fixture (tools/prefab-fixtures.py): a member
edit is recorded on that instance only and the asset file stays the same until Apply; Apply
to the inner prefab or as an override in the outer one reaches exactly the instances it
should; Revert, undo and redo restore values and overrides; edits while playing are gone
after `playStop`; overrides survive a save and a reload.

