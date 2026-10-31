// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ParticleSystem.h"

#include "subsystem/ActionSystem.h"

using namespace jasperengine;

ParticleSystem::ParticleSystem(Scene* scene): Action(scene){
    addComponent<ParticleSystemComponent>();
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
}

unsigned int ParticleSystem::getMaxParticles() const{
    ParticleSystemComponent& particomp = getComponent<ParticleSystemComponent>();

    return particomp.maxParticles;
}

void ParticleSystem::setRate(int rate){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();
    
    partAnim.rate = rate;
}

int ParticleSystem::getRate() const{
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();
    
    return partAnim.rate;
}

void ParticleSystem::setEmitter(bool emitter){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    partAnim.emitter = emitter;
}

bool ParticleSystem::isEmitter() const{
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    return partAnim.emitter;
}

void ParticleSystem::setLoop(bool loop){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    partAnim.loop = loop;
}

bool ParticleSystem::isLoop() const{
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    return partAnim.loop;
}

void ParticleSystem::setLocalSpace(bool localSpace){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    partAnim.localSpace = localSpace;
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
}

void ParticleSystem::setPositionInitializer(Vector3 position){
    setPositionInitializer(position, position);
}

void ParticleSystem::setPositionInitializer(Vector3 minPosition, Vector3 maxPosition){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    partAnim.positionInitializer.shape = ParticleEmitterShape::Box;
    partAnim.positionInitializer.minPosition = minPosition;
    partAnim.positionInitializer.maxPosition = maxPosition;
}

void ParticleSystem::setPositionInitializerShape(ParticleEmitterShape shape){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    partAnim.positionInitializer.shape = shape;
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
}

void ParticleSystem::setHemispherePositionInitializer(float radius){
    setHemispherePositionInitializer(radius, 0.0f);
}

void ParticleSystem::setHemispherePositionInitializer(float radius, float innerRadius){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    partAnim.positionInitializer.shape = ParticleEmitterShape::Hemisphere;
    partAnim.positionInitializer.radius = radius;
    partAnim.positionInitializer.innerRadius = innerRadius;
}

void ParticleSystem::setCirclePositionInitializer(float radius){
    setCirclePositionInitializer(radius, 0.0f);
}

void ParticleSystem::setCirclePositionInitializer(float radius, float innerRadius){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    partAnim.positionInitializer.shape = ParticleEmitterShape::Circle;
    partAnim.positionInitializer.radius = radius;
    partAnim.positionInitializer.innerRadius = innerRadius;
}

void ParticleSystem::setConePositionInitializer(float angle, float height){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    partAnim.positionInitializer.shape = ParticleEmitterShape::Cone;
    partAnim.positionInitializer.coneAngle = angle;
    partAnim.positionInitializer.coneHeight = height;
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
}

void ParticleSystem::setBursts(std::vector<ParticleBurst> bursts){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    partAnim.bursts = bursts;
    partAnim.currentBurst = 0;
}

void ParticleSystem::clearBursts(){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    partAnim.bursts.clear();
    partAnim.currentBurst = 0;
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
}

void ParticleSystem::setVelocityInitializer(Vector3 velocity){
    setVelocityInitializer(velocity, velocity);
}

void ParticleSystem::setVelocityInitializer(Vector3 minVelocity, Vector3 maxVelocity){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    partAnim.velocityInitializer.minVelocity = minVelocity;
    partAnim.velocityInitializer.maxVelocity = maxVelocity;
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
}

void ParticleSystem::setAccelerationInitializer(Vector3 acceleration){
    setAccelerationInitializer(acceleration, acceleration);
}

void ParticleSystem::setAccelerationInitializer(Vector3 minAcceleration, Vector3 maxAcceleration){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    partAnim.accelerationInitializer.minAcceleration = minAcceleration;
    partAnim.accelerationInitializer.maxAcceleration = maxAcceleration;
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
}

void ParticleSystem::setColorInitializer(Vector3 color){
    setColorInitializer(color, color);
}

void ParticleSystem::setColorInitializer(Vector3 minColor, Vector3 maxColor){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    partAnim.colorInitializer.minColor = minColor;
    partAnim.colorInitializer.maxColor = maxColor;
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
}

void ParticleSystem::addColorGradientStop(float time, Vector3 color){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    ParticleColorGradientStop stop;
    stop.time = time;
    stop.color = color;
    partAnim.colorGradient.stops.push_back(stop);
    partAnim.colorGradient.normalize();
}

void ParticleSystem::setColorGradient(std::vector<ParticleColorGradientStop> stops){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    partAnim.colorGradient.stops = stops;
    partAnim.colorGradient.normalize();
}

void ParticleSystem::clearColorGradient(){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    partAnim.colorGradient.stops.clear();
}

void ParticleSystem::setColorGradientUseSRGB(bool useSRGB){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    partAnim.colorGradient.useSRGB = useSRGB;
}

void ParticleSystem::setAlphaInitializer(float alpha){
    setAlphaInitializer(alpha, alpha);
}

void ParticleSystem::setAlphaInitializer(float minAlpha, float maxAlpha){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    partAnim.alphaInitializer.minAlpha = minAlpha;
    partAnim.alphaInitializer.maxAlpha = maxAlpha;
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
}

void ParticleSystem::setSizeInitializer(float size){
    setSizeInitializer(size, size);
}

void ParticleSystem::setSizeInitializer(float minSize, float maxSize){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    partAnim.sizeInitializer.minSize = minSize;
    partAnim.sizeInitializer.maxSize = maxSize;
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
}

void ParticleSystem::setSpriteIntializer(std::vector<int> frames){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    partAnim.spriteInitializer.frames = frames;
}

void ParticleSystem::setSpriteIntializer(int minFrame, int maxFrame){
    if (minFrame <= maxFrame){
        ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();
        partAnim.spriteInitializer.frames.clear();
        for (int i = minFrame; i <= maxFrame; i++){
            partAnim.spriteInitializer.frames.push_back(i);
        }
    }
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
}

void ParticleSystem::setRotationInitializer(float minRotation, float maxRotation){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    partAnim.rotationInitializer.minRotation = Quaternion(0, 0, minRotation);
    partAnim.rotationInitializer.maxRotation = Quaternion(0, 0, maxRotation);
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
}

void ParticleSystem::setScaleInitializer(Vector3 minScale, Vector3 maxScale){
    ParticleSystemComponent& partAnim = getComponent<ParticleSystemComponent>();

    partAnim.scaleInitializer.minScale = minScale;
    partAnim.scaleInitializer.maxScale = maxScale;
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
}

void ParticleSystem::setEnabled(bool enabled){
    getComponent<ParticleSystemComponent>().enabled = enabled;
}

bool ParticleSystem::isEnabled() const{
    return getComponent<ParticleSystemComponent>().enabled;
}
