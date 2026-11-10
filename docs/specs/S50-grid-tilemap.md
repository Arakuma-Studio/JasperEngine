# S50 — Grid Tilemap: Grid and Tilemap Layers, Tileset assets, autotiling, tile colliders, Tile Palette and brushes, Tiled/LDtk import

Pixel-art family (ledger `PIXEL-ART-LEDGER.md`), wave 5C: every family spec it uses has landed
before it. Depends on S52 (sprite instancing: chunks draw as static instance batches), S41 (pixel
snap, shared-edge snapping) and S44 (Aseprite tileset and tilemap chunks, `SpriteSheetPool`), all
wave 5A. Uses S45 (texture sheets and atlases, wave 5B), S42 (wave 5B: Tilemap Layers join
`check_pixel_art`'s input), S04 (GUIDs), S05 (file watcher), S19 (NavGrid
from a tilemap), S24 (undo), S25 (shortcuts), S26 (picking) and S30 (cooked scenes). Read `README.md`
and the ledger first.

## 1. Goal

JasperEngine's tilemap is a list of at most 2,048 free-placed quads with at most 200 tile rects,
clamped to positive coordinates and painted cell by cell. Every modern engine has a real grid tilemap
instead: Unity's Grid + Tilemap + Tile Palette, Godot's TileMapLayer + TileSet. S50 gives JasperEngine
one: a Grid with rectangular, isometric and hexagonal layouts, any number of Tilemap Layers with sort
order, a `.tileset` asset made from a texture, an S45 sheet or atlas, or an Aseprite tileset, per-tile collision,
properties, probability and animation, per-cell flips, terrain autotiling (dual grid, peering bits,
47-tile blob), chunked instanced rendering with no tile limit and no seams, merged Box2D colliders, a
Tile Palette with Scene-view brushes and undo, Tiled and LDtk import, a Lua API, and MCP tools that let
an agent paint terrain and read a map region back as text or an image.

## 2. Scope

In scope:
- Runtime (`engine/core/tilemap/`): `GridComponent`, `TilemapLayerComponent`, `TilemapCollider2DComponent`;
  the cell store (sparse chunks, unbounded, negative coordinates); layout maths (rectangle, isometric,
  isometric staggered, hexagon pointy-top and flat-top); the `Tileset` asset and its loader; terrain
  autotiling; animated tiles; chunk building into S52 static batches; tile collider generation;
  serialization (scene YAML, Compile codegen, cooked scenes); the Lua API; NavGrid integration.
- Editor (`editor/tilemap/`): the Tile Palette window, the Tileset Editor window, a minimal Scene-view
  tool framework (`editor/scenetool/`) and the tile brushes (paint, box, line, flood, eraser, picker,
  random, terrain, select/copy), the Grid and layer Inspectors, `TilemapCellsCmd` undo, the
  legacy-to-grid conversion, the Tiled and LDtk importers, the Aseprite tilemap import.
- MCP: `edit_tilemap` and `get_tilemap` extended (core, where they are), `edit_tileset` and
  `import_tilemap` (new group `tilemaps`); `docs/mcp/guide/tilemaps.md`; user docs `docs/tilemaps.md`.

