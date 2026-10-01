# Prefabs: contracts

Developer notes for the prefab system (`editor/project/ProjectPrefab*.cpp`,
`editor/StreamPrefab.cpp`, `editor/util/Prefab.*`, `editor/util/AssetDatabase.*`).
Read this before changing how prefabs are identified, stored or loaded.

## 1. Prefab file format (version 2)

```yaml
type: Prefab
version: 2
guid: 9c1d0f3e5b7a42c8a1e06d2f4b8c7a19   # the asset's GUID, 32 lowercase hex digits
nextEntity: 1007                          # the id the next new GameObject of the prefab takes
members:                                  # the top-level GameObjects, a list even for one
  - type: Entity
    entity: 1001                          # registry entity id = the GameObject's file id
    name: Crate
    components: {...}
    children: [...]                       # a nested prefab root has no children here
```

- The header is always written, single-root prefabs included (`Project::encodePrefabNode`).
- A nested prefab instance inside a prefab is its root entity only, with
  `components.prefab: {name, path, guid}` (see section 3).
- Legacy files (written before the header) are the root entity node itself
  (`type: Entity ...`) or `{type: Prefab, members: [...]}`. The project-load scan inserts a
  `guid:` line at the top of each one and changes nothing else (`AssetDatabase::writeGuid`);
  they load with `nextEntity = highest id + 1` and are written as version 2 the next time
  the prefab is saved. `Prefab::readHeader` reads either form.

## 2. Identity contract: registry ids are file ids, never reused

A prefab registry entity id is the persistent per-object id of a prefab GameObject
(Unity's fileID). Scenes, closed ones included, key everything by it:
`prefabOverrides[].registryEntity`, `prefabRemovedGameObjects[].registryEntity`,
`prefabLocalEntities[].parentRegistryEntity`, `prefabRootRegistryEntity` of nested entries.
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
| nested entries of `prefabOverrides`, `prefabLocalEntities`, `prefabRemovedGameObjects` | `prefabRootRegistryEntity` names the nested root (`prefabPath` is only informative; entries without a registry entity, from the oldest files, match by path) |

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
  `prefabOverrides`, `prefabLocalEntities`, `prefabRemovedGameObjects` and, for a root
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
  `encodePrefabInstance` / `encodeRemovedGameObjects` for a missing prefab nested in a live
  instance (its lists tagged with the nested root's `prefabPath` /
  `prefabRootRegistryEntity`, one level deep like a live one's).
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
  root) or as tagged added GameObjects (nested root) and come back as added GameObjects of
  the instance once relinked.
- Export: a missing top-level root is exported as a plain GameObject; a missing nested
  prefab is built as its root alone (`Factory::createPrefab` skips its build call).

## 5. Notes for the override data model (A2) and later streams

- New per-instance keys written next to a top-level instance root are preserved for
  Missing Prefabs automatically. A missing prefab *nested* in a live instance only keeps
  the three lists above (`appendNestedEntries`, `encodeRemovedGameObjects`); a recursive
  schema must keep its nested data for missing nested roots too.
- `importPrefab(sceneProject, entities, filepath, root, needSaveScene, overrides,
  localEntities, entityRemap, removals, preferredIds)`: the root's `PrefabComponent`
  decides the prefab; `preferredIds` (`PrefabMemberIds`: instance root -> registry id ->
  local id) is the hook for persisted member ids.
- Anything creating a prefab instance root must set `PrefabComponent::guid`
  (`Project::getPrefabGuid(path)`).
