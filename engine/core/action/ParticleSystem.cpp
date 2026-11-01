// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ParticleSystem.h"

#include "Log.h"
#include "particles/ParticleFields.h"
#include "particles/ParticleSimulation.h"
#include "subsystem/ActionSystem.h"
#include "subsystem/MeshSystem.h"
#include "component/MeshComponent.h"
#include "component/PointsComponent.h"
#include "component/InstancedMeshComponent.h"
#include "component/Transform.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

using namespace jasperengine;

namespace {

    // the module fields each legacy setter maps to on a version 2 system (particles spec 6.6:
    // as the upgrade maps them, "module.*" every field of the module); none: no equivalent
    const std::unordered_map<std::string, std::vector<std::string>>& legacyMappings(){
        static const std::unordered_map<std::string, std::vector<std::string>> map = {
            {"setMaxParticles", {"main.maxParticles"}},
            {"setRate", {"emission.rateOverTime"}},
            {"setLoop", {"main.looping", "main.duration"}},
            {"setLocalSpace", {"main.simulationSpace", "main.scalingMode"}},
            {"setLifeInitializer", {"main.startLifetime"}},
            {"setPositionInitializer", {"shape.*"}},
            {"setPositionInitializerShape", {"shape.*"}},
            {"setSpherePositionInitializer", {"shape.*"}},
            {"setHemispherePositionInitializer", {"shape.*"}},
            {"setCirclePositionInitializer", {"shape.*"}},
            {"setConePositionInitializer", {"shape.*"}},
            {"addBurst", {"emission.bursts"}},
            {"setBursts", {"emission.bursts"}},
            {"clearBursts", {"emission.bursts"}},
            {"setVelocityInitializer", {"velocityOverLifetime.*", "main.startSpeed"}},
            {"setVelocityModifier", {"velocityOverLifetime.*"}},
            {"setAccelerationInitializer", {"forceOverLifetime.*"}},
            {"setAccelerationModifier", {"forceOverLifetime.*"}},
            {"setColorInitializer", {"main.startColor"}},
            {"setColorModifier", {"colorOverLifetime.*", "main.startColor"}},
            {"addColorGradientStop", {"colorOverLifetime.*", "main.startColor"}},
            {"setColorGradient", {"colorOverLifetime.*", "main.startColor"}},
            {"clearColorGradient", {"colorOverLifetime.*", "main.startColor"}},
            {"setColorGradientUseSRGB", {"colorOverLifetime.*", "main.startColor"}},
            {"setAlphaInitializer", {"main.startColor"}},
            {"setAlphaModifier", {"colorOverLifetime.*", "main.startColor"}},
            {"setSizeInitializer", {"main.startSize", "main.startSize3D", "main.startSizeY", "main.startSizeZ"}},
            {"setSizeModifier", {"sizeOverLifetime.*", "main.startSize", "main.startSize3D"}},
            {"setSpriteIntializer", {"textureSheetAnimation.*"}},
            {"setSpriteModifier", {"textureSheetAnimation.*"}},
            {"setRotationInitializer", {"main.startRotation", "main.startRotationX", "main.startRotationY", "main.startRotation3D"}},
            {"setRotationModifier", {"rotationOverLifetime.*", "main.startRotation", "main.startRotationX", "main.startRotationY", "main.startRotation3D"}},
            {"setScaleInitializer", {"main.startSize", "main.startSize3D", "main.startSizeY", "main.startSizeZ"}},
            {"setScaleModifier", {"sizeOverLifetime.*", "main.startSize", "main.startSize3D"}},
            {"setEmitter", {}},
            {"setMaxPerUpdate", {}},
            {"setPositionModifier", {}},
        };
        return map;
    }

    const char* moduleOf(const std::string& legacySetter){
        const auto& map = legacyMappings();
        auto it = map.find(legacySetter);
        if (it == map.end() || it->second.empty()) return nullptr;
        return it->second.front().c_str();
    }

}

// a legacy setter on a version 2 system: its legacy fields are set (above), then the module
// fields they stand for take the values the upgrade gives them; one deprecation warning per
// setter and process
static void mapLegacySetter(ParticleSystemComponent& component, const char* setter, Scene* scene, Entity entity){
    if (component.version < 2) return;
    static std::unordered_set<std::string> warned;
    const auto& map = legacyMappings();
    auto it = map.find(setter);
    const bool mapped = it != map.end() && !it->second.empty();
    if (warned.insert(setter).second){
        if (mapped){
            Log::warn("ParticleSystem:%s() is a legacy (version 1) setter: on this version 2 system it sets %s; use the modules (ps.main, ps.emission, ...)",
                      setter, moduleOf(setter));
        }else{
            Log::warn("ParticleSystem:%s() is a legacy (version 1) setter with no module equivalent: it has no effect on a version 2 system", setter);
        }
    }
    if (!mapped) return;
    Entity target = entity;
    if (ActionComponent* action = scene->findComponent<ActionComponent>(entity)){
        if (action->target != NULL_ENTITY && scene->isEntityCreated(action->target)) target = action->target;
    }
    ParticleSystemComponent legacy = component;
    legacy.version = 1;
    const particles::UpgradeResult result = particles::upgradeLegacy(legacy, scene->findComponent<PointsComponent>(target) != nullptr, true);
    for (const std::string& path : it->second){
        if (path.size() > 2 && path.compare(path.size() - 2, 2, ".*") == 0){
            for (const particles::FieldInfo* field : particles::moduleFields(path.substr(0, path.size() - 2))){
                particles::copyField(*field, result.component, component);
            }
        }else if (const particles::FieldInfo* field = particles::findField(path)){
            particles::copyField(*field, result.component, component);
        }
    }
}

