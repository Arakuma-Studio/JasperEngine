// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ActionSystem.h"

#include "Scene.h"
#include "util/Color.h"
#include "math/Interpolation.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/ParticleSubsystem.h"
#include "lattice/LatticeSolver.h"
#include "subsystem/UISystem.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <functional>
#include <unordered_set>

using namespace jasperengine;

ActionSystem::ActionSystem(Scene* scene): SubSystem(scene){
    signature.set(scene->getComponentId<ActionComponent>());
    // the types onComponentRemoved handles
    declareHookType(scene->getComponentId<ActionComponent>());
    declareHookType(scene->getComponentId<AnimationComponent>());
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

        // a controller of a particle system (D1): it plays the system
        if (isParticleController(entity, *started)){
            scene->getSystem<ParticleSubsystem>()->play(target);
        }
    }
}

bool ActionSystem::isParticleController(Entity entity, const ActionComponent& action){
    if (action.target == NULL_ENTITY || !scene->isEntityCreated(action.target) ||
            !scene->getSignature(action.target).test(scene->getComponentId<ParticleSystemComponent>())){
        return false;
    }
    const Signature signature = scene->getSignature(entity);
    return !signature.test(scene->getComponentId<TweenComponent>()) && !signature.test(scene->getComponentId<SpriteAnimationComponent>()) &&
           !signature.test(scene->getComponentId<AnimationComponent>()) && !signature.test(scene->getComponentId<KeyframeTracksComponent>());
}

void ActionSystem::previewParticleController(double dt, Entity entity, const ActionComponent& action){
    if (isParticleController(entity, action))
        scene->getSystem<ParticleSubsystem>()->previewAdvance(action.target, dt);
}

