// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ScriptMessages.h"
#include "LuaBinding.h"
#include "ScriptRuntime.h"
#include "Scene.h"
#include "Log.h"
#include "component/ScriptComponent.h"

#include "lua.hpp"

#include <algorithm>
#include <exception>
#include <unordered_map>

using namespace jasperengine;

namespace {
    // receivers() per scene and GameObject, dropped by notifyReceiversChanged
    std::unordered_map<Scene*, std::unordered_map<Entity, ScriptMessageMask>> receiverCache;
    std::vector<ScriptMessages::ReceiversChanged> receiverListeners;

    int luaRefOf(const ScriptEntry& entry) {
        return static_cast<int>(reinterpret_cast<intptr_t>(entry.instance));
    }

    // initialized (the GameObject was active once) and not destroyed; an instance the
    // lifecycle never saw (made by hand, its scene not started) has no such state: it is live
    bool live(const ScriptEntry& entry) {
        const ScriptState* state = ScriptRuntime::getState(entry.instance);
        return !state || (state->initialized && !state->destroyed);
    }

    bool destroyed(const ScriptEntry& entry) {
        const ScriptState* state = ScriptRuntime::getState(entry.instance);
        return state && state->destroyed;
    }

    bool mainThread() {
        if (LuaBinding::isLuaThread()) {
            return true;
        }
        Log::error("Script messages are sent on the main thread only");
        return false;
    }
}

const char* ScriptMessages::methodName(ScriptMessage m) {
    switch (m) {
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
        case ScriptMessage::Init: return "init";
        case ScriptMessage::Start: return "start";
        case ScriptMessage::Enable: return "onEnable";
        case ScriptMessage::Disable: return "onDisable";
        case ScriptMessage::Destroy: return "onDestroy";
        case ScriptMessage::ApplicationPause: return "onApplicationPause";
        case ScriptMessage::ApplicationFocus: return "onApplicationFocus";
        case ScriptMessage::ApplicationQuit: return "onApplicationQuit";
        case ScriptMessage::Count: break;
    }
    return "";
}

ScriptMessageMask ScriptMessages::receivers(Scene* scene, Entity entity) {
    if (!scene || !scene->isEntityCreated(entity)) {
        return 0;
    }
    auto& sceneCache = receiverCache[scene];
    auto cached = sceneCache.find(entity);
    if (cached != sceneCache.end()) {
        return cached->second;
    }
    ScriptMessageMask mask = 0;
    lua_State* L = LuaBinding::getLuaState();
    if (ScriptComponent* scripts = scene->findComponent<ScriptComponent>(entity)) {
        for (const ScriptEntry& entry : scripts->scripts) {
            if (!entry.instance || destroyed(entry)) {
                continue;
            }
            if (entry.type == ScriptType::LUA) {
                if (!L) {
                    continue;
                }
                lua_rawgeti(L, LUA_REGISTRYINDEX, luaRefOf(entry));
                for (int m = 0; m < static_cast<int>(ScriptMessage::Count); m++) {
                    lua_getfield(L, -1, methodName(static_cast<ScriptMessage>(m)));
                    if (lua_isfunction(L, -1)) {
                        mask |= 1u << m;
                    }
                    lua_pop(L, 1);
                }
                lua_pop(L, 1);
            } else if (entry.cppScript) {
                // less what reached the base methods (not overridden)
                mask |= entry.cppMessageMask & ~entry.cppScript->inheritedMessages;
            }
        }
    }
    sceneCache[entity] = mask;
    return mask;
}

// One entry, no delivery rules (ScriptRuntime's own checks, or send/sendTo's)
bool ScriptMessages::invoke(Scene* scene, Entity entity, size_t scriptIndex, ScriptMessage m,
                            const std::function<int(lua_State*)>& pushArgs,
                            const std::function<void(ScriptBase&)>& callCpp) {
    ScriptComponent* scripts = scene->findComponent<ScriptComponent>(entity);
    if (!scripts || scriptIndex >= scripts->scripts.size()) {
        return false;
    }
    const ScriptEntry& entry = scripts->scripts[scriptIndex];
    if (!entry.instance) {
        return false;
    }
    const char* method = methodName(m);
    if (entry.type == ScriptType::LUA) {
        lua_State* L = LuaBinding::getLuaState();
        if (!L) {
            return false;
        }
        const std::string className = entry.className;
        const int top = lua_gettop(L);
        lua_rawgeti(L, LUA_REGISTRYINDEX, luaRefOf(entry));
        lua_getfield(L, -1, method);
        if (!lua_isfunction(L, -1)) {
            lua_settop(L, top);
            return false;
        }
        lua_insert(L, -2); // method, self
        const std::string entityName = scene->getEntityName(entity);
        const int nargs = pushArgs ? pushArgs(L) : 0;
        if (LuaBinding::pcallWithTraceback(L, 1 + nargs, 0) != LUA_OK) {
            Log::error("Lua %s() failed for '%s' on '%s': %s", method, className.c_str(), entityName.c_str(),
                       LuaBinding::getLuaStackErrorString(L, -1).c_str());
        }
        lua_settop(L, top);
        return true;
    }
    if (!entry.cppScript || !callCpp || !(entry.cppMessageMask & scriptMessageBit(m)) ||
            (entry.cppScript->inheritedMessages & scriptMessageBit(m))) {
        return false;
    }
    ScriptBase* script = entry.cppScript;
    const std::string className = entry.className;
    ScriptBase::baseMessageRan = false;
    try {
        callCpp(*script);
    } catch (const std::exception& e) {
        Log::error("C++ %s() failed for '%s': %s", method, className.c_str(), e.what());
    } catch (...) {
        Log::error("C++ %s() failed for '%s': unknown exception", method, className.c_str());
    }
    if (ScriptBase::baseMessageRan) {
        // the base method ran (read from the flag, the script may be gone): not an override,
        // not called again, not a receiver
        ScriptBase::baseMessageRan = false;
        auto sceneCache = receiverCache.find(scene);
        if (sceneCache != receiverCache.end()) {
            sceneCache->second.erase(entity);
        }
        return false;
    }
    return true;
}

