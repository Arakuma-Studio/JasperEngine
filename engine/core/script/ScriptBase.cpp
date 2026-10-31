// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ScriptBase.h"
#include "ScriptRuntime.h"
#include "component/ScriptComponent.h"
#include "manager/SceneManager.h"
#include "manager/ObjectManager.h"
#include "registry/EntityCloner.h"

#include <algorithm>

using namespace jasperengine;

namespace {
    // The entry of a script object: its instance (the generated factory's pointer, which the
    // engine events' guard compares) or, for a C++ script, its ScriptBase
    ScriptEntry* findEntryOf(Scene* scene, Entity entity, const void* instance, size_t* index = nullptr) {
        if (!instance) {
            return nullptr;
        }
        if (ScriptComponent* scripts = scene->findComponent<ScriptComponent>(entity)) {
            for (size_t i = 0; i < scripts->scripts.size(); i++) {
                ScriptEntry& entry = scripts->scripts[i];
                if (entry.instance == instance || (entry.cppScript && static_cast<const void*>(entry.cppScript) == instance)) {
                    if (index) *index = i;
                    return &entry;
                }
            }
        }
        return nullptr;
    }
}

bool jasperengine::isScriptActive(Scene* scene, Entity entity, const void* instance) {
    if (!scene || entity == NULL_ENTITY || !scene->isEntityCreated(entity)) {
        return true;
    }
    if (!scene->isEntityActiveInHierarchy(entity)) {
        return false;
    }
    if (ScriptEntry* entry = findEntryOf(scene, entity, instance)) {
        return entry->enabled;
    }
    return true;
}

bool ScriptBase::isActiveAndEnabled() const {
    return isScriptActive(scene, entity, this);
}

bool ScriptBase::isEnabled() const {
    if (!scene || !scene->isEntityCreated(entity)) {
        return false;
    }
    ScriptEntry* entry = findEntryOf(scene, entity, this);
    return entry && entry->enabled;
}

void ScriptBase::setEnabled(bool enabled) {
    if (!scene || !scene->isEntityCreated(entity)) {
        return;
    }
    size_t index = 0;
    if (findEntryOf(scene, entity, this, &index)) {
        ScriptRuntime::setScriptEnabled(scene, entity, index, enabled);
    }
}

ScriptBase::ScriptBase(Scene* scene, Entity entity)
    : scene(scene), entity(entity) {
}

Scene* ScriptBase::getScene() const {
    return scene;
}

Entity ScriptBase::getEntity() const {
    return entity;
}

void ScriptBase::rebind(Scene* scene, Entity entity) {
    this->scene = scene;
    this->entity = entity;
}

bool ScriptBase::dontDestroyOnLoad() {
    if (!scene || !scene->isEntityCreated(entity)) {
        return false;
    }
    return SceneManager::dontDestroyOnLoad(scene, entity);
}

bool ScriptBase::isPersistent() const {
    return SceneManager::isPersistent(scene, entity);
}

ScriptBase* ScriptBase::findPersistentScript(bool (*matches)(ScriptBase*)) const {
    auto search = [this, matches](Scene* where, const std::vector<Entity>& entities) -> ScriptBase* {
        for (Entity e : entities) {
            ScriptComponent* scripts = where->isEntityCreated(e) ? where->findComponent<ScriptComponent>(e) : nullptr;
            if (!scripts) {
                continue;
            }
            for (const ScriptEntry& entry : scripts->scripts) {
                const ScriptState* state = ScriptRuntime::getState(entry.cppScript);
                if (entry.cppScript && entry.cppScript != this && !(state && (state->destroyed || state->destroying)) &&
                        matches(entry.cppScript)) {
                    return entry.cppScript;
                }
            }
        }
        return nullptr;
    };
    if (Scene* persistent = SceneManager::getPersistentScene()) {
        std::vector<Entity> entities;
        auto scripts = persistent->getComponentArray<ScriptComponent>();
        for (size_t i = 0; i < scripts->size(); i++) {
            entities.push_back(scripts->getEntity(i));
        }
        std::sort(entities.begin(), entities.end());
        if (ScriptBase* found = search(persistent, entities)) {
            return found;
        }
    }
    for (const auto& [from, root] : SceneManager::getPendingPersistentRoots()) {
        if (ScriptBase* found = search(from, EntityCloner::collectHierarchy(from, root))) {
            return found;
        }
    }
    return nullptr;
}

void ScriptBase::destroyOwnGameObject() {
    if (scene && scene->isEntityCreated(entity)) {
        ObjectManager::destroy(scene, entity);
    }
}
