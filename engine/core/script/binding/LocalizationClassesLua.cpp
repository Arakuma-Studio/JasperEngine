// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Lua's Localization table (docs/specs/game-ui-localization.md R17, docs/localization.md): String
// Table lookups with ICU arguments, the game language and its change event, and the development
// scan. Bound with the Lua C API (stubs/Localization.lua is hand-written); always registered,
// whatever modules the game compiles. `language`, `languages` and `onLanguageChanged` are fields
// through the table's metatable.

#include "LuaBinding.h"
#include "script/binding/LocalizationArgsLua.h"
#include "lua.hpp"
#include "LuaBridge.h"

#include "localization/Localization.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>

using namespace jasperengine;

namespace {

    // Lua is compiled as C: a raise (luaL_error, luaL_check*) longjmps over the C++ frames up to the
    // pcall, running no destructor (MSVC may end the process). The functions check their arguments
    // while their frame holds nothing with a destructor, and raise only then.

    // the arguments at `index`: nil or a table, checked here before any is made
    void checkArgs(lua_State* L, int index) {
        if (!lua_isnoneornil(L, index)) luaL_checktype(L, index, LUA_TTABLE);
    }

    // a Lua table {name = value} (checkArgs first) -> arguments; numbers with no fraction become integers
    LocalizationArgs argsAt(lua_State* L, int index) {
        return luaargs::localizationArgs(L, index);
    }

    void pushString(lua_State* L, const std::string& text) {
        lua_pushlstring(L, text.data(), text.size());
    }

    int luaGet(lua_State* L) {
        size_t length = 0;
        const char* key = luaL_checklstring(L, 1, &length);
        checkArgs(L, 2);
        size_t fallbackLength = 0;
        const char* fallback = luaL_optlstring(L, 3, "", &fallbackLength);
        const LocalizationArgs args = argsAt(L, 2);
        pushString(L, Localization::get(std::string_view(key, length), args, std::string_view(fallback, fallbackLength)));
        return 1;
    }

    int luaFormat(lua_State* L) {
        size_t length = 0;
        const char* pattern = luaL_checklstring(L, 1, &length);
        checkArgs(L, 2);
        const char* language = luaL_optstring(L, 3, "");
        const LocalizationArgs args = argsAt(L, 2);
        pushString(L, Localization::format(std::string_view(pattern, length), args, language));
        return 1;
    }

    int luaHas(lua_State* L) {
        size_t length = 0;
        const char* key = luaL_checklstring(L, 1, &length);
        lua_pushboolean(L, Localization::has(std::string_view(key, length)));
        return 1;
    }

    // false (and nothing changes) for an invalid tag
    int luaSetLanguage(lua_State* L) {
        lua_pushboolean(L, Localization::setLanguage(luaL_checkstring(L, 1)));
        return 1;
    }

    int luaGetLanguageName(lua_State* L) {
        pushString(L, Localization::getLanguageName(luaL_checkstring(L, 1)));
        return 1;
    }

    int luaIsRightToLeft(lua_State* L) {
        lua_pushboolean(L, Localization::isRightToLeft(luaL_optstring(L, 1, "")));
        return 1;
    }

    void pushMiss(lua_State* L, const Localization::Miss& miss) {
        lua_createtable(L, 0, 5);
        pushString(L, miss.key);
        lua_setfield(L, -2, "key");
        pushString(L, miss.language);
        lua_setfield(L, -2, "language");
        pushString(L, miss.kind);
        lua_setfield(L, -2, "kind");
        pushString(L, miss.where);
        lua_setfield(L, -2, "where");
        lua_pushinteger(L, miss.count);
        lua_setfield(L, -2, "count");
    }

    // {literal = {{where, text}...}, missing = {{key, language, kind, where, count}...}}; empty in release
    int luaScan(lua_State* L) {
        const Localization::ScanReport report = Localization::scan();
        lua_createtable(L, 0, 2);
        lua_createtable(L, static_cast<int>(report.literal.size()), 0);
        for (size_t k = 0; k < report.literal.size(); k++) {
            lua_createtable(L, 0, 2);
            pushString(L, report.literal[k].where);
            lua_setfield(L, -2, "where");
            pushString(L, report.literal[k].text);
            lua_setfield(L, -2, "text");
            lua_rawseti(L, -2, static_cast<lua_Integer>(k + 1));
        }
        lua_setfield(L, -2, "literal");
        lua_createtable(L, static_cast<int>(report.missing.size()), 0);
        for (size_t k = 0; k < report.missing.size(); k++) {
            pushMiss(L, report.missing[k]);
            lua_rawseti(L, -2, static_cast<lua_Integer>(k + 1));
        }
        lua_setfield(L, -2, "missing");
        return 1;
    }

    int luaLocalizationIndex(lua_State* L) {
        const std::string_view field = luaL_checkstring(L, 2);
        if (field == "language") {
            pushString(L, Localization::getLanguage());
            return 1;
        }
        if (field == "languages") {
            const std::vector<std::string>& languages = Localization::getLanguages();
            lua_createtable(L, static_cast<int>(languages.size()), 0);
            for (size_t k = 0; k < languages.size(); k++) {
                pushString(L, languages[k]);
                lua_rawseti(L, -2, static_cast<lua_Integer>(k + 1));
            }
            return 1;
        }
        if (field == "onLanguageChanged") {
            if (!luabridge::push(L, &Localization::onLanguageChanged)) lua_pushnil(L);
            return 1;
        }
        lua_pushnil(L);
        return 1;
    }

    // Localization.onLanguageChanged = function(code) ... end
    int luaLocalizationNewIndex(lua_State* L) {
        const char* field = luaL_checkstring(L, 2);
        if (std::string_view(field) == "onLanguageChanged") {
            luaL_checktype(L, 3, LUA_TFUNCTION);
            lua_pushvalue(L, 3);
            Localization::onLanguageChanged = L;
            lua_settop(L, 3);
            return 0;
        }
        if (std::string_view(field) == "language") {
            // D-15: as Unity's SelectedLocale, assigning is setLanguage; a bad tag raises
            const char* code = luaL_checkstring(L, 3);
            char message[256] = {};
            {
                // no C++ object may be alive when luaL_error leaves this frame
                std::string error;
                if (!Localization::setLanguage(code, &error))
                    std::snprintf(message, sizeof(message), "%s", error.empty() ? "not a language tag" : error.c_str());
            }
            if (message[0])
                return luaL_error(L, "Localization.language = '%s': %s; use a tag such as \"de\" or \"pt-BR\", or \"\" to follow the device", code, message);
            return 0;
        }
        return luaL_error(L, "Localization.%s is read-only", field);
    }

}

void LuaBinding::registerLocalizationClasses(lua_State* L){
#ifndef DISABLE_LUA_BINDINGS
    static const luaL_Reg functions[] = {
        {"get", luaGet},
        {"format", luaFormat},
        {"has", luaHas},
        {"setLanguage", luaSetLanguage},
        {"getLanguageName", luaGetLanguageName},
        {"isRightToLeft", luaIsRightToLeft},
        {"scan", luaScan},
        {nullptr, nullptr},
    };
    luaL_newlib(L, functions);
    lua_createtable(L, 0, 2);
    lua_pushcfunction(L, luaLocalizationIndex);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, luaLocalizationNewIndex);
    lua_setfield(L, -2, "__newindex");
    lua_setmetatable(L, -2);
    lua_setglobal(L, "Localization");
#endif //DISABLE_LUA_BINDINGS
}
