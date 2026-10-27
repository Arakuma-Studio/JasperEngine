// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "WaterVolume.h"

using namespace jasperengine;

WaterVolume::WaterVolume(Scene* scene, Entity entity): EntityHandle(scene, entity){
}

WaterVolume::~WaterVolume() = default;
WaterVolume::WaterVolume(const WaterVolume& rhs) = default;
WaterVolume& WaterVolume::operator=(const WaterVolume& rhs) = default;
WaterVolume::WaterVolume(WaterVolume&& rhs) noexcept = default;
WaterVolume& WaterVolume::operator=(WaterVolume&& rhs) noexcept = default;

WaterVolumeComponent& WaterVolume::component() const{
    return getComponent<WaterVolumeComponent>();
}

bool WaterVolume::isValid() const{
    return scene && scene->findComponent<WaterVolumeComponent>(entity) != nullptr;
}

void WaterVolume::setEnabled(bool enabled){ component().enabled = enabled; }
bool WaterVolume::isEnabled() const{ return component().enabled; }

void WaterVolume::setSize(Vector3 size){ component().size = size; }
Vector3 WaterVolume::getSize() const{ return component().size; }

void WaterVolume::setSurfaceOffset(float surfaceOffset){ component().surfaceOffset = surfaceOffset; }
float WaterVolume::getSurfaceOffset() const{ return component().surfaceOffset; }

void WaterVolume::setInfinite(bool infinite){ component().infinite = infinite; }
bool WaterVolume::isInfinite() const{ return component().infinite; }

void WaterVolume::setDensity(float density){ component().density = density; }
float WaterVolume::getDensity() const{ return component().density; }

void WaterVolume::setFlow(Vector3 flow){ component().flow = flow; }
Vector3 WaterVolume::getFlow() const{ return component().flow; }

void WaterVolume::setLinearDrag(float linearDrag){ component().linearDrag = linearDrag; }
float WaterVolume::getLinearDrag() const{ return component().linearDrag; }

void WaterVolume::setAngularDrag(float angularDrag){ component().angularDrag = angularDrag; }
float WaterVolume::getAngularDrag() const{ return component().angularDrag; }

void WaterVolume::setWaves(WaterVolumeWaves waves){ component().waves = waves; }
WaterVolumeWaves WaterVolume::getWaves() const{ return component().waves; }