int ScriptMessages::send(Scene* scene, Entity entity, ScriptMessage m,
                         const std::function<int(lua_State*)>& pushArgs,
                         const std::function<void(ScriptBase&)>& callCpp,
                         uint32_t deliver) {
    if (!scene || !scene->isEntityCreated(entity) || !mainThread()) {
        return 0;
    }
    if ((deliver & DeliverRequireActive) && !scene->isEntityActiveInHierarchy(entity)) {
        return 0;
    }
    int called = 0;
    for (size_t i = 0; scene->isEntityCreated(entity); i++) {
        ScriptComponent* scripts = scene->findComponent<ScriptComponent>(entity);
        if (!scripts || i >= scripts->scripts.size()) {
            break;
        }
        const ScriptEntry& entry = scripts->scripts[i];
        if (!entry.instance || !live(entry)) {
            continue;
        }
        if ((deliver & DeliverRequireEnabled) && !entry.enabled) {
            continue;
        }
        if (invoke(scene, entity, i, m, pushArgs, callCpp)) {
            called++;
        }
    }
    return called;
}

bool ScriptMessages::sendTo(Scene* scene, Entity entity, size_t scriptIndex, ScriptMessage m,
                            const std::function<int(lua_State*)>& pushArgs,
                            const std::function<void(ScriptBase&)>& callCpp) {
    if (!scene || !scene->isEntityCreated(entity) || !mainThread()) {
        return false;
    }
    ScriptComponent* scripts = scene->findComponent<ScriptComponent>(entity);
    if (!scripts || scriptIndex >= scripts->scripts.size()) {
        return false;
    }
    const ScriptEntry& entry = scripts->scripts[scriptIndex];
    if (!entry.instance || !live(entry)) {
        return false;
    }
    return invoke(scene, entity, scriptIndex, m, pushArgs, callCpp);
}

bool ScriptMessages::sendNamed(Scene* scene, Entity entity, const std::string& method, const std::vector<AnimationEventArg>& args) {
    lua_State* L = LuaBinding::getLuaState();
    if (!L || !scene || method.empty() || !scene->isEntityCreated(entity)) {
        return false;
    }
    bool found = false;
    // read again for every script: a method can make GameObjects and move the components
    for (size_t s = 0; scene->isEntityCreated(entity); s++) {
        ScriptComponent* scripts = scene->findComponent<ScriptComponent>(entity);
        if (!scripts || s >= scripts->scripts.size()) {
            break;
        }
        const ScriptEntry& entry = scripts->scripts[s];
        if (entry.type != ScriptType::LUA || !entry.enabled || !entry.instance || destroyed(entry)) {
            continue;
        }
        const std::string className = entry.className;
        const int top = lua_gettop(L);
        lua_rawgeti(L, LUA_REGISTRYINDEX, luaRefOf(entry));
        lua_getfield(L, -1, method.c_str());
        if (!lua_isfunction(L, -1)) {
            lua_settop(L, top);
            continue;
        }
        found = true;
        lua_insert(L, -2); // method, self
        for (const AnimationEventArg& arg : args) {
            if (const double* number = std::get_if<double>(&arg)) {
                lua_pushnumber(L, *number);
            } else if (const std::string* text = std::get_if<std::string>(&arg)) {
                lua_pushlstring(L, text->data(), text->size());
            } else {
                lua_pushboolean(L, std::get<bool>(arg) ? 1 : 0);
            }
        }
        if (LuaBinding::pcallWithTraceback(L, 1 + (int)args.size(), 0) != LUA_OK) {
            Log::error("Lua %s() failed for '%s': %s", method.c_str(), className.c_str(), LuaBinding::getLuaStackErrorString(L, -1).c_str());
        }
        lua_settop(L, top);
    }
    return found;
}

void ScriptMessages::addReceiversChangedListener(ReceiversChanged listener) {
    if (listener && std::find(receiverListeners.begin(), receiverListeners.end(), listener) == receiverListeners.end()) {
        receiverListeners.push_back(listener);
    }
}

void ScriptMessages::removeReceiversChangedListener(ReceiversChanged listener) {
    receiverListeners.erase(std::remove(receiverListeners.begin(), receiverListeners.end(), listener), receiverListeners.end());
}

void ScriptMessages::notifyReceiversChanged(Scene* scene, Entity entity) {
    auto sceneCache = receiverCache.find(scene);
    if (sceneCache != receiverCache.end()) {
        sceneCache->second.erase(entity);
    }
    for (ReceiversChanged listener : receiverListeners) {
        listener(scene, entity);
    }
}

void ScriptMessages::forgetScene(Scene* scene) {
    receiverCache.erase(scene);
}
