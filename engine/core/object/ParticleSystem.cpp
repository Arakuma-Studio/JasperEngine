// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ParticleSystem.h"

#include "Log.h"
#include "Scene.h"
#include "component/ActionComponent.h"
#include "component/Transform.h"
#include "particles/LegacyParticleConverter.h"
#include "particles/ParticleEffectCodec.h"
#include "particles/ParticleEffectLibrary.h"
#include "particles/ParticleModules.h"
#include "subsystem/ParticleSubsystem.h"

#include <algorithm>
#include <functional>

using namespace jasperengine;
using namespace jasperengine::particles;

namespace {

    // an old script's ParticleSystem(scene, actionEntity): the system its action controls
    Entity resolveSystemEntity(Scene* scene, Entity entity){
        if (entity == NULL_ENTITY || scene->findComponent<ParticleSystemComponent>(entity))
            return entity;
        if (const ActionComponent* action = scene->findComponent<ActionComponent>(entity)){
            if (action->target != NULL_ENTITY && scene->findComponent<ParticleSystemComponent>(action->target)){
                static bool noted = false;
                if (!noted){
                    noted = true;
                    Log::warn("[Particles] ParticleSystem(scene, entity) of an action entity: the particle system is on its target now (docs/specs/S20-particles.md D1); deprecated");
                }
                return action->target;
            }
        }
        return entity;
    }

    void deprecated(const char* what){
        static std::vector<std::string> noted;
        if (std::find(noted.begin(), noted.end(), what) != noted.end())
            return;
        noted.push_back(what);
        Log::warn("[Particles] ParticleSystem.%s is deprecated: edit the effect's modules (edit_particle_effect, the Inspector, setFloat)", what);
    }

    // an old Action member that has a new name now
    void renamed(const char* what, const char* now){
        static std::vector<std::string> noted;
        if (std::find(noted.begin(), noted.end(), what) != noted.end())
            return;
        noted.push_back(what);
        Log::warn("[Particles] ParticleSystem.%s is deprecated: use %s", what, now);
    }

}

// the effect's old view, changed by a deprecated setter and written back: an asset effect
// becomes the component's inline one (the asset is left as it is)
class ParticleSystem::LegacyEdit{
public:
    explicit LegacyEdit(ParticleSystem& owner): owner(owner){
        ParticleSystemComponent& particles = owner.system();
        if (!particles.effect.empty()){
            if (const ParticleEffectDocument* asset = ParticleSubsystem::getDocument(particles))
                particles.inlineEffect = *asset;
            Log::warn("[Particles] a deprecated setter edits the effect of '%s': the GameObject's own copy now", particles.effect.path.c_str());
            particles.effect.clear();
        }
        legacy = LegacyParticleConverter::extract(particles.inlineEffect);
    }
    ~LegacyEdit(){
        ParticleSystemComponent& particles = owner.system();
        LegacyParticleConverter::apply(legacy, particles.inlineEffect);
        particles.needReload = true;
    }
    LegacyEdit(const LegacyEdit&) = delete;
    LegacyEdit& operator=(const LegacyEdit&) = delete;

    LegacyParticleSystem legacy;

private:
    ParticleSystem& owner;
};

ParticleSystem::ParticleSystem(Scene* scene): GameObject(scene){
    addComponent<ParticleSystemComponent>();
}

ParticleSystem::ParticleSystem(Scene* scene, Entity entity): GameObject(scene, resolveSystemEntity(scene, entity)){
}

ParticleSystem::~ParticleSystem(){
}

ParticleSystem::ParticleSystem(const ParticleSystem& rhs) = default;
ParticleSystem& ParticleSystem::operator=(const ParticleSystem& rhs) = default;
ParticleSystem::ParticleSystem(ParticleSystem&& rhs) noexcept = default;
ParticleSystem& ParticleSystem::operator=(ParticleSystem&& rhs) noexcept = default;

ParticleSystemComponent& ParticleSystem::system() const{
    return getComponent<ParticleSystemComponent>();
}

LegacyParticleSystem ParticleSystem::legacyView() const{
    const ParticleEffectDocument* document = ParticleSubsystem::getDocument(system());
    return document ? LegacyParticleConverter::extract(*document) : LegacyParticleSystem();
}

void ParticleSystem::setEnabled(bool enabled){
    system().enabled = enabled;
}

bool ParticleSystem::isEnabled() const{
    return system().enabled;
}

// ---------------------------------------------------------------- playback

