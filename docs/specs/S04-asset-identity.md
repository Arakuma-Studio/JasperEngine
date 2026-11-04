# S04 — Asset identity: GUIDs for every asset, redirects, sub-assets, dependency graph, missing assets

Wave 1. Owner area: `editor/util/AssetDatabase.*`, new `editor/asset/`, new `engine/core/asset/`.

## 1. Goal

Every asset in a project (textures, models, materials, shaders, audio, Lua/C++ scripts, scenes,
prefabs, HTML/CSS UI documents, fonts, terrain maps, data files, and sub-assets inside models)
gets a stable 128-bit GUID that survives renames and moves, including moves made outside the
editor. Scenes, prefabs, materials and the project file reference assets by GUID, with the
path kept as a readable hint. A typed dependency index answers "who references X, and in
which field", and drives Find References, a project-wide Missing Assets panel, retargeting
and rename handling. Today only prefabs have this. Every other reference is a path, so a
rename means per-type fixup code that only reaches loaded scenes, and a move outside the
editor breaks references with no warning.

## 2. Scope

In scope:
- Identity for all asset types (§6.1 type table), stored as described in §6.2.
- Deterministic GUIDs for legacy upgrades and for files that arrive without identity.
- Deterministic, persisted sub-asset GUIDs for glTF/GLB/OBJ meshes, materials, animations,
  skins and embedded images.
- Redirects (former GUIDs) after merges, re-identification and "retarget all".
- An engine `AssetRef {guid, path}` value type, used by every asset-reference field of the
  components, materials, scene settings and project file.
- A typed dependency index (`DepEdge` with `FieldLocator`) as a deletable cache.
- Missing-asset tracker and panel (every type, project-wide) with Retarget, Clear and Locate.
- Find References in the Project window. One rename/move/delete/copy path that replaces the
  per-type remap code.
- Export-time resolution and a small runtime GUID map. Lua API, MCP tools, CLI.
- Migration of path-only references in existing projects.

Out of scope:
- OpenEngine's single JSONL store, its SQLite cache (`AssetDbCache_Sqlite.cpp`), and its
  `AssetStoreReconciler` legacy fixups. Rejected in §6.2.
- Import settings content. S14 owns the `import:` section of the meta file. S04 only provides
  the container and the rule that unknown sections are preserved.
- Asset labels and smart folders. S25 stores labels in the meta file (`labels:`) through the
  same API.
- Rewriting references inside source text (Lua `require`, HTML `href/src`, CSS `url()`,
  shader `#include`). These are indexed as path edges and warned about on rename (R14), but
  their text is not rewritten.
- Folder GUIDs. Folders have no meta files.
- Hot reload propagation along edges (S05 consumes the index).

## 3. Current state

- `editor/util/AssetDatabase.{h,cpp}` (110 + 713 lines). The design is Unity-style: a GUID of
  32 lowercase hex digits is written into the file as a top-level `guid:` key
  (`writeGuid`, line-level edit with a yaml-cpp verify fallback). `legacyGuid(path)` is
  `SHA1("jasperengine:asset:"+path)[0..32]` (`AssetDatabase.cpp:94`), so every machine that
  upgrades the same project writes the same GUIDs. A cache in `.jasperengine/assetdb.yaml`
  (`:315`) decides which file keeps a duplicated GUID. `refresh()` stat-scans for
  Added/Removed/Moved/Modified. `moveAssets/removeAssets/importAssets(copy, copyGuids)`
  handle redo-stable copies. **Only `.prefab` is tracked** (`isTrackedFile`, `:73`, which
  calls `Util::isPrefabFile`). The contract is in `editor/project/PREFABS.md` §3–5.
- Prefab references are `{name, path, guid}` (`PrefabComponent::guid`, the scene `prefabs:`
  list, `project.yaml standalonePrefabs`), resolved by
  `Project::resolvePrefabPath(guid, pathHint)` (`editor/project/ProjectPrefabPaths.cpp:32`).
  An unknown GUID is Missing even when a file sits at the hint (Unity semantics). The refresh
  runs every 2 s from `App.cpp:2526` → `ProjectPrefabPaths.cpp:135`, and changes are applied
  in `applyAssetChanges` (`:159`).
- Runtime prefab identity: `engine/core/manager/PrefabAsset.h` (`{guid, path}`, a
  PrefabManager lookup by GUID/path/name) and `ScriptPropertyType::Prefab`
  (`engine/core/script/ScriptProperty.h:42`).
- **Every other reference is a path string**, stored either project-relative (scenes,
  materials, shaders, scripts) or assets-dir-relative (`Util::isAssetFile`: images, models,
  fonts, audio, HTML, CSS; `editor/util/Util.h:168`). Examples: `ModelComponent::filename`
  (`engine/core/component/ModelComponent.h:54`), `AudioSourceComponent::filename` (`:39`),
  `UIDocumentComponent::filename`, the texture node `{source: path, path}`
  (`Stream::encodeTexture`, `editor/Stream.cpp:1659`), `customShader` (a base path or
  `"vert|frag"`, `Util.h` `makeCustomShader`), terrain height/blend/layer textures and
  `meshPath`, the linked material file (stored in `Material::name`, re-linked on load at
  `editor/Project.cpp:4272-4287`), the project `scenes:` list (`filepath`), and scene
  `cppScripts` paths (`Stream.cpp:2996`).
- Rename fixups are hand-written per type. `editor/command/type/RenameFileCmd.cpp:44-63`
  dispatches to `remapMaterialFilePath`, `remapSceneFilePath`, `remapPrefabFilePath`,
  `remapScriptFilePath`, `remapAssetFilePath` and `remapShaderFilePath`
  (`editor/Project.h:1210-1221`); `CopyFileCmd.cpp:43-55` repeats them. The visitors
  (`visitAssetPathsInRegistry`, `Project.cpp:488`; `applyAssetPathChange`, `:1274`;
  `applyCustomShaderPathChange`, `:1347`; `visitAssetPathsInMaterialFiles`, `:2181`) list
  fields by hand. They have these defects:
  1. Only **loaded** scenes and prefab stages plus `prefabAssets` are fixed. A closed scene
     that references a renamed texture breaks.
  2. A move outside the editor breaks references silently.
  3. Delete **clears** referencing fields (`cleanupAssetFilePath` → `assetPath.clear()`,
     `:1328`), so undoing the delete does not restore the references in closed files, and
     the user is never told what lost its asset.
  4. Every new reference field needs a matching edit in 3–4 visitors.
