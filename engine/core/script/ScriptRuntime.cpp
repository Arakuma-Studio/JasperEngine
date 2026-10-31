// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ScriptRuntime.h"
#include "ScriptMessages.h"
#include "LuaBinding.h"
#include "Engine.h"
#include "Scene.h"
#include "Log.h"
#include "manager/ObjectManager.h"
#include "component/ScriptComponent.h"
#include "component/Transform.h"

#include "lua.hpp"

#include <algorithm>

using namespace jasperengine;

namespace {

    // every instance the runtime knows, by its instance (Lua ref or C++ object)
    std::unordered_map<const void*, ScriptState> states;

    // scripts waiting for their start, in the order they were enabled: found again by instance
    // (scripts can be reordered, GameObjects destroyed meanwhile)
    std::vector<const void*> startQueue;

    // awakened and not unloaded, in start order
    std::vector<Scene*> startedScenes;

    bool isStarted(Scene* scene) {
        return scene && std::find(startedScenes.begin(), startedScenes.end(), scene) != startedScenes.end();
    }

    ScriptComponent* scriptsOf(Scene* scene, Entity entity) {
        return scene->isEntityCreated(entity) ? scene->findComponent<ScriptComponent>(entity) : nullptr;
    }

    ScriptEntry* entryAt(Scene* scene, Entity entity, size_t index) {
        ScriptComponent* scripts = scriptsOf(scene, entity);
        return scripts && index < scripts->scripts.size() ? &scripts->scripts[index] : nullptr;
    }

    // the state of the entry's instance (made when first needed), null without an instance
    ScriptState* stateOf(Scene* scene, Entity entity, const ScriptEntry& entry) {
        if (!entry.instance) {
            return nullptr;
        }
        ScriptState& state = states[entry.instance];
        state.scene = scene;
        state.entity = entity;
        return &state;
    }

    // The GameObjects of the scene that have scripts, in hierarchy pre-order
    std::vector<Entity> scriptedOrder(Scene* scene) {
        std::vector<Entity> order;
        if (scene->getComponentArray<ScriptComponent>()->size() == 0) {
            return order;
        }
        for (Entity entity : scene->getHierarchyOrder()) {
            if (scene->findComponent<ScriptComponent>(entity)) {
                order.push_back(entity);
            }
        }
        return order;
    }

    // Every index of the GameObject's scripts, the component read again after each call
    template<typename Fn>
    void forEachScript(Scene* scene, Entity entity, Fn&& fn) {
        for (size_t i = 0;; i++) {
            ScriptEntry* entry = entryAt(scene, entity, i);
            if (!entry) {
                return;
            }
            fn(*entry, i);
        }
    }

    bool call(Scene* scene, Entity entity, size_t index, ScriptMessage m) {
        return ScriptMessages::invoke(scene, entity, index, m, nullptr, [m](ScriptBase& script) {
            switch (m) {
                case ScriptMessage::Init: script.init(); break;
                case ScriptMessage::Start: script.start(); break;
                case ScriptMessage::Enable: script.onEnable(); break;
                case ScriptMessage::Disable: script.onDisable(); break;
                case ScriptMessage::Destroy: script.onDestroy(); break;
                case ScriptMessage::ApplicationQuit: script.onApplicationQuit(); break;
                default: break;
            }
        });
    }

    void queueStart(const void* instance, ScriptState& state) {
        if (state.started || state.startQueued) {
            return;
        }
        state.startQueued = true;
        startQueue.push_back(instance);
    }

    bool wantsEnabled(Scene* scene, Entity entity, const ScriptEntry& entry, const ScriptState& state) {
        return state.initialized && !state.destroyed && !state.destroying && entry.enabled &&
               isStarted(scene) && scene->isEntityActiveInHierarchy(entity);
    }

