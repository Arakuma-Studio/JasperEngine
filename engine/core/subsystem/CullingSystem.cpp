// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CullingSystem.h"

#include "Engine.h"
#include "Log.h"
#include "LuaFunction.h"
#include "Scene.h"
#include "component/CameraComponent.h"
#include "component/LineRendererComponent.h"
#include "component/MeshComponent.h"
#include "component/RigidbodyComponent.h"
#include "component/Rigidbody2DComponent.h"
#include "component/ScriptComponent.h"
#include "component/Transform.h"
#include "device/QualityResolver.h"
#include "jobs/JobSystem.h"
#include "object/LineRenderer.h"
#include "object/SimulationCulling.h"
#include "script/ScriptMessages.h"
#include "subsystem/PhysicsSystem.h"
#include "subsystem/TransformPropagation.h"
#if JASPERENGINE_TERRAIN_WORLD
#include "subsystem/TerrainWorldSystem.h"
#endif
#include "util/SubscriptionOwnerIndex.h"

#ifdef JASPERENGINE_PHYSICS_3D
#include "util/JoltPhysicsAux.h"
#endif

#include "lua.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>

using namespace jasperengine;

std::atomic<int> jasperengine::detail::simulationCullingActive{0};

namespace {

    constexpr uint32_t STATE_MASK = 0xFFu;
    constexpr uint32_t VISIBLE_BIT = 0x80u;
    constexpr uint32_t BAND_MASK = 0x0Fu;
    constexpr uint32_t BAND_UNKNOWN = 0x0Fu;
    constexpr float NO_FOCUS_DISTANCE = std::numeric_limits<float>::max();

    bool cullingDebugDraw = false;

    // the scenes whose systems hold a Simulation Culling component: the guard's lookup
    struct ActiveCulling {
        Scene* scene;
        CullingSystem* system;
    };
    std::vector<ActiveCulling>& activeCulling() {
        static std::vector<ActiveCulling>* list = new std::vector<ActiveCulling>();
        return *list;
    }

    CullingSystem* activeSystemOf(Scene* scene) {
        for (const ActiveCulling& active : activeCulling()) {
            if (active.scene == scene) return active.system;
        }
        return nullptr;
    }

    // the reduced call being made (SimulationReducedScope)
    struct ReducedCall {
        Scene* scene = nullptr;
        unsigned int entity = 0;
        double delta = 0.0;
    };
    ReducedCall reducedCall;

    // releases a Lua script's groups with it (SubscriptionOwnerIndex::releaseOwner)
    struct CullingGroupOwnership : public detail::SlotStateBase {
        CullingSystem* system = nullptr;
        bool removeSlot(uint64_t id) override {
            if (!system) return false;
            const uint32_t slot = static_cast<uint32_t>(id >> 32);
            const uint32_t generation = static_cast<uint32_t>(id & 0xFFFFFFFFu);
            if (!system->findGroup(slot, generation)) return false;
            system->disposeGroup(slot, generation);
            return true;
        }
    };

    // ownership states per system (a system's groups name it as theirs)
    std::vector<std::pair<const CullingSystem*, std::shared_ptr<CullingGroupOwnership>>>& ownerships() {
        static auto* list = new std::vector<std::pair<const CullingSystem*, std::shared_ptr<CullingGroupOwnership>>>();
        return *list;
    }

    std::shared_ptr<CullingGroupOwnership> ownershipOf(CullingSystem* system, bool make) {
        for (auto& entry : ownerships()) {
            if (entry.first == system) return entry.second;
        }
        if (!make) return nullptr;
        auto state = std::make_shared<CullingGroupOwnership>();
        state->system = system;
        ownerships().emplace_back(system, state);
        return state;
    }

    uint64_t ownershipId(uint32_t slot, uint32_t generation) {
        return (static_cast<uint64_t>(slot) << 32) | generation;
    }

    int8_t bandOf(uint32_t state) {
        const uint32_t band = state & BAND_MASK;
        return band == BAND_UNKNOWN ? int8_t(-1) : static_cast<int8_t>(band);
    }

    const Vector4 colorFull(0.2f, 0.9f, 0.3f, 1.0f);
    const Vector4 colorReduced(1.0f, 0.85f, 0.1f, 1.0f);
    const Vector4 colorCulledDistance(1.0f, 0.2f, 0.2f, 1.0f);
    const Vector4 colorCulledGround(0.55f, 0.55f, 0.55f, 1.0f);
    const Vector4 colorGroupVisible(0.1f, 0.9f, 0.95f, 1.0f);
    const Vector4 colorGroupHidden(0.1f, 0.2f, 0.6f, 1.0f);
    const Vector4 colorFocus(1.0f, 1.0f, 1.0f, 1.0f);

    // the spheres a group draws at most (a 100,000-sphere group would draw millions of lines)
    constexpr uint32_t DEBUG_SPHERES_PER_GROUP = 2048;

    void wireSphere(LineRenderer& lines, const Vector3& center, float radius, const Vector4& color) {
        constexpr int segments = 16;
        const float r = std::max(radius, 0.05f);
        for (int axis = 0; axis < 3; axis++) {
            Vector3 previous;
            for (int i = 0; i <= segments; i++) {
                const float a = 2.0f * 3.14159265f * static_cast<float>(i) / segments;
                const float c = std::cos(a) * r, s = std::sin(a) * r;
                const Vector3 point = axis == 0 ? center + Vector3(c, s, 0.0f) : (axis == 1 ? center + Vector3(c, 0.0f, s) : center + Vector3(0.0f, c, s));
                if (i > 0) lines.addLine(previous, point, color);
                previous = point;
            }
        }
    }

}

// ------------------------------------------------------------------ the scripts' guard

CullingSystem* CullingSystem::activeFor(Scene* scene) {
    return activeSystemOf(scene);
}

SimulationGate jasperengine::simulationCullingGate(Scene* scene, unsigned int entity, int family, double& reducedDelta) {
    CullingSystem* system = activeSystemOf(scene);
    if (!system) return SimulationGate::Call;
    return system->gate(static_cast<Entity>(entity), family, reducedDelta);
}

