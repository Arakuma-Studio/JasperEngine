// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "NavMeshSurface.h"

#include "Log.h"
#include "Scene.h"
#include "subsystem/NavigationSystem.h"

#include <algorithm>

using namespace jasperengine;

NavMeshSurface::NavMeshSurface(Scene* scene, Entity entity): EntityHandle(scene, entity){
}

NavMeshSurface::~NavMeshSurface() = default;
NavMeshSurface::NavMeshSurface(const NavMeshSurface& rhs) = default;
NavMeshSurface& NavMeshSurface::operator=(const NavMeshSurface& rhs) = default;
NavMeshSurface::NavMeshSurface(NavMeshSurface&& rhs) noexcept = default;
NavMeshSurface& NavMeshSurface::operator=(NavMeshSurface&& rhs) noexcept = default;

NavMeshSurfaceComponent& NavMeshSurface::component() const{
    return getComponent<NavMeshSurfaceComponent>();
}

NavigationSystem* NavMeshSurface::system() const{
    return scene->getSystem<NavigationSystem>().get();
}

bool NavMeshSurface::isValid() const{
    return scene && scene->findComponent<NavMeshSurfaceComponent>(entity) != nullptr;
}

bool NavMeshSurface::bakeAsync(){
    std::string error;
    if (!system()->bake(entity, NavBakeMode::FULL, error)){
        Log::error("NavMeshSurface.bakeAsync: %s", error.c_str());
        return false;
    }
    return true;
}

bool NavMeshSurface::updateTiles(const AABB& worldBounds){
    std::string error;
    if (!system()->updateTiles(entity, worldBounds, error)){
        Log::error("NavMeshSurface.updateTiles: %s", error.c_str());
        return false;
    }
    return true;
}

void NavMeshSurface::clear(){
    std::string error;
    system()->bake(entity, NavBakeMode::CLEAR, error);
}

bool NavMeshSurface::isBaking() const{ return system()->isBaking(entity); }
float NavMeshSurface::getBakeProgress() const{ return system()->getSurfaceStatus(entity).progress; }

std::string NavMeshSurface::getLastBakeError() const{
    const NavBakeResult* result = system()->getLastBakeResult(entity);
    return result ? result->error : std::string();
}

const NavBakeResult* NavMeshSurface::getLastBakeResult() const{
    return system()->getLastBakeResult(entity);
}

int NavMeshSurface::getTileCount() const{ return system()->getSurfaceStatus(entity).tiles; }
int NavMeshSurface::getPolyCount() const{ return system()->getSurfaceStatus(entity).polys; }
bool NavMeshSurface::hasNavMesh() const{ return system()->getNavMesh(entity) != nullptr; }

void NavMeshSurface::setEnabled(bool enabled){ component().enabled = enabled; }
bool NavMeshSurface::isEnabled() const{ return component().enabled; }
void NavMeshSurface::setAgentTypeID(int agentType){ component().agentType = agentType; }
int NavMeshSurface::getAgentTypeID() const{ return component().agentType; }
void NavMeshSurface::setCollectObjects(NavCollectObjects collect){ component().collectObjects = collect; }
NavCollectObjects NavMeshSurface::getCollectObjects() const{ return component().collectObjects; }
void NavMeshSurface::setUseGeometry(NavUseGeometry geometry){ component().useGeometry = geometry; }
NavUseGeometry NavMeshSurface::getUseGeometry() const{ return component().useGeometry; }
void NavMeshSurface::setLayerMask(int mask){ component().includeLayers = (uint32_t)mask; }
int NavMeshSurface::getLayerMask() const{ return (int)component().includeLayers; }
void NavMeshSurface::setDefaultArea(int area){ component().defaultArea = std::clamp(area, 0, nav::AREA_COUNT - 1); }
int NavMeshSurface::getDefaultArea() const{ return component().defaultArea; }
void NavMeshSurface::setCenter(Vector3 center){ component().volumeCenter = center; }
Vector3 NavMeshSurface::getCenter() const{ return component().volumeCenter; }
void NavMeshSurface::setSize(Vector3 size){ component().volumeSize = size; }
Vector3 NavMeshSurface::getSize() const{ return component().volumeSize; }
void NavMeshSurface::setRuntimeCarving(bool carving){ component().runtimeCarving = carving; }
bool NavMeshSurface::isRuntimeCarving() const{ return component().runtimeCarving; }
void NavMeshSurface::setFollowTerrainWorldFocus(bool follow){ system()->setFollowTerrainWorldFocus(entity, follow); }
bool NavMeshSurface::isFollowTerrainWorldFocus() const{ return component().followTerrainWorldFocus; }
void NavMeshSurface::setFollowRadius(float radius){ component().followRadius = std::max(radius, 0.0f); }
float NavMeshSurface::getFollowRadius() const{ return component().followRadius; }

FunctionSubscribe<void(NavMeshSurface)>& NavMeshSurface::getOnBakeCompleted(){ return component().onBakeCompleted; }
