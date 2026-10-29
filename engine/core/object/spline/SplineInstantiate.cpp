// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SplineInstantiate.h"

#include "Scene.h"
#include "subsystem/SplineSystem.h"

using namespace jasperengine;

SplineInstantiate::SplineInstantiate(Scene* scene, Entity entity): EntityHandle(scene, entity){
}

SplineInstantiate::~SplineInstantiate() = default;
SplineInstantiate::SplineInstantiate(const SplineInstantiate& rhs) = default;
SplineInstantiate& SplineInstantiate::operator=(const SplineInstantiate& rhs) = default;
SplineInstantiate::SplineInstantiate(SplineInstantiate&& rhs) noexcept = default;
SplineInstantiate& SplineInstantiate::operator=(SplineInstantiate&& rhs) noexcept = default;

SplineInstantiateComponent& SplineInstantiate::component() const{
    return getComponent<SplineInstantiateComponent>();
}

bool SplineInstantiate::isValid() const{
    return scene && scene->findComponent<SplineInstantiateComponent>(entity) != nullptr;
}

void SplineInstantiate::setEnabled(bool enabled){ component().enabled = enabled; }
bool SplineInstantiate::isEnabled() const{ return component().enabled; }

void SplineInstantiate::setMode(SplineInstantiateMode value){ component().mode = value; }
SplineInstantiateMode SplineInstantiate::getMode() const{ return component().mode; }

void SplineInstantiate::setSpacing(float value){ component().spacing = value; }
float SplineInstantiate::getSpacing() const{ return component().spacing; }

void SplineInstantiate::setFit(SplineSpacingFit value){ component().fit = value; }
SplineSpacingFit SplineInstantiate::getFit() const{ return component().fit; }

void SplineInstantiate::setPostPitch(float value){ component().postPitch = value; }
float SplineInstantiate::getPostPitch() const{ return component().postPitch; }

void SplineInstantiate::setSpanMaxStretch(float value){ component().spanMaxStretch = value; }
float SplineInstantiate::getSpanMaxStretch() const{ return component().spanMaxStretch; }

void SplineInstantiate::setFenceGrade(SplineGrade value){ component().fenceGrade = value; }
SplineGrade SplineInstantiate::getFenceGrade() const{ return component().fenceGrade; }

void SplineInstantiate::setCrestPitch(float value){ component().crestPitch = value; }
float SplineInstantiate::getCrestPitch() const{ return component().crestPitch; }

void SplineInstantiate::setLateralOffset(float value){ component().lateralOffset = value; }
float SplineInstantiate::getLateralOffset() const{ return component().lateralOffset; }

void SplineInstantiate::setVerticalOffset(float value){ component().verticalOffset = value; }
float SplineInstantiate::getVerticalOffset() const{ return component().verticalOffset; }

void SplineInstantiate::setSpacingJitter(float value){ component().spacingJitter = value; }
float SplineInstantiate::getSpacingJitter() const{ return component().spacingJitter; }

void SplineInstantiate::setYawJitter(float value){ component().yawJitter = value; }
float SplineInstantiate::getYawJitter() const{ return component().yawJitter; }

void SplineInstantiate::setLateralJitter(float value){ component().lateralJitter = value; }
float SplineInstantiate::getLateralJitter() const{ return component().lateralJitter; }

void SplineInstantiate::setScaleRange(Vector2 value){ component().scaleRange = value; }
Vector2 SplineInstantiate::getScaleRange() const{ return component().scaleRange; }

void SplineInstantiate::setDropout(float value){ component().dropout = value; }
float SplineInstantiate::getDropout() const{ return component().dropout; }

void SplineInstantiate::setSeed(uint32_t value){ component().seed = value; }
uint32_t SplineInstantiate::getSeed() const{ return component().seed; }

void SplineInstantiate::setEndTrim(float value){ component().endTrim = value; }
float SplineInstantiate::getEndTrim() const{ return component().endTrim; }

void SplineInstantiate::setPlantMode(SplinePlantMode value){ component().plantMode = value; }
SplinePlantMode SplineInstantiate::getPlantMode() const{ return component().plantMode; }

void SplineInstantiate::setMaxTilt(float value){ component().maxTilt = value; }
float SplineInstantiate::getMaxTilt() const{ return component().maxTilt; }

void SplineInstantiate::setConform(SplineConform value){ component().conform = value; }
SplineConform SplineInstantiate::getConform() const{ return component().conform; }

void SplineInstantiate::setSlopeBlend(float value){ component().slopeBlend = value; }
float SplineInstantiate::getSlopeBlend() const{ return component().slopeBlend; }

void SplineInstantiate::setStationSpacing(float value){ component().stationSpacing = value; }
float SplineInstantiate::getStationSpacing() const{ return component().stationSpacing; }

static SplineSystem* splineSystemOf(Scene* scene){
    return scene ? scene->findSystem<SplineSystem>().get() : nullptr;
}

bool SplineInstantiate::addItem(const std::string& path, float weight){
    SplineInstantiateComponent& instantiate = component();
    if (instantiate.items.size() >= SplineInstantiateComponent::MAX_POOL_ENTRIES){
        return false;
    }
    SplinePoolEntry entry;
    entry.asset = AssetRef(path);
    entry.weight = weight;
    instantiate.items.push_back(entry);
    return true;
}

void SplineInstantiate::clearItems(){ component().items.clear(); }
size_t SplineInstantiate::getItemCount() const{ return component().items.size(); }

unsigned int SplineInstantiate::getInstanceCount() const{
    SplineSystem* system = splineSystemOf(scene);
    return system ? system->getInstantiateStats(entity).instances : 0;
}

bool SplineInstantiate::isPending() const{
    SplineSystem* system = splineSystemOf(scene);
    return system && system->isRebuildPending(entity);
}

void SplineInstantiate::rebuild(){
    if (SplineSystem* system = splineSystemOf(scene)){
        system->rebuildNow(entity);
    }
}
