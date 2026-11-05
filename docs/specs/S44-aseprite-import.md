# S44 — Aseprite Import: a lossless `.aseprite` codec, AsepriteSprite assets, live reimport, Sprite Preview

**Wave / Depends on.** Pixel-art family (ledger `PIXEL-ART-LEDGER.md`), wave 5A (with S41, S43 and
S52). S44 is self-contained and lands before S45: besides the codec it owns the minimal sprite-sheet
runtime that its import needs (the `jasperengine.spritesheet` v1 record, `SpriteSheetPool`,
`ImportedAssets`, the packer, and the Sprite's `sheet` + `frame` + `frameName` references; moved here
from S45 in the family's consistency pass, R45–R52), and S45 (wave 5B) builds texture sheets and
Sprite Atlases on top of it. M1–M2 (the codec, `PixelImage`, composition, the family's shared editor
types) depend on nothing and unblock S46, S47 and S50. Uses S04 (GUIDs, sub-assets), S05 (file
watcher), S14 (cooks), S24 (undo), S31 (MCP groups and budget); S41 (same wave) for PPU, until it
lands 1 pixel = 1 unit.
Owner area: `editor/pixelart/` (`PixelImage`, `PixelProblem.h`, `lint/LintTypes.h`),
`editor/pixelart/aseprite/` (codec, composition, importer, detection), `engine/core/sprite/` (sheet
record, pool, frame geometry), `engine/core/asset/ImportedAssets.*`, the sheet fields of
`engine/core/component/SpriteComponent.h`, `editor/sprite/SpritePacker.*`,
`editor/window/SpritePreviewWindow.*`, `editor/window/AsepriteImporterView.*`,
`editor/mcp/tools/SpriteTools.cpp` (the `sprites` group module, shared with S45 and S47),
`docs/aseprite.md`, `docs/mcp/guide/sprites.md` (its Import section).

## 1. Goal

Aseprite is the drawing tool for people; the engine has no painting canvas. So the bridge must be
invisible: a person saves in Aseprite and, a fraction of a second later, the Scene view, the Game
view and every running preview show the new pixels, tags and slices, with nothing exported by hand.
JasperEngine gets its own C++ reader **and writer** for `.aseprite`/`.ase` that understands every
chunk of the published format and re-saves an untouched file byte for byte, so agents (S47) can edit
the same files a person edits without damaging what they do not touch. An imported file becomes an
**AsepriteSprite** asset: a packed sheet, sprite frames with pivots and 9-slice borders, one
Animation Clip per tag, named slices, user data, all with stable GUIDs. A read-only **Sprite
Preview** panel shows the file as the game will show it.

## 2. Scope

**In scope**
- `PixelImage`, the shared RGBA8 image with an optional index plane (`editor/pixelart/`).
- The codec (`editor/pixelart/aseprite/`): read every chunk of the format (§4.1), keep unknown
  chunks, write any document, byte-identical re-save of unchanged content, composition that matches
  Aseprite's own export pixel for pixel, a JSON description shared with S47.
- The family's shared editor types: `editor/pixelart/PixelProblem.h` (the typed error and the
  `Verdict`) and `editor/pixelart/lint/LintTypes.h` (the lint result types S46, S48 and S49 share).
- The minimal sprite-sheet runtime (R45–R52): the `jasperengine.spritesheet` v1 record, its pool,
  `ImportedAssets`, trimmed frame geometry and the From Frame pivot, the Sprite's frame references,
  the clip binding on sheet sprites, the packer, their serialization and Lua.
- The `AsepriteSprite` asset type, its import settings in `.meta`, the importer (a job), its cache,
  its outputs (sheet frames, pages, clips, slices, user data, palette and tileset hand-offs) with
  stable sub-asset GUIDs, export packaging, and hot reload through S05.
- Editor: the Aseprite Importer inspector, the Sprite Preview panel, Open in Aseprite (detection per
  OS, a setting), drag and drop into the Scene view, Hierarchy and Animation window.
- Runtime: Lua access to tags, slices and user data; colliders that follow a slice per frame.
- MCP: `set_sprite_import_settings` (group `sprites`) and parameters on existing tools; the CLI
  subcommand `jasperengine-editor aseprite`.
- The golden fixture corpus and its generator script.

**Out of scope**
- Any drawing, painting or pixel editing UI in the editor (owner decision, ledger). Agents draw
  through S47, which owns `get_sprite` and every editing tool; S44 gives S47 the codec.
- The Sprite Atlas asset, texture sprite sheets (Sprite Mode, `name@WxH`, the Slicer), the Sliced
  draw mode, growable legacy inline frames and their conversion: **S45**, built on S44's record,
  pool and packer.
- Palette assets and colour maths: **S43** (S44 hands it the palette entries). Grid tilemaps from
  tileset and tilemap chunks: **S50** (S44 decodes them). Cleanup: **S46**. PPU and pixel snapping:
  **S41**; lossless defaults: **S42**.
- Exporting from the engine to `.png`/`.json` sheets for other engines; Aseprite's `.aseprite-data`
  extensions and Lua scripts; Photoshop `.psd` layers.
- Nothing in the do-not-copy list of `docs/specs/README.md` is involved.

## 3. Current state (read on `specs/pixel-art`, 1b1c34fbd)

- **No Aseprite support at all.** No source file mentions `aseprite` or `.ase` (grep of
  `engine/core` and `editor`). `Util::isImageFile` (`editor/util/Util.h:32-39`) lists `.png .jpg
  .jpeg .bmp .tga .gif .hdr .psd .pic .pnm .svg`; the asset type table
  (`editor/asset/AssetTypes.cpp:23-54`) has no row for `.aseprite`, so such files are untracked:
  no GUID, no meta, no reload (`reload::kindOf`, `editor/reload/handlers/Common.cpp:19-44`, files
  them under nothing).
- **Asset types** are a file format, append only (`engine/core/asset/AssetType.h:12-40`; the last
  value is `VertexAnimation = 23`). `assetTypeFromName` loops up to `VertexAnimation`
  (`AssetType.h:74`), so a new last value must move that bound.
- **Sub-assets** (S04) exist for models only: `SubAssetKind {Mesh, Material, Animation, Skin,
  Image}` (`engine/core/asset/SubAssetKeys.h:107`), keys `<kind>:<name>` / `<kind>#<index>`
  (`:100-106`), containers `.gltf .glb .obj` (`isSubAssetContainerPath`, `:185-196`). The meta
  (`editor/asset/AssetMeta.h:40-53`) keeps `import:` opaque and preserves unknown sections.
- **Sprites** hold up to `MAX_SPRITE_FRAMES` (128, `engine/core/Engine.h:105-106`) inline
  `framesRect` (`engine/core/component/SpriteComponent.h:37-38`), a preset pivot only
  (`:13-23`, `:40`; `MeshSystem::createSprite`, `engine/core/subsystem/MeshSystem.cpp:668-689`), no
  per-frame offset, pivot or border. A frame swap is a texture-rect change
  (`MeshSystem::setSpriteFrameRect`, `:1180-1206`). S44 adds sheet references (R48); S45 makes the inline list growable.
- **Sprite animation** today: the Sprite Animation action (`SpriteAnimationComponent.h:12-25`,
  frame indices plus per-frame ms, 128 at most) started by `Sprite::startAnimation`
  (`engine/core/object/Sprite.cpp:256-294`, by name: frames whose name *contains* the string), and
  Animation Clip property tracks `{component: sprite, property: frame}` that step through frame
  indices (`docs/animation-clips.md:218,236`; `engine/core/animation/PropertyBindings.cpp:395-431`
  finds the shown frame by comparing texture rects).
- **Read-only sub-clips** exist for models: `"<model>#<clip>"` references, split after a model
  extension (`engine/core/animation/ModelSubClips.h:4-30,56-60`), resolved by `AnimationClipPool`
  (`engine/core/pool/AnimationClipPool.h:4-50`), and extracted to editable `.anim` files by
  Extract to .anim (`docs/animation-clips.md`). `.anim` is canonical JSON, `ANIM_VERSION = 2`
  (`engine/core/animation/AnimationClipIO.h:52-58`).
- **Hot reload** (S05) dispatches by kind (`editor/reload/handlers/ReloadHandlers.h:36-88`):
  textures, models, materials, shaders, particles, audio, fonts, animations, scripts, HTML UI.
- **Cooks** (S14): an export reads `assets/je-cooked/<family>/index.json`, the editor installs a
  provider (`engine/core/texture/CookedTextures.h:4-50`); sprites cook to BC7/ASTC/ETC by default
  (`docs/textures.md`, the format table), which is lossy.
- **zlib**: the editor links miniz (`CMakeLists.txt:628-630`, `editor/EditorToolsSources.cmake:44`);
  `stb_image` decodes zlib streams (`editor/spritenormals/SpriteMedia.cpp:594`).
- **Closest editor precedent**: the Sprite Normal Map window (`editor/window/SpriteNormalsWindow.*`,
  `editor/spritenormals/`): integer zoom, NEAREST preview, one session shared by window, MCP and CLI,
  `generate_sprite_normals` previews then writes with `output: true`.
- **MCP**: `get_asset_info` and `view_asset` are core (`editor/mcp/tools/AssetTools.cpp:3798-3829`),
  `import_files` copies files in (`:3916-3937`), `set_sprite_frames` / `set_sprite_animation` are
  core (`editor/mcp/tools/WorldTools.cpp:5645-5683`). The `assets_pipeline` group is full (12 tools,
  `docs/mcp/TOOLS.md` header). No `sprites` group exists yet (`editor/mcp/McpToolGroups.cpp:16-37`).

## 4. Reference

### 4.1 The file format
The published spec, `https://github.com/aseprite/aseprite/blob/main/docs/ase-file-specs.md`
(read 2026-11-04). Little-endian; 128-byte header (flags 1 = layer opacity valid, 2 = group
blend/opacity valid, 4 = layers have UUIDs; pixel ratio; grid); 16-byte frame headers (old/new chunk
counts, duration ms); chunks 0x0004 and 0x0011 (old palettes), 0x2004 layer (flags incl. 64 =
reference layer; types image/group/tilemap; blend modes 0–18; tileset index; UUID), 0x2005 cel
(raw, linked, compressed image, compressed tilemap with tile/flip/diagonal masks; z-index),
0x2006 cel extra, 0x2007 colour profile (none/sRGB/ICC), 0x2008 external files, 0x2016 mask
(deprecated), 0x2017 path (unused), 0x2018 tags (direction 0–3, repeat), 0x2019 palette (names),
0x2020 user data (text, colour, properties maps with 19 property types, nested maps, 128 levels,
association rules), 0x2022 slices (per-frame keys, 9-patch centre, pivot), 0x2023 tileset
(external/embedded, empty tile 0, base index). NOTE.5 (z-order: `layerIndex + zIndex`, ties by
zIndex) and NOTE.6 (which opacities are valid) are normative here.

