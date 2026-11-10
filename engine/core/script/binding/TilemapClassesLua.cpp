// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT
//
// Grid tilemaps in Lua (docs/specs/S50-grid-tilemap.md R80, §6.4): Grid, TilemapLayer, Tileset,
// TileFlip and TileGridLayout. Lua is compiled as C: luaL_error longjmps over C++ frames, so
// functions that raise errors do their work in helpers and raise only from frames holding nothing
// with a destructor.

#include "LuaBinding.h"

#include "lua.hpp"

#include "LuaBridge.h"
#include "LuaBridgeAddon.h"

#include "component/TilemapLayerComponent.h"
#include "math/Vector3.h"
#include "object/tilemap/Grid.h"
#include "object/tilemap/TilemapLayer.h"

#include <cstdio>
#include <string>

using namespace jasperengine;

namespace luabridge {
    template<> struct Stack<TileGridLayout> : Enum<TileGridLayout, TileGridLayout::Rectangle, TileGridLayout::Isometric, TileGridLayout::IsometricStaggered, TileGridLayout::Hexagon> {};
}

static_assert(TILE_FLIP_X == 16777216u && TILE_FLIP_Y == 33554432u && TILE_TRANSPOSE == 67108864u, "TileFlip's literals");

#ifndef DISABLE_LUA_BINDINGS
namespace {

    using TilemapErrorText = char[1024];

    void tilemapCopyError(TilemapErrorText& error, const std::string& text) {
        std::snprintf(error, sizeof(TilemapErrorText), "%s", text.c_str());
    }

    TilemapLayer* layerOf(lua_State* L) {
        return luabridge::detail::Userdata::get<TilemapLayer>(L, 1, false);
    }

    Vector3 vectorAt(lua_State* L, int index) {
        Vector3 value(0.0f, 0.0f, 0.0f);
        if (auto got = luabridge::Stack<Vector3>::get(L, index))
            value = *got;
        return value;
    }

    // layer:getCell(x, y) -> id, flags
    int layerGetCell(lua_State* L) {
        TilemapLayer* layer = layerOf(L);
        if (!layer) return 0;
        uint32_t flags = 0;
        const int tile = layer->getTile((int)luaL_checkinteger(L, 2), (int)luaL_checkinteger(L, 3), &flags);
        lua_pushinteger(L, tile);
        lua_pushinteger(L, (lua_Integer)flags);
        return 2;
    }

    int layerSetCell(lua_State* L) {
        TilemapLayer* layer = layerOf(L);
        if (!layer) return 0;
        layer->setCell((int)luaL_checkinteger(L, 2), (int)luaL_checkinteger(L, 3), (int)luaL_checkinteger(L, 4),
                       (uint32_t)luaL_optinteger(L, 5, 0));
        return 0;
    }

    int layerFill(lua_State* L) {
        TilemapLayer* layer = layerOf(L);
        if (!layer) return 0;
        layer->fill((int)luaL_checkinteger(L, 2), (int)luaL_checkinteger(L, 3), (int)luaL_checkinteger(L, 4), (int)luaL_checkinteger(L, 5),
                    (int)luaL_checkinteger(L, 6), (uint32_t)luaL_optinteger(L, 7, 0));
        return 0;
    }

    int layerWorldToCell(lua_State* L) {
        TilemapLayer* layer = layerOf(L);
        if (!layer) return 0;
        const CellCoord cell = layer->worldToCell(vectorAt(L, 2));
        lua_pushinteger(L, cell.x);
        lua_pushinteger(L, cell.y);
        return 2;
    }

    int layerCellToWorld(lua_State* L) {
        TilemapLayer* layer = layerOf(L);
        if (!layer) return 0;
        const Vector3 world = layer->cellToWorld((int)luaL_checkinteger(L, 2), (int)luaL_checkinteger(L, 3));
        if (!luabridge::push(L, world)) lua_pushnil(L);
        return 1;
    }