- Thumbnails are keyed by a hash of path and size (`Project::getThumbnailPath`,
  `Project.cpp:6889`), so a rename has to move them (`RenameFileCmd::moveThumbnail`).
- There is no dependency graph, no Find References (the Project window context menu in
  `editor/window/ProjectWindow.cpp:1678-1735` has none), and no missing-asset UI except for
  prefabs (`ProjectPrefabMissing.cpp`, the Prefabs dialog).
- MCP: `list_assets`, `get_asset_info`, `move_asset` and `delete_asset`
  (`editor/mcp/tools/AssetTools.cpp:2880+`) are path-only. Prefab tools return GUIDs
  (`PrefabTools.cpp`).
- Export: `Factory`/`Generator` bake paths into generated C++ (for example
  `modelcomp.filename = "..."`, `editor/Factory.cpp:2460`). `Exporter::copyAssets`
  (`editor/Exporter.cpp:1736`) ships the assets dir, and `io/ResourcePack` packs it.
- `.jasperengine/` is git-ignored by the generated `.gitignore` (`Project.cpp:5231-5250`),
  so anything shared between machines must live outside it.

## 4. Reference (OpenEngine, MIT, read-only)

Root: `/var/lib/orquester/workspaces/jaspersito/JasperEngine-2/external/OpenEngine`.

| File | Take | Improve / skip |
|---|---|---|
| `Engine/Modules/AssetCore/Source/GUID.cpp:178-213` (`GUID::Derive`, `FnvHash`, `SplitMix64`) | **Copy** the derivation (FNV-1a 64 over parent bytes + `'\|'` + key, SplitMix64 for the upper word, version and variant bits) into `engine/core/asset/Guid.cpp`, with their header and a `NOTICE.md` entry. Freeze it with a golden test. | Our text form stays 32 lowercase hex without dashes, so existing prefab GUIDs stay valid. |
| `Engine/Modules/AssetCore/Include/AssetCore/SubassetDeriveKeys.h` | The idea of per-scheme key formatters as the single source of truth. | Their container GUID changes on rename, so they need a cascade of journaled keys. Ours does not change (the meta moves with the file), so derived GUIDs need no redirect cascade. Our keys prefer names over indices (§6.4). |
| `Engine/Modules/AssetCore/Include/AssetCore/DepEdge.h` | The `DepEdge {Referrer, Target \| TargetPath, Kind, FieldLocator, Ordinal}` shape and `IsValidDepEdge`. | Our own `DepEdgeKind` list (§6.6). Edges live in a JSON cache file, not SQLite. |
| `Engine/Modules/AssetDatabase/Include/AssetDatabase/RedirectChain.h`, `Source/RedirectChain.cpp` | The semantics: a self-redirect terminates, a cycle resolves to the queried GUID and warns. | We have no chains. A former GUID is stored on the target asset (§6.3), so resolution takes one hop by construction. |
| `Engine/Modules/AssetDatabase/Include/AssetDatabase/AssetStore_TextJsonl.h` | Study only: case-folded path identity and the deterministic sorted snapshot. | **Not ported.** See §6.2. |
| `Apps/Editor/Include/MissingAssetTracker.h`, `Source/MissingAssetTracker.cpp` (324 lines) | The entry shape (GUID, type, authored path, last-known path, owner, component, property), `ClearAllReferencesTo`, the listener and version counter. | Theirs scans the active world only. Ours is project-wide from the dependency index, covers prefabs, materials and project lists, and adds Retarget. |
| `Apps/Editor/Source/UndoRedo/RenameAssetFileCommand.cpp` | Refuse to overwrite an existing destination. Preflight paired files (our `.vert`/`.frag` base-name pairs). | Undo goes on the S24 single timeline. |
| `Engine/Modules/AssetCore/Source/PathNormalization.cpp` | Case-folding rules for path keys. | NFC normalization needs utf8proc, which we do not vendor. Default in §9. |

## 5. Requirements

### Functional
- **R1 Identity for every type.** Every regular file of a type listed in §6.1, under the
  project root and outside the skipped directories (names starting with `.`, `build/`, the
  same set as `Project::findProjectFiles`), has exactly one live GUID after
  `AssetDatabase::open` and after every `refresh`.
- **R2 Storage.** Engine-authored YAML assets (`.prefab`, `.scene`, `.material`, and any YAML
  type added later, such as S21 clip files) keep their top-level `guid:` key. Every other
  asset gets a sidecar `<file name>.meta` next to it (for example `hero.png.meta`), in the
  format in §6.2.
- **R3 Deterministic first GUID.** A file without identity gets `legacyGuid(relativePath)`
  if no live or former GUID already uses that value. Otherwise it gets a random GUID. Two
  machines that upgrade the same commit produce byte-identical meta files.
- **R4 Moves keep identity.** An editor rename or move (file or directory) moves the asset,
  its meta and its sub-asset table together, and every GUID stays the same. A move outside
  the editor that carried the meta (git mv, or a file manager moving both files) is a Move
  in `refresh()`.