ParticleSystem::ParticleSystem(Scene* scene): Action(scene){
    addComponent<ParticleSystemComponent>();
}

ParticleSystem::ParticleSystem(Scene* scene, bool modules): Action(scene){
    ParticleSystemComponent component;
    if (modules){
        component.version = 2;
    }
    addComponent<ParticleSystemComponent>(component);
    if (modules){
        setupDefaultRenderer(scene, getEntity());
    }
}

ParticleSystem::ParticleSystem(Scene* scene, Entity entity): Action(scene, entity){

}

ParticleSystem::~ParticleSystem(){

}

ParticleSystem::ParticleSystem(const ParticleSystem& rhs) = default;
ParticleSystem& ParticleSystem::operator=(const ParticleSystem& rhs) = default;
ParticleSystem::ParticleSystem(ParticleSystem&& rhs) noexcept = default;
ParticleSystem& ParticleSystem::operator=(ParticleSystem&& rhs) noexcept = default;

void ParticleSystem::reset(){
    scene->getSystem<ActionSystem>()->particleActionReset(getEntity());
}

void ParticleSystem::setMaxParticles(unsigned int maxParticles){
    ParticleSystemComponent& particomp = getComponent<ParticleSystemComponent>();

    particomp.maxParticles = maxParticles;

    mapLegacySetter(particomp, "setMaxParticles", scene, getEntity());
}

unsigned int ParticleSystem::getMaxParticles() const{
    ParticleSystemComponent& particomp = getComponent<ParticleSystemComponent>();

    return particomp.maxParticles;
}

void ParticleSystem::setRate(int rate){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();
    
    partAnim.rate = rate;

    mapLegacySetter(partAnim, "setRate", scene, getEntity());
}

int ParticleSystem::getRate() const{
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();
    
    return partAnim.rate;
}

void ParticleSystem::setEmitter(bool emitter){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    partAnim.emitter = emitter;

    mapLegacySetter(partAnim, "setEmitter", scene, getEntity());
}

bool ParticleSystem::isEmitter() const{
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    return partAnim.emitter;
}

void ParticleSystem::setLoop(bool loop){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    partAnim.loop = loop;

    mapLegacySetter(partAnim, "setLoop", scene, getEntity());
}

bool ParticleSystem::isLoop() const{
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    return partAnim.loop;
}

void ParticleSystem::setLocalSpace(bool localSpace){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    partAnim.localSpace = localSpace;

    mapLegacySetter(partAnim, "setLocalSpace", scene, getEntity());
}

bool ParticleSystem::isLocalSpace() const{
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    return partAnim.localSpace;
}

void ParticleSystem::setUseUnscaledTime(bool useUnscaledTime){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    partAnim.useUnscaledTime = useUnscaledTime;
}

bool ParticleSystem::isUseUnscaledTime() const{
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    return partAnim.useUnscaledTime;
}

void ParticleSystem::setMaxPerUpdate(int maxPerUpdate){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();
    
    partAnim.maxPerUpdate = maxPerUpdate;

    mapLegacySetter(partAnim, "setMaxPerUpdate", scene, getEntity());
}

int ParticleSystem::getMaxPerUpdate() const{
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();
    
    return partAnim.maxPerUpdate;
}

void ParticleSystem::setLifeInitializer(float life){
    setLifeInitializer(life, life);
}

void ParticleSystem::setLifeInitializer(float minLife, float maxLife){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    partAnim.lifeInitializer.minLife = minLife;
    partAnim.lifeInitializer.maxLife = maxLife;

    mapLegacySetter(partAnim, "setLifeInitializer", scene, getEntity());
}

void ParticleSystem::setPositionInitializer(Vector3 position){
    setPositionInitializer(position, position);
}

void ParticleSystem::setPositionInitializer(Vector3 minPosition, Vector3 maxPosition){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    partAnim.positionInitializer.shape = ParticleEmitterShape::Box;
    partAnim.positionInitializer.minPosition = minPosition;
    partAnim.positionInitializer.maxPosition = maxPosition;

    mapLegacySetter(partAnim, "setPositionInitializer", scene, getEntity());
}

