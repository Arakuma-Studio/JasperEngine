// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "WaterExcluder.h"
#include "subsystem/WaterInteractionSystem.h"

using namespace jasperengine;

WaterExcluder::WaterExcluder(Scene* scene, Entity entity): EntityHandle(scene, entity){
}

WaterExcluder::~WaterExcluder() = default;
WaterExcluder::WaterExcluder(const WaterExcluder& rhs) = default;
WaterExcluder& WaterExcluder::operator=(const WaterExcluder& rhs) = default;
WaterExcluder::WaterExcluder(WaterExcluder&& rhs) noexcept = default;
WaterExcluder& WaterExcluder::operator=(WaterExcluder&& rhs) noexcept = default;

WaterExcluderComponent& WaterExcluder::component() const{
    return getComponent<WaterExcluderComponent>();
}

bool WaterExcluder::isValid() const{
    return scene && scene->findComponent<WaterExcluderComponent>(entity) != nullptr;
}

void WaterExcluder::setEnabled(bool enabled){ component().enabled = enabled; }
bool WaterExcluder::isEnabled() const{ return component().enabled; }

void WaterExcluder::setShape(WaterExcluderShape shape){ component().shape = shape; }
WaterExcluderShape WaterExcluder::getShape() const{ return component().shape; }

void WaterExcluder::setCenter(Vector3 center){ component().center = center; }
Vector3 WaterExcluder::getCenter() const{ return component().center; }

void WaterExcluder::setSize(Vector3 size){ component().size = size; }
Vector3 WaterExcluder::getSize() const{ return component().size; }

bool WaterExcluder::contains(Vector3 point) const{
    std::shared_ptr<WaterInteractionSystem> system = scene ? scene->findSystem<WaterInteractionSystem>() : nullptr;
    return system && system->contains(entity, point);
}

std::string WaterExcluder::getHullStatus() const{
    std::shared_ptr<WaterInteractionSystem> system = scene ? scene->findSystem<WaterInteractionSystem>() : nullptr;
    if (!system || component().shape != WaterExcluderShape::CONVEX_HULL)
        return getWaterHullStatusName(WaterHullStatus::NONE);
    return getWaterHullStatusName(system->getHullInfo(entity).status);
}

int WaterExcluder::getHullFaces() const{
    std::shared_ptr<WaterInteractionSystem> system = scene ? scene->findSystem<WaterInteractionSystem>() : nullptr;
    return system ? (int)system->getHullInfo(entity).faces : 0;
}
