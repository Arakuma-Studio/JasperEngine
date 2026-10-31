// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AsyncOwner.h"

#include "Scene.h"
#include "EntityHandle.h"
#include "script/ScriptBase.h"

#include "lua.hpp"
#include "LuaBridge.h"

#include <atomic>
#include <map>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace jasperengine;

namespace {

    enum class OwnerKind { Script, Entity, Scene };

    struct OwnerRecord {
        OwnerKind kind = OwnerKind::Script;
        const void* script = nullptr;               // Script: the instance key
        const EntityRegistry* registry = nullptr;   // Entity, Scene; Script: its GameObject's scene (may be null)
        Entity entity = NULL_ENTITY;                // Entity; Script: its GameObject (may be NULL_ENTITY)
        std::string label;
    };

    struct OwnerState {
        std::mutex mutex;
        uint64_t nextToken = 1;
        std::unordered_map<uint64_t, OwnerRecord> records;
        std::unordered_map<const void*, uint64_t> byScript;
        std::map<std::pair<const EntityRegistry*, Entity>, uint64_t> byEntity;
        std::unordered_map<const EntityRegistry*, uint64_t> byScene;
        // the script owners of a GameObject
        std::multimap<std::pair<const EntityRegistry*, Entity>, uint64_t> scriptsByEntity;
        // the engine hooks run for every destroyed entity: nothing to do while no owner exists
        std::atomic<size_t> count{0};
    };

    OwnerState& ownerState(){
        static OwnerState* state = new OwnerState();
        return *state;
    }

    FunctionSubscribe<void(uint64_t)>& goneEvent(){
        static FunctionSubscribe<void(uint64_t)>* event = new FunctionSubscribe<void(uint64_t)>();
        return *event;
    }

    // with the lock held
    uint64_t addRecord(OwnerState& state, OwnerRecord record){
        const uint64_t token = state.nextToken++;
        switch (record.kind){
            case OwnerKind::Script:
                state.byScript[record.script] = token;
                if (record.registry) state.scriptsByEntity.emplace(std::make_pair(record.registry, record.entity), token);
                break;
            case OwnerKind::Entity: state.byEntity[{record.registry, record.entity}] = token; break;
            case OwnerKind::Scene: state.byScene[record.registry] = token; break;
        }
        state.records.emplace(token, std::move(record));
        state.count.store(state.records.size(), std::memory_order_relaxed);
        return token;
    }

    // with the lock held
    void removeRecord(OwnerState& state, uint64_t token, std::vector<uint64_t>& gone){
        auto it = state.records.find(token);
        if (it == state.records.end()) return;
        const OwnerRecord& record = it->second;
        switch (record.kind){
            case OwnerKind::Script: {
                state.byScript.erase(record.script);
                auto range = state.scriptsByEntity.equal_range({record.registry, record.entity});
                for (auto link = range.first; link != range.second; ++link){
                    if (link->second == token){
                        state.scriptsByEntity.erase(link);
                        break;
                    }
                }
                break;
            }
            case OwnerKind::Entity: state.byEntity.erase({record.registry, record.entity}); break;
            case OwnerKind::Scene: state.byScene.erase(record.registry); break;
        }
        state.records.erase(it);
        state.count.store(state.records.size(), std::memory_order_relaxed);
        gone.push_back(token);
    }

    void fireGone(const std::vector<uint64_t>& gone){
        for (uint64_t token : gone){
            goneEvent().call(token);
        }
    }

    // a token no record has: an owner that is gone already
    uint64_t goneToken(){
        OwnerState& state = ownerState();
        std::lock_guard<std::mutex> lock(state.mutex);
        return state.nextToken++;
    }

    std::string entityLabel(Scene* scene, Entity entity){
        const std::string name = scene ? scene->getEntityName(entity) : std::string();
        return name.empty() ? "GameObject " + std::to_string(entity) : "GameObject '" + name + "'";
    }

