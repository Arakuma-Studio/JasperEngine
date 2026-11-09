// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TerrainWorld.h"

#include "Scene.h"
#include "component/TerrainWorldComponent.h"
#include "component/Transform.h"
#include "physics/TerrainWorldCollision.h"
#include "subsystem/TerrainWorldRuntime.h"
#include "subsystem/TerrainWorldSystem.h"

#include <cmath>

using namespace jasperengine;

namespace {

    // A point within the addressable tiles: the runtime casts a tile index to 16 bits, so a point
    // far past them would wrap onto a real tile
    bool terrainWorldOnLattice(const TerrainWorldRuntime& runtime, float localX, float localZ) {
        const float size = runtime.lattice().tileSize();
        const float low = static_cast<float>(terraincore::WORLD_TILE_MIN) * size;
        const float high = static_cast<float>(terraincore::WORLD_TILE_MAX + 1) * size;
        return localX >= low && localX < high && localZ >= low && localZ < high;
    }

}

TerrainWorld::TerrainWorld(Scene* scene, Entity entity): EntityHandle(scene, entity){
}

TerrainWorld::~TerrainWorld() = default;
TerrainWorld::TerrainWorld(const TerrainWorld& rhs) = default;
TerrainWorld& TerrainWorld::operator=(const TerrainWorld& rhs) = default;
TerrainWorld::TerrainWorld(TerrainWorld&& rhs) noexcept = default;
TerrainWorld& TerrainWorld::operator=(TerrainWorld&& rhs) noexcept = default;

bool TerrainWorld::isValid() const{
    return scene && scene->isEntityCreated(entity) && scene->findComponent<TerrainWorldComponent>(entity) != nullptr;
}

TerrainWorldRuntime* TerrainWorld::getRuntime(std::string* error) const{
    if (!isValid()){
        if (error) *error = "the GameObject has no Terrain World component";
        return nullptr;
    }
    std::shared_ptr<TerrainWorldSystem> system = scene->findSystem<TerrainWorldSystem>();
    if (!system){
        if (error) *error = "the scene has no Terrain World system";
        return nullptr;
    }
    return system->ensureRuntime(entity, error);
}

Vector3 TerrainWorld::getOrigin() const{
    const Transform* transform = scene ? scene->findComponent<Transform>(entity) : nullptr;
    return transform ? transform->worldPosition : Vector3(0.0f, 0.0f, 0.0f);
}

int TerrainWorld::addFocus(const Vector3& position, float radiusScale, const Vector3& velocity){
    TerrainWorldRuntime* runtime = getRuntime();
    if (!runtime) return 0;
    const Vector3 origin = getOrigin();
    return runtime->addFocus(position.x - origin.x, position.z - origin.z, radiusScale, velocity.x, velocity.z);
}

bool TerrainWorld::setFocus(int id, const Vector3& position, const Vector3& velocity){
    TerrainWorldRuntime* runtime = getRuntime();
    if (!runtime) return false;
    const Vector3 origin = getOrigin();
    return runtime->setFocus(id, position.x - origin.x, position.z - origin.z, velocity.x, velocity.z);
}

bool TerrainWorld::removeFocus(int id){
    TerrainWorldRuntime* runtime = getRuntime();
    return runtime && runtime->removeFocus(id);
}

TerrainWorldSurface TerrainWorld::getSurfaceAt(float x, float z) const{
    TerrainWorldSurface out;
    TerrainWorldRuntime* runtime = getRuntime();
    const Vector3 origin = getOrigin();
    const float localX = x - origin.x, localZ = z - origin.z;
    if (!runtime || !terrainWorldOnLattice(*runtime, localX, localZ)) return out;
    const terraincore::SurfaceSample sample = runtime->query(localX, localZ, !runtime->playing());
    out.height = sample.height + origin.y;
    out.normal = sample.normal;
    out.hole = sample.hole;
    out.present = sample.present;
    out.exact = sample.present && sample.exact;
    out.state = terrainWorldPointStateName(runtime->pointState(localX, localZ));
    return out;
}

std::string TerrainWorld::getTileState(float x, float z) const{
    TerrainWorldRuntime* runtime = getRuntime();
    const Vector3 origin = getOrigin();
    const float localX = x - origin.x, localZ = z - origin.z;
    if (!runtime || !terrainWorldOnLattice(*runtime, localX, localZ)) return terraincore::tileStateName(terraincore::TileState::Absent);
    const float size = runtime->lattice().tileSize();
    const terraincore::TileCoord tile(terraincore::tileOfLocal(localX, size), terraincore::tileOfLocal(localZ, size));
    return terraincore::tileStateName(runtime->tileState(tile));
}

bool TerrainWorld::isAreaReady(float x, float z, float radius, TerrainWorldReadyLevel level) const{
    TerrainWorldRuntime* runtime = getRuntime();
    if (!runtime) return false;
    const Vector3 origin = getOrigin();
    return runtime->isAreaReady(x - origin.x, z - origin.z, radius, level);
}

int TerrainWorld::onAreaReady(float x, float z, float radius, TerrainWorldReadyLevel level, std::function<void()> callback){
    if (!getRuntime()) return 0;
    const Vector3 origin = getOrigin();
    return scene->getSystem<TerrainWorldSystem>()->onAreaReady(entity, x - origin.x, z - origin.z, radius, level, std::move(callback));
}

bool TerrainWorld::ensureCollisionAt(float x, float z){
    TerrainWorldRuntime* runtime = getRuntime();
    const Vector3 origin = getOrigin();
    const float localX = x - origin.x, localZ = z - origin.z;
    if (!runtime || !runtime->collision || !terrainWorldOnLattice(*runtime, localX, localZ)) return false;
    return runtime->collision->ensureAt(localX, localZ);
}

bool TerrainWorld::isReady() const{
    TerrainWorldRuntime* runtime = getRuntime();
    return runtime && runtime->isReady();
}

// The system applies the budgets to the runtime at each pump
void TerrainWorld::setPoolSize(int poolSize){ getComponent<TerrainWorldComponent>().poolSize = poolSize; }
int TerrainWorld::getPoolSize() const{ return getComponent<TerrainWorldComponent>().poolSize; }
void TerrainWorld::setLodDistance(float lodDistance){ getComponent<TerrainWorldComponent>().lodDistance = lodDistance; }
float TerrainWorld::getLodDistance() const{ return getComponent<TerrainWorldComponent>().lodDistance; }
void TerrainWorld::setUploadBudget(float uploadBudget){ getComponent<TerrainWorldComponent>().uploadBudget = uploadBudget; }
float TerrainWorld::getUploadBudget() const{ return getComponent<TerrainWorldComponent>().uploadBudget; }
void TerrainWorld::setCollisionRadius(float collisionRadius){ getComponent<TerrainWorldComponent>().collisionRadius = collisionRadius; }
float TerrainWorld::getCollisionRadius() const{ return getComponent<TerrainWorldComponent>().collisionRadius; }
