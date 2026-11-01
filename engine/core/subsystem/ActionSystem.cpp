// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ActionSystem.h"

#include "Scene.h"
#include "util/Color.h"
#include "util/Angle.h"
#include "animation/AnimationPlayerRuntime.h"
#include "animation/KeyframeSampling.h"
#include "subsystem/MeshSystem.h"
#include "lattice/LatticeSolver.h"
#include "subsystem/UISystem.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <functional>
#include <unordered_set>

using namespace jasperengine;

float ActionSystem::getParticleInverseScale(float value){
    if (value == 0.0f)
        return 1.0f;

    return 1.0f / value;
}

Vector3 ActionSystem::getParticleDisplayScale(Vector3 scale, Transform* targetTransform){
    if (!targetTransform)
        return scale;

    return Vector3(
        scale.x * getParticleInverseScale(targetTransform->worldScale.x),
        scale.y * getParticleInverseScale(targetTransform->worldScale.y),
        scale.z * getParticleInverseScale(targetTransform->worldScale.z));
}

bool ActionSystem::isParticleWorldSpace(ParticleSystemComponent& particles, Transform* targetTransform){
    return !particles.localSpace && targetTransform;
}

void ActionSystem::updateParticleTargetTransform(Transform& transform){
    Matrix4 translateMatrix = Matrix4::translateMatrix(transform.position);
    Matrix4 rotationMatrix = transform.rotation.getRotationMatrix();
    Matrix4 scaleMatrix = Matrix4::scaleMatrix(transform.scale);

    transform.localMatrix = translateMatrix * rotationMatrix * scaleMatrix;

    if (transform.parent != NULL_ENTITY){
        Transform* transformParent = scene->findComponent<Transform>(transform.parent);
        if (transformParent){
            updateParticleTargetTransform(*transformParent);

            transform.modelMatrix = transformParent->modelMatrix * transform.localMatrix;
            transform.worldPosition = transformParent->modelMatrix * transform.position;
            transform.worldScale = transformParent->worldScale * transform.scale;
            transform.worldRotation = transformParent->worldRotation * transform.rotation;
            return;
        }
    }

    transform.modelMatrix = transform.localMatrix;
    transform.worldPosition = transform.position;
    transform.worldScale = transform.scale;
    transform.worldRotation = transform.rotation;
}

Vector3 ActionSystem::getParticleSimulationPosition(ParticleSystemComponent& particles, Transform* targetTransform, Vector3 position){
    if (isParticleWorldSpace(particles, targetTransform))
        return targetTransform->modelMatrix * position;

    return position;
}

Vector3 ActionSystem::getParticleSimulationDirection(ParticleSystemComponent& particles, Transform* targetTransform, Vector3 direction){
    if (isParticleWorldSpace(particles, targetTransform))
        return targetTransform->worldRotation * direction;

    return direction;
}

Quaternion ActionSystem::getParticleSimulationRotation(ParticleSystemComponent& particles, Transform* targetTransform, Quaternion rotation){
    if (isParticleWorldSpace(particles, targetTransform))
        return targetTransform->worldRotation * rotation;

    return rotation;
}

Vector3 ActionSystem::getParticleDisplayPosition(ParticleSystemComponent& particles, Transform* targetTransform, Vector3 position){
    if (isParticleWorldSpace(particles, targetTransform))
        return particles.cachedTargetModelInv * position;

    return position;
}

Quaternion ActionSystem::getParticleDisplayRotation(ParticleSystemComponent& particles, Transform* targetTransform, Quaternion rotation){
    if (isParticleWorldSpace(particles, targetTransform))
        return particles.cachedTargetWorldRotInv * rotation;

    return rotation;
}

Vector3 ActionSystem::getParticleDisplayScale(ParticleSystemComponent& particles, Transform* targetTransform, Vector3 scale){
    if (isParticleWorldSpace(particles, targetTransform))
        return Vector3(
            scale.x * particles.cachedTargetInvScale.x,
            scale.y * particles.cachedTargetInvScale.y,
            scale.z * particles.cachedTargetInvScale.z);

    return scale;
}

void ActionSystem::updateParticleTargetCache(ParticleSystemComponent& particles, Transform* targetTransform){
    if (!isParticleWorldSpace(particles, targetTransform))
        return;

    updateParticleTargetTransform(*targetTransform);

    particles.cachedTargetModelInv = targetTransform->modelMatrix.inverse();
    particles.cachedTargetWorldRotInv = targetTransform->worldRotation.inverse();
    particles.cachedTargetInvScale = Vector3(
        getParticleInverseScale(targetTransform->worldScale.x),
        getParticleInverseScale(targetTransform->worldScale.y),
        getParticleInverseScale(targetTransform->worldScale.z));
}

Vector3 ActionSystem::samplePositionInitializer(ParticlePositionInitializer& init){
    auto frand = [](){ return (float)rand() / (float)RAND_MAX; };
    const float twoPi = 6.28318530717958647692f;
    const float degToRad = 0.01745329251994329577f;
    float outerRadius = std::max(0.0f, init.radius);
    float innerRadius = std::min(std::max(0.0f, init.innerRadius), outerRadius);

    auto sampleVolumeRadius = [&](){
        float inner3 = innerRadius * innerRadius * innerRadius;
        float outer3 = outerRadius * outerRadius * outerRadius;
        return std::pow(inner3 + frand() * (outer3 - inner3), 1.0f / 3.0f);
    };

    auto sampleDiscRadius = [&](){
        float inner2 = innerRadius * innerRadius;
        float outer2 = outerRadius * outerRadius;
        return std::sqrt(inner2 + frand() * (outer2 - inner2));
    };

    switch (init.shape){
        case ParticleEmitterShape::Sphere:{
            float u = frand() * 2.0f - 1.0f;
            float theta = frand() * twoPi;
            float r = sampleVolumeRadius();
            float s = std::sqrt(std::max(0.0f, 1.0f - u*u));
            return Vector3(r * s * std::cos(theta), r * u, r * s * std::sin(theta));
        }
        case ParticleEmitterShape::Hemisphere:{
            float u = frand();
            float theta = frand() * twoPi;
            float r = sampleVolumeRadius();
            float s = std::sqrt(std::max(0.0f, 1.0f - u*u));
            return Vector3(r * s * std::cos(theta), r * u, r * s * std::sin(theta));
        }
        case ParticleEmitterShape::Circle:{
            float theta = frand() * twoPi;
            float r = sampleDiscRadius();
            return Vector3(r * std::cos(theta), 0.0f, r * std::sin(theta));
        }
        case ParticleEmitterShape::Cone:{
            float coneHeight = std::max(0.0f, init.coneHeight);
            float coneAngle = std::min(std::max(0.0f, init.coneAngle), 89.0f);
            float h = coneHeight * std::pow(frand(), 1.0f / 3.0f);
            float angleRad = coneAngle * degToRad;
            float maxR = h * std::tan(angleRad);
            float theta = frand() * twoPi;
            float r = maxR * std::sqrt(frand());
            return Vector3(r * std::cos(theta), h, r * std::sin(theta));
        }
        case ParticleEmitterShape::Box:
        default:
            return getVector3InitializerValue(init.minPosition, init.maxPosition, false);
    }
}

int ActionSystem::sampleBurstCount(ParticleBurst& burst){
    int minCount = std::max(0, burst.minCount);
    int maxCount = std::max(minCount, burst.maxCount);
    if (maxCount <= minCount)
        return minCount;
    int span = maxCount - minCount + 1;
    return minCount + (int)((float)span * (float)rand() / (float)(RAND_MAX + 1.0));
}

void ActionSystem::sortParticleBursts(ParticleSystemComponent& particles){
    std::sort(particles.bursts.begin(), particles.bursts.end(), [](const ParticleBurst& a, const ParticleBurst& b){
        return a.time < b.time;
    });
}

void ActionSystem::sortParticleColorGradient(ParticleSystemComponent& particles){
    particles.colorGradient.normalize();
}

Vector3 ActionSystem::sampleParticleColorGradient(const ParticleColorGradient& gradient, float time, float life){
    const std::vector<ParticleColorGradientStop>& stops = gradient.stops;
    if (stops.empty()){
        return Vector3(1,1,1);
    }

    float normalizedTime = (life > 0.0f) ? (time / life) : 0.0f;
    if (normalizedTime < 0.0f) normalizedTime = 0.0f;
    if (normalizedTime > 1.0f) normalizedTime = 1.0f;

    const ParticleColorGradientStop* previousStop = nullptr;
    const ParticleColorGradientStop* nextStop = nullptr;
    float previousTime = 0.0f;
    float nextTime = 0.0f;

    for (const ParticleColorGradientStop& stop : stops){
        float stopTime = stop.time;
        if (stopTime < 0.0f) stopTime = 0.0f;
        if (stopTime > 1.0f) stopTime = 1.0f;

        if (stopTime <= normalizedTime && (!previousStop || stopTime >= previousTime)){
            previousStop = &stop;
            previousTime = stopTime;
        }
        if (stopTime >= normalizedTime && (!nextStop || stopTime <= nextTime)){
            nextStop = &stop;
            nextTime = stopTime;
        }
    }

    Vector3 color;
    if (!previousStop){
        color = nextStop ? nextStop->color : stops.front().color;
    } else if (!nextStop){
        color = previousStop->color;
    } else {
        float span = nextTime - previousTime;
        float value = (span > 0.0f) ? (normalizedTime - previousTime) / span : 0.0f;
        color = previousStop->color + (nextStop->color - previousStop->color) * value;
    }
    if (gradient.useSRGB) color = Color::sRGBToLinear(color);
    return color;
}

float ActionSystem::getParticleCycleDuration(ParticleSystemComponent& particles){
    float cycleDuration = 0.0f;
    if (particles.rate > 0){
        cycleDuration = std::max(cycleDuration, (float)particles.maxParticles / (float)particles.rate);
    }
    for (ParticleBurst& burst : particles.bursts){
        cycleDuration = std::max(cycleDuration, burst.time);
    }
    if (cycleDuration <= 0.0f && !particles.bursts.empty()){
        cycleDuration = std::max(0.0001f, particles.lifeInitializer.maxLife);
    }
    return cycleDuration;
}

void ActionSystem::syncParticleInstance(size_t idx, ParticleSystemComponent& particles, InstancedMeshComponent& instmesh, Transform* targetTransform){
    ParticleData& particle = particles.particles[idx];
    InstanceData& instance = instmesh.instances[idx];

    instance.position = getParticleDisplayPosition(particles, targetTransform, particle.position);
    instance.rotation = getParticleDisplayRotation(particles, targetTransform, particle.rotation);
    instance.scale = getParticleDisplayScale(particles, targetTransform, particle.scale);
}

void ActionSystem::syncParticlePoint(size_t idx, ParticleSystemComponent& particles, PointsComponent& points, Transform* targetTransform){
    points.points[idx].position = getParticleDisplayPosition(particles, targetTransform, particles.particles[idx].position);
}


ActionSystem::ActionSystem(Scene* scene): SubSystem(scene){
    signature.set(scene->getComponentId<ActionComponent>());
}

