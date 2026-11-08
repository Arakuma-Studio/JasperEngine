// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "subsystem/TerrainWorldSystem.h"

#include "Engine.h"
#include "Scene.h"
#include "component/TerrainComponent.h"
#include "component/TerrainWorldComponent.h"
#include "component/Transform.h"
#include "physics/TerrainWorldCollision.h"
#include "profiler/Profile.h"
#include "subsystem/TerrainWorldRender.h"
#include "Log.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::terrain;

namespace {

    TerrainWorldSystem::Host& worldHost() {
        static TerrainWorldSystem::Host host;
        return host;
    }

    // every live system, for the pump; main thread only
    std::vector<TerrainWorldSystem*>& liveSystems() {
        static std::vector<TerrainWorldSystem*> systems;
        return systems;
    }

    double& lastPumpAll() {
        static double last = -1.0;
        return last;
    }

    // R15: the camera's velocity, from its last two positions
    struct FocusTrack {
        Vector3 last;
        double at = -1.0;
    };

    std::map<std::pair<const Scene*, Entity>, FocusTrack>& focusTracks() {
        static std::map<std::pair<const Scene*, Entity>, FocusTrack> tracks;
        return tracks;
    }

}

void TerrainWorldSystem::setHost(Host host) {
    worldHost() = std::move(host);
}

TerrainWorldSystem::TerrainWorldSystem(Scene* scene): SubSystem(scene) {
    declareHookType(scene->getComponentId<TerrainWorldComponent>());
    declareHookType(scene->getComponentId<TerrainComponent>());
    liveSystems().push_back(this);
}

TerrainWorldSystem::~TerrainWorldSystem() {
    std::vector<TerrainWorldSystem*>& systems = liveSystems();
    systems.erase(std::remove(systems.begin(), systems.end(), this), systems.end());
    for (auto it = focusTracks().begin(); it != focusTracks().end();) {
        it = it->first.first == scene ? focusTracks().erase(it) : std::next(it);
    }
}

void TerrainWorldSystem::load() {}

void TerrainWorldSystem::destroy() {
    std::vector<Entity> entities;
    for (const auto& [entity, runtime] : runtimes) entities.push_back(entity);
    for (Entity entity : entities) release(entity);
    errors.clear();
    areaCallbacks.clear();
}

void TerrainWorldSystem::draw() {}

void TerrainWorldSystem::onComponentRemoved(Entity entity, ComponentId componentId) {
    (void)componentId;
    release(entity);
}

void TerrainWorldSystem::release(Entity entity) {
    const auto it = runtimes.find(entity);
    if (it == runtimes.end()) return;
    if (worldHost().released) worldHost().released(scene, entity, *it->second);
    if (TerrainComponent* terrain = scene->findComponent<TerrainComponent>(entity)) {
        if (terrain->world == it->second.get()) {
            terrain->world = nullptr;
            terrain->needUpdateTerrain = true;
        }
    }
    for (auto callback = areaCallbacks.begin(); callback != areaCallbacks.end();) {
        callback = callback->entity == entity ? areaCallbacks.erase(callback) : std::next(callback);
    }
    runtimes.erase(it);
}

void TerrainWorldSystem::restart(Entity entity) {
    release(entity);
    errors.erase(entity);
}

std::string TerrainWorldSystem::getError(Entity entity) const {
    const auto it = errors.find(entity);
    return it == errors.end() ? std::string() : it->second;
}

TerrainWorldRuntime* TerrainWorldSystem::getRuntime(Entity entity) {
    const auto it = runtimes.find(entity);
    return it == runtimes.end() ? nullptr : it->second.get();
}

TerrainWorldRuntime* TerrainWorldSystem::ensureRuntime(Entity entity, std::string* error) {
    if (TerrainWorldRuntime* runtime = getRuntime(entity)) return runtime;
    syncRuntimes();
    if (TerrainWorldRuntime* runtime = getRuntime(entity)) return runtime;
    if (error) {
        const std::string why = getError(entity);
        *error = why.empty() ? "the GameObject is not a Terrain World (it needs a Terrain and an enabled Terrain World)" : why;
    }
    return nullptr;
}

