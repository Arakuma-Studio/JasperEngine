// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Lua's Localization (docs/specs/S95-lua-runtime-gaps.md REQ-26, REQ-27): String Tables lookup in
// the game's language, which is TitleArt's (one value, one onLanguageChanged event). The raising
// calls are plain lua_CFunctions that free their C++ values before lua_error (Lua is built as C).

#include "LuaBinding.h"

#include "lua.hpp"

#include "LuaBridge.h"
#include "LuaBridgeAddon.h"

#include "LocalizationArgsLua.h"
#include "localization/Localization.h"
#include "object/TitleArt.h"

#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    // Localization.get(key [, args])
    int localizationGet(lua_State* L){
        const char* key = luaL_checkstring(L, 1);
        bool failed = false;
        {
            LocalizationArgs args;
            std::string error;
            if (!luaReadLocalizationArgs(L, 2, args, error)){
                lua_pushstring(L, ("Localization.get: " + error + ".").c_str());
                failed = true;
            }else{
                const std::string text = Localization::get(key, args);
                lua_pushlstring(L, text.data(), text.size());
            }
        }
        if (failed) return lua_error(L);
        return 1;
    }

    // Localization.language = code: an invalid tag raises, nothing changes
    int localizationSetLanguage(lua_State* L){
        const char* code = luaL_checkstring(L, 1);
        bool failed = false;
        {
            std::string error;
            if (!Localization::setLanguage(code, &error)){
                lua_pushstring(L, ("Localization: " + error + ".").c_str());
                failed = true;
            }
        }
        if (failed) return lua_error(L);
        return 0;
    }

}

void LuaBinding::registerLocalizationClasses(lua_State *L){
    luabridge::getGlobalNamespace(L)
        .beginClass<Localization>("Localization")
        .addStaticProperty("language", &Localization::getLanguage, &localizationSetLanguage)
        .addStaticProperty("languages", &Localization::getLanguages)
        .addStaticProperty("onLanguageChanged", +[]() { return &TitleArt::getOnLanguageChanged(); })
        .addStaticFunction("get", &localizationGet)
        .addStaticFunction("has", &Localization::has)
        .endClass();
}