SimulationReducedScope::SimulationReducedScope(Scene* scene, unsigned int entity, double delta)
    : previousScene(reducedCall.scene), previousEntity(reducedCall.entity), previousDelta(reducedCall.delta) {
    reducedCall.scene = scene;
    reducedCall.entity = entity;
    reducedCall.delta = delta;
}

SimulationReducedScope::~SimulationReducedScope() {
    reducedCall.scene = previousScene;
    reducedCall.entity = previousEntity;
    reducedCall.delta = previousDelta;
}

bool SimulationCulling::getReducedScriptDelta(Scene* scriptScene, Entity scriptEntity, double& delta) {
    if (!reducedCall.scene) return false;
    // a nested call of another GameObject's script (a message it sent) has its own delta
    if (scriptScene && (scriptScene != reducedCall.scene || scriptEntity != reducedCall.entity)) return false;
    delta = reducedCall.delta;
    return true;
}

SimulationGate CullingSystem::gate(Entity entity, int family, double& delta) {
    const uint8_t flags = getSimFlags(entity);
    if (!flags) return SimulationGate::Call;
    if (flags & SimScriptsCulled) {
        skippedThisFrame++;
        return SimulationGate::Skip;
    }
    if ((flags & SimReduced) && family == 1 && entity < reducedTimings.size()) {
        ReducedTiming& timing = reducedTimings[entity];
        const uint64_t frame = Engine::getGameFrames();
        if ((frame + entity) % timing.interval != 0) {
            skippedThisFrame++;
            return SimulationGate::Skip;
        }
        // once a frame: its onUpdate and onPostUpdate (and every script of it) share the delta
        if (timing.frame != frame) {
            const double now = Engine::getGameTime();
            timing.delta = now - timing.lastTime;
            timing.lastTime = now;
            timing.frame = frame;
        }
        delta = timing.delta;
        return SimulationGate::Reduced;
    }
    return SimulationGate::Call;
}

bool CullingSystem::reducedDelta(Entity entity, double& delta) const {
    if (!(getSimFlags(entity) & SimReduced) || entity >= reducedTimings.size()) return false;
    delta = reducedTimings[entity].delta;
    return true;
}

// ------------------------------------------------------------------ the system

CullingSystem::CullingSystem(Scene* scene): SubSystem(scene) {
    signature.set(scene->getComponentId<SimulationCullingComponent>());
    declareHookType(scene->getComponentId<SimulationCullingComponent>());
    // a destroyed GameObject's flags go with it (its id may be reused)
    declareHookType(scene->getComponentId<Transform>());
}

CullingSystem::~CullingSystem() {
    if (registeredActive) {
        auto& list = activeCulling();
        list.erase(std::remove_if(list.begin(), list.end(), [this](const ActiveCulling& a) { return a.system == this; }), list.end());
        detail::simulationCullingActive.fetch_sub(1, std::memory_order_relaxed);
        registeredActive = false;
    }
    auto& states = ownerships();
    for (auto it = states.begin(); it != states.end(); ++it) {
        if (it->first == this) {
            it->second->system = nullptr;
            states.erase(it);
            break;
        }
    }
}

void CullingSystem::load() {
}

void CullingSystem::draw() {
}

void CullingSystem::destroy() {
}

bool CullingSystem::hasWork() const {
    return liveGroups > 0 || scene->getComponentArray<SimulationCullingComponent>()->size() > 0;
}

void CullingSystem::refreshActive() {
    const bool want = scene->getComponentArray<SimulationCullingComponent>()->size() > 0;
    if (want == registeredActive) return;
    auto& list = activeCulling();
    if (want) {
        list.push_back({scene, this});
        detail::simulationCullingActive.fetch_add(1, std::memory_order_relaxed);
    } else {
        list.erase(std::remove_if(list.begin(), list.end(), [this](const ActiveCulling& a) { return a.system == this; }), list.end());
        detail::simulationCullingActive.fetch_sub(1, std::memory_order_relaxed);
    }
    registeredActive = want;
}

void CullingSystem::onComponentAdded(Entity entity, ComponentId componentId) {
    if (componentId != scene->getComponentId<SimulationCullingComponent>()) return;
    // evaluated before its first callback: at the next frame start, or this frame's update
    pendingNew.push_back(entity);
    refreshActive();
}

void CullingSystem::onComponentRemoved(Entity entity, ComponentId componentId) {
    if (simFlags.empty() || entity >= simFlags.size()) return;
    if (componentId == scene->getComponentId<Transform>()) {
        simFlags[entity] = 0;
        return;
    }
    if (componentId != scene->getComponentId<SimulationCullingComponent>()) return;
    // what it froze runs again once the removal is over (a destroyed GameObject's flags went with
    // its Transform): not inside the removal, which must not reorder the arrays
    if (!scene->isTearingDown() && simFlags[entity]) removedComponents.push_back(entity);
}

void CullingSystem::flushRemoved() {
    if (removedComponents.empty()) return;
    std::vector<Entity> removed;
    removed.swap(removedComponents);
    const SimulationCullingComponent defaults;
    for (Entity entity : removed) {
        if (scene->isEntityCreated(entity) && !scene->findComponent<SimulationCullingComponent>(entity)) {
            applyFlags(entity, defaults, SimulationState::Full);
        }
    }
    refreshActive();
}

void CullingSystem::preUpdate() {
    stats.scriptUpdatesSkipped = skippedThisFrame;
    skippedThisFrame = 0;
    if (paused) return;
    flushRemoved();
    if (!hasWork()) {
        if (registeredActive) refreshActive();
        return;
    }
    refreshActive();
    if (!started) {
        start();
        return;
    }
    if (pendingNew.empty() || scene->getComponentArray<SimulationCullingComponent>()->size() == 0) return;
    // instantiated since the last update: their world positions now, every transition applied
    updateWorldTransforms(*scene);
    gatherFocuses();
    std::vector<Entity> only;
    only.swap(pendingNew);
    evaluateSimulation(true, &only);
    sendMessages();
}

void CullingSystem::start() {
    started = true;
    // files hold no world values and Render has not run: the positions now
    updateWorldTransforms(*scene);
    gatherCameras();
    pendingNew.clear();
    if (scene->getComponentArray<SimulationCullingComponent>()->size() > 0) {
        gatherFocuses();
        evaluateSimulation(true, nullptr);
        sendMessages();
    }
}

