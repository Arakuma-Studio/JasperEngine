// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ScriptBase.h"
#include "component/ScriptComponent.h"
#include "script/ScriptMessages.h"

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

Scene* ScriptBase::getScene() const {
    return scene;
}

Entity ScriptBase::getEntity() const {
    return entity;
}

bool ScriptBase::baseMessageRan = false;

void ScriptBase::onJointBreak2D(Joint2D& joint) {
    (void)joint;
    inheritedMessages |= scriptMessageBit(ScriptMessage::JointBreak2D);
    baseMessageRan = true;
}

void ScriptBase::onJointBreak(float force, Joint3D& joint) {
    (void)force;
    (void)joint;
    inheritedMessages |= scriptMessageBit(ScriptMessage::JointBreak);
    baseMessageRan = true;
}
