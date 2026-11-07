# Asset identity: GUIDs, references, missing assets

Every asset of a project has a GUID: a 128-bit id, written as 32 lowercase hex digits, that
stays the same when the file is renamed or moved, in the editor or outside it. Scenes, prefabs,
materials and `project.yaml` reference assets by GUID and keep the path as a readable hint, so
a rename or a move rewrites no file but the moved asset and its meta. The full design is in
`docs/specs/S04-asset-identity.md`.

## Where the GUID lives

| Asset | Identity |
|---|---|
| `.prefab`, `.scene`, `.material` | a top-level `guid:` key in the file (and `formerGuids:` right after it) |
| everything else: textures, models, audio, fonts, HTML/CSS, shaders, Lua and C++ scripts, data files (`.json .txt .csv .xml .bytes`, in the assets directory only) | a `<file name>.meta` file next to it (`hero.png.meta`) |

A meta file looks like this:

```yaml
metaVersion: 1
guid: 4b1e9c0d2a7f4e31b6c58d0f9e2a7c13
formerGuids: [9c1d0f3e5b7a42c8a1e06d2f4b8c7a19]   # optional, sorted
subAssets:                                         # models only, sorted by key
  - {key: "mesh:Body", guid: 7f2c..., name: Body}
  - {key: "animation#3", guid: 21d0..., name: ""}
```

- Commit the metas with their assets. The Project window hides them and moves, copies and
  deletes them with their assets. Sections the editor does not know (`import:`, `labels:`, or
  anything a newer editor wrote) are kept byte for byte. A meta holds no mtimes or machine
  paths, and is rewritten only when its content changes.
- The first open of a project written before GUIDs gives every file the GUID derived from its
  path (`legacyGuid`), so two machines upgrading the same commit write identical metas. It
  inserts a `guid:` line into scenes, prefabs and materials and rewrites no reference.
- `.jasperengine/assetdb/` (`index.json`, `deps.json`) is a cache. Deleting it changes no result.

## Sub-assets

The meshes, materials, animations, skins and embedded images of a `.gltf`, `.glb` or `.obj`
are sub-assets. A key is `<kind>:<name>` when the name is unique among that kind in the
file, else `<kind>#<index>`. A sub-asset's first GUID is derived from the model's GUID and
the key. When the model changes, entries match by key, then by kind and index; what matches
nothing is dropped, and its references become missing. A reference to a sub-asset uses the
path hint `<model path>#<key>`, for example `models/hero.glb#mesh:Body`.

## How a reference resolves

1. A live GUID: the asset that has it now.
2. A former GUID (`formerGuids` of an asset, written by "Retarget All" or "Keep"): that asset.
3. A sub-asset GUID: the sub-asset.
4. No GUID (written before GUIDs): the asset whose legacy GUID is the hint's, else the file at the hint.
5. An unknown GUID whose hint names an existing file of the expected type: that file, listed
   under "Resolved by path" (not for prefabs: a GUID no prefab has is a Missing Prefab).
6. Missing.

A reference that resolves to a new path gets the new hint in memory. A loaded scene is then
marked modified, and the next save writes the new path. A path you assign in the Inspector, in
a tool or in code gets its asset's GUID when the scene is saved.

In files, a reference keeps its key and value and gains a GUID next to it:

```yaml
model:
  filename: models/hero.glb
  filenameGuid: 4b1e9c0d2a7f4e31b6c58d0f9e2a7c13
mesh:
  customShader: shaders/water                  # shaders/water.vert + shaders/water.frag
  customShaderGuid: 7f2c...|21d0...            # the .vert's GUID | the .frag's
  submeshes:
    - material:
        baseColorTexture: {textureType: 2D, source: path, path: textures/a.png, guid: 9c1d..., ...}
        name: materials/Rock.material           # the linked .material file
        nameGuid: 0a4f...
```

## Linked folders and files

A folder or file link inside the project (a symbolic link, an NTFS junction, a mount point) works
the way a real folder does. One file is one asset, with one GUID, whatever the paths it is reached by.

- **A link to a place inside the project** is a second name. Nothing is tracked under it and it
  gets no metas: the asset stays at its real path. A lookup, a reference or an instance made by
  the link's path names the asset by its GUID and stores the real path as its hint.
  A file link is the file it leads to, and `list_prefabs` shows it once.