### 4.2 Code to port or study
| Source | What we take | What we do better |
|---|---|---|
| tenten `src/aseprite.ts` (MIT, debuff; 238 lines) | The chunk walk, old/new palette precedence, linked-cel resolution, slice and tag parsing, the indexed writer's chunk order | Keep RGBA/grayscale/indexed natively (tenten maps everything to ≤255 palette indices and fails above), groups, blend modes, cel opacity, z-index, user data, tilemaps, unknown chunks, byte-identical re-save |
| Pixelorama `src/Classes/SoftwareParsers/AsepriteParser.gd` (MIT) | Blend-mode mapping, user data property decoding (`parse_aseprite_variant`, all types), tileset and tilemap cel decoding, cel z-index | Typed errors instead of silent skips; tile flips |
| Pixelorama `src/Classes/AnimationExporters/AsepriteExporter.gd` (MIT) | Writer layout for groups, tilesets and user data | Determinism, minimal re-encoding |
| Aseprite `src/doc/blend_funcs.cpp` (MIT, "Aseprite Document Library", Igara Studio / David Capello) | The 19 RGBA and grayscale blenders and their 8-bit rounding (`MUL_UN8`, `DIV_UN8` from the same library's headers), both the classic and the `_n` ("new blending") variants | Ported as is: our composition must equal Aseprite's export bit for bit |
| Aseprite `src/render/render.cpp` (MIT, "Aseprite Render Library") | The render plan: groups rendered into a cleared temporary then composited with the group's opacity and blend; cel × layer opacity via `MUL_UN8`; hidden and reference layers skipped; background first | Only the parts an export uses (no onion skin, no UI opacity) |

Ported files keep their headers; `NOTICE.md` gets entries for tenten, Pixelorama and the two
Aseprite libraries. tenten's README states its codec was written from the spec, not from
Aseprite's source; ours ports Aseprite's MIT libraries for the blending only.

### 4.3 Product references
Unity's **Aseprite Importer** (`com.unity.2d.aseprite`: import modes Merge Frames / Individual
Layers, Include Hidden Layers, Pivot, Generate Animation Clips, an inspector with layer lists, files
re-import on save) and Godot's Aseprite Wizard (tags to `AnimatedSprite2D`/`AnimationPlayer`,
slices). We take Unity's names and its sub-asset model (the texture, sprites and clips live inside
the imported asset) and Godot's tag-driven playback, and go beyond both with live reimport inside a
second, per-frame slice colliders, byte-identical round trips and agent tools.

## 5. Requirements

### Functional — the codec
- **R1 Read everything.** `aseprite::read` decodes the header, every frame and every chunk of §4.1
  in colour depths 32 (RGBA), 16 (grayscale + alpha) and 8 (indexed). Old files: every frame gets
  the header speed, then frames whose duration field is > 0 override it. A palette chunk 0x2019
  wins over 0x0004/0x0011 (0x0011 values are scaled ×255/63, rounded); entry names are kept.
- **R2 Layers.** Image, group and tilemap layers with all flags (visible, editable, lock movement,
  background, prefer linked, collapsed, reference), the parent tree rebuilt from child levels
  (NOTE.1), blend modes 0–18, opacity (valid per NOTE.6: when the header flag says it is not, it
  reads as 255), tileset index, UUIDs (header flag 4), user data.
- **R3 Cels.** Raw, linked (resolved to the frame they link to, kept as links), compressed image
  and compressed tilemap cels at any offset, partly or wholly off-canvas; cel opacity, z-index, cel
  extra (precise bounds) and user data. A link to a missing cel is a warning and an empty cel.
- **R4 Metadata.** Tags (from, to, direction forward/reverse/ping-pong/ping-pong reverse, repeat,
  the deprecated colour, name, and the user data that follows each tag); slices (flags, every key:
  frame, bounds, 9-patch centre, pivot; a key of width 0 hides the slice from its frame on); user
  data on the sprite, layers, cels, tags, slices, tilesets and tiles, with all 19 property types,
  mixed-type vectors, nested maps up to 128 levels and extension-keyed maps; tilesets (embedded
  pixels, external references, empty-tile flag, base index, auto-flip flags); colour profile
  (type, fixed gamma, ICC bytes); external files; the deprecated mask chunk; grid and pixel ratio.
- **R5 Nothing is dropped.** A chunk of an unknown type, or the unused path chunk, is kept as
  opaque bytes anchored to its frame and the object read before it, and written back in place.
- **R6 Hostile input.** Every length, count, offset and decompressed size is checked against the
  remaining bytes and against limits (canvas ≤ 16,384 × 16,384, ≤ 65,535 frames, decoded pixels
  ≤ 1 GiB, user data nesting ≤ 128); a violation is a typed error `{kind, message, fix, field}`
  (§6.9), never a crash, an assert or an unbounded allocation. Recoverable problems (a link to a
  missing cel, a slice key past the last frame) are warnings with the chunk offset.
- **R7 Write.** `aseprite::write` encodes any valid `Document` in any colour mode, with groups,
  tilemaps, user data, tilesets and unknown chunks. Output is deterministic (same document →
  same bytes on every platform; miniz at a fixed level). New files follow Aseprite 1.3's layout
  (both palette chunks when the palette has ≤ 256 entries and no alpha, the sRGB profile chunk,
  user data after the palette for the sprite). **Unchanged content is not re-encoded**: each chunk
  read keeps its original bytes and a fingerprint of its decoded meaning, and the writer emits the
  original bytes when the fingerprint still matches, so `write(read(f)) == f` byte for byte for
  every file Aseprite wrote, and an edit changes only the chunks (and frame headers) it touched.
- **R8 Composition.** `aseprite::renderFrame` produces the frame exactly as Aseprite's
  `--sheet` export does: back to front by `layerIndex + zIndex` (NOTE.5), groups rendered into a
  cleared temporary and composited with the group's opacity and blend mode (when header flag 2 is
  set), cel opacity × layer opacity, the ported blenders (§4.2, the `_n` variants: Aseprite 1.3's
  default "new blending"), hidden and reference layers skipped unless asked for, indexed cels
  through the palette with the transparent index clear (except on the background layer), grayscale
  as gray RGBA, tilemap layers drawn from their tileset with flips. Options select layers, include
  hidden or reference layers, render one layer alone, or keep the index plane (R10).
- **R9 Description.** `aseprite::describe(doc, detail)` returns the document as JSON (size, mode,
  palette, layers tree, frames with durations, tags, slices, tilesets, user data; with `cels` detail
  the cel table) in one shape used by `get_asset_info`, S47's `get_sprite` and the CLI.