    AsyncOwner scriptOwner(const void* instance, Scene* scene, Entity entity, const std::string& className){
        AsyncOwner owner;
        const bool linked = scene && entity != NULL_ENTITY;
        if (!instance || (linked && !scene->isEntityCreated(entity))){
            owner.token = goneToken();
            return owner;
        }
        std::string label = className.empty() ? "script" : "script " + className;
        if (linked) label += " (" + entityLabel(scene, entity) + ")";
        OwnerState& state = ownerState();
        std::lock_guard<std::mutex> lock(state.mutex);
        auto it = state.byScript.find(instance);
        if (it != state.byScript.end()){
            owner.token = it->second;
            return owner;
        }
        OwnerRecord record;
        record.kind = OwnerKind::Script;
        record.script = instance;
        if (linked){
            record.registry = scene;
            record.entity = entity;
        }
        record.label = std::move(label);
        owner.token = addRecord(state, std::move(record));
        return owner;
    }

}

AsyncOwner AsyncOwners::fromEntity(Scene* scene, Entity entity){
    AsyncOwner owner;
    if (!scene || entity == NULL_ENTITY || !scene->isEntityCreated(entity)){
        owner.token = goneToken();
        return owner;
    }
    const EntityRegistry* registry = scene;
    OwnerState& state = ownerState();
    std::lock_guard<std::mutex> lock(state.mutex);
    auto it = state.byEntity.find({registry, entity});
    if (it != state.byEntity.end()){
        owner.token = it->second;
        return owner;
    }
    OwnerRecord record;
    record.kind = OwnerKind::Entity;
    record.registry = registry;
    record.entity = entity;
    record.label = entityLabel(scene, entity);
    owner.token = addRecord(state, std::move(record));
    return owner;
}

AsyncOwner AsyncOwners::fromScene(Scene* scene){
    AsyncOwner owner;
    if (!scene){
        owner.token = goneToken();
        return owner;
    }
    const EntityRegistry* registry = scene;
    OwnerState& state = ownerState();
    std::lock_guard<std::mutex> lock(state.mutex);
    auto it = state.byScene.find(registry);
    if (it != state.byScene.end()){
        owner.token = it->second;
        return owner;
    }
    OwnerRecord record;
    record.kind = OwnerKind::Scene;
    record.registry = registry;
    record.label = "scene";
    owner.token = addRecord(state, std::move(record));
    return owner;
}

AsyncOwner AsyncOwners::fromScriptInstance(const void* instance, Scene* scene, Entity entity){
    return scriptOwner(instance, scene, entity, std::string());
}

AsyncOwner AsyncOwners::fromScript(const ScriptBase* script){
    if (!script) return fromScriptInstance(nullptr);
    return fromScriptInstance(script, script->getScene(), script->getEntity());
}

bool AsyncOwners::fromLua(lua_State* L, int index, AsyncOwner& out, std::string& error){
    out = AsyncOwner();
    index = lua_absindex(L, index);
    const int type = lua_type(L, index);
    if (type == LUA_TNONE || type == LUA_TNIL) return true;

    if (type == LUA_TTABLE){
        // a script instance: LuaBinding::createLuaScriptInstance stores its registry ref in it
        const int top = lua_gettop(L);
        bool isInstance = false;
        int ref = 0;
        lua_pushliteral(L, "__scriptRef");
        lua_rawget(L, index);
        if (lua_isinteger(L, -1)){
            ref = (int)lua_tointeger(L, -1);
            lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
            isInstance = lua_rawequal(L, -1, index) != 0;
        }
        lua_settop(L, top);
        if (isInstance){
            Scene* scene = nullptr;
            Entity entity = NULL_ENTITY;
            std::string className;
            lua_pushliteral(L, "scene");
            lua_rawget(L, index);
            if (luabridge::isInstance<Scene>(L, -1)){
                auto result = luabridge::get<Scene*>(L, -1);
                if (result) scene = result.value();
            }
            lua_pushliteral(L, "entity");
            lua_rawget(L, index);
            if (lua_isinteger(L, -1)) entity = (Entity)lua_tointeger(L, -1);
            lua_pushliteral(L, "__name");
            lua_rawget(L, index);
            if (lua_type(L, -1) == LUA_TSTRING) className = lua_tostring(L, -1);
            lua_settop(L, top);
            const void* key = reinterpret_cast<const void*>(static_cast<intptr_t>(ref));
            out = scriptOwner(key, scene, scene ? entity : NULL_ENTITY, className);
            return true;
        }
    } else if (type == LUA_TUSERDATA){
        if (luabridge::isInstance<EntityHandle>(L, index)){
            auto handle = luabridge::get<EntityHandle*>(L, index);
            if (handle && handle.value()){
                out = fromEntity(handle.value()->getScene(), handle.value()->getEntity());
                return true;
            }
        }
        if (luabridge::isInstance<Scene>(L, index)){
            auto scene = luabridge::get<Scene*>(L, index);
            if (scene && scene.value()){
                out = fromScene(scene.value());
                return true;
            }
        }
    }
    error = std::string("owner must be a script instance (self), a GameObject or a Scene, got ") + luaL_typename(L, index);
    return false;
}