- **R5 External moves without the meta.** When `refresh()` finds an orphan meta (its asset is
  gone) and a new file without a meta, the meta moves to the new file if all of these hold:
  same extension, same size, and (when the index has the orphan's content hash) the same
  content hash. If the cache has no hash, the file name and size must match one candidate
  only. Otherwise the orphan is kept, reported in the Missing Assets panel as "orphan meta",
  and the new file gets an identity per R3.
- **R6 Duplicates.** If two files carry the same GUID (a copy outside the editor, or a merge),
  the current rule stays: the file the cache names keeps it, else the older file, and the
  other file gets a new GUID with its sub-asset GUIDs re-derived. Editor copies keep
  `copyGuids` redo stability for all types.
- **R7 Sub-assets.** Model files (`.gltf`, `.glb`, `.obj`) list their sub-assets in the meta
  (`subAssets:`), with GUID = `Guid::derive(containerGuid, key)` (§6.4). The list is
  refreshed on import and when the file changes. Re-matching on change follows §6.4.
- **R8 Redirects.** An asset may list `formerGuids:`. Resolving a former GUID yields that
  asset (one hop). A GUID that is both live on asset A and a former GUID of asset B resolves
  to A, and a warning names both. Former GUIDs are added by "Retarget all" (R16), by an
  editor merge of duplicates, and by R11 path healing when the user confirms it.
- **R9 AssetRef.** Every asset-reference field holds an `AssetRef` (path hint and GUID) in
  memory and on disk (§6.5). This covers component fields, material textures and shaders,
  scene settings, the scene `prefabs:`, `childScenes` and `cppScripts` lists, the
  `project.yaml` scene list and standalone prefabs, and script properties.
- **R10 Resolution order** (editor, on load and on every refresh change):
  1. a live GUID;
  2. a former GUID;
  3. a sub-asset GUID (its container plus key);
  4. no GUID, a legacy reference: the asset with `legacyGuid(pathHint)`, else the file at
     the hint;
  5. **non-prefab types only**: an unknown GUID whose hint names an existing file of the
     expected type resolves to that file, flagged `resolvedByPath` (R11);
  6. Missing.

  Prefabs keep the PREFABS.md §3 rule (an unknown GUID is Missing) because relinking to the
  wrong prefab destroys overrides. When a reference resolves to a different path or GUID, the
  owning scene or prefab is marked modified (the existing `prefabRefsChanged` behaviour,
  generalized).
- **R11 Path healing.** References resolved at step 5 appear in the Missing Assets panel under
  "Resolved by path" with a "Keep" action, which adds the old GUID to the file's
  `formerGuids`. They are written with the new GUID on the next save of their owner.
- **R12 Dependency index.** A cache of `DepEdge`s (referrer GUID → target GUID or path, kind,
  `FieldLocator`, ordinal) is built from every scene, prefab, material, `project.yaml`, HTML,
  CSS and Lua file, plus model files for their internal texture URIs. It covers open and
  closed files. Edges of a loaded, modified scene come from memory, so the index reflects
  unsaved edits.
- **R13 Rename/move/delete/copy through one service.** `AssetOps` (§6.7) replaces the
  `remap*FilePath`/`cleanup*FilePath` family and the per-type dispatch in `RenameFileCmd`
  and `CopyFileCmd`. A rename or move rewrites **no file other than the moved assets and
  their metas**. Path hints in memory are refreshed in place. `project.yaml` is written only
  when an open-scene tab or the scene list hint changed.
- **R14 Text references.** Before a rename, move or delete, `AssetOps` lists the path-only
  edges (Lua require, HTML/CSS, shader include) that will break. The editor shows them in the
  confirmation dialog; MCP returns them as `text_references_broken`.
- **R15 Delete never clears fields.** Deleting an asset leaves references intact. They become
  Missing, and undoing the delete makes them resolve again. Missing Prefab behaviour is
  unchanged.
- **R16 Missing assets.** `MissingAssetTracker` lists every unresolved reference project-wide,
  grouped by missing GUID. Each group has the expected type, the last known path, and its
  referrers (owner asset, GameObject path, component, field). Actions:
  - Retarget all: adds the GUID to the chosen asset's `formerGuids` (no referrer file is
    rewritten).
  - Retarget selected: rewrites the chosen fields.
  - Clear selected / Clear all.
  - Locate: opens the owner and selects the GameObject.

  Prefab groups offer Replace Prefab instead of Retarget, and keep instance data.
- **R17 Find References.** Given an asset (or sub-asset), return every referrer edge, optionally
  transitive (who uses the material that uses this texture), from the index in ≤ 50 ms.
- **R18 Thumbnails** are keyed by GUID plus content fingerprint. `RenameFileCmd::moveThumbnail`
  is deleted.

### Platform & backends
- **R19** GUID derivation, `legacyGuid`, meta writing and key ordering are byte-identical on
  Windows, Linux and macOS, and in 32- and 64-bit builds (Android, iOS and wasm run the
  runtime map only). Meta files are written with LF line endings, UTF-8, and no BOM.
- **R20** The runtime GUID map (§6.8) works on every export target, including the HTML5 packed
  file system and `resources.pak`. Meta files and the dependency cache are never shipped.

### Performance (desktop SSD reference machine; the editor is desktop-only)
- **R21** Warm `open()` of a project with 10,000 assets and an intact cache takes ≤ 250 ms on
  the main thread. It does stat-only work; a meta is read only when its size or mtime changed.
- **R22** A cold `open()` (no cache) with 10,000 assets and 1,000 YAML referrers takes ≤ 3 s,
  with the parse on S06 background jobs. The editor is usable after the identity pass
  (≤ 1 s); the index finishes in the background, and the panels show "Indexing…" until then.
- **R23** Incremental update for one changed referrer takes ≤ 5 ms plus its YAML parse.
  `refresh()` is event-driven from S05. The fallback poll costs ≤ 2 ms per frame amortized.
- **R24** Editor GUID resolution averages ≤ 200 ns (hash map). Runtime map lookup is
  O(log n), lazily loaded, and costs ≤ 24 bytes per asset plus path strings. A game that never
  calls a GUID API pays nothing.
- **R25** `AssetRef` adds ≤ 16 bytes per field over `std::string`. No per-frame cost.

### Editor UX (human)
- **R26** The Project window hides `.meta` files, always. Rename, move, copy, paste, delete and
  drag-to-folder carry metas. "Copy GUID" and "Find References" are added to the file context
  menu.