    // layer:getCellAt(point) -> tile (-1 when none), cx, cy
    int layerGetCellAt(lua_State* L) {
        TilemapLayer* layer = layerOf(L);
        if (!layer) return 0;
        int tile = NO_TILE;
        CellCoord cell;
        layer->getCellAt(vectorAt(L, 2), tile, cell);
        lua_pushinteger(L, tile);
        lua_pushinteger(L, cell.x);
        lua_pushinteger(L, cell.y);
        return 3;
    }

    // a terrain argument: a name, or a global index (-1: none)
    bool terrainIndexOf(lua_State* L, int index, TilemapLayer* layer, int& out, TilemapErrorText& error) {
        if (lua_isinteger(L, index)) {
            out = (int)lua_tointeger(L, index);
            return true;
        }
        const std::string name = luaL_checkstring(L, index);
        out = layer->getTileset().findTerrain(name);
        if (out == NO_TERRAIN) {
            tilemapCopyError(error, "TilemapLayer: unknown terrain \"" + name + "\" (see Tileset:getTerrainCount and getTerrainName)");
            return false;
        }
        return true;
    }

    bool applyTerrain(lua_State* L, const CellRect& rect, int argument, TilemapErrorText& error) {
        TilemapLayer* layer = layerOf(L);
        if (!layer) return true;
        int terrain = NO_TERRAIN;
        if (!terrainIndexOf(L, argument, layer, terrain, error)) return false;
        std::string failure;
        if (!layer->paintTerrain(rect, terrain, failure)) {
            tilemapCopyError(error, "TilemapLayer: " + failure);
            return false;
        }
        return true;
    }

    int layerSetTerrain(lua_State* L) {
        TilemapErrorText error;
        const CellCoord cell{(int32_t)luaL_checkinteger(L, 2), (int32_t)luaL_checkinteger(L, 3)};
        if (!applyTerrain(L, CellRect::of(cell, cell), 4, error)) return luaL_error(L, "%s", error);
        return 0;
    }

    int layerFillTerrain(lua_State* L) {
        TilemapErrorText error;
        const CellCoord a{(int32_t)luaL_checkinteger(L, 2), (int32_t)luaL_checkinteger(L, 3)};
        const CellCoord b{(int32_t)luaL_checkinteger(L, 4), (int32_t)luaL_checkinteger(L, 5)};
        if (!applyTerrain(L, CellRect::of(a, b), 6, error)) return luaL_error(L, "%s", error);
        return 0;
    }

    bool applyRefresh(lua_State* L, TilemapErrorText& error) {
        TilemapLayer* layer = layerOf(L);
        if (!layer) return true;
        const CellCoord a{(int32_t)luaL_checkinteger(L, 2), (int32_t)luaL_checkinteger(L, 3)};
        const CellCoord b{(int32_t)luaL_checkinteger(L, 4), (int32_t)luaL_checkinteger(L, 5)};
        std::string failure;
        if (!layer->refreshAutotile(CellRect::of(a, b), failure)) {
            tilemapCopyError(error, "TilemapLayer: " + failure);
            return false;
        }
        return true;
    }

    int layerRefresh(lua_State* L) {
        TilemapErrorText error;
        if (!applyRefresh(L, error)) return luaL_error(L, "%s", error);
        return 0;
    }

    int gridWorldToCell(lua_State* L) {
        Grid* grid = luabridge::detail::Userdata::get<Grid>(L, 1, false);
        if (!grid) return 0;
        const CellCoord cell = grid->worldToCell(vectorAt(L, 2));
        lua_pushinteger(L, cell.x);
        lua_pushinteger(L, cell.y);
        return 2;
    }

    Vector3 gridCellToWorld(const Grid* grid, int x, int y) {
        return grid->cellToWorld(x, y);
    }