    // The latch, the only place onEnable / onDisable are called. The flag changes before the
    // call, so what the callback changes (disabling itself in onEnable, deactivating its parent
    // in onDisable) gives the matching call afterwards and never a second one.
    void refreshAt(Scene* scene, Entity entity, size_t index) {
        ScriptEntry* entry = entryAt(scene, entity, index);
        ScriptState* state = entry ? stateOf(scene, entity, *entry) : nullptr;
        if (!state) {
            return;
        }
        const bool want = wantsEnabled(scene, entity, *entry, *state);
        ScriptMessage message = ScriptMessage::Count;
        if (want && !state->enabledLatch) {
            state->enabledLatch = true;
            queueStart(entry->instance, *state);
            message = ScriptMessage::Enable;
        } else if (!want && state->enabledLatch) {
            state->enabledLatch = false;
            message = ScriptMessage::Disable;
        }
        // self.enabled / self.isActiveAndEnabled as the callback should see them
        LuaBinding::syncScriptMembers(scene, entity, *entry, entry->enabled && !state->destroyed &&
                                      scene->isEntityActiveInHierarchy(entity));
        if (message != ScriptMessage::Count) {
            call(scene, entity, index, message);
        }
    }

    void refreshAll(Scene* scene, Entity entity) {
        forEachScript(scene, entity, [&](ScriptEntry&, size_t index) {
            refreshAt(scene, entity, index);
        });
    }

    // init of the entries not initialized yet, while the GameObject is active in hierarchy (a
    // script's init may deactivate it: the scripts after it wait for its activation, as Unity)
    void initAll(Scene* scene, Entity entity) {
        forEachScript(scene, entity, [&](ScriptEntry& entry, size_t index) {
            ScriptState* state = stateOf(scene, entity, entry);
            if (!state || state->initialized || state->destroyed || state->destroying) {
                return;
            }
            if (!scene->isEntityActiveInHierarchy(entity)) {
                return;
            }
            // first: a nested activation (the script activates something that activates this)
            // does not init it twice
            state->initialized = true;
            call(scene, entity, index, ScriptMessage::Init);
        });
    }

    void awake(Scene* scene, const std::vector<Entity>& entities) {
        for (Entity entity : entities) {
            initAll(scene, entity);
        }
        for (Entity entity : entities) {
            refreshAll(scene, entity);
        }
    }

    void markDestroying(Scene* scene, const std::vector<Entity>& entities) {
        for (Entity entity : entities) {
            if (ScriptComponent* scripts = scriptsOf(scene, entity)) {
                for (ScriptEntry& entry : scripts->scripts) {
                    if (ScriptState* state = stateOf(scene, entity, entry)) {
                        state->destroying = true;
                    }
                }
            }
        }
    }

    // Unity: onDisable first (all of them), then onDestroy, only for what was initialized
    void disableAll(Scene* scene, const std::vector<Entity>& entities) {
        for (Entity entity : entities) {
            refreshAll(scene, entity);
        }
    }

    void destroyAll(Scene* scene, const std::vector<Entity>& entities) {
        for (Entity entity : entities) {
            forEachScript(scene, entity, [&](ScriptEntry& entry, size_t index) {
                ScriptState* state = stateOf(scene, entity, entry);
                if (!state || state->destroyed) {
                    return;
                }
                state->destroying = true;
                state->destroyed = true;
                state->startQueued = false;
                LuaBinding::syncScriptMembers(scene, entity, entry, false);
                if (state->initialized) {
                    call(scene, entity, index, ScriptMessage::Destroy);
                }
            });
            if (scene->isEntityCreated(entity)) {
                ScriptMessages::notifyReceiversChanged(scene, entity);
            }
        }
    }

    void dropQueuedStarts(Scene* scene) {
        startQueue.erase(std::remove_if(startQueue.begin(), startQueue.end(), [scene](const void* instance) {
            auto state = states.find(instance);
            if (state == states.end() || state->second.scene != scene) {
                return false;
            }
            state->second.startQueued = false;
            return true;
        }), startQueue.end());
    }

}

const ScriptState* ScriptRuntime::getState(const void* instance) {
    auto state = instance ? states.find(instance) : states.end();
    return state != states.end() ? &state->second : nullptr;
}

void ScriptRuntime::forgetInstance(const void* instance) {
    if (!instance) {
        return;
    }
    states.erase(instance);
    startQueue.erase(std::remove(startQueue.begin(), startQueue.end(), instance), startQueue.end());
}

