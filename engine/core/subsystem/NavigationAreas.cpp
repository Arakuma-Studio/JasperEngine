// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// NavigationSystem's runtime areas and links (docs/specs/S76-navigation-gameplay-api.md R11-R16,
// D1, D9): NavMesh Modifier volumes with Apply At Runtime, script volumes (setAreaInVolume) and
// NavMesh Link Activated, sampled from their components at each sync (every editor frame, fixed
// tick step 1b in play) and handed to each surface's navmesh as overrides and link states.

#include "NavigationAreas.h"

#include "Log.h"
#include "Scene.h"
#include "component/NavMeshLinkComponent.h"
#include "component/NavMeshModifierComponent.h"
#include "component/NavMeshSurfaceComponent.h"
#include "component/Transform.h"
#include "navigation/NavBakeCollector.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::nav;

namespace {

    // a volume moved this much is applied again (R11)
    constexpr float AREA_MOVE_EPSILON = 1e-3f;
    // cos of half of 0.1 degrees: |dot| of two rotations 0.1 degrees apart
    constexpr float AREA_TURN_COS = 0.99999996f;

    // modifier volumes first (by entity), then script volumes (by handle): R13's order
    uint64_t modifierOrder(Entity entity) { return (uint64_t)entity; }
    uint64_t handleOrder(uint32_t handle) { return (1ull << 32) | handle; }

    uint8_t runtimeArea(int area) {
        return (uint8_t)std::clamp(area, 0, AREA_COUNT - 1);
    }

    bool vectorMoved(const Vector3& a, const Vector3& b) {
        return a.distance(b) > AREA_MOVE_EPSILON;
    }

    bool rotationTurned(const Quaternion& a, const Quaternion& b) {
        return std::fabs(a.dot(b)) < AREA_TURN_COS;
    }

    // the world matrix of a box of `size` turned by `rotation` about `center`
    Matrix4 boxMatrix(const Vector3& center, const Quaternion& rotation) {
        return Matrix4::translateMatrix(center) * rotation.getRotationMatrix();
    }

}

void NavigationSystem::AreaState::forgetSurface(Entity surface) {
    for (auto& entry : modifiers)
        entry.second.ids.erase(surface);
    for (auto& entry : handles)
        entry.second.ids.erase(surface);
    surfaces.erase(surface);
    changed.erase(surface);
}

void NavigationSystem::onAreaMeshReplaced(Entity surface) {
    if (areas && areaSyncing) {
        // a sync loading the surface: it sees a mesh it did not add to, and adds again
        auto found = areas->surfaces.find(surface);
        if (found != areas->surfaces.end())
            found->second.mesh = nullptr;
        return;
    }
    if (areas)
        areas->forgetSurface(surface);
    syncAreaOverrides();
}

uint32_t NavigationSystem::setAreaInVolume(const Vector3& center, const Vector3& size, const Quaternion& rotation, uint8_t area, int agentType) {
    if (area >= AREA_COUNT) {
        Log::warn("Navigation: setAreaInVolume: area %d is not one of the 16 areas", (int)area);
        return 0;
    }
    if (!areas)
        areas = std::make_unique<AreaState>();
    const uint32_t handle = areas->nextHandle++;
    AreaState::Handle& entry = areas->handles[handle];
    entry.center = center;
    entry.size = size;
    entry.rotation = rotation;
    entry.area = area;
    entry.agentType = agentType;
    // a query right after the call already sees it (R14)
    syncAreaOverrides();
    return handle;
}

bool NavigationSystem::updateAreaOverride(uint32_t handle, const Vector3& center, const Vector3& size, const Quaternion& rotation, uint8_t area) {
    if (!areas || area >= AREA_COUNT)
        return false;
    auto found = areas->handles.find(handle);
    if (found == areas->handles.end())
        return false;
    AreaState::Handle& entry = found->second;
    const bool same = !vectorMoved(entry.center, center) && !vectorMoved(entry.size, size) && !rotationTurned(entry.rotation, rotation) &&
                      entry.area == area;
    if (same)
        return true;
    entry.center = center;
    entry.size = size;
    entry.rotation = rotation;
    entry.area = area;
    for (auto& id : entry.ids) {
        NavMesh* mesh = editableNavMesh(id.first);
        NavConvexVolume volume;
        if (mesh && NavBakeCollector::boxVolume(NavBakeCollector::frameOf(scene, id.first), boxMatrix(center, rotation), Vector3(), size, volume))
            mesh->updateAreaOverride(id.second, volume, area);
    }
    syncAreaOverrides();
    return true;
}

