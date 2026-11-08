// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "NavMeshModifier.h"

#include "Scene.h"
#include "subsystem/NavigationSystem.h"

#include <algorithm>

using namespace jasperengine;

NavMeshModifier::NavMeshModifier(Scene* scene, Entity entity): EntityHandle(scene, entity){
}

NavMeshModifier::~NavMeshModifier() = default;
NavMeshModifier::NavMeshModifier(const NavMeshModifier& rhs) = default;
NavMeshModifier& NavMeshModifier::operator=(const NavMeshModifier& rhs) = default;
NavMeshModifier::NavMeshModifier(NavMeshModifier&& rhs) noexcept = default;
NavMeshModifier& NavMeshModifier::operator=(NavMeshModifier&& rhs) noexcept = default;

NavMeshModifierComponent& NavMeshModifier::component() const{
    return getComponent<NavMeshModifierComponent>();
}

void NavMeshModifier::sync() const{
    // a runtime volume re-applies at once: a query right after the call sees it (S76 R14)
    if (component().useVolume && component().applyAtRuntime)
        scene->getSystem<NavigationSystem>()->syncAreaOverrides();
}

bool NavMeshModifier::isValid() const{
    return scene && scene->findComponent<NavMeshModifierComponent>(entity) != nullptr;
}

void NavMeshModifier::setEnabled(bool enabled){ component().enabled = enabled; sync(); }
bool NavMeshModifier::isEnabled() const{ return component().enabled; }
void NavMeshModifier::setArea(int area){ component().area = std::clamp(area, 0, nav::AREA_COUNT - 1); sync(); }
int NavMeshModifier::getArea() const{ return component().area; }
void NavMeshModifier::setCenter(Vector3 center){ component().volumeCenter = center; sync(); }
Vector3 NavMeshModifier::getCenter() const{ return component().volumeCenter; }
void NavMeshModifier::setSize(Vector3 size){ component().volumeSize = size; sync(); }
Vector3 NavMeshModifier::getSize() const{ return component().volumeSize; }
bool NavMeshModifier::isUseVolume() const{ return component().useVolume; }
bool NavMeshModifier::isApplyAtRuntime() const{ return component().applyAtRuntime; }
bool NavMeshModifier::isOverrideArea() const{ return component().overrideArea; }
bool NavMeshModifier::isApplyToChildren() const{ return component().applyToChildren; }

int NavMeshModifier::getAppliedPolygons() const{
    return scene->getSystem<NavigationSystem>()->getModifierAppliedPolygons(entity);
}
