# S45 — Sprite Atlas: shared sprite sheets, frames by reference, lossless packing

**Wave / Depends on.** Pixel-art family (ledger `PIXEL-ART-LEDGER.md`), wave 5B. Depends on **S44**
(wave 5A), which owns the minimal sprite-sheet runtime S45 builds on: the `jasperengine.spritesheet`
v1 record, `SpriteSheetPool`, `ImportedAssets`, the packer (`SpritePacker`, S44 §6.10), trimmed frame
geometry, `PivotPreset::FRAME`, the Sprite's `sheet`/`frame`/`frameName` references, their clip
binding, Lua `SpriteSheet` and the scene version bump (moved from this spec to S44 in the family's
consistency pass, so that S44 is self-contained and lands first). Uses S04 (GUIDs, `AssetRef`,
dependency index), S05 (reload), S06 (jobs), S14 (cooks), S24 (undo), S31 (MCP). Owner area:
the atlas and texture-sheet sources of `SpriteSheetPool` (`engine/core/sprite/`), the Sliced draw
mode and growable inline frames of `SpriteComponent` and `MeshSystem`, `editor/sprite/` (sheet and
atlas builders, migration), `editor/window/SpriteAtlasInspector.*`, the texture importer's Sprite
Mode, `TextureSlicerToolDialog`, `docs/sprites.md`, `docs/mcp/guide/sprites.md` (its Sheets and
Atlases sections).

## 1. Goal

A sprite shows a **frame of a sheet**, and the sheet is an asset: an Aseprite file (S44), a
texture sliced once in its import settings, or a **Sprite Atlas** that packs many of them into a
few shared pages. Today every sprite carries its own copy of up to 128 frame rectangles and
animations are tied to whatever rectangles that copy holds. With S44's sheet runtime a sprite
stores a reference (`sheet` + `frame`) and frames carry trim offsets and pivots; S45 adds texture
sheets, 9-slice borders drawn Sliced, and atlases, and adding a file to an atlas changes no scene: sprites keep referencing their source and the runtime draws them from the
atlas page, as Unity's Sprite Atlas does. Packing is lossless for pixel art, deterministic,
incremental and runs as a job.

## 2. Scope

**In scope**
- New sheet sources in S44's `SpriteSheetPool` (texture Sprite Mode, `name@WxH`, `.spriteatlas`) and
  the atlas remap of packed sources; S44 owns the record, the pool, `ImportedAssets` and the frame
  references (`sheet`, `frame`, `frameName`), the From Frame pivot and trimmed geometry.
- The **Sliced** draw mode (9-slice), and inline frames kept as a legacy path with no fixed
  per-entity capacity.
- Texture sprite sheets: Sprite Mode Single / Multiple in the texture's import settings, the
  `name@WxH.png` grid convention, and the Texture Slicer writing them.
- The `SpriteAtlas` asset (`.spriteatlas`): members (files, folders), packing (MaxRects, trim,
  extrusion, padding, POT or not, max size 2048, multiple pages, per-platform max size), lossless
  pages, the build job, incremental rebuilds, export.
- Migration: old scenes and prefabs load and play unchanged; an explicit, undoable conversion of
  inline frames to texture sheets.
- Editor: the Sprite Atlas inspector (page preview, frame list, search, waste statistics), the
  Sprite inspector's frame picker, Project Settings > 2D > Sprite Atlas.
- Lua sheet and frame lookup by name; MCP `edit_sprite_atlas`, `get_sprite_atlas` (group
  `sprites`) and parameters on `set_sprite_frames`, `get_asset_info`.