void ActionSystem::actionStart(Entity entity, bool newRun){
    ActionComponent& action = scene->getComponent<ActionComponent>(entity);
    Signature signature = scene->getSignature(entity);

    // a paused action resumes where it was, a new run starts from the target's current pose
    if (action.state == ActionState::Stopped && newRun){
        if (AnimationComponent* animcomp = scene->findComponent<AnimationComponent>(entity)){
            for (ActionFrame& frame : animcomp->actions){
                frame.started = false;
            }
        }

        KeyframeTracksComponent* keyframe = scene->findComponent<KeyframeTracksComponent>(entity);
        if (keyframe && keyframe->relative){
            keyframe->hasBase = getRelativeTrackPose(action.target, keyframe->basePosition, keyframe->baseRotation, keyframe->baseScale);
        }
    }

    actionComponentStart(action);

    // onStart is user code: it may destroy the entity or add components, which moves
    // component arrays, so nothing found before it is used after it
    ActionComponent* started = scene->isEntityCreated(entity) ? scene->findComponent<ActionComponent>(entity) : nullptr;
    if (!started){
        return;
    }
    signature = scene->getSignature(entity);
    const Entity target = started->target;

    // Reset blend weight to full when an animation starts without a pending fade
    // (a fade-in sets weight/fadeSpeed before triggering start, so preserve those).
    if (signature.test(scene->getComponentId<AnimationComponent>())){
        AnimationComponent& animcomp = scene->getComponent<AnimationComponent>(entity);
        if (animcomp.fadeSpeed <= 0.0f){
            animcomp.weight = 1.0f;
            animcomp.fadeTarget = 1.0f;
            animcomp.stopOnFadeOut = false;
        }
    }

    // a version 2 Particle System draws on its target or, without one, on its own GameObject
    if (ParticleSystemComponent* ps = scene->findComponent<ParticleSystemComponent>(entity); ps && ps->version >= 2){
        particleSystemStart(entity, *ps);
        return;
    }

    if (target != NULL_ENTITY && scene->isEntityCreated(target)){
        Signature targetSignature = scene->getSignature(target);

        if (signature.test(scene->getComponentId<SpriteAnimationComponent>())){
            SpriteAnimationComponent& spriteanim = scene->getComponent<SpriteAnimationComponent>(entity);
            if (targetSignature.test(scene->getComponentId<SpriteComponent>()) && targetSignature.test(scene->getComponentId<MeshComponent>())){
                SpriteComponent& sprite = scene->getComponent<SpriteComponent>(target);
                MeshComponent& mesh = scene->getComponent<MeshComponent>(target);

                spriteActionStart(mesh, sprite, spriteanim);

            }
        }

        if (signature.test(scene->getComponentId<ParticleSystemComponent>())){
            ParticleSystemComponent& particles = scene->getComponent<ParticleSystemComponent>(entity);
            if (targetSignature.test(scene->getComponentId<MeshComponent>())) {
                MeshComponent& mesh = scene->getComponent<MeshComponent>(target);

                scene->getSystem<MeshSystem>()->createInstancedMesh(target);
                targetSignature = scene->getSignature(target);

                InstancedMeshComponent& instmesh = scene->getComponent<InstancedMeshComponent>(target);

                particleActionStart(particles, instmesh, mesh);
            }
            if (targetSignature.test(scene->getComponentId<PointsComponent>()) ){
                PointsComponent& points = scene->getComponent<PointsComponent>(target);

                particleActionStart(particles, points);

            }
        }
    }
}

void ActionSystem::actionComponentStart(ActionComponent& action){
    // Dispatch from a copy, as actionComponentStop does: a callback that adds components
    // moves the array holding the original subscription list while call() iterates it.
    auto onStart = action.onStart;

    action.state = ActionState::Running;
    action.startTrigger = false;

    onStart.call();
}

void ActionSystem::actionStop(Entity entity){
    if (entity == NULL_ENTITY || !scene->isEntityCreated(entity)){
        return;
    }

    ActionComponent* action = scene->findComponent<ActionComponent>(entity);
    if (!action){
        return;
    }

    // Snapshot everything needed after onStop before invoking user code. A callback
    // may clear animation frames, remove components, or destroy this entity.
    Entity target = action->target;
    std::vector<Entity> animationChildren;
    if (AnimationComponent* animcomp = scene->findComponent<AnimationComponent>(entity)){
        animationChildren = animationChildEntities(*animcomp);

        // Reset runtime fade state before callbacks. A callback may deliberately
        // start/fade the animation again, in which case its newer state is kept.
        animcomp->weight = 0.0f;
        animcomp->fadeTarget = 0.0f;
        animcomp->fadeSpeed = 0.0f;
        animcomp->stopOnFadeOut = false;
    }

    // a version 2 Particle System stops and clears first: onStop sees it stopped
    if (isParticleSystemV2(entity)){
        particleSystemHardStop(entity);
        action = scene->findComponent<ActionComponent>(entity);
        if (!action){
            return;
        }
    }

    actionComponentStop(*action);
    animationStopChildren(animationChildren);

    if (!scene->isEntityCreated(entity)){
        return;
    }

    if (TweenComponent* tween = scene->findComponent<TweenComponent>(entity)){
        tweenStop(*tween);
    }

    if (target != NULL_ENTITY && scene->isEntityCreated(target)){
        SpriteAnimationComponent* spriteanim = scene->findComponent<SpriteAnimationComponent>(entity);
        SpriteComponent* sprite = scene->findComponent<SpriteComponent>(target);
        MeshComponent* mesh = scene->findComponent<MeshComponent>(target);

        if (spriteanim && sprite && mesh){
            spriteActionStop(*mesh, *sprite, *spriteanim);
        }
    }
}

void ActionSystem::actionComponentStop(ActionComponent& action){
    action.state = ActionState::Stopped;
    action.stopTrigger = false;
    action.timecount = 0;

    // not from a copy, which would still call the scripts unsubscribed meanwhile: call()
    // keeps its subscriber list even when a callback destroys the component
    action.onStop.call();
}

void ActionSystem::actionPause(Entity entity){
    if (entity == NULL_ENTITY || !scene->isEntityCreated(entity)){
        return;
    }

    ActionComponent* action = scene->findComponent<ActionComponent>(entity);
    if (!action){
        return;
    }

    // Snapshot child tracks before onPause callbacks, mirroring actionStop.
    std::vector<Entity> animationChildren;
    if (AnimationComponent* animcomp = scene->findComponent<AnimationComponent>(entity)){
        animationChildren = animationChildEntities(*animcomp);
    }

    if (ParticleSystemComponent* ps = scene->findComponent<ParticleSystemComponent>(entity); ps && ps->version >= 2){
        particles::pause(*ps);
    }

    actionComponentPause(*action);
    animationPauseChildren(animationChildren);
}

void ActionSystem::actionComponentPause(ActionComponent& action){
    action.state = ActionState::Paused;
    action.pauseTrigger = false;

    action.onPause.call();
}

void ActionSystem::actionUpdate(double dt, ActionComponent& action){
    action.timecount += dt * action.speed;
    if (action.timecount < 0){
        // a negative speed plays it back to the start
        action.timecount = 0;
    }

    action.onStep.call();
}

void ActionSystem::animationUpdate(double dt, Entity entity, ActionComponent& action, AnimationComponent& animcomp){
    int totalActionsPassed = 0;
    float actionsEnd = 0; // last frame end, used to wrap a loop

    // Advance the blend weight toward its fade target. Child actions inherit this
    // weight so their sampled poses are combined by flushPoseBlend() into a smooth
    // weighted average, producing crossfades between overlapping animations.
    if (animcomp.fadeSpeed > 0.0f){
        float step = (float)(animcomp.fadeSpeed * dt);
        if (animcomp.weight < animcomp.fadeTarget){
            animcomp.weight = std::min(animcomp.fadeTarget, animcomp.weight + step);
        }else if (animcomp.weight > animcomp.fadeTarget){
            animcomp.weight = std::max(animcomp.fadeTarget, animcomp.weight - step);
        }

        if (animcomp.weight == animcomp.fadeTarget){
            animcomp.fadeSpeed = 0.0f; // fade finished
        }

        if (animcomp.weight <= 0.0f && animcomp.stopOnFadeOut){
            actionStop(entity);
            return;
        }
    }

    // A child's onStart is user code that can add components, which moves component arrays:
    // the animation's own components are found again after each start
    ActionComponent* parent = &action;
    AnimationComponent* anim = &animcomp;

    for (size_t i = 0; i < anim->actions.size(); i++){

        float timeDiff = parent->timecount - anim->actions[i].startTime;
        Entity child = anim->actions[i].action;

        Signature isignature = scene->getSignature(child);

        if (isignature.test(scene->getComponentId<ActionComponent>())){

            if (timeDiff >= 0) {
                //TODO: Support loop actions
                // an action that ended starts again every frame to hold its last value, and
                // again on each loop: only its first start in this run of the animation is a
                // new run (a relative track keeps the pose it started from)
                bool newRun = !anim->actions[i].started;
                anim->actions[i].started = true;
                if (scene->getComponent<ActionComponent>(child).state != ActionState::Running) {
                    actionStart(child, newRun);

                    parent = scene->isEntityCreated(entity) ? scene->findComponent<ActionComponent>(entity) : nullptr;
                    anim = parent ? scene->findComponent<AnimationComponent>(entity) : nullptr;
                    if (!anim || i >= anim->actions.size()){
                        return;
                    }
                }

                ActionComponent* iaction = scene->isEntityCreated(child) ? scene->findComponent<ActionComponent>(child) : nullptr;
                if (!iaction){
                    continue;
                }

                iaction->timecount = timeDiff * iaction->speed;
                iaction->weight = anim->weight;

                float frameDuration = getFrameDuration(anim->actions[i]) / iaction->speed;
                actionsEnd = std::max(actionsEnd, anim->actions[i].startTime + frameDuration);

                if (timeDiff > frameDuration) {
                    totalActionsPassed++;
                }
            }

        }

    }

    bool actionsPassed = totalActionsPassed == (int)anim->actions.size();
    // timecount already runs at the action's speed
    bool durationPassed = anim->duration >= 0 && parent->timecount >= anim->duration;

    if (actionsPassed || durationPassed) {
        if (!anim->loop) {
            animationApplyEnd(*parent, *anim, durationPassed);
            actionStop(entity);
        }else{
            float loopDuration = durationPassed ? anim->duration : actionsEnd;

            // keep the overshoot, or the cycle gets longer at lower frame rates
            if (loopDuration > 0 && parent->timecount >= loopDuration){
                parent->timecount = std::fmod(parent->timecount, loopDuration);
            }else{
                parent->timecount = 0;
            }
        }
    }
}

// A clip that ends in this update stops its tracks before the all-actions pass samples them
// at the end, so the last keys would be missed by up to a frame (at low frame rates, visibly).
// Sample every running keyframe track once at the clip's end: the pose blend of this update
// then writes the final values. A track is sampled at the time animationUpdate gives it on
// every other frame (an explicit frame duration does not cut it there either), so a frame
// shorter than its keys does not jump back to its frame's end on the last update.
void ActionSystem::animationApplyEnd(ActionComponent& action, AnimationComponent& animcomp, bool durationPassed){
    float endTime = action.timecount;
    if (durationPassed){
        endTime = std::min(endTime, animcomp.duration / action.speed);
    }
    // the tracks' onStop callbacks are user code that may edit the frames
    std::vector<ActionFrame> frames = animcomp.actions;

    for (const ActionFrame& frame : frames){
        Entity child = frame.action;
        if (child == NULL_ENTITY || !scene->isEntityCreated(child)){
            continue;
        }
        ActionComponent* childAction = scene->findComponent<ActionComponent>(child);
        if (!childAction || childAction->state != ActionState::Running || !scene->findComponent<KeyframeTracksComponent>(child)){
            continue;
        }
        float timeDiff = endTime - frame.startTime;
        if (timeDiff < 0 || childAction->speed <= 0){
            continue;
        }
        childAction->timecount = timeDiff * childAction->speed;
        processRunningAction(0, child, *childAction);
    }
}

std::vector<Entity> ActionSystem::animationChildEntities(const AnimationComponent& animcomp){
    std::vector<Entity> children;
    children.reserve(animcomp.actions.size());
    for (const ActionFrame& frame : animcomp.actions){
        children.push_back(frame.action);
    }
    return children;
}

void ActionSystem::animationStopChildren(const std::vector<Entity>& children){
    // Child tracks are regular actions and are also visited by the system's
    // all-actions pass. The entity list is a pre-callback snapshot, so callbacks
    // may safely mutate or clear the parent's action frames while children stop.
    for (Entity child : children){
        if (child == NULL_ENTITY || !scene->isEntityCreated(child)){
            continue;
        }

        ActionComponent* childAction = scene->findComponent<ActionComponent>(child);
        if (!childAction){
            continue;
        }

        if (childAction->state == ActionState::Running || childAction->state == ActionState::Paused){
            actionStop(child);
        }
    }
}

void ActionSystem::animationPauseChildren(const std::vector<Entity>& children){
    // Child tracks advance on their own in the all-actions pass, so a paused clip
    // would keep animating unless its children pause with it. animationUpdate
    // restarts them when the parent resumes.
    for (Entity child : children){
        if (child == NULL_ENTITY || !scene->isEntityCreated(child)){
            continue;
        }

        ActionComponent* childAction = scene->findComponent<ActionComponent>(child);
        if (!childAction){
            continue;
        }

        if (childAction->state == ActionState::Running){
            actionPause(child);
        }
    }
}

void ActionSystem::setSpriteTextureRect(MeshComponent& mesh, SpriteComponent& sprite, SpriteAnimationComponent& spriteanim){
    if (mesh.numSubmeshes <= 0 || !spriteanim.frames.validIndex(spriteanim.frameIndex)) {
        return;
    }

    int frameId = spriteanim.frames[spriteanim.frameIndex];
    if (frameId < 0 || (unsigned int)frameId >= sprite.numFramesRect) {
        return;
    }

    MeshSystem::setSpriteFrameRect(mesh, sprite, sprite.framesRect[frameId].rect);
}

void ActionSystem::spriteActionStart(MeshComponent& mesh, SpriteComponent& sprite, SpriteAnimationComponent& spriteanim){
    spriteanim.frameIndex = 0;
    spriteanim.frameTimeIndex = 0;
    spriteanim.spriteFrameCount = 0;

    setSpriteTextureRect(mesh, sprite, spriteanim);
}