ScriptEntry* ScriptRuntime::findEntry(Scene* scene, Entity entity, const void* instance, size_t* index) {
    if (!scene || !instance) {
        return nullptr;
    }
    ScriptComponent* scripts = scriptsOf(scene, entity);
    if (!scripts) {
        return nullptr;
    }
    for (size_t i = 0; i < scripts->scripts.size(); i++) {
        ScriptEntry& entry = scripts->scripts[i];
        if (entry.instance == instance || (entry.cppScript && static_cast<const void*>(entry.cppScript) == instance)) {
            if (index) *index = i;
            return &entry;
        }
    }
    return nullptr;
}

void ScriptRuntime::awakeScene(Scene* scene) {
    if (!scene || isStarted(scene)) {
        return;
    }
    startedScenes.push_back(scene);
    // what the scripts instantiate goes to their scene, not yet the Engine's
    ObjectManager::pushActiveScene(scene);
    awake(scene, scriptedOrder(scene));
    ObjectManager::popActiveScene();
}

void ScriptRuntime::awakeEntities(Scene* scene, const std::vector<Entity>& entities) {
    // a scene that has not started awakens them with the others
    if (!isStarted(scene)) {
        return;
    }
    awake(scene, entities);
}

void ScriptRuntime::onActiveInHierarchyChanged(Scene* scene, const std::vector<Entity>& changed, bool nowActive) {
    if (!isStarted(scene)) {
        return;
    }
    if (nowActive) {
        awake(scene, changed);
    } else {
        disableAll(scene, changed);
        // (coroutines: ScriptScheduler::stopCoroutinesOf for the instances of `changed`)
    }
}

void ScriptRuntime::setScriptEnabled(Scene* scene, Entity entity, size_t index, bool enabled) {
    ScriptEntry* entry = scene ? entryAt(scene, entity, index) : nullptr;
    if (!entry || entry->enabled == enabled) {
        return;
    }
    entry->enabled = enabled;
    ScriptMessages::notifyReceiversChanged(scene, entity);
    refreshAt(scene, entity, index);
}

void ScriptRuntime::refreshEntity(Scene* scene, Entity entity) {
    if (!isStarted(scene) || !scene->isEntityCreated(entity)) {
        return;
    }
    ScriptMessages::notifyReceiversChanged(scene, entity);
    refreshAll(scene, entity);
}

void ScriptRuntime::flushStarts() {
    if (startQueue.empty()) {
        return;
    }
    // what is queued meanwhile waits for the next frame
    std::vector<const void*> due;
    due.swap(startQueue);
    for (const void* instance : due) {
        auto found = states.find(instance);
        if (found == states.end() || !found->second.startQueued) {
            continue;
        }
        ScriptState& state = found->second;
        state.startQueued = false;
        if (state.started || state.destroyed || !isStarted(state.scene)) {
            continue;
        }
        Scene* scene = state.scene;
        const Entity entity = state.entity;
        size_t index = 0;
        ScriptEntry* entry = findEntry(scene, entity, instance, &index);
        // not active and enabled now: the next onEnable queues it again
        if (!entry || !state.enabledLatch || !wantsEnabled(scene, entity, *entry, state)) {
            continue;
        }
        state.started = true;
        // (coroutines: a Lua start runs as a coroutine through ScriptScheduler::startCoroutine,
        // so it can wait; until then it is a plain call)
        call(scene, entity, index, ScriptMessage::Start);
    }
}

void ScriptRuntime::destroyEntities(Scene* scene, const std::vector<Entity>& entities) {
    if (!scene || entities.empty()) {
        return;
    }
    markDestroying(scene, entities);
    disableAll(scene, entities);
    // (coroutines: ScriptScheduler::cancelOwner for their instances)
    destroyAll(scene, entities);
}

