// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Lua's DataAsset table (docs/specs/S95-lua-runtime-gaps.md REQ-23): Data Assets, Unity's
// ScriptableObjects, as shared tables (DataAsset.h). Bound with the Lua C API (`nil, message`
// results), so its stub is hand-written (stubs/DataAsset.lua).

#include "LuaBinding.h"
#include "lua.hpp"

#include "script/DataAsset.h"

#include <string>

using namespace jasperengine;

namespace {

    int pushNilAndMessage(lua_State* L, const std::string& message) {
        lua_pushnil(L);
        lua_pushstring(L, message.c_str());
        return 2;
    }

    int luaDataAssetLoad(lua_State* L) {
        std::string error;
        if (!DataAsset::push(L, luaL_checkstring(L, 1), &error)) return pushNilAndMessage(L, error);
        return 1;
    }

    int luaDataAssetLoadAll(lua_State* L) {
        std::string error;
        if (!DataAsset::pushAll(L, luaL_checkstring(L, 1), &error)) return pushNilAndMessage(L, error);
        return 1;
    }

    int luaDataAssetCreate(lua_State* L) {
        std::string error;
        if (!DataAsset::pushNew(L, luaL_checkstring(L, 1), &error)) return pushNilAndMessage(L, error);
        return 1;
    }

    int luaDataAssetClassOf(lua_State* L) {
        std::string className;
        if (DataAsset::classOf(L, 1, className)) lua_pushstring(L, className.c_str());
        else lua_pushnil(L);
        return 1;
    }

    int luaDataAssetPathOf(lua_State* L) {
        std::string path;
        if (DataAsset::pathOf(L, 1, path)) lua_pushstring(L, path.c_str());
        else lua_pushnil(L);
        return 1;
    }

}

void LuaBinding::registerDataAssetClasses(lua_State* L){
#ifndef DISABLE_LUA_BINDINGS
    static const luaL_Reg functions[] = {
        {"load", luaDataAssetLoad},
        {"loadAll", luaDataAssetLoadAll},
        {"create", luaDataAssetCreate},
        {"classOf", luaDataAssetClassOf},
        {"pathOf", luaDataAssetPathOf},
        {nullptr, nullptr},
    };
    luaL_newlib(L, functions);
    lua_setglobal(L, "DataAsset");
#endif //DISABLE_LUA_BINDINGS
}