void CullingSystem::evaluateNow() {
    if (!started) {
        start();
    } else {
        updateWorldTransforms(*scene);
        gatherCameras();
        gatherFocuses();
        evaluateSimulation(true, nullptr);
        sendMessages();
        pendingNew.clear();
    }
    evaluateGroups();
}

void CullingSystem::update(double dt) {
    (void)dt;
    if (!paused) flushRemoved();
    if (paused || !hasWork()) {
        // edit mode adds and removes components: nothing to evaluate until Play starts the scene
        if (paused) pendingNew.clear();
        else if (registeredActive) refreshActive();
        if (debugLinesShown) updateDebugLines();
        return;
    }
    refreshActive();
    if (!started) start();
    const auto begin = std::chrono::steady_clock::now();
    frameCounter++;
    gatherCameras();
    evaluateGroups();
    if (scene->getComponentArray<SimulationCullingComponent>()->size() > 0) {
        gatherFocuses();
        // made since the frame started (in onUpdate): their first evaluation is never paced
        if (!pendingNew.empty()) {
            std::vector<Entity> only;
            only.swap(pendingNew);
            evaluateSimulation(true, &only);
        }
        evaluateSimulation(false, nullptr);
        sendMessages();
    } else {
        pendingNew.clear();
    }
    stats.evaluateMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    if (cullingDebugDraw || debugLinesShown) updateDebugLines();
}

// ------------------------------------------------------------------ cameras and positions

void CullingSystem::gatherCameras() {
    cameraPlanes.clear();
    auto cameras = scene->getComponentArray<CameraComponent>();
    for (size_t c = 0; c < cameras->size(); c++) {
        const CameraComponent& camera = cameras->getComponentFromIndex(c);
        CameraPlanes planes{};
        // a camera Render never updated has no planes: it sees nothing yet
        bool valid = false;
        for (int p = 0; p < 6; p++) {
            planes.nx[p] = camera.frustumPlanes[p].normal.x;
            planes.ny[p] = camera.frustumPlanes[p].normal.y;
            planes.nz[p] = camera.frustumPlanes[p].normal.z;
            planes.d[p] = camera.frustumPlanes[p].d;
            valid = valid || camera.frustumPlanes[p].normal != Vector3(0, 0, 0);
        }
        planes.skipFar = camera.farClip == 0;
        planes.entity = cameras->getEntity(c);
        planes.usable = valid && camera.enabled && !camera.editorView && scene->isEntityActiveInHierarchy(planes.entity);
        cameraPlanes.push_back(planes);
    }
}

bool CullingSystem::sphereVisible(const Vector3& center, float radius, Entity targetCamera, bool anyCamera) const {
    for (const CameraPlanes& planes : cameraPlanes) {
        if (!planes.usable) continue;
        if (!anyCamera && planes.entity != targetCamera) continue;
        bool inside = true;
        for (int p = 0; p < 6 && inside; p++) {
            if (p == 1 && planes.skipFar) continue;
            inside = planes.nx[p] * center.x + planes.ny[p] * center.y + planes.nz[p] * center.z + planes.d[p] >= -radius;
        }
        if (inside) return true;
    }
    return false;
}

Vector3 CullingSystem::worldPositionOf(Entity entity) const {
    const Transform* transform = scene->findComponent<Transform>(entity);
    return transform ? transform->worldPosition : Vector3(0, 0, 0);
}

Vector3 CullingSystem::referenceOf(const GroupSlot& group) const {
    if (group.referenceEntity != NULL_ENTITY && scene->isEntityCreated(group.referenceEntity)) return worldPositionOf(group.referenceEntity);
    if (group.hasReferencePoint) return group.referencePoint;
    if (group.targetCamera != NULL_ENTITY && scene->isEntityCreated(group.targetCamera)) return worldPositionOf(group.targetCamera);
    const Entity camera = scene->getCamera();
    if (camera != NULL_ENTITY && scene->isEntityCreated(camera)) return worldPositionOf(camera);
    return Vector3(0, 0, 0);
}

// ------------------------------------------------------------------ groups

CullingGroup CullingSystem::createGroup() {
    uint32_t slot = 0;
    while (slot < groups.size() && groups[slot].alive) slot++;
    if (slot >= CullingGroup::MAX_GROUPS) {
        Log::error("CullingGroup: a scene has at most %u Culling Groups; dispose() the ones no longer used", CullingGroup::MAX_GROUPS);
        return CullingGroup();
    }
    if (slot == groups.size()) groups.emplace_back();
    GroupSlot& group = groups[slot];
    const uint32_t generation = group.generation;
    group = GroupSlot();
    group.generation = generation;
    group.alive = true;
    liveGroups++;
    return CullingGroup(scene, slot, generation);
}

CullingSystem::GroupSlot* CullingSystem::findGroup(uint32_t slot, uint32_t generation) {
    if (slot >= groups.size() || !groups[slot].alive || groups[slot].generation != generation) return nullptr;
    return &groups[slot];
}

const CullingSystem::GroupSlot* CullingSystem::findGroup(uint32_t slot, uint32_t generation) const {
    if (slot >= groups.size() || !groups[slot].alive || groups[slot].generation != generation) return nullptr;
    return &groups[slot];
}

void CullingSystem::disposeGroup(uint32_t slot, uint32_t generation) {
    GroupSlot* group = findGroup(slot, generation);
    if (!group) return;
    if (group->owner) {
        if (std::shared_ptr<CullingGroupOwnership> state = ownershipOf(this, false)) {
            SubscriptionOwnerIndex::forget(group->owner, state.get(), ownershipId(slot, generation));
        }
    }
    const uint32_t next = group->generation + 1;
    *group = GroupSlot();
    group->generation = next == 0 ? 1 : next;
    group->alive = false;
    liveGroups--;
}

void CullingSystem::setGroupOwner(uint32_t slot, uint32_t generation, uint64_t ownerKey) {
    GroupSlot* group = findGroup(slot, generation);
    if (!group || ownerKey == 0) return;
    group->owner = ownerKey;
    SubscriptionOwnerIndex::add(ownerKey, ownershipOf(this, true), ownershipId(slot, generation));
}

