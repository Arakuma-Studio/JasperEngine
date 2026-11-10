"""The grid tilemap Lua API (docs/specs/S50-grid-tilemap.md R80, R29, §6.4, AC12; bindings in
engine/core/script/binding/TilemapClassesLua.cpp), in Play through eval_lua: Grid and TilemapLayer,
cells with TileFlip flags, fill, erase, clear, world <-> cell on a rotated and scaled Grid, getCellAt,
terrain painting and refresh with the error of an unknown terrain, usedRect, cellCount, Tileset lookups
and properties, the Grid's cellSize and layout, and 1,000 setCells in one frame.

The rebuild counter of R29 (TilemapSystem::getRebuiltChunks, TilemapLayerState::rebuilt) has no Lua or
MCP reader, so the batch's test checks what a script sees: every cell, and the chunks they fill.
"""

from __future__ import annotations

import pngdiff
from harness import Editor

CI = True
EDITOR = {"create": True, "name": "TilemapLua"}
TIMEOUT = 300

TILES_PNG = "assets/art/lua_tiles.png"
TILESET = "assets/art/lua_tiles.tileset"
HEAD = "local grid = Grid(scene, scene:findEntity('Map'))\nlocal layer = TilemapLayer(scene, scene:findEntity('Tilemap'))\nlocal ts = layer.tileset\n"

S: dict = {}