- **A hard link of a tracked file** is compared by file identity (`std::filesystem::equivalent`), so
  it is never taken for a copy and gets no new GUID. The path tracked first stays the asset's, and
  no meta is written for the other. A real copy (another file with the same GUID) gets a new GUID.
- **A link to a place out of the project** is a project folder, as in Unity. Its assets are tracked
  under the link's name (`assets/shared/tile.png`), each `.meta` lies beside the file in the folder
  the link leads to, and a linked file's meta beside the link. The scan goes through links inside it too.
  Two links to one such folder are one folder: the first name in path order has the assets, and
  the second is another name of them. This is for links under the assets or Lua directory: a link
  beside them (`lib` at the project root, with the assets in `assets/`) can be read, but is not
  scanned, gets no metas and ships only its shader sources.
- **A project root swapped for a link** behind the editor's back (`assets` renamed and a link left at
  its name) is not followed: the database neither scans nor writes through it, with a warning, and an
  export fails with an error, until it is put back or the project is opened again.
- **A folder moved and linked at its old name** is a move: GUIDs, bytes and references survive, the hints follow.
- **A cycle** ends the walk. A link back to a folder already walked is another name of it; a link
  to a folder that holds the project (`assets/up` leading to the project root) is left out, with
  a warning. The walk through links stops at 500,000 entries, with a warning. A link that leads
  nowhere is a missing file, silently.
- **A read-only target** is no failure. Assets there work: a file without an identity on disk gets the
  GUID its path derives, kept by the database, so it is the same every session. Missing Assets lists
  them under "Read-only linked files" (`read_only_linked` in `list_missing_assets`). Give the folder
  write access and the metas are written on the next open.
- **Changes through a link** are seen: the file watcher watches each folder linked in from out of
  the project from its own real path, and reports its files by their project paths.
- **Exports** ship every file the editor reaches through a link, at the path the editor reads it by, in
  every mode: assets, Lua and custom shader sources. A link to the project's own folder ships under
  both names. A linked file ships inside the export tree. A link that would hold itself is left out,
  with a warning naming it.

**MCP write tools stay inside the project.** The agent write sandbox (`write_file`, `move_asset`,
`delete_asset` and the others, `docs/mcp/DESIGN.md`) refuses a path that resolves through a link out of the
project, and `search_files` does not read through one: reading, referencing, listing and exporting work, and the
database's own metas are written beside the files. To let an agent edit such a folder, copy it into
the project. Links inside the project are written through.

## Renaming, moving, deleting

- **In the editor** (the Project window, `move_asset`, `delete_asset`): the file and its meta
  move together, and every GUID stays. No referrer is rewritten, the open scenes' hints
  update in memory, and thumbnails are kept.
- **Outside the editor**: a move that carries the meta (`git mv`, a file manager moving both)
  is recognized as a move. A file moved without its meta adopts the meta it left behind when
  its extension and size match, and its content hash too when the index knows it, or else its
  name. Otherwise the old meta is listed as an orphan meta.
- **A copy outside the editor** (two files with one GUID): the file that had the GUID keeps it.
  The other one gets a new GUID, and its sub-asset GUIDs are derived from the new one.
- **Delete** never clears a field: references to a deleted asset show as missing, in red in the
  Inspector with the last path. Undoing the delete makes them resolve again.
- **References in text** (Lua `require`, HTML `href`/`src`, CSS `url()`/`@import`, shader
  `#include`) are not rewritten. A rename, move or delete lists the ones it breaks in the
  Console, and MCP returns them as `text_references_broken`.

## Duplicating, and changing the assets directory

- `move_asset {path, copy: true}` without `to` is Unity's Duplicate: the copy lands beside the
  source under its free name (`hero 1.png`, `hero 2.png`, a folder `Sprites 1`) and the result's
  `to` says which. With `to` the destination must be free.
- A folder is never copied or moved into itself or below itself (refused, nothing changed).
- `set_project_settings {"editor.assets_dir": ...}` (or `scripting.lua_dir`) moves every
  referenced file into the new directory with its `.meta`, `.humanoid` and Vertex Animation pair,
  rewrites the scenes, prefabs and materials referring to them, and removes the folders the moves
  emptied. Pass `dry_run: true` first: `relocation` lists `moved`, `rewritten`, `already_there`
  (identical files used as they are), `kept_outside` and `text_references_broken`. A blocker (a
  different file at a destination, an unwritable file, a link out of the new directory, unsaved
  Code Editor edits of a moving file) refuses the call and nothing changes. After the change,
  `failed_saves` lists files that keep the old paths on disk (save them again).