void ParticleSystem::setPositionInitializerShape(ParticleEmitterShape shape){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    partAnim.positionInitializer.shape = shape;

    mapLegacySetter(partAnim, "setPositionInitializerShape", scene, getEntity());
}

ParticleEmitterShape ParticleSystem::getPositionInitializerShape() const{
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    return partAnim.positionInitializer.shape;
}

void ParticleSystem::setSpherePositionInitializer(float radius){
    setSpherePositionInitializer(radius, 0.0f);
}

void ParticleSystem::setSpherePositionInitializer(float radius, float innerRadius){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    partAnim.positionInitializer.shape = ParticleEmitterShape::Sphere;
    partAnim.positionInitializer.radius = radius;
    partAnim.positionInitializer.innerRadius = innerRadius;

    mapLegacySetter(partAnim, "setSpherePositionInitializer", scene, getEntity());
}

void ParticleSystem::setHemispherePositionInitializer(float radius){
    setHemispherePositionInitializer(radius, 0.0f);
}

void ParticleSystem::setHemispherePositionInitializer(float radius, float innerRadius){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    partAnim.positionInitializer.shape = ParticleEmitterShape::Hemisphere;
    partAnim.positionInitializer.radius = radius;
    partAnim.positionInitializer.innerRadius = innerRadius;

    mapLegacySetter(partAnim, "setHemispherePositionInitializer", scene, getEntity());
}

void ParticleSystem::setCirclePositionInitializer(float radius){
    setCirclePositionInitializer(radius, 0.0f);
}

void ParticleSystem::setCirclePositionInitializer(float radius, float innerRadius){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    partAnim.positionInitializer.shape = ParticleEmitterShape::Circle;
    partAnim.positionInitializer.radius = radius;
    partAnim.positionInitializer.innerRadius = innerRadius;

    mapLegacySetter(partAnim, "setCirclePositionInitializer", scene, getEntity());
}

void ParticleSystem::setConePositionInitializer(float angle, float height){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    partAnim.positionInitializer.shape = ParticleEmitterShape::Cone;
    partAnim.positionInitializer.coneAngle = angle;
    partAnim.positionInitializer.coneHeight = height;

    mapLegacySetter(partAnim, "setConePositionInitializer", scene, getEntity());
}

void ParticleSystem::addBurst(float time, int count){
    addBurst(time, count, count);
}

void ParticleSystem::addBurst(float time, int minCount, int maxCount){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    ParticleBurst burst;
    burst.time = time;
    burst.minCount = minCount;
    burst.maxCount = maxCount;
    partAnim.bursts.push_back(burst);
    partAnim.currentBurst = 0;

    mapLegacySetter(partAnim, "addBurst", scene, getEntity());
}

void ParticleSystem::setBursts(std::vector<ParticleBurst> bursts){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    partAnim.bursts = bursts;
    partAnim.currentBurst = 0;

    mapLegacySetter(partAnim, "setBursts", scene, getEntity());
}

void ParticleSystem::clearBursts(){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    partAnim.bursts.clear();
    partAnim.currentBurst = 0;

    mapLegacySetter(partAnim, "clearBursts", scene, getEntity());
}

void ParticleSystem::setPositionModifier(float fromTime, float toTime, Vector3 fromPosition, Vector3 toPosition){
    setPositionModifier(fromTime, toTime, fromPosition, toPosition, EaseType::LINEAR);
}

void ParticleSystem::setPositionModifier(float fromTime, float toTime, Vector3 fromPosition, Vector3 toPosition, EaseType functionType){
    setPositionModifier(fromTime, toTime, fromPosition, toPosition, Ease(functionType));
}

void ParticleSystem::setPositionModifier(float fromTime, float toTime, Vector3 fromPosition, Vector3 toPosition, Ease function){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    partAnim.positionModifier.fromTime = fromTime;
    partAnim.positionModifier.toTime = toTime;
    partAnim.positionModifier.fromPosition = fromPosition;
    partAnim.positionModifier.toPosition = toPosition;
    partAnim.positionModifier.function = function;

    mapLegacySetter(partAnim, "setPositionModifier", scene, getEntity());
}

void ParticleSystem::setVelocityInitializer(Vector3 velocity){
    setVelocityInitializer(velocity, velocity);
}

void ParticleSystem::setVelocityInitializer(Vector3 minVelocity, Vector3 maxVelocity){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    partAnim.velocityInitializer.minVelocity = minVelocity;
    partAnim.velocityInitializer.maxVelocity = maxVelocity;

    mapLegacySetter(partAnim, "setVelocityInitializer", scene, getEntity());
}

void ParticleSystem::setVelocityModifier(float fromTime, float toTime, Vector3 fromVelocity, Vector3 toVelocity){
    setVelocityModifier(fromTime, toTime, fromVelocity, toVelocity, EaseType::LINEAR);
}