bool AsyncOwners::alive(const AsyncOwner& owner){
    if (owner.token == 0) return true;
    OwnerState& state = ownerState();
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.records.count(owner.token) != 0;
}

std::string AsyncOwners::describe(const AsyncOwner& owner){
    if (owner.token == 0) return "none";
    OwnerState& state = ownerState();
    std::lock_guard<std::mutex> lock(state.mutex);
    auto it = state.records.find(owner.token);
    return it == state.records.end() ? "gone" : it->second.label;
}

void AsyncOwners::scriptInstanceReleased(const void* instance){
    OwnerState& state = ownerState();
    if (!instance || state.count.load(std::memory_order_relaxed) == 0) return;
    std::vector<uint64_t> gone;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        auto it = state.byScript.find(instance);
        if (it != state.byScript.end()) removeRecord(state, it->second, gone);
    }
    fireGone(gone);
}

void AsyncOwners::sceneDestroyed(const EntityRegistry* scene){
    OwnerState& state = ownerState();
    if (!scene || state.count.load(std::memory_order_relaxed) == 0) return;
    std::vector<uint64_t> gone;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        std::vector<uint64_t> tokens;
        for (const auto& [token, record] : state.records){
            if (record.registry == scene) tokens.push_back(token);
        }
        for (uint64_t token : tokens) removeRecord(state, token, gone);
    }
    fireGone(gone);
}

void AsyncOwners::entityDestroyed(const EntityRegistry* scene, Entity entity){
    OwnerState& state = ownerState();
    if (!scene || state.count.load(std::memory_order_relaxed) == 0) return;
    std::vector<uint64_t> gone;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        auto it = state.byEntity.find({scene, entity});
        if (it != state.byEntity.end()) removeRecord(state, it->second, gone);
        // the scripts of that GameObject
        std::vector<uint64_t> scripts;
        auto range = state.scriptsByEntity.equal_range({scene, entity});
        for (auto link = range.first; link != range.second; ++link) scripts.push_back(link->second);
        for (uint64_t token : scripts) removeRecord(state, token, gone);
    }
    fireGone(gone);
}

void AsyncOwners::entitiesMoved(const EntityRegistry* from, const EntityRegistry* to, const std::unordered_map<Entity, Entity>& moved){
    OwnerState& state = ownerState();
    if (!from || !to || moved.empty() || state.count.load(std::memory_order_relaxed) == 0) return;
    std::lock_guard<std::mutex> lock(state.mutex);
    for (const auto& [oldEntity, newEntity] : moved){
        // the GameObject's own owner
        auto entity = state.byEntity.find({from, oldEntity});
        if (entity != state.byEntity.end()){
            const uint64_t token = entity->second;
            state.byEntity.erase(entity);
            state.byEntity[{to, newEntity}] = token;
            auto record = state.records.find(token);
            if (record != state.records.end()){
                record->second.registry = to;
                record->second.entity = newEntity;
            }
        }
        // its scripts' owners: the instances move as they are, their link to the GameObject follows
        auto range = state.scriptsByEntity.equal_range({from, oldEntity});
        std::vector<uint64_t> scripts;
        for (auto link = range.first; link != range.second; ++link) scripts.push_back(link->second);
        state.scriptsByEntity.erase(range.first, range.second);
        for (uint64_t token : scripts){
            state.scriptsByEntity.emplace(std::make_pair(to, newEntity), token);
            auto record = state.records.find(token);
            if (record != state.records.end()){
                record->second.registry = to;
                record->second.entity = newEntity;
            }
        }
    }
}

FunctionSubscribe<void(uint64_t)>& AsyncOwners::onOwnerGone(){
    return goneEvent();
}

size_t AsyncOwners::count(){
    return ownerState().count.load(std::memory_order_relaxed);
}
