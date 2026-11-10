# Grid tilemaps

Build levels from tiles on a grid: rectangle, isometric, staggered isometric and hexagonal layouts,
terrain autotiling, tile collision and animation, chunked rendering with no tile limit.

## Concepts

- **Grid** sets the cell layout (Layout, Cell Size, Cell Gap, Plane) for the Tilemap Layers on its
  GameObject and below it. Cell Size is in world units; with the default Pixels Per Unit of 1 that
  is pixels. Without a Grid, a layer uses a Rectangle grid of its tileset's tile size.
- **Tilemap Layer** holds the cells of one **Tileset**. A Grid can have any number of layers; they
  sort like sprites, by Sorting Layer and Order in Layer.
- **Cells** are addressed by signed (x, y) with y up: row 0 is the bottom row, negative coordinates
  are allowed, and a layer has no tile limit.
- **Tileset** (`.tileset`) is an asset made from a texture slice, a Sprite Atlas or sprite sheet, or an
  Aseprite tileset. It stores each tile's name, probability, offset, collision, animation, terrain
  and properties.

Create one from GameObject > 2D Object > Tilemap (Rectangular, Isometric, Isometric Staggered,
Hexagonal Pointy Top or Flat Top). That makes a Grid with a child Tilemap. Create tilesets from
Assets > Create > 2D > Tileset, or from a texture's right-click menu.

A texture that becomes a tileset's image (Create Tileset, the Tileset Editor, `edit_tileset`, a
Tiled or LDtk import) switches to the pixel-art Sprite import: no mipmaps, Point filtering and no
compression, so tiles never blend with their neighbours in the atlas. The switch is part of the
same undo step, and the Console names any other asset (a material, say) that uses the texture too.
An Aseprite tileset's images follow its file's own import settings.

## The Tile Palette

Window > 2D > Tile Palette paints tiles in the Scene view. Choose the Active Tilemap, then a tool:
Select (S), Paint (B), Box (U), Line (L), Flood (G), Eraser (D), Picker (I). Rotate the brush with
`[` and `]`, flip it with Shift+`[` and Shift+`]`.

- Hold Shift while painting to erase, Ctrl to pick a tile from the map, Esc to cancel a stroke.
  Alt and the right and middle mouse buttons keep moving the camera.
- The **Tiles** tab: drag a rectangle to make a multi-tile stamp. The **Random** tab paints
  weighted random tiles, the **Stamps** tab reuses saved stamps.
- Every stroke is one undo step.

## Terrains

A terrain set in the Tileset Editor (double-click the `.tileset`) describes how tiles fit together.
Three modes exist:

- **Blob** (Match Corners and Sides): the 47-tile layouts.
- **Sides** (Match Sides): 16 tiles.
- **Dual Grid** (Match Corners): each drawn tile sits between four logical cells; set a layer's Dual
  Grid on.

Use **Apply Terrain Template...** to assign a standard sheet layout (`blob_47`, `blob_47_godot`,
`sides_16`, `dual_grid_4x4`) in one step, and the coverage panel to list the masks no tile matches.
In the Tile Palette's **Terrains** tab, pick a terrain and paint: the tiles around the painted cells
are chosen automatically, and the choice only depends on the painted cells and the layer's seed.
Paint a tile by hand over terrain with Keep Terrain on to lock that one cell.

## Collision

Set per-tile collision in the Tileset Editor: Full, Alpha (traced from the tile's pixels) or Custom
polygons, each Solid, One Way or Sensor. Add a **Tilemap Collider 2D** to the layer's GameObject (a
static Rigidbody2D comes with it). Source Tiles uses each tile's shapes; Source Terrain makes solid
terrains of a Dual Grid or LDtk IntGrid layer collide cell by cell. Geometry Outlines makes merged
chains; a body resting on them slides across chunk borders without catching. Outlines have no area,
so an overlap test at a point inside solid ground misses: use Geometry Polygons when your game
queries points inside walls.

## Animated tiles

A tile may list frames with durations. Animated tiles run on the game clock, times the layer's
Animation Speed; in Edit mode they move only while Always Refresh is on.

## Importing

Drop a Tiled (`.tmj`, `.tmx`) or LDtk (`.ldtk`) file into the Hierarchy to preview and import it. The
tilesets are written next to the map. A **Linked** import keeps the map read-only and re-imports when
the file changes, matching layers by name. Unlink it on the Grid to edit it in the engine. Aseprite
tilemap layers import with "Create Tilemap from Aseprite". Convert an older Free Tilemap with the
Convert to Grid Tilemap button in its Inspector.

## Lua

```lua
local grid  = Grid(scene, gridEntity)
local layer = TilemapLayer(scene, layerEntity)
local ts    = layer.tileset

local lava = ts:findTile("lava")
layer:setCell(4, -2, lava)
layer:setCell(5, -2, lava, TileFlip.X | TileFlip.TRANSPOSE)
local id, flags = layer:getCell(4, -2)
layer:fill(0, 0, 9, 3, ts:findTile("stone"))
layer:eraseCell(4, -2)

local x, y = layer:worldToCell(player.position)
local p = layer:cellToWorld(x, y)
local tile, cx, cy = layer:getCellAt(contact.point)
print(ts:getProperty(tile, "damage"))

layer:setTerrain(3, 3, "Water")
layer:fillTerrain(0, 0, 15, 15, "Grass")
print(layer:getTerrain(3, 3))
layer:refreshAutotile(0, 0, 31, 31)

local r = layer.usedRect
print(layer.cellCount, grid.cellSize, grid.layout)
```

Edits in one frame rebuild each touched chunk once. Edits in Play are not saved.