### Functional — `PixelImage`
- **R10** `PixelImage` (`editor/pixelart/PixelImage.h`) is the one image type of the pixel-art
  family: width, height, straight-alpha RGBA8 rows top-left first, and an optional index plane of
  the same size; pixels are `jasperengine::pixelart::Rgba8` (`engine/core/pixelart/Rgba8.h`, the
  family's one colour type, shared with S43). When the index plane is present the RGBA is its
  palette lookup; operations that
  cannot keep that invariant drop the plane and say so in their return value. It has crop, blit
  (with and without blending), opaque bounds, equality, a 64-bit content hash, flips, PNG
  load/save/encode, and no GPU or editor-UI dependency (S43, S46, S47, S48, S50 use it from jobs).

### Functional — the import
- **R11 Asset type.** `.aseprite` and `.ase` are `AssetType::AsepriteSprite` (name
  `aseprite_sprite`), sidecar identity, path base assets, with sub-assets (§6.5). Old projects that
  already hold such files get their metas on first open (S04 R3, deterministic GUIDs).
- **R12 Import settings** live in the meta under `import.asepriteImporter` (§6.4), all optional
  with the defaults of §6.4. Editing them is one project undo step and reimports.
- **R13 Layers.** By default the visible, non-reference layers are imported (Aseprite's export
  rule). Settings add or remove layers by name or path (`Body/Arm`), include hidden layers, and
  ignore layers whose name starts with a listed prefix. A filter that names a missing layer is a
  warning naming the layers there are.
- **R14 Import modes.** `sprite_sheet` (Unity's Merge Frames): one frame per Aseprite frame, the
  selected layers flattened. `layers_as_sprites` (Unity's Individual Layers): each selected
  top-level layer or group becomes its own set of frames (`<layer>/<frame>`), all sharing the
  canvas origin, so a hierarchy of child sprites lines up exactly; cel z-index is applied in
  `sprite_sheet` mode only (a warning lists the cels whose z-index the split cannot express).
- **R15 Frames.** Each output frame carries its name (§6.5), duration, source size (the canvas),
  trim rectangle and offset (trimming on by default, alpha 0 only), pivot (R17), border (R17) and
  pixel ratio. Identical frames are packed once. The frames go to the packer (R52) as one sheet per
  file (`texture.layout: packed`, or `grid`: untrimmed cells in rows, Aseprite's sheet look); pages
  are RGBA8, nearest filtering, no mipmaps and uncompressed by default (S42: lossless), cooked
  through S14 like any texture. Once S45 lands, a file can also be a member of a Sprite Atlas, whose
  pages then hold its frames (S45 R2, transparent to sprites).
- **R16 Animation Clips.** One read-only clip per tag (on by default), referenced as
  `<file>#<tag>` like model clips: a property track `{path: "", component: sprite, property:
  frame}` with step keys at the cumulative frame durations; direction and repeat unrolled (§6.6);
  `wrap: loop` when repeat is 0, else `once`. A file without tags gets one clip of all frames named
  after the file. In `layers_as_sprites` mode a clip has one track per layer child. Cel user data
  text on a layer named by `events.layer` (default `events`) becomes clip events at that frame's
  start (`onFootstep("left")`, the `.anim` event grammar). Clips play in the Animation Player and
  Animator, show in the Animation window read-only, and Extract to .anim (and `edit_animation
  create {from}`) copies one into an editable file.
- **R17 Slices.** A slice's key for each frame (keys hold until the next key) is exported per
  frame. The pivot source (§6.4) can be a preset, a normalized point, or the pivot of a named slice
  (default: a slice named `pivot`, else the first slice with a pivot); the border source is a
  9-patch slice (default: the first one), turned into `left/top/right/bottom` pixels of the frame
  (frames with a border are never trimmed). Every slice is also kept by name in the sheet, with
  its keys, centre, pivot and user data, for Lua (R31) and colliders: a 2D collider with `slice:
  "<name>"` becomes the slice's box on every frame (rebuilt only when the box changes, disabled on
  frames where the slice is hidden), and **Create Colliders from Slices** adds such colliders in
  one undo step (sensor when the slice's user data has `sensor: true`).
- **R18 User data.** The sprite's, tags', slices' and layers' user data (text, colour, user
  properties) are in the sheet record and readable from Lua and MCP. Sprite-level user properties
  whose keys have the form `<component>.<property>` (the Catalog names `set_properties` uses, e.g.
  `light2d.intensity`) are applied when the file is dropped into a scene and by **Apply Sprite
  Properties** in the Inspector, as one undo step; others are data only. Tag colours from user
  data show in the Sprite Preview and the Animation Player's clip list.
- **R19 Hand-offs.** The palette (entries, names, transparent index) is a `palette#0` sub-asset
  S43 reads as a Palette; tilesets (`tileset:<name>`: tile size, base index, the tile strip as a
  `PixelImage`, per-tile user data) and tilemap cels are exposed through
  `AsepriteImport::tilesets()` / `tilemapCels()` for S50. Until S50 lands, tilemap layers import
  as pixels (Aseprite's export does the same).
- **R20 Stable GUIDs.** Every output is an S04 sub-asset with a GUID that survives reimports,
  file renames, frame edits and tag renames (§6.5).
- **R21 Cache and export.** Imports run as Background jobs (S06) and write a derived, deletable
  cache keyed by content (§6.5). Exports ship only the generated outputs (`assets/je-imported/`,
  the `ImportedAssets` index of R47, pages cooked through S14); the `.aseprite` file never ships.
- **R22 Hot reload.** A save from Aseprite (any program) reaches S05 as a change of kind
  `aseprite`; the importer reruns on a job, and on success the sheet, pages, clips, slices and
  every Sprite Preview swap in place with no flicker (the old version stays until the new one is
  ready). A file that fails to read (half written) keeps the previous import, logs `[Hot Reload]
  <path>: <reason> (kept the previous version)` and is retried after 0.5 s (S05's rule). The pool
  bumps the sheet's generation (R46), so atlases that pack the file rebuild once S45 is present.
  Players of affected clips rebind (clip pool generations).
- **R23 Editor writes.** A write by the editor itself (S47's tools, which S05's self-write
  suppression hides from the watcher) calls `AsepriteImporter::reimport(path)` directly, so agent
  edits show up exactly like a person's.

### Platform & backends
- **R24** The codec and importer are editor-only (Linux, Windows, macOS); no `.aseprite` code is
  compiled into players. Games load only the sheet records (R45), the generated clips and cooked pages,
  which work on GL 3.3, GLES3/WebGL2, Metal, D3D11 and Vulkan and every export target because
  they are plain textures, quads and property tracks. Byte order is handled explicitly (no
  reinterpret casts of file bytes), so big-endian hosts would read correctly too.
- **R25** A project without `.aseprite` files pays nothing: no job, no cache directory, no
  per-frame work. Players carry the sheet runtime (R45–R51), which a sprite without a `sheet` never
  touches (today's code path), and the slice-collider field (R17).

### Performance (editor Release, the CI Linux runner)
- **R26 Codec.** Read of a 256×256, 64-frame, 8-layer RGBA file (≈1.5 MB) ≤ 40 ms; composition
  of one 256² frame with 8 layers ≤ 1 ms; write of the same file with every cel changed ≤ 120 ms,
  unchanged ≤ 5 ms. Peak memory while reading ≤ 3× the decoded pixel bytes.
- **R27 Import.** A 64×64, 24-frame, 4-tag file: full import (decode, compose, trim, pack, write
  cache) ≤ 50 ms on one job thread; cache hit ≤ 1 ms. First open of a project with 200 such files
  never blocks the main thread more than 2 ms per frame (S05 R22).
- **R28 Live loop.** From the end of a save in Aseprite to the new frame visible in the Scene view
  (native watcher): ≤ 300 ms p95 for the 64×64 file, ≤ 1 s for a 512×512, 120-frame file; no editor
  frame longer than the median + 16 ms during the swap.
- **R29 Runtime.** Slice colliders cost nothing for colliders without `slice`; with it, one
  4-point rebuild only on frames whose box changed.

### Editor UX (human)
- **R30 Aseprite Importer inspector** (selecting `.aseprite` files in the Project window,
  multi-select edits all, Apply / Revert, one undo step): sections **General** (Import Mode,
  Pixels Per Unit (S41), Pivot, Border), **Layers** (Include Hidden Layers, Ignore Prefixes, the
  layer tree with checkboxes, blend mode and opacity shown), **Animation** (Generate Animation
  Clips, the tag list with frames, direction, repeat and length, Events Layer), **Slices** (the
  slice list, what each feeds: pivot, border, collider), **Texture** (Layout Packed/Grid, Padding,
  Extrude, Max Size, Compression, the packed-in atlas if any), a summary line
  (`4 tags · 24 frames · 1 page 128×64 · 3 slices · 2 warnings`), the warnings, and the buttons
  **Open in Aseprite**, **Open Sprite Preview**, **Reimport**, **Extract Clips…**.
- **R31 Sprite Preview** (Window > 2D > Sprite Preview; also double-click when Aseprite is not
  found, and the inspector button): read-only. A tag dropdown with play/pause/loop and speed, frame
  stepping (`,` and `.` as in Aseprite, Home/End), a frame strip with durations and tag bands in
  their colours, onion skin (previous/next 1–3 frames, Aseprite's red/blue tints, opacity),
  layer toggles (preview only; **Exclude from Import** turns the current toggles into the import
  setting, one undo step), slice and pivot overlays with names, the trim rectangle, a pixel grid,
  integer zoom only (1×–32×, wheel and `+`/`-`, Fit picks the largest integer), checkerboard or
  scene background, Source (the file, all layers) / Imported (what the game draws) toggle, and
  **Compare at 1×**: the current frame next to a chosen reference tile (a frame of a sheet,
  remembered per project) at the Game view's integer scale (S41), warning when their pixel sizes
  differ (`hero 32 PPU, tiles 16 PPU: mixed pixel sizes`). It follows hot reloads live.
- **R32 Open in Aseprite.** A button, a Project window context entry, and double-click on an
  `.aseprite` file open it in Aseprite, detached. The executable is found per OS (§6.8) or set in
  **Preferences > External Tools > Aseprite** (path, **Detect**, **Browse…**, the detected version);
  when none is found the editor says so with **Locate…** and a link to aseprite.org, and
  double-click falls back to the Sprite Preview.
- **R33 Drag and drop.** Dropping an `.aseprite` file on the Scene view or Hierarchy creates a
  GameObject named after the file with a Sprite using the file's sheet (frame 0, pivot from the
  frame), an Animation Player with the tag clips (Default Clip: a tag named `idle`,
  case-insensitive, else the first; Play Automatically on), the sprite properties of R18, and in
  `layers_as_sprites` mode one child per layer; all one undo step. Dropping on an existing Sprite
  swaps its sheet (one step); dropping on the Animation window or an Animation Player adds the clips.
- **R34 Feedback.** The Project window thumbnail is frame 0 (animated while hovered); an import
  in progress shows the S14-style badge; the Console logs one line per import or batch
  (`[Aseprite] Imported sprites/hero.aseprite: 24 frames, 4 clips (38 ms)`), clickable to select.

### Agent UX (MCP, Lua, CLI)
- **R35 `set_sprite_import_settings`** (group `sprites`): partial update of the import settings
  of one or more `.aseprite` files. Without `output` it is a preview: the resolved settings, the
  outputs they would give (frames, pages and their size, clips with lengths, slices, warnings) and,
  with `include_preview`, a contact sheet image; the same arguments plus `output: true` write the
  metas (one undo step), reimport, and return the outputs with their sub-asset GUIDs and reference
  paths. More than 1 s of work becomes a job.
- **R36 Existing tools.** `get_asset_info` on an `.aseprite` gives the R9 summary (no cels), the
  stored and resolved settings, the outputs with GUIDs and reference strings, warnings and the cache
  state, and names `get_sprite` (S47, wave 5C; named once it is registered) for cels and pixels. `view_asset` gives the imported
  frames as a contact sheet labelled with frame indices and tag bands (`max_size` bounds it).
  `set_sprite_frames` gains `sheet` (a sheet path), `frame` as an index or a name, and `clips`
  (with an `.aseprite` sheet: add or update an Animation Player with the tag clips, default true);
  S45 adds `inline`, `convert` and `all_users`.
  `get_animation` reads `<file>#<tag>` clips; `edit_animation`
  on one is refused with `read_only_clip` and the fix (`create {from}`). Import problems appear in
  `external_changes.problems` like `.anim` load warnings.
- **R37 CLI.** `jasperengine-editor aseprite info <file> [--json] [--cels]`, `render <file> <out.png>
  [--frame N|--tag T] [--layers a,b] [--sheet horizontal|rows] [--scale N]`, and `resave <file>
  <out>` (exit 0 ok, 2 usage, 3 read error, 4 written bytes differ from the input when
  `--expect-identical`), without a project: the golden tests and agents use them.
- **R38 Lua.** On `Sprite` (with R51's sheet properties): `sprite:playTag(name [, speed])`, `stopTag()`,
  `tag` (current tag name or nil), `isTagPlaying`, `getTag(name)` → `{name, from, to, direction,
  repeat, durationMs, color, userData}`, `getTags()`, `getSlice(name)` → `{name, rect, center,
  pivot, visible, userData}` for the current frame in the GameObject's local units, `getSlices()`,
  `getUserData()`. `SpriteSheet` (R51) gets the same `getTag`/`getSlice(name, frame)`/`tags`/
  `slices`/`properties`. `playTag` follows direction and repeat with the Sprite Animation action
  (no 128-frame limit: it steps from–to, reading durations from the sheet) and fires no clip
  events (use the Animation Player for events).
- **R39 Guide.** `docs/mcp/guide/sprites.md` (`jasper://docs/sprites`, shared with S45 and S47)
  gets the Import section: the settings keys, outputs and reference strings, the human/agent loop
  (a person saves, the agent re-reads `get_asset_info`), and the errors; the agent guide gets one
  line: the shared Sprites line of the `## Pixel art` digest section, which S44 adds in wave 5A
  (§6.1).

### Data & compatibility
- **R40** `AssetType::AsepriteSprite = 25`, the family's reserved value (ledger: Palette 24,
  AsepriteSprite 25, SpriteAtlas 26, Tileset 27), whatever order the specs land in: the entry has
  its explicit value, `assetTypeFromName`'s bound is the highest value present and its loop skips
  unnamed values (a gap a sibling fills later);
  `SubAssetKind` gains `Frame`, `Slice`, `Tileset`, `Palette` (appended; frozen key names
  `frame`, `slice`, `tileset`, `palette`); `isSubAssetContainerPath` and sub-clip references accept
  `.aseprite`/`.ase`.
- **R41** `import.asepriteImporter` has `version: 1`; unknown keys are kept; a newer version is
  read-only in this editor (the inspector says so) and imported with the settings it understands.
- **R42** Generated formats are versioned (the sheet record v1, R45; the clips as `.anim` v1 text),
  and the importer version is part of the cache key, so a changed importer reimports on its own.
- **R43** The codec reads every file Aseprite 1.0–1.3.x writes and the writer's files open in
  Aseprite ≥ 1.3 (1.2 for files without tilemaps, UUIDs or z-index); the version a feature needs
  is listed in `docs/aseprite.md`.
- **R44** Scene and prefab changes: the collider's `slice` key (written only when set; older
  editors keep it as unknown data, S03) and the Sprite's sheet keys (§6.12). The sheet keys bump the
  scene and prefab formats (moved from S45 R20 with their migration content): each takes **the
  next version number at landing** (scene 7 and prefab 8 today, `editor/StreamMigrations.cpp:702-708`;
  if a sibling has bumped first, the next free numbers; no number is fixed in this spec), with
  identity migrations ("sprite sheets: an older file has none") and the migration test of AC23.
  The bump tells older editors they cannot read sheet sprites; S03's unknown-data preservation keeps
  the keys if they open the file anyway.

### Functional — the sprite-sheet runtime (moved from S45, so that S44 lands first)
- **R45 Sheet record.** A sheet is `jasperengine.spritesheet` version 1 (§6.11): pages (texture
  path, size), frames (name, page, rect in page pixels, source size, trim offset, pivot, border,
  duration), plus the tags, slices and user data the importer fills (R16–R18). Parsing is lenient
  after `format`/`version` (as `.anim`: bad entries dropped with a JSON-path warning); writing is
  canonical and deterministic.
- **R46 Pool.** `SpriteSheetPool::get(path)` gives the sheet of an `.aseprite`/`.ase` (this spec's
  output) and of any plain texture (one frame: the whole image), shared per normalized path with a
  generation that reloads bump (`AnimationClipPool`'s rules). Other sources register through
  `SpriteSheetPool::registerSource(...)`: S45 adds texture Sprite Mode, `name@WxH` and
  `.spriteatlas` sheets and the atlas remap of packed sources, without changing what sprites see.
- **R47 `ImportedAssets`.** Finds generated runtime files by `"<source>#<key>"`: in exports from
  `assets/je-imported/index.json`, in the editor through a provider answering from
  `.jasperengine/cache/imported/v1` (`CookedTextures`' pattern). Keys `<source>#sheet`,
  `<source>#image#<n>`, `<source>#animation:<tag>`; S45 adds `<atlas>#sheet`.
- **R48 Frame reference.** `SpriteComponent` gains `sheet` (`AssetRef`), `frame` (index) and
  `frameName` (the name when saved). On load the frame is found by `frameName` when the sheet has
  it, else by `frame` (clamped, with a warning in the load report), so renaming an Aseprite file or
  (with S45) reordering an atlas keeps the right frame. A sprite with a `sheet` uses only the
  sheet's frames: its inline frames are ignored and not saved, and its texture is never saved (the
  sheet decides; the mesh encoder leaves out submesh 0's `baseColorTexture`).
- **R49 Geometry and pivot.** A sheet frame is drawn at its **source size**: the quad covers the
  trimmed rect placed at its trim offset, so trimmed and untrimmed packing draw identical pixels at
  identical positions (`SpriteFrameGeometry`). `width`/`height` 0 (the default for sheet sprites)
  means the frame's native size (S41 divides by the PPU); non-zero values stretch it. The quad is
  rebuilt only when the next frame's trim geometry differs; frames with equal geometry only swap the
  texture rect, as today. `PivotPreset::FRAME` (appended; Inspector "From Frame") uses the frame's
  pivot in source pixels and is the default for sprites created from a sheet; the nine presets keep
  their meaning, relative to the source size. A frame on another page binds that page as submesh
  0's base colour texture with the sprite's own sampler settings. S45 adds the Sliced draw mode.
- **R50 Clips on sheet sprites.** A `sprite.frame` key on a sheet sprite is an index into its sheet;
  the binding reads and writes `SpriteComponent::frame` directly (no rect comparison). The binding
  report warns when a clip's `source.path` names another sheet than the sprite's
  (`clip made for hero.aseprite, sprite shows slime.aseprite`). Sprite Animations
  (`set_sprite_animation`) accept sheet frame names and indices.
- **R51 Lua.** `Sprite`: `sheet` (property, a path; setting it loads the sheet and shows frame 0),
  `frame` (index property), `frameName` (property, set by name), `frameCount`, `getSheet()` →
  `SpriteSheet` or nil; `setFrame(index|name)` works on sheets. `SpriteSheet`:
  `SpriteSheet.load(path)` (any source the pool knows; nil with a log error when missing), `path`,
  `frameCount`, `pageCount`, `frameNames`, `findFrame(name)` → index or -1, `getFrame(name|index)` →
  `{name, index, page, rect, sourceSize, offset, pivot, border, durationMs}`. S45 adds `drawMode`
  and `.spriteatlas` sources.
- **R52 Packer.** `editor/sprite/SpritePacker` (pure, any thread) packs frames into pages exactly
  as §6.10 says: MaxRects BSSF, trim, extrusion, padding, duplicates, Keep Members Together,
  POT/square, block alignment, max size, several pages. S44 packs each Aseprite file as one member
  (`texture.layout: packed`); S45's atlas builder calls the same function with many members.

## 6. Design constraints & decisions

### 6.1 MCP tools and the budget (S31)

| Tool | Group | Change | Estimated listing chars |
|---|---|---|---|
| `set_sprite_import_settings` | `sprites` (new group) | new: `paths`, `settings` (free-form, keys in `jasper://docs/sprites`), `output`, `include_preview` | ≤ 1,100 |
| `get_asset_info` | core | description +`; .aseprite: frames, layers, tags, slices, import outputs` | +60 |
| `view_asset` | core | description +`.aseprite frames as a sheet` | +35 |
| `set_sprite_frames` | core | `sheet` (string), `frame` (integer\|string), `clips` (boolean), no per-parameter descriptions (meanings in `jasper://docs/sprites`); S45 adds `inline`, `convert`, `all_users` | +110 (S45 +70 more). The tool has a grandfathered `exceptions` limit of 2,075 (1,977 when S31 measured it): S44 cuts its description by ≥ 120 characters (the per-argument prose moves to `jasper://docs/sprites`) so S44 + S45 stay within 2,075; the limit is never raised |
| `get_animation`, `edit_animation` | core | none (accept `.aseprite#tag` like model clips) | 0 |

- **Ownership in `sprites`** (≤ 12 tools, ≤ 15,000 chars, `group_default`): S44 1 tool (≤ 1,100),
  S45 2 (`edit_sprite_atlas` ≤ 1,300, `get_sprite_atlas` ≤ 900), S47 6 (≤ 5,350 with `edit_sprite`
  ≤ 1,450): 9 tools, ≈ 8,650 characters (ledger "MCP budget"). **`get_sprite` is S47's**: it
  describes the document (layers, cels, pixels, palette) through `aseprite::describe`; S44 adds no
  describe tool and puts its import view in `get_asset_info`. Two agents' needs, one shape:
  `get_asset_info` embeds the same JSON at `summary` detail.
- The group row: `{"sprites", "Aseprite import settings, sprite atlases, and agent sprite
  drawing on .aseprite files (jasper://docs/sprites)."}`, added by S44 (wave 5A, the first spec of
  the group to land); S45 and S47 add their tools to it.
- Core: +≈205 chars from S44 (`get_asset_info` +60, `view_asset` +35, `set_sprite_frames` +110, less
  the ≥ 120 its description sheds); the default listing must still fit `budget.json`; if not, the
  description additions move to `jasper://docs/sprites` (never a raised limit).
- Guide: one Sprites line in the `## Pixel art` digest section (S41 creates it, S41 §6.4), ≈110
  bytes, added by S44 and shared by S44/S45/S47: "Sprites: Aseprite files
  are the art (people edit in Aseprite, agents with the sprites group); sheets, atlases, tags:
  jasper://docs/sprites."
- Conventions: snake_case, closed objects except `settings` (`anyObject`, said so), preview then
  `output: true`, typed errors, a group-hidden-by-default test, `test_tool_coverage.py`,
  `gen_tool_docs.py`.

### 6.2 Module layout
```
editor/pixelart/PixelImage.{h,cpp}                 R10 (shared by the family)
editor/pixelart/PixelProblem.h                     the family's typed error and Verdict (§6.3)
editor/pixelart/lint/LintTypes.{h,cpp}             the family's lint result types and JSON (§6.3)
engine/core/pixelart/Rgba8.h                       the family's 8-bit colour (S43 §6.4; whichever lands first)
engine/core/sprite/SpriteSheet.h                   SpriteSheetData, SpriteFrame, SpritePage, tags/slices (§6.11)
engine/core/sprite/SpriteSheetIO.{h,cpp}           parse/write jasperengine.spritesheet (pure, any thread)
engine/core/sprite/SpriteSheetPool.{h,cpp}         shared_ptr per normalized path, generations, registerSource
engine/core/sprite/SpriteFrameGeometry.{h,cpp}     trimmed quad of a frame (CPU, unit tested; S45 adds 9-slice)
engine/core/asset/ImportedAssets.{h,cpp}           "<source>#<key>" -> generated file (CookedTextures' pattern)
editor/sprite/SpritePacker.{h,cpp}                 MaxRects (§6.10), pure; S45's atlas builder reuses it
editor/pixelart/aseprite/AsepriteDocument.h        the model (§6.3)
editor/pixelart/aseprite/AsepriteRead.cpp          R1-R6
editor/pixelart/aseprite/AsepriteWrite.cpp         R7
editor/pixelart/aseprite/AsepriteBlend.{h,cpp}     ported blend_funcs (MIT header kept)
editor/pixelart/aseprite/AsepriteRender.{h,cpp}    R8 (render plan ported from render.cpp)
editor/pixelart/aseprite/AsepriteDescribe.cpp      R9
editor/pixelart/aseprite/AsepriteImport.{h,cpp}    R11-R21: settings -> outputs, pure (any thread)
editor/pixelart/aseprite/AsepriteImporter.{h,cpp}  jobs, cache, reload, sub-asset table (main thread API)
editor/pixelart/aseprite/AsepriteLocator.{h,cpp}   §6.8
editor/pixelart/aseprite/AsepriteCli.cpp           R37
editor/window/AsepriteImporterView.*, SpritePreviewWindow.*
editor/mcp/tools/SpriteTools.cpp                   the sprites group module (S44, S45, S47 rows)
```
The codec namespace is `jasperengine::editor::pixelart::aseprite`; it has no dependency on
`Project`, ImGui or the GPU, so S46/S47 use it from jobs and the CLI.

### 6.3 Codec API sketch
```cpp
namespace jasperengine::editor::pixelart {
    using jasperengine::pixelart::Rgba8;     // engine/core/pixelart/Rgba8.h: {r, g, b, a = 255}, operator<=>
    struct IntRect { int x = 0, y = 0, w = 0, h = 0; };

    // PixelProblem.h: the typed error of every pixel-art tool ({kind, message, fix, field}),
    // lifted from spritenormals::Problem; and the family's verdict (ritmo's order)
    struct PixelProblem { std::string kind, field, message, fix; };
    enum class Verdict : uint8_t { Clean, Fixable, NeedsPerson };    // "clean", "fixable", "needs_person"

    // lint/LintTypes.h: one result shape for every lint (S46 R25; S46, S48 and S49 produce it)
    namespace lint {
        enum class LintStatus : uint8_t { Pass, Warn, Fail, Skip };
        struct LintSpot { int frame = 0, x = 0, y = 0; };            // frames from 1, x/y from the top-left
        struct LintResult {
            std::string id; LintStatus status = LintStatus::Skip;
            double measured = 0, threshold = 0; std::string thresholdSource;   // "default", "rules.max_colors"
            int count = 0; std::vector<LintSpot> at;                          // at: <= 8
            std::string message, fix;
        };
        struct LintReport { std::vector<LintResult> results; int pass = 0, warn = 0, fail = 0, skip = 0; };
        nlohmann::json toJson(const LintReport& report);              // the MCP / CLI shape
    }
    class PixelImage {                       // straight alpha, rows top-left first
    public:
        PixelImage() = default; PixelImage(int width, int height);          // transparent ({0, 0, 0, 0})
        static PixelImage fromIndices(int w, int h, std::vector<uint8_t> indices,
                                      std::span<const Rgba8> palette, int transparentIndex /* -1: none */);
        int width() const; int height() const; bool empty() const;
        std::span<Rgba8> pixels(); std::span<const Rgba8> pixels() const;
        bool hasIndices() const; std::span<const uint8_t> indices() const;
        bool setIndices(std::vector<uint8_t>, std::span<const Rgba8> palette, int transparentIndex);
        void dropIndices();
        Rgba8 at(int x, int y) const; void set(int x, int y, Rgba8);         // set drops indices
        PixelImage crop(IntRect) const; void blit(const PixelImage&, int x, int y);
        void blendOver(const PixelImage&, int x, int y, uint8_t opacity);   // Aseprite normal
        IntRect opaqueBounds(uint8_t alphaThreshold = 0) const;             // {0,0,0,0} if empty
        void flipX(); void flipY();
        uint64_t hash() const;                                              // xxHash64, indices too
        bool operator==(const PixelImage&) const;
        static std::optional<PixelImage> loadPng(const std::filesystem::path&, std::string* error);
        std::string encodePng() const; bool savePng(const std::filesystem::path&) const;
    };
}
namespace jasperengine::editor::pixelart::aseprite {
    enum class ColorMode : uint16_t { Rgba = 32, Grayscale = 16, Indexed = 8 };
    enum class LayerType : uint16_t { Image = 0, Group = 1, Tilemap = 2 };
    enum class BlendMode : uint16_t { Normal, Multiply, Screen, Overlay, Darken, Lighten, ColorDodge,
        ColorBurn, HardLight, SoftLight, Difference, Exclusion, Hue, Saturation, Color, Luminosity,
        Addition, Subtract, Divide };                                       // 0..18, the file values
    enum class TagDirection : uint8_t { Forward, Reverse, PingPong, PingPongReverse };
    using PropertyValue = std::variant<bool, int64_t, uint64_t, double, std::string, IntPoint, IntSize,
        IntRect, Uuid, PropertyVector, PropertyMap>;                      // keeps the file type code
    struct UserData { std::optional<std::string> text; std::optional<Rgba8> color;
                      std::map<uint32_t, PropertyMap> properties; };       // 0 = user properties
    struct Layer { std::string name; LayerType type; uint16_t flags; int childLevel; int parent = -1;
                   BlendMode blend; uint8_t opacity; uint32_t tilesetIndex; std::optional<Uuid> uuid;
                   UserData userData; bool visible() const; bool reference() const; bool background() const; };
    struct Tilemap { int columns, rows; uint32_t idMask, xFlipMask, yFlipMask, diagonalMask;
                     std::vector<uint32_t> tiles; };
    struct Cel { int layer, frame; int x, y; uint8_t opacity; int16_t zIndex;
                 int linkedFrame = -1;                                     // >= 0: a linked cel
                 std::shared_ptr<const PixelImage> image;                  // image cels (shared by links)
                 std::shared_ptr<const Tilemap> tilemap; std::optional<CelExtra> extra; UserData userData; };
    struct SliceKey { uint32_t frame; IntRect bounds; std::optional<IntRect> center; std::optional<IntPoint> pivot; };
    struct Slice { std::string name; uint32_t flags; std::vector<SliceKey> keys; UserData userData;
                   const SliceKey* keyAt(int frame) const; };               // nullptr: not on that frame
    struct Tag { int from, to; TagDirection direction; uint16_t repeat; std::string name; Rgba8 legacyColor; UserData userData; };
    struct Tileset { uint32_t id, flags; int tileWidth, tileHeight, baseIndex; std::string name;
                     std::optional<ExternalTileset> external; PixelImage tiles;     // tileWidth x (tileHeight*count)
                     UserData userData; std::vector<UserData> tileUserData; };
    struct Document {
        int width, height; ColorMode mode; uint32_t headerFlags; uint8_t transparentIndex;
        uint8_t pixelWidth, pixelHeight; IntRect grid; uint16_t speed;
        std::vector<Layer> layers; std::vector<uint16_t> frameDurations; std::vector<Cel> cels;
        std::vector<Tag> tags; std::vector<PaletteEntry> palette; std::vector<Slice> slices;
        std::vector<Tileset> tilesets; std::vector<ExternalFile> externalFiles;
        std::optional<ColorProfile> colorProfile; UserData spriteUserData;
        std::vector<OpaqueChunk> opaque;          // R5: type, frame, anchor, bytes
        Provenance provenance;                    // R7: original bytes + fingerprints, per chunk
        const Cel* cel(int layer, int frame) const;          // follows links
    };
    struct CodecError : PixelProblem { size_t offset = 0; bool empty() const; };
    struct ReadResult { std::optional<Document> document; std::vector<CodecError> warnings; CodecError error; };
    ReadResult read(std::span<const uint8_t> bytes, const ReadOptions& = {});
    ReadResult readFile(const std::filesystem::path&, const ReadOptions& = {});
    bool write(const Document&, std::vector<uint8_t>& out, CodecError* error);   // deterministic
    struct RenderOptions { bool includeHidden = false, includeReference = false, keepIndices = false;
                           std::vector<int> layers; /* empty: by the visibility rules */ int soloLayer = -1; };
    PixelImage renderFrame(const Document&, int frame, const RenderOptions& = {});
    enum class Detail { Summary, Full, Cels };
    nlohmann::json describe(const Document&, Detail);
}
```
`Provenance` is internal to the writer: S47 mutates the plain structs and never manages bytes.
Fingerprints are hashes of the decoded fields (and of the cel's pixels), so any change is seen.

### 6.4 Import settings (`.meta`, `import.asepriteImporter`, camelCase like S14's `textureImporter`)
```yaml
import:
  asepriteImporter:
    version: 1
    importMode: sprite_sheet          # sprite_sheet | layers_as_sprites
    layers:
      includeHidden: false
      ignorePrefixes: []              # e.g. ["_", "ref"]
      include: []                     # names or paths; empty: by the rules above
      exclude: []
    pixelsPerUnit: project            # S41's project value, or a number
    pivot: {source: slice, slice: "", preset: bottom_center, custom: [0.5, 0.0]}
                                      # source: preset | custom | slice ("" = 'pivot', else first with a pivot,
                                      # else preset)
    border: {slice: ""}               # "" = the first 9-patch slice; "none" disables
    trim: true
    clips: {generate: true, tags: [], events: {layer: events}}   # tags: [] = all
    texture: {layout: packed, padding: 1, extrude: 1, maxSize: 2048, compression: none}
                                      # compression: none | project (S14's sprite rules)
    palette: {asset: true}            # expose palette#0 to S43
```
MCP uses the same keys in snake_case (`import_mode`, `layers.include_hidden`, `pixels_per_unit`).
Defaults are chosen for pixel art: trimmed, lossless, nearest, characters pivoted at the feet.

### 6.5 Outputs, keys, cache
- **Frame names and keys.** `sprite_sheet`: frame *i* is named `<file stem> <i>` for display
  (`hero 0`), key `frame#<i>`; `layers_as_sprites`: `<layer path>/<i>`, key `frame:<layer path>/<i>`.
  Frame keys never contain the file name, so a rename keeps every GUID. Tag clips:
  `animation:<tag>` (index form for duplicate names), the untagged clip `animation#0`; slices
  `slice:<name>`; pages `image#<n>`; tilesets `tileset:<name>`; the palette `palette#0`.
- **GUIDs.** `Guid::derive(fileGuid, key)` (S04 §6.4). Only *named* entries (`animation:`,
  `slice:`, `tileset:`) are persisted in the meta's `subAssets:`; index-keyed ones are always
  derived and never written (no meta churn on a 500-frame file). S04's matching then keeps a
  renamed tag's GUID (unmatched old ↔ unmatched new of the same kind at the same index), so an
  Animation Player entry that names the clip by GUID survives a tag rename in Aseprite; its path
  hint is rewritten.
- **Reference strings.** Sheet: the file path (`sprites/hero.aseprite`). Clips:
  `sprites/hero.aseprite#walk` in the Animation Player (`clip:`) with the sub-asset `guid:` (as
  `.anim` entries have, `docs/animation-clips.md` "In a scene"). `ModelSubClips`'s reference split
  accepts `.aseprite`/`.ase` as containers.
- **Cache.** `.jasperengine/cache/imported/v1/<k[0:2]>/<k>/`, `k` = SHA-256 of (file content hash,
  resolved settings, the S41 PPU, importer version, packer version): `sheet.json` (the §6.11
  record, with tags, slices and user data), `page<n>.png`, `clip-<n>.anim` (the
  canonical `.anim` text), `import.json` (warnings, timings, the sub-asset table). Deleting the
  cache only costs a reimport. The editor's `ImportedAssets` provider (R47) answers
  `"<file>#sheet"`, `"<file>#animation:<tag>"` and `"<file>#image#<n>"` from it; exports copy the
  same files under `assets/je-imported/` with the `index.json` of R47; `AnimationClipPool` resolves an
  `.aseprite` sub-clip through `ImportedAssets` and `anim::parseClip`, never through the codec.

### 6.6 Clip generation (exact)
- Durations are milliseconds; key *j* sits at `Σ durations before j / 1000` s, on the 1e-6 grid.
  `frame_rate` = `1000 / gcd(durations)` clamped to [1, 240]; `length` = the sum.
- Forward: from..to. Reverse: to..from. Ping-pong: from..to then to−1..from+1. Ping-pong
  reverse: to..from then from+1..to−1. One-frame tags have one key.
- Repeat 0 (Aseprite: infinite in the UI) → the sequence once, `wrap: loop`. Repeat N ≥ 1 → the
  sequence N times (for ping-pong, N counts directions as Aseprite does: 1 = one direction only,
  2 = there and back), `wrap: once`, holding the last frame.
- `source: {kind: "aseprite", path, name: <tag>}`; tracks sorted as `.anim` requires; the text is
  written by `anim::writeClip` so it is canonical.

### 6.7 Flatten and layer rules (exact)
Selected layers = (all image/tilemap layers whose ancestors are all selected) minus reference
layers, minus hidden ones unless `includeHidden`, minus `ignorePrefixes` matches (a group match
removes its subtree), then `include` adds and `exclude` removes by name or path. Composition is
R8 restricted to them. Trim uses alpha 0 only, after composition, per frame; `layers_as_sprites`
trims each layer's frame separately with offsets relative to the canvas.

### 6.8 Finding Aseprite
Order: the preference `externalTools.aseprite.path` if set and executable; `JASPER_ASEPRITE`;
then per OS:
- Windows: `%ProgramFiles%\Aseprite\Aseprite.exe`, `%ProgramFiles(x86)%\Aseprite\Aseprite.exe`,
  `%LOCALAPPDATA%\Programs\Aseprite\Aseprite.exe`, every Steam library in
  `steamapps\libraryfolders.vdf` + `steamapps\common\Aseprite\Aseprite.exe`, then `PATH`.
- macOS: `/Applications/Aseprite.app/Contents/MacOS/aseprite`, `~/Applications/…`, Steam
  `~/Library/Application Support/Steam/steamapps/common/Aseprite/Aseprite.app/Contents/MacOS/aseprite`.
- Linux: `PATH` (`aseprite`), `~/.steam/steam/steamapps/common/Aseprite/aseprite`,
  `~/.local/share/Steam/steamapps/common/Aseprite/aseprite`, `/opt/aseprite/aseprite`.
Detection is off the main thread, cached per session, and confirmed by `--version` (timeout 3 s);
launching passes the absolute path only, detached (no shell, arguments as an array).

### 6.9 Errors
Kinds: `not_aseprite` (bad magic), `truncated`, `corrupt_chunk` (`field` = chunk type and offset),
`too_large`, `unsupported_depth`, `unknown_layer`, `unknown_tag`, `unknown_slice`, `invalid_settings`
(`field` = the settings key), `read_only_clip`, `aseprite_not_found`, `import_failed`. Each has a
`fix` (for example `unknown_layer`: "layers are: Body, Body/Arm, Shadow"). The sheet runtime adds
`unknown_frame` (with the valid names, first 20) and `not_a_sheet` (S45 adds its atlas kinds).

### 6.10 Packing (exact; moved from S45 §6.6, the contract of `SpritePacker`)
1. Collect frames: members in path order; each member's frames in sheet order; trim (alpha 0) unless
   disabled or bordered; duplicates (same size, same pixels) share the first frame's rect.
2. Each frame occupies `trimmed size + 2·extrude`, plus `padding` to its right and bottom; pages
   keep `padding` from their top and left edges. With a block format, x, y and size round up to 4.
3. Members are packed in order of their largest frame (max side, then area, then path, descending);
   inside a member, frames by (max side, area, name). MaxRects Best Short Side Fit (Jylänki's
   `MaxRectsBinPack`, public domain, ported with its header and a `NOTICE.md` entry; rotation
   removed, ties by (y, x)). Page 0 first; with Keep Members Together a member that does not fit
   wholly in the current page rolls back and goes to the next page that holds it, starting a new page
   when none does.
4. Each page shrinks to its used bounds (then POT/square/4 rounding); extrusion pixels are written
   last.
5. The layout is a pure function of (frame sizes in order, settings, max size); callers cache it with
   that key. The same input on any machine gives the same layout and page bytes (sorted input,
   ordered tie-breaks, no hash-map iteration order, no threads in the layout).

### 6.11 The sheet record (`jasperengine.spritesheet` v1; moved from S45 §6.3)
```json
{
  "format": "jasperengine.spritesheet",
  "version": 1,
  "source": "sprites/hero.aseprite",
  "source_guid": "4b1e9c0d2a7f4e31b6c58d0f9e2a7c13",
  "pixels_per_unit": 16,
  "pixel_ratio": [1, 1],
  "pages": [{"texture": "je-imported/3f/3fa1.../page0.png", "size": [128, 64]}],
  "frames": [
    {"name": "hero 0", "key": "frame#0", "page": 0, "rect": [1, 1, 14, 22], "source_size": [16, 24],
     "offset": [1, 2], "pivot": [8, 24], "border": [0, 0, 0, 0], "duration_ms": 100}
  ],
  "tags": [], "slices": [], "user_data": {}
}
```
Coordinates are pixels, origin top-left, y down (Aseprite's and PNG's); the runtime converts once
per frame. `tags`, `slices` and `user_data` are filled from Aseprite files (R16–R18); other producers
(S45's texture sheets and atlases) leave them empty. Writing: keys in this order, frames in sheet
order, integers only. A frame name is unique in its sheet.
```cpp
// engine/core/sprite/SpriteSheet.h
struct SpriteFrame { std::string name; uint16_t page; Rect rect; Vector2 sourceSize, offset, pivot;
                     Vector4 border; uint32_t durationMs; };
struct SpritePage { std::string texture; int width, height; };
struct SpriteSheetData { std::string source; Guid sourceGuid; float pixelsPerUnit; Vector2 pixelRatio;
                         std::vector<SpritePage> pages; std::vector<SpriteFrame> frames;
                         std::vector<SpriteTag> tags; std::vector<SpriteSlice> slices; nlohmann::json userData;
                         int findFrame(std::string_view name) const; };   // -1 when absent
class JASPERENGINE_API SpriteSheetPool {
public:
    static std::shared_ptr<const SpriteSheetData> get(const std::string& path);   // R46; null + log on failure
    static uint64_t generation(const std::string& path);                          // bumped on reload
    static void invalidate(const std::string& path);                              // editor reloads, atlas builds
    using Loader = std::function<std::shared_ptr<const SpriteSheetData>(const std::string& path)>;
    static void registerSource(std::function<bool(const std::string& path)> accepts, Loader loader); // S45's sources
};
// engine/core/asset/ImportedAssets.h: exports read assets/je-imported/index.json; the editor
// installs a provider answering from .jasperengine/cache/imported/v1
class JASPERENGINE_API ImportedAssets {
public:
    static constexpr const char* kImportedDir = "je-imported";
    static bool find(const std::string& sourceRef, std::string& fileOut);   // "sprites/hero.aseprite#sheet"
    using Provider = std::function<bool(const std::string&, std::string&)>;
    static void setProvider(Provider);
};
```
Page textures are cooked by S14 under their `je-imported/` path. The record and the `ImportedAssets`
index are versioned, and so are their cache directories (`v1`), so a format change reimports.

### 6.12 Scene and runtime fields (the sheet part; moved from S45 §6.7)
```cpp
enum class PivotPreset { CENTER, TOP_CENTER, BOTTOM_CENTER, LEFT_CENTER, RIGHT_CENTER,
                         TOP_LEFT, BOTTOM_LEFT, TOP_RIGHT, BOTTOM_RIGHT, FRAME };      // FRAME appended
struct SpriteComponent {
    // ...existing fields (S45 changes framesRect to a growable list and adds drawMode)...
    AssetRef sheet;                  // "" = legacy: the mesh texture and inline frames
    int frame = 0;
    std::string frameName;
    // runtime, not saved
    std::shared_ptr<const SpriteSheetData> sheetData; uint64_t sheetGeneration = 0;
    int builtFrame = -1;             // the frame whose geometry the buffer holds
};
```
```yaml
sprite:
  enabled: true
  sheet: sprites/hero.aseprite
  sheetGuid: 4b1e9c0d2a7f4e31b6c58d0f9e2a7c13
  frame: 3
  frameName: hero 3
  width: 0
  height: 0
  pivotPreset: frame               # the enum table's file name (editor/reflect/Enums.cpp:581 style)
```
`sheet`, `sheetGuid` (S04's scalar `<key>Guid` rule), `frame`, `frameName` are written only for
sheet sprites (`encodeSpriteComponent`/`decodeSpriteComponent`, `editor/StreamComponents.cpp:337-395`).
The generated scene code (`Factory.cpp`) emits the same fields; `RuntimeFields.gen.inc` is
regenerated; the cooked scene format carries them through the generated field tables.

## 7. Acceptance criteria

| AC | Verification |
|---|---|
| AC1 (R1, R3, R8) | `editor/tests/pixelart/AsepriteCodecTest.cpp` `ReadsTentenFixtures`: the 8 Overbuffed files decode (RGBA, compressed cels, old palette 0x0004, sRGB profile, 3 frames of 125 ms each, verified with a chunk scan on 2026-11-04); `buff` and `spiker` render to the committed Aseprite exports `buff@22x22.png` / `spiker@16x16.png` with zero differing pixels (alpha and, where alpha > 0, RGB). Red control: the same test with cel offsets ignored fails. |
| AC2 (R1–R4, R8) | `AsepriteCodecTest.GoldenCorpus`: every file of the corpus (§9 fixture list) renders each frame to its Aseprite `--sheet` export (and `--split-layers` exports) pixel for pixel; `describe` equals Aseprite's `--data` JSON for frames, durations, tags (direction, repeat), slices (keys, centre, pivot) and layer names/opacity/blend. |
| AC3 (R8) | `AsepriteBlendTest.AllModes`: the 19 modes × opacity {0, 1, 127, 254, 255} × group/no group on the `blend_modes` fixture equal Aseprite's export exactly; the classic variants are tested against hand-computed `blend_funcs` values. |
| AC4 (R7) | `AsepriteCodecTest.ResaveIsIdentical`: `write(read(f)) == f` byte for byte for every corpus file; `EditTouchesOnlyItsChunks`: changing one pixel of one cel changes only that cel chunk, its frame header and the header size (diff of chunk lists). |
| AC5 (R7, R43) | `AsepriteCodecTest.WriteRoundTrip`: documents built in code (each colour mode, groups, every blend, linked cels, tilemap with flips, user data with all 19 property types and 3-level nesting, unknown chunk) survive `read(write(d))` field for field. Manual compat gate `tools/pixelart/aseprite_compat.py` (needs `JASPER_ASEPRITE`): Aseprite 1.3 opens each written file, `--save-as` and `--sheet` match ours; its run is recorded in `docs/aseprite.md` per release. |
| AC6 (R6) | `AsepriteFuzzTest.Mutations`: 20,000 seeded byte mutations, truncations and size-field inflations of the corpus under ASan/UBSan: no crash, no leak, allocations bounded by R6's limits, every failure a typed error. |
| AC7 (R10) | `editor/tests/pixelart/PixelImageTest.cpp`: index-plane invariant kept by crop/blit/flip, dropped by `set` (return value says so), `hash` stable across runs, PNG round trip lossless. |
| AC8 (R11, R20, R40) | `editor/tests/pixelart/AsepriteImportTest.cpp` `GuidsAreStable`: a fixture project's `.aseprite` gets a meta; reimport, rename of the file, an added frame and a renamed tag keep the GUIDs of the frames, clips (tag rename: same GUID, new path hint), slices; `AssetType.h` round-trips `aseprite_sprite` through `assetTypeFromName`. |
| AC9 (R13–R15, §6.7) | `AsepriteImportTest.Layers`: include/exclude/prefix/hidden/reference rules on the `layers_groups` fixture give the documented frame pixels; `layers_as_sprites` children overlay to the flattened frame exactly (z-index-free fixture) and warn on the z-index fixture; trim offsets restore every frame at the canvas origin; identical frames share one rect. |
| AC10 (R16, §6.6) | `AsepriteImportTest.Clips`: the `tags_slices` fixture's four directions × repeat {0,1,3} produce the key lists and wraps of §6.6 (table in the test); event text `onStep("l", 1)` on the events layer becomes the clip event; `tools/mcp/tests/test_aseprite_import.py::test_clip_plays`: an Animation Player with `hero.aseprite#walk` in play mode shows frames at the expected times (`get_runtime_entity` sampling under `fixed_fps 60`). |
| AC11 (R17) | `AsepriteImportTest.Slices`: pivot from slice, border from the 9-patch slice (untrimmed frame), per-frame keys and hidden keys; `test_aseprite_import.py::test_slice_collider`: a Rigidbody 2D with a collider `slice: hurt` changes its box on the frames whose key changes (Box2D AABB read back), and is disabled where the slice is hidden; a collider without `slice` allocates nothing (counter). |
| AC12 (R18, R33) | `test_aseprite_import.py::test_drop`: dropping (`create_entity` from the asset, the drop path) makes the Sprite, the Animation Player with Default Clip `idle`, applies `light2d.intensity` from user data, all one undo step (`undo` removes all). |
| AC13 (R21, R24, R25) | `test_aseprite_import.py::test_export`: a web and a Linux export of a project using one file contain `assets/je-imported/…` and no `.aseprite`; the exported Linux player shows the sprite's frame 3 pixel-exactly (`tools/smoke-test.sh` screenshot vs the `render` CLI output); a project without `.aseprite` files creates no cache dir and starts no job (counter). |
| AC14 (R22, R23, R28) | `tools/mcp/tests/test_aseprite_hot_reload.py`: overwrite the file outside the editor (atomic and in-place writes, and a half-written file then the full one): the Scene view frame pixel changes (`sample_view_color`), the Console line appears once, a half-written file keeps the old pixels; latency from write to new pixel ≤ 300 ms p95 over 20 runs (native watcher, CI runner). An S47-style in-editor write (`WriteFilesCmd`) reimports too. |
| AC15 (R26, R27) | `editor/tests/pixelart/AsepriteBench.cpp` (`--gtest_filter=AsepriteBench*`): the R26/R27 numbers on the CI runner with the `big` and `hero64` fixtures, recorded in this spec's Deviations if beaten or missed. |
| AC16 (R30, R31, R32) | `tools/mcp/tests/test_sprite_preview_editor.py` (xvfb, isolated editor, `tools/editor-shot.sh`): the importer inspector and the Sprite Preview with onion skin, slices, the 1× compare warning; screenshots in the evidence. `AsepriteLocatorTest`: fake install trees per OS layout (temp dirs, injected env) are found in the documented order; a missing tool gives `aseprite_not_found`. |
| AC17 (R35, R36, §6.1) | `tools/mcp/tests/test_aseprite_import.py`: `set_sprite_import_settings` preview changes nothing on disk (file hashes), `output: true` writes the meta, reimports and one `undo` restores the meta and the previous outputs; typed errors for an unknown layer and a bad key; `get_asset_info`/`view_asset` on a file; `edit_animation` on `#walk` gives `read_only_clip`; the `sprites` group is hidden until `tool_groups` enables it; `test_tool_budget.py`, `test_tool_conventions.py`, `test_tool_coverage.py`, `gen_tool_docs.py --check`. |
| AC18 (R37) | `tools/mcp/tests/test_aseprite_cli.py`: `aseprite info --json` equals `describe`; `render --tag walk` equals the golden strip; `resave --expect-identical` exits 0 on the corpus and 4 on a file with a re-encoded cel; usage errors exit 2. |
| AC20 (R45, R47) | `engine/tests/core/sprite/SpriteSheetIOTest.cpp` (moved from S45 AC1): canonical `writeSheet(parseSheet(t)) == t`; bad frames dropped with JSON-path warnings; a newer version refused; a 2,000-frame parse ≤ 5 ms desktop, ≤ 20 ms web (`SpriteSheetBench`); `ImportedAssetsTest`: the editor provider and an export's `index.json` resolve the same keys. |
| AC21 (R48, R49) | `engine/tests/core/sprite/SpriteFrameGeometryTest.cpp` (moved from S45 AC2): trimmed and untrimmed versions of the same frame give the same covered pixel area at the same position for all 10 pivots and both flips; GPU golden `SpriteSheetRender.TrimmedEqualsUntrimmed` (glcore and Vulkan): 12 sprites drawn from a trimmed `.aseprite` sheet page and from untrimmed PNGs of the same frames are pixel-identical. Red control: offsets ignored → the golden fails. |
| AC22 (R52, §6.10) | `editor/tests/sprite/SpritePackerTest.cpp` (moved from S45 AC4): no overlap, inside page, padding and extrusion exact, multiple pages, keep-together, POT/square/block rounding, oversize error; the same input packed twice and on a shuffled member list gives byte-identical layout and pages; the reference corpus (this spec's fixtures plus 500 generated frames of seeded sizes) ≥ 85 % usage with trim; layout of 2,000 frames into 2048² pages ≤ 100 ms (`SpritePackerBench`). |
| AC23 (R44, §6.12) | `editor/tests/stream/SpriteSheetMigrationTest.cpp` (the migration test; moved from S45 AC8): committed fixtures at the scene and prefab versions before S44's bump, with inline sprites (frames, Sprite Animations, a `sprite.frame` clip), load, save at the new versions with byte-identical sprite sections apart from the version, and render the same frames (golden); a sheet sprite round-trips `sheet`, `sheetGuid`, `frame`, `frameName`; generated scene code compiles and shows the same frame (`tools/smoke-test.sh` on the fixture). The test reads the versions from `StreamMigrations`, so it holds whichever number S44 takes at landing. |
| AC24 (R50, R51) | `tools/mcp/tests/test_aseprite_import.py::test_clip_on_sheet` (moved from S45 AC12): a clip keying `sprite.frame` 0→5 on a sheet sprite shows the frames by index (sampled in play); `get_animation` bindings warn when the clip's `source.path` is another sheet; a script loads `SpriteSheet.load("sprites/hero.aseprite")`, logs `frameCount`, `findFrame("hero 3")` and `getFrame(3).rect`; `tools/lua_api/tests` list the members. |
| AC19 (R38) | `test_aseprite_import.py::test_lua`: a script plays `walk` with `playTag`, reads `tag`, `getTag("walk").repeat`, `getSlice("hurt").rect` per frame and logs them; the Console matches the expected values; `tools/lua_api/tests` find the new members; the docs' Lua blocks run. |

## 8. Dependencies & conflicts

- **Depends on.** S04 (sub-assets, deterministic GUIDs, meta sections), S05 (watcher, reload
  kinds, self-write suppression), S06 (Background jobs), S14 (cooks; S44's pages are lossless by its
  own default, R15, so S42 is not needed), S24 (`WriteFilesCmd`, undo), S31 (groups, budget,
  `call`). Nothing from S45: the sheet runtime it needed is now this spec's (R45–R52). S41 (same
  wave) for PPU (until it lands, 1 pixel = 1 unit as today).
- **Blocks.** S45 (wave 5B: texture sheets and atlases on the record, pool, packer and frame
  references), S46 and S47 (codec, `PixelImage`, `describe`, `PixelProblem`, `LintTypes`), S48 and
  S49 (`LintTypes`), S50 (tilesets/tilemap cels), S43 (palette entries, same wave), S52 (same wave:
  sheets with pages through `SpriteSheetPool`; until S44 lands S52 draws textures and rects).
- **Interfaces given to siblings.** `PixelImage`; `PixelProblem`, `Verdict`, `lint::LintResult`
  and `LintReport`; `SpriteSheetData`/`SpriteSheetPool` (with `registerSource`), `ImportedAssets`,
  `SpritePacker::pack(frames, settings) -> layout`, `SpriteFrameGeometry`, the sheet record format,
  `PivotPreset::FRAME`, `SpriteComponent::{sheet, frame, frameName}`;
  `aseprite::read/write/renderFrame/describe`;
  `AsepriteImporter::reimport(path)` and `outputs(path)` (sheet, clip refs, GUIDs);
  `AsepriteImport::tilesets()/tilemapCels()`; the `palette#0` sub-asset; `SpriteTools.cpp` as the
  `sprites` module.
- **Family decisions recorded in the ledger:** S44 lands before S45 and owns the sheet runtime;
  the `AssetType` values are reserved (Palette 24, AsepriteSprite 25, SpriteAtlas 26, Tileset 27);
  `SubAssetKind` appends in landing order (S44: Frame, Slice, Tileset, Palette; others after).
- **Merge hotspots.** `engine/core/asset/AssetType.h`, `SubAssetKeys.h`, `editor/asset/AssetTypes.cpp`
  (S43, S45, S50 rows), `SpriteComponent.h`, `MeshSystem.cpp` (sprite creation and frame rects; S41,
  S45 and S52 also touch them), `PropertyBindings.cpp`, `StreamComponents.cpp`,
  `StreamMigrations.cpp` (the version bump), `Factory.cpp`, `RuntimeFields.gen.inc`,
  `editor/reflect/Enums.cpp` (`PivotPreset`), `WorldTools.cpp` (`set_sprite_frames`, S45 too),
  `editor/reload/handlers/` (a new `Aseprite.cpp` + `kindOf`),
  `ModelSubClips`/`AnimationClipPool` (sub-clip containers), `Rigidbody2DComponent.h` and its
  stream code (`Collider2D::slice`), `editor/window/SceneWindow.cpp` drop handling,
  `McpToolGroups.cpp`, `McpInstructions.cpp`, `NOTICE.md`, `docs/mcp/guide/sprites.md` (S45, S47).

## 9. Risks & defaults

- **Exactness vs Aseprite.** Aseprite's "new blending" preference changes group and opacity
  results. Default: we port the `_n` variants (1.3's default) and the goldens are made with
  default preferences; `render` takes `--classic-blend` for old files. The corpus README records
  the Aseprite version; a mismatch on a newer Aseprite is a bug against the port, not a tolerance.
- **Fixture licences.** tenten's eight Overbuffed files (and the two PNG exports) are in a repo
  whose `LICENSE` is MIT (© 2026 debuff) with no separate art licence: treated as MIT, copied with
  that LICENSE beside them and a `NOTICE.md` entry. They cover only RGBA, one layer, three frames,
  no tags or slices, so the **new corpus** is authored by the team in Aseprite (MIT, in
  `editor/tests/data/aseprite/` with a README per file): `modes_indexed` (transparent index 3,
  named palette), `modes_gray`, `layers_groups` (nested groups, hidden, reference, background),
  `blend_modes` (19 modes × opacities, in groups and not), `cels` (links, cel opacity, z-index ±,
  off-canvas cels), `tags_slices` (4 directions × repeats, overlapping tags, user data of every kind
  and property type, 9-patch and pivot slices with per-frame and hidden keys), `tilemap` (embedded
  tileset, flips), `profile_icc`, `pixel_ratio_2x1`, `old_speed` (synthesized by our writer: no
  modern Aseprite writes it), `hero64` and `big` (512², 120 frames, 12 layers). Goldens come from
  `tools/pixelart/make_aseprite_goldens.py` (run by a person with Aseprite; it records the version
  and every CLI line: `-b --sheet … --sheet-type horizontal --data … --format json-array
  --list-layers --list-tags --list-slices`, and `--split-layers`). Default if nobody with Aseprite
  is available at M2: the codec lands on the tenten files plus writer-synthesized fixtures, and the
  golden corpus is a precondition of M3's merge.
- **Double-click opens Aseprite** (Unity opens the external editor). Default kept; the preference
  **Double-click .aseprite** offers Sprite Preview instead.
- **Tag repeat semantics** follow the spec text; Aseprite's UI plays repeat 0 forever but exports
  it once. Default: loop (what a game wants for idle/walk); a tag user property `loop: false`
  forces `once`.
- **Frame-name display includes the file stem** (`hero 3`) while keys do not: the sprite resolves a
  stored frame by name, then by index, so a file rename still shows the right frame.
- **Colour profiles.** Pixels are imported as sRGB values; an embedded ICC profile is kept on
  re-save and reported as a warning (`ICC profile ignored: pixels used as sRGB`), never applied.
- **Pixel ratio** (non-square pixels): applied to the quad width through the sheet's
  `pixel_ratio`; a warning recommends square pixels for S41.
- **Huge files** (> 1 GiB decoded) are refused with `too_large`; the importer never pages.

## Milestones

- **M1** `PixelImage`, `PixelProblem.h`, `lint/LintTypes.h`, reader, writer with provenance,
  describe, CLI `info`/`resave` (R1–R7, R9, R10, R37 partly); AC1, AC4–AC7, AC18 partly. Unblocks
  S46/S47 and the lint users S48/S49.
- **M2** Composition and blenders, the golden corpus and its script, CLI `render` (R8, §9);
  AC2, AC3, AC18.
- **M2b** The sheet runtime (moved from S45 M1): record and IO, `SpriteSheetPool`, `ImportedAssets`,
  the Sprite's frame references, trimmed geometry and From Frame pivot, the clip binding, the packer,
  serialization with the version bump and its migration, generated code, Lua `SpriteSheet`
  (R44–R52); AC20–AC24.
- **M3** Asset type, settings, importer job and cache, frames, pages, clips, slices, user data,
  hand-offs, GUIDs, export (R11–R21, R24, R25, R40–R43); AC8–AC13.
- **M4** Hot reload and editor writes (R22, R23, R28); AC14, AC15.
- **M5** Importer inspector, Sprite Preview, Open in Aseprite, drag and drop, feedback (R30–R34);
  AC12, AC16.
- **M6** MCP tool and extensions, Lua, guide and docs (`docs/aseprite.md`), budgets (R35, R36,
  R38, R39); AC17, AC19.

## Deviations

Recorded by the implementation (`feature/s44-aseprite-import`):

- **Old palette chunks.** The 6-bit palette chunk is `0x000B` (Aseprite's decoder and its
  `FLI_COLOR`); §4/R1 call it `0x0011`. Both are read. Its 6-bit values are widened by bit
  replication (`(v << 2) | (v >> 4)`), as Aseprite does, not by `v × 255 / 63` (10 values differ by 1).
- **Hue and Saturation.** The default is Aseprite's current (1.3.15+) `set_sat`; the asefile
  goldens and `blend_saturation_bug` predate that fix, so `RenderOptions::legacyHsl` (CLI
  `--legacy-hsl`) reproduces 1.3.12 and earlier, and those goldens are checked with it.
- **Groups.** `RenderOptions::groups` (`FromFile`, `Flatten`, `Compose`): Aseprite's own CLI
  `--sheet` export never composes group opacity or blend, so goldens made with it use `Flatten`;
  the importer uses `FromFile` (what the artist saw).
- **Guide.** No `## Pixel art` digest section: the upstream MCP rules (merged into this branch)
  give an area one "Other areas" entry; S44's is "sprites and .aseprite files (set_sprite_frames
  sheet): jasper://docs/sprites", with eval case `bad_aseprite_by_hand`.
- **AC12's `create_entity` from the asset** is `set_sprite_frames {create, sheet}` (the drop's
  code, `AsepriteSpriteSetup`), so no new `create_entity` parameter; it names the GameObject from
  `create.name` and gives no layer children (the drop does both). The drop itself is tested by
  dragging in the editor (`test_aseprite_import.py::test_drop`).
- **`playTag`** is stepped by `MeshSystem` on the Sprite's own runtime fields with the sheet's
  durations instead of a Sprite Animation action entity: a Lua `Sprite` wrapper owns its action's
  entity, so a tag would stop with the wrapper. Behaviour is R38's (direction, repeat, speed, no
  128-frame limit, no clip events).
- **Pixels per unit.** S41 is not on main: `pixelsPerUnit: project` resolves to 1 (R11).
- **Colours.** No `Rgba8`/OKLab types (S43's): `PixelImage` uses `PixelRgba` (`std::array<uint8_t, 4>`);
  overloads for S43's `Rgba8` are left to whichever lands second.
- **Format numbers.** Scene 7 → 8 and prefab 8 → 9 (identity migrations, fixture
  `editor/tests/fixtures/migrations/scene-v7-sprites.scene`, `SpriteSheetMigrationTest`).
- **Compression `project`.** Pages are added to the export's S14 texture cook keyed by their
  `je-imported/` path (sprite type); `none` ships the PNG pages. The editor draws the pages as
  imported in both cases: its view cook (S14 R8) skips what the editor derives into `.jasperengine/`,
  so a `none` sheet is never shown block-compressed.
- **Real Aseprite goldens.** §9's corpus is authored with Aseprite 1.3.18
  (`tools/pixelart/make_golden_corpus.lua`/`.py`, `editor/tests/fixtures/aseprite/s44-corpus/`) and
  every fixture has Aseprite's `--data` JSON; `AsepriteParityTest` checks the frames (flattened and
  composed), the indexed + background file and `describe` against them; the manual gate is
  `tools/pixelart/aseprite_compat.py`. A batch save never sets the "groups are composed" header flag
  and has no Lua API for slice keys after frame 0: the script patches both and Aseprite re-saves.
  Aseprite's `--sheet` clears an indexed background's transparent index while `--save-as` keeps it
  opaque; the importer follows `--save-as` (a background layer is opaque by definition).
- **Cache key.** It holds the file's asset path and GUID besides §6.5's content, settings and
  versions: the outputs name the file (frame names, the sheet's source, the clips' `source.path`),
  so a copy or a rename is its own import (one reimport after a rename).
- **Palettes (R6).** The palettes a file keeps (frame 0's and one copy per change) are limited to
  64 MiB, besides the 1 GiB of pixels: a file of many changes to a 65,536-entry palette is
  `too_large` instead of an allocation of gigabytes.
- **Tag repeats.** A repeat count above Aseprite's 16-bit 65,535 is clamped (with a warning), and
  a playback unrolls at most 2^20 frames (fewer passes beyond that).
- **Sprite Preview.** Compare at 1x draws both frames at the preview's integer zoom (S41's Game
  view scale is not on main).
- **Budgets (AC15, AC20).** Measured on the shared 8-core build host under load (budgets printed,
  enforced only by the benchmark run's `perfSlack()`), with `AsepriteBench` documents built in
  code at R26's sizes (2.67 MB file, not 1.5 MB): read 238 ms (budget 40), one 256² 8-layer
  composition 2.8 ms (1), write unchanged 3.4 ms (5), write with every cel changed 639 ms (120:
  deflating 128 MB of cels at level 6 on one thread), import of a 64×64 24-frame file 2.4 ms (50);
  a 2,000-frame sheet parse 9.8 ms (5). The read, composition and full-write budgets are missed
  here; a quiet-machine benchmark run should confirm before tuning.
- **`--data` names.** Aseprite's `--data` calls the four HSL modes `hsl_hue`, `hsl_saturation`,
  `hsl_color` and `hsl_luminosity`; `describe` keeps §6.3's `hue` ... (the parity test maps them),
  and omits a slice key's centre when the key has none (Aseprite's hidden keys).
- **Late import problems (R36).** An import runs as a job, so its problems are not known when the
  call that synced the file returns: they are reported by the next call's `external_changes`
  (`problems`, with an empty `changed`). An edit of a `.aseprite.meta` on disk reimports its file.
- **AC23's render.** The before/after comparison is `tools/mcp/tests/test_sprite_migration_render.py`
  (the v7 scene drawn in the Scene view, saved at the new version, reopened and drawn again, pixel
  for pixel), not a committed PNG golden: a golden would pin the GPU's rasterisation, not the
  migration. The v8 prefab is checked on the YAML side (`SpriteSheetMigrationTest`).
- **Load report.** R48's "the sheet is missing, the inline frames are used" is a new
  `LoadIssueKind::Fallback` (`fallback`), a warning; the field keeps its value.