bool NavigationSystem::removeAreaOverride(uint32_t handle) {
    if (!areas)
        return false;
    auto found = areas->handles.find(handle);
    if (found == areas->handles.end())
        return false;
    for (auto& id : found->second.ids) {
        if (NavMesh* mesh = editableNavMesh(id.first))
            mesh->removeAreaOverride(id.second);
    }
    areas->handles.erase(found);
    syncAreaOverrides();
    return true;
}

bool NavigationSystem::hasAreaOverride(uint32_t handle) const {
    return areas && areas->handles.count(handle) > 0;
}

bool NavigationSystem::getAreaOverride(uint32_t handle, Vector3& center, Vector3& size, Quaternion& rotation, uint8_t& area) const {
    if (!areas)
        return false;
    auto found = areas->handles.find(handle);
    if (found == areas->handles.end())
        return false;
    center = found->second.center;
    size = found->second.size;
    rotation = found->second.rotation;
    area = found->second.area;
    return true;
}

bool NavigationSystem::syncAreaOverrides() {
    auto modifierArray = scene->getComponentArray<NavMeshModifierComponent>();
    auto linkArray = scene->getComponentArray<NavMeshLinkComponent>();
    if (!areas) {
        // nothing at runtime: nothing is made (R18)
        bool any = false;
        for (size_t i = 0; i < modifierArray->size() && !any; i++) {
            const NavMeshModifierComponent& modifier = modifierArray->getComponentFromIndex(i);
            any = modifier.useVolume && modifier.applyAtRuntime;
        }
        for (size_t i = 0; i < linkArray->size() && !any; i++)
            any = !linkArray->getComponentFromIndex(i).activated;
        if (!any)
            return false;
        areas = std::make_unique<AreaState>();
    }
    AreaState& state = *areas;
    struct Syncing {
        bool& flag;
        explicit Syncing(bool& flag): flag(flag) { flag = true; }
        ~Syncing() { flag = false; }
    } syncing(areaSyncing);

    // each surface's navmesh now: a replaced one holds none of the overrides
    for (auto it = state.surfaces.begin(); it != state.surfaces.end();) {
        if (!scene->findComponent<NavMeshSurfaceComponent>(it->first)) {
            const Entity gone = it->first;
            ++it;
            state.forgetSurface(gone);
        } else {
            ++it;
        }
    }
    auto surfaceArray = scene->getComponentArray<NavMeshSurfaceComponent>();
    for (size_t i = 0; i < surfaceArray->size(); i++) {
        const Entity entity = surfaceArray->getEntity(i);
        NavMesh* mesh = editableNavMesh(entity);
        auto known = state.surfaces.find(entity);
        if (known != state.surfaces.end() && known->second.mesh != mesh) {
            state.forgetSurface(entity);
            known = state.surfaces.end();
        }
        if (known == state.surfaces.end() && mesh)
            state.surfaces[entity].mesh = mesh;
    }

    // ---- modifier volumes (R11)
    for (auto it = state.modifiers.begin(); it != state.modifiers.end();) {
        const NavMeshModifierComponent* modifier = scene->findComponent<NavMeshModifierComponent>(it->first);
        if (modifier && modifier->useVolume && modifier->applyAtRuntime) {
            ++it;
            continue;
        }
        for (auto& id : it->second.ids) {
            if (NavMesh* mesh = editableNavMesh(id.first))
                mesh->removeAreaOverride(id.second);
        }
        it = state.modifiers.erase(it);
    }
    bool runtimeModifiers = false;
    for (size_t i = 0; i < modifierArray->size(); i++) {
        const NavMeshModifierComponent& component = modifierArray->getComponentFromIndex(i);
        if (!component.useVolume || !component.applyAtRuntime)
            continue;
        runtimeModifiers = true;
        const Entity entity = modifierArray->getEntity(i);
        const Transform* transform = scene->findComponent<Transform>(entity);
        const bool active = component.enabled && transform && scene->isEntityActiveInHierarchy(entity);
        auto found = state.modifiers.find(entity);
        if (!active && found == state.modifiers.end())
            continue;
        AreaState::Modifier& modifier = found != state.modifiers.end() ? found->second : state.modifiers[entity];
        const uint8_t area = runtimeArea(component.area);
        bool moved = !modifier.active;
        if (active) {
            moved = moved || vectorMoved(modifier.position, transform->worldPosition) || vectorMoved(modifier.scale, transform->worldScale) ||
                    rotationTurned(modifier.rotation, transform->worldRotation) || vectorMoved(modifier.center, component.volumeCenter) ||
                    vectorMoved(modifier.size, component.volumeSize) || modifier.area != area;
            modifier.position = transform->worldPosition;
            modifier.scale = transform->worldScale;
            modifier.rotation = transform->worldRotation;
            modifier.center = component.volumeCenter;
            modifier.size = component.volumeSize;
            modifier.area = area;
        }
        modifier.active = active;
        for (auto& surface : state.surfaces) {
            NavMesh* mesh = surface.second.mesh ? editableNavMesh(surface.first) : nullptr;
            if (!mesh)
                continue;
            auto id = modifier.ids.find(surface.first);
            if (!active) {
                if (id != modifier.ids.end()) {
                    mesh->removeAreaOverride(id->second);
                    modifier.ids.erase(id);
                }
                continue;
            }
            if (id != modifier.ids.end() && !moved)
                continue;
            NavConvexVolume volume;
            if (!NavBakeCollector::boxVolume(NavBakeCollector::frameOf(scene, surface.first), transform->modelMatrix, component.volumeCenter,
                                             component.volumeSize, volume))
                continue;
            if (id != modifier.ids.end())
                mesh->updateAreaOverride(id->second, volume, area);
            else
                modifier.ids[surface.first] = mesh->addAreaOverride(volume, area, modifierOrder(entity));
        }
        if (!active && modifier.ids.empty())
            state.modifiers.erase(entity);
    }

    // ---- script volumes (R12): the surfaces of their agent type that have none yet
    for (auto& entry : state.handles) {
        AreaState::Handle& handle = entry.second;
        for (auto& surface : state.surfaces) {
            if (handle.ids.count(surface.first))
                continue;
            const NavMeshSurfaceComponent* component = scene->findComponent<NavMeshSurfaceComponent>(surface.first);
            NavMesh* mesh = editableNavMesh(surface.first);
            if (!component || !mesh || (handle.agentType >= 0 && component->agentType != handle.agentType))
                continue;
            NavConvexVolume volume;
            if (NavBakeCollector::boxVolume(NavBakeCollector::frameOf(scene, surface.first), boxMatrix(handle.center, handle.rotation),
                                            Vector3(), handle.size, volume))
                handle.ids[surface.first] = mesh->addAreaOverride(volume, handle.area, handleOrder(entry.first));
        }
    }

    // ---- links (R16): a deactivated link's connections pass nothing, in edit mode too
    for (auto& surface : state.surfaces) {
        NavMesh* mesh = editableNavMesh(surface.first);
        if (!mesh)
            continue;
        std::set<Entity>& inactive = surface.second.inactiveLinks;
        for (auto it = inactive.begin(); it != inactive.end();) {
            const NavMeshLinkComponent* link = scene->findComponent<NavMeshLinkComponent>(*it);
            if (!link || link->activated) {
                mesh->setLinkActive((uint32_t)*it, true);
                it = inactive.erase(it);
            } else {
                ++it;
            }
        }
        for (size_t i = 0; i < linkArray->size(); i++) {
            const Entity entity = linkArray->getEntity(i);
            if (!linkArray->getComponentFromIndex(i).activated && inactive.insert(entity).second)
                mesh->setLinkActive((uint32_t)entity, false);
        }
    }

    // ---- what changed, for the crowds (step 1b): only surfaces with agents keep it
    bool changedAny = false;
    for (auto& surface : state.surfaces) {
        NavMesh* mesh = editableNavMesh(surface.first);
        if (!mesh)
            continue;
        mesh->takeChangedPolys(state.scratch);
        changedAny = changedAny || !state.scratch.empty();
        if (state.scratch.empty() || !hasAreaCrowd(surface.first))
            continue;
        std::vector<uint32_t>& list = state.changed[surface.first];
        list.insert(list.end(), state.scratch.begin(), state.scratch.end());
        std::sort(list.begin(), list.end());
        list.erase(std::unique(list.begin(), list.end()), list.end());
    }
    state.scratch.clear();

    // nothing at runtime any more: back to nothing (R18)
    bool inactiveLinks = false;
    for (const auto& surface : state.surfaces)
        inactiveLinks = inactiveLinks || !surface.second.inactiveLinks.empty();
    bool pending = false;
    for (const auto& list : state.changed)
        pending = pending || !list.second.empty();
    if (!runtimeModifiers && state.modifiers.empty() && state.handles.empty() && !inactiveLinks && !pending)
        areas.reset();
    return changedAny;
}