void ParticleSystem::setVelocityModifier(float fromTime, float toTime, Vector3 fromVelocity, Vector3 toVelocity, EaseType functionType){
    setVelocityModifier(fromTime, toTime, fromVelocity, toVelocity, Ease(functionType));
}

void ParticleSystem::setVelocityModifier(float fromTime, float toTime, Vector3 fromVelocity, Vector3 toVelocity, Ease function){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    partAnim.velocityModifier.fromTime = fromTime;
    partAnim.velocityModifier.toTime = toTime;
    partAnim.velocityModifier.fromVelocity = fromVelocity;
    partAnim.velocityModifier.toVelocity = toVelocity;
    partAnim.velocityModifier.function = function;

    mapLegacySetter(partAnim, "setVelocityModifier", scene, getEntity());
}

void ParticleSystem::setAccelerationInitializer(Vector3 acceleration){
    setAccelerationInitializer(acceleration, acceleration);
}

void ParticleSystem::setAccelerationInitializer(Vector3 minAcceleration, Vector3 maxAcceleration){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    partAnim.accelerationInitializer.minAcceleration = minAcceleration;
    partAnim.accelerationInitializer.maxAcceleration = maxAcceleration;

    mapLegacySetter(partAnim, "setAccelerationInitializer", scene, getEntity());
}

void ParticleSystem::setAccelerationModifier(float fromTime, float toTime, Vector3 fromAcceleration, Vector3 toAcceleration){
    setAccelerationModifier(fromTime, toTime, fromAcceleration, toAcceleration, EaseType::LINEAR);
}

void ParticleSystem::setAccelerationModifier(float fromTime, float toTime, Vector3 fromAcceleration, Vector3 toAcceleration, EaseType functionType){
    setAccelerationModifier(fromTime, toTime, fromAcceleration, toAcceleration, Ease(functionType));
}

void ParticleSystem::setAccelerationModifier(float fromTime, float toTime, Vector3 fromAcceleration, Vector3 toAcceleration, Ease function){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    partAnim.accelerationModifier.fromTime = fromTime;
    partAnim.accelerationModifier.toTime = toTime;
    partAnim.accelerationModifier.fromAcceleration = fromAcceleration;
    partAnim.accelerationModifier.toAcceleration = toAcceleration;
    partAnim.accelerationModifier.function = function;

    mapLegacySetter(partAnim, "setAccelerationModifier", scene, getEntity());
}

void ParticleSystem::setColorInitializer(Vector3 color){
    setColorInitializer(color, color);
}

void ParticleSystem::setColorInitializer(Vector3 minColor, Vector3 maxColor){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    partAnim.colorInitializer.minColor = minColor;
    partAnim.colorInitializer.maxColor = maxColor;

    mapLegacySetter(partAnim, "setColorInitializer", scene, getEntity());
}

void ParticleSystem::setColorModifier(float fromTime, float toTime, Vector3 fromColor, Vector3 toColor){
    setColorModifier(fromTime, toTime, fromColor, toColor, EaseType::LINEAR);
}

void ParticleSystem::setColorModifier(float fromTime, float toTime, Vector3 fromColor, Vector3 toColor, EaseType functionType){
    setColorModifier(fromTime, toTime, fromColor, toColor, Ease(functionType));
}

void ParticleSystem::setColorModifier(float fromTime, float toTime, Vector3 fromColor, Vector3 toColor, Ease function){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    partAnim.colorModifier.fromTime = fromTime;
    partAnim.colorModifier.toTime = toTime;
    partAnim.colorModifier.fromColor = fromColor;
    partAnim.colorModifier.toColor = toColor;
    partAnim.colorModifier.function = function;

    mapLegacySetter(partAnim, "setColorModifier", scene, getEntity());
}

void ParticleSystem::addColorGradientStop(float time, Vector3 color){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    ParticleColorGradientStop stop;
    stop.time = time;
    stop.color = color;
    partAnim.colorGradient.stops.push_back(stop);
    partAnim.colorGradient.normalize();

    mapLegacySetter(partAnim, "addColorGradientStop", scene, getEntity());
}

void ParticleSystem::setColorGradient(std::vector<ParticleColorGradientStop> stops){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    partAnim.colorGradient.stops = stops;
    partAnim.colorGradient.normalize();

    mapLegacySetter(partAnim, "setColorGradient", scene, getEntity());
}

void ParticleSystem::clearColorGradient(){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    partAnim.colorGradient.stops.clear();

    mapLegacySetter(partAnim, "clearColorGradient", scene, getEntity());
}

void ParticleSystem::setColorGradientUseSRGB(bool useSRGB){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    partAnim.colorGradient.useSRGB = useSRGB;

    mapLegacySetter(partAnim, "setColorGradientUseSRGB", scene, getEntity());
}