Out of scope:
- Scriptable tiles (Unity's RuleTile/TileBase subclasses). Terrain sets and the random brush cover the
  common cases; a Lua tile rule is a later spec.
- Per-cell colour and per-cell transform matrices (Unity's `SetColor`/`SetTransformMatrix`); layers
  have a tint.
- Tiled object layers, image layers and parallax, LDtk entities: listed in the import report, not
  imported. Writing Tiled or LDtk files.
- Moving the existing Scene-view special cases (splines, lattice, terrain brushes, fluid painting) onto
  the new tool framework (they keep working as they are).
- 3D tile meshes (Godot's GridMap), Shadow Caster 2D generated from tiles, hex 120° rotation (Tiled's
  bit 28: reported, imported unrotated).
- Navigation baking beyond S19's NavGrid source adapter (R50).

## 3. Current state (read, not built or run)

- **Component.** `TilemapComponent` (`engine/core/component/TilemapComponent.h:50-78`) holds
  `HybridArray<TileRectData, MAX_TILEMAP_TILESRECT>` rects (name, submesh, rect; `:26-30`) and
  `HybridArray<TileData, MAX_TILEMAP_TILES>` tiles (name, rect id, position, width, height; `:32-38`):
  at most 200 rects and 2,048 tiles (`engine/core/Engine.h:97-103`). The geometry is the entity's
  `MeshComponent`, one submesh per texture.
- **Mesh build.** `MeshSystem::createTilemap` (`engine/core/subsystem/MeshSystem.cpp:926-1147`) writes 4
  vertices of 16 floats per tile (`prepareTilemapMesh`, `:596-608`) into a DYNAMIC buffer with 16-bit
  indices, so at most `MAX_TILEMAP_RENDERED_TILES` 16,383 tiles render (`TilemapComponent.h:7-9`; the
  rest are dropped with an error, `MeshSystem.cpp:1001-1004`). Negative positions are clamped to 0
  (`:1012-1014`). UVs are inset by `textureScaleFactor` texels against bleeding (`:1049-1052`). Any change
  rebuilds the whole map (`createOrUpdateTilemap`, `:7078-7088`).
- **Chunks.** `buildTilemapChunks` (`MeshSystem.cpp:121-183`) groups indices into at most 16×16 chunks of
  about 64 tiles above 256 tiles (`TilemapComponent.h:11-17`); `RenderSystem::selectTilemapChunks`
  (`engine/core/subsystem/RenderSystem.cpp:12925-12950`) culls them and merges adjacent ranges; each
  visible range is one draw per submesh (`drawMesh`, `:8719-8721`); chunk world bounds are refreshed
  with the mesh (`:14975-14984`). There are no seams guarantees: positions are not snapped and the
  inset is a texel fraction.
- **Serialization.** The scene codec writes every rect and tile as YAML maps (`editor/StreamComponents.cpp:399-470`);
  the reflected fields are `RenderReflect.cpp:370-381`, the component desc displays "Tilemap"
  (`editor/reflect/components/RenderReflect.cpp:635-646`); runtime field tables in
  `engine/core/serialization/RuntimeFields.gen.inc:1577-1590`; Compile builds generate
  `Factory::createTilemapComponent` (`editor/Factory.cpp:1593`); cooked scenes use the generated tables
  and `serialization/codecs/ComponentHooks.cpp`'s hooks.
- **Lua.** `Tilemap` derives `Mesh` (`engine/core/object/Tilemap.h:11-68`): `addRect`, `addTile`,
  `removeTile`, `findTileByString`, ... (`engine/core/script/binding/GameObjectClassesLua.cpp:2116-2126`);
  `TilemapComponent` fields (`ECSClassesLua.cpp:1184-1192`). No cell access, no world↔cell conversion.
- **MCP.** `edit_tilemap` and `get_tilemap` (`editor/mcp/tools/WorldTools.cpp:5596-5642`, handlers
  `:2431` and `:2753`) are **core** tools: the World module is core (`editor/mcp/tools/McpTools.cpp:51`)
  and the group variable is cleared before them (`WorldTools.cpp:5593`). `edit_tilemap` builds rects from
  a tileset slice or explicit rects, then runs ops (`paint` ASCII grid + legend, `fill`, `erase`, `set`,
  `clear`), one undo step, rollback on a bad op, refuses more than `MAX_TILEMAP_TILES` (`:2717`).
  Its listing is grandfathered at 2,523 characters (`tools/mcp/budget.json`). `create_entity` type
  `tilemap` creates the same component (`EntityTools.cpp:205`). Test: `tools/mcp/tests/test_world.py:33-60`.
- **Editor.** The Inspector lists rects with previews (`InspectorWindow::drawTilemapComponent`,
  `editor/window/InspectorWindow.cpp:9309`) and clamps tile positions to ≥ 0 (`:2891`); a rect dragged
  into the Scene view adds one tile (`editor/window/SceneWindow.cpp:1084-1131`). There is no palette,
  no brush, no grid overlay. GameObject > 2D Object > Tilemap creates it (`HierarchyWindow.cpp:511`).
- **No Scene-view tool framework.** `SceneWindow` special-cases each editing mode in its mouse
  handlers: splines (`SplineSceneTools`, `SceneWindow.cpp:1278-1292`), lattice points (`:1300-1344`),
  terrain editing (`:1245`), fluid painting (`:1702`). S19's nav-grid brush keeps its cell ops in
  `editor/navigation/NavGridBrush.h` and its undo in `NavGridCellsCmd.h` (cell patches); terrain uses
  `TerrainMapPatchCmd` (dirty rects).
- **Physics 2D.** Colliders live in `Rigidbody2DComponent::colliders` (`engine/core/component/Rigidbody2DComponent.h:47-120`),
  types POLYGON (decomposed), CIRCLE, CAPSULE, SEGMENT, CHAIN (`:25-31`). `PhysicsSystem::createCollider2DShapes`
  (`engine/core/subsystem/PhysicsSystem.cpp:1471`) builds a CHAIN with `b2CreateChain` (`:1621-1666`).
  Box2D's chains are one-sided, counter-clockwise, at least 4 points, and "an open chain shape has NO
  COLLISION on the first and final edge; you may overlap two open chains on their first three and/or
  last three points to get smooth collision" (`engine/libs/box2d/include/box2d/types.h:390-399`).
  Points are converted at 64 points per metre (`PhysicsSystem.cpp:296`).
- **Navigation.** `NavGridComponent` can rasterize a Tilemap through a rect-name → area table
  (`engine/core/component/NavGridComponent.h`, `NavGridSource::TILEMAP`; `NavigationSystem.cpp:1265`,
  `:1329`). Its hex layout is pointy-top odd-r (`engine/core/navigation/NavGridMap.h:17`).
- **Assets.** `AssetType` is append-only; the last value is `VertexAnimation = 23`
  (`engine/core/asset/AssetType.h:15-39`); the ledger reserves `Tileset` (`.tileset`) for S50.
- **Format rule.** Adding optional components or fields does not bump the scene version
  (`editor/StreamMigrations.h:17-20`).
- **Libraries at hand.** `engine/libs/zstd`, `stb` (zlib inflate), `tinyxml2`, `json`, `box2d`; no
  polygon clipping library.

## 4. Reference (research clones, read-only; the owner allows copying with header and `NOTICE.md` entry)

| Source | What | Take | Improve |
|---|---|---|---|
| Pixelorama (MIT) `src/Classes/TileSetCustom.gd` | `:67-77` per-tile `probability` and `terrain_peering_bits` (−1 = unset), `:247-266` `pick_random_tile` (weighted), `:435` `is_valid_terrain_peering_bit_for_mode`, `:526-800` peering-bit polygons for square and iso tiles | the per-tile data model, the peering-bit regions for the editor's painter | random picks are `randf` (non-deterministic): ours hash (seed, x, y) |
| Pixelorama `src/Classes/Cels/CelTileMap.gd` | `:85-97` `Cell` (index, flip_h, flip_v, transpose); `:380-412` `autotile_with_neighbors` (queue propagation); `:414-435` mask from neighbours' centre terrains; `:437-476` best match: +10 per matching bit, −2 per mismatch, + specificity, ties weighted random; `:492-509` `autotile_filter_corners` (a corner counts only with both sides) | the scoring, the blob corner filter, the flip/transpose bits | the mask comes from a stored terrain plane, not from neighbour tiles, so a change touches only its neighbourhood (no propagation queue, D6) |
| SpriteCook (MIT) `skills/spritecook-use-dual-grid-tilesets/SKILL.md` | dual grid: each drawn tile between 4 logical cells, mask TL=1 TR=2 BL=4 BR=8, 4×4 sheet `frameByMask=[-1,15,8,9,0,11,14,7,13,4,1,10,3,2,5,6]`; two-surface sets put the counter surface at frame 12 | the template `dual_grid_4x4` verbatim | expressed as peering values, so two-surface sets are two terrains and mix with the other modes |
| cr31 "Blob" Wang tiles (behaviour, `http://www.cr31.co.uk/stagecast/wang/blob.html`) | the 47-tile blob from an 8-neighbour mask with corner reduction; the 7×7 sheet layout | template `blob_47` | — |
| Godot TileSet/TileMapLayer, Unity Tilemap (behaviour only) | Match Corners and Sides / Match Sides, alternative tiles with flips, Grid layouts, Tile Palette tools and shortcuts, Chunk vs Individual mode, Composite Collider "Outlines/Polygons" | names and semantics | — |
| spryte (MIT) `src/panels.ts:119-132` | `snapEdges`: neighbouring quads round shared edges the same way | through S41 R13 (tile edges from one edge array, rounded to the texel grid) and S41's per-vertex snap in S52's stage | — |
| Tiled map format (JSON/TMX docs) | orientations, stagger axis/index, hex side length, GID flip bits 31/30/29 (28 hex rotation), csv/base64 + zlib/gzip/zstd, infinite-map chunks, tile offsets, wang sets | the importer's mapping (D11) | — |
| LDtk JSON format | levels, `gridSize`, layer instances, `gridTiles`/`autoLayerTiles` (`px`, `src`, `f` flip bits), `intGridCsv`, external `.ldtkl` levels | the importer's mapping (D11) | — |

Clipper2 (Boost Software License) is vendored as `engine/libs/clipper2` for polygon union (D8), with its
licence and a `NOTICE.md` entry.

## 5. Requirements

### Functional — Grid, layers, cells

- **R1 Grid.** A `GridComponent` ("Grid") defines the cell layout of the Tilemap Layers on its
  GameObject and below it: `layout` (Rectangle, Isometric, Isometric Staggered, Hexagon), `cellSize`
  (world units; pixels in 2D), `cellGap`, `plane` (XY, default, or XZ for 3D boards), for staggered and
  hex layouts `staggerAxis` (X or Y) and `staggerIndex` (Odd or Even), for hex `hexSideLength`
  (default: a regular hexagon in the cell). The default hex is pointy-top odd-r, as S19's NavGrid.
- **R2 Tilemap Layer.** A `TilemapLayerComponent` ("Tilemap Layer") holds cells of one Tileset. It
  uses the nearest Grid on itself or an ancestor; without one it draws as a Rectangle grid of the
  tileset's tile size and the Inspector says so. Fields: `tileset`, `tint` (colour × opacity),
  `material` (default sprite material; `library:sprite` features apply), Sorting Layer and Order in
  Layer (S52), `mode` (Chunk or Individual, S52 R31), `chunkSize` (8, 16, 32 default, 64),
  `tileAnchor`, `animationSpeed` (1), `dualGrid` (R27), `seed` (random choices), `enabled`.
- **R3 Many layers.** A Grid has any number of child layers; their draw order is S52's sort (Sorting
  Layer, Order in Layer, hierarchy). Creating a layer from the Grid Inspector or the Tile Palette
  gives it the next Order in Layer.
- **R4 Unbounded cells.** Cells are addressed by signed 32-bit (x, y), y up (row 0 is the bottom, as
  `edit_tilemap` today), negative coordinates included. There is no tile limit; memory is the limit
  (R59).
- **R5 Cell value.** A cell is empty or holds a tile id of the layer's tileset plus three transform bits
  (flip X, flip Y, transpose; Tiled's H, V, D order: transpose applies first) and a lock bit (R26).
  Hex layouts accept flips only (transpose is refused with a typed error).
- **R6 Terrain plane.** A layer that has been painted with terrain keeps one terrain value per cell
  (none or a terrain of its tileset); a layer never painted with terrain stores none of it.
- **R7 Layout maths.** For every layout, `cellToLocal`, `localToCell` (exact: a point on a shared edge
  belongs to one cell, by a documented half-open rule), the neighbour offsets (8 for rectangle and
  isometric, 6 for hex), cell polygons (for the overlay and picking), and the painter's order (isometric
  and hex: back to front). Isometric is Unity/Tiled's diamond: local = ((x − y)·w/2, (x + y)·h/2);
  Staggered and Hexagon follow Tiled's stagger rules.
- **R8 Tile placement.** A tile image larger than the cell is anchored at the cell's bottom-left
  (rectangle) or bottom-centre (isometric, hex), plus the tile's own `offset` in pixels, as Tiled does;
  `tileAnchor` moves the anchor for the whole layer.
- **R9 Queries.** A layer answers: the cell at a world point, the used rect, cells in a rect, the
  non-empty cells, the tile data of a cell (properties, collision kind, terrain), and tile id ↔ name.
- **R10 Picking (S26).** The Scene view picks a layer where it shows a tile (the cell polygon of a
  non-empty cell, then the tile's opaque rect), not its bounding box.

### Functional — the Tileset asset

- **R11 Asset.** `Tileset` (`.tileset`, JSON, `version: 1`, GUID in `.meta`, S04) is the new asset type
  `AssetType::Tileset = 27`, the family's reserved value (ledger: Palette 24, AsepriteSprite 25,
  SpriteAtlas 26, Tileset 27), whatever order the specs land in (`assetTypeFromName`'s bound is the
  highest value present and its loop skips unnamed values). A layer refers to it by GUID.
- **R12 Sources.** A tileset has one or more sources, each a texture page set for batching (S52):
  - `texture`: a texture sliced by `tile_size`, `margin`, `spacing` (Tiled's names), with
    `skip_empty` (fully transparent tiles get no id);
  - `sheet`: frames by name from any sheet S44's `SpriteSheetPool::get` gives (an `.aseprite`, and
    S45's `.spriteatlas`, Sprite Mode Multiple texture or `name@WxH` texture); tiles follow the frames
    into atlas pages and keep working when the atlas repacks;
  - `aseprite`: an Aseprite file's tileset (S44's `AsepriteImport::tilesets()`, its tile strip as a
    `PixelImage` and per-tile user data), tile i ≥ 1 of the Aseprite tileset is our tile i − 1 of that
    source (Aseprite's tile 0 is the empty tile); Aseprite tile user data becomes properties.
- **R13 Stable ids.** Every tile has an integer id (0-based) unique in its tileset and never reused;
  re-slicing a texture keeps the ids of tiles at the same source position and appends new ones. Cells
  store ids, so editing a tileset never shifts a map.
- **R14 Per-tile data.** `name` (default `<source>_<index>`), `offset`, `probability` (default 1),
  `collision`, `animation`, `terrain` (set, centre terrain, peering values), `properties`,
  `nav_area` (S19, R50). Unset fields cost nothing in the file.
- **R15 Collision shapes.** Per tile: `none` (default), `full` (the cell rect), `alpha` (traced from
  the tile's alpha at a threshold, default 128, pixel-exact outline with collinear points merged), or
  `custom` (polygons drawn in the Tileset Editor, points on the pixel grid). Each shape has a kind:
  `solid`, `one_way` (with a direction, default up) or `sensor`. Shapes follow the cell's flip and
  transpose bits.
- **R16 Properties.** A tileset declares a property schema (name, type bool|int|float|string|color,
  default); tiles override values. Undeclared properties are refused at write time (typed error with
  the declared names as the fix).
- **R17 Animation.** A tile may list frames (another tile id of the same tileset, duration in ms).
  Cells showing it animate on the scene's game clock (paused with the game) × the layer's
  `animationSpeed`; `random_start` offsets each cell by a hash of its coordinates.
- **R18 Terrain sets.** A tileset has terrain sets; each has a `mode` (`blob`: Match Corners and
  Sides; `sides`: Match Sides; `dual_grid`: corners on the dual grid) and named, coloured terrains (at
  most 254 per tileset). A tile's peering values are, per peering direction of the mode, `any` (no
  constraint), `none` (no terrain of this set) or a terrain of the set.
- **R19 Templates.** Applying a template to a block of tiles assigns names and peering values in one
  step: `blob_47` (cr31 7×7), `blob_47_godot` (Godot's 12×4 "3×3 minimal" layout), `sides_16` (4×4),
  `dual_grid_4x4` (SpriteCook, with `two_surface` assigning frame 12 to the second terrain). Custom
  layouts are painted by hand.
- **R20 Coverage check.** For each terrain set, the masks the mode can produce that no tile matches
  exactly are listed (as masks and as small images); `allow_transforms` (off by default) lets flipped
  and transposed variants of a tile count as candidates for rectangle layouts.

### Functional — painting and autotiling

- **R21 Brush operations** (one implementation, `editor/tilemap/TilemapBrush`, shared by the Scene view
  and `edit_tilemap`): paint a stamp (one or more tiles, with transform bits), box fill, line
  (Bresenham on rectangle and isometric cells, cube lines on hex, as `NavGridBrush::line`), flood fill
  (contiguous by the layout's 4-side or 6 neighbours, or every equal cell in the used rect, bounded at
  1,000,000 cells), erase (all of the above), pick (cell → brush), random, terrain paint, select (a
  rect of cells, copy into the brush, delete, move).
- **R22 Random.** A random brush has tiles with weights (default: their probabilities); each painted
  cell chooses by a hash of (layer seed, stroke seed, x, y), so the same operation gives the same map.
- **R23 Terrain painting.** Painting terrain t sets the terrain plane of the painted cells to t (or
  none for erase) and re-chooses the tiles of the cells whose mask changed (D6), all in one step.
- **R24 Best match.** For each such cell, the candidate tiles are the set's tiles whose centre terrain
  is the cell's (dual grid: every tile of the set); each is scored against the cell's mask: +10 per
  constrained peering value that matches, −2 per one that does not, + the number of constrained values
  (specificity); the highest wins, ties choose by probability with the R22 hash. Blob mode clears a
  corner's requirement unless both adjacent sides hold the centre terrain (Pixelorama's corner filter).
  A cell with no candidate keeps its terrain, shows no tile, and is reported (`no_tile_for_terrain`).
- **R25 Bounded refresh.** A terrain change at a cell re-chooses at most its 8 neighbours and itself
  (blob, isometric), 4 + 1 (sides), the 4 dual cells around it (dual grid), 6 + 1 (hex); a box of n
  cells touches O(n + perimeter) cells. `refresh` over a rect re-chooses every terrain cell in it.
- **R26 Manual tiles over terrain.** Painting a tile onto a terrain cell clears that cell's terrain;
  with "Keep Terrain" on (palette toggle, `keep_terrain` op flag) it keeps the terrain and sets the
  cell's lock bit, and refreshes leave locked cells alone. Erasing a tile clears the lock.
- **R27 Dual grid layers.** A layer with `dualGrid` on (set automatically when made from a dual-grid
  terrain set) draws its tiles offset by half a cell down-left: tile (x, y) sits between logical cells
  (x − 1, y − 1)…(x, y); its mask bits are TL = logical (x − 1, y), TR = (x, y), BL = (x − 1, y − 1),
  BR = (x, y − 1) (SpriteCook's image-space order with y up). Brushes target the logical cell under the
  cursor. Only rectangle and isometric Grids accept it; other terrain modes are refused on it.
- **R28 Determinism.** Every operation's result depends only on its arguments, the layer, the tileset
  and the seeds; MCP, Lua and the Scene view give identical cells for identical operations (AC6).
- **R29 Runtime edits.** Lua and C++ edits in Play mark chunks dirty; chunks rebuild once per frame
  (R33) and colliders at the next fixed step (R41). Edits in Play are not saved (as every component).
- **R30 Linked imports are read-only.** A layer whose Grid is linked to an imported map (R47) refuses
  brush edits with `locked_linked` and the fix "unlink the Grid or edit the source map".

### Functional — rendering

- **R31 Chunks are S52 static batches.** Each chunk builds one instance per non-empty cell (S52's 40-byte
  core stream, `CLAMP_FRAME`, the cell's flip bits in the UVs and transpose in the flags; tile edges
  from one edge array per axis, rounded to the texel grid under a Pixel Perfect Camera, S41 R13),
  grouped by source page, in the layout's painter's order, in layer space; the layer's model matrix is
  the batch transform, so moving the GameObject re-uploads nothing.
- **R32 Order.** Where tiles may overlap (a tile larger than its cell, an offset, isometric or hex),
  the painter's order wins and a page change starts a new run in that order; where they cannot
  overlap, a chunk's instances are grouped by page (one draw per page). Chunk mode sorts the layer as
  one item, its chunks back to front; Individual mode gives each tile's anchor to S52's run sort
  (characters walk behind tall tiles with the Y axis).
- **R33 Rebuild.** Only dirty chunks rebuild, on the main thread at the end of the frame (or on jobs
  when more than 4 are dirty), one upload each. Chunks build their GPU buffer the first time they are
  visible; a chunk invisible for 2 s releases its GPU buffer and keeps its cells (`gpuResidency`, a
  layer setting, default "Visible").
- **R34 Culling.** Chunks are culled by world bounds that include tile overhang; the layer's bounds
  are the union.
- **R35 No seams.** With S41's pixel-perfect camera, shared tile edges coincide at every camera
  position (S41 R13 edges, S41's per-vertex snap), and at any zoom no tile samples its atlas
  neighbour (`CLAMP_FRAME`). The
  legacy `textureScaleFactor` inset is not used by layers. The snap is exactly S41's: its rule
  `floor(p + 0.5 + 2⁻⁷)` in base pixels (`pixelSnapClip` per vertex in S52's stage, `pixelart::snap`
  for the CPU edge arrays), no tilemap rounding of its own. A rotated Grid follows S41's rotation
  policy through S52 R11: under Steps90 (the default) the batch transform is drawn rounded to the
  nearest 90° (`roundRotation90`) and every vertex snaps; under Rotate On Grid the layer keeps its
  angle and its pivot snaps (`pixelSnapCenterDelta`), so a chunk's vertices move together and shared
  edges stay joined.
- **R36 Animation.** A chunk keeps the list of its animated instances; when a frame index changes it
  rewrites those instances' UVs and re-uploads that chunk only; chunks without animated tiles do
  nothing per frame.
- **R37 Tint and material.** The layer tint is the material colour (one value per batch); `library:sprite`
  features (OUTLINE, HUE_SHIFT, ...) and the S43/S48 per-instance slots are whatever S52 allows; 2D lights
  and normal maps (the source page's normal texture) work as on sprites.

### Functional — collision

- **R38 Tilemap Collider 2D.** A component on the layer's GameObject (with a Rigidbody2D, static by
  default, added with it) generates colliders from the layer: `source` (`tiles`: the tiles' shapes;
  `terrain`: full logical cells of the listed `solidTerrains`, the natural choice for dual-grid and
  LDtk IntGrid layers), `geometry` (`outlines`: chains, default; `polygons`: convex pieces), `merge`
  (default on), `offset`, and the Collider2D surface fields (friction, restitution, material, categories,
  mask, group, events) shared by every generated collider.
- **R39 Merged chains per chunk.** With `outlines`, the solid shapes of a chunk are unioned with their
  neighbours (D8) into loops, counter-clockwise for solids and clockwise for holes (Box2D's winding,
  `types.h:393`); loops inside the chunk are closed chains; loops crossing the chunk border are split
  into open chains that overlap the neighbour chunk's chains on three points at each crossing
  (`types.h:397-398`), so bodies slide across chunk borders without catching.
- **R40 Kinds.** `one_way` shapes give open chains of their edges facing the one-way direction
  (Box2D chains are one-sided); `sensor` shapes give sensor polygons (convex pieces, `PolygonDecomposition2D`),
  never chains; `polygons` geometry decomposes merged solids into convex pieces per chunk.
- **R41 Incremental.** A cell edit rebuilds the colliders of its chunk and of the neighbour chunks whose
  shared crossings changed, at the next fixed step, before it; the rest of the body is untouched.
- **R42 Generated, not saved.** Generated colliders are runtime state of the Rigidbody2D (a separate
  list from `colliders`), drawn by the physics debug view, read-only in the Inspector ("Generated by
  Tilemap Collider 2D: 312 chains"), refused by `edit_colliders` with a typed error. Contacts report
  the generated collider; `TilemapLayer:getCellAt(point)` (§6.4) gives the cell.
- **R43 Physics queries.** Raycasts, overlaps and contact events work on generated colliders as on any
  other (outlines have no area: an overlap point inside solid ground is not a hit; use `polygons` when
  that matters, the docs say so).

### Functional — import

- **R44 Tiled.** `.tmj` (JSON) and `.tmx` (XML, tinyxml2) with embedded or external (`.tsj`/`.tsx`)
  tilesets: orthogonal, isometric, staggered and hexagonal maps → Grid layouts; finite and infinite
  (chunked) tile layers, csv and base64 with zlib, gzip (stb inflate) or zstd; GID flip bits 31/30/29
  → our flip X/Y/transpose (bit 28 → reported, unrotated); tileset image or image-collection tilesets
  → sources (a collection makes one page per image, reported); tile properties, probability, animation,
  object-group collision (rect, polygon; ellipses as 16-gons) → per-tile data; wang sets (edge → `sides`,
  mixed → `blob`, corner → `blob` with sides `any`) → terrain sets without a terrain plane; layer
  visibility, opacity, offset and group nesting ("Group/Layer" names).
- **R45 LDtk.** `.ldtk` with embedded or external (`.ldtkl`) levels: one Grid per level (Rectangle,
  `gridSize`), Tiles and AutoLayer tiles (`gridTiles`, `autoLayerTiles`: `px`, `src`, `f` flips) → layers
  with a texture-source tileset per LDtk tileset (tile ids from `src` positions); IntGrid values →
  the terrain plane of that layer with a generated terrain set "IntGrid <layer>" (names and colours
  from the LDtk definition), so a Tilemap Collider 2D with `source: terrain` collides on them; tile
  `enumTags` and custom data → properties.
- **R46 Report and preview.** An import first reports what it would make (Grids, layers, tilesets,
  cells, warnings, and what it skips: object and image layers, entities, parallax, hex rotation); the
  same call with `output` writes `.tileset` files next to the map (or in `tilesets_dir`) and creates
  the GameObjects (one undo step) or writes a prefab.
- **R47 Linked maps.** With `linked`, the Grid keeps `importSource` and the layers are read-only;
  when the source file changes (S05), the import runs again in place, matching layers by name (their
  GameObjects, extra components and scripts kept). Unlink keeps the data and makes it editable.
- **R48 Aseprite tilemaps (S44).** "Create Tilemap from Aseprite" turns each Aseprite tilemap layer
  into a Tilemap Layer of an `aseprite`-source tileset, with the cells and flip bits of S44's
  `AsepriteImport::tilemapCels()` (S44 R19; until S50 lands S44 imports them as pixels).

### Functional — legacy tilemaps and other systems

- **R49 Legacy kept.** The legacy `TilemapComponent` loads, renders, saves and scripts exactly as
  today; its Inspector title becomes "Free Tilemap (Legacy)" and it leaves the create menu (code and
  `Tilemap` Lua keep working).
- **R50 Conversion.** "Convert to Grid Tilemap" (Inspector button, `edit_tilemap` op `convert`) turns a
  legacy tilemap whose tiles all have one size and sit on that size's grid into a Grid + Tilemap Layer
  on the same GameObject (same id, name, transform, children): a texture-source tileset with one tile
  per rect (rect names and indices kept as tile names and ids), cells from tiles; anything else is
  refused with `not_grid_aligned` listing up to 10 offending tiles. `flipY` and `textureScaleFactor` are
  dropped with warnings; scripts on the GameObject that use `Tilemap` are named in the result.
- **R51 NavGrid (S19).** A NavGrid with source TILEMAP accepts a Tilemap Layer: a tile's `nav_area`
  wins, else S19's tile-name table (`NavGridTileArea::rect` matches tile names), else
  `emptyCellArea`/walkable as today.
- **R52 Placement and picking users.** Physics placement (`engine/core/physics/PlacementWorld.cpp:169`)
  and the Scene view's bounds (S26) treat a Tilemap Layer like the legacy one (its tiles' bounds).

### Platform & backends

- **R53** Everything runs on GL 3.3, GLES3/WebGL2, Metal, D3D11 and Vulkan through S52's instanced
  path (vertex and fragment shaders only); no compute, no storage buffers.
- **R54** Every export target (Windows, Linux, macOS, Android, iOS, HTML5) loads and draws layers from
  Compile builds and cooked scenes; importers, the Tile Palette and the Tileset Editor are editor-only.
- **R55** Chunk rebuilds and collider builds use the job system (S06) where threads exist and run
  synchronously on single-threaded web, within the same budgets (R57, R58).

### Performance (release; "mobile" = Pixel 6a / iPhone 12 class, "web" = wasm Chrome one thread, "desktop" = build server; `je-bench-tilemap`)

- **R56 Drawing.** 1080p, 16 px tiles, three fully covered layers (about 24,500 visible tiles), one
  page: ≤ 25 draws per layer (visible chunks × pages); GPU ≤ 1.5 ms mobile; CPU per frame ≤ 0.15 ms
  mobile with a static or scrolling map (no rebuild).
- **R57 Edits.** 1,000 random `setCell`s in one frame ≤ 1.0 ms mobile, 1.5 ms web, including
  rebuilds; one 32×32 chunk rebuild ≤ 50 µs desktop, 200 µs mobile; a 1,000-cell terrain box ≤ 2 ms
  mobile.
- **R58 Colliders.** One chunk's outlines ≤ 0.5 ms mobile; a 256×256 solid-and-holes map's first
  build ≤ 30 ms desktop, 120 ms mobile (parallel per chunk).
- **R59 Memory and load.** 4 bytes per cell of a stored chunk (+1 with a terrain plane); 40 bytes per
  non-empty tile of a GPU-resident chunk; a 1,000,000-cell layer decodes in ≤ 40 ms desktop.
- **R60 Animation.** A chunk's animated-tile patch ≤ 30 µs desktop; frames without a frame change cost
  one comparison per animated chunk.
- **R61 Editor.** Brush hover preview ≤ 0.3 ms; a 100,000-cell flood fill with undo ≤ 20 ms desktop.
- **R62 Disabled costs nothing.** A scene without layers runs no tilemap code past the empty component
  arrays; a disabled layer draws nothing, has no colliders and no per-frame CPU.

### Editor UX (human)

- **R63 Creation.** GameObject > 2D Object > Tilemap > Rectangular | Isometric | Isometric Staggered |
  Hexagonal (Pointy Top) | Hexagonal (Flat Top) creates "Grid" with a child "Tilemap"; Assets > Create >
  2D > Tileset; "Create Tileset" in a texture's, atlas's or Aseprite file's context menu; dropping a
  `.tmj`/`.tmx`/`.ldtk` into the Hierarchy opens the import preview.
- **R64 Tile Palette** (Window > 2D > Tile Palette): toolbar Select (S), Paint (B), Box (U), Line (L),
  Flood (G), Eraser (D), Picker (I); "Active Tilemap" dropdown (the selected Grid's layers, "Create
  Layer..."); rotate `[`/`]`, flip `Shift+[`/`Shift+]`; toggles Random and Keep Terrain; tabs Tiles
  (integer-zoom palette, drag a rectangle to make a stamp, hover tooltip id/name/properties), Terrains
  (sets and terrains with colours; choosing one switches the tools to terrain painting), Random
  (weighted list from the selection), Stamps (saved in the tileset). Shortcuts through S25, rebindable.
- **R65 Scene view.** While a tile tool is active: the active layer's grid (diamonds, hexes, dual-grid
  logical cells), the hovered cell, a 50 % ghost of the brush, box and line previews, and a status pill
  "Cell (12, −3) · grass_04 · Grass". Shift while painting erases, Ctrl picks, Esc cancels the stroke
  (restoring the cells), Alt and the right and middle buttons keep the camera. "Focus On" (None, Layer,
  Grid) dims the others.
- **R66 Scene-view tool framework.** A minimal `SceneTool` interface (D9) owns the view's mouse, keys,
  overlay, cursor and status while active; the tile brushes are its first users; the Scene toolbar shows
  the active tool with an exit button; selection and gizmos are suspended, camera navigation is not.
- **R67 Tileset Editor** (double-click a `.tileset`): sources and the tile grid on the left; the
  selected tiles' Inspector on the right (Name, Probability, Offset, Collision with Auto from Alpha and
  a pixel-snapped polygon editor, Animation strip with durations and a preview, Terrain with a peering
  painter on the tile's 3×3 regions (6/12 on hex), Properties from the schema, Navigation Area);
  multi-tile edits; "Apply Terrain Template..." and the coverage panel (R20).
- **R68 Grid Inspector.** Layout and its fields, Cell Size, Cell Gap, Plane; the layer list (add,
  remove, drag to reorder = Order in Layer); Import Source, Linked, Reimport.
- **R69 Layer Inspector.** Tileset, Tint, Material, Sorting Layer, Order in Layer, Mode, Chunk Size,
  Tile Anchor, Animation Speed, Dual Grid, Seed; read-only Used Rect, Cells, Chunks; buttons Clear,
  Refresh Autotile, Compress Bounds.
- **R70 Undo.** One undo step per stroke or command, named "Paint Tiles", "Box Fill", "Flood Fill",
  "Erase Tiles", "Paint Terrain", "Move Tiles", "Refresh Autotile", holding only the changed cells
  (`TilemapCellsCmd`, D10), with its memory counted for S24's cap. Tileset edits are `WriteFilesCmd`s.
- **R71 Thumbnails.** `.tileset` assets show a contact sheet thumbnail; tile tooltips everywhere show the
  tile at integer zoom.
- **R72 Legacy.** The legacy Inspector shows "Convert to Grid Tilemap" with the R50 check result
  before converting.
- **R73 Play mode.** Layers animate in the Scene view only while "Always Refresh" is on or in Play.
- **R74 Docs.** `docs/tilemaps.md` (people: concepts, the palette, terrains, templates, collision,
  importing, Lua) with screenshots from an isolated editor.
- **R74a Pixel-art check.** Tilemap Layers whose scene has a Pixel Perfect Camera (or whose tileset
  pages are pixel-art textures) join S42's `check_pixel_art` input as drawables (S42 R9): S50 adds
  them to `PixelArtCheckInput`, so `filtering`, `mipmaps_compression`, `texel_size`, `off_grid` and
  `palette` cover tile pages and layers, with the tests in S50's AC10.

### Agent UX (MCP, Lua, CLI)

- **R75 `edit_tilemap` (core, extended).** Works on a layer, or on a Grid with `layer` (name or index;
  default its first). New top-level `layer`, `layout` (with `create`: the Grid's layout, default
  rectangle), `tileset` (a `.tileset` path, or the existing slice object, which now writes
  `<texture>.tileset` and assigns it), `preview` (run everything, return the result and image, write
  nothing). New ops: `line`, `flood`, `random`, `terrain` (grid + legend of terrain names, or from/to,
  or cells; `erase`; `keep_terrain`), `refresh`, `stamp`, `add_layer`, `convert`. `set` cells accept a
  fourth element of flip letters (`"x"`, `"y"`, `"t"`). `create` makes Grid + Tilemap Layer. Every call
  is one undo step with rollback on any failing op (as today). The result: `{grid, tilemap, layer,
  created, changed_cells, dirty_chunks, warnings, grid ASCII of the touched region}` plus an image when
  asked. Legacy tilemaps keep the old behaviour, `test_world.py::test_tilemap_from_ascii` passing
  unchanged against the new default.
- **R76 `get_tilemap` (core, extended).** For a Grid or layer: layout, cell size, the layers (name,
  entity, tileset, sorting, cells, used rect), and for the chosen layer and `region` (default: the used
  rect within `max_cells`) the ASCII grid with legend (tile names; `#n` for unnamed), `include_terrain`
  (a second ASCII grid of terrain names), `include_tiles` (cells as `[x, y, tile, flags]`, up to
  `limit`), `missing_tiles` (cells whose id the tileset lacks), and `image` (a CPU composite of the
  region at integer `scale` 1–8, deterministic, no GPU) as a `ToolResult` image. Legacy output unchanged.
- **R77 `edit_tileset` (group `tilemaps`, new).** Create or edit a `.tileset`: sources, tile edits by
  id, name or range (name, probability, offset, collision with `alpha` threshold or polygons,
  animation, properties, nav area), the property schema, terrain sets (mode, terrains, `template` +
  `origin` tile, or explicit peering values), stamps. Preview by default (the result, the coverage
  report and a contact sheet image with ids); `output: true` writes, one undo step. `check: true`
  returns only the coverage and validity report (PASS/WARN/FAIL with counts).
- **R78 `import_tilemap` (group `tilemaps`, new).** `file` (`.tmj`, `.tmx`, `.ldtk`), `level` (LDtk),
  `scene`/`parent` or `prefab`, `tilesets_dir`, `linked`. Preview by default (R46's report); `output:
  true` imports (a job above 1 s, `get_job`), one undo step.
- **R79 Typed errors.** Every failure is `{kind, message, fix, field}`: `unknown_tile`,
  `unknown_terrain`, `unknown_layer`, `layout_mismatch`, `not_grid_aligned`, `tileset_missing`,
  `locked_linked`, `region_too_large` (> 1,000,000 cells), `parse` (import, with line or JSON path),
  `invalid_peering`, `unknown_property`.
- **R80 Lua** (§6.4): cells (get, set, fill, erase, clear), world ↔ cell conversion on the layer and the
  Grid, terrain (set, fill, get, refresh), queries (used rect, cell at a point, tile data and
  properties), `Tileset` lookups; batch edits in one frame rebuild once.
- **R81 CLI.** Everything through `jasperengine-editor call edit_tilemap|get_tilemap|edit_tileset|import_tilemap`
  (S31); `import_tilemap` with `output` exits non-zero on `parse` errors.
- **R82 Agent guide.** `docs/mcp/guide/tilemaps.md` (`jasper://docs/tilemaps`): the coordinate
  convention (y up, the ASCII top line is the highest row, how isometric and hex rows print), the op
  vocabulary, terrain templates and the mask conventions, the import mappings, recipes ("paint a lake
  with the water terrain then read it back as an image"). The guide's `## Pixel art` line names it (§6.1).
- **R83 Determinism.** Same arguments, same cells, same ASCII, same image bytes (R28).
- **R84 Verification loop.** An agent can paint terrain, then check with `get_tilemap` (`image`,
  `include_terrain`, `missing_tiles`) and `edit_tileset check`, without a screenshot or Play.

### Data & compatibility

- **R85** New components (`grid`, `tilemaplayer`, `tilemapcollider2d`) and the new asset type need no
  scene format bump (`StreamMigrations.h:17-20`); the layer node carries `version: 1` for its cell
  encoding, the `.tileset` its own `version: 1`. Any later change to either bumps it with a migration.
  If implementation finds a scene or prefab bump necessary after all, it takes the next version
  number at landing (S44 takes one in wave 5A), with a migration and a migration test.
- **R86** Cells are saved per chunk as one base64 string of the D4 run-length encoding, keyed
  `"<cx>,<cy>"`, chunks in (cy, cx) order: deterministic, diff-friendly per chunk (S29), and prefab
  instance overrides work per chunk.
- **R87** Compile builds emit each chunk string as its own literal (≤ 7 KB for 32×32 chunks, ≤ 28 KB
  for 64×64, split under MSVC's 16 KB literal limit); cooked scenes store the raw chunk bytes through a
  `ComponentHooks` hook.
- **R88** The legacy component's keys and reflection are unchanged (old scenes load and re-save
  byte-identically when untouched); only its display name changes.
- **R89** A layer whose tileset is missing loads its cells, draws nothing, warns in the load report,
  and keeps the cells on save.
- **R90** Tile ids missing from the tileset keep their cells (saved as they are), draw nothing, and are
  counted in `get_tilemap`'s `missing_tiles`.

## 6. Design constraints & decisions

- **D1 New components, legacy kept.** The legacy `TilemapComponent` is a mesh of free quads with a Lua
  API (`addTile`, positions, rects) that scripts depend on; turning it into a grid in place would break
  those scripts and every saved tile with an off-grid position. So S50 adds Unity's split (Grid on the
  parent, a Tilemap Layer per layer, a separate collider component), with Godot's model of one tileset
  per layer and terrain sets in the tileset. The legacy component stays as is (R49); conversion is an
  explicit, checked, undoable command (R50). Names: "Grid", "Tilemap Layer", "Tilemap Collider 2D"; Lua
  `Grid`, `TilemapLayer`, `Tileset`.
- **D2 Layout.** Runtime in `engine/core/tilemap/` (`TileGridLayout.{h,cpp}`, `TileCells.{h,cpp}`, `Tileset.{h,cpp}`,
  `TilesetLoader`, `TerrainSolver`, `TerrainTemplates`, `TilemapChunkBuilder`, `TileColliderBuilder`),
  components in `engine/core/component/`, `TilemapSystem` in `engine/core/subsystem/` (registered after
  the transform update and before `PhysicsSystem`), Lua wrappers in `engine/core/object/tilemap/`.
  Editor in `editor/tilemap/` (`TilemapBrush`, `TilemapCellsCmd`, `TilePaletteWindow`,
  `TilesetEditorWindow`, `TilemapSceneTools`, `import/TiledImporter`, `import/LdtkImporter`,
  `TilemapConvert`) and `editor/scenetool/` (D9). Tilemaps serve every 2D and board game, not only
  pixel art, so they live outside the ledger's `engine/core/pixelart/`.
- **D3 Cell encoding.** `uint32_t`: bits 0–23 tile id + 1 (0 = empty; 16,777,214 ids per tileset),
  bit 24 flip X, 25 flip Y, 26 transpose, 27 lock, 28–31 reserved (0; a loader keeps unknown bits and
  warns). The terrain plane is `uint8_t`: 0 none, 1–254 the tileset's terrain index + 1, 255 reserved.
- **D4 Storage.** A layer holds `std::unordered_map<ChunkCoord, Chunk>`; a chunk is `chunkSize²`
  cells, an optional terrain plane, a non-empty count, a dirty flag, its animated-instance list and its
  GPU batches. Empty chunks are dropped. Encoding per chunk: a sequence of runs `[varint count][u32
  value]` over the cells in row order (then the terrain plane the same way with u8 values when
  present), base64. Worst case for 32×32 is 5 KB of runs, ≤ 6.9 KB base64.
- **D5 Coordinates.** Cell (x, y) with y up; `cellToLocal` gives the cell's anchor point in the Grid's
  plane; `localToCell` uses floor on rectangle and isometric axes and cube rounding on hex (as S19's
  `NavGridMap`); the half-open rule puts a shared edge in the cell above or to the right. All maths in
  double for `localToCell` beyond ±2²⁴ cells, float inside chunks.
- **D6 Terrain solver.** The terrain plane is the truth; tiles are derived. `TerrainSolver::paint(layer,
  cells, terrain)` writes the plane, computes the set of affected display cells (R25), builds each one's
  mask from the plane (blob: 8 directions with the corner filter; sides: 4; dual grid: the 4 logical
  cells as corners; hex: 6 sides, + 6 corners in blob), scores the candidates (R24, after Pixelorama
  `CelTileMap.gd:437-476`), and writes cells, all into one cell patch. No propagation beyond the
  neighbourhood is needed because a mask never depends on neighbour tiles. `two_surface` dual-grid sets
  treat the second terrain as a real terrain, so frame 12 (all corners second terrain) is a normal match.
- **D7 Rendering.** `TilemapChunkBuilder` writes S52 `SpriteInstance`s from cells (UVs from the source
  page with the cell's flips; edges from the layer's edge arrays (S41 R13), origin from R8; flags `CLAMP_FRAME`
  and transpose) into one `StaticSpriteBatch` per page run, with the chunk's revision; S52 draws and culls
  them. Painter's order: rectangle row by row from the top; isometric by (x + y) descending then x;
  hex by row with the stagger. A layer contributes one sort item per chunk (Chunk mode) or per tile
  (Individual mode) to S52.
- **D8 Collider builder.** Per dirty chunk, on jobs: collect the solid shapes of the chunk's cells and of
  a one-cell margin ring (plus overhanging tiles), in integer coordinates (1/16 pixel), union them with
  Clipper2 (non-zero fill), orient the result (solids CCW, holes CW), then: closed loops inside the
  chunk → chain loops; loops crossing the chunk's half-open rectangle → open chains cut at the
  crossings, each extended by the margin geometry so that neighbouring chunks' chains share three points
  at the crossing (inserting collinear points at the border and one cell beyond where the outline has
  none); `polygons` geometry → convex pieces of the clipped union (`PolygonDecomposition2D`). One-way
  shapes skip the union and become open chains of their facing edges; sensors become sensor polygons.
  The results replace the chunk's generated colliders in the Rigidbody2D (`PhysicsSystem::syncRigidbody2DColliders`,
  `PhysicsSystem.cpp:1679`, gains the generated list).
- **D9 Scene-view tool framework.** `editor/scenetool/SceneTool.h`:

  ```cpp
  namespace jasperengine::editor {
      struct SceneToolEvent { float x, y; Ray ray; bool shift, ctrl, alt; int button; };
      enum class SceneToolPress { NotMine, Captured };
      class SceneTool {
      public:
          virtual ~SceneTool() = default;
          virtual const char* id() const = 0;            // "tilemap.paint"
          virtual const char* label() const = 0;         // "Paint (B)"
          virtual bool available(const SceneProject&) const = 0;
          virtual SceneToolPress press(const SceneToolEvent&) = 0;
          virtual void drag(const SceneToolEvent&) {}
          virtual void release(const SceneToolEvent&) {}
          virtual void hover(const SceneToolEvent&) {}
          virtual bool key(ImGuiKey, const SceneToolEvent&) { return false; }
          virtual void cancel() {}                       // Esc, tool switch, scene change: revert
          virtual void drawOverlay(SceneRender&, ImDrawList&) {}
          virtual ImGuiMouseCursor cursor() const { return ImGuiMouseCursor_Arrow; }
          virtual std::string status() const { return {}; }
      };
      class SceneTools {   // one active tool per Scene view
      public:
          static void activate(uint32_t sceneId, std::unique_ptr<SceneTool> tool);
          static SceneTool* active(uint32_t sceneId);
          static void deactivate(uint32_t sceneId);   // calls cancel()
      };
  }
  ```

  `SceneWindow` asks the active tool first in its press (before the spline hook, `SceneWindow.cpp:1278`),
  drag (`:1695`), release (`:1831`) and key (`:2173`) paths and draws its overlay; camera navigation
  keeps priority. The existing special cases are untouched.
- **D10 Undo.** `TilemapCellsCmd` (after `NavGridCellsCmd`): per layer, the changed cells as (cell
  coordinate, old u32, new u32, old terrain, new terrain); `execute` reapplies, `undo` restores, both
  mark the chunks dirty; `memoryBytes` counts the patch; strokes accumulate in a map like
  `NavGridBrush::Stroke` and become one command on release. MCP calls wrap their ops in one command
  through `UndoGroupScope` (one call = one step).
- **D11 Importers** are editor code with no engine dependency beyond the cell store and the tileset
  writer; each has a pure `parse(bytes) → ImportPlan` (tested on fixtures) and an `apply(plan)` that
  makes files and GameObjects. Tiled GIDs map through the map's tileset `firstgid` table; flip bits as
  R44; LDtk tile ids are `(src.y / gridSize) × columns + src.x / gridSize` of the tileset texture.

### 6.1 MCP tools and the budget (S31)

| Tool | Group | Change | Estimated listing chars |
|---|---|---|---|
| `edit_tilemap` | core | `layer`, `layout`, `tileset` as anyOf(path, slice object), `preview`; ops `line`, `flood`, `random`, `terrain`, `refresh`, `stamp`, `add_layer`, `convert`; description rewritten to ≤ 400 characters with the op vocabulary in `jasper://docs/tilemaps` | ≤ 2,523 (its grandfathered limit; the description shrinks by about 80 while the schema grows by about 200; if the measure exceeds it, `layout` moves into the `add_layer` op instead of a raised limit) |
| `get_tilemap` | core | `layer`, `region`, `include_terrain`, `image`, `scale` | ≈ 1,050 (≤ 1,500) |
| `create_entity` | core | none: type `tilemap` now makes Grid + Tilemap Layer | 0 |
| `edit_tileset` | `tilemaps` (new) | new | ≤ 1,500 |
| `import_tilemap` | `tilemaps` (new) | new | ≤ 1,250 |

- Core grows by about 400 characters (get_tilemap); measured with `tool_budget.py --check` at
  implementation. If the default listing has no room, `image` and `scale` leave `get_tilemap`, the map
  image comes from `screenshot` of a framed Scene view, and the guide says so.
- New group `tilemaps` (one row in `McpToolGroups.cpp`): "Tilesets (tiles, collision, terrains,
  animation, properties) and Tiled/LDtk map import (jasper://docs/tilemaps)." 2 tools, ≈ 2,750 of 15,000
  characters. The ledger's "existing tilemap tools and their group" are core; new tools cannot join core
  without a `core_trades` entry, so they form this group.
- Guide: one line in the family's `## Pixel art` digest section (S41 creates it; the `## 2D and 3D`
  section has no room): "tilemaps: Grid + Tilemap Layers, terrain painting: edit_tilemap; tilesets
  and imports: group tilemaps (jasper://docs/tilemaps)" (≈ 130 bytes, the ledger's share).
- Conventions (S31): snake_case, closed objects (`ops` items stay `anyObject` as today), `anyOf` for
  `tileset`, the hidden-until-enabled test for the `tilemaps` group, `test_tool_coverage.py`,
  `gen_tool_docs.py`.

### 6.2 The `.tileset` format (version 1)

```json
{
  "version": 1,
  "name": "Overworld",
  "tile_size": [16, 16],
  "tile_shape": "square",
  "sources": [
    {"id": 0, "kind": "texture", "texture": "guid:9f3c…", "tile_size": [16, 16], "margin": 0, "spacing": 0, "skip_empty": true},
    {"id": 1, "kind": "sheet", "sheet": "guid:…", "frames": ["water_0", "water_1"]},
    {"id": 2, "kind": "aseprite", "file": "guid:…", "tileset": 0}
  ],
  "property_schema": [{"name": "damage", "type": "int", "default": 0}],
  "terrain_sets": [
    {"name": "Ground", "mode": "blob", "terrains": [{"name": "Grass", "color": "#5fa04e"}, {"name": "Dirt", "color": "#8a6a3f"}]},
    {"name": "Shore", "mode": "dual_grid", "two_surface": true, "terrains": [{"name": "Sand", "color": "#e3c27a"}, {"name": "Water", "color": "#3a6fd8"}]}
  ],
  "tiles": [
    {"id": 0, "source": 0, "at": [0, 0], "name": "grass"},
    {"id": 7, "source": 0, "at": [7, 0], "name": "lava", "properties": {"damage": 3},
     "collision": {"mode": "alpha", "threshold": 128, "kind": "solid"},
     "animation": [{"tile": 7, "ms": 120}, {"tile": 8, "ms": 120}], "random_start": true},
    {"id": 12, "source": 0, "at": [4, 1], "probability": 0.25,
     "terrain": {"set": 0, "center": 0, "peering": {"n": 0, "ne": "any", "e": 0, "se": "none", "s": 0, "sw": 0, "w": 0, "nw": 0}}}
  ],
  "stamps": [{"name": "house", "size": [3, 2], "cells": [[0, 0, 20], [1, 0, 21, "x"]]}]
}
```

Tiles absent from `tiles` but present in a texture source are implied with defaults and get ids at
creation (the file then lists them by `id` and `at`, so ids never move). Peering keys are `n ne e se s sw
w nw` (rectangle and isometric; hex uses its six sides and six corners); `any` and `none` as R18.
Written deterministically (keys in this order, tiles by id).

### 6.3 Scene data

```yaml
- grid:
    layout: hexagon
    cellSize: [16, 14]
    cellGap: [0, 0]
    plane: xy
    staggerAxis: y
    staggerIndex: odd
    importSource: {guid: "", path: ""}
    linked: false
- tilemaplayer:
    version: 1
    enabled: true
    tileset: {guid: "…", path: "tiles/overworld.tileset"}
    tint: [1, 1, 1, 1]
    sortingLayer: 0
    sortingOrder: 0
    mode: chunk
    chunkSize: 32
    dualGrid: false
    seed: 0
    chunks:
      "-1,0": "AQEAAAD…"
      "0,0": "IQEAAAA…"
- tilemapcollider2d:
    source: tiles
    geometry: outlines
    merge: true
```

### 6.4 API sketches

```cpp
namespace jasperengine {
    // TileGridLayout, not GridLayout: nav::GridLayout (S19's NavGrid, NavGridMap.h:31) already exists
    enum class TileGridLayout : uint8_t { Rectangle, Isometric, IsometricStaggered, Hexagon };
    enum class StaggerAxis : uint8_t { X, Y };
    enum class StaggerIndex : uint8_t { Odd, Even };
    enum class TilemapRenderMode : uint8_t { Chunk, Individual };
    struct CellCoord { int32_t x = 0, y = 0; };
    constexpr uint32_t TILE_FLIP_X = 1u << 24, TILE_FLIP_Y = 1u << 25, TILE_TRANSPOSE = 1u << 26, TILE_LOCK = 1u << 27;
    constexpr int32_t NO_TILE = -1;

    struct GridComponent {
        TileGridLayout layout = TileGridLayout::Rectangle;
        Vector2 cellSize = Vector2(16, 16);
        Vector2 cellGap = Vector2(0, 0);
        GridPlane plane = GridPlane::XY;
        StaggerAxis staggerAxis = StaggerAxis::Y;
        StaggerIndex staggerIndex = StaggerIndex::Odd;
        float hexSideLength = 0;          // 0 = regular
        AssetRef importSource; bool linked = false;
    };

    struct TilemapLayerComponent {
        bool enabled = true;
        AssetRef tileset;
        Vector4 tint = Vector4(1, 1, 1, 1);
        Material material;
        int32_t sortingLayer = 0; int16_t sortingOrder = 0;   // S52
        TilemapRenderMode mode = TilemapRenderMode::Chunk;
        uint8_t chunkSize = 32;
        Vector2 tileAnchor = Vector2(0, 0);
        float animationSpeed = 1.0f;
        bool dualGrid = false;
        uint32_t seed = 0;
        // runtime: std::shared_ptr<TileCells> cells; tileset handle; revisions
    };

    struct TilemapCollider2DComponent {
        TileColliderSource source = TileColliderSource::Tiles;   // Tiles, Terrain
        std::vector<std::string> solidTerrains;
        TileColliderGeometry geometry = TileColliderGeometry::Outlines;   // Outlines, Polygons
        bool merge = true;
        Vector2 offset = Vector2(0, 0);
        Collider2D surface;   // friction, restitution, material, filter, events; shape fields unused
    };

    class TileCells {   // engine/core/tilemap/TileCells.h
    public:
        int32_t getTile(CellCoord c, uint32_t* flags = nullptr) const;
        void setTile(CellCoord c, int32_t tile, uint32_t flags = 0);
        int getTerrain(CellCoord c) const;                 // -1 none
        void setTerrain(CellCoord c, int terrain);
        Recti usedRect() const;
        template<class F> void forEachCell(Recti r, F&& f) const;
        std::string encodeChunk(ChunkCoord) const;  bool decodeChunk(ChunkCoord, std::string_view, std::string& error);
    };
}
```

```lua
local grid  = Grid(scene, gridEntity)
local layer = TilemapLayer(scene, layerEntity)
local ts    = layer.tileset                      -- Tileset

local lava = ts:findTile("lava")                 -- id, or -1
layer:setCell(4, -2, lava)                       -- id -1 erases
layer:setCell(5, -2, lava, TileFlip.X | TileFlip.TRANSPOSE)
local id, flags = layer:getCell(4, -2)
layer:fill(0, 0, 9, 3, ts:findTile("stone"))
layer:eraseCell(4, -2) ; layer:clear()

local x, y = layer:worldToCell(player.position)  -- Vector3 in world space
local p = layer:cellToWorld(x, y)                -- the cell's anchor, world space
local tile, cx, cy = layer:getCellAt(contact.point)
print(ts:getProperty(tile, "damage"))

layer:setTerrain(3, 3, "Water")                  -- name or index; re-chooses the neighbours
layer:fillTerrain(0, 0, 15, 15, "Grass")
print(layer:getTerrain(3, 3))                    -- index, or -1
layer:refreshAutotile(0, 0, 31, 31)

local r = layer.usedRect                          -- Rect in cells
print(layer.cellCount, grid.cellSize, grid.layout)
```

## 7. Acceptance criteria

| AC | Verification |
|---|---|
| AC1 (R4, R5, D3, D4, R86, R87, R59) | `engine/tests/core/tilemap/TileCellsTest.cpp`: set/get/erase at ±2³⁰, chunk drop when empty, encode/decode round trip of random chunks and of the worst case, deterministic bytes, unknown high bits kept; decode of a 1,000,000-cell layer within R59 (`je-bench-tilemap --decode`). |
| AC2 (R1, R7, R8, D5) | `TileGridLayoutTest.cpp`: for every layout and stagger option, `localToCell(cellToLocal(c)) == c` over a 200×200 range, edge points by the half-open rule, neighbour sets, painter's order, and equality with Tiled's documented pixel positions for a fixture map of each orientation. |
| AC3 (R11–R17, R19, R20, §6.2) | `TilesetTest.cpp`: load/save byte-identical; re-slicing keeps ids (red control: a build that numbers by position fails); alpha collision of fixture tiles equals expected pixel outlines; templates assign the expected peering values (golden JSON); coverage lists exactly the masks missing from a fixture blob set with 3 tiles removed. |
| AC4 (R23–R27, D6, R28) | `TerrainSolverTest.cpp`: blob 47 painting of fixture shapes equals golden cell grids; scoring and corner filter cases from Pixelorama's rules; dual grid masks equal SpriteCook's `frameByMask` for all 16 masks, two-surface frame 12; hex sides; refresh touches only R25's cells (counted); same seed → same tiles, other seed → other variations; locked cells untouched. |
| AC5 (R31–R37, R56, R60) | `engine/tests/core/render/TilemapGpuTest.cpp`: goldens of a rectangle, isometric, staggered and both hex maps with tall tiles, flips and transposes, dual grid, tint and `library:sprite` OUTLINE, on glcore and Vulkan; `SeamFree`: camera at 0.25 px steps and 1.5× zoom with a high-contrast atlas: no background pixel between tiles and no neighbour-frame colour (red control: tile edges computed per tile instead of from the edge arrays, and `CLAMP_FRAME` cleared, fails); `AnimatedChunkOnly`: one animated tile re-uploads only its chunk (counters); draw counts asserted for R56's scene; web gate `GATE=tilemap tools/build.sh web-gate`. |
| AC6 (R21, R22, R28, R83, D10) | `editor/tests/tilemap/TilemapBrushTest.cpp`: every brush op on every layout equals golden grids; MCP `edit_tilemap` and the brush give identical cells for the same operation; one undo per stroke restores the cells exactly; memory of a 100,000-cell flood patch reported; R61 timing in `je-bench-tilemap --editor`. |
| AC7 (R38–R43, D8, R58) | `engine/tests/core/physics/TilemapColliderTest.cpp`: a box slides over a 64-chunk floor at 3 speeds with friction 0 and keeps its velocity within 1 % (no catch at chunk borders; red control: chains cut at the border without the three-point overlap catch); outlines match the union of shapes (point-in-polygon sampling); holes; one-way passes from below and blocks from above; sensors fire events; an edit rebuilds only the touched and border-sharing chunks (counters); `source: terrain` on a dual-grid layer collides on logical cells; R58 timings in `je-bench-tilemap --colliders`. |
| AC8 (R44–R47, D11) | `editor/tests/tilemap/TiledImporterTest.cpp`, `LdtkImporterTest.cpp` on fixture maps under `editor/tests/fixtures/tilemaps/` (every orientation, infinite chunks, csv/base64/zlib/gzip/zstd, flips, external tilesets, wang sets, LDtk Tiles/AutoLayer/IntGrid, external levels): the plan's cells, tiles and properties equal golden JSON; skipped items reported; malformed files give `parse` errors with location. `tools/mcp/tests/test_import_tilemap.py`: preview writes nothing, `output` writes tilesets and GameObjects in one undo step, a linked map re-imports on a file change keeping a user component on its layer, linked layers refuse brush edits. |
| AC9 (R49, R50, R88) | `editor/tests/tilemap/TilemapConvertTest.cpp`: an aligned legacy map converts with equal pixels (GPU golden before/after), ids = old rect indices, names kept; a misaligned one refuses with the offending tiles; undo restores the legacy component. `StreamComponentTest`: legacy fixtures re-save byte-identically; `test_world.py::test_tilemap_from_ascii` passes unchanged on the new default. |
| AC10 (R75, R76, R79, R84, R74a) | `tools/mcp/tests/test_tilemap_grid.py`: `check_pixel_art` on a pixel-art scene lists a Tilemap Layer with a bilinear tile page under `filtering` (and PASS once fixed); create each layout; paint ASCII; `terrain` op paints a lake then `get_tilemap include_terrain` and `image` read it back (image bytes stable over two runs); `region`; `missing_tiles`; every typed error kind with its `fix`; rollback on a failing later op; `preview` writes nothing and adds no undo entry. |
| AC11 (R77, R78, §6.1) | `tools/mcp/tests/test_tileset_tools.py`: `edit_tileset` preview/output, templates, coverage `check` PASS/WARN/FAIL, one undo step, contact sheet image; the `tilemaps` group hidden until `tool_groups` enables it, then listed; `test_tool_budget.py`, `test_tool_conventions.py`, `test_tool_coverage.py`, `gen_tool_docs.py --check`, `test_protocol.py`. |
| AC12 (R80, R29) | `tools/mcp/tests/test_tilemap_lua.py` (Play, `read_console`): every Lua function of §6.4; 1,000 `setCell`s in one frame rebuild each dirty chunk once (counter); world↔cell on a rotated, scaled Grid; `tools/lua_api/tests` find the classes; the doc test runs `docs/tilemaps.md`'s Lua blocks. |
| AC13 (R63–R73, R66) | `tools/mcp/tests/test_tile_palette_editor.py` (xvfb, isolated editor, driven through MCP and `find_ui_item`): create a Grid from the menu, choose tiles, paint, box, flood, erase, pick, terrain, Esc cancels a stroke, undo names; the Tileset Editor paints peering values and applies a template; screenshots of the palette, the Scene view grid overlay on each layout and the Tileset Editor in the evidence (`docs/specs/evidence/s50/`). `check-hardcoded-shortcuts.py` passes. |
| AC14 (R51, R52, R10) | `NavGridTilemapTest`: a NavGrid from a Tilemap Layer takes `nav_area` and the name table; `tools/mcp/tests/test_tilemap_grid.py::test_pick`: `pick_at` on a tile hits the layer, on an empty cell does not. |
| AC15 (R53–R55, R57, R62) | the GPU suites on glcore and Vulkan; `tools/build.sh web` + `web-gate`, `tools/build.sh android` (emulator gate) with the `tilemap` test project; `tools/smoke-test.sh` exports a scene with layers and colliders (Compile and prebuilt players) and its screenshot matches the editor's; `je-bench-tilemap` desktop numbers in CI, mobile and web measured once on a Pixel 6a and an iPhone 12, recorded in `docs/specs/evidence/s50/` with the table appended here; a scene with no layers shows no `TilemapSystem` time in the CPU profiler (S15). |

## 8. Dependencies & conflicts

- **Depends on (all landed before wave 5C).** S52 M2 (static batches, `SpriteInstance`, flags) for
  drawing (R31–R37), S52 M1 for Sorting Layers on layers; S41 for the snap rule and the shared-edge
  rule (R13); S44 for the `aseprite` source, R48 and `SpriteSheetPool`; S45 for atlas and texture
  sheets in the `sheet` source; S42 for `PixelArtCheckInput` (R74a). Existing: S05 for linked
  re-import; S19 for R51; S26 for R10.
- **Interfaces given.** S52 receives one sort item per chunk or per tile; S42 Tilemap Layer drawables
  (R74a); S49/S48 nothing; S19 a Tilemap Layer source.
- **Merge hotspots.** `WorldTools.cpp` (tilemap section, `create_entity` in `EntityTools.cpp`),
  `SceneWindow.cpp` (S26, S18, S19 also hook there; D9 adds one hook per path), `InspectorWindow.cpp`,
  `HierarchyWindow.cpp:511`, `PhysicsSystem.cpp` (generated colliders), `NavigationSystem.cpp`,
  `AssetType.h` (S43, S44, S45 also append; `Tileset = 27` is reserved so landing order does not matter),
  `McpToolGroups.cpp`, `McpInstructions.cpp`, `budget.json`, `RuntimeFields.gen.inc`, `NOTICE.md`
  (Clipper2), `engine/libs/` (Clipper2).

## 9. Risks & defaults

- **Two tilemap systems.** The legacy one stays for compatibility. Default: hidden from the create
  menu, a conversion command, docs that only teach the grid; removal is a later decision with a
  migration.
- **Core listing room.** If `get_tilemap`'s growth does not fit the default listing, `image` and
  `scale` drop out of it and the guide points to `screenshot` (§6.1).
- **Aseprite tile indices.** Deleting a tile in Aseprite shifts the indices after it; our ids follow
  Aseprite's indices for that source. Default: S44's reimport compares tile images and warns when a
  map's cells would change meaning; the docs say to append, not delete.
- **Tiled corner wang sets** do not map one to one onto our modes (R44). Default: imported as blob with
  sides `any` and reported; the tiles themselves always import exactly.
- **Chain overlap correctness** depends on both chunks computing the same crossing geometry. Default:
  the margin ring is built from the same cell data on both sides, in integer coordinates; AC7's
  sliding test is the gate.
- **Hex terrain.** Hex blob uses 12 peering values; few hex tilesets exist. Default: supported by the
  solver, no template.
- **Clipper2** adds a vendored library (Boost licence, compatible). Default: vendored, header kept,
  `NOTICE.md` entry.
- **Ledger location.** Code under `engine/core/tilemap/` and `editor/tilemap/` rather than
  `engine/core/pixelart/` (D2).

## Milestones

- **M1** Cell store, layouts, Grid and Tilemap Layer components, serialization (YAML, codegen, cooked),
  Lua cells and conversion maths, legacy conversion (R1–R10, R49, R50, R80 part, R85–R90); AC1, AC2, AC9.
- **M2** Tileset asset with texture sources, per-tile data, alpha collision tracing, Tileset Editor
  basics (R11–R17, R67 part, R71); AC3 part.
- **M3** Rendering through S52 static batches, animation, residency (R31–R37, R53, R54); AC5. Needs S52 M2.
- **M4** Scene-view tool framework, brushes, Tile Palette, undo, Inspectors, creation menus (R21, R22,
  R26, R29, R30, R63–R66, R68–R70, R72, R73); AC6, AC13 part.
- **M5** Terrain sets, solver, templates, coverage, dual grid, Terrains tab (R18–R20, R23–R25, R27,
  R28); AC4, AC3 rest.
- **M6** Tilemap Collider 2D (R38–R43); AC7.
- **M7** MCP: `edit_tilemap`, `get_tilemap`, `edit_tileset`, guide (R75–R77, R79, R81–R84); AC10, AC11.
- **M8** Importers: Tiled, LDtk, linked maps, Aseprite tilemaps, sheet sources (R12 rest, R44–R48, R78);
  AC8. Needs S44/S45 for their sources.
- **M9** NavGrid and picking integration, budgets, docs, final gate (R51, R52, R55–R62, R74); AC12,
  AC14, AC15.

## Deviations

- **R31, R59 (instance stream).** S52 landed `SpriteInstanceBatch` with `InstanceRenderData` (96 bytes: matrix, colour,
  texture rect; plus 8 bytes of snap pivot in a snapping scene), not the old 40-byte stream, so a GPU-resident tile costs
  96-104 bytes. Flips are a negative texture rect, transpose swaps the quad's axes in the instance matrix (exact zeros,
  so S41's every-vertex snap applies).
- **R31-R33 (chunks through S52).** S52's attach takes one batch per GameObject; S50 adds `SpriteInstanceChunkList`
  (`RenderSystem::attachInstanceChunks`): a lead batch the mesh loads with, and one `SpriteInstanceBatch` per chunk whose
  buffers are swapped in for its draw, each culled, built on first view and uploaded on its own.
- **R35 `CLAMP_FRAME`.** A MESH variant bit, `Tcl` (54, `TILE_CLAMP_FRAME`), set for a Tilemap Layer's instanced
  variant: the fragment stage clamps the first UV set half a texel inside the instance's frame (mesh.frag and
  library:sprite). Shader cache v65 (after the terrain programme's v63 and S48's v64).
- **R2, §6.3 (sorting and material).** Sorting Layer, Order in Layer and the material live on the layer's MeshComponent,
  as for Sprites (S52's renderer fields), not on `TilemapLayerComponent`; the Inspector shows them in the layer section.
- **D4, R86, R87 (encoding).** Each chunk starts with a header byte (log2 of the chunk size, a terrain-plane bit), so a
  chunk string decodes on its own. Cooked scenes store the raw chunk bytes through the `chunks` field's `Codec`
  (`CustomCodecs.h`) instead of a `ComponentHooks` hook; Compile builds emit one `decodeChunk` statement per chunk with
  the string split under 16,000 characters. A prefab instance overrides the `chunks` field whole, not per chunk.
- **§6.2 (references, ids).** A source's asset is written `{"path": ..., "guid": ...}` (a path or `"guid:<hex>"`
  string is read too), and `"next_id"` keeps deleted ids from being handed out again (R13).
- **D5 (hexagons).** `localToCell` on hexagons uses an exact band and zigzag test of the layout's rows instead of cube
  rounding, so a non-regular `hexSideLength` is exact too; the half-open rule is the documented one.
- **R32 (Individual mode).** A layer in Individual mode gives each anchor row of a chunk its own sort item (the tiles of a
  row share their sort distance along the Y axis), not each tile.
- **§6.4 (enums).** The layout and collider enums are int-sized, not `: uint8_t`: the editor's enum fields read and write
  4 bytes.
- **R38 (surface).** `TilemapCollider2DComponent` lists the Collider2D surface fields it shares (friction, restitution,
  material, category, mask, group, events) instead of holding a `Collider2D`; category and mask are 32-bit fields
  narrowed to Box2D's 16 bits. The Inspector and MCP refuse wider values; one set from Lua or C++ is clamped (category
  and mask to 65535, group to -32768..32767) with one warning per change of the fields.
- **Units.** A Grid's Cell Size is in world units; a tile's image is its pixels over the project's Pixels Per Unit.
  Colliders are built in texels and scaled back.
- **R12 (aseprite source).** The Aseprite importer (version 2) writes each tileset's strip and resolves
  `"<file>#tileset#<index>"`, so the runtime and exports read the tiles without the `.aseprite` file.
- **R12, R53 (sprite import, open).** A tileset's image infers Sprite (no mips) only when it gets its first import
  settings after the `.tileset` names it. An image imported before (dropped in, then made a tileset) keeps the `default`
  type S14 wrote down ("written then, never silently re-inferred"): it cooks with mips and the exported map shows seams
  at a minifying zoom. Setting the source image's type from `edit_tileset` and the Tileset Editor is left to the
  coordinator's decision (it changes S14's rule).
- **AC5, AC15 (measurement).** No `je-bench-tilemap` binary: the R56-R61 timings are measured in the unit tests through
  `perfSlack()`, and the mobile (Pixel 6a, iPhone 12) and web numbers are not measured (no devices on this host). Web,
  Android, Vulkan and the smoke export run at the batch landing (owner decision of 2026-11-08). The GPU placement tests
  compare against pictures computed in the test from R7/R8, not PNG goldens.
- **R73 (Always Refresh).** "Always Refresh" is the Scene view's former "Animate water" toggle (Rendering section; the
  stored key stays `animateWater`, MCP `animate_water`): water, tile animations and the shaders on the scene clock run
  in Edit mode while it is on, and always in Play.