void ActionSystem::actionComponentStart(ActionComponent& action){
    // Dispatch from a copy, as actionComponentStop does: a callback that adds components
    // moves the array holding the original subscription list while call() iterates it.
    auto onStart = action.onStart;

    action.state = ActionState::Running;
    action.startTrigger = false;

    // what the Lua callbacks destroy goes when they are done (StructuralDeferScope)
    StructuralDeferScope deferScope(*scene);
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

    actionComponentStop(*action);
    animationStopChildren(animationChildren);

    if (!scene->isEntityCreated(entity)){
        return;
    }

    if (TweenComponent* tween = scene->findComponent<TweenComponent>(entity)){
        tweenStop(*tween);
    }

    // a particle system's controller stops its emission (what was emitted lives on)
    if (ActionComponent* stopped = scene->findComponent<ActionComponent>(entity)){
        if (isParticleController(entity, *stopped)){
            scene->getSystem<ParticleSubsystem>()->stop(target, false);
        }
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
    StructuralDeferScope deferScope(*scene);
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

    actionComponentPause(*action);
    animationPauseChildren(animationChildren);

    if (ActionComponent* paused = scene->findComponent<ActionComponent>(entity)){
        if (isParticleController(entity, *paused)){
            scene->getSystem<ParticleSubsystem>()->pause(paused->target);
        }
    }
}

void ActionSystem::actionComponentPause(ActionComponent& action){
    action.state = ActionState::Paused;
    action.pauseTrigger = false;

    StructuralDeferScope deferScope(*scene);
    action.onPause.call();
}

void ActionSystem::actionUpdate(double dt, ActionComponent& action){
    action.timecount += dt * action.speed;
    if (action.timecount < 0){
        // a negative speed plays it back to the start
        action.timecount = 0;
    }

    StructuralDeferScope deferScope(*scene);
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

void ActionSystem::keyframeUpdate(double dt, ActionComponent& action, KeyframeTracksComponent& keyframe){
    if (keyframe.times.size() == 0)
        return;

    float currentTime = action.timecount;

    keyframe.interpolation = 0;

    keyframe.index = 0;
    while (keyframe.index < (keyframe.times.size()-1) && keyframe.times[keyframe.index] < currentTime){
        keyframe.index++;
    }

    float previousTime = 0;
    float nextTime = keyframe.times[keyframe.index];

    if (keyframe.index > 0){
        previousTime = keyframe.times[keyframe.index-1];
    }

    if (nextTime > previousTime){
        keyframe.interpolation = (currentTime - previousTime) / (nextTime - previousTime);
    }

    if (keyframe.interpolation > 1){
        keyframe.interpolation = 1;
    }

    // Per-segment easing: easings[i] shapes the segment from key i to key i+1.
    // Empty easings (e.g. GLTF LINEAR clips) keep pure linear interpolation;
    // GLTF STEP clips use EaseType::STEP here.
    if (keyframe.index > 0){
        size_t segment = (size_t)keyframe.index - 1;
        if (segment < keyframe.easings.size() && keyframe.easings[segment] != EaseType::LINEAR){
            keyframe.interpolation = Ease::getFunction(keyframe.easings[segment])(keyframe.interpolation);
        }
    }
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
    return !tracks.values.empty() && (size_t)keyframe.index < tracks.values.size();
}

// Sample a vector track at the current playhead: cubic Hermite when tangents
// are present, lerp otherwise.
template<typename TracksComp>
static Vector3 sampleVectorTrack(const KeyframeTracksComponent& keyframe, const TracksComp& tracks){
    Vector3 previous = tracks.values[(keyframe.index > 0) ? keyframe.index - 1 : 0];

    if (hasCubicSplineTangents(keyframe, tracks.values, tracks.inTangents, tracks.outTangents)){
        float td = keyframe.times[keyframe.index] - keyframe.times[keyframe.index-1];
        return Interpolation::cubicSpline(keyframe.interpolation, td,
                previous, tracks.outTangents[keyframe.index-1],
                tracks.values[keyframe.index], tracks.inTangents[keyframe.index]);
    }

    return previous + keyframe.interpolation * (tracks.values[keyframe.index] - previous);
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

    Quaternion previousRotation = rotatetracks.values[(keyframe.index > 0) ? keyframe.index - 1 : 0];

    Quaternion value;
    if (hasCubicSplineTangents(keyframe, rotatetracks.values, rotatetracks.inTangents, rotatetracks.outTangents)){
        float td = keyframe.times[keyframe.index] - keyframe.times[keyframe.index-1];
        value = Interpolation::cubicSpline(keyframe.interpolation, td,
                previousRotation, rotatetracks.outTangents[keyframe.index-1],
                rotatetracks.values[keyframe.index], rotatetracks.inTangents[keyframe.index]);
        // spec: the interpolated quaternion MUST be normalized before applying.
        // Guard the all-zero result the spec warns exporters about (v_k == -v_k+1):
        // normalizing it would spread NaN through the pose blend.
        if (value.norm() > FLT_EPSILON){
            value.normalize();
        }else{
            value = (keyframe.interpolation < 0.5f) ? previousRotation : rotatetracks.values[keyframe.index];
        }
    }else{
        value = Quaternion::slerp(keyframe.interpolation, previousRotation, rotatetracks.values[keyframe.index]);
    }
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

    if ((keyframe.index == 0) || (morpthtracks.values[keyframe.index].size() == morpthtracks.values[keyframe.index-1].size())) {
        bool cubic = hasCubicSplineTangents(keyframe, morpthtracks.values, morpthtracks.inTangents, morpthtracks.outTangents) &&
                     morpthtracks.outTangents[keyframe.index-1].size() == previousMorph.size() &&
                     morpthtracks.inTangents[keyframe.index].size() == morpthtracks.values[keyframe.index].size();
        float td = cubic ? keyframe.times[keyframe.index] - keyframe.times[keyframe.index-1] : 0;
        MorphBlendAccum& accum = morphBlend[target];
        for (int morphIndex = 0; morphIndex < morpthtracks.values[keyframe.index].size() && morphIndex < MAX_MORPHTARGETS; morphIndex++) {
            float value;
            if (cubic){
                value = Interpolation::cubicSpline(keyframe.interpolation, td,
                        previousMorph[morphIndex], morpthtracks.outTangents[keyframe.index-1][morphIndex],
                        morpthtracks.values[keyframe.index][morphIndex], morpthtracks.inTangents[keyframe.index][morphIndex]);
            }else{
                value = previousMorph[morphIndex] + keyframe.interpolation * (morpthtracks.values[keyframe.index][morphIndex] - previousMorph[morphIndex]);
            }
            accum.sums[morphIndex] += value * weight;
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
    float td = cubic ? keyframe.times[keyframe.index] - keyframe.times[keyframe.index-1] : 0;

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
        if (cubic){
            value = Interpolation::cubicSpline(keyframe.interpolation, td,
                    previous[n], latticetracks.outTangents[keyframe.index-1][n],
                    next[n], latticetracks.inTangents[keyframe.index][n]);
        }else{
            value = previous[n] + keyframe.interpolation * (next[n] - previous[n]);
        }
        accum.sums[point] += value * weight;
        accum.weights[point] += weight;
    }
}

void ActionSystem::clearPoseBlend(){
    transformBlend.clear();
    morphBlend.clear();
    for (auto it = latticeBlend.begin(); it != latticeBlend.end();){
        if (!scene->isEntityCreated(it->first)){
            it = latticeBlend.erase(it);
        }else{
            it->second.active = false;
            ++it;
        }
    }
}

void ActionSystem::flushPoseBlend(){
    for (auto& entry : transformBlend){
        Entity target = entry.first;
        TransformBlendAccum& accum = entry.second;

        if (!scene->isEntityCreated(target))
            continue;

        Transform* transform = scene->findComponent<Transform>(target);
        if (!transform)
            continue;

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
                mesh->morphWeights[i] = accum.sums[i] / accum.weights[i];
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
                Vector3 value = accum.sums[i] / accum.weights[i];
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

    clearPoseBlend();
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
    } else if (ActionComponent* action = scene->findComponent<ActionComponent>(entity)) {
        // a particle system's controller: one cycle of a looping effect (timeline wrapping),
        // else until its last particle dies
        if (isParticleController(entity, *action)) {
            if (const particles::ParticleEffectDocument* effect = ParticleSubsystem::getDocument(scene->getComponent<ParticleSystemComponent>(action->target))) {
                float lifetimeLo = 0.0f, lifetimeHi = 0.0f;
                effect->main.startLifetime.range(lifetimeLo, lifetimeHi);
                duration = effect->startDelay + effect->duration + (effect->looping ? 0.0f : lifetimeHi);
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

    // a particle system's controller runs until the system has stopped and its last particle died
    if (isParticleController(entity, action)){
        std::shared_ptr<ParticleSubsystem> particleSystem = scene->getSystem<ParticleSubsystem>();
        if (!particleSystem->isPlaying(action.target) && !particleSystem->isPaused(action.target) &&
                particleSystem->getParticleCount(action.target) == 0){
            actionStop(entity);
            return;
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

            if (targetSignature.test(scene->getComponentId<MeshComponent>())){
                MeshComponent& mesh = scene->getComponent<MeshComponent>(action.target);

                alphaTweenUpdate(tween, alphatween, mesh.submeshes[0].material.baseColorFactor);
            }
            if (targetSignature.test(scene->getComponentId<UIComponent>())){
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
                previewParticleController(dt, childEntity, *childAction);
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

    ActionComponent* action = actionStateChange(entity, scene->getComponent<ActionComponent>(entity));

    if (action && action->state == ActionState::Running){
        previewParticleController(dt, entity, *action);
        clearPoseBlend();
        processRunningAction(dt, entity, *action);
        flushPoseBlend();
    }
}

void ActionSystem::update(double dt){
    if (paused) {
        return;
    }

    clearPoseBlend();

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
    auto actions = scene->getComponentArray<ActionComponent>();
    for (int i = 0; i < actions->size(); i++){
		Entity entity = actions->getEntity(i);

        if (!scene->isEntityActiveInHierarchy(entity)){
            continue;
        }

        ActionComponent* action = actionStateChange(entity, actions->getComponentFromIndex(i));

        // Action update
        if (action && action->state == ActionState::Running){
            processRunningAction(dt, entity, *action);
        }

	}

    // Write the blended (weighted-average) pose to each animated target.
    flushPoseBlend();
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

		if (ownedTarget && target != NULL_ENTITY && scene->isEntityCreated(target)) {
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