void ActionSystem::spriteActionStop(MeshComponent& mesh, SpriteComponent& sprite, SpriteAnimationComponent& spriteanim){
    spriteanim.frameTimeIndex = 0;
    spriteanim.spriteFrameCount = 0;

    setSpriteTextureRect(mesh, sprite, spriteanim);
}

void ActionSystem::spriteActionUpdate(double dt, Entity entity, ActionComponent& action, MeshComponent& mesh, SpriteComponent& sprite, SpriteAnimationComponent& spriteanim){
    if (spriteanim.framesSize == 0 || spriteanim.framesTimeSize == 0){
        return;
    }
    // the frame lists can shrink while it runs
    if (spriteanim.frameTimeIndex < 0 || spriteanim.frameTimeIndex >= (int)spriteanim.framesTimeSize){
        spriteanim.frameTimeIndex = 0;
    }

    // frames run at the action's speed, as its timecount does (a negative speed holds the frame)
    double step = dt * 1000 * action.speed;
    if (step > 0){
        spriteanim.spriteFrameCount += (unsigned int)step;
    }

    while (true) {
        // a negative time counts as zero
        int frameTime = std::max(0, (int)spriteanim.framesTime[spriteanim.frameTimeIndex]);
        if (spriteanim.spriteFrameCount < (unsigned int)frameTime){
            break;
        }

        // a non-looping animation ends once its last frame had its time, and keeps showing it
        // (checked before the advance, so a single frame animation also stops)
        if (!spriteanim.loop && spriteanim.frameIndex >= (int)spriteanim.framesSize - 1) {
            actionStop(entity);
            return;
        }

        spriteanim.spriteFrameCount -= frameTime;

        spriteanim.frameIndex++;
        spriteanim.frameTimeIndex++;

        if (spriteanim.frameIndex >= (int)spriteanim.framesSize){
            spriteanim.frameIndex = 0;
            if (spriteanim.loop){
                action.timecount = 0;
            }
        }

        if (spriteanim.frameTimeIndex >= (int)spriteanim.framesTimeSize)
            spriteanim.frameTimeIndex = 0;

        setSpriteTextureRect(mesh, sprite, spriteanim);

        // zero time frames advance once per update
        if (frameTime == 0)
            break;
    }
}

void ActionSystem::tweenStop(TweenComponent& tween){
    tween.time = 0;
    tween.value = 0;
}

bool ActionSystem::tweenUpdate(double dt, Entity entity, ActionComponent& action, TweenComponent& tween){
    // timecount already runs at the action's speed
    float duration = tween.duration;
    // ping-pong comes back to time 0 in twice the duration
    float period = tween.pingPong ? duration * 2 : duration;
    bool finished = false;

    if (duration > 0) {

        if (action.timecount >= period){
            if (!tween.loop){
                action.timecount = period;
                finished = true;
            }else{
                // a step can be longer than a period
                action.timecount = std::fmod(action.timecount, period);
            }
        }else if (action.timecount < 0){
            // a negative speed plays it back to the start
            action.timecount = 0;
        }

        tween.time = action.timecount / duration;
        if (tween.pingPong && tween.time > 1){
            tween.time = 2 - tween.time;
        }
    } else {
        tween.time = tween.pingPong ? 0 : 1;
        finished = !tween.loop;
    }

    tween.value = tween.function.call(tween.time);

    return finished;
}

void ActionSystem::positionTweenUpdate(TweenComponent& tween, PositionTweenComponent& posaction, Transform& transform){
    Vector3 position = (posaction.endPosition - posaction.startPosition) * tween.value;
    transform.position = posaction.startPosition + position;
    transform.needUpdate = true;
}

void ActionSystem::rotationTweenUpdate(TweenComponent& tween, RotationTweenComponent& rotaction, Transform& transform){
    if (rotaction.spin){
        // a slerp between two rotations never turns more than half a turn
        Vector3 axis = rotaction.axis;
        if (axis.length() > 0){
            axis.normalize();
            transform.rotation = rotaction.startRotation * Quaternion(Angle::degToDefault(rotaction.angle * tween.value), axis);
        }
    }else{
        transform.rotation = Quaternion::slerp(tween.value, rotaction.startRotation, rotaction.endRotation, rotaction.shortestPath);
    }
    transform.needUpdate = true;
}

void ActionSystem::scaleTweenUpdate(TweenComponent& tween, ScaleTweenComponent& scaletween, Transform& transform){
    Vector3 scale = (scaletween.endScale - scaletween.startScale) * tween.value;
    transform.scale = scaletween.startScale + scale;
    transform.needUpdate = true;
}

void ActionSystem::colorTweenUpdate(TweenComponent& tween, ColorTweenComponent& colortween, Vector4& targetColor){
    Vector3 color = (colortween.endColor - colortween.startColor) * tween.value;
    if (colortween.useSRGB){
        targetColor = Color::sRGBToLinear(colortween.startColor + color);
    }else{
        targetColor = colortween.startColor + color;
    }
}

void ActionSystem::alphaTweenUpdate(TweenComponent& tween, AlphaTweenComponent& alphatween, Vector4& targetColor){
    float alpha = (alphatween.endAlpha - alphatween.startAlpha) * tween.value;

    targetColor.w = alphatween.startAlpha + alpha;
}

int ActionSystem::findUnusedParticle(ParticleSystemComponent& particles){

    for (int i=particles.lastUsedParticle; i<particles.particles.size(); i++){
        if (particles.particles[i].life <= particles.particles[i].time){
            particles.lastUsedParticle = i;
            return i;
        }
    }

    if (particles.loop){
        for (int i=0; i<particles.lastUsedParticle; i++){
            if (particles.particles[i].life <= particles.particles[i].time){
                particles.lastUsedParticle = i;
                return i;
            }
        }
    }

    return -1;
}

float ActionSystem::getFloatInitializerValue(float& min, float& max){
    if (min != max) {
        return min + ((max - min) * (float) rand() / (float) RAND_MAX);
    }
    return max;
}

Vector3 ActionSystem::getVector3InitializerValue(Vector3& min, Vector3& max, bool linearSort){
    if (min != max) {
        if (!linearSort){
            return Vector3( min.x + ((max.x - min.x) * (float) rand() / (float) RAND_MAX),
                            min.y + ((max.y - min.y) * (float) rand() / (float) RAND_MAX),
                            min.z + ((max.z - min.z) * (float) rand() / (float) RAND_MAX));
        }else{
            float factor = (float) rand() / (float) RAND_MAX;

            return Vector3( min.x + ((max.x - min.x) * factor),
                            min.y + ((max.y - min.y) * factor),
                            min.z + ((max.z - min.z) * factor));
        }
    }
    return max;
}

Quaternion ActionSystem::getQuaternionInitializerValue(Quaternion& min, Quaternion& max, bool shortestPath){
    if (min != max) {
        return Quaternion::slerp((float) rand() / (float) RAND_MAX, min, max, shortestPath);
    }
    return max;
}

Rect ActionSystem::getSpriteInitializerValue(std::vector<int>& frames, SpriteComponent& sprite){
    if (frames.size() > 0){
        int id = frames[int(frames.size()*rand()/(RAND_MAX + 1.0))];

        if (sprite.framesRect.validIndex(id) && (unsigned int)id < sprite.numFramesRect){
            return sprite.framesRect[id].rect;
        }
    }

    return Rect(0,0,1,1);
}

Rect ActionSystem::getSpriteInitializerValue(std::vector<int>& frames, PointsComponent& points){
    if (frames.size() > 0){
        int id = frames[int(frames.size()*rand()/(RAND_MAX + 1.0))];

        if (points.framesRect.validIndex(id) && (unsigned int)id < points.numFramesRect){
            return points.framesRect[id].rect;
        }
    }

    return Rect(0,0,1,1);
}

void ActionSystem::applyParticleInitializers(size_t idx, ParticleSystemComponent& particles, InstancedMeshComponent& instmesh, SpriteComponent* sprite, Transform* targetTransform){
    ParticleLifeInitializer& lifeInit = particles.lifeInitializer;
    particles.particles[idx].life = getFloatInitializerValue(lifeInit.minLife, lifeInit.maxLife);

    ParticlePositionInitializer& posInit = particles.positionInitializer;
    Vector3 position = samplePositionInitializer(posInit);
    particles.particles[idx].position = getParticleSimulationPosition(particles, targetTransform, position);

    ParticleVelocityInitializer& velInit = particles.velocityInitializer;
    Vector3 velocity = getVector3InitializerValue(velInit.minVelocity, velInit.maxVelocity, false);
    particles.particles[idx].velocity = getParticleSimulationDirection(particles, targetTransform, velocity);

    ParticleAccelerationInitializer& accInit = particles.accelerationInitializer;
    Vector3 acceleration = getVector3InitializerValue(accInit.minAcceleration, accInit.maxAcceleration, false);
    particles.particles[idx].acceleration = getParticleSimulationDirection(particles, targetTransform, acceleration);

    ParticleColorInitializer& colInit = particles.colorInitializer;
    instmesh.instances[idx].color = getVector3InitializerValue(colInit.minColor, colInit.maxColor, false);
    if (colInit.useSRGB){
        instmesh.instances[idx].color = Color::sRGBToLinear(instmesh.instances[idx].color);
    }

    ParticleAlphaInitializer& alpInit = particles.alphaInitializer;
    instmesh.instances[idx].color.w = getFloatInitializerValue(alpInit.minAlpha, alpInit.maxAlpha);

    // size initializer is not applicable to instanced meshes

    if (sprite){
        ParticleSpriteInitializer& spriteInit = particles.spriteInitializer;
        instmesh.instances[idx].textureRect = getSpriteInitializerValue(spriteInit.frames, *sprite);
    }

    ParticleRotationInitializer& rotInit = particles.rotationInitializer;
    Quaternion rotation = getQuaternionInitializerValue(rotInit.minRotation, rotInit.maxRotation, rotInit.shortestPath);
    particles.particles[idx].rotation = getParticleSimulationRotation(particles, targetTransform, rotation);

    ParticleScaleInitializer& scaInit = particles.scaleInitializer;
    particles.particles[idx].scale = getVector3InitializerValue(scaInit.minScale, scaInit.maxScale, scaInit.linearSort);

    syncParticleInstance(idx, particles, instmesh, targetTransform);

}

void ActionSystem::applyParticleInitializers(size_t idx, ParticleSystemComponent& particles, PointsComponent& points, Transform* targetTransform){
    ParticleLifeInitializer& lifeInit = particles.lifeInitializer;
    particles.particles[idx].life = getFloatInitializerValue(lifeInit.minLife, lifeInit.maxLife);

    ParticlePositionInitializer& posInit = particles.positionInitializer;
    Vector3 position = samplePositionInitializer(posInit);
    particles.particles[idx].position = getParticleSimulationPosition(particles, targetTransform, position);

    ParticleVelocityInitializer& velInit = particles.velocityInitializer;
    Vector3 velocity = getVector3InitializerValue(velInit.minVelocity, velInit.maxVelocity, false);
    particles.particles[idx].velocity = getParticleSimulationDirection(particles, targetTransform, velocity);

    ParticleAccelerationInitializer& accInit = particles.accelerationInitializer;
    Vector3 acceleration = getVector3InitializerValue(accInit.minAcceleration, accInit.maxAcceleration, false);
    particles.particles[idx].acceleration = getParticleSimulationDirection(particles, targetTransform, acceleration);

    ParticleColorInitializer& colInit = particles.colorInitializer;
    points.points[idx].color = getVector3InitializerValue(colInit.minColor, colInit.maxColor, false);
    if (colInit.useSRGB){
        points.points[idx].color = Color::sRGBToLinear(points.points[idx].color);
    }

    ParticleAlphaInitializer& alpInit = particles.alphaInitializer;
    points.points[idx].color.w = getFloatInitializerValue(alpInit.minAlpha, alpInit.maxAlpha);

    ParticleSizeInitializer& sizeInit = particles.sizeInitializer;
    points.points[idx].size = getFloatInitializerValue(sizeInit.minSize, sizeInit.maxSize);

    ParticleSpriteInitializer& spriteInit = particles.spriteInitializer;
    points.points[idx].textureRect = getSpriteInitializerValue(spriteInit.frames, points);

    ParticleRotationInitializer& rotInit = particles.rotationInitializer;
    points.points[idx].rotation = Angle::defaultToRad(getQuaternionInitializerValue(rotInit.minRotation, rotInit.maxRotation, rotInit.shortestPath).getRoll());

    syncParticlePoint(idx, particles, points, targetTransform);

    // scale initializer is not applicable to points
}