void ScriptRuntime::unloadScenes(const std::vector<Scene*>& scenes) {
    std::vector<std::pair<Scene*, std::vector<Entity>>> unloading;
    for (Scene* scene : scenes) {
        if (!scene || std::any_of(unloading.begin(), unloading.end(), [scene](const auto& item) { return item.first == scene; })) {
            continue;
        }
        // not started from here on: what the callbacks activate or instantiate in it stays asleep
        startedScenes.erase(std::remove(startedScenes.begin(), startedScenes.end(), scene), startedScenes.end());
        dropQueuedStarts(scene);
        unloading.push_back({scene, scriptedOrder(scene)});
    }
    for (auto& [scene, entities] : unloading) {
        markDestroying(scene, entities);
    }
    for (auto& [scene, entities] : unloading) {
        disableAll(scene, entities);
    }
    for (auto& [scene, entities] : unloading) {
        destroyAll(scene, entities);
    }
}

void ScriptRuntime::quitAll() {
    const std::vector<Scene*> scenes = getStartedScenes();
    for (Scene* scene : scenes) {
        if (!isStarted(scene)) {
            continue;
        }
        for (Entity entity : scriptedOrder(scene)) {
            forEachScript(scene, entity, [&](ScriptEntry& entry, size_t index) {
                const ScriptState* state = getState(entry.instance);
                if (state && state->initialized && !state->destroyed) {
                    call(scene, entity, index, ScriptMessage::ApplicationQuit);
                }
            });
        }
    }
    unloadScenes(scenes);
}

void ScriptRuntime::broadcast(ScriptMessage m, const std::function<int(lua_State*)>& pushArgs,
                              const std::function<void(ScriptBase&)>& callCpp) {
    for (Scene* scene : getStartedScenes()) {
        if (!Engine::isSceneRunning(scene)) {
            continue;
        }
        for (Entity entity : scriptedOrder(scene)) {
            if (!isStarted(scene)) {
                break;
            }
            ScriptMessages::send(scene, entity, m, pushArgs, callCpp, DeliverRequireActive);
        }
    }
}

Entity ScriptRuntime::findHandler(Scene* scene, Entity entity, ScriptMessage m) {
    if (!scene) {
        return NULL_ENTITY;
    }
    for (Entity current = entity; current != NULL_ENTITY && scene->isEntityCreated(current);) {
        if (scene->isEntityActiveInHierarchy(current) && ScriptMessages::defines(scene, current, m)) {
            return current;
        }
        Transform* transform = scene->findComponent<Transform>(current);
        current = transform ? transform->parent : NULL_ENTITY;
    }
    return NULL_ENTITY;
}

int ScriptRuntime::sendMessage(Scene* scene, Entity entity, ScriptMessage m,
                               const std::function<int(lua_State*)>& pushArgs,
                               const std::function<void(ScriptBase&)>& callCpp,
                               uint32_t deliver) {
    return ScriptMessages::send(scene, entity, m, pushArgs, callCpp, deliver);
}

bool ScriptRuntime::hasStarted(Scene* scene) {
    return isStarted(scene);
}

std::vector<Scene*> ScriptRuntime::getStartedScenes() {
    std::vector<Scene*> scenes;
    for (Scene* scene : Engine::getSceneLayers()) {
        if (isStarted(scene) && std::find(scenes.begin(), scenes.end(), scene) == scenes.end()) {
            scenes.push_back(scene);
        }
    }
    for (Scene* scene : startedScenes) {
        if (std::find(scenes.begin(), scenes.end(), scene) == scenes.end()) {
            scenes.push_back(scene);
        }
    }
    return scenes;
}

void ScriptRuntime::forgetScene(Scene* scene) {
    startedScenes.erase(std::remove(startedScenes.begin(), startedScenes.end(), scene), startedScenes.end());
    dropQueuedStarts(scene);
    for (auto it = states.begin(); it != states.end();) {
        it = it->second.scene == scene ? states.erase(it) : std::next(it);
    }
    ScriptMessages::forgetScene(scene);
}

void ScriptRuntime::remap(Scene* from, Scene* to, const std::unordered_map<Entity, Entity>& entities) {
    for (auto& [instance, state] : states) {
        if (state.scene != from) {
            continue;
        }
        auto moved = entities.find(state.entity);
        if (moved != entities.end()) {
            state.scene = to;
            state.entity = moved->second;
        }
    }
}
