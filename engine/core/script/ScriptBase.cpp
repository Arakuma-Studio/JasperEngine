// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ScriptBase.h"
#include "ScriptRuntime.h"
#include "component/ScriptComponent.h"
#include "script/AsyncOwner.h"
#include "script/ScriptMessages.h"
#include "manager/SceneManager.h"
#include "manager/ObjectManager.h"
#include "registry/EntityCloner.h"

#include <algorithm>

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

ScriptBase::~ScriptBase() {
    ScriptScheduler::cancelOwner(this);
    AsyncOwners::scriptInstanceReleased(this);
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
                const ScriptState* state = ScriptRuntime::getState(entry.instance);
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

// The base message methods: the script does not override the message, it is not sent to it again
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

void ScriptBase::onJointBreak(float force, Joint3D& joint) {
    (void)force;
    (void)joint;
    inherited(*this, ScriptMessage::JointBreak);
}

void ScriptBase::onCollisionEnter(const Collision&) { inherited(*this, ScriptMessage::CollisionEnter); }
void ScriptBase::onCollisionStay(const Collision&) { inherited(*this, ScriptMessage::CollisionStay); }
void ScriptBase::onCollisionExit(const Collision&) { inherited(*this, ScriptMessage::CollisionExit); }
void ScriptBase::onTriggerEnter(const ColliderRef&) { inherited(*this, ScriptMessage::TriggerEnter); }
void ScriptBase::onTriggerStay(const ColliderRef&) { inherited(*this, ScriptMessage::TriggerStay); }
void ScriptBase::onTriggerExit(const ColliderRef&) { inherited(*this, ScriptMessage::TriggerExit); }
void ScriptBase::onCollisionEnter2D(const Collision2D&) { inherited(*this, ScriptMessage::CollisionEnter2D); }
void ScriptBase::onCollisionStay2D(const Collision2D&) { inherited(*this, ScriptMessage::CollisionStay2D); }
void ScriptBase::onCollisionExit2D(const Collision2D&) { inherited(*this, ScriptMessage::CollisionExit2D); }
void ScriptBase::onTriggerEnter2D(const ColliderRef2D&) { inherited(*this, ScriptMessage::TriggerEnter2D); }
void ScriptBase::onTriggerStay2D(const ColliderRef2D&) { inherited(*this, ScriptMessage::TriggerStay2D); }
void ScriptBase::onTriggerExit2D(const ColliderRef2D&) { inherited(*this, ScriptMessage::TriggerExit2D); }

void ScriptBase::init() { inherited(*this, ScriptMessage::Init); }
void ScriptBase::start() { inherited(*this, ScriptMessage::Start); }
void ScriptBase::onEnable() { inherited(*this, ScriptMessage::Enable); }
void ScriptBase::onDisable() { inherited(*this, ScriptMessage::Disable); }
void ScriptBase::onDestroy() { inherited(*this, ScriptMessage::Destroy); }
void ScriptBase::onApplicationPause(bool paused) { (void)paused; inherited(*this, ScriptMessage::ApplicationPause); }
void ScriptBase::onApplicationFocus(bool focused) { (void)focused; inherited(*this, ScriptMessage::ApplicationFocus); }
void ScriptBase::onApplicationQuit() { inherited(*this, ScriptMessage::ApplicationQuit); }

void ScriptBase::onPointerDown(PointerEventData&) { inherited(*this, ScriptMessage::PointerDown); }
void ScriptBase::onPointerUp(PointerEventData&) { inherited(*this, ScriptMessage::PointerUp); }
void ScriptBase::onPointerClick(PointerEventData&) { inherited(*this, ScriptMessage::PointerClick); }
void ScriptBase::onPointerEnter(PointerEventData&) { inherited(*this, ScriptMessage::PointerEnter); }
void ScriptBase::onPointerExit(PointerEventData&) { inherited(*this, ScriptMessage::PointerExit); }
void ScriptBase::onInitializePotentialDrag(PointerEventData&) { inherited(*this, ScriptMessage::InitializePotentialDrag); }
void ScriptBase::onBeginDrag(PointerEventData&) { inherited(*this, ScriptMessage::BeginDrag); }
void ScriptBase::onDrag(PointerEventData&) { inherited(*this, ScriptMessage::Drag); }
void ScriptBase::onEndDrag(PointerEventData&) { inherited(*this, ScriptMessage::EndDrag); }
void ScriptBase::onDrop(PointerEventData&) { inherited(*this, ScriptMessage::Drop); }
void ScriptBase::onScroll(PointerEventData&) { inherited(*this, ScriptMessage::Scroll); }
void ScriptBase::onSelect(BaseEventData&) { inherited(*this, ScriptMessage::Select); }
void ScriptBase::onDeselect(BaseEventData&) { inherited(*this, ScriptMessage::Deselect); }
void ScriptBase::onMove(AxisEventData&) { inherited(*this, ScriptMessage::Move); }
void ScriptBase::onSubmit(BaseEventData&) { inherited(*this, ScriptMessage::Submit); }
void ScriptBase::onCancel(BaseEventData&) { inherited(*this, ScriptMessage::Cancel); }
