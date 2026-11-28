// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef LOCALIZATION_ARGS_LUA_H
#define LOCALIZATION_ARGS_LUA_H

// A Lua table as the arguments of a localised message (docs/specs/game-ui-localization.md R5, D-15):
// string keys fill {name}, the list part fills {0} {1} ... (its first element is {0}); numbers stay
// numbers, booleans read "true" / "false". Shared by Localization, Text and UIElement:setLocalized.

#include "localization/LocalizationArgs.h"

#include "lua.hpp"

#include <string>

namespace jasperengine::luaargs {

    inline LocalizationArgs localizationArgs(lua_State* L, int index) {
        LocalizationArgs args;
        if (!lua_istable(L, index)) return args;
        index = lua_absindex(L, index);
        lua_pushnil(L);
        while (lua_next(L, index) != 0) {
            std::string name;
            if (lua_type(L, -2) == LUA_TSTRING) {
                name = lua_tostring(L, -2);
            } else if (lua_isinteger(L, -2) && lua_tointeger(L, -2) >= 1) {
                name = std::to_string(lua_tointeger(L, -2) - 1);
            }
            if (!name.empty()) {
                if (lua_isinteger(L, -1)) args.emplace_back(name, static_cast<int64_t>(lua_tointeger(L, -1)));
                else if (lua_type(L, -1) == LUA_TNUMBER) args.emplace_back(name, static_cast<double>(lua_tonumber(L, -1)));
                else if (lua_type(L, -1) == LUA_TBOOLEAN) args.emplace_back(name, std::string(lua_toboolean(L, -1) ? "true" : "false"));
                else if (lua_type(L, -1) == LUA_TSTRING) args.emplace_back(name, std::string(lua_tostring(L, -1)));
            }
            lua_pop(L, 1);
        }
        return args;
    }

}

#endif //LOCALIZATION_ARGS_LUA_H