std::vector<NavigationSystem::AreaOverrideStatus> NavigationSystem::getAreaOverrides() {
    std::vector<AreaOverrideStatus> out;
    auto polygonsOf = [&](const std::map<Entity, uint32_t>& ids, bool& applied) {
        int polygons = 0;
        applied = false;
        for (const auto& id : ids) {
            NavMesh* mesh = editableNavMesh(id.first);
            const int count = mesh ? mesh->getAreaOverridePolyCount(id.second) : -1;
            if (count >= 0 && mesh->isValid()) {
                applied = true;
                polygons += count;
            }
        }
        return polygons;
    };
    // every runtime modifier volume, applied or not
    std::vector<Entity> entities;
    auto modifierArray = scene->getComponentArray<NavMeshModifierComponent>();
    for (size_t i = 0; i < modifierArray->size(); i++) {
        const NavMeshModifierComponent& component = modifierArray->getComponentFromIndex(i);
        if (component.useVolume && component.applyAtRuntime)
            entities.push_back(modifierArray->getEntity(i));
    }
    std::sort(entities.begin(), entities.end());
    for (Entity entity : entities) {
        const NavMeshModifierComponent& component = scene->getComponent<NavMeshModifierComponent>(entity);
        AreaOverrideStatus status;
        status.modifier = entity;
        status.area = runtimeArea(component.area);
        if (const Transform* transform = scene->findComponent<Transform>(entity))
            status.box = transform->modelMatrix * Matrix4::translateMatrix(component.volumeCenter) * Matrix4::scaleMatrix(component.volumeSize);
        if (areas) {
            auto found = areas->modifiers.find(entity);
            if (found != areas->modifiers.end())
                status.polygons = polygonsOf(found->second.ids, status.applied);
        }
        out.push_back(status);
    }
    if (areas) {
        for (const auto& entry : areas->handles) {
            AreaOverrideStatus status;
            status.handle = entry.first;
            status.area = entry.second.area;
            status.agentType = entry.second.agentType;
            status.box = boxMatrix(entry.second.center, entry.second.rotation) * Matrix4::scaleMatrix(entry.second.size);
            status.polygons = polygonsOf(entry.second.ids, status.applied);
            out.push_back(status);
        }
    }
    return out;
}

int NavigationSystem::getModifierAppliedPolygons(Entity modifier) {
    if (!areas)
        return 0;
    auto found = areas->modifiers.find(modifier);
    if (found == areas->modifiers.end() || !found->second.active)
        return 0;
    int polygons = 0;
    for (const auto& id : found->second.ids) {
        const NavMesh* mesh = editableNavMesh(id.first);
        const int count = mesh && mesh->isValid() ? mesh->getAreaOverridePolyCount(id.second) : -1;
        polygons += std::max(count, 0);
    }
    return polygons;
}

int NavigationSystem::getAreaOverridePolygons(uint32_t handle) {
    if (!areas)
        return 0;
    auto found = areas->handles.find(handle);
    if (found == areas->handles.end())
        return 0;
    int polygons = 0;
    for (const auto& id : found->second.ids) {
        const NavMesh* mesh = editableNavMesh(id.first);
        const int count = mesh && mesh->isValid() ? mesh->getAreaOverridePolyCount(id.second) : -1;
        polygons += std::max(count, 0);
    }
    return polygons;
}

bool NavigationSystem::isLinkActive(Entity link) const {
    if (const NavMeshLinkComponent* component = scene->findComponent<NavMeshLinkComponent>(link))
        return component->activated;
    return false;
}