float ActionSystem::getTimeFromParticleTime(float& time, float& fromTime, float& toTime){
    if ((fromTime != toTime) && (time >= fromTime) && (time <= toTime)) {
        return (time - fromTime) / (toTime - fromTime);
    }

    return -1;
}

bool ActionSystem::getParticleModifierValue(float& particleTime, float& fromTime, float& toTime, Ease& function, float& value){
    float time = getTimeFromParticleTime(particleTime, fromTime, toTime);
    if (time < 0 || time > 1){
        return false;
    }

    value = function.call(time);
    return value >= 0 && value <= 1;
}

float ActionSystem::getFloatModifierValue(float& value, float& fromValue, float& toValue){
    return fromValue + ((toValue - fromValue) * value);
}

Vector3 ActionSystem::getVector3ModifierValue(float& value, Vector3& fromValue, Vector3& toValue){
    return fromValue + ((toValue - fromValue) * value);
}

Quaternion ActionSystem::getQuaternionModifierValue(float& value, Quaternion& fromValue, Quaternion& toValue, bool shortestPath){
    return Quaternion::slerp(value, fromValue, toValue, shortestPath);
}

Rect ActionSystem::getSpriteModifierValue(float& value, std::vector<int>& frames, SpriteComponent& sprite){
    if (frames.size() > 0){
        // value 1 is the modifier's end: the last frame, not one past it (and an ease
        // overshooting below 0 the first: a negative float cast to size_t is undefined)
        float position = std::min(std::max(value, 0.0f), 1.0f) * (float)frames.size();
        size_t frameIndex = std::min(static_cast<size_t>(position), frames.size() - 1);
        int id = frames[frameIndex];

        if (sprite.framesRect.validIndex(id) && (unsigned int)id < sprite.numFramesRect){
            return sprite.framesRect[id].rect;
        }
    }

    return Rect(0,0,1,1);
}

Rect ActionSystem::getSpriteModifierValue(float& value, std::vector<int>& frames, PointsComponent& points){
    if (frames.size() > 0){
        // value 1 is the modifier's end: the last frame, not one past it (and an ease
        // overshooting below 0 the first: a negative float cast to size_t is undefined)
        float position = std::min(std::max(value, 0.0f), 1.0f) * (float)frames.size();
        size_t frameIndex = std::min(static_cast<size_t>(position), frames.size() - 1);
        int id = frames[frameIndex];

        if (points.framesRect.validIndex(id) && (unsigned int)id < points.numFramesRect){
            return points.framesRect[id].rect;
        }
    }

    return Rect(0,0,1,1);
}

void ActionSystem::applyParticleModifiers(size_t idx, ParticleSystemComponent& particles, InstancedMeshComponent& instmesh, SpriteComponent* sprite, Transform* targetTransform){
    float particleTime = particles.particles[idx].time;
    float value;

    ParticlePositionModifier& posMod = particles.positionModifier;
    if (getParticleModifierValue(particleTime, posMod.fromTime, posMod.toTime, posMod.function, value)){
        particles.particles[idx].position = getParticleSimulationPosition(particles, targetTransform, getVector3ModifierValue(value, posMod.fromPosition, posMod.toPosition));
    }

    ParticleVelocityModifier& velMod = particles.velocityModifier;
    if (getParticleModifierValue(particleTime, velMod.fromTime, velMod.toTime, velMod.function, value)){
        particles.particles[idx].velocity = getParticleSimulationDirection(particles, targetTransform, getVector3ModifierValue(value, velMod.fromVelocity, velMod.toVelocity));
    }

    ParticleAccelerationModifier& accMod = particles.accelerationModifier;
    if (getParticleModifierValue(particleTime, accMod.fromTime, accMod.toTime, accMod.function, value)){
        particles.particles[idx].acceleration = getParticleSimulationDirection(particles, targetTransform, getVector3ModifierValue(value, accMod.fromAcceleration, accMod.toAcceleration));
    }

    if (!particles.colorGradient.stops.empty()){
        instmesh.instances[idx].color = sampleParticleColorGradient(particles.colorGradient, particleTime, particles.particles[idx].life);
    } else {
        ParticleColorModifier& colMod = particles.colorModifier;
        if (getParticleModifierValue(particleTime, colMod.fromTime, colMod.toTime, colMod.function, value)){
            instmesh.instances[idx].color = getVector3ModifierValue(value, colMod.fromColor, colMod.toColor);
            if (colMod.useSRGB){
                instmesh.instances[idx].color = Color::sRGBToLinear(instmesh.instances[idx].color);
            }
        }
    }

    ParticleAlphaModifier& alpMod = particles.alphaModifier;
    if (getParticleModifierValue(particleTime, alpMod.fromTime, alpMod.toTime, alpMod.function, value)){
        instmesh.instances[idx].color.w = getFloatModifierValue(value, alpMod.fromAlpha, alpMod.toAlpha);
    }

    // size modifier is not applicable to instanced meshes

    if (sprite){
        ParticleSpriteModifier& spriteMod = particles.spriteModifier;
        if (getParticleModifierValue(particleTime, spriteMod.fromTime, spriteMod.toTime, spriteMod.function, value)){
            instmesh.instances[idx].textureRect = getSpriteModifierValue(value, spriteMod.frames, *sprite);
        }
    }

    ParticleRotationModifier& rotMod = particles.rotationModifier;
    if (getParticleModifierValue(particleTime, rotMod.fromTime, rotMod.toTime, rotMod.function, value)){
        particles.particles[idx].rotation = getParticleSimulationRotation(particles, targetTransform, getQuaternionModifierValue(value, rotMod.fromRotation, rotMod.toRotation, rotMod.shortestPath));
    }

    ParticleScaleModifier& scaMod = particles.scaleModifier;
    if (getParticleModifierValue(particleTime, scaMod.fromTime, scaMod.toTime, scaMod.function, value)){
        particles.particles[idx].scale = getVector3ModifierValue(value, scaMod.fromScale, scaMod.toScale);
    }

}

void ActionSystem::applyParticleModifiers(size_t idx, ParticleSystemComponent& particles, PointsComponent& points, Transform* targetTransform){
    float particleTime = particles.particles[idx].time;
    float value;

    ParticlePositionModifier& posMod = particles.positionModifier;
    if (getParticleModifierValue(particleTime, posMod.fromTime, posMod.toTime, posMod.function, value)){
        particles.particles[idx].position = getParticleSimulationPosition(particles, targetTransform, getVector3ModifierValue(value, posMod.fromPosition, posMod.toPosition));
    }

    ParticleVelocityModifier& velMod = particles.velocityModifier;
    if (getParticleModifierValue(particleTime, velMod.fromTime, velMod.toTime, velMod.function, value)){
        particles.particles[idx].velocity = getParticleSimulationDirection(particles, targetTransform, getVector3ModifierValue(value, velMod.fromVelocity, velMod.toVelocity));
    }

    ParticleAccelerationModifier& accMod = particles.accelerationModifier;
    if (getParticleModifierValue(particleTime, accMod.fromTime, accMod.toTime, accMod.function, value)){
        particles.particles[idx].acceleration = getParticleSimulationDirection(particles, targetTransform, getVector3ModifierValue(value, accMod.fromAcceleration, accMod.toAcceleration));
    }

    if (!particles.colorGradient.stops.empty()){
        points.points[idx].color = sampleParticleColorGradient(particles.colorGradient, particleTime, particles.particles[idx].life);
    } else {
        ParticleColorModifier& colMod = particles.colorModifier;
        if (getParticleModifierValue(particleTime, colMod.fromTime, colMod.toTime, colMod.function, value)){
            points.points[idx].color = getVector3ModifierValue(value, colMod.fromColor, colMod.toColor);
            if (colMod.useSRGB){
                points.points[idx].color = Color::sRGBToLinear(points.points[idx].color);
            }
        }
    }

    ParticleAlphaModifier& alpMod = particles.alphaModifier;
    if (getParticleModifierValue(particleTime, alpMod.fromTime, alpMod.toTime, alpMod.function, value)){
        points.points[idx].color.w = getFloatModifierValue(value, alpMod.fromAlpha, alpMod.toAlpha);
    }

    ParticleSizeModifier& sizeMod = particles.sizeModifier;
    if (getParticleModifierValue(particleTime, sizeMod.fromTime, sizeMod.toTime, sizeMod.function, value)){
        points.points[idx].size = getFloatModifierValue(value, sizeMod.fromSize, sizeMod.toSize);
    }

    ParticleSpriteModifier& spriteMod = particles.spriteModifier;
    if (getParticleModifierValue(particleTime, spriteMod.fromTime, spriteMod.toTime, spriteMod.function, value)){
        points.points[idx].textureRect = getSpriteModifierValue(value, spriteMod.frames, points);
    }

    ParticleRotationModifier& rotMod = particles.rotationModifier;
    if (getParticleModifierValue(particleTime, rotMod.fromTime, rotMod.toTime, rotMod.function, value)){
        points.points[idx].rotation = Angle::defaultToRad(getQuaternionModifierValue(value, rotMod.fromRotation, rotMod.toRotation, rotMod.shortestPath).getRoll());
    }

    // scale modifier is not applicable to points
}

void ActionSystem::advanceParticle(size_t idx, float dt, ParticleSystemComponent& particles, InstancedMeshComponent& instmesh, SpriteComponent* sprite, Transform* targetTransform){
    float life = particles.particles[idx].life;
    float time = particles.particles[idx].time;

    if (life <= time){
        instmesh.instances[idx].visible = false;
        return;
    }

    applyParticleModifiers(idx, particles, instmesh, sprite, targetTransform);

    Vector3 velocity = particles.particles[idx].velocity;
    Vector3 position = particles.particles[idx].position;
    Vector3 acceleration = particles.particles[idx].acceleration;

    velocity += acceleration * dt * 0.5f;
    position += velocity * dt;
    velocity += acceleration * dt * 0.5f;
    time += dt;

    particles.particles[idx].time = time;
    particles.particles[idx].velocity = velocity;
    particles.particles[idx].position = position;

    syncParticleInstance(idx, particles, instmesh, targetTransform);

    instmesh.instances[idx].visible = true;
    instmesh.needUpdateInstances = true;
}

void ActionSystem::advanceParticle(size_t idx, float dt, ParticleSystemComponent& particles, PointsComponent& points, Transform* targetTransform){
    float life = particles.particles[idx].life;
    float time = particles.particles[idx].time;

    if (life <= time){
        points.points[idx].visible = false;
        return;
    }

    applyParticleModifiers(idx, particles, points, targetTransform);

    Vector3 velocity = particles.particles[idx].velocity;
    Vector3 position = particles.particles[idx].position;
    Vector3 acceleration = particles.particles[idx].acceleration;

    velocity += acceleration * dt * 0.5f;
    position += velocity * dt;
    velocity += acceleration * dt * 0.5f;
    time += dt;

    particles.particles[idx].time = time;
    particles.particles[idx].velocity = velocity;
    particles.particles[idx].position = position;

    syncParticlePoint(idx, particles, points, targetTransform);

    points.points[idx].visible = true;
    points.needUpdate = true;
}

void ActionSystem::particleActionStart(ParticleSystemComponent& particles, InstancedMeshComponent& instmesh, MeshComponent& mesh){
    // Creating particles
    particles.particles.clear();
    instmesh.instances.clear();
    for (int i = 0; i < particles.maxParticles; i++){
        particles.particles.push_back({});
        particles.particles.back().life = 0;
        particles.particles.back().time = 0;

        instmesh.instances.push_back({});
        instmesh.instances.back().visible = false;

        instmesh.needUpdateInstances = true;
    }

    if (instmesh.maxInstances != particles.maxParticles){
        instmesh.maxInstances = particles.maxParticles;
        if (mesh.loaded)
            mesh.needReload = true;
    }

    particles.emitter = true;
    particles.newParticlesCount = 0;
    particles.lastUsedParticle = 0;
    sortParticleBursts(particles);
    sortParticleColorGradient(particles);
    particles.currentBurst = 0;
}

void ActionSystem::particleActionStart(ParticleSystemComponent& particles, PointsComponent& points){
    if (particles.sizeInitializer.minSize == 0 && particles.sizeInitializer.maxSize == 0){
        float size = std::max(points.texture.getWidth(), points.texture.getHeight());
        particles.sizeInitializer.minSize = size;
        particles.sizeInitializer.maxSize = size;
    }

    // Creating particles
    particles.particles.clear();
    points.points.clear();
    for (int i = 0; i < particles.maxParticles; i++){
        particles.particles.push_back({});
        particles.particles.back().life = 0;
        particles.particles.back().time = 0;

        points.points.push_back({});
        points.points.back().visible = false;

        points.needUpdate = true;
    }

    if (points.maxPoints != particles.maxParticles){
        points.maxPoints = particles.maxParticles;

        points.needReload = true;
    }

    particles.emitter = true;
    particles.newParticlesCount = 0;
    particles.lastUsedParticle = 0;
    sortParticleBursts(particles);
    sortParticleColorGradient(particles);
    particles.currentBurst = 0;
}