void ParticleSystem::play(){
    scene->getSystem<ParticleSubsystem>()->play(entity);
}

void ParticleSystem::pause(){
    scene->getSystem<ParticleSubsystem>()->pause(entity);
}

void ParticleSystem::stop(bool clear){
    scene->getSystem<ParticleSubsystem>()->stop(entity, clear);
}

void ParticleSystem::restart(){
    scene->getSystem<ParticleSubsystem>()->restart(entity);
}

void ParticleSystem::emit(unsigned int count){
    scene->getSystem<ParticleSubsystem>()->emit(entity, count);
}

void ParticleSystem::emit(unsigned int count, const EmitOverrides& overrides){
    scene->getSystem<ParticleSubsystem>()->emit(entity, count, &overrides);
}

void ParticleSystem::seek(float time){
    scene->getSystem<ParticleSubsystem>()->seek(entity, time);
}

void ParticleSystem::step(unsigned int ticks){
    scene->getSystem<ParticleSubsystem>()->step(entity, ticks);
}

void ParticleSystem::sendTrigger(const std::string& name){
    scene->getSystem<ParticleSubsystem>()->sendTrigger(entity, name);
}

bool ParticleSystem::isPlaying() const{
    return scene->getSystem<ParticleSubsystem>()->isPlaying(entity);
}

bool ParticleSystem::isPaused() const{
    return scene->getSystem<ParticleSubsystem>()->isPaused(entity);
}

bool ParticleSystem::isEmitting() const{
    return scene->getSystem<ParticleSubsystem>()->isEmitting(entity);
}

unsigned int ParticleSystem::getParticleCount() const{
    return scene->getSystem<ParticleSubsystem>()->getParticleCount(entity);
}

float ParticleSystem::getTime() const{
    return (float)scene->getSystem<ParticleSubsystem>()->getTime(entity);
}

void ParticleSystem::setPlayOnAwake(bool playOnAwake){
    system().playOnAwake = playOnAwake;
}

bool ParticleSystem::isPlayOnAwake() const{
    return system().playOnAwake;
}

void ParticleSystem::setPlaybackSpeed(float playbackSpeed){
    system().playbackSpeed = playbackSpeed;
}

float ParticleSystem::getPlaybackSpeed() const{
    return system().playbackSpeed;
}

void ParticleSystem::setUseUnscaledTime(bool useUnscaledTime){
    system().useUnscaledTime = useUnscaledTime;
}

bool ParticleSystem::isUseUnscaledTime() const{
    return system().useUnscaledTime;
}

void ParticleSystem::setSeed(unsigned int seed){
    system().seed = seed;
    system().needReload = true;
}

unsigned int ParticleSystem::getSeed() const{
    return system().seed;
}

void ParticleSystem::setEffect(const std::string& path){
    ParticleSystemComponent& particles = system();
    particles.effect = path;
    particles.needReload = true;
}

std::string ParticleSystem::getEffect() const{
    return system().effect.path;
}

bool ParticleSystem::setInlineEffect(const std::string& json){
    ParticleEffectDocument document;
    std::vector<Diagnostic> diagnostics;
    if (!ParticleEffectCodec::read(json, document, diagnostics)){
        Log::error("[Particles] setInlineEffect: %s", ParticleEffectCodec::format(diagnostics).c_str());
        return false;
    }
    ParticleSystemComponent& particles = system();
    particles.inlineEffect = document;
    particles.effect.clear();
    particles.needReload = true;
    return true;
}

std::string ParticleSystem::getInlineEffect() const{
    return ParticleEffectCodec::write(system().inlineEffect);
}

ParticleEffectDocument ParticleSystem::getEffectDocument() const{
    const ParticleEffectDocument* document = ParticleSubsystem::getDocument(system());
    return document ? *document : system().inlineEffect;
}

void ParticleSystem::setEffectDocument(const ParticleEffectDocument& document){
    ParticleSystemComponent& particles = system();
    if (!particles.effect.empty()){
        Log::verbose("[Particles] %s: a module edit makes the effect of %s its own (the asset is not changed)",
                     particles.effect.path.c_str(), getName().c_str());
        particles.effect.clear();
    }
    particles.inlineEffect = document;
    particles.needReload = true;
}

namespace {

