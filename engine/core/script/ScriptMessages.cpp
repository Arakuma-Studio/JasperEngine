// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ScriptMessages.h"

#include <cmath>
#include "LuaBinding.h"
#include "ScriptRuntime.h"
#include "Scene.h"
#include "Log.h"
#include "component/ScriptComponent.h"

#include "lua.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <exception>
#include <set>
#include <string>
#include <unordered_map>

using namespace jasperengine;

namespace {
    // receivers() per scene and GameObject, dropped by notifyReceiversChanged
    std::unordered_map<Scene*, std::unordered_map<Entity, ScriptMessageMask>> receiverCache;
    std::vector<ScriptMessages::ReceiversChanged> receiverListeners;
    // D4's collision rule: Lua instance tables, the methods they subscribed to UIComponent
    // events and a probe of each subscription (true while it lives)
    std::unordered_map<const void*, std::unordered_map<std::string, std::vector<std::function<bool()>>>> legacySubscriptions;

    int luaRefOf(const ScriptEntry& entry) {
        return static_cast<int>(reinterpret_cast<intptr_t>(entry.instance));
    }

    // initialized (the GameObject was active once) and not destroyed; an instance the runtime
    // never saw (made by hand, or before its scene's lifecycle starts) gets its messages
    bool live(const ScriptEntry& entry) {
        const ScriptState* state = ScriptRuntime::getState(entry.instance);
        return !state || (state->initialized && !state->destroyed);
    }

    bool destroyed(const ScriptEntry& entry) {
        const ScriptState* state = ScriptRuntime::getState(entry.instance);
        return state && state->destroyed;
    }

    // The contact messages run every fixed step: an error in one is logged once a second per
    // script class and message (physics-contacts spec 7.13), the others every time
    bool reportError(ScriptMessage m, const std::string& className) {
        if (static_cast<int>(m) > static_cast<int>(ScriptMessage::TriggerExit2D)) {
            return true;
        }
        static std::unordered_map<std::string, std::chrono::steady_clock::time_point> lastReports;
        const auto now = std::chrono::steady_clock::now();
        const std::string key = className + "." + ScriptMessages::methodName(m);
        auto last = lastReports.find(key);
        if (last != lastReports.end() && now - last->second < std::chrono::seconds(1)) {
            return false;
        }
        lastReports[key] = now;
        return true;
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
        case ScriptMessage::ParticleSystemStopped: return "onParticleSystemStopped";
        case ScriptMessage::PointerDown: return "onPointerDown";
        case ScriptMessage::PointerUp: return "onPointerUp";
        case ScriptMessage::PointerClick: return "onPointerClick";
        case ScriptMessage::PointerEnter: return "onPointerEnter";
        case ScriptMessage::PointerExit: return "onPointerExit";
        case ScriptMessage::InitializePotentialDrag: return "onInitializePotentialDrag";
        case ScriptMessage::BeginDrag: return "onBeginDrag";
        case ScriptMessage::Drag: return "onDrag";
        case ScriptMessage::EndDrag: return "onEndDrag";
        case ScriptMessage::Drop: return "onDrop";
        case ScriptMessage::Scroll: return "onScroll";
        case ScriptMessage::Select: return "onSelect";
        case ScriptMessage::Deselect: return "onDeselect";
        case ScriptMessage::Move: return "onMove";
        case ScriptMessage::Submit: return "onSubmit";
        case ScriptMessage::Cancel: return "onCancel";
        case ScriptMessage::Count: break;
    }
    return "";
}

bool ScriptMessages::fromMethodName(const char* name, ScriptMessage& out) {
    if (!name) {
        return false;
    }
    for (int m = 0; m < static_cast<int>(ScriptMessage::Count); m++) {
        if (std::strcmp(methodName(static_cast<ScriptMessage>(m)), name) == 0) {
            out = static_cast<ScriptMessage>(m);
            return true;
        }
    }
    return false;
}

bool ScriptMessages::defines(Scene* scene, Entity entity, ScriptMessage m) {
    if (!scene || !scene->isEntityCreated(entity)) {
        return false;
    }
    ScriptComponent* scripts = scene->findComponent<ScriptComponent>(entity);
    if (!scripts) {
        return false;
    }
    const char* method = methodName(m);
    lua_State* L = LuaBinding::getLuaState();
    for (const ScriptEntry& entry : scripts->scripts) {
        if (!entry.instance || !entry.enabled || !live(entry)) {
            continue;
        }
        if (entry.type == ScriptType::LUA) {
            if (!L) {
                continue;
            }
            const int top = lua_gettop(L);
            lua_rawgeti(L, LUA_REGISTRYINDEX, luaRefOf(entry));
            const void* table = lua_topointer(L, -1);
            LuaBinding::protectedGetField(L, -1, method);
            const bool found = lua_isfunction(L, -1) && !isLegacySubscription(table, method);
            lua_settop(L, top);
            if (found) {
                return true;
            }
        } else if (entry.cppScript && (entry.cppMessageMask & ~entry.cppScript->inheritedMessages & scriptMessageBit(m))) {
            return true;
        }
    }
    return false;
}

void ScriptMessages::noteLegacySubscription(const void* instanceTable, const std::string& method,
                                            std::function<bool()> subscriptionAlive) {
    if (instanceTable && subscriptionAlive) {
        legacySubscriptions[instanceTable][method].push_back(std::move(subscriptionAlive));
    }
}

