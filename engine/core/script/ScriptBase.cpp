// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ScriptBase.h"
#include "component/ScriptComponent.h"

using namespace jasperengine;

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

ScriptBase::~ScriptBase() {
    ScriptScheduler::cancelOwner(this);
}

TaskId ScriptBase::invoke(std::function<void()> fn, float delay, TimeMode mode) {
    return ScriptScheduler::after(delay, std::move(fn), mode, this);
}

TaskId ScriptBase::invokeRepeating(std::function<void()> fn, float delay, float interval, TimeMode mode) {
    return ScriptScheduler::every(delay, interval, std::move(fn), mode, this);
}

bool ScriptBase::cancelInvoke(TaskId id) {
    if (!isInvoking(id)) return false;
    return ScriptScheduler::cancel(id);
}

void ScriptBase::cancelAllInvokes() {
    ScriptScheduler::cancelTimersOf(this);
}

bool ScriptBase::isInvoking() const {
    return ScriptScheduler::timerCountOf(this) > 0;
}

bool ScriptBase::isInvoking(TaskId id) const {
    return ScriptScheduler::isPending(id) && ScriptScheduler::timerOwner(id) == this;
}

Scene* ScriptBase::getScene() const {
    return scene;
}

Entity ScriptBase::getEntity() const {
    return entity;
}