async def setup(ed: Editor) -> int:
    if S.get("ed") is ed:
        return S["scene"]
    S.clear()
    S["ed"] = ed
    data = bytearray()
    for y in range(64):
        for x in range(64):
            data.extend([(x // 16) * 60, (y // 16) * 60, 100, 255])
    path = ed.project / TILES_PNG
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(pngdiff.encode(64, 64, bytes(data)))
    await ed.call("edit_tileset", path=TILESET, output=True, sources=[{"texture": TILES_PNG, "tile_size": [16, 16], "skip_empty": False}],
                  property_schema=[{"name": "damage", "type": "int", "default": 0}],
                  terrain_sets=[{"name": "Shore", "mode": "sides", "terrains": [{"name": "Water", "color": "#3a6fd8"}], "template": "sides_16", "origin": 0}],
                  tiles=[{"id": 15, "name": "lava", "properties": {"damage": 3}}, {"id": 1, "name": "stone"}])
    made = await ed.call("create_scene", name="TilemapLua", type="2D", path="scenes/TilemapLua")
    sid = made["scene"]["id"]
    await ed.call("create_entity", scene=sid, type="camera", name="Main Camera")
    await ed.call("set_main_camera", scene=sid, entity="Main Camera")
    await ed.call("edit_tilemap", scene=sid, create={"name": "Map", "position": [100, 50, 0]}, tileset=TILESET, cell_size=[16, 16])
    await ed.call("set_properties", scene=sid, entity="Map", values={"transform": {"rotation": {"euler": [0, 0, 90]}, "scale": [2, 2, 1]}})
    await ed.call("save_scene", scene=sid)
    S["scene"] = sid
    return sid


async def lua(ed: Editor, code: str) -> list:
    return (await ed.call("eval_lua", code=HEAD + code))["values"][0]


async def test_lua_api(ed: Editor):
    sid = await setup(ed)
    started = await ed.call("play", action="start", scene=sid)
    ed.check_eq(started["state"], "playing", "the scene plays")
    try:
        cells = await lua(ed, """
local lava, stone = ts:findTile('lava'), ts:findTile('stone')
layer:setCell(4, -2, lava)
layer:setCell(5, -2, lava, TileFlip.X | TileFlip.TRANSPOSE)
local id4, flags4 = layer:getCell(4, -2)
local id5, flags5 = layer:getCell(5, -2)
layer:fill(0, 0, 9, 3, stone)
local filled = layer.cellCount
layer:eraseCell(4, -2)
local erased = layer:getCell(4, -2)
local r = layer.usedRect
local used = {r.x, r.y, r.width, r.height}
layer:clear()
return {lava, stone, id4, flags4, id5, flags5 == (TileFlip.X | TileFlip.TRANSPOSE), filled, erased, used, layer.cellCount,
        ts:findTile('nope'), ts:getProperty(lava, 'damage'), (ts:getProperty(stone, 'damage') or -99), ts:getTileName(lava),
        grid.cellSize.x, grid.cellSize.y, grid.layout == TileGridLayout.RECTANGLE, ts.tileCount}
""")
        lava, stone, id4, flags4, id5, flipped, filled, erased, used, after_clear, nope, damage, stone_damage, name, cw, ch, rect, count = cells
        ed.check(lava == 15 and stone == 1, f"findTile by name: {cells}")
        ed.check(id4 == lava and flags4 == 0 and id5 == lava and flipped, f"setCell/getCell with TileFlip flags: {cells}")
        ed.check_eq(filled, 42, "fill 10 x 4 plus the two cells")
        ed.check_eq(erased, -1, "eraseCell empties the cell")
        ed.check_eq(used, [0, -2, 10, 6], "usedRect in cells (x, y, width, height)")
        ed.check_eq(after_clear, 0, "clear empties the layer")
        ed.check_eq(nope, -1, "findTile of an unknown name")
        ed.check(damage == 3 and stone_damage == 0, f"getProperty: the tile's value and the schema default: {cells}")
        ed.check_eq(name, "lava", "getTileName")
        ed.check(cw == 16 and ch == 16 and rect, f"grid.cellSize and grid.layout: {cells}")
        ed.check_eq(count, 16, "tileCount")

        space = await lua(ed, """
local a = grid:cellToWorld(1, 0)
local b = grid:cellToWorld(2, 1)
local center = Vector3((a.x + b.x) / 2, (a.y + b.y) / 2, 0)
local gx, gy = grid:worldToCell(center)
local lx, ly = layer:worldToCell(center)
local p = layer:cellToWorld(1, 0)
layer:setCell(1, 0, ts:findTile('lava'))
local tile, cx, cy = layer:getCellAt(center)
local none = layer:getCellAt(Vector3(center.x + 200, center.y + 200, 0))
return {a.x, a.y, p.x, p.y, gx, gy, lx, ly, tile, cx, cy, none}
""")
        ax, ay, px, py, gx, gy, lx, ly, tile, cx, cy, none = space
        # rotated 90 degrees and scaled 2: cell (1, 0)'s anchor is 32 units from the Grid, across its x axis
        ed.check(abs(ax - 100) < 1e-3 and abs(abs(ay - 50) - 32) < 1e-3, f"cellToWorld on a rotated, scaled Grid: {space}")
        ed.check(abs(px - ax) < 1e-3 and abs(py - ay) < 1e-3, "the layer's cellToWorld is its Grid's")
        ed.check((gx, gy, lx, ly) == (1, 0, 1, 0), f"worldToCell inverts it: {space}")
        ed.check((tile, cx, cy) == (15, 1, 0), f"getCellAt: {space}")
        ed.check_eq(none, -1, "getCellAt on an empty cell")

        terrain = await lua(ed, """
local water = ts:findTerrain('Water')
layer:fillTerrain(10, 10, 14, 14, 'Water')
layer:setTerrain(20, 20, 'Water')
layer:refreshAutotile(10, 10, 20, 20)
local inner = layer:getCell(12, 12)
local ok, message = pcall(function() layer:setTerrain(1, 1, 'Lava') end)
return {water, layer:getTerrain(12, 12), layer:getTerrain(20, 20), layer:getTerrain(30, 30), inner, ok, tostring(message), ts.terrainCount, ts:getTerrainName(water)}
""")
        water, t12, t20, t30, inner, ok, message, terrains, terrain_name = terrain
        ed.check(water >= 0 and t12 == water and t20 == water, f"fillTerrain, setTerrain and getTerrain: {terrain}")
        ed.check_eq(t30, -1, "getTerrain of a cell without terrain")
        ed.check(inner >= 0, f"the terrain chose a tile: {terrain}")
        ed.check(not ok and 'unknown terrain "Lava"' in message, f"an unknown terrain is an error naming it: {terrain}")
        ed.check(terrains == 1 and terrain_name == "Water", f"terrainCount and getTerrainName: {terrain}")

        batch = await lua(ed, """
layer:clear()
for y = 0, 24 do
    for x = 0, 39 do
        layer:setCell(x, y, 1)
    end
end
return {layer.cellCount, layer.chunkCount}
""")
        ed.check_eq(batch, [1000, 2], "1,000 setCells in one frame: every cell, in the two 32 x 32 chunks they touch")
        await ed.call("wait", frames=3)
        later = await lua(ed, "return {layer.cellCount, layer.chunkCount}")
        ed.check_eq(later, [1000, 2], "the frame after keeps them")
    finally:
        await ed.call("play", action="stop")