- **R69 (Compress Bounds).** The cell store drops empty chunks as it goes, so the button reports the used rect and
  chunk count and changes nothing.
- **R67 (Navigation Area).** The Tileset Editor edits a tile's `nav_area` as an integer area index, not a named list.
- **R49, R88 (legacy positions).** The legacy Tilemap's mesh build clamped negative tile positions to 0 by writing
  the clamp back into the component, so a map that was drawn once saved, and converted, with moved tiles. It now draws
  them at 0 as before and leaves the stored positions as authored (the picture is unchanged; a re-save keeps what was
  loaded), so R50's conversion reads the authored cells.
- **R82, §6.1 (MCP guide).** No `## Pixel art` line: per the ledger's upstream rule change the area is one "Other
  areas" entry (`tilemaps (edit_tilemap)`, `jasper://docs/tilemaps`). The guide's room came from moving the Unity-names
  sentence on Build Settings and Scenes in Build to `docs/unity-glossary.md` (already there) and the Code section.
- **R2, R8 (tileAnchor).** `tileAnchor` (ax, ay) moves every tile by ax cell pitches along the plane's u and ay along v, on
  every layout (not along the isometric cell axes). One rule (`tilegrid::tileShift` in `TileGridLayout.h`: tileAnchor plus the
  R27 dual-grid shift) places the drawn instances, the colliders, the culling bounds, picking (R10), `getCellAt`, the NavGrid
  source (R51) and the editor's previews.