**Out of scope**
- Reading `.aseprite` files, tags, slices and clips, and the sheet runtime itself (S44).
- Batching and instancing many sprites in one draw (S52 uses the pages and frame tables).
- Pixel snapping, PPU (S41); the project-wide lossless defaults (S42); palettes (S43).
- Rotated packing and polygon ("tight") meshes: rotation breaks pixel art and polygon meshes
  break the one-quad path; atlas variants at other scales (Unity's Variant atlases).
- Tilemap tile rects (`TilemapComponent`, 200 rects) and particle sheets (`PointsComponent`):
  unchanged; the grid tilemap of S50 uses sheets on its own.
- UI Image frames: the UI Image (`ImageComponent`) keeps its texture and patch margins.

## 3. Current state (read on `specs/pixel-art`, 1b1c34fbd)

- **Frames live in each sprite.** `SpriteComponent` (`engine/core/component/SpriteComponent.h:25-47`)
  holds `numFramesRect` and `HybridArray<SpriteFrameData, MAX_SPRITE_FRAMES> framesRect`
  (`:37-38`); `SpriteFrameData` is `{std::string name; Rect rect;}`
  (`engine/core/util/SpriteFrameData.h:11-14`). `MAX_SPRITE_FRAMES` is 128
  (`engine/core/Engine.h:105-106`). In players `HybridArray` is an inline `T values[Size]`
  (`engine/core/util/HybridArray.h`, the non-editor branch), so every sprite carries 128 frames,
  roughly 6 KB (128 × (32 + 16) bytes with libstdc++'s `std::string`; inferred), used or not. The
  exporter grows the capacity to the largest use but never below 128 (`editor/Exporter.cpp:3999-4009`).
  `SpriteAnimationComponent` holds two more 128-entry int arrays (`SpriteAnimationComponent.h:16,19`).
- **The texture is the mesh's.** The sprite draws `mesh.submeshes[0].material.baseColorTexture`
  (`MeshSystem::createSprite`, `engine/core/subsystem/MeshSystem.cpp:621-650`); a frame is a
  normalized `textureRect` on submesh 0 (`setSpriteFrameRect`, `:1180-1206`). The quad is
  `width × height` around a preset pivot (`:668-713`); frames have no offset, pivot or border, so
  trimmed sheets cannot be drawn.
- **No current-frame field.** The clip binding of `sprite.frame` finds the shown frame by comparing
  the texture rect with each inline rect (`engine/core/animation/PropertyBindings.cpp:395-431`);
  `Sprite::setFrame(int|name)` (`engine/core/object/Sprite.cpp:236-254`) only applies a rect.
- **Who writes frames.** The Inspector's **Slicer Tool** writes `framesRect[i]` entries into the
  component through a `MultiPropertyCmd` (`editor/window/InspectorWindow.cpp:8541-8610`,
  `editor/window/dialog/TextureSlicerToolDialog.h:18-92`), so two sprites sharing a sheet hold two
  copies. `set_sprite_frames` does the same for agents (`editor/mcp/tools/WorldTools.cpp:5645-5666`),
  `set_sprite_animation` makes Sprite Animations (`:5668-5683`). Generated scene code writes every
  rect (`editor/Factory.cpp:1576-1583`).
- **Serialization.** `encodeSpriteComponent` / `decodeSpriteComponent`
  (`editor/StreamComponents.cpp:337-395`) write `width height automaticFlipY flipY
  textureScaleFactor pivotPreset framesRect`. Scenes are at version 7, prefabs at 8
  (`editor/StreamMigrations.cpp:702-708`, table `:739-761`).
- **Packing.** `engine/libs/stb/stb_rect_pack.c` is compiled (`engine/libs/stb/CMakeLists.txt:13`)
  and used by nothing. No atlas asset exists; `AssetType` ends at `VertexAnimation = 23`
  (`engine/core/asset/AssetType.h:39`).
- **Cooks.** The runtime finds a cooked texture by source path in
  `assets/je-cooked/<family>/index.json`, the editor through a provider
  (`engine/core/texture/CookedTextures.h:4-50`); sprite textures cook to BC7 / ASTC / ETC2 /
  UASTC by default and BC/ETC2 need multiples of 4 (`docs/textures.md`), lossy for pixel art.
- **Animation Clips** key sprite frames as `{component: sprite, property: frame}` integers
  (`docs/animation-clips.md:218,236`), stepped.

## 4. Reference

| Source | What we take | Notes |
|---|---|---|
| Jukka Jylänki, *A Thousand Ways to Pack the Bin* (2010) and `RectangleBinPack` `MaxRectsBinPack.cpp` (public domain) | MaxRects with Best Short Side Fit, free-rectangle split and prune | Ported (header and NOTICE entry); deterministic tie-breaks added, rotation removed |
| `engine/libs/stb/stb_rect_pack.h` v1.01 (public domain, vendored) | Studied as the alternative: skyline bottom-left | Not used: skyline cannot fill holes, which incremental rebuilds and "keep a member on one page" need; MaxRects packs a few percent tighter on sprite sets. It stays vendored for its existing build line (S34 decides removal) |
| Unity Sprite Atlas v2 | Names: Objects for Packing, Include in Build, Padding, Allow Rotation (we say no), Tight Packing (we say no), Max Texture Size per platform, Pack Preview, Sprite Packer Mode; sprites keep their source reference and are drawn from the atlas | We add trim offsets per frame, waste statistics, agents |
| Godot `AtlasTexture` / TexturePacker | The page + region + margin (trim) model | |
| tenten `src/aseprite.ts` sheet export and ritmo `spritesheet.ts` (MIT) | The `name@WxH.png` naming (tenten's fixtures `buff@22x22.png`, `spiker@16x16.png`) and Aseprite-style frame tables | Naming only |

## 5. Requirements

### Functional — sheets and frame references
- **R1 Sheet record.** S44's `jasperengine.spritesheet` v1 (S44 R45, §6.11). S45's texture sheets
  and atlases write the same record with `tags`, `slices` and `user_data` empty.
- **R2 Sheet sources.** S45 registers sources in S44's `SpriteSheetPool` (`registerSource`, S44
  R46), which already serves `.aseprite`/`.ase` files and plain textures (one frame): a
  `.spriteatlas` (all its frames), a texture with Sprite Mode Grid or Multiple (its frames on the
  texture itself), and a `name@WxH.<ext>` texture (a grid, below). When a source is packed into an
  atlas that is in use (R18), the pool's sheet for that source points into the atlas pages; names,
  order, sizes, offsets and pivots do not change, so nothing that references the source notices.
- **R3 Frame reference.** S44 R48 (`sheet`, `frame`, `frameName`; found by name first, so
  reordering an atlas keeps the right frame).
- **R4 Geometry.** S44 R49 (source size, trim offsets, native size at `width`/`height` 0). S45 adds
  the Sliced sizing (R6): non-zero `width`/`height` stretch a Simple sprite and size a Sliced one.
- **R5 Pivot.** S44 R49 (`PivotPreset::FRAME`, the default for sprites created from a sheet).
- **R6 Sliced draw mode.** `drawMode` Simple (default) or Sliced: Sliced draws the frame as 9
  quads from its border (left, top, right, bottom in source pixels) at the sprite's `width ×
  height`, corners unscaled, edges and centre stretched, on the existing dynamic sprite buffer (16
  vertices, 54 indices). A frame without a border draws as Simple with a warning in the Inspector.
  Frames with a border are never trimmed by the packer.
- **R7 Texture pages.** S44 R48–R49 (the frame's page is bound with the sprite's own sampler; the
  texture path of a sheet sprite is never saved). Atlas pages follow the same rule.
- **R8 Inline frames (legacy).** Sprites without a `sheet` keep today's behaviour: `framesRect`,
  `numFramesRect`, `addFrame`, `setFrame`, Sprite Animations, clip keys. The storage becomes a
  growable list, empty by default, with no 128-frame cap; generated scene code sizes it first.
  When the sprite's texture is packed into an atlas in use, each inline rect is mapped into the
  page (translated, clipped to the member's trim with the clipped part turned into an offset), so
  legacy sprites also draw from atlases.
- **R9 Animation Clips.** S44 R50 (`sprite.frame` keys index the sheet). Clips keyed on an atlas
  sheet work the same way; the guide recommends keying the source sheet (the atlas is transparent).

### Functional — texture sheets
- **R10 Sprite Mode.** A texture's import settings gain `spriteSheet` (§6.4): `single` (default),
  `grid` (cell size, offset, spacing, skip empty cells, count) or `multiple` (explicit frames with
  name, rect, pivot, border). Grid and multiple textures are sheets on the texture itself (no
  repacking, no trim), so a sprite sheet PNG works without an atlas. `multiple` frames can be filled
  from an Aseprite json-hash file beside the texture (`<stem>.json`, the layout Aseprite's `--data`
  writes, and what S46 R23 and S49 R21 write): Slice… › From JSON, one undo step.
- **R11 `name@WxH`.** A texture whose file name ends in `@<W>x<H>` before the extension
  (`coin@16x16.png`) and has no stored `spriteSheet` is a grid of `W × H` cells, row-major, fully
  transparent cells skipped, frames named `<base>_<i>` (`coin_0`…); nothing is written to the meta
  until the user edits the setting. A size that does not divide the image is a warning naming the
  leftover pixels.
- **R12 Texture Slicer.** `TextureSlicerToolDialog` edits a texture's `spriteSheet` (grid by
  count or cell size, offset, padding, prefix, skip empty, per-frame pivot and border, live
  preview) instead of a sprite's inline frames; opened from the texture importer (**Slice…**) and
  from the Sprite inspector (**Slicer Tool**), where applying also sets the sprite's sheet and
  frame. One undo step (a scene step including the meta write when a sprite changes, a project
  step otherwise).

### Functional — Sprite Atlas
- **R13 Asset.** `.spriteatlas` is `AssetType::SpriteAtlas` (name `sprite_atlas`), canonical JSON
  with its GUID in the file (§6.5), created by **Assets > Create > 2D > Sprite Atlas** or
  **Create Sprite Atlas from Selection**. Members are files (`AssetRef`: textures, `.aseprite`) and
  folders (paths, recursive, filtered to those types), with an exclude list.
- **R14 Packing.** MaxRects BSSF (§6.6) with: trim (alpha 0, on by default, never for bordered
  frames), extrusion 0–4 px (default 1: edge pixels repeated around each frame), padding 0–16 px
  between extruded rects and at page edges (default 1), duplicates packed once (pixel equality),
  **Keep Members Together** (default on: a member's frames share a page when they fit, so an
  animation never switches texture), Power of Two (default off), Square (off), Max Texture Size
  32–8192 (default 2048) and per-platform overrides (Desktop, Mobile, Web, Android ETC2), as many
  pages as needed, each page shrunk to its used bounds (rounded to POT or to the block size when
  the format needs it). A frame larger than the max size is an error naming it and the size.
- **R15 Determinism.** The same members, pixels and settings give the same layout and the same
  page bytes on every machine and run (sorted input, ordered tie-breaks, no hash-map iteration
  order, no threads in the layout).
- **R16 Lossless pages.** Page pixels equal the source pixels exactly (no resampling, no colour
  conversion, straight alpha kept, transparent texels' colour from the source). Compression
  **Auto** is lossless (RGBA8 + Zstandard, S14 quality `none`) when the project is in Pixel Art
  Mode (S42) or any member is an Aseprite sprite or a nearest-filtered sprite texture, else the
  S14 sprite format of each platform; with block formats, rects align to the 4-px block grid so
  no block holds two frames. **Lossless** and **Platform Default** force either.
- **R17 Build job.** Building is a Background job (S06): member frames from the S44 and texture
  caches, layout, page images, `sheet.json`, then S14 cooks. The status bar shows `Building
  sprite atlas ui (2/3 pages)` with Cancel. Incremental: unchanged members are not re-read;
  the layout is recomputed (cheap) only when the member set or a frame size changed, otherwise
  changed pixels are blitted in place; only pages whose bytes changed are rewritten and recooked.
  A member edit (S05 reload of a texture or `.aseprite`) rebuilds the atlases that pack it.
- **R18 Atlas mode.** Project Settings > 2D > Sprite Atlas > **Mode**: *Enabled* (default: the
  editor, Play and builds draw from atlases), *Enabled for Builds* (the editor and Play draw from
  sources), *Disabled*. An atlas with **Include in Build** off is never used by builds (a game can
  load it explicitly from Lua by path). A source packed by two atlases in use is a warning on both;
  the atlas whose path sorts first wins.
- **R19 Export.** Exports build every included atlas for each platform family (its own max size),
  ship pages and `sheet.json` through `ImportedAssets` (`assets/je-imported/`), and leave out a
  member texture's source when nothing but sprites references it (S14's "shipped only when read"
  rule extended to sheets).

### Functional — migration
- **R20 Old content loads.** Scenes and prefabs of every earlier version load, render and play
  exactly as before (inline frames, Sprite Animations, clips). The scene and prefab version bump for
  the sheet keys, with its identity migration ("sprite sheets: an older file has none") and its
  migration test, is S44's (S44 R44: the next version at landing). S45's own scene key, `drawMode`,
  is optional and written only when not `simple`, so it needs no bump (S03 rule); an older editor
  keeps it as unknown data and draws the sprite Simple. If implementation finds a bump necessary
  after all, it takes the next number at landing, with a migration and a migration test.
- **R21 Convert to Sprite Sheet.** An explicit, undoable action (Inspector button on the legacy
  section, `set_sprite_frames {convert: true}`, and a project-wide **Tools > 2D > Convert Inline
  Sprite Frames…** with a dry-run list) moves a sprite's inline rects into its texture's
  `spriteSheet` (mode multiple, names and order kept), sets `sheet`, `frame`, `frameName`, and
  clears the inline list. Every other sprite in the project (open and closed scenes, prefabs,
  through S04's dependency index) that uses the same texture with the same rect list is offered in
  the same step. A texture that already has a different sheet is a `sheet_conflict` error with the
  choice to keep the sprite inline. Indices are preserved, so clip keys and Sprite Animations keep
  working; one undo step restores everything.
- **R22 Reporting.** `jasperengine-editor check --project P --sprites` lists legacy sprites
  (scene/prefab, GameObject path, texture, frame count) and atlas problems (missing members,
  double packing, oversize frames); exit 1 on atlas errors, 0 otherwise.

### Platform & backends
- **R23** Pages are ordinary 2D textures (RGBA8 or S14 formats) and frames are vertex positions
  plus the existing texture rect: no shader change, so GL 3.3, GLES3/WebGL2, Metal, D3D11 and
  Vulkan draw identically. 2048 is the default because GLES 3.0 and WebGL2 guarantee it; NPOT pages
  are valid on every target (GLES3/WebGL2 have full NPOT support), POT is offered for old tools.
- **R24** A project without sheets or atlases runs today's sprite code paths unchanged; a disabled
  atlas mode builds nothing; a sprite without frames allocates no frame storage.

### Performance
- **R25 Memory.** A sheet sprite's component adds ≤ 64 bytes over a frameless sprite, and frame
  data is shared per sheet; a legacy sprite pays only for the frames it has (players: down from
  the fixed 128-frame array of §3).
- **R26 Frame changes.** 1,000 sprites changing frame every frame: ≤ 0.5 ms of CPU per frame on the
  CI Linux runner and ≤ 2 ms on a mid-range Android device (the device and number recorded in this spec), including
  the quad rebuilds of trimmed frames; zero allocations per frame change.
- **R27 Packing.** Layout of 2,000 frames into 2048² pages ≤ 100 ms; a full build of a
  500-frame atlas from warm member caches ≤ 400 ms; one changed member's pixels to the new page
  visible ≤ 300 ms; page usage on the reference corpus (§7 AC4) ≥ 85 % with trim.
- **R28 Load.** Parsing a 2,000-frame `sheet.json` ≤ 5 ms on desktop, ≤ 20 ms on the web build.

### Editor UX (human)
- **R29 Sprite Atlas inspector.** **Objects for Packing** (files and folders, drag and drop,
  +/−, Exclude), **Include in Build**; **Packing**: Max Texture Size with platform tabs (Default |
  Desktop | Mobile | Web | Android ETC2, Override for …), Padding, Extrude Edges, Trim, Keep
  Members Together, Power of Two, Square; **Texture**: Compression (Auto / Lossless / Platform
  Default), Filter (Auto from members / Point / Bilinear); **Pack Preview** (builds in memory);
  Apply / Revert (one undo step). The preview: page tabs, integer zoom (1×–16×, Fit), checkerboard,
  frame outlines, trim and extrusion overlay, hover tooltip (name, member, size, trimmed from,
  page rect), click selects the member in the Project window, double-click opens it (Aseprite for
  `.aseprite`). The **frame list**: search (name substring, `member:` and `page:` filters), sort by
  name, size, page or waste, a thumbnail per row. **Stats**: `3 pages · 1,204 frames · 87 % used ·
  412 duplicate frames merged · 1.9 MB VRAM (Desktop) · 2.4 MB (Web)`, wasted pixels per page and
  the worst members (largest transparent area before trim). Warnings with a Fix button where one
  exists (remove a double-packed member, raise Max Size).
- **R30 Sprite inspector.** **Sprite** (an asset field accepting textures, `.aseprite` and
  `.spriteatlas`, drop target), **Frame** (a dropdown with thumbnails and search, plus the index),
  **Draw Mode** (Simple / Sliced), **Size** (Native checkbox, then width/height), **Pivot**
  (presets + From Frame), and for legacy sprites an **Inline Frames (legacy)** section with the
  frame count and **Convert to Sprite Sheet**. Edits are scene undo steps.
- **R31 Texture importer.** **Sprite Mode** (Single / Grid / Multiple) with **Slice…**, the frame
  table (name, rect, pivot, border) and, for `name@WxH` files, `Grid from file name (16×16)`. A
  texture or `.aseprite` packed in an atlas shows `Packed in: ui/icons.spriteatlas (page 1)` with a
  link.
- **R32 Feedback.** Project window badges for atlases building/failed; the Console line
  `[Sprite Atlas] Built ui/icons.spriteatlas: 3 pages, 1,204 frames, 87 % (210 ms)`.

### Agent UX (MCP, Lua, CLI)
- **R33 `edit_sprite_atlas`** (group `sprites`): create an atlas, add or remove members, change
  settings (optionally for one platform). Without `output` it is a preview (pack in memory: pages,
  frames, usage, waste, duplicates, warnings, and the page images unless `include_pages: false`);
  the same arguments plus `output: true` write the `.spriteatlas` (one project undo step) and
  build it (a job when longer than 1 s).
- **R34 `get_sprite_atlas`** (group `sprites`): an atlas's settings, members, pages (size, frame
  count, used %, VRAM per platform), frames (name, member, page, rect, trim, waste; `search`,
  `limit`, `page`), warnings and build state; given a member path, the atlas that packs it and its
  placements. `include_pages` adds page images.
- **R35 Existing tools.** `set_sprite_frames` (which S44 gave `sheet` and `frame` as an index or a
  name, S44 R36) gains `inline` (keep the legacy per-sprite frames) and `convert` (R21, with
  `all_users`); its
  `slice` and `frames` now write the texture's Sprite Mode (as the Slicer does) unless `inline`.
  `get_asset_info` reports a texture's Sprite Mode and frames and `packed_in` for any sheet
  source. `get_entity` shows `sheet`, `frame`, `frameName`. `set_properties` sets them.
- **R36 Lua.** S44 R51 binds `Sprite.sheet`, `frame`, `frameName`, `frameCount`, `getSheet()`,
  `setFrame(index|name)` and the `SpriteSheet` class. S45 adds `Sprite.drawMode`, and
  `SpriteSheet.load` accepts every source S45 registers. An atlas is just a sheet:
  `SpriteSheet.load("ui/icons.spriteatlas"):getFrame("sword")`.
- **R37 Guide.** `docs/mcp/guide/sprites.md` Sheets and Atlases sections (reference strings, the
  preview → `output` loop, waste reading, migration), one digest line shared with S44/S47.

### Data & compatibility
- **R38** `AssetType::SpriteAtlas = 26`, the family's reserved value (ledger: Palette 24,
  AsepriteSprite 25, SpriteAtlas 26, Tileset 27), whatever order the specs land in;
  `assetTypeFromName`'s bound is the highest value present and its loop skips unnamed values. Atlas files carry `format`, `version
  1`, `guid`; a newer version is refused read-only with the reason, unknown keys are kept.
- **R39** `import.spriteSheet` in texture metas has `version: 1` and sits beside S14's
  `textureImporter` (S04 keeps both opaque); the sheet record and the `ImportedAssets` index (S44)
  are versioned, and so are their cache directories (`v1`), so a format change reimports.
- **R40** S45's scene keys (§6.7) are additive and need no bump (R20); the sheet keys and their bump
  are S44's.

## 6. Design constraints & decisions

### 6.1 MCP tools and the budget (S31)

| Tool | Group | Change | Estimated listing chars |
|---|---|---|---|
| `edit_sprite_atlas` | `sprites` | new: `path`, `create`, `add[]`, `remove[]`, `settings` (free-form; keys in `jasper://docs/sprites`), `platform`, `include_pages`, `output` | ≤ 1,300 |
| `get_sprite_atlas` | `sprites` | new: `path`, `search`, `limit`, `page`, `platform`, `include_pages` | ≤ 900 |
| `set_sprite_frames` | core | `inline`, `convert`, `all_users` (S44 adds `sheet`, `frame` integer\|string, `clips`), no per-parameter descriptions; one description clause: slices now write the texture's sheet | +70; S44 + S45 stay within the tool's grandfathered 2,075 (S44 §6.1 sheds ≥ 120 description characters to `jasper://docs/sprites`) |
| `get_asset_info` | core | description +`; textures: Sprite Mode, packed_in` | +40 |

- `sprites` group (≤ 12 tools, ≤ 15,000 chars, `group_default`): S44 1 (`set_sprite_import_settings`,
  ≤ 1,100), S45 2 (≤ 2,200), S47 6 (`get_sprite` among them, ≤ 5,350): 9 tools, ≈ 8,650 characters
  (ledger "MCP budget"), no `budget.json` entry needed. S44 adds the group row in wave 5A.
- Names: `pack_` and `describe_` are not verbs of `test_tool_conventions.py`'s `VERBS`; `edit_`
  (several operations, packing among them) and `get_` are, so no verb is added.
- Core: +≈110 chars across S45's rows; the default listing must still fit, else the parameter
  texts move to `jasper://docs/sprites`. Guide: the shared Sprites line of the `## Pixel art`
  section (S44 §6.1).

### 6.2 Module layout
```
engine/core/sprite/SpriteSheetSources.{h,cpp}  S45's pool sources: texture sheets, @WxH, .spriteatlas, atlas remap
engine/core/sprite/SpriteFrameGeometry.{h,cpp}  S44's file; S45 adds the 9-slice vertices (Sliced mode)
editor/sprite/SpriteSheetBuilder.*      texture sheets (single/grid/multiple, @WxH, From JSON)
editor/sprite/SpriteAtlasBuilder.*      atlas jobs, incremental state, remaps
editor/sprite/SpriteAtlasDocument.*     the .spriteatlas model, canonical JSON
editor/sprite/SpriteMigration.*         R21, R22
editor/window/SpriteAtlasInspector.*    R29
```
S44 owns `SpriteSheet.h`, `SpriteSheetIO`, `SpriteSheetPool`, `ImportedAssets` and
`editor/sprite/SpritePacker` (S44 §6.2); S45's builders call them.

### 6.3 The sheet record (S44 §6.11) and what S45 adds to the pool
The record, `SpriteSheetData`, `SpriteSheetPool` and `ImportedAssets` are S44's (S44 §6.11). S45
registers its sources with `SpriteSheetPool::registerSource` and adds one entry point and one key:
```cpp
// engine/core/sprite/SpriteSheetPool.h (S45's addition)
// a legacy inline rect (pixels of `texture`) as drawn from the atlas page that packs it, if any
static bool mapInlineRect(const std::string& texture, const Rect& rect, SpriteFrame& out);
```
`ImportedAssets` gains `<atlas>#sheet`; a source packed by an atlas in use resolves `#sheet` to the
atlas's remapped view of it (R2). Page textures are cooked by S14 under their `je-imported/` path.

### 6.4 Texture Sprite Mode (`.meta`, `import.spriteSheet`)
```yaml
import:
  textureImporter: {...}          # S14, unchanged
  spriteSheet:
    version: 1
    mode: grid                    # single | grid | multiple
    grid: {cell: [16, 16], offset: [0, 0], spacing: [0, 0], count: 0, skipEmpty: true, prefix: "coin_"}
    frames:                       # multiple only
      - {name: idle_0, rect: [0, 0, 16, 16], pivot: [8, 16], border: [0, 0, 0, 0]}
    pivot: bottom_center          # default pivot of frames without one (presets or [x, y] normalized)
```

### 6.5 The atlas file (`.spriteatlas`, canonical JSON, identity in the file)
```json
{
  "format": "jasperengine.spriteatlas",
  "version": 1,
  "guid": "7c0e5d1f3a2b4c6d8e9f0a1b2c3d4e5f",
  "include_in_build": true,
  "members": [
    {"path": "sprites/hero.aseprite", "guid": "4b1e9c0d2a7f4e31b6c58d0f9e2a7c13"},
    {"folder": "ui/icons", "types": ["texture", "aseprite_sprite"]}
  ],
  "exclude": ["ui/icons/old"],
  "settings": {"max_size": 2048, "padding": 1, "extrude": 1, "trim": true, "keep_members_together": true,
               "power_of_two": false, "square": false, "compression": "auto", "filter": "auto",
               "platforms": {"web": {"override": true, "max_size": 1024}}}
}
```
File members are S04 `AssetRef`s (rename-safe); folders have no GUID (S04: folders have no metas),
so a moved folder is reported by the Missing Assets panel as a missing atlas folder member. Atlas
frame names: the member's frame name when unique in the atlas, else `<member path without
extension>/<frame name>`; a single-frame texture member is named by its stem (`sword`). Pages have
derived, unpersisted GUIDs (`Guid::derive(atlas, "image#<n>")`): nothing references them directly.
`AssetTypes` row: identity in-file, path base assets, `hasSubAssets` false.

### 6.6 Packing
The packer and its exact rules are S44's (`SpritePacker`, S44 §6.10: member order, trim, extrusion,
padding, duplicates, Keep Members Together, POT/square/block rounding, pure and deterministic).
The atlas builder passes each atlas's settings and members (per platform family, R19) and caches
the layout by S44's key.

### 6.7 Scene and runtime fields
```cpp
// PivotPreset::FRAME and the sheet fields (sheet, frame, frameName, sheetData, ...) are S44's (S44 §6.12)
enum class SpriteDrawMode { SIMPLE, SLICED };
struct SpriteComponent {
    // ...existing fields and S44's sheet fields...
    SpriteDrawMode drawMode = SpriteDrawMode::SIMPLE;
    std::vector<SpriteFrameData> framesRect;     // was HybridArray<SpriteFrameData, MAX_SPRITE_FRAMES>
};
```
```yaml
sprite:
  enabled: true
  sheet: ui/panel.png               # S44's keys (sheet, sheetGuid, frame, frameName, pivotPreset)
  frame: 0
  frameName: panel
  drawMode: sliced                  # S45: written only when not simple
  width: 48
  height: 32
```
The generated scene code (`Factory.cpp`) emits `drawMode` and `framesRect.resize(n)` before
inline rects; `RuntimeFields.gen.inc` is regenerated. `MAX_SPRITE_FRAMES` keeps sizing
`SpriteAnimationComponent` and `PointsComponent` only.

### 6.8 Errors
`frame_too_large` (`field`: the frame), `missing_member`, `double_packed`, `sheet_conflict`,
`invalid_settings` (`field`: the key), `atlas_version` (newer file); `unknown_frame` and
`not_a_sheet` are S44's (S44 §6.9). Each has `fix`.

## 7. Acceptance criteria

| AC | Verification |
|---|---|
| AC1 (R1, R2, R39) | `engine/tests/core/sprite/SpriteSheetSourcesTest.cpp`: S45's sources (grid, multiple, `@WxH`, `.spriteatlas`) write records that S44's `SpriteSheetIO` round-trips canonically; the record IO itself is S44 AC20. |
| AC2 (R2, R7) | GPU golden `SpriteSheetRender.AtlasEqualsSources` (glcore and Vulkan, `JASPER_TEST_UPDATE_GOLDEN` review): a scene of 12 sprites drawn from a trimmed atlas page and from the source PNGs is pixel-identical (S44 AC21 covers trimmed geometry on one sheet). Red control: the atlas remap's offsets ignored → the golden fails. |
| AC3 (R6) | Golden `SpriteSheetRender.Sliced`: a 9-slice frame at 3 sizes has unscaled corners (pixel check) on glcore and Vulkan; a frame without border warns in the Inspector. |
| AC4 (R14, R15, R27) | `editor/tests/sprite/SpriteAtlasLayoutTest.cpp`: atlas settings reach S44's packer (S44 AC22 tests the packer): per-platform max size, Keep Members Together across members, block rounding with a forced block format; the same atlas built twice and from a shuffled member list gives byte-identical pages; a 500-frame atlas from warm member caches builds ≤ 400 ms (`SpriteAtlasBench`). |
| AC5 (R16) | `editor/tests/sprite/SpriteAtlasBuildTest.cpp`: every page texel inside a frame equals the source texel (RGBA, transparent texels' colour included) after the lossless cook is decoded (`cook-textures --decode`); with a forced BC7 format, rects align to 4. |
| AC6 (R17, R32) | `tools/mcp/tests/test_sprite_atlas.py::test_incremental`: editing one member's pixels rewrites only its page (page file hashes) and recooks only it; adding a member of a new size relayouts; the job reports progress and can be cancelled; one changed member visible ≤ 300 ms (S05-style timing, 20 runs). |
| AC7 (R2, R18) | `test_sprite_atlas.py::test_transparent_atlas`: a scene referencing `sprites/hero.aseprite` and `items/sword.png` renders identically (screenshot diff = 0) with the atlas mode Disabled and Enabled, while `get_render_stats` shows one page texture bound instead of two; Enabled for Builds draws sources in Play and atlas pages in an export. Double packing warns on both atlases. |
| AC8 (R8, R20, R40) | `editor/tests/stream/SpriteInlineFramesTest.cpp`: a legacy sprite with 300 inline frames loads, saves, renders and plays (no cap); generated scene code sizes the list first and compiles (`tools/smoke-test.sh` on the fixture); a Sliced sprite round-trips `drawMode` and an editor without S45 keeps it as unknown data. The version-bump migration test is S44's (S44 AC23). |
| AC9 (R8) | `SpriteAtlasBuildTest.InlineRectsRemap`: a legacy sprite whose texture is packed (trimmed) shows the same pixels for each inline rect, including rects that cross the trim. |
| AC10 (R21, R22) | `tools/mcp/tests/test_sprite_migration.py`: `set_sprite_frames {convert: true, all_users: true}` on one of three sprites sharing a texture (two in a closed scene, one in a prefab) writes the meta, converts all three, keeps the clip keyed `frame 2` showing the same rect, and one `undo` restores the scenes and the meta; a texture with another sheet gives `sheet_conflict`. `check --sprites` lists the legacy sprites before and none after; exits 1 on a missing atlas member. |
| AC11 (R10, R11, R12) | `editor/tests/sprite/SpriteSheetBuilderTest.cpp`: grid with offset/spacing/skip-empty, multiple frames with pivots, `coin@16x16.png` inferred without a meta write, a non-dividing size warns; `test_sprite_atlas.py::test_slicer`: the Slicer writes `import.spriteSheet` and sets the sprite's sheet in one undo step. |
| AC12 (R9) | `test_sprite_atlas.py::test_clip_on_atlas`: a clip keying `sprite.frame` on a sprite whose source is packed in an atlas in use shows the same frames as with the atlas mode Disabled (S44 AC24 covers clips on plain sheets). |
| AC13 (R19) | `test_sprite_atlas.py::test_export`: a Linux and a web export contain the atlas pages under `assets/je-imported/` cooked per family (web max size override honoured: 2 pages instead of 1), no member PNG that only sprites used, and the player screenshot equals the editor's Game view (S30 headless player, `--fixed-fps 60`). |
| AC14 (R25, R26) | `engine/tests/core/sprite/SpriteFrameBench.cpp`: 1,000 sprites switching trimmed frames each frame ≤ 0.5 ms CPU (CI runner), 0 allocations (counting allocator); `sizeof` checks: a sheet sprite's component ≤ frameless + 64 bytes; the Android number measured on a mid-range device with S15's CPU profiler, recorded here. |
| AC15 (R29–R31) | `tools/mcp/tests/test_sprite_atlas_editor.py` (xvfb, isolated editor, `tools/editor-shot.sh`): the atlas inspector with pages, frame search and stats, the Sprite inspector's frame picker and legacy section, the texture importer's Sprite Mode; screenshots in the evidence. |
| AC16 (R33–R35, §6.1) | `tools/mcp/tests/test_sprite_atlas.py`: `edit_sprite_atlas` preview writes nothing (file hashes) and returns page images; `output: true` creates the file, builds it (job polled with `get_job`), one `undo` deletes it; `get_sprite_atlas` search/limit/page and member lookup; `set_sprite_frames {sheet, frame: "sword"}`; typed errors `unknown_frame`, `frame_too_large`; the `sprites` group hidden by default and listed after `tool_groups`; `test_tool_budget.py`, `test_tool_conventions.py`, `test_tool_coverage.py`, `gen_tool_docs.py --check`. |
| AC17 (R36) | `tools/mcp/tests/test_sprite_atlas.py::test_lua`: a script loads `ui/icons.spriteatlas`, finds `sword`, sets `sprite.sheet`/`frameName`, logs `frameCount` and `getFrame("sword").rect`; Console matches; `tools/lua_api/tests` list the members; the docs' Lua blocks run. |

## 8. Dependencies & conflicts

- **Depends on.** **S44** (wave 5A, landed first): the record, `SpriteSheetPool`, `ImportedAssets`,
  the packer, frame references and geometry, the version bump. S41 (wave 5A) for PPU. S04
  (`AssetRef`, dependency index for R21, Missing Assets), S05 (reload of members), S06 (jobs), S14
  (cooks, `quality none`, per-family max size), S24 (undo across scene and meta), S31 (MCP). S42
  (same wave) for the Pixel Art Mode flag (until it lands, Auto uses the members rule).
- **Blocks.** S50 (tiles from sheets and atlases, wave 5C). S52 (wave 5A) reads sheets through
  S44's pool, so atlas pages reach it transparently when S45 lands; S45 does no batching. S47 (sheet
  previews of edited files go through S44's importer).
- **Interfaces given to siblings.** The pool's texture-sheet and atlas sources,
  `SpriteSheetPool::mapInlineRect`, `SpriteDrawMode` and `SpriteComponent::drawMode`, the
  `.spriteatlas` format, the json-hash import (R10).
- **Asset type value.** `SpriteAtlas = 26`, reserved (ledger), whatever the landing order.
- **Merge hotspots.** `SpriteComponent.h`, `MeshSystem.cpp` (sprite creation, frame rects;
  S41/S52 also touch it), `PropertyBindings.cpp`, `StreamComponents.cpp`, `StreamMigrations.cpp`
  (the version numbers: S41, S50, S52 may bump too), `Factory.cpp`, `RuntimeFields.gen.inc`,
  `InspectorWindow.cpp` (sprite section), `editor/reflect/Enums.cpp` (`PivotPreset`, `SpriteDrawMode` rows), `TextureImporterView.*` (S14), `AssetType.h` /
  `AssetTypes.cpp` (S43, S44, S50), `Exporter.cpp` (capacities, shipping), `WorldTools.cpp`
  (`set_sprite_frames`), `McpToolGroups.cpp`, `docs/mcp/guide/sprites.md`.

## 9. Risks & defaults

- **Atlas order changes frame indices of atlas sheets.** Default: sprites store `frameName` and
  resolve by name first (R3); clips on atlas sheets are discouraged in the guide (key the source
  sheet: the atlas is transparent).
- **Texture switches inside an animation.** Keep Members Together is on by default; a member too
  big for one page is split with a warning in the inspector.
- **Linear-filtered (non-pixel-art) sprites** need extrusion ≥ 2 and no mips; Auto sets extrude 2
  when Filter resolves to Bilinear. Mipmapped atlas pages are not offered.
- **NPOT pages with block compression** on old Android drivers: pages round to multiples of 4,
  which BC/ETC2/ASTC need; POT is a setting for anything stricter.
- **Legacy inline frames stay forever.** Removing them would break scripts calling `addFrame`;
  the cost is now zero for sprites that do not use them. Default kept.
- **Folder members and renames.** Folders have no GUIDs; a moved folder member is reported, not
  followed. Default kept (S04 has no folder identity).
- **Changing `set_sprite_frames`'s `slice`/`frames` to write the texture sheet** changes where the
  data lives, not what the agent sees; `inline: true` keeps the old behaviour and the guide says so.
- **Version bumps** are S44's (S44 R44, the next numbers at landing); S45 adds no bump (R20).

## Milestones

- **M1** (on S44's sheet runtime) S45's pool sources, Sliced mode, inline frames as a growable list,
  generated code, texture Sprite Mode, `name@WxH`, the json-hash import, the Slicer (R1, R2, R4, R6,
  R8, R10–R12, R20, R23–R26, R38–R40); AC1, AC3, AC8, AC11.
- **M2** Sprite Atlas asset, builder job, incremental rebuilds, atlas mode, lossless pages, inline
  remap, export (R13–R19, R27, R28); AC5–AC7, AC9, AC13, AC14.
- **M3** Aseprite members (S44 has landed), rebuild on `.aseprite` reload; AC2, AC4, AC12; AC6/AC7
  rerun with Aseprite members.
- **M4** Migration action and `check --sprites` (R21, R22); AC10.
- **M5** Inspectors and feedback (R29–R32); AC15.
- **M6** MCP, Lua, guide, docs (`docs/sprites.md`), budgets (R33–R37); AC16, AC17.

## Deviations

Recorded by the implementation (`feature/s45-sprite-atlas`):

- **Growable inline frames (R8).** The mesh-memory program made `framesRect` a `GrowArray` before
  S45 (no capacity, empty by default). S45 adds the generated code's `framesRect.resize(n)` before
  the rects and the 300-frame test.
- **Texture sheets ship as records.** Metas do not ship, so a texture's Sprite Mode (and a
  `name@WxH` grid) reaches the runtime as a `jasperengine.spritesheet` record through
  `ImportedAssets` (`<texture>#sheet`): the editor writes it to `.jasperengine/cache/imported/v1/sheets/`
  (`SpriteSheetBuilder`), exports copy it to `je-imported/sheets/`. A `name@WxH` file loaded with no
  record (a game without an editor) is decoded and gridded by the engine itself.
- **Texture frame keys.** A texture sheet's frames carry `key: "rect:<x>,<y>,<w>,<h>"` (their place
  in the texture), kept by the atlas remap, so a legacy inline rect maps into the page through a
  grid or multiple texture too (`mapInlineRect`); a single texture is mapped through its one frame.
- **The atlas remap of legacy sprites (R8)** is runtime state (`SpriteComponent::inlineAtlas`): the
  mesh draws the page while the scene encoder, the cooked scenes and the generated code write the
  source texture and its rect.
- **Auto compression (R16).** Texture import settings have no filter (the sampler is each
  sprite's), so Auto is lossless when a member is an `.aseprite` file, a texture with S14 quality
  `none`, or a texture with a Sprite Mode. S42's Pixel Art Mode is not on `main`.
- **Incremental builds (R17).** A rebuild reuses each unchanged member's decoded frames (cached by
  its content stamp) and lets S44's deterministic `pack()` lay out and draw the pages again (a few
  ms); the same sizes give the same layout, so the observable rules hold: unchanged members are not
  read again, page files are named by their bytes, and only pages whose bytes changed are written
  and cooked. `relayout` in the build tells whether the layout moved. A build whose inputs match
  `build.json` is adopted without packing.