void ParticleSystem::setAlphaInitializer(float alpha){
    setAlphaInitializer(alpha, alpha);
}

void ParticleSystem::setAlphaInitializer(float minAlpha, float maxAlpha){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    partAnim.alphaInitializer.minAlpha = minAlpha;
    partAnim.alphaInitializer.maxAlpha = maxAlpha;

    mapLegacySetter(partAnim, "setAlphaInitializer", scene, getEntity());
}

void ParticleSystem::setAlphaModifier(float fromTime, float toTime, float fromAlpha, float toAlpha){
    setAlphaModifier(fromTime, toTime, fromAlpha, toAlpha, EaseType::LINEAR);
}

void ParticleSystem::setAlphaModifier(float fromTime, float toTime, float fromAlpha, float toAlpha, EaseType functionType){
    setAlphaModifier(fromTime, toTime, fromAlpha, toAlpha, Ease(functionType));
}

void ParticleSystem::setAlphaModifier(float fromTime, float toTime, float fromAlpha, float toAlpha, Ease function){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    partAnim.alphaModifier.fromTime = fromTime;
    partAnim.alphaModifier.toTime = toTime;
    partAnim.alphaModifier.fromAlpha = fromAlpha;
    partAnim.alphaModifier.toAlpha = toAlpha;
    partAnim.alphaModifier.function = function;

    mapLegacySetter(partAnim, "setAlphaModifier", scene, getEntity());
}

void ParticleSystem::setSizeInitializer(float size){
    setSizeInitializer(size, size);
}

void ParticleSystem::setSizeInitializer(float minSize, float maxSize){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    partAnim.sizeInitializer.minSize = minSize;
    partAnim.sizeInitializer.maxSize = maxSize;

    mapLegacySetter(partAnim, "setSizeInitializer", scene, getEntity());
}

void ParticleSystem::setSizeModifier(float fromTime, float toTime, float fromSize, float toSize){
    setSizeModifier(fromTime, toTime, fromSize, toSize, EaseType::LINEAR);
}

void ParticleSystem::setSizeModifier(float fromTime, float toTime, float fromSize, float toSize, EaseType functionType){
    setSizeModifier(fromTime, toTime, fromSize, toSize, Ease(functionType));
}

void ParticleSystem::setSizeModifier(float fromTime, float toTime, float fromSize, float toSize, Ease function){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    partAnim.sizeModifier.fromTime = fromTime;
    partAnim.sizeModifier.toTime = toTime;
    partAnim.sizeModifier.fromSize = fromSize;
    partAnim.sizeModifier.toSize = toSize;
    partAnim.sizeModifier.function = function;

    mapLegacySetter(partAnim, "setSizeModifier", scene, getEntity());
}

void ParticleSystem::setSpriteIntializer(std::vector<int> frames){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    partAnim.spriteInitializer.frames = frames;

    mapLegacySetter(partAnim, "setSpriteIntializer", scene, getEntity());
}

void ParticleSystem::setSpriteIntializer(int minFrame, int maxFrame){
    if (minFrame <= maxFrame){
        ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();
        partAnim.spriteInitializer.frames.clear();
        for (int i = minFrame; i <= maxFrame; i++){
            partAnim.spriteInitializer.frames.push_back(i);
        }
    }

    mapLegacySetter(getComponent<ParticleSystemComponent>(), "setSpriteIntializer", scene, getEntity());
}

void ParticleSystem::setSpriteModifier(float fromTime, float toTime, std::vector<int> frames){
    setSpriteModifier(fromTime, toTime, frames, EaseType::LINEAR);
}

void ParticleSystem::setSpriteModifier(float fromTime, float toTime, std::vector<int> frames, EaseType functionType){
    setSpriteModifier(fromTime, toTime, frames, Ease(functionType));
}

void ParticleSystem::setSpriteModifier(float fromTime, float toTime, std::vector<int> frames, Ease function){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    partAnim.spriteModifier.fromTime = fromTime;
    partAnim.spriteModifier.toTime = toTime;
    partAnim.spriteModifier.frames = frames;
    partAnim.spriteModifier.function = function;

    mapLegacySetter(partAnim, "setSpriteModifier", scene, getEntity());
}

void ParticleSystem::setRotationInitializer(Quaternion rotation){
    setRotationInitializer(rotation, rotation);
}

void ParticleSystem::setRotationInitializer(float rotation){
    setRotationInitializer(rotation, rotation);
}

void ParticleSystem::setRotationInitializer(Quaternion minRotation, Quaternion maxRotation){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    partAnim.rotationInitializer.minRotation = minRotation;
    partAnim.rotationInitializer.maxRotation = maxRotation;

    mapLegacySetter(partAnim, "setRotationInitializer", scene, getEntity());
}

