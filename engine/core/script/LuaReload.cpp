// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LuaReload.h"

#include "LuaBinding.h"
#include "Log.h"
#include "Scene.h"
#include "component/ScriptComponent.h"
#include "util/SubscriptionOwnerIndex.h"

#include "lua.hpp"
#include "LuaBridge.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>
#include <unordered_map>

using namespace jasperengine;

namespace {

    // registry key of the weak-keyed table instance -> {registration...}
    const char* const REGISTRATIONS_KEY = "jasperengine.scriptRegistrations";

    std::unordered_map<std::string, std::shared_ptr<lua_reload::ChunkGeneration>>& generations(){
        static auto* table = new std::unordered_map<std::string, std::shared_ptr<lua_reload::ChunkGeneration>>();
        return *table;
    }

    std::shared_ptr<lua_reload::ChunkGeneration>& generationOf(const std::string& source){
        std::shared_ptr<lua_reload::ChunkGeneration>& generation = generations()[source];
        if (!generation) generation = std::make_shared<lua_reload::ChunkGeneration>();
        return generation;
    }

    // Pushes the instance -> registrations table (made on first use)
    void pushRegistrationTable(lua_State* L){
        if (lua_getfield(L, LUA_REGISTRYINDEX, REGISTRATIONS_KEY) == LUA_TTABLE) return;
        lua_pop(L, 1);
        lua_newtable(L);
        lua_newtable(L);
        lua_pushstring(L, "k");
        lua_setfield(L, -2, "__mode");
        lua_setmetatable(L, -2);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, REGISTRATIONS_KEY);
    }

    // Pushes self's list of registrations (an array table), or nil
    void pushRegistrationList(lua_State* L, int selfIndex, bool create){
        selfIndex = lua_absindex(L, selfIndex);
        pushRegistrationTable(L);
        lua_pushvalue(L, selfIndex);
        lua_rawget(L, -2);
        if (lua_isnil(L, -1) && create) {
            lua_pop(L, 1);
            lua_newtable(L);
            lua_pushvalue(L, selfIndex);
            lua_pushvalue(L, -2);
            lua_rawset(L, -4);
        }
        lua_remove(L, -2);
    }

    std::string firstLine(const std::string& text){
        const size_t end = text.find('\n');
        return end == std::string::npos ? text : text.substr(0, end);
    }

    void fail(lua_reload::Report& report, lua_State* L, const std::string& what){
        const std::string message = LuaBinding::getLuaStackErrorString(L, -1);
        lua_pop(L, 1);
        report.keptOld = true;
        report.error = firstLine(message);
        if (!what.empty() && report.error.find(report.path) == std::string::npos) report.error = report.path + ": " + what + ": " + report.error;
        report.errorLine = lua_reload::errorLine(message, report.path);
    }

    int instanceRef(const ScriptEntry& entry){
        return static_cast<int>(reinterpret_cast<intptr_t>(entry.instance));
    }

    bool sameStorage(ScriptPropertyType a, ScriptPropertyType b){
        auto storage = [](ScriptPropertyType type) {
            if (type == ScriptPropertyType::Color3) return ScriptPropertyType::Vector3;
            if (type == ScriptPropertyType::Color4) return ScriptPropertyType::Vector4;
            return type;
        };
        return storage(a) == storage(b);
    }

    // The runtime value at the top of the stack converted to `type` (replacing it); false when
    // it cannot be (the default applies)
    bool convertTop(lua_State* L, ScriptPropertyType from, ScriptPropertyType to){
        if (sameStorage(from, to)) return true;
        const int top = lua_gettop(L);
        if (lua_isnil(L, top)) return false;
        switch (to) {
            case ScriptPropertyType::Float:
                if (lua_type(L, top) == LUA_TNUMBER) {
                    const lua_Number value = lua_tonumber(L, top);
                    lua_pop(L, 1);
                    lua_pushnumber(L, value);
                    return true;
                }
                if (lua_isboolean(L, top)) {
                    const bool value = lua_toboolean(L, top);
                    lua_pop(L, 1);
                    lua_pushnumber(L, value ? 1.0 : 0.0);
                    return true;
                }
                return false;
            case ScriptPropertyType::Int:
                if (lua_type(L, top) == LUA_TNUMBER) {
                    const lua_Number value = lua_tonumber(L, top);
                    lua_pop(L, 1);
                    // half away from zero, like the numeric fields
                    lua_pushinteger(L, static_cast<lua_Integer>(std::round(value)));
                    return true;
                }
                if (lua_isboolean(L, top)) {
                    const bool value = lua_toboolean(L, top);
                    lua_pop(L, 1);
                    lua_pushinteger(L, value ? 1 : 0);
                    return true;
                }
                return false;
            case ScriptPropertyType::Bool:
                if (lua_type(L, top) == LUA_TNUMBER) {
                    const bool value = lua_tonumber(L, top) != 0.0;
                    lua_pop(L, 1);
                    lua_pushboolean(L, value);
                    return true;
                }
                return false;
            case ScriptPropertyType::String:
                if (lua_type(L, top) == LUA_TNUMBER || lua_isboolean(L, top)) {
                    luaL_tolstring(L, top, nullptr);
                    lua_remove(L, top);
                    return true;
                }
                return false;
            case ScriptPropertyType::Vector2:
            case ScriptPropertyType::Vector3:
            case ScriptPropertyType::Color3:
            case ScriptPropertyType::Vector4:
            case ScriptPropertyType::Color4: {
                float v[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                int count = 0;
                if (auto value = luabridge::get<Vector2>(L, top); value && (from == ScriptPropertyType::Vector2)) {
                    v[0] = value.value().x; v[1] = value.value().y; count = 2;
                } else if (auto value3 = luabridge::get<Vector3>(L, top); value3 && (from == ScriptPropertyType::Vector3 || from == ScriptPropertyType::Color3)) {
                    v[0] = value3.value().x; v[1] = value3.value().y; v[2] = value3.value().z; count = 3;
                } else if (auto value4 = luabridge::get<Vector4>(L, top); value4 && (from == ScriptPropertyType::Vector4 || from == ScriptPropertyType::Color4)) {
                    v[0] = value4.value().x; v[1] = value4.value().y; v[2] = value4.value().z; v[3] = value4.value().w; count = 4;
                }
                if (count == 0) return false;
                // a colour that gains alpha is opaque
                if ((to == ScriptPropertyType::Color4) && count < 4) v[3] = 1.0f;
                lua_pop(L, 1);
                bool pushed = false;
                if (to == ScriptPropertyType::Vector2) pushed = static_cast<bool>(luabridge::push<Vector2>(L, Vector2(v[0], v[1])));
                else if (to == ScriptPropertyType::Vector3 || to == ScriptPropertyType::Color3) pushed = static_cast<bool>(luabridge::push<Vector3>(L, Vector3(v[0], v[1], v[2])));
                else pushed = static_cast<bool>(luabridge::push<Vector4>(L, Vector4(v[0], v[1], v[2], v[3])));
                if (!pushed) lua_pushnil(L);
                return pushed;
            }
            default:
                return false;
        }
    }

    const ScriptProperty* findProperty(const std::vector<ScriptProperty>& properties, const std::string& name){
        for (const ScriptProperty& property : properties) {
            if (property.name == name) return &property;
        }
        return nullptr;
    }

    // A property's declared default for an instance that never had it
    ScriptProperty withDefault(const ScriptProperty& property){
        ScriptProperty result = property;
        if (!std::holds_alternative<std::monostate>(property.defaultValue)) result.value = property.defaultValue;
        return result;
    }

    void addUnique(std::vector<std::string>& list, const std::string& value){
        if (std::find(list.begin(), list.end(), value) == list.end()) list.push_back(value);
    }

    void pushStringArray(lua_State* L, const std::vector<std::string>& values){
        lua_createtable(L, static_cast<int>(values.size()), 0);
        for (size_t i = 0; i < values.size(); i++) {
            lua_pushstring(L, values[i].c_str());
            lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
        }
    }

}