- **R27** Window > Missing Assets (also reachable from a status bar badge showing the count)
  shows the panel for R16 and R11 and the orphan metas. All actions are undoable on the S24
  timeline, one entry per action.
- **R28** The Inspector's asset fields show missing references in red with the last known path
  and a "Missing (GUID…)" tooltip, and keep the reference until the user clears it.
- **R29** Find References opens a References panel listing owner, GameObject path, component
  and field. Double-click opens or focuses the owner and selects the GameObject.

### Agent UX (MCP, Lua, CLI)
- **R30** New MCP tools (§6.9): `resolve_asset`, `find_references`, `get_asset_dependencies`,
  `list_missing_assets`, `retarget_asset_references`, `clear_asset_references`. Existing tools
  accept a GUID wherever an asset path is accepted, and return `guid` next to every asset
  path (`list_assets`, `get_asset_info`, `move_asset`, `delete_asset`, `import_files`,
  `link_material`, the component get/set tools for asset fields).
- **R31** Lua (§6.10): an `Asset` module, an `AssetRef` userdata, and a typed `Asset` script
  property. `PrefabAsset` stays and is an `AssetRef` of type prefab.
- **R32** CLI: `jasperengine-editor assets check --project P` (exit code 1 when anything is
  missing, JSON on stdout) and `jasperengine-editor assets migrate --project P` (R35). Both
  route through the S31 call CLI when it lands.

### Data & compatibility
- **R33** Old projects load unchanged. The first open writes metas (R3) and in-file GUIDs for
  scenes and materials (the existing `writeGuid` path). It rewrites no referrer.
- **R34** Readers accept both the old forms (plain path strings, texture nodes without `guid`)
  and the new ones. Writers always write the new form. Scene, prefab and material format
  versions bump through S03 with a migration step that fills GUIDs by R10 step 4.
- **R35** `assets migrate` (also Project > Asset Database > Migrate References) rewrites every
  closed scene, prefab, material and `project.yaml` in the new form, through S03 atomic
  writes. It is idempotent: a second run writes nothing.
- **R36** Meta files preserve unknown keys and sections byte-stably (S03 unknown-data rule), so
  S14/S25 sections written by a newer editor survive an older one.
- **R37** No volatile data in meta files: no mtimes, no hashes, no machine paths. A meta is
  rewritten only when its semantic content changes.

## 6. Design constraints & decisions

### 6.1 Asset type table (replaces `AssetDatabase::isTrackedFile`)
`editor/asset/AssetTypes.{h,cpp}` holds one table and nothing else decides tracking:

| type | extensions | identity | path base | sub-assets |
|---|---|---|---|---|
| prefab | .prefab | in-file | project | – |
| scene | .scene | in-file | project | – |
| material | .material | in-file | project | – |
| texture | `Util::isImageFile` set (terrain maps are textures) | sidecar | assets | – |
| model | .gltf .glb .obj | sidecar | assets | mesh, material, animation, skin, image |
| audio | .wav .ogg .mp3 .flac | sidecar | assets | – |
| font | .ttf .otf | sidecar | assets | – |
| ui_document / stylesheet | .html .htm .rml / .css .rcss | sidecar | assets | – |
| shader | .vert .frag .glsl | sidecar | project | – |
| script | .lua, C++ script .h/.cpp (`Util::isScriptFile`) | sidecar | project | – |
| data | .json .txt .csv .xml .bytes (under the assets dir only) | sidecar | assets | – |

The path base preserves today's semantics of each field: path hints stay relative to the
same root they use now. Excluded: `project.yaml`, the generated `CMakeLists.txt`, `.meta`
files, and the skipped directories.

### 6.2 Storage model: per-asset identity, not a central DB (decided)
**Decision: hybrid per-asset storage.** Engine YAML assets carry `guid:` in-file. Everything
else gets a `.meta` sidecar. **OpenEngine's `AssetStore_TextJsonl` single file is rejected.**
Reasons:
1. Merge friendliness. Every add, move or delete in their store appends lines to one shared
   file, so any two branches that touch any assets conflict at end of file. With per-asset
   identity, two branches conflict only when they change the same asset.
2. `git mv`, file managers and directory moves carry the identity with the file. A central DB
   needs a reconciler for every out-of-band change.
3. It continues the prefab design already in production (PREFABS.md §3) and needs no
   compaction or cross-process lock.
4. The meta is the natural home for S14 import settings and S25 labels, which are per-asset
   and must be versioned.

Costs and mitigations:
- File litter: the Project window hides metas (R26).
- Forgotten metas: R3 makes regeneration deterministic, and S29 warns when an asset and its
  meta are staged apart.
- Scan cost: R21 skips unchanged metas using cache fingerprints.

The index (`.jasperengine/assetdb/index.json`, replacing `.jasperengine/assetdb.yaml`) is a
derived, deletable cache. It holds path↔GUID, per-file fingerprints (size, mtime, xxHash64
of content for assets ≤ 64 MB) and the edges. Deleting it changes no resolution result
(AC5).

Meta format (YAML, keys in this order; empty sections omitted):
```yaml
metaVersion: 1
guid: 4b1e9c0d2a7f4e31b6c58d0f9e2a7c13
formerGuids: [9c1d0f3e5b7a42c8a1e06d2f4b8c7a19]   # optional, sorted
subAssets:                                         # models only, sorted by key
  - {key: "mesh:Body", guid: 7f..., name: Body}
  - {key: "animation#3", guid: 21..., name: ""}
import: {...}                                      # S14, opaque to S04
labels: [...]                                      # S25, opaque to S04
```
In-file assets may carry `formerGuids:` as a top-level key too (written by the same
line-edit helper as `writeGuid`).

