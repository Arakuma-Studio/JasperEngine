// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SimulationCulling.h"

#include "Scene.h"
#include "subsystem/CullingSystem.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

namespace {
    // the global scale (CullingSystem.cpp reads it through getDistanceScale)
    float& distanceScaleValue() {
        static float scale = 1.0f;
        return scale;
    }
}

SimulationCulling::SimulationCulling(Scene* scene, Entity entity): EntityHandle(scene, entity){
    if (!scene->findComponent<SimulationCullingComponent>(entity)){
        scene->addComponent<SimulationCullingComponent>(entity, {});
    }
}

SimulationCulling::~SimulationCulling(){
}

SimulationCulling::SimulationCulling(const SimulationCulling& rhs) = default;
SimulationCulling& SimulationCulling::operator=(const SimulationCulling& rhs) = default;

void SimulationCulling::setEnabled(bool enabled){ getComponent<SimulationCullingComponent>().enabled = enabled; }
bool SimulationCulling::isEnabled() const{ return getComponent<SimulationCullingComponent>().enabled; }

void SimulationCulling::setSimulationDistance(float distance){
    getComponent<SimulationCullingComponent>().simulationDistance = std::isfinite(distance) ? std::max(0.0f, distance) : 0.0f;
}
float SimulationCulling::getSimulationDistance() const{ return getComponent<SimulationCullingComponent>().simulationDistance; }

void SimulationCulling::setReducedRateDistance(float distance){
    getComponent<SimulationCullingComponent>().reducedRateDistance = std::isfinite(distance) ? std::max(0.0f, distance) : 0.0f;
}
float SimulationCulling::getReducedRateDistance() const{ return getComponent<SimulationCullingComponent>().reducedRateDistance; }

void SimulationCulling::setReducedUpdateInterval(int frames){
    getComponent<SimulationCullingComponent>().reducedUpdateInterval = std::clamp(frames, 2, 60);
}
int SimulationCulling::getReducedUpdateInterval() const{ return getComponent<SimulationCullingComponent>().reducedUpdateInterval; }

void SimulationCulling::setHysteresis(float metres){
    getComponent<SimulationCullingComponent>().hysteresis = std::isfinite(metres) ? std::max(0.0f, metres) : 0.0f;
}
float SimulationCulling::getHysteresis() const{ return getComponent<SimulationCullingComponent>().hysteresis; }

void SimulationCulling::setBoundsRadius(float radius){
    getComponent<SimulationCullingComponent>().boundsRadius = std::isfinite(radius) ? std::max(0.0f, radius) : 0.0f;
}
float SimulationCulling::getBoundsRadius() const{ return getComponent<SimulationCullingComponent>().boundsRadius; }

void SimulationCulling::setSimulateWhenVisible(bool simulate){ getComponent<SimulationCullingComponent>().simulateWhenVisible = simulate; }
bool SimulationCulling::isSimulateWhenVisible() const{ return getComponent<SimulationCullingComponent>().simulateWhenVisible; }
void SimulationCulling::setFreezeWhenGroundNotLoaded(bool freeze){ getComponent<SimulationCullingComponent>().freezeWhenGroundNotLoaded = freeze; }
bool SimulationCulling::isFreezeWhenGroundNotLoaded() const{ return getComponent<SimulationCullingComponent>().freezeWhenGroundNotLoaded; }
void SimulationCulling::setCullScripts(bool cull){ getComponent<SimulationCullingComponent>().cullScripts = cull; }
bool SimulationCulling::isCullScripts() const{ return getComponent<SimulationCullingComponent>().cullScripts; }
void SimulationCulling::setCullAnimator(bool cull){ getComponent<SimulationCullingComponent>().cullAnimator = cull; }
bool SimulationCulling::isCullAnimator() const{ return getComponent<SimulationCullingComponent>().cullAnimator; }
void SimulationCulling::setCullPhysics(bool cull){ getComponent<SimulationCullingComponent>().cullPhysics = cull; }
bool SimulationCulling::isCullPhysics() const{ return getComponent<SimulationCullingComponent>().cullPhysics; }

