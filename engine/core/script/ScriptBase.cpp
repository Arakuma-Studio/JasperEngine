// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ScriptBase.h"
#include "ScriptRuntime.h"
#include "component/ScriptComponent.h"
#include "script/ScriptMessages.h"

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

namespace {
    bool activeAndEnabled(Scene* scene, Entity entity, const void* instance, bool beforeStartCounts) {
        if (!scene || entity == NULL_ENTITY || !scene->isEntityCreated(entity)) {
            return true;
        }
        if (!scene->isEntityActiveInHierarchy(entity)) {
            return false;
        }
        if (ScriptEntry* entry = findEntryOf(scene, entity, instance)) {
            if (!entry->enabled) {
                return false;
            }
            // Unity: start before the first update; a script enabled during a frame gets its
            // updates from the next one, after its start
            const ScriptState* state = ScriptRuntime::getState(entry->instance);
            return beforeStartCounts || !(state && state->enabledLatch && !state->started);
        }
        return true;
    }
}

bool jasperengine::isScriptActive(Scene* scene, Entity entity, const void* instance) {
    return activeAndEnabled(scene, entity, instance, false);
}

bool ScriptBase::isActiveAndEnabled() const {
    return activeAndEnabled(scene, entity, this, true);
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

bool ScriptBase::baseMessageRan = false;

namespace {
    // a base message method ran: the script does not override it
    void inherited(ScriptBase& script, ScriptMessage message) {
        script.inheritedMessages |= scriptMessageBit(message);
        ScriptBase::baseMessageRan = true;
    }
}

void ScriptBase::onJointBreak2D(Joint2D& joint) {
    (void)joint;
    inherited(*this, ScriptMessage::JointBreak2D);
}

void ScriptBase::onParticleSystemStopped() {
    inherited(*this, ScriptMessage::ParticleSystemStopped);
}

void ScriptBase::init() { inherited(*this, ScriptMessage::Init); }
void ScriptBase::start() { inherited(*this, ScriptMessage::Start); }
void ScriptBase::onEnable() { inherited(*this, ScriptMessage::Enable); }
void ScriptBase::onDisable() { inherited(*this, ScriptMessage::Disable); }
void ScriptBase::onDestroy() { inherited(*this, ScriptMessage::Destroy); }
void ScriptBase::onApplicationPause(bool paused) { (void)paused; inherited(*this, ScriptMessage::ApplicationPause); }
void ScriptBase::onApplicationFocus(bool focused) { (void)focused; inherited(*this, ScriptMessage::ApplicationFocus); }
void ScriptBase::onApplicationQuit() { inherited(*this, ScriptMessage::ApplicationQuit); }