void ParticleSystem::setRotationInitializer(float minRotation, float maxRotation){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    partAnim.rotationInitializer.minRotation = Quaternion(0, 0, minRotation);
    partAnim.rotationInitializer.maxRotation = Quaternion(0, 0, maxRotation);

    mapLegacySetter(partAnim, "setRotationInitializer", scene, getEntity());
}

void ParticleSystem::setRotationModifier(float fromTime, float toTime, float fromRotation, float toRotation){
    setRotationModifier(fromTime, toTime, fromRotation, toRotation, EaseType::LINEAR);
}

void ParticleSystem::setRotationModifier(float fromTime, float toTime, Quaternion fromRotation, Quaternion toRotation){
    setRotationModifier(fromTime, toTime, fromRotation, toRotation, EaseType::LINEAR);
}

void ParticleSystem::setRotationModifier(float fromTime, float toTime, float fromRotation, float toRotation, EaseType functionType){
    setRotationModifier(fromTime, toTime, fromRotation, toRotation, Ease(functionType));
}

void ParticleSystem::setRotationModifier(float fromTime, float toTime, float fromRotation, float toRotation, Ease function){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    partAnim.rotationModifier.fromTime = fromTime;
    partAnim.rotationModifier.toTime = toTime;
    partAnim.rotationModifier.fromRotation = Quaternion(0, 0, fromRotation);
    partAnim.rotationModifier.toRotation = Quaternion(0, 0, toRotation);;
    partAnim.rotationModifier.function = function;

    mapLegacySetter(partAnim, "setRotationModifier", scene, getEntity());
}

void ParticleSystem::setRotationModifier(float fromTime, float toTime, Quaternion fromRotation, Quaternion toRotation, EaseType functionType){
    setRotationModifier(fromTime, toTime, fromRotation, toRotation, Ease(functionType));
}

void ParticleSystem::setRotationModifier(float fromTime, float toTime, Quaternion fromRotation, Quaternion toRotation, Ease function){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    partAnim.rotationModifier.fromTime = fromTime;
    partAnim.rotationModifier.toTime = toTime;
    partAnim.rotationModifier.fromRotation = fromRotation;
    partAnim.rotationModifier.toRotation = toRotation;
    partAnim.rotationModifier.function = function;

    mapLegacySetter(partAnim, "setRotationModifier", scene, getEntity());
}

void ParticleSystem::setScaleInitializer(float scale){
    setScaleInitializer(scale, scale);
}

void ParticleSystem::setScaleInitializer(Vector3 scale){
    setScaleInitializer(scale, scale);
}

void ParticleSystem::setScaleInitializer(float minScale, float maxScale){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    partAnim.scaleInitializer.minScale = Vector3(minScale, minScale, minScale);
    partAnim.scaleInitializer.maxScale = Vector3(maxScale, maxScale, maxScale);

    mapLegacySetter(partAnim, "setScaleInitializer", scene, getEntity());
}

void ParticleSystem::setScaleInitializer(Vector3 minScale, Vector3 maxScale){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    partAnim.scaleInitializer.minScale = minScale;
    partAnim.scaleInitializer.maxScale = maxScale;

    mapLegacySetter(partAnim, "setScaleInitializer", scene, getEntity());
}

void ParticleSystem::setScaleModifier(float fromTime, float toTime, float fromScale, float toScale){
    setScaleModifier(fromTime, toTime, Vector3(fromScale, fromScale, fromScale), Vector3(toScale, toScale, toScale), EaseType::LINEAR);
}

void ParticleSystem::setScaleModifier(float fromTime, float toTime, Vector3 fromScale, Vector3 toScale){
    setScaleModifier(fromTime, toTime, fromScale, toScale, EaseType::LINEAR);
}

void ParticleSystem::setScaleModifier(float fromTime, float toTime, Vector3 fromScale, Vector3 toScale, EaseType functionType){
    setScaleModifier(fromTime, toTime, fromScale, toScale, Ease(functionType));
}

void ParticleSystem::setScaleModifier(float fromTime, float toTime, Vector3 fromScale, Vector3 toScale, Ease function){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    partAnim.scaleModifier.fromTime = fromTime;
    partAnim.scaleModifier.toTime = toTime;
    partAnim.scaleModifier.fromScale = fromScale;
    partAnim.scaleModifier.toScale = toScale;
    partAnim.scaleModifier.function = function;

    mapLegacySetter(partAnim, "setScaleModifier", scene, getEntity());
}

void ParticleSystem::setEnabled(bool enabled){
    getComponent<ParticleSystemComponent>().enabled = enabled;
}

bool ParticleSystem::isEnabled() const{
    return getComponent<ParticleSystemComponent>().enabled;
}

// ------------------------------------------------------------------ version 2

void ParticleSystem::play(bool withChildren){
    if (getVersion() < 2){
        start();
        return;
    }
    scene->getSystem<ActionSystem>()->particleSystemPlay(getEntity(), withChildren);
}