### 6.3 Identity types
```cpp
// engine/core/asset/Guid.h — 16 bytes, trivially copyable, hashable
struct Guid { uint64_t hi = 0, lo = 0;
  static Guid parse(std::string_view hex32);   // accepts the existing 32-hex prefab form
  std::string str() const;                     // 32 lowercase hex, no dashes
  static Guid derive(const Guid& parent, std::string_view key); // OpenEngine Derive, frozen
  bool isNull() const; auto operator<=>(const Guid&) const = default; };
```
`AssetDatabase::legacyGuid` keeps its SHA-1 formula, which is frozen because existing
prefabs depend on it. Random GUIDs keep the existing `mt19937_64` generator. There are no
redirect chains: former GUIDs are indexed straight to their current owner.

### 6.4 Sub-asset keys
The key is `"<kind>:<name>"` when the name is non-empty and unique among that kind in the
file. Otherwise it is `"<kind>#<index>"`. Kinds are `mesh`, `material`, `animation`, `skin`
and `image`. The first GUID is `Guid::derive(container, key)`, so two machines agree without
the meta, and it is then persisted in `subAssets:`. When the file changes, entries are
matched by key first. An unmatched old entry is then matched to an unmatched new entry of the
same kind at the same index. Remaining old entries are dropped, and their references become
Missing. The key formatters live in one header (`engine/core/asset/SubAssetKeys.h`) and are
frozen.

### 6.5 `AssetRef` and serialization
```cpp
// engine/core/asset/AssetRef.h
struct AssetRef { std::string path; Guid guid;
  AssetRef& operator=(std::string p);          // keeps C++ script/generated code compiling
  operator const std::string&() const; bool empty() const; const char* c_str() const; };
```
Engine component fields that hold asset paths become `AssetRef`. The runtime keeps loading
by `path`, so pools are not touched. `Texture` stores one `Guid` per source path slot (6 for
cube maps). Serialization rules:
- Map-shaped references get a `guid` key inside the map: texture nodes, and `prefab:
  {name, path, guid}` as today.
- Scalar path fields keep their key and value and gain a sibling `<key>Guid` (for example
  `filename: models/a.glb` + `filenameGuid: 4b1e…`). Diffs stay minimal and old readers keep
  working by path.
- `customShader` gains `customShaderGuid: "<vertGuid>|<fragGuid>"`.
- The linked material file moves out of `Material::name` into `materialFile: AssetRef`. The
  old form is read through the S03 migration.

### 6.6 Dependency index and field locators
```cpp
// editor/asset/DepEdge.h  (shape from OpenEngine DepEdge.h)
enum class DepEdgeKind : uint16_t { Other, SceneComponent, PrefabComponent, MaterialTexture,
  MaterialShader, MeshMaterialFile, SceneSettings, SceneList, ChildScene, ScenePrefabList,
  ScriptProperty, CppScript, UIStylesheet, UIImage, LuaRequire, ShaderInclude, ModelUri };
struct DepEdge { Guid referrer; Guid target; std::string targetPath; DepEdgeKind kind;
                 std::string field; uint32_t ordinal; };
```
FieldLocator grammar: `<scope>/<Component>.<field>[<i>].<field>`. The scope is
`entity:<file entity id>` (the `entity:` key of scene and prefab files, which is stable per
PREFABS.md §2), `settings`, `material`, `project` or `text:<line>`. Examples:
`entity:12/MeshComponent.submeshes[0].material.baseColorTexture` and
`project/scenes[3]`.

One table, `editor/asset/AssetRefSchema.cpp`, lists every reference field: component, YAML key
path, expected type and kind (S73 generates its component rows from the reflection tables). It
provides:
- `forEachAssetRef(EntityRegistry&, fn(AssetRef&, const FieldLocator&, AssetType))`, the
  single visitor that replaces `visitAssetPathsInRegistry`, `applyCustomShaderPathChange`,
  `remapScriptPathsInRegistry` and `visitAssetPathsInMaterialFiles`;
- the closed-file YAML extractor that feeds the index.

A new reference field of a component only needs its field in the reflection table. The index lives only in memory and in the cache JSON, with no SQLite.

### 6.7 `AssetOps` (editor/asset/AssetOps.{h,cpp})
`move(from, to)`, `copy(from, to, copyGuids*)`, `remove(path)` (to `.trash`, metas included)
and `restore(path)`. Each one does three things:
1. moves or copies the asset and its meta atomically (S03 writer, rename with retry);
2. updates `AssetDatabase`;
3. walks the loaded scenes, stages and `prefabAssets` with `forEachAssetRef` to refresh path
   hints for the affected GUIDs.

It returns `{changes, textReferencesBroken}`. `RenameFileCmd`, `CopyFileCmd`, `DeleteFileCmd`
and MCP `move_asset`/`delete_asset` call it. The `remap*`/`cleanup*` members of `Project` are
deleted, except the scene-tab and project-list hint update, which moves into `AssetOps`.

### 6.8 Export and runtime
- Generated code and cooked scene data always hold **resolved current paths**. The exporter
  resolves every `AssetRef` and fails the export, listing each one, on any Missing reference
  in a scene in the build. "Export anyway" is a per-build override. The runtime never
  resolves GUIDs for scene data.
- The exporter writes `assetmap.bin` (in the pack when the pack is enabled). It is a sorted
  array of `{Guid, uint32 pathOffset}` plus a string blob, covering every shipped asset, its
  former GUIDs and its sub-assets (sub-asset entries point at `container#key`).
  `engine/core/asset/AssetMap` loads it lazily on the first GUID query. The S30 prebuilt
  player uses the same map when scene data carries GUID-only references.
- `.meta` files and `.jasperengine/assetdb` are excluded by `copyAssets` and the pack builder.

### 6.9 MCP tools (editor/mcp/tools/AssetTools.cpp)
- `resolve_asset {asset: path|guid}` → `{guid, path, type, exists, resolved_by: live|former|sub_asset|legacy_path|path_fallback|missing, former_guids, sub_assets[{key,guid,name,kind}]}`
- `find_references {asset, transitive=false, limit=500}` → `[{referrer:{path,guid,type}, entity:{id,path}?, component?, field, locator, kind}]`
- `get_asset_dependencies {asset, depth=1}` → the outgoing edges in the same shape.
- `list_missing_assets {scope: project|open_scenes = project}` → `[{guid, expected_type, last_path, referrers[...]}]`, `orphan_metas[]`, `resolved_by_path[]`
- `retarget_asset_references {from: guid, to: path|guid, mode: redirect|rewrite = redirect, referrers?: [locator…]}`
- `clear_asset_references {guid, referrers?}`