void ActionSystem::particleActionReset(Entity entity){
    ActionComponent* action = scene->findComponent<ActionComponent>(entity);
    ParticleSystemComponent* particles = scene->findComponent<ParticleSystemComponent>(entity);
    if (!action || !particles) {
        return;
    }

    action->state = ActionState::Stopped;
    action->timecount = 0.0f;
    action->startTrigger = false;
    action->stopTrigger = false;
    action->pauseTrigger = false;

    if (particles->version >= 2){
        particleSystemReset(entity);
        return;
    }

    particles->particles.clear();
    particles->newParticlesCount = 0.0f;
    particles->lastUsedParticle = 0;
    particles->currentBurst = 0;
    particles->emitter = false;

    if (action->target == NULL_ENTITY) {
        return;
    }

    if (PointsComponent* points = scene->findComponent<PointsComponent>(action->target)) {
        points->points.clear();
        points->renderPoints.clear();
        points->numVisible = 0;
        points->needUpdate = true;
        points->needUpdateBuffer = true;
    }

    if (InstancedMeshComponent* instmesh = scene->findComponent<InstancedMeshComponent>(action->target)) {
        for (InstanceData& instance : instmesh->instances) {
            instance.visible = false;
        }
        instmesh->renderInstances.clear();
        instmesh->numVisible = 0;
        instmesh->needUpdateInstances = true;
    }
}

void ActionSystem::particlesActionUpdate(double dt, Entity entity, Entity target, ActionComponent& action, ParticleSystemComponent& particles, InstancedMeshComponent& instmesh){
    SpriteComponent* sprite = scene->findComponent<SpriteComponent>(target);
    Transform* targetTransform = scene->findComponent<Transform>(target);
    updateParticleTargetCache(particles, targetTransform);

    bool existParticles = false;
    for(int i=0; i<particles.particles.size(); i++){
        if (particles.particles[i].life > particles.particles[i].time){
            advanceParticle(i, dt, particles, instmesh, sprite, targetTransform);
            existParticles = true;
        }else if (instmesh.instances[i].visible){
            // the last particles to die would otherwise stay drawn
            instmesh.instances[i].visible = false;
            instmesh.needUpdateInstances = true;
        }
    }

    if (particles.emitter){
        // Bursts (process all bursts whose scheduled time has been reached)
        while (particles.currentBurst < (int)particles.bursts.size() &&
               particles.bursts[particles.currentBurst].time <= action.timecount){
            int count = sampleBurstCount(particles.bursts[particles.currentBurst]);
            for (int b = 0; b < count; b++){
                int particleIndex = findUnusedParticle(particles);
                if (particleIndex < 0) break;
                particles.particles[particleIndex].time = 0;
                applyParticleInitializers(particleIndex, particles, instmesh, sprite, targetTransform);
                existParticles = true;
            }
            particles.currentBurst++;
        }

        particles.newParticlesCount += dt * particles.rate;

        int newparticles = (int)particles.newParticlesCount;
        particles.newParticlesCount -= newparticles;
        if (newparticles > particles.maxPerUpdate)
            newparticles = particles.maxPerUpdate;

        for(int i=0; i<newparticles; i++){
            int particleIndex = findUnusedParticle(particles);

            if (particleIndex >= 0){
                particles.particles[particleIndex].time = 0;
                applyParticleInitializers(particleIndex, particles, instmesh, sprite, targetTransform);

                float spawnDt = (float)dt * (float)(newparticles - i) / (float)(newparticles + 1);
                if (spawnDt > 0.0f){
                    advanceParticle(particleIndex, spawnDt, particles, instmesh, sprite, targetTransform);
                }

                existParticles = true;
            }else{
                if (!particles.loop)
                    particles.emitter = false;
                break;
            }
        }
    }

    if (!existParticles && !particles.emitter){
        actionStop(entity);
    } else if (particles.loop) {
        float cycleDuration = getParticleCycleDuration(particles);
        if (cycleDuration > 0.0f){
            while (action.timecount >= cycleDuration) {
                action.timecount -= cycleDuration;
                particles.currentBurst = 0;
            }
        }
    }
}

void ActionSystem::particlesActionUpdate(double dt, Entity entity, Entity target, ActionComponent& action, ParticleSystemComponent& particles, PointsComponent& points){
    Transform* targetTransform = scene->findComponent<Transform>(target);
    updateParticleTargetCache(particles, targetTransform);

    bool existParticles = false;
    for(int i=0; i<particles.particles.size(); i++){
        if (particles.particles[i].life > particles.particles[i].time){
            advanceParticle(i, dt, particles, points, targetTransform);
            existParticles = true;
        }else if (points.points[i].visible){
            // the last particles to die would otherwise stay drawn
            points.points[i].visible = false;
            points.needUpdate = true;
        }
    }

    if (particles.emitter){
        // Bursts (process all bursts whose scheduled time has been reached)
        while (particles.currentBurst < (int)particles.bursts.size() &&
               particles.bursts[particles.currentBurst].time <= action.timecount){
            int count = sampleBurstCount(particles.bursts[particles.currentBurst]);
            for (int b = 0; b < count; b++){
                int particleIndex = findUnusedParticle(particles);
                if (particleIndex < 0) break;
                particles.particles[particleIndex].time = 0;
                applyParticleInitializers(particleIndex, particles, points, targetTransform);
                existParticles = true;
            }
            particles.currentBurst++;
        }

        particles.newParticlesCount += dt * particles.rate;

        int newparticles = (int)particles.newParticlesCount;
        particles.newParticlesCount -= newparticles;
        if (newparticles > particles.maxPerUpdate)
            newparticles = particles.maxPerUpdate;

        for(int i=0; i<newparticles; i++){
            int particleIndex = findUnusedParticle(particles);

            if (particleIndex >= 0){
                particles.particles[particleIndex].time = 0;
                applyParticleInitializers(particleIndex, particles, points, targetTransform);

                float spawnDt = (float)dt * (float)(newparticles - i) / (float)(newparticles + 1);
                if (spawnDt > 0.0f){
                    advanceParticle(particleIndex, spawnDt, particles, points, targetTransform);
                }

                existParticles = true;
            }else{
                if (!particles.loop)
                    particles.emitter = false;
                break;
            }
        }
    }

    if (!existParticles && !particles.emitter){
        actionStop(entity);
    } else if (particles.loop) {
        float cycleDuration = getParticleCycleDuration(particles);
        if (cycleDuration > 0.0f){
            while (action.timecount >= cycleDuration) {
                action.timecount -= cycleDuration;
                particles.currentBurst = 0;
            }
        }
    }
}

void ActionSystem::keyframeUpdate(double dt, ActionComponent& action, KeyframeTracksComponent& keyframe){
    if (keyframe.times.size() == 0)
        return;

    // Per-segment easing: easings[i] shapes the segment from key i to key i+1.
    // Empty easings (e.g. GLTF LINEAR clips) keep pure linear interpolation;
    // GLTF STEP clips use EaseType::STEP here.
    anim::KeyLocation at = anim::locateKey(keyframe.times.data(), keyframe.times.size(),
                                           keyframe.easings.data(), keyframe.easings.size(), action.timecount);
    keyframe.index = (int)at.index;
    keyframe.interpolation = at.interpolation;
}

// The sampler location of the keyframe's current index and interpolation (keyframeUpdate,
// or the clip end that processRunningAction forces)
static anim::KeyLocation keyLocation(const KeyframeTracksComponent& keyframe){
    anim::KeyLocation at;
    at.index = (size_t)keyframe.index;
    at.interpolation = keyframe.interpolation;
    if (keyframe.index > 0 && (size_t)keyframe.index < keyframe.times.size()){
        at.segmentDuration = keyframe.times[keyframe.index] - keyframe.times[keyframe.index-1];
    }
    return at;
}

// Tangent presence is what marks a track as GLTF CUBICSPLINE; editor-authored
// and GLTF LINEAR/STEP tracks keep the tangent vectors empty.
template<typename T>
static bool hasCubicSplineTangents(const KeyframeTracksComponent& keyframe, const std::vector<T>& values, const std::vector<T>& inTangents, const std::vector<T>& outTangents){
    return keyframe.index > 0 && values.size() > 0 &&
           inTangents.size() == values.size() && outTangents.size() == values.size();
}

// keyframe.index comes from the times array, which the editor can edit out of
// sync with a track's values; a track with fewer values than times must freeze
// instead of reading out of bounds.
template<typename TracksComp>
static bool trackIndexValid(const KeyframeTracksComponent& keyframe, const TracksComp& tracks){
    return anim::keyIndexValid(keyLocation(keyframe), tracks.values.size());
}

// Sample a vector track at the current playhead: cubic Hermite when tangents
// are present, lerp otherwise.
template<typename TracksComp>
static Vector3 sampleVectorTrack(const KeyframeTracksComponent& keyframe, const TracksComp& tracks){
    bool cubic = hasCubicSplineTangents(keyframe, tracks.values, tracks.inTangents, tracks.outTangents);
    return anim::sampleVector3(keyLocation(keyframe), tracks.values.data(),
                               cubic ? tracks.inTangents.data() : nullptr, cubic ? tracks.outTangents.data() : nullptr);
}

void ActionSystem::translateTracksUpdate(KeyframeTracksComponent& keyframe, TranslateTracksComponent& translatetracks, Entity target, float weight){
    if (weight <= 0.0f || !trackIndexValid(keyframe, translatetracks))
        return;

    Vector3 value = sampleVectorTrack(keyframe, translatetracks);
    if (keyframe.relative){
        value = getRelativeTrackMatrix(target, keyframe.basePosition) * value;
    }

    TransformBlendAccum& accum = transformBlend[target];
    accum.positionSum = accum.positionSum + value * weight;
    accum.positionWeight += weight;
}

void ActionSystem::scaleTracksUpdate(KeyframeTracksComponent& keyframe, ScaleTracksComponent& scaletracks, Entity target, float weight){
    if (weight <= 0.0f || !trackIndexValid(keyframe, scaletracks))
        return;

    Vector3 value = sampleVectorTrack(keyframe, scaletracks);
    if (keyframe.relative){
        value = keyframe.baseScale * value;
    }

    TransformBlendAccum& accum = transformBlend[target];
    accum.scaleSum = accum.scaleSum + value * weight;
    accum.scaleWeight += weight;
}

void ActionSystem::rotateTracksUpdate(KeyframeTracksComponent& keyframe, RotateTracksComponent& rotatetracks, Entity target, float weight){
    if (weight <= 0.0f || !trackIndexValid(keyframe, rotatetracks))
        return;

    // slerp, or Hermite normalized with the zero-norm guard (KeyframeSampling)
    bool cubic = hasCubicSplineTangents(keyframe, rotatetracks.values, rotatetracks.inTangents, rotatetracks.outTangents);
    Quaternion value = anim::sampleQuaternion(keyLocation(keyframe), rotatetracks.values.data(),
                                              cubic ? rotatetracks.inTangents.data() : nullptr,
                                              cubic ? rotatetracks.outTangents.data() : nullptr);
    if (keyframe.relative){
        value = keyframe.baseRotation * value;
    }

    TransformBlendAccum& accum = transformBlend[target];
    // Weighted quaternion average (nlerp-style): align each sample to the first
    // sample's hemisphere so the shortest arc is taken, sum, then normalize on flush.
    if (!accum.hasRotationRef){
        accum.rotationRef = value;
        accum.hasRotationRef = true;
    } else if (accum.rotationRef.dot(value) < 0.0f){
        value = -value;
    }
    accum.rotationSum = accum.rotationSum + value * weight;
    accum.rotationWeight += weight;
}