    // the GameObject's Particle System and, with children, every one below it (hierarchy order)
    void eachSystem(Scene* scene, Entity entity, bool withChildren, const std::function<void(Entity)>& visit){
        if (scene->findComponent<ParticleSystemComponent>(entity))
            visit(entity);
        if (!withChildren)
            return;
        // the descendants follow it in the Transform array (depth first): its branch
        auto transforms = scene->getComponentArray<Transform>();
        if (!transforms->hasEntity(entity))
            return;
        const size_t start = transforms->getIndex(entity);
        const size_t last = scene->findBranchLastIndex(entity);
        std::vector<Entity> below;
        for (size_t i = start + 1; i <= last && i < transforms->size(); i++)
            below.push_back(transforms->getEntity(i));
        for (Entity child : below){
            if (scene->isEntityCreated(child) && scene->findComponent<ParticleSystemComponent>(child))
                visit(child);
        }
    }

}

void ParticleSystem::play(bool withChildren){
    ParticleSubsystem* subsystem = scene->getSystem<ParticleSubsystem>().get();
    eachSystem(scene, entity, withChildren, [&](Entity e){ subsystem->play(e); });
}

void ParticleSystem::stop(bool withChildren, bool clear){
    ParticleSubsystem* subsystem = scene->getSystem<ParticleSubsystem>().get();
    eachSystem(scene, entity, withChildren, [&](Entity e){ subsystem->stop(e, clear); });
}

void ParticleSystem::pause(bool withChildren){
    ParticleSubsystem* subsystem = scene->getSystem<ParticleSubsystem>().get();
    eachSystem(scene, entity, withChildren, [&](Entity e){ subsystem->pause(e); });
}

void ParticleSystem::clear(bool withChildren){
    // Unity's Clear: the particles go, the system keeps playing (or stays stopped)
    ParticleSubsystem* subsystem = scene->getSystem<ParticleSubsystem>().get();
    eachSystem(scene, entity, withChildren, [&](Entity e){ subsystem->clear(e); });
}

// ---------------------------------------------------------------- exposed parameters

namespace {

    bool setExposedValue(ParticleSystemComponent& particles, const std::string& name, const Vector4& value){
        const ParticleEffectDocument* document = ParticleSubsystem::getDocument(particles);
        Vector4 current;
        if (!document || !ParticleModules::getExposed(*document, name, current)){
            Log::warn("[Particles] the effect has no exposed parameter '%s'", name.c_str());
            return false;
        }
        for (ParticleExposedValue& v : particles.exposedValues){
            if (v.name == name){
                if (v.value != value){
                    v.value = value;
                    particles.exposedChanged = true;
                }
                return true;
            }
        }
        particles.exposedValues.push_back({name, value});
        particles.exposedChanged = true;
        return true;
    }

    Vector4 getExposedValue(const ParticleSystemComponent& particles, const std::string& name){
        for (const ParticleExposedValue& v : particles.exposedValues){
            if (v.name == name)
                return v.value;
        }
        Vector4 value(0.0f, 0.0f, 0.0f, 0.0f);
        if (const ParticleEffectDocument* document = ParticleSubsystem::getDocument(particles))
            ParticleModules::getExposed(*document, name, value);
        return value;
    }

}

bool ParticleSystem::setFloat(const std::string& name, float value){
    return setExposedValue(system(), name, Vector4(value, 0.0f, 0.0f, 0.0f));
}

bool ParticleSystem::setVector3(const std::string& name, Vector3 value){
    return setExposedValue(system(), name, Vector4(value.x, value.y, value.z, 0.0f));
}

bool ParticleSystem::setColor(const std::string& name, Vector4 value){
    return setExposedValue(system(), name, value);
}

float ParticleSystem::getFloat(const std::string& name) const{
    return getExposedValue(system(), name).x;
}

Vector3 ParticleSystem::getVector3(const std::string& name) const{
    const Vector4 v = getExposedValue(system(), name);
    return Vector3(v.x, v.y, v.z);
}

Vector4 ParticleSystem::getColor(const std::string& name) const{
    return getExposedValue(system(), name);
}

void ParticleSystem::addCollisionPlane(Entity plane){
    system().collisionPlanes.push_back(plane);
}

void ParticleSystem::clearCollisionPlanes(){
    system().collisionPlanes.clear();
}

// ---------------------------------------------------------------- deprecated

void ParticleSystem::start(){
    deprecated("start");
    play();
}

void ParticleSystem::reset(){
    deprecated("reset");
    restart();
}

bool ParticleSystem::isRunning() const{
    return isPlaying();
}