void TerrainWorldSystem::syncRuntimes() {
    auto worlds = scene->getComponentArray<TerrainWorldComponent>();
    // R37: a scene with no world stops here
    if (worlds->size() == 0 && runtimes.empty()) return;

    const Host& host = worldHost();
    // a scene the editor plays in place restarts its worlds: Play caps edits and waits differently (R29, R19)
    const bool playingNow = host.playing ? host.playing(scene) : true;
    std::vector<Entity> stale;
    for (const auto& [entity, runtime] : runtimes) {
        const TerrainWorldComponent* world = scene->findComponent<TerrainWorldComponent>(entity);
        if (!world || !world->enabled || !scene->findComponent<TerrainComponent>(entity) || runtime->playing() != playingNow) {
            stale.push_back(entity);
        }
    }
    for (Entity entity : stale) release(entity);

    for (int i = 0; i < worlds->size(); i++) {
        const TerrainWorldComponent& world = worlds->getComponentFromIndex(i);
        const Entity entity = worlds->getEntity(i);
        TerrainComponent* terrain = scene->findComponent<TerrainComponent>(entity);
        if (!world.enabled || !terrain || runtimes.count(entity) || errors.count(entity)) continue;

        std::shared_ptr<TerrainWorldSource> source;
        if (host.makeSource) {
            source = host.makeSource(scene, entity, world);
        } else if (!world.worldData.empty()) {
            source = std::make_shared<CookedWorldSource>(world.worldData.path);
        }
        auto runtime = std::make_unique<TerrainWorldRuntime>(scene, entity, source, playingNow);
        std::string error;
        if (!runtime->start(error)) {
            Log::error("Terrain World '%s' cannot start: %s", scene->getEntityName(entity).c_str(), error.c_str());
            errors[entity] = error;
            continue;
        }
        runtime->collision = std::make_unique<TerrainWorldCollision>(*runtime);
        terrain->world = runtime.get();
        // its grids are skirted and it has no node tree (MeshSystem::createTerrain)
        terrain->needUpdateTerrain = true;
        TerrainWorldRuntime* made = runtime.get();
        runtimes[entity] = std::move(runtime);
        if (host.started) host.started(scene, entity, *made);
    }
}

void TerrainWorldSystem::updateFocuses(TerrainWorldRuntime& runtime, const TerrainWorldComponent& component) {
    Vector3 origin;
    if (const Transform* transform = scene->findComponent<Transform>(runtime.getEntity())) origin = transform->worldPosition;
    const double now = Engine::getRealtimeSinceStartup();
    std::vector<StreamFocus> focuses;
    auto follow = [&](Entity entity, int id) {
        const Transform* transform = scene->findComponent<Transform>(entity);
        if (!transform) return;
        StreamFocus focus;
        focus.id = id;
        focus.x = transform->worldPosition.x - origin.x;
        focus.z = transform->worldPosition.z - origin.z;
        FocusTrack& track = focusTracks()[{scene, entity}];
        if (track.at >= 0.0 && now > track.at) {
            const float dt = static_cast<float>(now - track.at);
            focus.velocityX = (transform->worldPosition.x - track.last.x) / dt;
            focus.velocityZ = (transform->worldPosition.z - track.last.z) / dt;
            // a teleport is no velocity
            if (std::fabs(focus.velocityX) > 500.0f || std::fabs(focus.velocityZ) > 500.0f) focus.velocityX = focus.velocityZ = 0.0f;
        }
        track.last = transform->worldPosition;
        track.at = now;
        focuses.push_back(focus);
    };
    if (component.useCameraFocus && scene->getCamera() != NULL_ENTITY) follow(scene->getCamera(), -1);
    if (component.focus != NULL_ENTITY && scene->isEntityCreated(component.focus)) follow(component.focus, -2);
    runtime.setAutoFocuses(focuses);
}

void TerrainWorldSystem::forEachRuntime(const std::function<void(TerrainWorldRuntime& runtime)>& fn) {
    for (TerrainWorldSystem* system : liveSystems()) {
        for (auto& [entity, runtime] : system->runtimes) fn(*runtime);
    }
}