// ------------------------------------------------------------------ bookkeeping

lua_reload::CallbackRecord::CallbackRecord(std::shared_ptr<ChunkGeneration> generation): generation(std::move(generation)){
    this->generation->live.fetch_add(1, std::memory_order_relaxed);
}

lua_reload::CallbackRecord::~CallbackRecord(){
    generation->live.fetch_sub(1, std::memory_order_relaxed);
}

std::shared_ptr<lua_reload::CallbackRecord> lua_reload::callbackRecordForFunction(lua_State* L){
    if (!L || !lua_isfunction(L, -1) || lua_iscfunction(L, -1)) return nullptr;
    lua_Debug ar{};
    lua_pushvalue(L, -1);
    if (!lua_getinfo(L, ">S", &ar) || !ar.source || ar.source[0] != '@') return nullptr;
    return std::make_shared<CallbackRecord>(generationOf(ar.source));
}

void lua_reload::clearGenerations(){
    generations().clear();
}

std::string lua_reload::chunkName(std::string_view path){
    return "@" + std::string(path);
}

void lua_reload::recordRegistration(lua_State* L, int selfIndex, int eventIndex, const char* method, const char* tag, bool guarded){
    selfIndex = lua_absindex(L, selfIndex);
    eventIndex = lua_absindex(L, eventIndex);
    pushRegistrationList(L, selfIndex, true);
    const int list = lua_gettop(L);

    // the same tag replaces (FunctionSubscribe replaces the subscriber too). By tag alone:
    // an engine event is a new userdata on every access (Engine.onUpdate), so the event
    // values of two registrations on it never compare equal
    lua_Integer slot = static_cast<lua_Integer>(lua_rawlen(L, list)) + 1;
    for (lua_Integer i = 1; i < slot; i++) {
        lua_rawgeti(L, list, i);
        lua_getfield(L, -1, "tag");
        const bool same = lua_isstring(L, -1) && std::string(lua_tostring(L, -1)) == tag;
        lua_pop(L, 2);
        if (same) {
            slot = i;
            break;
        }
    }

    lua_createtable(L, 0, 4);
    lua_pushvalue(L, eventIndex);
    lua_setfield(L, -2, "event");
    lua_pushstring(L, method);
    lua_setfield(L, -2, "method");
    lua_pushstring(L, tag);
    lua_setfield(L, -2, "tag");
    lua_pushboolean(L, guarded);
    lua_setfield(L, -2, "guarded");
    lua_rawseti(L, list, slot);
    lua_pop(L, 1);
}

