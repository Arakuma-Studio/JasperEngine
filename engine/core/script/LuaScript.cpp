// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LuaScript.h"

#include "lua.hpp"

#include "LuaBridge.h"

#include "LuaBinding.h"

using namespace jasperengine;

void LuaScript::setGameObject(const std::string& global, GameObject* object){
    lua_State *L = LuaBinding::getLuaState();

    if (!luabridge::push<GameObject*>(L, object))
        throw luabridge::makeErrorCode(luabridge::ErrorCode::LuaStackOverflow);

    lua_setglobal(L, global.c_str());
}

GameObject* LuaScript::getGameObject(const std::string& global){
    lua_State *L = LuaBinding::getLuaState();

    lua_getglobal(L, global.c_str());

    auto result = luabridge::get<GameObject*>(L, -1);
    if (! result)
        throw result.error();

    return result.value();
}