bool ScriptMessages::isLegacySubscription(const void* instanceTable, const char* method) {
    auto it = legacySubscriptions.find(instanceTable);
    if (it == legacySubscriptions.end() || !method) {
        return false;
    }
    auto methodIt = it->second.find(method);
    if (methodIt == it->second.end()) {
        return false;
    }
    // the subscriptions that went (UnregisterEvent, the event cleared, its component destroyed)
    std::vector<std::function<bool()>>& probes = methodIt->second;
    probes.erase(std::remove_if(probes.begin(), probes.end(),
        [](const std::function<bool()>& alive) { return !alive(); }), probes.end());
    if (!probes.empty()) {
        return true;
    }
    it->second.erase(methodIt);
    if (it->second.empty()) {
        legacySubscriptions.erase(it);
    }
    return false;
}

void ScriptMessages::forgetLegacySubscriptions(const void* instanceTable) {
    legacySubscriptions.erase(instanceTable);
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
                    // protected: a class's own __index may raise for what it does not define
                    LuaBinding::protectedGetField(L, -1, methodName(static_cast<ScriptMessage>(m)));
                    if (lua_isfunction(L, -1)) {
                        mask |= scriptMessageBit(static_cast<ScriptMessage>(m));
                    }
                    lua_pop(L, 1);
                }
                lua_pop(L, 1);
            } else if (entry.cppScript) {
                // less what the script was seen not to override
                mask |= entry.cppMessageMask & ~entry.cppScript->inheritedMessages;
            }
        }
    }
    sceneCache[entity] = mask;
    return mask;
}

std::vector<std::string> ScriptMessages::describeReceivers(Scene* scene, Entity entity, ScriptMessageMask filter) {
    std::vector<std::string> names;
    if (!scene || !scene->isEntityCreated(entity)) {
        return names;
    }
    ScriptComponent* scripts = scene->findComponent<ScriptComponent>(entity);
    lua_State* L = LuaBinding::getLuaState();
    if (!scripts) {
        return names;
    }
    for (const ScriptEntry& entry : scripts->scripts) {
        if (!entry.instance || destroyed(entry)) {
            continue;
        }
        for (int m = 0; m < static_cast<int>(ScriptMessage::Count); m++) {
            if (!(filter & (1u << m))) {
                continue;
            }
            bool defines = false;
            if (entry.type == ScriptType::LUA) {
                if (!L) continue;
                lua_rawgeti(L, LUA_REGISTRYINDEX, luaRefOf(entry));
                if (lua_istable(L, -1)) {
                    LuaBinding::protectedGetField(L, -1, methodName(static_cast<ScriptMessage>(m)));
                    defines = lua_isfunction(L, -1);
                    lua_pop(L, 1);
                }
                lua_pop(L, 1);
            } else if (entry.cppScript) {
                defines = (entry.cppMessageMask & ~entry.cppScript->inheritedMessages & (1u << m)) != 0;
            }
            if (defines) {
                names.push_back(std::string(methodName(static_cast<ScriptMessage>(m))) + " (" + entry.className + ")");
            }
        }
    }
    return names;
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
        LuaBinding::protectedGetField(L, -1, method);
        if (!lua_isfunction(L, -1) || isLegacySubscription(lua_topointer(L, -2), method)) {
            lua_settop(L, top);
            return false;
        }
        lua_insert(L, -2); // method, self
        const std::string entityName = scene->getEntityName(entity);
        const int nargs = pushArgs ? pushArgs(L) : 0;
        if (LuaBinding::pcallWithTraceback(L, 1 + nargs, 0) != LUA_OK && reportError(m, className)) {
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
    const bool nested = ScriptBase::baseMessageRan;
    ScriptBase::baseMessageRan = false;
    bool overridden = true;
    try {
        callCpp(*script);
        // the base method ran: not an override (not counted, not sent again; the script may be
        // gone, so only the flag is read)
        overridden = !ScriptBase::baseMessageRan;
    } catch (const std::exception& e) {
        if (reportError(m, className)) Log::error("C++ %s() failed for '%s': %s", method, className.c_str(), e.what());
    } catch (...) {
        if (reportError(m, className)) Log::error("C++ %s() failed for '%s': unknown exception", method, className.c_str());
    }
    ScriptBase::baseMessageRan = nested;
    if (!overridden) {
        // the listeners follow (physics stops recording contacts for a message nobody overrides)
        notifyReceiversChanged(scene, entity);
    }
    return overridden;
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
    // Lua scripts, then C++ ones, each in the component order (read again after every call)
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 0; scene->isEntityCreated(entity); i++) {
            ScriptComponent* scripts = scene->findComponent<ScriptComponent>(entity);
            if (!scripts || i >= scripts->scripts.size()) {
                break;
            }
            const ScriptEntry& entry = scripts->scripts[i];
            if ((entry.type == ScriptType::LUA) != (pass == 0) || !entry.instance || !live(entry)) {
                continue;
            }
            if ((deliver & DeliverRequireEnabled) && !entry.enabled) {
                continue;
            }
            if (invoke(scene, entity, i, m, pushArgs, callCpp)) {
                called++;
            }
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

bool ScriptMessages::sendNamed(Scene* scene, Entity entity, const std::string& method, const std::vector<AnimationEventArg>& args,
                               bool integers) {
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
        LuaBinding::protectedGetField(L, -1, method.c_str());
        if (!lua_isfunction(L, -1)) {
            lua_settop(L, top);
            continue;
        }
        found = true;
        lua_insert(L, -2); // method, self
        for (const AnimationEventArg& arg : args) {
            if (const double* number = std::get_if<double>(&arg)) {
                if (integers && std::floor(*number) == *number && std::fabs(*number) < 9.0e15) lua_pushinteger(L, (lua_Integer)*number);
                else lua_pushnumber(L, *number);
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