- Set the directories right after creating a project, before importing: a later change moves files.

## Missing Assets and Find References

**Window > Assets > Missing Assets** opens the Missing Assets panel. The status bar badge shows
how many there are, and clicking it opens the panel too. It lists every unresolved reference of
the project, in open and closed files, grouped by the missing GUID with each referrer's owner,
GameObject, component and field:

- **Retarget All...**: the missing GUID becomes a former GUID of the asset you choose, so every
  reference resolves to it and no referrer file is rewritten.
- **Retarget Rewrite... / Retarget Selected...**: the chosen fields are rewritten to the asset.
  Open documents change in memory, closed files are written.
- **Clear All / Clear Selected**: the fields are emptied.
- **Replace Prefab...** (prefab groups): the instances use the chosen prefab and keep their overrides.
- **Resolved by path > Keep**: the file keeps those references, and the GUID they hold becomes
  one of its former GUIDs.
- **Orphan metas > Delete Meta** moves the meta to the project trash.
- Double-click a referrer, or use **Locate**, to open its owner and select the GameObject.

Every action is one undo step (Edit > Undo while the Project window has focus, or MCP
`undo` with `scope: "project"`).

Right-click a file in the Project window for **Copy GUID** and **Find References**. Find
References opens the References panel, with a Transitive option that also lists who uses the
referrers.

**Assets > Asset Database > Migrate References** rewrites every scene, prefab, material and
`project.yaml` with the GUIDs of what it references. A second run writes nothing.

## Scripts

Lua:

```lua
local path = Asset.path("4b1e9c0d2a7f4e31b6c58d0f9e2a7c13")   -- "textures/hero.png" or nil
local guid = Asset.guid("textures/hero.png")                    -- or nil
local ok = Asset.exists("textures/hero.png")                    -- a GUID or a path
local ref = Asset.ref("textures/hero.png")                      -- AssetRef: .guid .path .type .valid

-- an Asset script property, set in the Inspector by dragging a file onto it
properties = {
    { name = "portrait", displayName = "Portrait", type = "Asset", assetType = "texture" },
}
-- self.portrait is an AssetRef (nil while None)
```

Paths are relative to the root the asset type uses in references: the assets directory for
textures, models, audio, fonts, UI documents and data files, and the project for scenes,
prefabs, materials, shaders and scripts. C++ scripts declare a `DPROPERTY("Portrait") AssetRef
portrait;` member. A component field such as `ModelComponent::filename` is an `AssetRef`; it
converts to and from a path string, so existing code keeps working.

## Builds

The exporter resolves every reference to its current path, so the game loads by path. A
build whose scenes, or the prefabs and materials they use, reference missing assets fails and
lists each reference. Use "Export Anyway" in Build Settings, `--allow-missing-assets` on the
command line, or `allow_missing_assets` for `build_project` to build anyway. The build ships
`assetmap.bin` (every asset's GUID, its former GUIDs and its sub-assets, mapped to paths) for
the `Asset` Lua API. It is loaded on the first query and costs nothing until then. Metas and
the asset database never ship.

## Command line

```
jasperengine-editor assets check --project <dir>     # JSON; exit code 1 when anything is missing
jasperengine-editor assets migrate --project <dir>   # write the GUIDs into every referrer
```

## MCP tools

| Tool | Does |
|---|---|
| `resolve_asset {asset, path?, type?}` | GUID, path, type, `resolved_by` (live, former, sub_asset, legacy_path, path_fallback, missing), former GUIDs, sub-assets |
| `find_references {asset, transitive, limit}` | referrer, GameObject, component, field, locator, kind |
| `get_asset_dependencies {asset, depth}` | what an asset references |
| `list_missing_assets {scope}` | `missing` groups, `resolved_by_path`, `orphan_metas` |
| `retarget_asset_references {from, to, mode, referrers?}` | `redirect` (a former GUID; no referrer written) or `rewrite` |
| `clear_asset_references {guid, referrers?}` | empties the fields |

Paths and GUIDs are interchangeable wherever a tool takes an asset. `list_assets`,
`get_asset_info`, `move_asset`, `delete_asset`, `import_files`, `link_material` and
`get_component` (`asset_guids`) return GUIDs. `move_asset` and the mutating tools report
`files_written`. A locator names a field: `entity:12/MeshComponent.submeshes[0].material.baseColorTexture`,
`settings/defaultMeshShader`, `project/scenes[3].filepath`, `text:14`.
