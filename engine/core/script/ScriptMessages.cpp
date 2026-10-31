// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ScriptMessages.h"

#include "Scene.h"
#include "Log.h"
#include "component/ScriptComponent.h"
#include "script/LuaBinding.h"

#include "lua.hpp"

#include <algorithm>
#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    std::vector<ScriptMessages::ReceiversChanged>& listeners(){
        static std::vector<ScriptMessages::ReceiversChanged> list;
        return list;
    }

    // a function named `method` in the instance table or what its __index reaches
    bool luaInstanceDefines(lua_State* L, void* instance, const char* method){
        int ref = static_cast<int>(reinterpret_cast<intptr_t>(instance));
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        if (!lua_istable(L, -1)){
            lua_pop(L, 1);
            return false;
        }
        lua_getfield(L, -1, method);
        const bool defined = lua_isfunction(L, -1);
        lua_pop(L, 2);
        return defined;
    }

}

const char* ScriptMessages::methodName(ScriptMessage m){
    switch (m){
        case ScriptMessage::CollisionEnter: return "onCollisionEnter";
        case ScriptMessage::CollisionStay: return "onCollisionStay";
        case ScriptMessage::CollisionExit: return "onCollisionExit";
        case ScriptMessage::TriggerEnter: return "onTriggerEnter";
        case ScriptMessage::TriggerStay: return "onTriggerStay";
        case ScriptMessage::TriggerExit: return "onTriggerExit";
        case ScriptMessage::CollisionEnter2D: return "onCollisionEnter2D";
        case ScriptMessage::CollisionStay2D: return "onCollisionStay2D";
        case ScriptMessage::CollisionExit2D: return "onCollisionExit2D";
        case ScriptMessage::TriggerEnter2D: return "onTriggerEnter2D";
        case ScriptMessage::TriggerStay2D: return "onTriggerStay2D";
        case ScriptMessage::TriggerExit2D: return "onTriggerExit2D";
        case ScriptMessage::JointBreak: return "onJointBreak";
        case ScriptMessage::JointBreak2D: return "onJointBreak2D";
        case ScriptMessage::Count: break;
    }
    return "";
}

ScriptMessageMask ScriptMessages::receivers(Scene* scene, Entity entity){
    if (!scene || !scene->isEntityCreated(entity)){
        return 0;
    }
    ScriptComponent* scripts = scene->findComponent<ScriptComponent>(entity);
    if (!scripts){
        return 0;
    }
    lua_State* L = LuaBinding::getLuaState();
    ScriptMessageMask mask = 0;
    for (const ScriptEntry& entry : scripts->scripts){
        if (!entry.instance){
            continue;
        }
        if (entry.type == ScriptType::LUA){
            if (!L) continue;
            for (uint32_t m = 0; m < (uint32_t)ScriptMessage::Count; m++){
                if (!(mask & (1u << m)) && luaInstanceDefines(L, entry.instance, methodName((ScriptMessage)m))){
                    mask |= 1u << m;
                }
            }
        }else{
            mask |= entry.cppMessageMask;
        }
    }
    return mask;
}

int ScriptMessages::send(Scene* scene, Entity entity, ScriptMessage m,
                         const std::function<int(lua_State*)>& pushArgs,
                         const std::function<void(ScriptBase&)>& callCpp){
    if (!scene || !scene->isEntityCreated(entity)){
        return 0;
    }
    const char* method = methodName(m);
    lua_State* L = LuaBinding::getLuaState();
    int called = 0;

    // Lua first, then C++, each in the ScriptComponent order; read again after every call
    for (int pass = 0; pass < 2; pass++){
        ScriptComponent* scripts = scene->findComponent<ScriptComponent>(entity);
        for (size_t s = 0; scripts && s < scripts->scripts.size(); s++){
            const ScriptEntry& entry = scripts->scripts[s];
            if (!entry.instance){
                continue;
            }
            if (pass == 0 && entry.type == ScriptType::LUA && L){
                const std::string className = entry.className;
                int ref = static_cast<int>(reinterpret_cast<intptr_t>(entry.instance));
                lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
                if (lua_istable(L, -1)){
                    lua_getfield(L, -1, method);
                    if (lua_isfunction(L, -1)){
                        lua_pushvalue(L, -2);
                        const int top = lua_gettop(L);
                        int nargs = pushArgs ? pushArgs(L) : 0;
                        if (lua_gettop(L) != top + nargs){
                            nargs = lua_gettop(L) - top;
                        }
                        called++;
                        if (LuaBinding::pcallWithTraceback(L, 1 + nargs, 0) != LUA_OK){
                            Log::error("Lua %s() failed for '%s': %s", method, className.c_str(), LuaBinding::getLuaStackErrorString(L, -1).c_str());
                            lua_pop(L, 1);
                        }
                    }else{
                        lua_pop(L, 1);
                    }
                }
                lua_pop(L, 1);
            }else if (pass == 1 && entry.type != ScriptType::LUA && entry.cppScript &&
                      (entry.cppMessageMask & scriptMessageBit(m)) && callCpp){
                called++;
                try{
                    callCpp(*entry.cppScript);
                }catch (const std::exception& e){
                    Log::error("C++ %s() failed for '%s': %s", method, entry.className.c_str(), e.what());
                }catch (...){
                    Log::error("C++ %s() failed for '%s'", method, entry.className.c_str());
                }
            }
            if (!scene->isEntityCreated(entity)){
                return called;
            }
            scripts = scene->findComponent<ScriptComponent>(entity);
        }
    }
    return called;
}

void ScriptMessages::addReceiversChangedListener(ReceiversChanged listener){
    auto& list = listeners();
    if (listener && std::find(list.begin(), list.end(), listener) == list.end()){
        list.push_back(listener);
    }
}

void ScriptMessages::removeReceiversChangedListener(ReceiversChanged listener){
    auto& list = listeners();
    list.erase(std::remove(list.begin(), list.end(), listener), list.end());
}

void ScriptMessages::notifyReceiversChanged(Scene* scene, Entity entity){
    for (ReceiversChanged listener : listeners()){
        listener(scene, entity);
    }
}