Entity ParticleSystem::getTarget() const{
    renamed("target", "the system's own GameObject (it lives on its old target now)");
    return entity;
}

void ParticleSystem::setSpeed(float speed){
    renamed("speed", "playbackSpeed");
    setPlaybackSpeed(speed);
}

float ParticleSystem::getSpeed() const{
    renamed("speed", "playbackSpeed");
    return getPlaybackSpeed();
}

float ParticleSystem::getTimeCount() const{
    renamed("getTimeCount()", "time");
    return getTime();
}

bool ParticleSystem::isStopped() const{
    renamed("isStopped()", "not (isPlaying or isPaused)");
    return !isPlaying() && !isPaused();
}

void ParticleSystem::setTarget(GameObject* target){
    setTarget(target ? target->getEntity() : NULL_ENTITY);
}

void ParticleSystem::setTarget(Entity target){
    // the system renders itself now: it moves onto the target (the old emitter's place)
    deprecated("setTarget");
    if (target == NULL_ENTITY || target == entity || !scene->isEntityCreated(target))
        return;
    ParticleSystemComponent moved = system();
    moved.state.reset();
    moved.needReload = true;
    scene->removeComponent<ParticleSystemComponent>(entity);
    if (ParticleSystemComponent* existing = scene->findComponent<ParticleSystemComponent>(target))
        *existing = std::move(moved);
    else
        scene->addComponent<ParticleSystemComponent>(target, std::move(moved));
    entity = target;
}

void ParticleSystem::setEmitter(bool emitter){
    deprecated("setEmitter");
    if (emitter)
        play();
    else
        stop(false);
}

bool ParticleSystem::isEmitter() const{
    return isEmitting();
}

void ParticleSystem::setMaxParticles(unsigned int maxParticles){
    LegacyEdit edit(*this);
    LegacyParticleSystem& particomp = edit.legacy;

    particomp.maxParticles = maxParticles;
}

unsigned int ParticleSystem::getMaxParticles() const{
    const LegacyParticleSystem particomp = legacyView();

    return particomp.maxParticles;
}

void ParticleSystem::setRate(int rate){
    LegacyEdit edit(*this);
    LegacyParticleSystem& partAnim = edit.legacy;
    
    partAnim.rate = rate;
}

int ParticleSystem::getRate() const{
    const LegacyParticleSystem partAnim = legacyView();
    
    return partAnim.rate;
}

void ParticleSystem::setLoop(bool loop){
    LegacyEdit edit(*this);
    LegacyParticleSystem& partAnim = edit.legacy;

    partAnim.loop = loop;
}

bool ParticleSystem::isLoop() const{
    const LegacyParticleSystem partAnim = legacyView();

    return partAnim.loop;
}

void ParticleSystem::setLocalSpace(bool localSpace){
    LegacyEdit edit(*this);
    LegacyParticleSystem& partAnim = edit.legacy;

    partAnim.localSpace = localSpace;
}

bool ParticleSystem::isLocalSpace() const{
    const LegacyParticleSystem partAnim = legacyView();

    return partAnim.localSpace;
}

void ParticleSystem::setMaxPerUpdate(int maxPerUpdate){
    LegacyEdit edit(*this);
    LegacyParticleSystem& partAnim = edit.legacy;
    
    partAnim.maxPerUpdate = maxPerUpdate;
}

int ParticleSystem::getMaxPerUpdate() const{
    const LegacyParticleSystem partAnim = legacyView();
    
    return partAnim.maxPerUpdate;
}

void ParticleSystem::setLifeInitializer(float life){
    setLifeInitializer(life, life);
}

void ParticleSystem::setLifeInitializer(float minLife, float maxLife){
    LegacyEdit edit(*this);
    LegacyParticleSystem& partAnim = edit.legacy;

    partAnim.lifeInitializer.minLife = minLife;
    partAnim.lifeInitializer.maxLife = maxLife;
}

void ParticleSystem::setPositionInitializer(Vector3 position){
    setPositionInitializer(position, position);
}

void ParticleSystem::setPositionInitializer(Vector3 minPosition, Vector3 maxPosition){
    LegacyEdit edit(*this);
    LegacyParticleSystem& partAnim = edit.legacy;

    partAnim.positionInitializer.shape = ParticleEmitterShape::Box;
    partAnim.positionInitializer.minPosition = minPosition;
    partAnim.positionInitializer.maxPosition = maxPosition;
}

