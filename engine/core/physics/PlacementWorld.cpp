// SPDX-License-Identifier: MIT
// STUB (contract only): replaced by the Jolt implementation.

#include "PlacementWorld.h"

using namespace jasperengine;

const char* jasperengine::placementShapeKindName(PlacementShapeKind kind) {
    switch (kind) {
        case PlacementShapeKind::NONE: return "none";
        case PlacementShapeKind::AUTHORED: return "authored";
        case PlacementShapeKind::BOX: return "box";
        case PlacementShapeKind::SPHERE: return "sphere";
        case PlacementShapeKind::CAPSULE: return "capsule";
        case PlacementShapeKind::CONVEX_HULL: return "convex_hull";
        case PlacementShapeKind::DECOMPOSED: return "decomposed";
        case PlacementShapeKind::MESH: return "mesh";
        case PlacementShapeKind::HEIGHTFIELD: return "heightfield";
    }
    return "none";
}

struct PlacementWorld::Impl {
    Scene* scene = nullptr;
    PlacementWorldSettings settings;
    std::vector<Entity> movers;
    std::vector<Entity> neighbours;
    Vector3 gravity;
};

PlacementWorld::PlacementWorld(Scene* scene, const PlacementWorldSettings& settings): impl(new Impl) {
    impl->scene = scene;
    impl->settings = settings;
}
PlacementWorld::~PlacementWorld() = default;
Scene* PlacementWorld::getScene() const { return impl->scene; }
void PlacementWorld::setSettings(const PlacementWorldSettings& s) { impl->settings = s; }
const PlacementWorldSettings& PlacementWorld::getSettings() const { return impl->settings; }
bool PlacementWorld::addMover(Entity, std::string* why) { if (why) *why = "physics placement is not implemented yet"; return false; }
void PlacementWorld::removeMover(Entity) {}
bool PlacementWorld::isMover(Entity) const { return false; }
const std::vector<Entity>& PlacementWorld::getMovers() const { return impl->movers; }
void PlacementWorld::ensureContext(const AABB&) {}
void PlacementWorld::addContextEntity(Entity) {}
size_t PlacementWorld::getContextBodyCount() const { return 0; }
const std::vector<Entity>& PlacementWorld::getNeighbours() const { return impl->neighbours; }
AABB PlacementWorld::getContextRegion() const { return AABB(); }
void PlacementWorld::refreshShapes() {}
bool PlacementWorld::hasPendingDecompositions() const { return false; }
void PlacementWorld::setGravity(const Vector3& g) { impl->gravity = g; }
Vector3 PlacementWorld::getGravity() const { return impl->gravity; }
void PlacementWorld::setTarget(Entity, const PlacementPose&) {}
void PlacementWorld::clearTarget(Entity) {}
void PlacementWorld::clearTargets() {}
bool PlacementWorld::hasTarget(Entity) const { return false; }
void PlacementWorld::teleport(Entity, const PlacementPose&) {}
void PlacementWorld::stopAll() {}
void PlacementWorld::setDamping(float, float) {}
void PlacementWorld::setSeparationBoost(float) {}
void PlacementWorld::wakeAll() {}
void PlacementWorld::clearFlags() {}
void PlacementWorld::step() {}
uint32_t PlacementWorld::getStepCount() const { return 0; }
double PlacementWorld::getSimulatedSeconds() const { return 0.0; }
bool PlacementWorld::allAsleep() const { return true; }
float PlacementWorld::getMaxPenetration() const { return 0.0f; }
PlacementBodyState PlacementWorld::getState(Entity body) const { PlacementBodyState s; s.entity = body; return s; }
std::vector<PlacementBodyState> PlacementWorld::getStates() const { return {}; }
AABB PlacementWorld::getMoversBounds() const { return AABB(); }
bool PlacementWorld::raycast(const Ray&, PlacementRayHit&, const std::vector<Entity>&) const { return false; }
uint64_t PlacementWorld::stateHash() const { return 0; }
void PlacementWorld::collectShapeLines(std::vector<Vector3>&, bool, size_t) const {}