std::vector<lua_reload::Registration> lua_reload::registrations(lua_State* L, int selfIndex){
    std::vector<Registration> out;
    pushRegistrationList(L, selfIndex, false);
    if (lua_istable(L, -1)) {
        const lua_Integer count = static_cast<lua_Integer>(lua_rawlen(L, -1));
        for (lua_Integer i = 1; i <= count; i++) {
            lua_rawgeti(L, -1, i);
            Registration item;
            lua_getfield(L, -1, "method");
            item.method = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
            lua_pop(L, 1);
            lua_getfield(L, -1, "tag");
            item.tag = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
            lua_pop(L, 1);
            lua_getfield(L, -1, "guarded");
            item.guarded = lua_toboolean(L, -1) != 0;
            lua_pop(L, 2);
            out.push_back(std::move(item));
        }
    }
    lua_pop(L, 1);
    return out;
}

int lua_reload::errorLine(const std::string& message, const std::string& path){
    if (path.empty()) return 0;
    // "path:12:" or [string "path"]:12:
    for (size_t at = message.find(path); at != std::string::npos; at = message.find(path, at + 1)) {
        size_t after = at + path.size();
        if (after < message.size() && message[after] == '"') {
            after++;
            if (after < message.size() && message[after] == ']') after++;
        }
        if (after >= message.size() || message[after] != ':') continue;
        size_t digits = after + 1;
        int line = 0;
        while (digits < message.size() && std::isdigit(static_cast<unsigned char>(message[digits]))) {
            line = line * 10 + (message[digits] - '0');
            digits++;
        }
        if (digits > after + 1 && digits < message.size() && message[digits] == ':') return line;
    }
    return 0;
}