void ActionSystem::morphTracksUpdate(KeyframeTracksComponent& keyframe, MorphTracksComponent& morpthtracks, Entity target, float weight){
    if (weight <= 0.0f || !trackIndexValid(keyframe, morpthtracks))
        return;

    const std::vector<float>& previousMorph = morpthtracks.values[(keyframe.index > 0) ? keyframe.index - 1 : 0];
    const std::vector<float>& nextMorph = morpthtracks.values[keyframe.index];

    if ((keyframe.index == 0) || (nextMorph.size() == previousMorph.size())) {
        bool cubic = hasCubicSplineTangents(keyframe, morpthtracks.values, morpthtracks.inTangents, morpthtracks.outTangents) &&
                     morpthtracks.outTangents[keyframe.index-1].size() == previousMorph.size() &&
                     morpthtracks.inTangents[keyframe.index].size() == nextMorph.size();
        size_t count = std::min(nextMorph.size(), (size_t)MAX_MORPHTARGETS);
        float values[MAX_MORPHTARGETS];
        anim::sampleFloatRow(keyLocation(keyframe), previousMorph.data(), nextMorph.data(), count,
                             cubic ? morpthtracks.outTangents[keyframe.index-1].data() : nullptr,
                             cubic ? morpthtracks.inTangents[keyframe.index].data() : nullptr, values);
        MorphBlendAccum& accum = morphBlend[target];
        for (size_t morphIndex = 0; morphIndex < count; morphIndex++) {
            accum.sums[morphIndex] += values[morphIndex] * weight;
            accum.weights[morphIndex] += weight;
        }
    }else{
        Log::error("MorphTrack of index %i is different size than index %i", keyframe.index, keyframe.index-1);
    }
}

void ActionSystem::latticeTracksUpdate(KeyframeTracksComponent& keyframe, LatticeTracksComponent& latticetracks, Entity target, LatticeComponent& latticecomp, float weight){
    if (weight <= 0.0f || !trackIndexValid(keyframe, latticetracks))
        return;

    const std::vector<Vector3>& previous = latticetracks.values[(keyframe.index > 0) ? keyframe.index - 1 : 0];
    const std::vector<Vector3>& next = latticetracks.values[keyframe.index];

    // a key with fewer values than points (hand-edited data) animates only the points both
    // keys of the segment have
    size_t count = std::min(latticetracks.points.size(), std::min(previous.size(), next.size()));
    if (count == 0)
        return;

    bool cubic = hasCubicSplineTangents(keyframe, latticetracks.values, latticetracks.inTangents, latticetracks.outTangents) &&
                 latticetracks.outTangents[keyframe.index-1].size() == previous.size() &&
                 latticetracks.inTangents[keyframe.index].size() == next.size();
    const Vector3* previousOut = cubic ? latticetracks.outTangents[keyframe.index-1].data() : nullptr;
    const Vector3* nextIn = cubic ? latticetracks.inTangents[keyframe.index].data() : nullptr;
    anim::KeyLocation at = keyLocation(keyframe);

    int pointCount = lattice::pointCount(latticecomp.resolutionX, latticecomp.resolutionY, latticecomp.resolutionZ);

    LatticeBlendAccum& accum = latticeBlend[target];
    if (!accum.active){
        accum.sums.assign(pointCount, Vector3(0, 0, 0));
        accum.weights.assign(pointCount, 0.0f);
        accum.active = true;
    }else if (accum.sums.size() < (size_t)pointCount){
        accum.sums.resize(pointCount, Vector3(0, 0, 0));
        accum.weights.resize(pointCount, 0.0f);
    }

    for (size_t n = 0; n < count; n++){
        int point = latticetracks.points[n];
        // points out of range for the target's current resolution are ignored (spec §7)
        if (point < 0 || point >= pointCount)
            continue;

        Vector3 value;
        anim::sampleVector3Row(at, &previous[n], &next[n], 1,
                               cubic ? previousOut + n : nullptr, cubic ? nextIn + n : nullptr, &value);
        accum.sums[point] += value * weight;
        accum.weights[point] += weight;
    }
}

void ActionSystem::clearPoseBlend(){
    // the nodes stay for the next frame (no allocation per animated entity and frame); the
    // entries of destroyed entities go
    for (auto it = transformBlend.begin(); it != transformBlend.end();){
        if (!scene->isEntityCreated(it->first)){
            it = transformBlend.erase(it);
        }else{
            it->second.reset();
            ++it;
        }
    }
    morphBlend.clear();
    for (auto it = propertyBlend.begin(); it != propertyBlend.end();){
        if (!scene->isEntityCreated(it->second.target)){
            it = propertyBlend.erase(it);
        }else{
            it->second.active = false;
            it->second.rest = nullptr;
            ++it;
        }
    }
    for (auto it = latticeBlend.begin(); it != latticeBlend.end();){
        if (!scene->isEntityCreated(it->first)){
            it = latticeBlend.erase(it);
        }else{
            it->second.active = false;
            if (!it->second.hasRest.empty()){
                std::fill(it->second.hasRest.begin(), it->second.hasRest.end(), 0);
            }
            ++it;
        }
    }
}

void ActionSystem::flushPoseBlend(){
    for (auto it = transformBlend.begin(); it != transformBlend.end();){
        // kept from an earlier frame, nothing this one: dropped (the nodes of what animates
        // every frame stay; the rest does not cost later frames)
        if (it->second.empty()){
            it = transformBlend.erase(it);
            continue;
        }
        Entity target = it->first;
        TransformBlendAccum& accum = it->second;
        ++it;

        // one lookup: a destroyed entity has no components
        Transform* transform = scene->findComponent<Transform>(target);
        if (!transform)
            continue;

        // an Animation Player's rest fills the weight the contributors leave (spec 9.R1)
        if (accum.hasRestPosition && accum.positionWeight > 0.0f && accum.positionWeight < 1.0f){
            accum.positionSum = accum.positionSum + accum.restPosition * (1.0f - accum.positionWeight);
            accum.positionWeight = 1.0f;
        }
        if (accum.hasRestScale && accum.scaleWeight > 0.0f && accum.scaleWeight < 1.0f){
            accum.scaleSum = accum.scaleSum + accum.restScale * (1.0f - accum.scaleWeight);
            accum.scaleWeight = 1.0f;
        }
        if (accum.hasRestRotation && accum.rotationWeight > 0.0f && accum.rotationWeight < 1.0f){
            Quaternion rest = accum.restRotation;
            if (accum.hasRotationRef && accum.rotationRef.dot(rest) < 0.0f){
                rest = -rest;
            }
            accum.rotationSum = accum.rotationSum + rest * (1.0f - accum.rotationWeight);
            accum.rotationWeight = 1.0f;
        }

        bool changed = false;
        if (accum.positionWeight > 0.0f){
            Vector3 position = accum.positionSum * (1.0f / accum.positionWeight);
            // the layout places an anchored element from its offset every frame, so the
            // move goes to the offset too or the next layout snaps it back
            RectTransformComponent* layout = scene->findComponent<RectTransformComponent>(target);
            if (layout && layout->usingAnchors){
                UISystem* uisystem = scene->getSystem<UISystem>().get();
                layout->positionOffset += uisystem->parentToLayoutSpace(target, Vector2(position.x, position.y)) -
                                          uisystem->parentToLayoutSpace(target, Vector2(transform->position.x, transform->position.y));
            }
            transform->position = position;
            changed = true;
        }
        if (accum.scaleWeight > 0.0f){
            transform->scale = accum.scaleSum * (1.0f / accum.scaleWeight);
            changed = true;
        }
        if (accum.rotationWeight > 0.0f){
            transform->rotation = accum.rotationSum.normalized();
            changed = true;
        }
        if (changed){
            transform->needUpdate = true;
        }
    }

    for (auto& entry : morphBlend){
        Entity target = entry.first;
        MorphBlendAccum& accum = entry.second;

        if (!scene->isEntityCreated(target))
            continue;

        MeshComponent* mesh = scene->findComponent<MeshComponent>(target);
        if (!mesh)
            continue;

        for (int i = 0; i < MAX_MORPHTARGETS; i++){
            if (accum.weights[i] > 0.0f){
                if (accum.hasRest[i] && accum.weights[i] < 1.0f){
                    mesh->morphWeights[i] = accum.sums[i] + accum.rest[i] * (1.0f - accum.weights[i]);
                }else{
                    mesh->morphWeights[i] = accum.sums[i] / accum.weights[i];
                }
            }
        }
    }

    for (auto& entry : latticeBlend){
        Entity target = entry.first;
        LatticeBlendAccum& accum = entry.second;

        if (!accum.active || !scene->isEntityCreated(target))
            continue;

        LatticeComponent* latticecomp = scene->findComponent<LatticeComponent>(target);
        if (!latticecomp)
            continue;

        // points no running track animates keep their offsets
        size_t pointCount = (size_t)lattice::pointCount(latticecomp->resolutionX, latticecomp->resolutionY, latticecomp->resolutionZ);
        size_t count = std::min(pointCount, accum.weights.size());
        bool changed = false;
        for (size_t i = 0; i < count; i++){
            if (accum.weights[i] > 0.0f){
                Vector3 value = (i < accum.hasRest.size() && accum.hasRest[i] && accum.weights[i] < 1.0f) ?
                                accum.sums[i] + accum.rest[i] * (1.0f - accum.weights[i]) :
                                accum.sums[i] / accum.weights[i];
                if (latticecomp->offsets.size() < pointCount){
                    latticecomp->offsets.resize(pointCount, Vector3(0, 0, 0));
                }
                if (latticecomp->offsets[i] != value){
                    latticecomp->offsets[i] = value;
                    changed = true;
                }
            }
        }
        if (changed){
            latticecomp->revision++;
        }
    }

    // properties after the transforms (anim-properties spec 5.6); the writes that run user
    // callbacks (GameObject active, a script's enabled) wait for the end of the flush, and in
    // update() for the end of its pose blend scope (an onEnable may sample a player)
    flushPropertyBlend(deferredPropertyWrites);

    clearPoseBlend();

    if (!poseBlendScope) applyDeferredPropertyWrites();
}

size_t ActionSystem::hierarchyIndex(Scene* scene, Entity entity){
    if (!scene || !scene->isEntityCreated(entity) || !scene->findComponent<Transform>(entity)) return SIZE_MAX;
    return scene->getComponentArray<Transform>()->getIndex(entity);
}

void ActionSystem::applyDeferredPropertyWrites(){
    if (deferredPropertyWrites.empty()) return;
    // taken out first: a callback sampling a player flushes (and applies) its own
    std::vector<DeferredPropertyWrite> writes;
    writes.swap(deferredPropertyWrites);
    // hierarchy order (parents first, siblings in order); every one re-checked (an onEnable /
    // onDisable before it may have destroyed GameObjects or added components: nothing is held
    // across these writes)
    std::vector<std::pair<size_t, size_t>> order;
    order.reserve(writes.size());
    for (size_t i = 0; i < writes.size(); i++) order.push_back({hierarchyIndex(scene, writes[i].target), i});
    std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b){ return a.first < b.first; });
    for (const auto& entry : order){
        const DeferredPropertyWrite& write = writes[entry.second];
        if (!scene->isEntityCreated(write.target)) continue;
        anim::writePropertyKeySpace(scene, write.target, write.binding, &write.value);
    }
}

ActionSystem::PropertyBlendAccum& ActionSystem::propertyAccum(Entity target, uint32_t key, const anim::PropertyRest* rest){
    PropertyBlendAccum& accum = propertyBlend[((uint64_t)target << 32) | key];
    if (!accum.active){
        accum.active = true;
        accum.target = target;
        accum.rest = rest;
        for (int k = 0; k < 4; k++){
            accum.sums[k] = 0.0f;
            accum.weights[k] = 0.0f;
        }
        accum.total = 0.0f;
        accum.bestWeight = -1.0f;
        accum.bestValue = 0.0f;
    }
    return accum;
}

void ActionSystem::flushPropertyBlend(std::vector<DeferredPropertyWrite>& deferred){
    for (auto& entry : propertyBlend){
        PropertyBlendAccum& accum = entry.second;
        if (!accum.active || !accum.rest) continue;
        const Entity target = accum.target;
        const anim::PropertyRest& rest = *accum.rest;
        const anim::PropertyBinding& binding = rest.binding;
        const size_t width = std::min<size_t>(binding.width, 4);
        if (width == 0 || !scene->isEntityCreated(target)) continue;

        float stored[4] = {0, 0, 0, 0};
        const bool known = anim::readPropertyStored(scene, target, binding, stored);
        float current[4] = {stored[0], stored[1], stored[2], stored[3]};
        anim::storedToKey(binding.kind, binding.display, current, width);

        // the value in key space: the rest fills what the contributors leave (R1)
        float value[4] = {0, 0, 0, 0};
        if (binding.discrete){
            value[0] = accum.bestValue;
            if (accum.total < 1.0f && rest.has && 1.0f - accum.total > accum.bestWeight) value[0] = rest.value[0];
        }else{
            for (size_t k = 0; k < width; k++){
                const float w = accum.weights[k];
                if (w > 0.0f){
                    value[k] = (rest.has && w < 1.0f) ? accum.sums[k] + rest.value[k] * (1.0f - w) : accum.sums[k] / w;
                }else{
                    // a component no track keys keeps its value
                    value[k] = known ? current[k] : rest.value[k];
                }
            }
        }

        // written only when the stored value changes; a component no track keys keeps its stored
        // value exactly (no key-space round trip: a ui.color.a fade leaves r, g, b alone)
        float next[4] = {value[0], value[1], value[2], value[3]};
        anim::keyToStored(binding.kind, binding.display, binding.discrete || binding.interpolateInt, next, width);
        if (known && !binding.discrete){
            for (size_t k = 0; k < width; k++){
                if (!(accum.weights[k] > 0.0f)) next[k] = stored[k];
            }
        }
        bool changed = !known;
        for (size_t k = 0; k < width && !changed; k++){
            if (next[k] != stored[k]) changed = true;
        }
        if (!changed) continue;
        if (binding.deferred){
            DeferredPropertyWrite write;
            write.target = target;
            write.value = value[0];
            write.binding = binding;
            deferred.push_back(std::move(write));
            continue;
        }
        anim::writePropertyStored(scene, target, binding, next);
    }
}

