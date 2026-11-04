// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "NavMeshObstacle.h"

#include "Scene.h"
#include "subsystem/NavigationSystem.h"

#include <algorithm>

using namespace jasperengine;

NavMeshObstacle::NavMeshObstacle(Scene* scene, Entity entity): EntityHandle(scene, entity){
}

NavMeshObstacle::~NavMeshObstacle() = default;
NavMeshObstacle::NavMeshObstacle(const NavMeshObstacle& rhs) = default;
NavMeshObstacle& NavMeshObstacle::operator=(const NavMeshObstacle& rhs) = default;
NavMeshObstacle::NavMeshObstacle(NavMeshObstacle&& rhs) noexcept = default;
NavMeshObstacle& NavMeshObstacle::operator=(NavMeshObstacle&& rhs) noexcept = default;

NavMeshObstacleComponent& NavMeshObstacle::component() const{
    return getComponent<NavMeshObstacleComponent>();
}

bool NavMeshObstacle::isValid() const{
    return scene && scene->findComponent<NavMeshObstacleComponent>(entity) != nullptr;
}

void NavMeshObstacle::setEnabled(bool enabled){ component().enabled = enabled; }
bool NavMeshObstacle::isEnabled() const{ return component().enabled; }
void NavMeshObstacle::setShape(NavObstacleShape shape){ component().shape = shape; }
NavObstacleShape NavMeshObstacle::getShape() const{ return component().shape; }
void NavMeshObstacle::setCenter(Vector3 center){ component().center = center; }
Vector3 NavMeshObstacle::getCenter() const{ return component().center; }
void NavMeshObstacle::setSize(Vector3 size){ component().size = size; }
Vector3 NavMeshObstacle::getSize() const{ return component().size; }
void NavMeshObstacle::setRadius(float radius){ component().radius = std::max(radius, 0.0f); }
float NavMeshObstacle::getRadius() const{ return component().radius; }
void NavMeshObstacle::setHeight(float height){ component().height = std::max(height, 0.0f); }
float NavMeshObstacle::getHeight() const{ return component().height; }
void NavMeshObstacle::setCarving(bool carving){ component().carve = carving; }
bool NavMeshObstacle::isCarving() const{ return component().carve; }
void NavMeshObstacle::setCarveOnlyStationary(bool only){ component().carveOnlyStationary = only; }
bool NavMeshObstacle::isCarveOnlyStationary() const{ return component().carveOnlyStationary; }
void NavMeshObstacle::setCarvingMoveThreshold(float threshold){ component().moveThreshold = std::max(threshold, 0.0f); }
float NavMeshObstacle::getCarvingMoveThreshold() const{ return component().moveThreshold; }
void NavMeshObstacle::setCarvingTimeToStationary(float seconds){ component().timeToStationary = std::max(seconds, 0.0f); }
float NavMeshObstacle::getCarvingTimeToStationary() const{ return component().timeToStationary; }

Vector3 NavMeshObstacle::getVelocity() const{
    return scene->getSystem<NavigationSystem>()->getObstacleVelocity(entity);
}