// ------------------------------------------------------------------ scripts

lua_reload::Report lua_reload::reloadScript(lua_State* L, const std::string& path, std::string_view code,
                                            const std::vector<ScriptInstanceRef>& instances, const std::vector<ScriptProperty>& newProps){
    Report report;
    report.path = path;
    if (!L) {
        report.keptOld = true;
        report.error = path + ": the Lua state is not running";
        return report;
    }
    const int base = lua_gettop(L);
    const std::string source = chunkName(path);

    // 1. compile
    if (luaL_loadbuffer(L, code.data(), code.size(), source.c_str()) != LUA_OK) {
        fail(report, L, "");
        lua_settop(L, base);
        return report;
    }
    const int chunk = lua_gettop(L);

    // what the new code makes from here on is a generation of its own; on failure it is the
    // one dropped and the old one stays current
    std::shared_ptr<ChunkGeneration>& current = generationOf(source);
    const std::shared_ptr<ChunkGeneration> oldGeneration = current;
    current = std::make_shared<ChunkGeneration>();

    // 2. one module table per instance (as at creation); the first run is the probe
    const size_t runs = std::max<size_t>(instances.size(), 1);
    for (size_t i = 0; i < runs; i++) {
        lua_pushvalue(L, chunk);
        if (LuaBinding::pcallWithTraceback(L, 0, 1) != LUA_OK) {
            fail(report, L, "error while loading");
            break;
        }
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            report.keptOld = true;
            report.error = path + ": the script must return a table (its module)";
            break;
        }
    }
    if (report.keptOld) {
        current->dropped = true;
        current = oldGeneration;
        lua_settop(L, base);
        return report;
    }
    const int firstModule = chunk + 1;

    // 3. the swap. The old code's anonymous callbacks and timers are counted, then dropped.
    report.droppedCallbacks = oldGeneration->live.load(std::memory_order_relaxed);
    oldGeneration->dropped = true;
    LuaBinding::setScriptChunk(L, path, chunk);

    std::set<std::string> missingWarned;
    for (size_t i = 0; i < instances.size(); i++) {
        const ScriptInstanceRef& instance = instances[i];
        ScriptEntry* entry = instance.entry;
        if (!entry || !entry->instance) continue;
        const int ref = instanceRef(*entry);
        const int module = firstModule + static_cast<int>(i);

        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        const int self = lua_gettop(L);
        if (!lua_istable(L, self)) {
            lua_settop(L, self - 1);
            continue;
        }

        // the named registrations, before the owner release removes their subscriptions
        std::vector<Registration> recorded = registrations(L, self);
        pushRegistrationList(L, self, false);
        const int recordList = lua_gettop(L);
        LuaBinding::removeScriptSubscriptions(ref);
        // the release ended them all: what is replayed below is recorded again
        pushRegistrationTable(L);
        lua_pushvalue(L, self);
        lua_pushnil(L);
        lua_rawset(L, -3);
        lua_pop(L, 1);

        // new code
        if (lua_getmetatable(L, self)) {
            lua_pushvalue(L, module);
            lua_setfield(L, -2, "__index");
            lua_pop(L, 1);
        } else {
            lua_newtable(L);
            lua_pushvalue(L, module);
            lua_setfield(L, -2, "__index");
            lua_setmetatable(L, self);
        }

        // properties, by name
        std::vector<ScriptProperty> merged;
        merged.reserve(newProps.size());
        for (const ScriptProperty& newProp : newProps) {
            const ScriptProperty* oldProp = findProperty(entry->properties, newProp.name);
            ScriptProperty property = newProp;
            property.luaRef = ref;
            property.memberPtr = nullptr;
            property.ownedInstance = nullptr;
            if (oldProp) {
                if (oldProp->type == newProp.type) {
                    // the runtime value stays in the table; the entry reads it below
                    property.value = oldProp->value;
                } else {
                    lua_getfield(L, self, newProp.name.c_str());
                    if (convertTop(L, oldProp->type, newProp.type)) {
                        lua_setfield(L, self, newProp.name.c_str());
                        addUnique(report.converted, newProp.name);
                    } else {
                        lua_pop(L, 1);
                        property = withDefault(newProp);
                        property.luaRef = ref;
                        LuaBinding::pushScriptPropertyValue(L, instance.scene, property);
                        lua_setfield(L, self, newProp.name.c_str());
                        addUnique(report.reset, newProp.name);
                    }
                }
            } else {
                property = withDefault(newProp);
                property.luaRef = ref;
                LuaBinding::pushScriptPropertyValue(L, instance.scene, property);
                lua_setfield(L, self, newProp.name.c_str());
                addUnique(report.added, newProp.name);
            }
            merged.push_back(std::move(property));
        }
        for (const ScriptProperty& oldProp : entry->properties) {
            if (findProperty(newProps, oldProp.name)) continue;
            lua_pushnil(L);
            lua_setfield(L, self, oldProp.name.c_str());
            addUnique(report.removed, oldProp.name);
        }
        for (ScriptProperty& property : merged) {
            // EntityReference values are the edit-time ones (the table holds the resolved object)
            if (property.type != ScriptPropertyType::EntityReference) property.syncFromMember();
        }
        entry->properties = std::move(merged);

        // the named registrations, against the new methods
        if (lua_istable(L, recordList)) {
            const lua_Integer count = static_cast<lua_Integer>(lua_rawlen(L, recordList));
            for (lua_Integer r = 1; r <= count && static_cast<size_t>(r) <= recorded.size(); r++) {
                const Registration& registration = recorded[static_cast<size_t>(r - 1)];
                lua_pushvalue(L, self);
                lua_getfield(L, -1, registration.method.c_str());
                const bool exists = lua_isfunction(L, -1);
                lua_pop(L, 2);
                if (!exists) {
                    if (missingWarned.insert(registration.method).second) {
                        addUnique(report.missingMethods, registration.method);
                        Log::warn("%s: method '%s' is gone after the reload, its event registration was dropped",
                                  path.c_str(), registration.method.c_str());
                    }
                    continue;
                }
                lua_rawgeti(L, recordList, r);
                lua_getfield(L, -1, "event");
                const int event = lua_gettop(L);
                std::string error;
                if (LuaBinding::replayScriptEvent(L, event, self, registration.method, registration.tag, registration.guarded, error)) {
                    report.replayedCallbacks++;
                } else {
                    report.warnings.push_back(path + ": could not register '" + registration.method + "' again: " + firstLine(error));
                }
                lua_settop(L, recordList);
            }
        }
        lua_settop(L, self);
        report.instances++;
        lua_settop(L, self - 1);
    }

    // 4. onReload(info), after every instance has the new code
    for (const ScriptInstanceRef& instance : instances) {
        if (!instance.entry || !instance.entry->instance) continue;
        lua_rawgeti(L, LUA_REGISTRYINDEX, instanceRef(*instance.entry));
        const int self = lua_gettop(L);
        if (lua_istable(L, self)) {
            lua_getfield(L, self, "onReload");
            if (lua_isfunction(L, -1)) {
                lua_pushvalue(L, self);
                lua_createtable(L, 0, 5);
                lua_pushstring(L, path.c_str());
                lua_setfield(L, -2, "path");
                pushStringArray(L, report.added);
                lua_setfield(L, -2, "addedProperties");
                pushStringArray(L, report.removed);
                lua_setfield(L, -2, "removedProperties");
                std::vector<std::string> changed = report.converted;
                changed.insert(changed.end(), report.reset.begin(), report.reset.end());
                pushStringArray(L, changed);
                lua_setfield(L, -2, "changedProperties");
                lua_pushinteger(L, report.droppedCallbacks);
                lua_setfield(L, -2, "droppedCallbacks");
                if (LuaBinding::pcallWithTraceback(L, 2, 0) != LUA_OK) {
                    LuaBinding::logLuaError("Lua onReload() failed for '%s': %s", path.c_str(), LuaBinding::getLuaStackErrorString(L, -1).c_str());
                }
            }
        }
        lua_settop(L, self - 1);
    }

    lua_settop(L, base);
    return report;
}