void ParticleSystem::stop(bool withChildren, ParticleSystemStopBehavior behavior){
    if (getVersion() < 2){
        Action::stop();
        return;
    }
    scene->getSystem<ActionSystem>()->particleSystemStop(getEntity(), withChildren, behavior);
}

void ParticleSystem::pause(bool withChildren){
    if (getVersion() < 2){
        Action::pause();
        return;
    }
    scene->getSystem<ActionSystem>()->particleSystemPause(getEntity(), withChildren);
}

void ParticleSystem::clear(bool withChildren){
    if (getVersion() < 2){
        reset();
        return;
    }
    scene->getSystem<ActionSystem>()->particleSystemClear(getEntity(), withChildren);
}

int ParticleSystem::emit(int count){
    if (getVersion() < 2){
        Log::warn("ParticleSystem:emit() needs a version 2 system: call upgrade() first");
        return 0;
    }
    return scene->getSystem<ActionSystem>()->particleSystemEmit(getEntity(), count);
}

void ParticleSystem::simulate(float time, bool withChildren, bool restart){
    if (getVersion() < 2){
        Log::warn("ParticleSystem:simulate() needs a version 2 system: call upgrade() first");
        return;
    }
    scene->getSystem<ActionSystem>()->particleSystemSimulate(getEntity(), time, withChildren, restart);
}

bool ParticleSystem::isPlaying() const{
    const ParticleSystemComponent& c = getComponent<ParticleSystemComponent>();
    if (c.version < 2) return isRunning();
    return c.state.playing && !c.state.paused;
}

bool ParticleSystem::isPaused() const{
    const ParticleSystemComponent& c = getComponent<ParticleSystemComponent>();
    if (c.version < 2) return Action::isPaused();
    return c.state.playing && c.state.paused;
}

bool ParticleSystem::isStopped() const{
    const ParticleSystemComponent& c = getComponent<ParticleSystemComponent>();
    if (c.version < 2) return Action::isStopped();
    return !c.state.playing;
}

bool ParticleSystem::isEmitting() const{
    const ParticleSystemComponent& c = getComponent<ParticleSystemComponent>();
    if (c.version < 2) return c.emitter && isRunning();
    return c.state.playing && c.state.emitting && !c.state.paused;
}

bool ParticleSystem::isAlive(bool withChildren) const{
    const ParticleSystemComponent& c = getComponent<ParticleSystemComponent>();
    if (c.version < 2) return isRunning();
    for (Entity system : scene->getSystem<ActionSystem>()->particleSystemsOf(getEntity(), withChildren)){
        const ParticleSystemComponent* other = scene->findComponent<ParticleSystemComponent>(system);
        if (other && particles::isAlive(*other)) return true;
    }
    return false;
}

unsigned int ParticleSystem::getParticleCount() const{
    const ParticleSystemComponent& c = getComponent<ParticleSystemComponent>();
    if (c.version < 2){
        unsigned int alive = 0;
        for (const ParticleData& particle : c.particles){
            if (particle.life > particle.time) alive++;
        }
        return alive;
    }
    return (unsigned int)c.state.count();
}

float ParticleSystem::getTime() const{
    const ParticleSystemComponent& c = getComponent<ParticleSystemComponent>();
    if (c.version < 2) return getTimeCount();
    return c.state.time;
}

void ParticleSystem::setTime(float time){
    ParticleSystemComponent& c = getComponent<ParticleSystemComponent>();
    if (c.version < 2){
        Log::warn("ParticleSystem.time needs a version 2 system: call upgrade() first");
        return;
    }
    // the clock moves; bursts before it count as done for this cycle
    const float duration = std::max(0.05f, c.main.duration);
    c.state.time = std::min(std::max(0.0f, time), duration);
    c.state.bursts.assign(c.emission.bursts.size(), ParticleBurstState());
    for (size_t i = 0; i < c.emission.bursts.size(); i++){
        const ParticleEmissionBurst& burst = c.emission.bursts[i];
        const float interval = std::max(1e-4f, burst.interval);
        int done = 0;
        while ((burst.cycles <= 0 || done < burst.cycles) && burst.time + (float)done * interval < c.state.time && done < 1000000) done++;
        c.state.bursts[i].cyclesDone = done;
    }
}

uint32_t ParticleSystem::getRandomSeed() const{
    return getComponent<ParticleSystemComponent>().main.randomSeed;
}

void ParticleSystem::setRandomSeed(uint32_t seed){
    getComponent<ParticleSystemComponent>().main.randomSeed = seed;
}

bool ParticleSystem::getUseAutoRandomSeed() const{
    return getComponent<ParticleSystemComponent>().main.autoRandomSeed;
}

void ParticleSystem::setUseAutoRandomSeed(bool automatic){
    getComponent<ParticleSystemComponent>().main.autoRandomSeed = automatic;
}