void CullingSystem::setLuaCallback(uint32_t slot, uint32_t generation, std::shared_ptr<LuaFunction<void>> callback) {
    if (GroupSlot* group = findGroup(slot, generation)) group->luaCallback = std::move(callback);
}

bool CullingSystem::hasLuaCallback(uint32_t slot, uint32_t generation) const {
    const GroupSlot* group = findGroup(slot, generation);
    return group && group->luaCallback;
}

void CullingSystem::clearSubscriptions() {
    for (GroupSlot& group : groups) {
        group.forEachEvent([](auto& event) { event.clear(); });
        group.luaCallback.reset();
    }
    groundCheck = nullptr;
    // Stop: what the session made goes with the code that made it (the editor plays a scene in
    // place and edits it again): its groups, focuses and frozen GameObjects
    for (uint32_t slot = 0; slot < groups.size(); slot++) {
        if (groups[slot].alive) disposeGroup(slot, groups[slot].generation);
    }
    scriptFocuses.clear();
    cameraIsFocus = true;
    PhysicsSystem* physics = scene->getSystem<PhysicsSystem>().get();
    for (Entity entity = 0; entity < simFlags.size(); entity++) {
        if (simFlags[entity] & SimPhysicsFrozen) {
            physics->setSimulationFrozen3D(entity, false);
            physics->setSimulationFrozen2D(entity, false);
        }
    }
    simFlags.clear();
    reducedTimings.clear();
    pendingNew.clear();
    removedComponents.clear();
    auto components = scene->getComponentArray<SimulationCullingComponent>();
    for (size_t i = 0; i < components->size(); i++) {
        SimulationCullingComponent& component = components->getComponentFromIndex(i);
        component.state = SimulationState::Full;
        component.reason = SimulationCullReason::None;
        component.evaluated = false;
        component.groundChecked = false;
    }
    started = false;
    if (registeredActive) {
        auto& list = activeCulling();
        list.erase(std::remove_if(list.begin(), list.end(), [this](const ActiveCulling& a) { return a.system == this; }), list.end());
        detail::simulationCullingActive.fetch_sub(1, std::memory_order_relaxed);
        registeredActive = false;
    }
}

void CullingSystem::removeSubscriptionsByTag(const std::string& substring) {
    for (GroupSlot& group : groups) {
        group.forEachEvent([&substring](auto& event) { event.removeByTagSubstring(substring); });
    }
}

void CullingSystem::evaluateGroups() {
    for (uint32_t slot = 0; slot < groups.size(); slot++) {
        GroupSlot& group = groups[slot];
        if (!group.alive || !group.enabled) continue;
        evaluateGroup(slot, group);
        dispatchGroupEvents(slot);
    }
}

void CullingSystem::updateFollows(GroupSlot& group) {
    // one lookup each: a destroyed GameObject has no Transform left
    auto transforms = scene->getComponentArray<Transform>();
    for (size_t f = 0; f < group.follows.size();) {
        Follow& follow = group.follows[f];
        const Transform* transform = transforms->findComponent(follow.entity);
        if (!transform) {
            if (!group.followWarned) {
                group.followWarned = true;
                Log::warn("CullingGroup: the GameObject sphere %u followed was destroyed; the sphere keeps its last position", follow.index);
            }
            // the sphere stops following: the last follow takes this one's slot
            group.meta[follow.index] &= STATE_MASK;
            if (f + 1 != group.follows.size()) {
                group.follows[f] = group.follows.back();
                group.meta[group.follows[f].index] = (group.meta[group.follows[f].index] & STATE_MASK) | (static_cast<uint32_t>(f + 1) << 8);
            }
            group.follows.pop_back();
            continue;
        }
        const Vector3 position = transform->worldPosition + follow.offset;
        group.x[follow.index] = position.x;
        group.y[follow.index] = position.y;
        group.z[follow.index] = position.z;
        f++;
    }
}

void CullingSystem::evaluateGroup(uint32_t slot, GroupSlot& group) {
    (void)slot;
    const uint32_t count = static_cast<uint32_t>(group.x.size());
    // a group evaluated on the workers reads its follows there, in the same chunks (R22: a serial
    // lookup each was a third of a 10,000-sphere evaluation); the others first, here
    const bool parallel = count > PARALLEL_THRESHOLD;
    if (!parallel) updateFollows(group);
    group.events.clear();
    if (count == 0) return;
    const Vector3 reference = referenceOf(group);
    const bool anyCamera = group.targetCamera == NULL_ENTITY;
    evaluatedSpheres += count;
    if (parallel) {
        const size_t chunks = (count + PARALLEL_CHUNK - 1) / PARALLEL_CHUNK;
        if (group.chunkEvents.size() < chunks) group.chunkEvents.resize(chunks);
        std::atomic<bool> gone{false};
        std::atomic<bool>* follows = group.follows.empty() ? nullptr : &gone;
        jobs::ParallelForOptions options;
        // single-threaded (web, JE_ECS_SERIAL) or a test asking for it: every chunk on this thread
        if (!parallelEvaluation) options.maxHelpers = 0;
        jobs::parallelFor(chunks, [&](size_t chunk) {
            std::vector<CullingGroupEvent>& out = group.chunkEvents[chunk];
            out.clear();
            const uint32_t begin = static_cast<uint32_t>(chunk * PARALLEL_CHUNK);
            evaluateRange(group, begin, std::min(count, begin + PARALLEL_CHUNK), reference, group.targetCamera, anyCamera, out, follows);
        }, options);
        for (size_t chunk = 0; chunk < chunks; chunk++) {
            group.events.insert(group.events.end(), group.chunkEvents[chunk].begin(), group.chunkEvents[chunk].end());
        }
        // a destroyed GameObject's sphere was evaluated where it was, as it is from now on: the
        // follow is dropped (and warned about) as before
        if (gone.load(std::memory_order_relaxed)) updateFollows(group);
    } else {
        evaluateRange(group, 0, count, reference, group.targetCamera, anyCamera, group.events);
    }
}