All mutating tools are undoable and report `files_written`. Update the MCP agent guide
(`McpInstructions.cpp`) and `docs/mcp`.

### 6.10 Lua API
- `Asset.path(guid) -> string|nil`, `Asset.guid(path) -> string|nil`,
  `Asset.exists(guidOrPath) -> bool`, `Asset.ref(guidOrPath) -> AssetRef`.
- `AssetRef` has `.guid`, `.path`, `.type` and `.valid`.

These are backed by `AssetDatabase` in Play and by `AssetMap` in exported games.
`ScriptPropertyType::Asset` takes an `assetType` filter (`texture`, `model`, `audio`, …). Its
value is an `AssetRef`, serialized as `{guid, path}`. The Prefab property type stays and is
read as `Asset` with filter prefab. Regenerate the Lua stubs (`generate_lua_stubs.py`) and the
single API description (S23).

## 7. Acceptance criteria

Unit tests are gtest (S02) under `editor/tests/asset/` and `engine/tests/asset/`. MCP tests go
in `tools/mcp/tests/test_asset_identity.py`.

- **AC1 (R1, R2, R3, R33)** `AssetIdentity.UpgradeIsDeterministic`: two copies of fixture
  `tools/fixtures/legacy_paths_project` are opened in two temp dirs. Every typed file has
  exactly one GUID, every meta is byte-identical between the copies, and the meta GUIDs equal
  `legacyGuid(path)`. No scene, prefab or material file changes except the inserted `guid:`
  line.
- **AC2 (R4, R13)** `test_asset_identity.py::test_rename_touches_no_referrers`: a texture used
  by a closed scene, an open scene, a prefab and a material is renamed via `move_asset`.
  `files_written` is exactly {the texture, its meta}, the mtimes of all referrer files are
  unchanged, and reopening the closed scene shows the texture bound (`resolve_asset` returns
  `live`).
- **AC3 (R5)** `AssetDatabase.ExternalMoveWithoutMeta`: the file is moved on disk with the meta
  left behind, then `refresh()` runs. The meta is adopted and the GUID is unchanged. A variant
  with two same-size candidates and no hash keeps the orphan, which is listed by
  `list_missing_assets.orphan_metas`.
- **AC4 (R4)** `AssetDatabase.ExternalDirectoryMove`: a directory with 50 assets and metas
  moved outside the editor gives 50 Moved changes and no GUID changes.
- **AC5 (R12, §6.2)** `AssetIndex.CacheIsDisposable`: deleting `.jasperengine/assetdb/`
  produces identical `resolve_asset` and `find_references` results for every asset of the
  fixture after reopen.
- **AC6 (R6)** `AssetDatabase.DuplicateMetaResolution`: an `a.png` + meta copied to `b.png`
  outside the editor leaves `a` keeping the GUID and `b` with a new one. For a model, all of
  `b`'s sub-asset GUIDs equal `derive(newGuid, key)`.
- **AC7 (R7, §6.4)** `SubAssets.StableKeys`: on a glTF fixture with named and unnamed meshes,
  re-importing after reordering the named meshes keeps every GUID. Renaming an unnamed-index
  mesh keeps its GUID by index match. `GuidDerive.GoldenVectors` checks 10 fixed
  (parent, key) → GUID pairs, and the same vectors run in the wasm and 32-bit Android unit
  builds.
- **AC8 (R8, R16)** `test_asset_identity.py::test_retarget_all_is_redirect`: a texture with
  three referrers in closed files is deleted, then
  `retarget_asset_references {mode: redirect}` points them at another texture. Only the
  target's meta is written, all referrers resolve with `resolved_by: former`, and undo
  restores the Missing state.
- **AC9 (R10, R11)** `AssetResolve.PathFallbackNonPrefab`: a reference with an unknown GUID
  and a hint pointing at an existing PNG resolves `path_fallback` and appears in
  `resolved_by_path`. The same setup with a `.prefab` gives Missing (the PREFABS.md rule).
  The existing `tools/prefab-fixtures.py` suite passes unchanged.
- **AC10 (R15)** `test_asset_identity.py::test_delete_keeps_references`: deleting a model used
  by a closed scene leaves the scene file byte-identical. `list_missing_assets` names the
  scene, the entity path and `ModelComponent.filename`, and undoing the delete empties the
  list.
- **AC11 (R17, R29)** `find_references` on a texture used via a material file returns the
  material edge, plus the scene edges with `transitive: true`. A 10k-asset generated
  benchmark project (`tools/performance/assetdb_bench.py`) answers in ≤ 50 ms.
- **AC12 (§6.6)** `AssetRefSchema.CoversAllFields`: every asset property `Catalog` lists for every
  component has a row. `AssetRefSchemaFrozen.EveryRowOfTheHandTable` (S73) replaces the explicit
  member list this test had.
- **AC13 (R21, R22, R23)** `assetdb_bench.py` on the reference machine: warm open ≤ 250 ms,
  cold identity pass ≤ 1 s, full index ≤ 3 s, single-file update ≤ 5 ms + parse. These are
  reported in CI as a benchmark, and the thresholds are checked on the nightly job.
- **AC14 (R34, R35)** `test_asset_identity.py::test_migrate_idempotent`: `assets migrate` on
  the legacy fixture rewrites each referrer once with `guid`/`<key>Guid`. A second run writes
  0 files. A scene dump (`scene-dump`) before and after is equal apart from the GUID keys.
- **AC15 (R36, R37)** `AssetMeta.PreservesUnknownSections`: a meta with `import:`, `labels:`
  and a `futureKey:` survives a rename and a former-GUID add byte-for-byte outside the edited
  keys. Re-saving an unchanged project changes no meta's mtime.