void ParticleSystem::setPositionInitializerShape(ParticleEmitterShape shape){
    LegacyEdit edit(*this);
    LegacyParticleSystem& partAnim = edit.legacy;

    partAnim.positionInitializer.shape = shape;
}

ParticleEmitterShape ParticleSystem::getPositionInitializerShape() const{
    const LegacyParticleSystem partAnim = legacyView();

    return partAnim.positionInitializer.shape;
}

void ParticleSystem::setSpherePositionInitializer(float radius){
    setSpherePositionInitializer(radius, 0.0f);
}

void ParticleSystem::setSpherePositionInitializer(float radius, float innerRadius){
    LegacyEdit edit(*this);
    LegacyParticleSystem& partAnim = edit.legacy;

    partAnim.positionInitializer.shape = ParticleEmitterShape::Sphere;
    partAnim.positionInitializer.radius = radius;
    partAnim.positionInitializer.innerRadius = innerRadius;
}

void ParticleSystem::setHemispherePositionInitializer(float radius){
    setHemispherePositionInitializer(radius, 0.0f);
}

void ParticleSystem::setHemispherePositionInitializer(float radius, float innerRadius){
    LegacyEdit edit(*this);
    LegacyParticleSystem& partAnim = edit.legacy;

    partAnim.positionInitializer.shape = ParticleEmitterShape::Hemisphere;
    partAnim.positionInitializer.radius = radius;
    partAnim.positionInitializer.innerRadius = innerRadius;
}

void ParticleSystem::setCirclePositionInitializer(float radius){
    setCirclePositionInitializer(radius, 0.0f);
}

void ParticleSystem::setCirclePositionInitializer(float radius, float innerRadius){
    LegacyEdit edit(*this);
    LegacyParticleSystem& partAnim = edit.legacy;

    partAnim.positionInitializer.shape = ParticleEmitterShape::Circle;
    partAnim.positionInitializer.radius = radius;
    partAnim.positionInitializer.innerRadius = innerRadius;
}

void ParticleSystem::setConePositionInitializer(float angle, float height){
    LegacyEdit edit(*this);
    LegacyParticleSystem& partAnim = edit.legacy;

    partAnim.positionInitializer.shape = ParticleEmitterShape::Cone;
    partAnim.positionInitializer.coneAngle = angle;
    partAnim.positionInitializer.coneHeight = height;
}

void ParticleSystem::addBurst(float time, int count){
    addBurst(time, count, count);
}

void ParticleSystem::addBurst(float time, int minCount, int maxCount){
    LegacyEdit edit(*this);
    LegacyParticleSystem& partAnim = edit.legacy;

    ParticleBurst burst;
    burst.time = time;
    burst.minCount = minCount;
    burst.maxCount = maxCount;
    partAnim.bursts.push_back(burst);
}

void ParticleSystem::setBursts(std::vector<ParticleBurst> bursts){
    LegacyEdit edit(*this);
    LegacyParticleSystem& partAnim = edit.legacy;

    partAnim.bursts = bursts;
}

void ParticleSystem::clearBursts(){
    LegacyEdit edit(*this);
    LegacyParticleSystem& partAnim = edit.legacy;

    partAnim.bursts.clear();
}

void ParticleSystem::setPositionModifier(float fromTime, float toTime, Vector3 fromPosition, Vector3 toPosition){
    setPositionModifier(fromTime, toTime, fromPosition, toPosition, EaseType::LINEAR);
}

void ParticleSystem::setPositionModifier(float fromTime, float toTime, Vector3 fromPosition, Vector3 toPosition, EaseType functionType){
    setPositionModifier(fromTime, toTime, fromPosition, toPosition, Ease(functionType));
}

void ParticleSystem::setPositionModifier(float fromTime, float toTime, Vector3 fromPosition, Vector3 toPosition, Ease function){
    LegacyEdit edit(*this);
    LegacyParticleSystem& partAnim = edit.legacy;

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
    LegacyEdit edit(*this);
    LegacyParticleSystem& partAnim = edit.legacy;

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
    LegacyEdit edit(*this);
    LegacyParticleSystem& partAnim = edit.legacy;

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
    LegacyEdit edit(*this);
    LegacyParticleSystem& partAnim = edit.legacy;

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
    LegacyEdit edit(*this);
    LegacyParticleSystem& partAnim = edit.legacy;

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
    LegacyEdit edit(*this);
    LegacyParticleSystem& partAnim = edit.legacy;

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
    LegacyEdit edit(*this);
    LegacyParticleSystem& partAnim = edit.legacy;

    partAnim.colorModifier.fromTime = fromTime;
    partAnim.colorModifier.toTime = toTime;
    partAnim.colorModifier.fromColor = fromColor;
    partAnim.colorModifier.toColor = toColor;
    partAnim.colorModifier.function = function;
}

