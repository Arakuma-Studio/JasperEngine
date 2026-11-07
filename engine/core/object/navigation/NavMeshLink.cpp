// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "NavMeshLink.h"

#include "Scene.h"
#include "subsystem/NavigationSystem.h"

#include <algorithm>

using namespace jasperengine;

NavMeshLink::NavMeshLink(Scene* scene, Entity entity): EntityHandle(scene, entity){
}

NavMeshLink::~NavMeshLink() = default;
NavMeshLink::NavMeshLink(const NavMeshLink& rhs) = default;
NavMeshLink& NavMeshLink::operator=(const NavMeshLink& rhs) = default;
NavMeshLink::NavMeshLink(NavMeshLink&& rhs) noexcept = default;
NavMeshLink& NavMeshLink::operator=(NavMeshLink&& rhs) noexcept = default;

NavMeshLinkComponent& NavMeshLink::component() const{
    return getComponent<NavMeshLinkComponent>();
}

bool NavMeshLink::isValid() const{
    return scene && scene->findComponent<NavMeshLinkComponent>(entity) != nullptr;
}

void NavMeshLink::setEnabled(bool enabled){ component().enabled = enabled; }
bool NavMeshLink::isEnabled() const{ return component().enabled; }
void NavMeshLink::setAgentTypeID(int agentType){ component().agentType = agentType; }
int NavMeshLink::getAgentTypeID() const{ return component().agentType; }
void NavMeshLink::setStartPoint(Vector3 point){ component().startPoint = point; }
Vector3 NavMeshLink::getStartPoint() const{ return component().startPoint; }
void NavMeshLink::setEndPoint(Vector3 point){ component().endPoint = point; }
Vector3 NavMeshLink::getEndPoint() const{ return component().endPoint; }
void NavMeshLink::setWidth(float width){ component().width = std::max(width, 0.0f); }
float NavMeshLink::getWidth() const{ return component().width; }
void NavMeshLink::setCostModifier(float cost){ component().costModifier = cost; }
float NavMeshLink::getCostModifier() const{ return component().costModifier; }
void NavMeshLink::setBidirectional(bool bidirectional){ component().bidirectional = bidirectional; }
bool NavMeshLink::isBidirectional() const{ return component().bidirectional; }
void NavMeshLink::setArea(int area){ component().area = std::clamp(area, 0, nav::AREA_COUNT - 1); }
int NavMeshLink::getArea() const{ return component().area; }
void NavMeshLink::setAutoUpdate(bool autoUpdate){ component().autoUpdatePosition = autoUpdate; }
bool NavMeshLink::isAutoUpdate() const{ return component().autoUpdatePosition; }

void NavMeshLink::setActivated(bool activated){
    component().activated = activated;
    // a query right after the call already sees it (S76 R14)
    scene->getSystem<NavigationSystem>()->syncAreaOverrides();
}

bool NavMeshLink::isActivated() const{ return component().activated; }

bool NavMeshLink::isOccupied() const{
    return scene->getSystem<NavigationSystem>()->isLinkOccupied(entity);
}

bool NavMeshLink::updateLink(){
    return scene->getSystem<NavigationSystem>()->updateLink(entity);
}