void CullingSystem::evaluateRange(GroupSlot& group, uint32_t begin, uint32_t end, const Vector3& reference, Entity camera, bool anyCamera,
                                  std::vector<CullingGroupEvent>& out, std::atomic<bool>* gone) {
    // lookups only (nothing writes the Transforms meanwhile), each sphere written by its own chunk
    if (gone) {
        auto transforms = scene->getComponentArray<Transform>();
        for (uint32_t i = begin; i < end; i++) {
            const uint32_t follow = group.meta[i] >> 8;
            if (follow == 0) continue;
            const Follow& target = group.follows[follow - 1];
            const Transform* transform = transforms->findComponent(target.entity);
            if (!transform) {
                gone->store(true, std::memory_order_relaxed);
                continue;
            }
            const Vector3 position = transform->worldPosition + target.offset;
            group.x[i] = position.x;
            group.y[i] = position.y;
            group.z[i] = position.z;
        }
    }
    const float* distances = group.distances.data();
    const uint32_t distanceCount = group.distanceCount;
    const float hysteresis = group.hysteresis;
    // the cameras this group tests, chosen once (R22: a sphere call each, with a branch per plane,
    // was most of the evaluation); more than a few take sphereVisible's loop
    constexpr size_t kInlineCameras = 4;
    const CameraPlanes* tested[kInlineCameras];
    size_t testedCount = 0;
    bool inlineCameras = true;
    for (const CameraPlanes& planes : cameraPlanes) {
        if (!planes.usable || (!anyCamera && planes.entity != camera)) continue;
        if (testedCount == kInlineCameras) {
            inlineCameras = false;
            break;
        }
        tested[testedCount++] = &planes;
    }
    for (uint32_t i = begin; i < end; i++) {
        const float dx = group.x[i] - reference.x, dy = group.y[i] - reference.y, dz = group.z[i] - reference.z;
        const float radius = group.r[i];
        const float distance = std::max(0.0f, std::sqrt(dx * dx + dy * dy + dz * dz) - radius);
        const uint32_t old = group.meta[i] & STATE_MASK;
        const uint32_t oldBand = old & BAND_MASK;
        uint32_t band = 0;
        for (uint32_t k = 0; k < distanceCount; k++) band += distance >= distances[k] ? 1u : 0u;
        // outward only past the edge by more than the hysteresis
        if (hysteresis > 0.0f && oldBand != BAND_UNKNOWN && band > oldBand) {
            uint32_t outward = 0;
            for (uint32_t k = 0; k < distanceCount; k++) outward += distance > distances[k] + hysteresis ? 1u : 0u;
            band = std::max(oldBand, outward);
        }
        bool visible = false;
        if (inlineCameras) {
            const float x = group.x[i], y = group.y[i], z = group.z[i];
            for (size_t c = 0; c < testedCount && !visible; c++) {
                const CameraPlanes& planes = *tested[c];
                // every plane tested, the results joined without a branch (the same as stopping at
                // the first plane that fails); the far plane passes when the camera has none
                bool inside = true;
                for (int p = 0; p < 6; p++) {
                    const bool in = planes.nx[p] * x + planes.ny[p] * y + planes.nz[p] * z + planes.d[p] >= -radius;
                    inside &= in || (p == 1 && planes.skipFar);
                }
                visible = inside;
            }
        } else {
            visible = sphereVisible(Vector3(group.x[i], group.y[i], group.z[i]), radius, camera, anyCamera);
        }
        const uint32_t state = band | (visible ? VISIBLE_BIT : 0u);
        if (state != old) {
            CullingGroupEvent event;
            event.index = i;
            event.isVisible = visible;
            event.wasVisible = (old & VISIBLE_BIT) != 0;
            event.currentDistance = static_cast<int8_t>(band);
            event.previousDistance = bandOf(old);
            out.push_back(event);
            group.meta[i] = (group.meta[i] & ~STATE_MASK) | state;
        }
    }
}

void CullingSystem::dispatchGroupEvents(uint32_t slot) {
    const uint32_t generation = groups[slot].generation;
    for (size_t e = 0;; e++) {
        // read again each time: a callback may dispose the group or make others (which may move it)
        GroupSlot* group = findGroup(slot, generation);
        if (!group || e >= group->events.size()) return;
        const CullingGroupEvent event = group->events[e];
        if (group->onStateChanged && group->onStateChanged->hasSubscribers()) {
            group->onStateChanged->call(event);
            group = findGroup(slot, generation);
            if (!group) return;
        }
        if (group->luaCallback) {
            const std::shared_ptr<LuaFunction<void>> callback = group->luaCallback;
            (*callback)(static_cast<int>(event.index) + 1, event.isVisible, event.wasVisible,
                        static_cast<int>(event.currentDistance), static_cast<int>(event.previousDistance));
        }
    }
}

// ------------------------------------------------------------------ Simulation Culling

int CullingSystem::addFocus(const Vector3& position, Entity entity) {
    if (scriptFocuses.size() >= static_cast<size_t>(SimulationCulling::MAX_SCRIPT_FOCUSES)) return 0;
    ScriptFocus focus;
    focus.id = nextFocusId++;
    focus.position = position;
    focus.entity = entity;
    scriptFocuses.push_back(focus);
    return focus.id;
}

bool CullingSystem::setFocus(int id, const Vector3& position) {
    for (ScriptFocus& focus : scriptFocuses) {
        if (focus.id != id) continue;
        focus.position = position;
        focus.entity = NULL_ENTITY;
        return true;
    }
    return false;
}

bool CullingSystem::removeFocus(int id) {
    const size_t before = scriptFocuses.size();
    scriptFocuses.erase(std::remove_if(scriptFocuses.begin(), scriptFocuses.end(), [id](const ScriptFocus& f) { return f.id == id; }),
                        scriptFocuses.end());
    return scriptFocuses.size() != before;
}

void CullingSystem::setCameraIsFocus(bool value) {
    cameraIsFocus = value;
}

void CullingSystem::setGroundCheck(std::function<bool(const Vector3&, float)> check) {
    groundCheck = check ? std::make_shared<std::function<bool(const Vector3&, float)>>(std::move(check)) : nullptr;
}

void CullingSystem::gatherFocuses() {
    focusPoints.clear();
    focusEntities.clear();
    if (scriptFocuses.empty() || cameraIsFocus) {
        const Entity camera = scene->getCamera();
        if (camera != NULL_ENTITY && scene->isEntityCreated(camera)) {
            focusPoints.push_back(worldPositionOf(camera));
            focusEntities.push_back(camera);
        }
    }
    for (const ScriptFocus& focus : scriptFocuses) {
        if (focus.entity != NULL_ENTITY) {
            if (!scene->isEntityCreated(focus.entity)) continue;
            focusPoints.push_back(worldPositionOf(focus.entity));
            focusEntities.push_back(focus.entity);
        } else {
            focusPoints.push_back(focus.position);
        }
    }
}