void TerrainWorldSystem::pumpAll() {
    // editor views draw a frame several times: the pump runs once (D9)
    const double now = Engine::getRealtimeSinceStartup();
    if (lastPumpAll() >= 0.0 && now - lastPumpAll() < 0.001) return;
    lastPumpAll() = now;
    for (TerrainWorldSystem* system : liveSystems()) system->pump(now);
}

void TerrainWorldSystem::pump(double now) {
    syncRuntimes();
    if (runtimes.empty()) return;
    JE_PROFILE_SCOPE(PROFILE_SCOPE);
    lastPump = now;
    for (auto& [entity, runtime] : runtimes) {
        const TerrainWorldComponent* component = scene->findComponent<TerrainWorldComponent>(entity);
        if (!component) continue;

        // R4: a world translates only
        if (Transform* transform = scene->findComponent<Transform>(entity)) {
            const bool rotated = transform->rotation != Quaternion();
            const bool scaled = transform->scale != Vector3(1.0f, 1.0f, 1.0f);
            if (rotated || scaled) {
                Log::warn("Terrain World '%s' translates only: its rotation and scale are set back to identity.", scene->getEntityName(entity).c_str());
                transform->rotation = Quaternion();
                transform->scale = Vector3(1.0f, 1.0f, 1.0f);
                transform->needUpdate = true;
            }
        }

        const WorldPlatformBudget budget = terrainWorldPlatformBudget();
        runtime->applySettings(component->poolSize, component->lodDistance, component->uploadBudget, budget.nearTiles);
        // R29: brushes, edit_terrain and generation make tiles; a game's Lua edits do not
        runtime->store().options().autoAddTiles = component->autoAddTiles && !runtime->playing();
        updateFocuses(*runtime, *component);
        const uint32_t overviewBefore = runtime->overviewRevision();
        const int residentBefore = runtime->tiles().stats().residentPages;
        runtime->pump(now);
        if (runtime->collision) {
            const float radius = component->collisionRadius > 0.0f ? component->collisionRadius : budget.collisionRadius;
            runtime->collision->pump(radius, component->collisionChunkQuads);
        }
        // D9: what draws changed, so the selection runs again this frame
        const bool pagesChanged = runtime->render && runtime->render->takeReselect();
        if (pagesChanged || runtime->overviewRevision() != overviewBefore || runtime->tiles().stats().residentPages != residentBefore) {
            if (Transform* transform = scene->findComponent<Transform>(entity)) transform->needUpdate = true;
        }
    }
}

int TerrainWorldSystem::onAreaReady(Entity entity, float x, float z, float radius, TerrainWorldReadyLevel level, std::function<void()> callback) {
    TerrainWorldRuntime* runtime = ensureRuntime(entity);
    if (!runtime || !callback) return 0;
    AreaCallback entry;
    entry.id = nextCallbackId++;
    entry.entity = entity;
    entry.wait = runtime->waitForArea(x, z, radius, level);
    entry.x = x;
    entry.z = z;
    entry.radius = radius;
    entry.level = level;
    entry.callback = std::move(callback);
    areaCallbacks.push_back(std::move(entry));
    return areaCallbacks.back().id;
}

void TerrainWorldSystem::cancelAreaReady(int id) {
    for (auto it = areaCallbacks.begin(); it != areaCallbacks.end(); ++it) {
        if (it->id != id) continue;
        if (TerrainWorldRuntime* runtime = getRuntime(it->entity)) runtime->dropAreaWait(it->wait);
        areaCallbacks.erase(it);
        return;
    }
}

void TerrainWorldSystem::update(double dt) {
    (void)dt;
    if (areaCallbacks.empty()) return;
    // R18: in the update phase, so a paused game hears of it on resume
    std::vector<AreaCallback> ready;
    for (auto it = areaCallbacks.begin(); it != areaCallbacks.end();) {
        TerrainWorldRuntime* runtime = getRuntime(it->entity);
        if (runtime && runtime->isAreaReady(it->x, it->z, it->radius, it->level)) {
            runtime->dropAreaWait(it->wait);
            ready.push_back(std::move(*it));
            it = areaCallbacks.erase(it);
        } else {
            ++it;
        }
    }
    for (AreaCallback& callback : ready) callback.callback();
}
