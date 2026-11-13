// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef LOCALIZATION_ARGS_LUA_H
#define LOCALIZATION_ARGS_LUA_H

// The arguments table of Localization.get and el:setLocalized (S95 REQ-26, REQ-28): its array
// part is {0} {1}..., its string keys {name}; values are strings, numbers or booleans. Reading it
// never raises (the callers raise once their C++ values are gone: Lua is built as C).

#include "lua.hpp"

#include "localization/Localization.h"

#include <map>
#include <string>

namespace jasperengine{

    // false with `error` for a value of another type; nil or none at `index` is no arguments
    inline bool luaReadLocalizationArgs(lua_State* L, int index, LocalizationArgs& out, std::string& error){
        out = LocalizationArgs();
        if (lua_isnoneornil(L, index)) return true;
        if (!lua_istable(L, index)){
            error = std::string("the arguments are a table, not a ") + luaL_typename(L, index);
            return false;
        }
        index = lua_absindex(L, index);
        std::map<lua_Integer, LocalizationArg> positional;
        lua_pushnil(L);
        while (lua_next(L, index) != 0){
            LocalizationArg arg;
            const int type = lua_type(L, -1);
            if (type == LUA_TNUMBER) arg = LocalizationArg::ofNumber(lua_tonumber(L, -1));
            else if (type == LUA_TSTRING){
                size_t length = 0;
                const char* text = lua_tolstring(L, -1, &length);
                arg = LocalizationArg::ofText(std::string(text, length));
            }else if (type == LUA_TBOOLEAN) arg = LocalizationArg::ofText(lua_toboolean(L, -1) ? "true" : "false");
            else{
                error = std::string("an argument is a ") + lua_typename(L, type) + ": pass strings, numbers or booleans";
                lua_pop(L, 2);
                return false;
            }
            // lua_isinteger never converts the key (lua_tostring on it would confuse lua_next)
            if (lua_type(L, -2) == LUA_TNUMBER && lua_isinteger(L, -2) && lua_tointeger(L, -2) >= 1){
                positional[lua_tointeger(L, -2)] = arg;
            }else if (lua_type(L, -2) == LUA_TSTRING){
                out.named.emplace_back(lua_tostring(L, -2), arg);
            }
            lua_pop(L, 1);
        }
        // {0} is the first of the list (t[1])
        for (lua_Integer i = 1; positional.count(i); i++) out.positional.push_back(positional[i]);
        return true;
    }

}

#endif //LOCALIZATION_ARGS_LUA_H