- **§6.4 (`Grid(scene, entity)`).** It adds a Grid to a GameObject that has none, as the other component wrappers do
  (SortingGroup, CanvasGroup, Animator, PixelPerfectCamera); `TilemapLayer(scene, entity)` adds nothing, and its calls on a
  GameObject without a Tilemap Layer raise a Lua error.
- **R89 (load report).** A layer whose tileset does not load gets an Info `fallback` issue on `tileset` ("tileset_missing: ...
  keeps its N cells and draws nothing"); chunks that do not read are refused whole as an invalid value (their text kept).
- **R75 `create`.** `create` makes a Grid + Tilemap Layer, except with `rects`, `replace_rects` or `origin`, which keep
  making a Free Tilemap (arbitrary pixel rects have no `.tileset` source). `create_entity` `tilemap` is a Grid + layer;
  the Free Tilemap is `free_tilemap`. The result's `grid` holds the Grid's GameObject fields and the ASCII view together.
  The result's `kind` (`grid`, `free_tilemap`) says which it made, a Free Tilemap create's `next` says why, and those
  fields on a Grid are refused (`invalid_value`).
- **§6.3 (the layer's tileset).** A layer stores its tileset as the `tileset` path and a sibling `tilesetGuid` key, the
  shape every other asset field of a scene has, not a `{guid, path}` map. A `.tileset`'s sources resolve by GUID, then by
  their path hint, in the editor; an export writes each `.tileset` with its sources' current paths and no GUIDs, so a game
  loads them by path. A `.tileset`'s texture imports as a sprite sheet does (no mips; point filtering in Pixel Art Mode).
