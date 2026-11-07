// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "WaterWake.h"
#include "subsystem/WaterInteractionSystem.h"

using namespace jasperengine;

WaterWake::WaterWake(Scene* scene, Entity entity): EntityHandle(scene, entity){
}

WaterWake::~WaterWake() = default;
WaterWake::WaterWake(const WaterWake& rhs) = default;
WaterWake& WaterWake::operator=(const WaterWake& rhs) = default;
WaterWake::WaterWake(WaterWake&& rhs) noexcept = default;
WaterWake& WaterWake::operator=(WaterWake&& rhs) noexcept = default;

WaterWakeComponent& WaterWake::component() const{
    return getComponent<WaterWakeComponent>();
}

bool WaterWake::isValid() const{
    return scene && scene->findComponent<WaterWakeComponent>(entity) != nullptr;
}

void WaterWake::setEnabled(bool enabled){ component().enabled = enabled; }
bool WaterWake::isEnabled() const{ return component().enabled; }

void WaterWake::setFoam(float foam){ component().foam = foam; }
float WaterWake::getFoam() const{ return component().foam; }

void WaterWake::setWaves(float waves){ component().waves = waves; }
float WaterWake::getWaves() const{ return component().waves; }

void WaterWake::setWidth(float width){ component().width = width; }
float WaterWake::getWidth() const{ return component().width; }

void WaterWake::setLength(float length){ component().length = length; }
float WaterWake::getLength() const{ return component().length; }

bool WaterWake::isEmitting() const{
    std::shared_ptr<WaterInteractionSystem> system = scene ? scene->findSystem<WaterInteractionSystem>() : nullptr;
    return system && system->getEmitterInfo(entity).emitting;
}

float WaterWake::getSpeed() const{
    std::shared_ptr<WaterInteractionSystem> system = scene ? scene->findSystem<WaterInteractionSystem>() : nullptr;
    return system ? system->getEmitterInfo(entity).speed : 0.0f;
}