void CullingSystem::ensureSimFlags(Entity entity) {
    if (entity < simFlags.size()) return;
    // the scene's ids now (and room for what is created next), grown when an id passes it
    const size_t wanted = std::max<size_t>(static_cast<size_t>(entity) + 1, static_cast<size_t>(scene->getLastEntity()) + 1);
    simFlags.resize(wanted, 0);
}

float CullingSystem::resolveRadius(Entity entity, SimulationCullingComponent& component) {
    if (component.boundsRadius > 0.0f) return component.boundsRadius;
    if (component.resolvedRadius >= 0.0f) return component.resolvedRadius;
    float radius = -1.0f;
    if (const MeshComponent* mesh = scene->findComponent<MeshComponent>(entity)) {
        AABB bounds = mesh->worldAABB;
        if (bounds.isNull() && !mesh->aabb.isNull() && !(mesh->aabb == AABB::ZERO)) {
            if (const Transform* transform = scene->findComponent<Transform>(entity)) {
                bounds = mesh->aabb;
                bounds.transform(transform->modelMatrix);
            }
        }
        if (!bounds.isNull() && !bounds.isInfinite()) radius = bounds.getHalfSize().length();
    }
#ifdef JASPERENGINE_PHYSICS_3D
    if (radius < 0.0f) {
        const RigidbodyComponent* body = scene->findComponent<RigidbodyComponent>(entity);
        PhysicsSystem* physics = scene->getSystem<PhysicsSystem>().get();
        if (body && !body->body.IsInvalid() && physics->hasWorld3D()) {
            const JPH::AABox box = physics->getBodyInterface3D().GetTransformedShape(body->body).GetWorldSpaceBounds();
            if (box.IsValid()) radius = 0.5f * (box.mMax - box.mMin).Length();
        }
    }
#endif
    component.resolvedRadius = radius >= 0.0f ? radius : 0.5f;
    return component.resolvedRadius;
}

bool CullingSystem::groundLoaded(TerrainWorldSystem* worlds, const Vector3& position, float radius, bool withinDistance, bool all, uint32_t& scriptChecks) {
    bool loaded = true;
#if JASPERENGINE_TERRAIN_WORLD
    if (worlds) loaded = worlds->isAreaLoaded(position.x, position.z, radius + 1.0f);
#else
    (void)worlds;
#endif
    // the game's own streaming: only within simulation distance (a far one is culled anyway)
    if (loaded && groundCheck && withinDistance && (all || scriptChecks < MAX_GROUND_CHECKS_PER_FRAME)) {
        scriptChecks++;
        // its own copy: the check may set another one, or nil, while it runs
        const std::shared_ptr<std::function<bool(const Vector3&, float)>> check = groundCheck;
        loaded = (*check)(position, radius);
    }
    return loaded;
}