void ParticleSystem::addColorGradientStop(float time, Vector3 color){
    LegacyEdit edit(*this);
    LegacyParticleSystem& partAnim = edit.legacy;

    ParticleColorGradientStop stop;
    stop.time = time;
    stop.color = color;
    partAnim.colorGradient.stops.push_back(stop);
    partAnim.colorGradient.normalize();
}

void ParticleSystem::setColorGradient(std::vector<ParticleColorGradientStop> stops){
    LegacyEdit edit(*this);
    LegacyParticleSystem& partAnim = edit.legacy;

    partAnim.colorGradient.stops = stops;
    partAnim.colorGradient.normalize();
}

void ParticleSystem::clearColorGradient(){
    LegacyEdit edit(*this);
    LegacyParticleSystem& partAnim = edit.legacy;

    partAnim.colorGradient.stops.clear();
}

void ParticleSystem::setColorGradientUseSRGB(bool useSRGB){
    LegacyEdit edit(*this);
    LegacyParticleSystem& partAnim = edit.legacy;

    partAnim.colorGradient.useSRGB = useSRGB;
}

void ParticleSystem::setAlphaInitializer(float alpha){
    setAlphaInitializer(alpha, alpha);
}

void ParticleSystem::setAlphaInitializer(float minAlpha, float maxAlpha){
    LegacyEdit edit(*this);
    LegacyParticleSystem& partAnim = edit.legacy;

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
    LegacyEdit edit(*this);
    LegacyParticleSystem& partAnim = edit.legacy;

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
    LegacyEdit edit(*this);
    LegacyParticleSystem& partAnim = edit.legacy;

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
    LegacyEdit edit(*this);
    LegacyParticleSystem& partAnim = edit.legacy;

    partAnim.sizeModifier.fromTime = fromTime;
    partAnim.sizeModifier.toTime = toTime;
    partAnim.sizeModifier.fromSize = fromSize;
    partAnim.sizeModifier.toSize = toSize;
    partAnim.sizeModifier.function = function;
}

void ParticleSystem::setSpriteIntializer(std::vector<int> frames){
    LegacyEdit edit(*this);
    LegacyParticleSystem& partAnim = edit.legacy;

    partAnim.spriteInitializer.frames = frames;
}

void ParticleSystem::setSpriteIntializer(int minFrame, int maxFrame){
    if (minFrame <= maxFrame){
        LegacyEdit edit(*this);
    LegacyParticleSystem& partAnim = edit.legacy;
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
    LegacyEdit edit(*this);
    LegacyParticleSystem& partAnim = edit.legacy;

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
    LegacyEdit edit(*this);
    LegacyParticleSystem& partAnim = edit.legacy;

    partAnim.rotationInitializer.minRotation = minRotation;
    partAnim.rotationInitializer.maxRotation = maxRotation;
}

void ParticleSystem::setRotationInitializer(float minRotation, float maxRotation){
    LegacyEdit edit(*this);
    LegacyParticleSystem& partAnim = edit.legacy;

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
    LegacyEdit edit(*this);
    LegacyParticleSystem& partAnim = edit.legacy;

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
    LegacyEdit edit(*this);
    LegacyParticleSystem& partAnim = edit.legacy;

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
    LegacyEdit edit(*this);
    LegacyParticleSystem& partAnim = edit.legacy;

    partAnim.scaleInitializer.minScale = Vector3(minScale, minScale, minScale);
    partAnim.scaleInitializer.maxScale = Vector3(maxScale, maxScale, maxScale);
}

void ParticleSystem::setScaleInitializer(Vector3 minScale, Vector3 maxScale){
    LegacyEdit edit(*this);
    LegacyParticleSystem& partAnim = edit.legacy;

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
    LegacyEdit edit(*this);
    LegacyParticleSystem& partAnim = edit.legacy;

    partAnim.scaleModifier.fromTime = fromTime;
    partAnim.scaleModifier.toTime = toTime;
    partAnim.scaleModifier.fromScale = fromScale;
    partAnim.scaleModifier.toScale = toScale;
    partAnim.scaleModifier.function = function;
}