float ActionSystem::getDuration(Entity entity) {
    return getDuration(entity, 0);
}

Matrix4 ActionSystem::getRelativeTrackMatrix(Entity target, const Vector3& base){
    Transform* transform = scene->findComponent<Transform>(target);
    RectTransformComponent* layout = scene->findComponent<RectTransformComponent>(target);
    if (!transform || !layout || !layout->usingAnchors){
        return Matrix4::translateMatrix(base);
    }

    // an anchored element moves by its layout offset, mapped to its parent's space
    UISystem* uisystem = scene->getSystem<UISystem>().get();
    Vector2 anchor = uisystem->parentToLayoutSpace(target, Vector2(transform->position.x, transform->position.y)) - layout->positionOffset;
    Vector2 origin = uisystem->layoutToParentSpace(target, anchor + Vector2(base.x, base.y));
    Vector2 axisX = uisystem->layoutToParentSpace(target, anchor + Vector2(base.x + 1, base.y)) - origin;
    Vector2 axisY = uisystem->layoutToParentSpace(target, anchor + Vector2(base.x, base.y + 1)) - origin;

    Matrix4 matrix;
    matrix.setColumn(0, Vector4(axisX.x, axisX.y, 0, 0));
    matrix.setColumn(1, Vector4(axisY.x, axisY.y, 0, 0));
    matrix.setColumn(3, Vector4(origin.x, origin.y, base.z, 1));
    return matrix;
}

bool ActionSystem::getRelativeTrackPose(Entity target, Vector3& position, Quaternion& rotation, Vector3& scale){
    Transform* transform = (target != NULL_ENTITY && scene->isEntityCreated(target)) ? scene->findComponent<Transform>(target) : nullptr;
    if (!transform){
        return false;
    }

    position = transform->position;
    rotation = transform->rotation;
    scale = transform->scale;

    // the layout places an anchored element from its offset every frame
    RectTransformComponent* layout = scene->findComponent<RectTransformComponent>(target);
    if (layout && layout->usingAnchors){
        position = Vector3(layout->positionOffset.x, layout->positionOffset.y, transform->position.z);
    }

    return true;
}

float ActionSystem::getFrameDuration(const ActionFrame& frame) {
    if (frame.duration > 0) {
        return frame.duration;
    }
    return getDuration(frame.action, 0);
}

bool ActionSystem::isAnimationReachable(Entity from, Entity target) {
    std::unordered_set<Entity> visited;
    return isAnimationReachable(from, target, visited);
}

bool ActionSystem::isAnimationReachable(Entity from, Entity target, std::unordered_set<Entity>& visited) {
    if (from == NULL_ENTITY) {
        return false;
    }
    if (from == target) {
        return true;
    }
    if (!visited.insert(from).second) {
        return false; // already walked (protects against pre-existing cycles)
    }

    AnimationComponent* anim = scene->findComponent<AnimationComponent>(from);
    if (!anim) {
        return false;
    }
    for (const ActionFrame& frame : anim->actions) {
        if (isAnimationReachable(frame.action, target, visited)) {
            return true;
        }
    }
    return false;
}

float ActionSystem::getDuration(Entity entity, int depth) {
    // Animations can nest other animations as frames; cap recursion so a cycle
    // of auto-duration frames cannot hang the engine.
    if (entity == NULL_ENTITY || depth > 8) {
        return 0;
    }

    float duration = 0;
    if (TweenComponent* timed = scene->findComponent<TweenComponent>(entity)) {
        duration = timed->pingPong ? timed->duration * 2 : timed->duration;
    } else if (AnimationComponent* anim = scene->findComponent<AnimationComponent>(entity)) {
        if (anim->duration > 0) {
            duration = anim->duration;
        } else {
            for (const ActionFrame& frame : anim->actions) {
                float frameDuration = (frame.duration > 0) ? frame.duration : getDuration(frame.action, depth + 1);
                duration = std::max(duration, frame.startTime + frameDuration);
            }
        }
    } else if (KeyframeTracksComponent* kf = scene->findComponent<KeyframeTracksComponent>(entity)) {
        if (!kf->times.empty()) {
            duration = kf->times.back();
        }
    } else if (SpriteAnimationComponent* spriteanim = scene->findComponent<SpriteAnimationComponent>(entity)) {
        float totalMs = 0;
        for (unsigned int i = 0; i < spriteanim->framesTimeSize; i++) {
            totalMs += spriteanim->framesTime[i];
        }
        duration = totalMs / 1000.0f;
    } else if (ParticleSystemComponent* parts = scene->findComponent<ParticleSystemComponent>(entity)) {
        float emitDuration = getParticleCycleDuration(*parts);
        if (emitDuration > 0.0f) {
            if (parts->loop) {
                // One emission cycle: used for timeline wrapping
                duration = emitDuration;
            } else {
                // Full system lifetime: emit phase + last particle dies
                duration = emitDuration + parts->lifeInitializer.maxLife;
            }
        }
    }
    return duration;
}

void ActionSystem::load(){

}

void ActionSystem::resetRunningActions(){
    // Reset actions serialized in Running/Paused state so actionStart fires on the first update tick.
    auto actions = scene->getComponentArray<ActionComponent>();
    for (int i = 0; i < actions->size(); i++) {
        ActionComponent& action = actions->getComponentFromIndex(i);

        if (action.state == ActionState::Running || action.state == ActionState::Paused) {
            action.state = ActionState::Stopped;
            action.timecount = 0;
            action.startTrigger = true;
            action.stopTrigger = false;
            action.pauseTrigger = false;
        }
    }
}

void ActionSystem::destroy(){
    auto actions = scene->getComponentArray<ActionComponent>();
    for (int i = 0; i < actions->size(); i++) {
        ActionComponent &action = actions->getComponentFromIndex(i);
        Entity entity = actions->getEntity(i);
        Signature signature = scene->getSignature(entity);

        action.state = ActionState::Stopped;
        action.timecount = 0;
        action.startTrigger = false;
        action.stopTrigger = false;
        action.pauseTrigger = false;

        if (signature.test(scene->getComponentId<TweenComponent>())) {
            TweenComponent &tween = scene->getComponent<TweenComponent>(entity);
            tween.time = 0;
            tween.value = 0;
        }
    }
}

void ActionSystem::draw(){

}

ActionComponent* ActionSystem::actionStateChange(Entity entity, ActionComponent& action){
    ActionComponent* current = &action;

    // Action stop, before the start so both together restart it
    if (current->stopTrigger == true && (current->state == ActionState::Running || current->state == ActionState::Paused)){
        actionStop(entity);

        // onStop may destroy the entity or move its components
        current = scene->isEntityCreated(entity) ? scene->findComponent<ActionComponent>(entity) : nullptr;
        if (!current){
            return nullptr;
        }
    }

    // Action start
    if (current->startTrigger == true && (current->state == ActionState::Stopped || current->state == ActionState::Paused)){
        actionStart(entity);

        // onStart may also
        current = scene->isEntityCreated(entity) ? scene->findComponent<ActionComponent>(entity) : nullptr;
        if (!current){
            return nullptr;
        }
    }

    // Action pause
    if (current->pauseTrigger == true && current->state == ActionState::Running){
        actionPause(entity);

        current = scene->isEntityCreated(entity) ? scene->findComponent<ActionComponent>(entity) : nullptr;
    }

    return current;
}

void ActionSystem::processRunningAction(double dt, Entity entity, ActionComponent& action){
    Signature signature = scene->getSignature(entity);
    Signature targetSignature;
    if (action.target != NULL_ENTITY){
        targetSignature = scene->getSignature(action.target);
    }

    //Sprite animation
    if (signature.test(scene->getComponentId<SpriteAnimationComponent>())){
        SpriteAnimationComponent& spriteanim = scene->getComponent<SpriteAnimationComponent>(entity);
        if (targetSignature.test(scene->getComponentId<SpriteComponent>()) && targetSignature.test(scene->getComponentId<MeshComponent>())){
            SpriteComponent& sprite = scene->getComponent<SpriteComponent>(action.target);
            MeshComponent& mesh = scene->getComponent<MeshComponent>(action.target);

            spriteActionUpdate(dt, entity, action, mesh, sprite, spriteanim);
            if (action.state != ActionState::Running) return;
        }
    }

    //Particles (a disabled Particle System holds its particles until enabled again); version 2
    //systems step in updateParticleSystems
    if (signature.test(scene->getComponentId<ParticleSystemComponent>()) &&
            scene->getComponent<ParticleSystemComponent>(entity).enabled &&
            scene->getComponent<ParticleSystemComponent>(entity).version < 2){
        ParticleSystemComponent& particles = scene->getComponent<ParticleSystemComponent>(entity);

        if (targetSignature.test(scene->getComponentId<InstancedMeshComponent>())){
            InstancedMeshComponent& instmesh = scene->getComponent<InstancedMeshComponent>(action.target);

            particlesActionUpdate(dt, entity, action.target, action, particles, instmesh);
            if (action.state != ActionState::Running) return;
        }
        if (targetSignature.test(scene->getComponentId<PointsComponent>())){
            PointsComponent& points = scene->getComponent<PointsComponent>(action.target);

            particlesActionUpdate(dt, entity, action.target, action, particles, points);
            if (action.state != ActionState::Running) return;
        }
    }

    //keyframe animation
    if (signature.test(scene->getComponentId<KeyframeTracksComponent>())){
        KeyframeTracksComponent& keyframe = scene->getComponent<KeyframeTracksComponent>(entity);

        // a looping track starts over, keeping the overshoot
        if (keyframe.loop && !keyframe.times.empty() && keyframe.times.back() > 0 && action.timecount >= keyframe.times.back()){
            action.timecount = std::fmod(action.timecount, keyframe.times.back());
        }

        // a relative track that did not start through actionStart (a decoded running one) takes its base now
        if (keyframe.relative && !keyframe.hasBase){
            keyframe.hasBase = getRelativeTrackPose(action.target, keyframe.basePosition, keyframe.baseRotation, keyframe.baseScale);
        }

        keyframeUpdate(dt, action, keyframe);
        if (action.state != ActionState::Running) return;

        // Stop when past the last keyframe time
        if (!keyframe.times.empty() && action.timecount >= keyframe.times.back()) {
            keyframe.interpolation = 1;
            keyframe.index = keyframe.times.size() - 1;
        }

        if (signature.test(scene->getComponentId<TranslateTracksComponent>())){
            TranslateTracksComponent& translatetracks = scene->getComponent<TranslateTracksComponent>(entity);

            if (targetSignature.test(scene->getComponentId<Transform>())){
                translateTracksUpdate(keyframe, translatetracks, action.target, action.weight);
            }
        }

        if (signature.test(scene->getComponentId<RotateTracksComponent>())){
            RotateTracksComponent& rotatetracks = scene->getComponent<RotateTracksComponent>(entity);

            if (targetSignature.test(scene->getComponentId<Transform>())){
                rotateTracksUpdate(keyframe, rotatetracks, action.target, action.weight);
            }
        }

        if (signature.test(scene->getComponentId<ScaleTracksComponent>())){
            ScaleTracksComponent& scaletracks = scene->getComponent<ScaleTracksComponent>(entity);

            if (targetSignature.test(scene->getComponentId<Transform>())){
                scaleTracksUpdate(keyframe, scaletracks, action.target, action.weight);
            }
        }

        if (signature.test(scene->getComponentId<MorphTracksComponent>())){
            MorphTracksComponent& morpthtracks = scene->getComponent<MorphTracksComponent>(entity);

            if (targetSignature.test(scene->getComponentId<MeshComponent>())){
                morphTracksUpdate(keyframe, morpthtracks, action.target, action.weight);
            }
        }

        if (signature.test(scene->getComponentId<LatticeTracksComponent>())){
            LatticeTracksComponent& latticetracks = scene->getComponent<LatticeTracksComponent>(entity);

            if (targetSignature.test(scene->getComponentId<LatticeComponent>())){
                LatticeComponent& latticecomp = scene->getComponent<LatticeComponent>(action.target);

                latticeTracksUpdate(keyframe, latticetracks, action.target, latticecomp, action.weight);
            }
        }

        // Stop after applying final values
        if (!keyframe.times.empty() && action.timecount >= keyframe.times.back()) {
            actionStop(entity);
            return;
        }
    }


    if (signature.test(scene->getComponentId<TweenComponent>())){
        TweenComponent& tween = scene->getComponent<TweenComponent>(entity);

        bool finished = tweenUpdate(dt, entity, action, tween);
        if (action.state != ActionState::Running) return;

        //Transform animation
        if (targetSignature.test(scene->getComponentId<Transform>())){
            Transform& transform = scene->getComponent<Transform>(action.target);

            if (signature.test(scene->getComponentId<PositionTweenComponent>())){
                PositionTweenComponent& posaction = scene->getComponent<PositionTweenComponent>(entity);

                positionTweenUpdate(tween, posaction, transform);
            }

            if (signature.test(scene->getComponentId<RotationTweenComponent>())){
                RotationTweenComponent& rotaction = scene->getComponent<RotationTweenComponent>(entity);

                rotationTweenUpdate(tween, rotaction, transform);
            }

            if (signature.test(scene->getComponentId<ScaleTweenComponent>())){
                ScaleTweenComponent& scaletween = scene->getComponent<ScaleTweenComponent>(entity);

                scaleTweenUpdate(tween, scaletween, transform);
            }
        }

        //Color animation
        if (signature.test(scene->getComponentId<ColorTweenComponent>())){
            ColorTweenComponent& colortween = scene->getComponent<ColorTweenComponent>(entity);

            if (targetSignature.test(scene->getComponentId<MeshComponent>())){
                MeshComponent& mesh = scene->getComponent<MeshComponent>(action.target);

                colorTweenUpdate(tween, colortween, mesh.submeshes[0].material.baseColorFactor);
            }
            if (targetSignature.test(scene->getComponentId<UIComponent>())){
                UIComponent& ui = scene->getComponent<UIComponent>(action.target);

                colorTweenUpdate(tween, colortween, ui.color);
            }
        }

        //Alpha animation
        if (signature.test(scene->getComponentId<AlphaTweenComponent>())){
            AlphaTweenComponent& alphatween = scene->getComponent<AlphaTweenComponent>(entity);

            // AUTO: the target's enabled Canvas Group when it has one, else its colour alpha
            CanvasGroupComponent* group = targetSignature.test(scene->getComponentId<CanvasGroupComponent>())
                ? &scene->getComponent<CanvasGroupComponent>(action.target) : nullptr;
            const bool toGroup = alphatween.alphaTarget == AlphaTweenTarget::CANVAS_GROUP ||
                                 (alphatween.alphaTarget == AlphaTweenTarget::AUTO && group && group->enabled);

            if (toGroup){
                if (group){
                    const float alpha = alphatween.startAlpha + (alphatween.endAlpha - alphatween.startAlpha) * tween.value;
                    // clamped (an overshooting ease stays in range), NaN rejected
                    if (!std::isnan(alpha)){
                        group->alpha = std::clamp(alpha, 0.0f, 1.0f);
                    }
                }
            }else if (targetSignature.test(scene->getComponentId<MeshComponent>())){
                MeshComponent& mesh = scene->getComponent<MeshComponent>(action.target);

                alphaTweenUpdate(tween, alphatween, mesh.submeshes[0].material.baseColorFactor);
            }
            if (!toGroup && targetSignature.test(scene->getComponentId<UIComponent>())){
                UIComponent& ui = scene->getComponent<UIComponent>(action.target);

                alphaTweenUpdate(tween, alphatween, ui.color);
            }
        }

        // Stop after applying the final values
        if (finished){
            actionStop(entity);
            return;
        }
    }

    actionUpdate(dt, action);
}