// ------------------------------------------------------------------ modules

bool lua_reload::isModuleLoaded(lua_State* L, const std::string& moduleName){
    if (!L) return false;
    lua_getfield(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
    lua_getfield(L, -1, moduleName.c_str());
    const bool loaded = !lua_isnil(L, -1);
    lua_pop(L, 2);
    return loaded;
}

lua_reload::Report lua_reload::reloadModule(lua_State* L, const std::string& moduleName, const std::string& path, std::string_view code){
    Report report;
    report.path = path;
    if (!L) {
        report.keptOld = true;
        report.error = path + ": the Lua state is not running";
        return report;
    }
    const int base = lua_gettop(L);
    lua_getfield(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
    const int loaded = lua_gettop(L);
    lua_getfield(L, loaded, moduleName.c_str());
    const int old = lua_gettop(L);
    if (lua_isnil(L, old)) {
        // nobody required it: the next require reads the file
        lua_settop(L, base);
        return report;
    }

    const std::string source = chunkName(path);
    if (luaL_loadbuffer(L, code.data(), code.size(), source.c_str()) != LUA_OK) {
        fail(report, L, "");
        lua_settop(L, base);
        return report;
    }
    // require's arguments: the module name and where it was found
    lua_pushstring(L, moduleName.c_str());
    lua_pushstring(L, path.c_str());
    if (LuaBinding::pcallWithTraceback(L, 2, 1) != LUA_OK) {
        fail(report, L, "error while loading");
        lua_settop(L, base);
        return report;
    }
    const int fresh = lua_gettop(L);
    report.instances = 1;

    if (!lua_istable(L, fresh) || !lua_istable(L, old)) {
        // require stores true for a module returning nothing
        if (lua_isnil(L, fresh)) {
            lua_pop(L, 1);
            lua_pushboolean(L, 1);
        }
        lua_pushvalue(L, -1);
        lua_setfield(L, loaded, moduleName.c_str());
        report.warnings.push_back(path + ": the module did not return a table; package.loaded[\"" + moduleName +
                                  "\"] was replaced, holders of the old value keep it");
        lua_settop(L, base);
        return report;
    }

    // removed functions first (the new table is read below)
    std::vector<std::string> stale;
    lua_pushnil(L);
    while (lua_next(L, old) != 0) {
        if (lua_type(L, -2) == LUA_TSTRING && lua_isfunction(L, -1)) {
            lua_pushvalue(L, -2);
            lua_rawget(L, fresh);
            if (lua_isnil(L, -1)) stale.push_back(lua_tostring(L, -3));
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    for (const std::string& name : stale) {
        lua_pushnil(L);
        lua_setfield(L, old, name.c_str());
        report.removed.push_back(name);
    }

    lua_pushnil(L);
    while (lua_next(L, fresh) != 0) {
        // key at -2, value at -1
        const bool isFunction = lua_isfunction(L, -1);
        lua_pushvalue(L, -2);
        lua_rawget(L, old);
        const bool existed = !lua_isnil(L, -1);
        lua_pop(L, 1);
        if (isFunction || !existed) {
            if (!existed && lua_type(L, -2) == LUA_TSTRING) report.added.push_back(lua_tostring(L, -2));
            lua_pushvalue(L, -2);
            lua_pushvalue(L, -2);
            lua_rawset(L, old);
        }
        lua_pop(L, 1);
    }
    std::sort(report.added.begin(), report.added.end());
    std::sort(report.removed.begin(), report.removed.end());

    lua_settop(L, base);
    return report;
}