void CullingSystem::evaluateSimulation(bool all, const std::vector<Entity>* only) {
    // a script's ground check runs inside the loop below, which holds a component of the array:
    // what it destroys goes after the transitions (StructuralDeferScope), as for other Lua callbacks
    std::optional<StructuralDeferScope> deferScope;
    if (groundCheck) deferScope.emplace(*scene);
    auto components = scene->getComponentArray<SimulationCullingComponent>();
    const float scale = SimulationCulling::getDistanceScale() * QualityResolver::simulationDistanceScale();
    float tileSize = 0.0f;
    bool terrain = false;
    // looked up once a pass (a lookup per ground check was most of R22's ground rule)
    TerrainWorldSystem* terrainWorlds = nullptr;
#if JASPERENGINE_TERRAIN_WORLD
    if (auto worlds = scene->findSystem<TerrainWorldSystem>(); worlds && worlds->hasWorlds()) {
        terrain = true;
        terrainWorlds = worlds.get();
        tileSize = worlds->getTileSize();
    }
#endif
    transitions.clear();
    uint32_t scriptChecks = 0;
    uint32_t full = 0, reducedCount = 0, culled = 0, ground = 0;
    const size_t total = only ? only->size() : components->size();
    for (size_t n = 0; n < total; n++) {
        Entity entity;
        SimulationCullingComponent* found;
        size_t ordinal = n;
        if (only) {
            entity = (*only)[n];
            found = scene->findComponent<SimulationCullingComponent>(entity);
            if (!found) continue;
        } else {
            entity = components->getEntity(n);
            found = &components->getComponentFromIndex(n);
        }
        SimulationCullingComponent& component = *found;
        const bool active = component.enabled && scene->isEntityActiveInHierarchy(entity);
        if (!active) {
            component.focusDistance = 0.0f;
            if (component.state != SimulationState::Full) transitions.push_back({entity, SimulationState::Full, SimulationCullReason::None, 0.0f, true});
            continue;
        }
        const Vector3 position = worldPositionOf(entity);
        const float radius = resolveRadius(entity, component);
        bool isFocus = false;
        float distance = focusPoints.empty() ? NO_FOCUS_DISTANCE : std::numeric_limits<float>::infinity();
        for (size_t f = 0; f < focusPoints.size(); f++) {
            distance = std::min(distance, std::max(0.0f, (position - focusPoints[f]).length() - radius));
        }
        for (Entity focus : focusEntities) isFocus = isFocus || focus == entity;
        component.focusDistance = distance;

        SimulationState state = SimulationState::Full;
        SimulationCullReason reason = SimulationCullReason::None;
        if (!isFocus) {
            const float simulation = std::max(0.0f, component.simulationDistance) * scale;
            const float reducedEdge = std::max(0.0f, component.reducedRateDistance) * scale;
            const bool band = reducedEdge > 0.0f && reducedEdge < simulation;
            const float fullEdge = band ? reducedEdge : simulation;
            auto level = [&](float slack) {
                if (distance <= fullEdge + slack) return 0;
                if (band && distance <= simulation + slack) return 1;
                return 2;
            };
            const int current = component.state == SimulationState::Full ? 0 : (component.state == SimulationState::Reduced ? 1 : 2);
            int wanted = level(0.0f);
            // outward only past the edge by more than the hysteresis
            if (wanted > current) wanted = std::max(current, level(std::max(0.0f, component.hysteresis)));
            if (wanted == 2 && component.simulateWhenVisible && sphereVisible(position, radius, NULL_ENTITY, true)) wanted = band ? 1 : 0;
            state = wanted == 0 ? SimulationState::Full : (wanted == 1 ? SimulationState::Reduced : SimulationState::Culled);
            if (state == SimulationState::Culled) reason = SimulationCullReason::Distance;

            if (component.freezeWhenGroundNotLoaded && (terrain || groundCheck)) {
                const bool moved = tileSize > 0.0f && component.groundChecked &&
                    std::max(std::fabs(position.x - component.groundCheckX), std::fabs(position.z - component.groundCheckZ)) > tileSize;
                // one held by unloaded ground is checked every frame: it resumes as soon as its tile is in
                const bool due = all || !component.groundChecked || !component.groundLoaded || moved ||
                                 ((frameCounter + ordinal + groundCursor) % 8) == 0;
                if (due) {
                    component.groundLoaded = groundLoaded(terrainWorlds, position, radius, distance <= simulation, all, scriptChecks);
                    component.groundChecked = true;
                    component.groundCheckX = position.x;
                    component.groundCheckZ = position.z;
                }
                if (!component.groundLoaded) {
                    state = SimulationState::Culled;
                    reason = SimulationCullReason::GroundNotLoaded;
                }
            }
        }
        if (state != component.state || reason != component.reason) {
            const bool now = reason == SimulationCullReason::GroundNotLoaded || state == component.state;
            transitions.push_back({entity, state, reason, distance, now});
        } else {
            component.evaluated = true;
        }
    }
    groundCursor++;

    // ground culls, reason-only changes and disabled components at once; the rest 64 a frame,
    // the nearest resumes first, then the farthest culls
    uint32_t applied = 0;
    uint32_t deferred = 0;
    if (all) {
        for (const Transition& t : transitions) applyTransition(t);
    } else {
        std::vector<Transition> paced;
        for (const Transition& t : transitions) {
            if (t.now) applyTransition(t);
            else paced.push_back(t);
        }
        auto rank = [this](const Transition& t) {
            const SimulationCullingComponent* c = scene->findComponent<SimulationCullingComponent>(t.entity);
            const bool resume = c && c->state == SimulationState::Culled;
            return resume ? 0 : (t.state == SimulationState::Culled ? 1 : 2);
        };
        std::stable_sort(paced.begin(), paced.end(), [&](const Transition& a, const Transition& b) {
            const int ra = rank(a), rb = rank(b);
            if (ra != rb) return ra < rb;
            if (ra == 1) return a.distance > b.distance;
            return a.distance < b.distance;
        });
        for (const Transition& t : paced) {
            if (applied < MAX_TRANSITIONS_PER_FRAME) {
                applyTransition(t);
                applied++;
            } else {
                deferred++;
            }
        }
    }
    if (!only) stats.transitionsDeferred = deferred;
    deferScope.reset();

    for (size_t i = 0; i < components->size(); i++) {
        const SimulationCullingComponent& c = components->getComponentFromIndex(i);
        if (c.state == SimulationState::Full) full++;
        else if (c.state == SimulationState::Reduced) reducedCount++;
        else {
            culled++;
            if (c.reason == SimulationCullReason::GroundNotLoaded) ground++;
        }
    }
    stats.components = static_cast<uint32_t>(components->size());
    stats.full = full;
    stats.reduced = reducedCount;
    stats.culled = culled;
    stats.groundNotLoaded = ground;
    stats.focuses = static_cast<uint32_t>(focusPoints.size());
    (void)terrain;
}

void CullingSystem::applyTransition(const Transition& transition) {
    SimulationCullingComponent* component = scene->findComponent<SimulationCullingComponent>(transition.entity);
    if (!component) return;
    const SimulationState previous = component->state;
    component->state = transition.state;
    component->reason = transition.reason;
    component->evaluated = true;
    if (previous != transition.state) applyFlags(transition.entity, *component, transition.state);
    if (previous != SimulationState::Culled && transition.state == SimulationState::Culled) {
        messages.push_back({transition.entity, true, transition.reason});
    } else if (previous == SimulationState::Culled && transition.state != SimulationState::Culled) {
        messages.push_back({transition.entity, false, SimulationCullReason::None});
    }
}

void CullingSystem::applyFlags(Entity root, const SimulationCullingComponent& component, SimulationState state) {
    uint8_t flags = 0;
    if (state == SimulationState::Culled) {
        if (component.cullScripts) flags |= SimScriptsCulled;
        if (component.cullAnimator) flags |= SimAnimatorCulled;
        if (component.cullPhysics) flags |= SimPhysicsFrozen;
    } else if (state == SimulationState::Reduced && component.cullScripts) {
        flags |= SimReduced;
    }
    const uint8_t interval = static_cast<uint8_t>(std::clamp(component.reducedUpdateInterval, 2, 60));
    auto transforms = scene->getComponentArray<Transform>();
    if (!transforms->hasEntity(root)) {
        setEntityFlags(root, flags, interval, true);
        return;
    }
    // the GameObject and its descendants, but the branches with a Simulation Culling of their own
    const size_t first = transforms->getIndex(root);
    const size_t last = scene->findBranchLastIndex(root);
    for (size_t i = first; i <= last && i < transforms->size(); i++) {
        const Entity entity = transforms->getEntity(i);
        if (i != first && scene->findComponent<SimulationCullingComponent>(entity)) {
            i = scene->findBranchLastIndex(entity);
            continue;
        }
        setEntityFlags(entity, flags, interval, true);
    }
}