void ActionSystem::updateAnimationPreview(double dt, Entity entity){
    std::vector<Entity> entities;
    entities.push_back(entity);
    updateAnimationPreview(dt, entities);
}

void ActionSystem::updateAnimationPreview(double dt, const std::vector<Entity>& entities){
    // Drives one or more top-level animations into a single pose-blend scope so
    // concurrently running clips crossfade (used by the editor transition preview).
    clearPoseBlend();

    std::unordered_set<Entity> visitedAnimations;

    std::function<void(Entity)> previewAnimation = [&](Entity animationEntity) {
        Signature signature = scene->getSignature(animationEntity);
        if (!signature.test(scene->getComponentId<AnimationComponent>()) ||
            !signature.test(scene->getComponentId<ActionComponent>()) ||
            !visitedAnimations.insert(animationEntity).second) {
            return;
        }

        ActionComponent& action = scene->getComponent<ActionComponent>(animationEntity);

        // the callbacks may remove or move the components
        ActionComponent* current = actionStateChange(animationEntity, action);
        AnimationComponent* animcomp = current ? scene->findComponent<AnimationComponent>(animationEntity) : nullptr;
        if (!animcomp){
            return;
        }
        if (current->state == ActionState::Running){
            animationUpdate(dt, animationEntity, *current, *animcomp);
            animcomp = scene->isEntityCreated(animationEntity) ? scene->findComponent<AnimationComponent>(animationEntity) : nullptr;
            if (!animcomp){
                return;
            }
        }

        for (Entity childEntity : animationChildEntities(*animcomp)){
            if (childEntity == NULL_ENTITY || !scene->isEntityCreated(childEntity)) {
                continue;
            }

            Signature childSig = scene->getSignature(childEntity);
            if (!childSig.test(scene->getComponentId<ActionComponent>())) {
                continue;
            }

            ActionComponent* childAction = actionStateChange(childEntity, scene->getComponent<ActionComponent>(childEntity));

            if (!childAction || childAction->state != ActionState::Running) {
                continue;
            }

            if (childSig.test(scene->getComponentId<AnimationComponent>())) {
                previewAnimation(childEntity);
            } else {
                processRunningAction(dt, childEntity, *childAction);
            }
        }

        current = scene->isEntityCreated(animationEntity) ? scene->findComponent<ActionComponent>(animationEntity) : nullptr;
        if (current && current->state == ActionState::Running){
            actionUpdate(dt, *current);
        }
    };

    for (Entity entity : entities){
        if (entity != NULL_ENTITY && scene->isEntityCreated(entity)){
            previewAnimation(entity);
        }
    }

    flushPoseBlend();
}

void ActionSystem::updateActionPreview(double dt, Entity entity){
    if (!scene->isEntityCreated(entity)) return;

    Signature signature = scene->getSignature(entity);
    if (!signature.test(scene->getComponentId<ActionComponent>())) return;

    // Animation entities need the full animation preview path
    if (signature.test(scene->getComponentId<AnimationComponent>())) {
        updateAnimationPreview(dt, entity);
        return;
    }

    // a version 2 Particle System steps whatever its action's state (emitted particles live
    // on a stopped system); the preview never runs its stop action
    if (isParticleSystemV2(entity)){
        updateParticlePreview(dt, {entity});
        return;
    }

    ActionComponent* action = actionStateChange(entity, scene->getComponent<ActionComponent>(entity));

    if (action && action->state == ActionState::Running){
        clearPoseBlend();
        processRunningAction(dt, entity, *action);
        flushPoseBlend();
    }
}

// The unscaled frame delta of an update given the scaled one. While the time scale is above 0
// it is dt / timeScale (the frame delta before the scale, exact for any dt a caller steps
// with); at time scale 0 it is the frame delta the engine measured (the fixed frame delta
// when set, else the last frame's, capped like Engine::deltatime): Engine::getUnscaledDeltatime(),
// or, for an update stepped outside an engine frame, the same value worked out here.
double ActionSystem::unscaledDeltatime(double dt){
    const double timeScale = Engine::getTimeScale();
    if (timeScale > 0.0){
        return dt / timeScale;
    }
    const double frameUnscaled = Engine::getUnscaledDeltatime();
    if (frameUnscaled > 0.0){
        return frameUnscaled;
    }
    if (Engine::getFixedDeltatime() > 0.0){
        return Engine::getFixedDeltatime();
    }
    const float framerate = Engine::getFramerate();
    if (framerate <= 0.0f){
        return 0.0;
    }
    return std::min(1.0 / (double)framerate, (double)Engine::getMaxDeltatime());
}

void ActionSystem::update(double dt){
    if (paused) {
        return;
    }

    clearPoseBlend();
    poseBlendScope = true;

    //Animations actions
    auto animations = scene->getComponentArray<AnimationComponent>();
    for (int i = 0; i < animations->size(); i++){
        Entity entity = animations->getEntity(i);
        Signature signature = scene->getSignature(entity);

        // an inactive GameObject's animations wait, triggers included
        if (!scene->isEntityActiveInHierarchy(entity)){
            continue;
        }

        if (signature.test(scene->getComponentId<ActionComponent>())){
            ActionComponent* action = actionStateChange(entity, scene->getComponent<ActionComponent>(entity));

            // the callbacks may have removed or moved the components
            AnimationComponent* current = action ? scene->findComponent<AnimationComponent>(entity) : nullptr;
            if (current && action->state == ActionState::Running){
                animationUpdate(dt, entity, *action, *current);
            }
        }
    }

    //All actions
    // a tween with useUnscaledTime advances with the frame delta before the time scale
    const double unscaledDt = unscaledDeltatime(dt);
    auto actions = scene->getComponentArray<ActionComponent>();
    for (int i = 0; i < actions->size(); i++){
		Entity entity = actions->getEntity(i);

        if (!scene->isEntityActiveInHierarchy(entity)){
            continue;
        }

        ActionComponent* action = actionStateChange(entity, actions->getComponentFromIndex(i));

        // Action update
        if (action && action->state == ActionState::Running){
            const TweenComponent* timed = scene->findComponent<TweenComponent>(entity);
            processRunningAction((timed && timed->useUnscaledTime) ? unscaledDt : dt, entity, *action);
        }

	}

    // version 2 Particle Systems (playOnAwake, stepping, their stop)
    updateParticleSystems(dt);

    // Animation Players add their states to the same blend
    updatePlayers(dt);

    // Write the blended (weighted-average) pose to each animated target.
    flushPoseBlend();
    poseBlendScope = false;
    // the writes that run onEnable / onDisable, now that a callback may sample a player
    applyDeferredPropertyWrites();

    // clip events and onFinish, after the pose is written
    dispatchPlayerCallbacks();

    // Particle System stop actions (disable, destroy, onParticleSystemStopped): after the
    // passes, which they would change under them
    runParticleStopActions();
}

void ActionSystem::onComponentAdded(Entity entity, ComponentId componentId) {

}

void ActionSystem::onComponentRemoved(Entity entity, ComponentId componentId) {
	if (componentId == scene->getComponentId<ActionComponent>()) {
		ActionComponent& action = scene->getComponent<ActionComponent>(entity);

		// Snapshot before stopping children: their onStop callbacks are user code
		// that may add/remove components and invalidate this reference.
		const bool wasPlaying = action.state == ActionState::Running || action.state == ActionState::Paused;
		const bool ownedTarget = action.ownedTarget;
		const Entity target = action.target;

		// A playing animation that loses its ActionComponent can no longer drive or
		// stop its child tracks; stop them now so they don't run on orphaned.
		// A Stopped parent's running children are standalone user-driven actions
		// and are deliberately left alone.
		if (wasPlaying) {
			if (AnimationComponent* animation = scene->findComponent<AnimationComponent>(entity)) {
				animationStopChildren(animationChildEntities(*animation));
			}
		}

		// (a system drawing on its own GameObject owns no other)
		if (ownedTarget && target != NULL_ENTITY && target != entity && scene->isEntityCreated(target)) {
			scene->destroyEntity(target);
		}
	} else if (componentId == scene->getComponentId<AnimationComponent>()) {
		AnimationComponent& animation = scene->getComponent<AnimationComponent>(entity);

		// Snapshot before callbacks, same reason as above.
		const bool ownedActions = animation.ownedActions;
		const std::vector<Entity> children = animationChildEntities(animation);

		// The clip being removed can no longer drive or stop anything: stop the
		// whole action (children included) so the remaining bare ActionComponent
		// doesn't keep running through the all-actions pass.
		if (ActionComponent* action = scene->findComponent<ActionComponent>(entity)) {
			if (action->state == ActionState::Running || action->state == ActionState::Paused) {
				actionStop(entity);
			}
		}

		if (ownedActions) {
			for (Entity child : children) {
				if (child != NULL_ENTITY && scene->isEntityCreated(child)) {
					scene->destroyEntity(child);
				}
			}
		}
	}
}