- **AC16 (R20, §6.8)** `tools/smoke-test.sh` export of the fixture: the build output contains
  no `.meta` and contains `assetmap.bin`. The Lua test script prints
  `Asset.path(<guid>)` correctly in the Linux and HTML5 headless players. An export with a
  Missing reference fails, listing it.
- **AC17 (R18, R26)** Manual check: in the Project window, rename `hero.png` to `h2.png`. The
  thumbnail appears immediately without regeneration (no thumbnail job is logged), no `.meta`
  is visible, and the context menu shows Copy GUID and Find References.
- **AC18 (R27, R28)** Manual check: delete a texture used by the open scene. The status badge
  shows 1, and Window > Missing Assets lists it with the GameObject path. The Inspector field
  is red with the last path. Retarget to another texture: the scene view updates, and Ctrl+Z
  shows it missing again.
- **AC19 (R31)** `engine/tests` Lua test: an `Asset` script property set in the editor
  survives renaming its file and resolves in Play and in the exported game. `PrefabAsset`
  scripts from `tools/prefab-*` still pass.
- **AC20 (R14)** `move_asset` on a CSS file linked from an HTML document returns
  `text_references_broken` with the HTML path and line, and does not rewrite the HTML.

## 8. Dependencies & conflicts

- **Depends on:**
  - S03 (atomic writes for metas and migrations, format version bump and migration step,
    unknown-data preservation).
  - S02 (gtest).
  - S06 (background index jobs).
  - S05 (watcher events feed `refresh()`; meta and cache writes must be registered as
    expected writes so they are not echoed back). S04 can ship first on the existing 2 s
    poll, extended to all types, within R23's budget.
- **Coordinates with:**
  - S08: FieldLocator syntax and the `AssetRef` reflected field type. S73 generates the
    `AssetRefSchema.cpp` component rows from its tables and keeps the test (AC12).
  - S14: owns `import:` in metas.
  - S25: `labels:` in metas, and an asset filter `ref:`/`missing:`.
  - S29: the commit dialog pairs an asset with its meta. `*.meta` is never LFS-tracked.
    Generated `.gitattributes` gets `*.meta text eol=lf`. The semantic scene diff shows
    GUID refs by resolved path.
  - S24: all `AssetOps` and Missing-panel actions go on the single undo timeline.
  - S21: animation clips as sub-assets / future `.anim` YAML in-file GUIDs.
  - S23: Lua API description.
  - S30: the prebuilt player uses `AssetMap`.
  - S31: CLI.
- **Blocks:** S05 reload propagation (uses edges), S14 (meta container), S25 asset labels,
  S29 overlays on metas, S21 cross-file clip references.
- **Merge hotspots:**
  - `editor/Project.{h,cpp}` (deleting the `remap*`/`cleanup*` family, ~600 lines);
  - `editor/Stream.cpp` (every reference encoder and decoder);
  - `editor/Factory.cpp` / `Generator.cpp` (paths from `AssetRef`);
  - `editor/command/type/{Rename,Copy,Delete}FileCmd.cpp`;
  - `editor/window/ProjectWindow.cpp` (S25 also edits it);
  - `editor/window/InspectorWindow.cpp` (S08 also edits it);
  - engine component headers (S07 and S08 also touch them);
  - `editor/mcp/tools/AssetTools.cpp`;
  - `editor/project/PREFABS.md` §3 (update the contract text to point at this spec);
  - `NOTICE.md`.

## 9. Risks & defaults

- **`AssetRef` in engine components breaks user C++ code** that passes a field as
  `std::string&`. Default: the implicit conversions in §6.5 and a release note. Generated code
  compiles unchanged. If the S01 C++20 build of the sample projects shows breakage beyond
  `std::string&` out-parameters, keep the fields as `std::string` and add `<field>Guid`
  members instead. The serialization in §6.5 is identical either way.
  **This is the biggest risk**, because it touches the most files.
- **Path fallback (R10 step 5) binds the wrong file** after an unrelated file replaced a
  deleted one. Default: accepted for non-prefab types. The binding is visible in "Resolved by
  path", and "replace file in place keeps references" is the common intent.
- **Meta forgotten in a commit.** Default: R3 makes the regenerated GUID equal on every machine
  only for legacy-derived GUIDs. Random GUIDs (files created in the editor after upgrade) are
  healed by R10 step 5 plus S29's staging warning.
- **Unicode normalization (macOS NFD names).** Default: no NFC normalization (no utf8proc).
  Path keys are case-folded only on case-insensitive volumes (detected per project at open).
  Revisit if a duplicate-path bug is reported.
- **Closed-file rewrites (Retarget selected, Clear, migrate)** go through the full scene,
  prefab and material load/encode path, not YAML text patching, so formatting matches normal
  saves. Default: these run as a background job with a progress bar, and undo restores the
  previous bytes captured before the write.
- **Index size** for huge projects (100k files). Default: JSON cache. If a load exceeds 500 ms,
  switch the cache to a binary format behind the same interface, still without SQLite.
- **New files created during Play** are registered only after Play stops (existing rule: no
  refresh while playing). Kept.

## Deviations (implementation)

Recorded by the implementer; each is the closest correct alternative to the text above.

1. **Linked material file.** `Material::name` (the linked `.material` path, the only meaning
   it had in the editor) became an `AssetRef` and keeps its key, serialized `name` +
   `nameGuid` (§6.5's scalar rule), instead of moving to a new `materialFile` key. `name` is
   part of prefab override paths (`submeshes[i].material.name`, `SubmeshOverride_MaterialName`)
   in closed scenes; renaming the key would have needed a migration of every override record
   for no gain in identity. No format migration is needed as a result.
2. **Format versions.** No scene, prefab or material version bump and no S03 migration step
   (R34). Every key S04 adds is optional (`<key>Guid`, texture `guid`/`guids`,
   `guid`/`formerGuids` in scenes, prefabs and materials), and S03 R22 bumps no version for
   optional keys; readers accept both forms directly. Filling GUIDs (R10 step 4) needs the
   project's assets directory and its asset database, which S03's pure YAML -> YAML
   migrations do not have, so it happens in the load-time resolution pass and on save. The
   S04 keys are in S03's `StreamSchema` known-key tables (`guid`/`formerGuids` at the root,
   texture `guid`/`guids`, and `<key>Guid` wherever `<key>` is known), so load reports do not
   list them as unknown and preservation does not copy them. Writes use S03's primitives:
   metas, in-file GUID lines and the index cache through `io::writeFileAtomic`
   (`editor/asset/AssetFileWrite`), moves through `io::renameWithRetry`, scenes and prefabs
   through `DocumentFiles::write`, and `.material` files through `DocumentFiles::saveMaterial`
   with its lossy-save gate.
