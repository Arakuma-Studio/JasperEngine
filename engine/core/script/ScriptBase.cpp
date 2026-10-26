// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ScriptBase.h"
#include "component/ScriptComponent.h"

using namespace jasperengine;

namespace {
    bool scriptsReloading = false;
}

bool ScriptBase::isReloading() {
    return scriptsReloading;
}

void ScriptBase::setReloading(bool reloading) {
    scriptsReloading = reloading;
}

bool jasperengine::isScriptActive(Scene* scene, Entity entity, const void* instance) {
    if (!scene || entity == NULL_ENTITY || !scene->isEntityCreated(entity)) {
        return true;
    }
    if (!scene->isEntityActiveInHierarchy(entity)) {
        return false;
    }
    if (instance) {
        if (ScriptComponent* scripts = scene->findComponent<ScriptComponent>(entity)) {
            for (const ScriptEntry& entry : scripts->scripts) {
                if (entry.instance == instance) {
                    return entry.enabled;
                }
            }
        }
    }
    return true;
}

bool ScriptBase::isActiveAndEnabled() const {
    return isScriptActive(scene, entity, this);
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