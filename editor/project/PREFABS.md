# Prefabs: contracts

Developer notes for the prefab system (`editor/project/ProjectPrefab*.cpp`,
`editor/StreamPrefab.cpp`, `editor/util/Prefab.*`, `editor/util/AssetDatabase.*`).
Sections 1-5: files, identity, GUIDs, Missing Prefab (A1). Sections 6-11: the real root,
the override model v2 and its APIs (A2). Section 12: prefabs at runtime (Instantiate,
Destroy, Prefab script properties, the build's prefab list). `tools/prefab-fixtures.py` makes test projects,
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
- **Edit-shared editing on the root** (today's Inspector default, kept until C switches to
  Unity semantics): root edits stay per-instance as before, now recorded as overrides:
  `prefabPropertyChanged` on a root records the properties that differ from the prefab's
  value and clears the ones equal again (`syncRootPropertyOverrides`), adding/removing a
  component on a root records an added/removed component (`addComponentToPrefab` /
  `removeComponentFromPrefab`), active/tag/layer likewise (`prefabStateChanged`). The
  name is a default override.
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
  before property overrides, "Override Prefab Component" (ComponentToPrefabLocalCmd) and
  a reference pointing outside the instance record `"*"`.
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
| `getPrefabApplyTargets(sceneId, entity)` | the owning prefab, then each prefab holding that instance, outwards (apply-target choice) |
| `recordPropertyOverride` / `clearPropertyOverride` / `revertPropertyOverride` / `applyPropertyOverride(..., target)` | property overrides (empty list = whole component) |
| `recordStateOverride` / `revertStateOverride` / `applyStateOverride` | name/active/tag/layer bits |
| `recordAddedComponent` / `revertAddedComponent` / `applyAddedComponent` | added components |
| `recordRemovedComponent` / `revertRemovedComponent` / `applyRemovedComponent` | removed components |
| `refreshPrefabInstances(path, reloadFromDisk)` | re-imports every open instance (top-level and nested, all loaded scenes) from the current registry (or the file), keeping overrides, member ids, added/removed GameObjects, the selection and undo validity (same ids) |
| `reimportPrefabInstance(sceneProject, root, rootNode)` | rebuilds one outermost instance from a root node |
| `capturePrefabUndoState(sceneId, entity, targets)` / `restorePrefabUndoState` | undo snapshots: the outermost instance, and for targets the assets (and the prefabs nesting them) plus every instance using them |
| `PrefabOverrideCmd` (`command/type/`) | any primitive above as one undo step |
| `prefabStateChanged(sceneId, entity, bits)` | edit-shared active/tag/layer (EntityStateCmd) |
| `loadPrefabAsset(path)` | the asset, read when not loaded |
| `getPrefabAssetLayers(sceneId, instanceRoot)` | the `assetLayers` of a (nested) instance, innermost first: pass them when importing a nested root on its own (relink does) |
| `nestedAssetLayerFor`, `applyTargetLayer`, `recordNestedRootAssetOverride` | asset-level nested data (B) |

Apply to the owning prefab writes the registry (refused for a reference outside the
instance), clears the override and refreshes the prefab's instances; apply to an outer
prefab records the value in that prefab's asset-level data for the nested instance
(`Prefab::nestedInstances`, nested chain) and refreshes it. External edits: the asset
database reports `Modified` for a prefab file changed on disk; a loaded prefab without
unsaved changes is re-read and refreshed (`applyAssetChanges`).

Kept from before for the current Inspector: `Prefab::setComponentOverride` /
`clearComponentOverride` / `hasComponentOverride` (whole component, "any property"),
`getPrefabComponentOverrides` (components with any override, root included),
`canApplyComponentOverride` / `applyComponentOverride` (whole component; the root's
default overrides are kept).

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
- Play (`registerPrefabManager`): the factory makes the bare root a copy of a template
  (section 12), as the export's `build_prefab` builds it.

## 12. Runtime: Instantiate, Destroy, Prefab properties

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
(`[PRT] ...` lines). Exported game: 1000 instantiates 40-44 ms (generated code), 1000
copies of a live instance 54-62 ms, 1000 `destroyImmediate` 165-193 ms oldest first (each
shifts the arrays' tail; 15 ms newest first), the frame destroying 1001 with `destroy`
48 ms. Editor Play: see the commit message (template copies, no YAML per instance).