3. **Closed-file rewrites** (Retarget Selected, Clear, `assets migrate`) edit the YAML tree
   at the field's node and write it back with the emitter normal saves use, rather than a
   full decode/encode. This keeps unknown data (S03's preservation rule), reaches prefab
   instance override records, and loads no scene. Undo restores the bytes captured before the
   write, as §9 asks. Loaded owners are edited in memory; their undo restores component and
   scene-settings snapshots.
4. **Cache files.** The index is split into `.jasperengine/assetdb/index.json` (identity,
   fingerprints, content hashes) and `deps.json` (edges), so the warm open (R21) parses only
   the identity part. Both are deletable caches.
5. **Index overlay.** Every loaded document (scene, prefab stage, loaded prefab asset)
   contributes its in-memory edges, not only modified ones. The locators of a loaded owner
   are then always the ones the editor edits.
6. **Child scenes** stay referenced by scene id (project-wide ids that the scene list maps to
   files); their edges are derived through `project.yaml`. The scene list entries gain
   `filepathGuid`, so a moved scene file is found by GUID on load. A scene's `cppScripts` list
   is regenerated from its Script components on every save; its edges come from the
   components and the list.
7. **Shader references** are a `ShaderRef` (a path plus the `.vert` and `.frag` GUIDs) for
   every custom shader field, because one value names two files (`customShaderGuid:
   "<vert>|<frag>"`, also for material and default shaders).
8. **Prefab references** keep their existing types (`PrefabComponent{name, path, guid}`,
   `PrefabAsset`): they already carry identity, and the schema adapts them. Prefab references
   resolve through the prefab code (PREFABS.md §3), which owns Missing Prefab state.
9. **Delete.** Deleting a closed scene's file keeps it in the project scene list, where it is
   reported Missing, instead of removing the scene from the project (R15); undo resolves it.
   A copy pasted over an existing name gets Unity's free name (`hero 1.png`) instead of
   silently overwriting. `import_files overwrite: true` replaces the content in place and
   keeps the file's identity, so its references stay.
10. **Menus.** There is no Project menu: Migrate References is under Assets > Asset Database,
    and Missing Assets is under Window > Assets (and Assets > Asset Database).
11. **Thumbnails** are keyed by GUID plus size and mtime (the content fingerprint that needs no
    read). A rename or move keeps both, so no thumbnail regenerates.
12. **Export gate scope.** The Missing check covers the scenes of the project list plus the
    prefabs and materials they reference, transitively.
13. **MCP parameters.** `resolve_asset` also takes `path` (a hint for a GUID) and `type`. The
    `referrers` of `retarget_asset_references` / `clear_asset_references` are locators or
    `"<owner>|<locator>"` (a locator is per owner). The component tools return asset GUIDs as
    `asset_guids` (and `guid` in detailed entries), so set-tool round trips are unchanged.
    S31's tool groups do not exist yet; the six tools join `assets_pipeline` when S31 lands.
14. **Benchmark.** `tools/performance/assetdb_bench.py` drives a developer subcommand,
    `jasperengine-editor assets bench --project P`, which times the database and the index
    without the engine. Measured on the benchmark-only server E (4 dedicated vCPU, nothing
    else running, load ~1; 11,000 assets, 1,000 referrers; median of 3): warm open 121 ms
    (budget 250), GUID resolution 59 ns (budget 200), one saved referrer 6.0 ms, cold
    identity pass 255 ms, full index 657 ms, worst Find References 1.3 ms, the poll's walk
    150 ms (Background job) and its main-thread part 11 ms every 2 s.
15. **Tests location.** The unit tests are where S02 puts them (`engine/tests/core/asset/`,
    `editor/tests/asset/`), built into `jasperengine-core-tests` and
    `jasperengine-editor-tests` (`tools/build.sh unit-tests`).
16. **HTML5 run of AC16** has no script of its own: the smoke test's export of the fixture is
    built with `emcmake cmake` in the web image and opened with S02's
    `tools/web/browser_gate.py --expect "[S04] Asset.path ..." --expect "[S04] portrait ..."`.
17. **Excluded files.** Besides `project.yaml` and `CMakeLists.txt`, the project root's
    `ProjectBuild.cmake` and `compile_commands.json` are not assets, and neither are hidden
    files (S03's temporaries).
18. **The poll (R23).** Until S05's watcher feeds `refresh()`, the editor polls every 2 s (and
    never more than a fifth of the time): `AssetDatabase::scanDisk()` walks and stats in an
    S06 Background job, `refresh(scan)` applies it on the main thread and drops a scan older
    than the database's last change. Tools that answer from the database
    (`resolve_asset`, `find_references`...) refresh first, so files an agent changed with its
    own tools count at once. A save updates its entry and its edges in place; the index cache
    is written by the database's Background strand (`jobs::SerialQueue`, after the content
    hashes queued before it, and on close), not by each save. The index build is a Background
    job whose referrer parse is a `jobs::parallelFor` (R22).
19. **First open of a project written before GUIDs** writes one meta per asset. Metas that hold
    only what their path derives (legacy GUID, derived sub-assets) skip the sync, since a crash
    rewrites them identically; it is still a write per asset (about 5 s for 25,000 assets on
    the build server, 1 s to open it again), which R22's 1 s identity budget (measured with
    metas present) does not cover.