SimulationState SimulationCulling::getState() const{ return getComponent<SimulationCullingComponent>().state; }
SimulationCullReason SimulationCulling::getReason() const{ return getComponent<SimulationCullingComponent>().reason; }
float SimulationCulling::getFocusDistance() const{ return getComponent<SimulationCullingComponent>().focusDistance; }

const char* SimulationCulling::stateName(SimulationState state){
    switch (state){
        case SimulationState::Full: return "full";
        case SimulationState::Reduced: return "reduced";
        case SimulationState::Culled: return "culled";
    }
    return "full";
}

const char* SimulationCulling::reasonName(SimulationCullReason reason){
    switch (reason){
        case SimulationCullReason::None: return "none";
        case SimulationCullReason::Distance: return "distance";
        case SimulationCullReason::GroundNotLoaded: return "ground_not_loaded";
    }
    return "none";
}

int SimulationCulling::addFocus(Scene* scene, const Vector3& position){
    return scene ? scene->getSystem<CullingSystem>()->addFocus(position, NULL_ENTITY) : 0;
}

int SimulationCulling::addFocus(Scene* scene, Entity entity){
    if (!scene || !scene->isEntityCreated(entity)) return 0;
    return scene->getSystem<CullingSystem>()->addFocus(Vector3(0, 0, 0), entity);
}

bool SimulationCulling::setFocus(Scene* scene, int id, const Vector3& position){
    return scene && scene->getSystem<CullingSystem>()->setFocus(id, position);
}

bool SimulationCulling::removeFocus(Scene* scene, int id){
    return scene && scene->getSystem<CullingSystem>()->removeFocus(id);
}

void SimulationCulling::setCameraIsFocus(Scene* scene, bool cameraIsFocus){
    if (scene) scene->getSystem<CullingSystem>()->setCameraIsFocus(cameraIsFocus);
}

bool SimulationCulling::isCameraFocus(Scene* scene){
    return !scene || scene->getSystem<CullingSystem>()->isCameraFocus();
}

void SimulationCulling::setDistanceScale(float scale){
    distanceScaleValue() = (std::isfinite(scale) && scale >= 0.0f) ? scale : 1.0f;
}

void SimulationCulling::resetSession(){
    distanceScaleValue() = 1.0f;
}

float SimulationCulling::getDistanceScale(){
    return distanceScaleValue();
}

void SimulationCulling::setGroundCheck(Scene* scene, std::function<bool(const Vector3& position, float radius)> check){
    if (scene) scene->getSystem<CullingSystem>()->setGroundCheck(std::move(check));
}

bool SimulationCulling::isCulled(Scene* scene, Entity entity){
    return getState(scene, entity) == SimulationState::Culled;
}

SimulationState SimulationCulling::getState(Scene* scene, Entity entity){
    CullingSystem* system = anyActive() ? CullingSystem::activeFor(scene) : nullptr;
    if (!system) return SimulationState::Full;
    const uint8_t flags = system->getSimFlags(entity);
    if (flags & (CullingSystem::SimScriptsCulled | CullingSystem::SimAnimatorCulled | CullingSystem::SimPhysicsFrozen)) return SimulationState::Culled;
    if (flags & CullingSystem::SimReduced) return SimulationState::Reduced;
    // a component that culls nothing still has its state
    if (const SimulationCullingComponent* component = scene->findComponent<SimulationCullingComponent>(entity)) return component->state;
    return SimulationState::Full;
}

bool SimulationCulling::isAnimatorCulled(Scene* scene, Entity entity){
    CullingSystem* system = CullingSystem::activeFor(scene);
    return system && (system->getSimFlags(entity) & CullingSystem::SimAnimatorCulled) != 0;
}