void CullingSystem::setEntityFlags(Entity entity, uint8_t flags, uint8_t interval, bool physics) {
    ensureSimFlags(entity);
    const uint8_t previous = simFlags[entity];
    simFlags[entity] = flags;
    if ((flags & SimReduced) && !(previous & SimReduced)) {
        if (reducedTimings.size() < simFlags.size()) reducedTimings.resize(simFlags.size());
        // entering the band resets its clock: the first reduced call gets the time since now
        ReducedTiming& timing = reducedTimings[entity];
        timing.interval = interval;
        timing.lastTime = Engine::getGameTime();
        timing.frame = Engine::getGameFrames();
        timing.delta = Engine::getDeltatime();
    } else if (flags & SimReduced) {
        reducedTimings[entity].interval = interval;
    }
    if (physics && ((previous ^ flags) & SimPhysicsFrozen)) {
        const bool frozen = (flags & SimPhysicsFrozen) != 0;
        PhysicsSystem* system = scene->getSystem<PhysicsSystem>().get();
        system->setSimulationFrozen3D(entity, frozen);
        system->setSimulationFrozen2D(entity, frozen);
    }
}

void CullingSystem::sendMessages() {
    if (messages.empty()) return;
    std::sort(messages.begin(), messages.end(), [](const Message& a, const Message& b) { return a.entity < b.entity; });
    // a callback may cause more (it moves a focus): they wait for the next evaluation
    std::vector<Message> sending;
    sending.swap(messages);
    for (const Message& message : sending) {
        if (!scene->isEntityCreated(message.entity) || !scene->findComponent<ScriptComponent>(message.entity)) continue;
        if (message.culled) {
            const std::string reason = SimulationCulling::reasonName(message.reason);
            ScriptMessages::send(scene, message.entity, ScriptMessage::SimulationCulled,
                [reason](lua_State* L) { lua_pushstring(L, reason.c_str()); return 1; },
                [reason](ScriptBase& script) { script.onSimulationCulled(reason); }, DeliverRequireActive);
        } else {
            ScriptMessages::send(scene, message.entity, ScriptMessage::SimulationResumed, nullptr,
                [](ScriptBase& script) { script.onSimulationResumed(); }, DeliverRequireActive);
        }
    }
    sending.clear();
    if (messages.empty()) messages.swap(sending);
}

// ------------------------------------------------------------------ stats and debug

CullingSystem::Stats CullingSystem::getStats() const {
    Stats out = stats;
    out.groups = 0;
    out.spheres = 0;
    out.visible = 0;
    out.perBand.fill(0);
    for (const GroupSlot& group : groups) {
        if (!group.alive) continue;
        out.groups++;
        out.spheres += static_cast<uint32_t>(group.meta.size());
        if (!group.enabled) continue;
        for (uint32_t meta : group.meta) {
            const uint32_t band = meta & BAND_MASK;
            if (meta & VISIBLE_BIT) out.visible++;
            if (band != BAND_UNKNOWN && band < out.perBand.size()) out.perBand[band]++;
        }
    }
    return out;
}

size_t CullingSystem::getMemoryBytes() const {
    size_t bytes = 0;
    for (const GroupSlot& group : groups) {
        bytes += (group.x.capacity() + group.y.capacity() + group.z.capacity() + group.r.capacity()) * sizeof(float);
        bytes += group.meta.capacity() * sizeof(uint32_t);
        bytes += group.follows.capacity() * sizeof(Follow);
    }
    return bytes;
}

void CullingSystem::setDebugDraw(bool draw) {
    cullingDebugDraw = draw;
}

bool CullingSystem::isDebugDraw() {
    return cullingDebugDraw;
}

size_t CullingSystem::getDebugLineCount() const {
    if (!debugLinesShown || debugLinesEntity == NULL_ENTITY || !scene->isEntityCreated(debugLinesEntity)) return 0;
    const LineRendererComponent* lines = scene->findComponent<LineRendererComponent>(debugLinesEntity);
    return lines ? lines->lines.size() : 0;
}

void CullingSystem::updateDebugLines() {
    if (!cullingDebugDraw || paused) {
        if (debugLinesShown && debugLinesEntity != NULL_ENTITY && scene->isEntityCreated(debugLinesEntity)) {
            LineRenderer lines(scene, debugLinesEntity);
            lines.clearLines();
            lines.setVisible(false);
        }
        debugLinesShown = false;
        return;
    }
    if (debugLinesEntity == NULL_ENTITY || !scene->isEntityCreated(debugLinesEntity) || !scene->findComponent<LineRendererComponent>(debugLinesEntity)) {
        debugLinesEntity = scene->createSystemEntity();
        if (debugLinesEntity == NULL_ENTITY) return;
        scene->addComponent<Transform>(debugLinesEntity, Transform());
        scene->addComponent<LineRendererComponent>(debugLinesEntity, LineRendererComponent());
        scene->setEntityName(debugLinesEntity, "Culling Debug Lines");
    }
    LineRenderer lines(scene, debugLinesEntity);
    lines.clearLines();
    lines.setVisible(true);
    debugLinesShown = true;

    auto components = scene->getComponentArray<SimulationCullingComponent>();
    for (size_t i = 0; i < components->size(); i++) {
        SimulationCullingComponent& c = components->getComponentFromIndex(i);
        const Entity entity = components->getEntity(i);
        const Vector4& color = c.state == SimulationState::Full ? colorFull
            : (c.state == SimulationState::Reduced ? colorReduced
            : (c.reason == SimulationCullReason::GroundNotLoaded ? colorCulledGround : colorCulledDistance));
        wireSphere(lines, worldPositionOf(entity), c.boundsRadius > 0.0f ? c.boundsRadius : std::max(c.resolvedRadius, 0.5f), color);
    }
    for (const GroupSlot& group : groups) {
        if (!group.alive) continue;
        const uint32_t count = std::min<uint32_t>(static_cast<uint32_t>(group.x.size()), DEBUG_SPHERES_PER_GROUP);
        for (uint32_t i = 0; i < count; i++) {
            wireSphere(lines, Vector3(group.x[i], group.y[i], group.z[i]), group.r[i], (group.meta[i] & VISIBLE_BIT) ? colorGroupVisible : colorGroupHidden);
        }
    }
    for (const Vector3& focus : focusPoints) {
        const float s = 0.5f;
        lines.addLine(focus - Vector3(s, 0, 0), focus + Vector3(s, 0, 0), colorFocus);
        lines.addLine(focus - Vector3(0, s, 0), focus + Vector3(0, s, 0), colorFocus);
        lines.addLine(focus - Vector3(0, 0, s), focus + Vector3(0, 0, s), colorFocus);
    }
}