    int tilesetGetProperty(lua_State* L) {
        TilesetHandle* tileset = luabridge::detail::Userdata::get<TilesetHandle>(L, 1, false);
        if (!tileset) return 0;
        const TilePropertyValue* value = tileset->findProperty((int32_t)luaL_checkinteger(L, 2), luaL_checkstring(L, 3));
        if (!value) {
            lua_pushnil(L);
            return 1;
        }
        switch (value->type) {
            case TilePropertyType::Bool: lua_pushboolean(L, value->b ? 1 : 0); break;
            case TilePropertyType::Int: lua_pushinteger(L, (lua_Integer)value->i); break;
            case TilePropertyType::Float: lua_pushnumber(L, value->f); break;
            case TilePropertyType::String: lua_pushlstring(L, value->s.data(), value->s.size()); break;
            case TilePropertyType::Color:
                if (!luabridge::push(L, value->color)) lua_pushnil(L);
                break;
        }
        return 1;
    }

}
#endif //DISABLE_LUA_BINDINGS

void LuaBinding::registerTilemapClasses(lua_State *L){
#ifndef DISABLE_LUA_BINDINGS
    luabridge::getGlobalNamespace(L)
        // (literals: the Lua API extractor reads the values; the static_asserts keep them D3's bits)
        .beginNamespace("TileFlip")
        .addVariable("X", 16777216u)
        .addVariable("Y", 33554432u)
        .addVariable("TRANSPOSE", 67108864u)
        .endNamespace()
        .beginNamespace("TileGridLayout")
        .addVariable("RECTANGLE", TileGridLayout::Rectangle)
        .addVariable("ISOMETRIC", TileGridLayout::Isometric)
        .addVariable("ISOMETRIC_STAGGERED", TileGridLayout::IsometricStaggered)
        .addVariable("HEXAGON", TileGridLayout::Hexagon)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .deriveClass<Grid, EntityHandle>("Grid")
        .addConstructor <void (*) (Scene*, Entity)> ()
        .addProperty("layout", &Grid::getLayout, &Grid::setLayout)
        .addProperty("cellSize", &Grid::getCellSize, &Grid::setCellSize)
        .addProperty("cellGap", &Grid::getCellGap, &Grid::setCellGap)
        .addFunction("worldToCell", &gridWorldToCell)
        .addFunction("cellToWorld", &gridCellToWorld)
        .addFunction("getGridComponent", &Grid::getComponent<GridComponent>)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<TilesetHandle>("Tileset")
        .addProperty("tileCount", &TilesetHandle::getTileCount)
        .addProperty("terrainCount", &TilesetHandle::getTerrainCount)
        .addProperty("valid", &TilesetHandle::isValid)
        .addFunction("findTile", &TilesetHandle::findTile)
        .addFunction("getTileName", &TilesetHandle::getTileName)
        .addFunction("findTerrain", &TilesetHandle::findTerrain)
        .addFunction("getTerrainName", &TilesetHandle::getTerrainName)
        .addFunction("getNavArea", &TilesetHandle::getNavArea)
        .addFunction("getProperty", &tilesetGetProperty)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<TilemapLayer, Mesh>("TilemapLayer")
        .addConstructor <void (*) (Scene*), void (*) (Scene*, Entity)> ()
        .addProperty("tileset", &TilemapLayer::getTileset)
        .addProperty("cellCount", &TilemapLayer::getCellCount)
        .addProperty("chunkCount", &TilemapLayer::getChunkCount)
        .addProperty("usedRect", &TilemapLayer::getUsedRectArea)
        .addFunction("getCell", &layerGetCell)
        .addFunction("setCell", &layerSetCell)
        .addFunction("eraseCell", &TilemapLayer::eraseCell)
        .addFunction("fill", &layerFill)
        .addFunction("clear", &TilemapLayer::clear)
        .addFunction("worldToCell", &layerWorldToCell)
        .addFunction("cellToWorld", &layerCellToWorld)
        .addFunction("getCellAt", &layerGetCellAt)
        .addFunction("setTerrain", &layerSetTerrain)
        .addFunction("fillTerrain", &layerFillTerrain)
        .addFunction("getTerrain", &TilemapLayer::getTerrain)
        .addFunction("refreshAutotile", &layerRefresh)
        .addFunction("getTilemapLayerComponent", &TilemapLayer::getComponent<TilemapLayerComponent>)
        .endClass();
#endif //DISABLE_LUA_BINDINGS
}
