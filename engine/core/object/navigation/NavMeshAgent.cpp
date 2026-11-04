// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "NavMeshAgent.h"

#include "Scene.h"
#include "subsystem/NavigationSystem.h"

#include <algorithm>

using namespace jasperengine;

NavMeshAgent::NavMeshAgent(Scene* scene, Entity entity): EntityHandle(scene, entity){
}

NavMeshAgent::~NavMeshAgent() = default;
NavMeshAgent::NavMeshAgent(const NavMeshAgent& rhs) = default;
NavMeshAgent& NavMeshAgent::operator=(const NavMeshAgent& rhs) = default;
NavMeshAgent::NavMeshAgent(NavMeshAgent&& rhs) noexcept = default;
NavMeshAgent& NavMeshAgent::operator=(NavMeshAgent&& rhs) noexcept = default;

NavMeshAgentComponent& NavMeshAgent::component() const{
    return getComponent<NavMeshAgentComponent>();
}

NavigationSystem* NavMeshAgent::system() const{
    return scene->getSystem<NavigationSystem>().get();
}

bool NavMeshAgent::isValid() const{
    return scene && scene->findComponent<NavMeshAgentComponent>(entity) != nullptr;
}

bool NavMeshAgent::setDestination(const Vector3& target){ return system()->setDestination(entity, target); }
Vector3 NavMeshAgent::getDestination() const{ return component().destination; }
void NavMeshAgent::resetPath(){ system()->resetPath(entity); }
bool NavMeshAgent::warp(const Vector3& position){ return system()->warp(entity, position); }
void NavMeshAgent::move(const Vector3& offset){ system()->move(entity, offset); }
void NavMeshAgent::completeOffMeshLink(){ system()->completeOffMeshLink(entity); }

std::vector<Vector3> NavMeshAgent::getPathCorners() const{
    std::vector<Vector3> corners;
    system()->getAgentPath(entity, corners);
    return corners;
}

void NavMeshAgent::setEnabled(bool enabled){ component().enabled = enabled; }
bool NavMeshAgent::isEnabled() const{ return component().enabled; }
void NavMeshAgent::setAgentTypeID(int agentType){ component().agentType = agentType; }
int NavMeshAgent::getAgentTypeID() const{ return component().agentType; }
void NavMeshAgent::setSpeed(float speed){ component().speed = std::max(0.0f, speed); }
float NavMeshAgent::getSpeed() const{ return component().speed; }
void NavMeshAgent::setAngularSpeed(float degreesPerSecond){ component().angularSpeed = std::max(0.0f, degreesPerSecond); }
float NavMeshAgent::getAngularSpeed() const{ return component().angularSpeed; }
void NavMeshAgent::setAcceleration(float acceleration){ component().acceleration = std::max(0.0f, acceleration); }
float NavMeshAgent::getAcceleration() const{ return component().acceleration; }
void NavMeshAgent::setStoppingDistance(float distance){ component().stoppingDistance = std::max(0.0f, distance); }
float NavMeshAgent::getStoppingDistance() const{ return component().stoppingDistance; }
void NavMeshAgent::setAutoBraking(bool autoBraking){ component().autoBraking = autoBraking; }
bool NavMeshAgent::isAutoBraking() const{ return component().autoBraking; }
void NavMeshAgent::setRadius(float radius){ component().radius = std::max(1e-3f, radius); }
float NavMeshAgent::getRadius() const{ return component().radius; }
void NavMeshAgent::setHeight(float height){ component().height = std::max(1e-3f, height); }
float NavMeshAgent::getHeight() const{ return component().height; }
void NavMeshAgent::setBaseOffset(float offset){ component().baseOffset = offset; }
float NavMeshAgent::getBaseOffset() const{ return component().baseOffset; }
void NavMeshAgent::setStopped(bool stopped){ component().isStopped = stopped; }
bool NavMeshAgent::isStopped() const{ return component().isStopped; }
void NavMeshAgent::setAvoidancePriority(int priority){ component().avoidancePriority = std::clamp(priority, 0, 99); }
int NavMeshAgent::getAvoidancePriority() const{ return component().avoidancePriority; }
void NavMeshAgent::setObstacleAvoidanceType(nav::AvoidanceQuality quality){ component().obstacleAvoidance = quality; }
nav::AvoidanceQuality NavMeshAgent::getObstacleAvoidanceType() const{ return component().obstacleAvoidance; }
void NavMeshAgent::setAreaMask(int mask){ component().areaMask = mask & nav::ALL_AREAS; }
int NavMeshAgent::getAreaMask() const{ return component().areaMask; }
void NavMeshAgent::setAutoRepath(bool autoRepath){ component().autoRepath = autoRepath; }
bool NavMeshAgent::isAutoRepath() const{ return component().autoRepath; }
void NavMeshAgent::setAutoTraverseOffMeshLink(bool autoTraverse){ component().autoTraverseOffMeshLink = autoTraverse; }
bool NavMeshAgent::isAutoTraverseOffMeshLink() const{ return component().autoTraverseOffMeshLink; }
void NavMeshAgent::setUpdatePosition(bool update){ component().updatePosition = update; }
bool NavMeshAgent::isUpdatePosition() const{ return component().updatePosition; }
void NavMeshAgent::setUpdateRotation(bool update){ component().updateRotation = update; }
bool NavMeshAgent::isUpdateRotation() const{ return component().updateRotation; }
void NavMeshAgent::setDriver(NavAgentDriver driver){ component().driver = driver; }
NavAgentDriver NavMeshAgent::getDriver() const{ return component().driver; }
void NavMeshAgent::setSurface(Entity surface){ component().surface = surface; }
Entity NavMeshAgent::getSurface() const{ return component().surface; }

float NavMeshAgent::getRemainingDistance() const{ return component().remainingDistance; }
bool NavMeshAgent::isPathPending() const{ return component().pathPending; }
bool NavMeshAgent::hasPath() const{ return component().hasPath && !component().pathPending; }
nav::PathStatus NavMeshAgent::getPathStatus() const{ return component().pathStatus; }
Vector3 NavMeshAgent::getVelocity() const{ return component().velocity; }
Vector3 NavMeshAgent::getDesiredVelocity() const{ return component().desiredVelocity; }
Vector3 NavMeshAgent::getNextPosition() const{ return component().nextPosition; }
bool NavMeshAgent::isOnNavMesh() const{ return component().isOnNavMesh; }
bool NavMeshAgent::isOnOffMeshLink() const{ return component().isOnOffMeshLink; }

bool NavMeshAgent::getCurrentOffMeshLink(Vector3& start, Vector3& end, Entity& link) const{
    return system()->getCurrentOffMeshLink(entity, start, end, link);
}

void NavMeshAgent::setAreaCost(int area, float cost){ system()->setAgentAreaCost(entity, area, cost); }
float NavMeshAgent::getAreaCost(int area) const{ return system()->getAgentAreaCost(entity, area); }
void NavMeshAgent::resetAreaCost(int area){ system()->resetAgentAreaCost(entity, area); }

FunctionSubscribe<void(NavMeshAgent)>& NavMeshAgent::getOnDestinationReached(){ return component().onDestinationReached; }
FunctionSubscribe<void(NavMeshAgent)>& NavMeshAgent::getOnPathFailed(){ return component().onPathFailed; }
FunctionSubscribe<void(NavMeshAgent)>& NavMeshAgent::getOnOffMeshLinkStart(){ return component().onOffMeshLinkStart; }
FunctionSubscribe<void(NavMeshAgent)>& NavMeshAgent::getOnOffMeshLinkEnd(){ return component().onOffMeshLinkEnd; }