- **Menus.** The editor has no Tools menu: the project-wide conversion is **Assets > Convert Inline
  Sprite Frames...**. The Project window's Create list is flat (its own **Sprite Atlas** item); the
  main menu has **Assets > Create > 2D > Sprite Atlas / Sprite Atlas from Selection**.
- **Project Settings > 2D** is a new page (S41's Pixels Per Unit is on Graphics); the MCP key is
  `sprite_atlas.mode`.
- **Families of a source export (R19).** A source export carries every platform: its atlases take
  the desktop layout, or the web one when its only backend is GLES3 / WebGL2 (`build_project mode
  source platform web`).
- **Include in Build off** atlases are not used by the editor either (no remaps anywhere); they
  ship and load by path.
- **VRAM (R29).** Lossless pages count 4 bytes a pixel, the platforms' sprite formats 1.
- **Sliced and lattices.** A Sliced sprite draws its 9-slice whatever grid a lattice asks for.
- **`set_sprite_frames` slicing (R35)** writes the texture's Sprite Mode as Multiple (the names
  given); the Slicer writes Grid when the cells are uniform and unedited. Existing MCP tests that pin
  per-sprite slicing (sprite normals, `test_world`, `test_saved_output_determinism`) now pass
  `inline: true`, and the sprite-normals wiring calls it with `inline: true` (its normal map pairs
  with the mesh texture).
- **`edit_sprite_atlas` `settings`** also takes `include_in_build`.
- **Guide.** The upstream MCP rules give the area one "Other areas" entry (S44's `sprites`); S45's
  agent text is in `docs/mcp/guide/sprites.md` (Sheets, Atlases, Legacy inline frames) with eval
  cases `bad_atlas_by_hand` and `bad_inline_frames_retyped`.
- **AC2/AC3 suites** are `SpriteAtlasRenderGpu.AtlasEqualsSources` and
  `SpriteAtlasRenderGpu.SlicedKeepsItsCorners` (the naming rule wants `*Gpu` suites); both compare
  two renders (or exact pixels) instead of a committed golden image.
- **AC15** saves `screenshot view: editor` images of the harness' display instead of running
  `tools/editor-shot.sh`.