bool ParticleSystem::getUseUnscaledTime() const{
    return getComponent<ParticleSystemComponent>().useUnscaledTime;
}

void ParticleSystem::setUseUnscaledTime(bool unscaled){
    getComponent<ParticleSystemComponent>().useUnscaledTime = unscaled;
}

int ParticleSystem::getVersion() const{
    return getComponent<ParticleSystemComponent>().version;
}

void ParticleSystem::setVersion(int version){
    const int current = getVersion();
    if (version == current) return;
    if (version == 2 && current == 1){
        const particles::UpgradeReport report = upgrade(false);
        if (!report.upgraded){
            Log::error("ParticleSystem.version = 2: not upgraded (a position modifier has no module equivalent): call upgrade(true) to drop it");
        }
        return;
    }
    Log::error("ParticleSystem.version: %d -> %d is not supported (only 1 -> 2, the upgrade)", current, version);
}

particles::UpgradeReport ParticleSystem::upgrade(bool force){
    ParticleSystemComponent& c = getComponent<ParticleSystemComponent>();
    if (c.version >= 2){
        particles::UpgradeReport report;
        return report;
    }
    ActionSystem* actions = scene->getSystem<ActionSystem>().get();
    const Entity target = actions->particleTarget(getEntity());
    particles::UpgradeResult result = particles::upgradeLegacy(c, legacyTargetOf(scene, target), force);
    if (result.report.upgraded){
        // the legacy particles go (the system restarts with the modules when played)
        actions->particleActionReset(getEntity());
        ParticleSystemComponent& now = getComponent<ParticleSystemComponent>();
        result.component.enabled = now.enabled;
        now = result.component;
    }
    return result.report;
}

void ParticleSystem::setupDefaultRenderer(Scene* scene, Entity entity){
    if (!scene->findComponent<Transform>(entity)){
        scene->addComponent<Transform>(entity, Transform());
    }
    if (!scene->findComponent<MeshComponent>(entity)){
        scene->addComponent<MeshComponent>(entity, MeshComponent());
        applyRendererMesh(scene, entity, "quad");
        MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);
        Material& material = mesh.submeshes[0].material;
        material.name = "Default-Particle";
        material.shader = "library:unlit";
        material.shaderFeatures = {"PARTICLE_TEXTURE"};
        material.blendMode = MaterialBlendMode::ALPHA;
        material.depthWrite = MaterialDepthWrite::DISABLED;
        material.cullMode = MaterialCullMode::OFF;
        mesh.castShadows = false;
        mesh.receiveShadows = false;
    }
    if (!scene->findComponent<InstancedMeshComponent>(entity)){
        scene->getSystem<MeshSystem>()->createInstancedMesh(entity);
    }
    InstancedMeshComponent& instmesh = scene->getComponent<InstancedMeshComponent>(entity);
    const ParticleSystemComponent* ps = scene->findComponent<ParticleSystemComponent>(entity);
    instmesh.maxInstances = ps ? std::max(1u, std::min(ps->main.maxParticles, 100000u)) : 1000u;
    instmesh.instances.clear();
    // particles move outside any bounds the instances were culled with last frame
    instmesh.cullInstances = false;
    if (ActionComponent* action = scene->findComponent<ActionComponent>(entity)){
        action->target = entity;
        action->ownedTarget = false;
    }
}

particles::LegacyTarget ParticleSystem::legacyTargetOf(Scene* scene, Entity target){
    if (scene->findComponent<PointsComponent>(target)) return particles::LegacyTarget::Points;
    if (const InstancedMeshComponent* instmesh = scene->findComponent<InstancedMeshComponent>(target)){
        if (instmesh->instancedCylindricalBillboard) return particles::LegacyTarget::VerticalBillboard;
        if (instmesh->instancedBillboard) return particles::LegacyTarget::Billboard;
    }
    return particles::LegacyTarget::Mesh;
}

bool ParticleSystem::applyRendererMesh(Scene* scene, Entity entity, const std::string& mesh){
    MeshComponent* component = scene->findComponent<MeshComponent>(entity);
    if (!component) return false;
    MeshSystem* meshes = scene->getSystem<MeshSystem>().get();
    if (mesh == "quad" || mesh.empty()){
        meshes->createWall(*component, 1.0f, 1.0f);
    }else if (mesh == "cube"){
        meshes->createBox(*component, 1.0f, 1.0f, 1.0f);
    }else if (mesh == "sphere"){
        meshes->createSphere(*component, 0.5f, 24, 12);
    }else if (mesh == "cylinder"){
        meshes->createCylinder(*component, 0.5f, 0.5f, 2.0f, 24, 1);
    }else if (mesh == "capsule"){
        meshes->createCapsule(*component, 0.5f, 0.5f, 2.0f, 24, 12);
    }else if (mesh == "plane"){
        meshes->createPlane(*component, 10.0f, 10.0f);
    }else{
        return false;
    }
    return true;
}
