// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ActionSystem.h"

#include "Scene.h"
#include "util/Color.h"
#include "math/Interpolation.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/ParticleSubsystem.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <functional>
#include <unordered_set>

using namespace jasperengine;

void ActionSystem::actionStart(Entity entity){
    ActionComponent& action = scene->getComponent<ActionComponent>(entity);
    Signature signature = scene->getSignature(entity);

    actionComponentStart(action);

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

    if (action.target != NULL_ENTITY){
        Signature targetSignature = scene->getSignature(action.target);

        if (signature.test(scene->getComponentId<SpriteAnimationComponent>())){
            SpriteAnimationComponent& spriteanim = scene->getComponent<SpriteAnimationComponent>(entity);
            if (targetSignature.test(scene->getComponentId<SpriteComponent>()) && targetSignature.test(scene->getComponentId<MeshComponent>())){
                SpriteComponent& sprite = scene->getComponent<SpriteComponent>(action.target);
                MeshComponent& mesh = scene->getComponent<MeshComponent>(action.target);

                spriteActionStart(mesh, sprite, spriteanim);

            }
        }

        // a controller of a particle system (D1): it plays the system
        if (isParticleController(entity, action)){
            scene->getSystem<ParticleSubsystem>()->play(action.target);
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

void ActionSystem::actionComponentStart(ActionComponent& action){
    action.state = ActionState::Running;
    action.startTrigger = false;

    // what the Lua callbacks destroy goes when they are done (StructuralDeferScope)
    StructuralDeferScope deferScope(*scene);
    action.onStart.call();
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
    // Dispatch from a copy so a callback can remove/destroy the component that
    // owns the original subscription list without invalidating call().
    auto onStop = action.onStop;

    action.state = ActionState::Stopped;
    action.stopTrigger = false;
    action.timecount = 0;

    StructuralDeferScope deferScope(*scene);
    onStop.call();
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
    // Dispatch from a copy so a callback can remove/destroy the component that
    // owns the original subscription list without invalidating call().
    auto onPause = action.onPause;

    action.state = ActionState::Paused;
    action.pauseTrigger = false;

    StructuralDeferScope deferScope(*scene);
    onPause.call();
}

void ActionSystem::actionUpdate(double dt, ActionComponent& action){
    action.timecount += dt * action.speed;

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

    for (int i = 0; i < animcomp.actions.size(); i++){

        float timeDiff = action.timecount - animcomp.actions[i].startTime;

        Signature isignature = scene->getSignature(animcomp.actions[i].action);

        if (isignature.test(scene->getComponentId<ActionComponent>())){

            if (timeDiff >= 0) {
                //TODO: Support loop actions
                ActionComponent& iaction = scene->getComponent<ActionComponent>(animcomp.actions[i].action);
                if (iaction.state != ActionState::Running) {
                    actionStart(animcomp.actions[i].action);
                }

                iaction.timecount = timeDiff * iaction.speed;
                iaction.weight = animcomp.weight;

                float frameDuration = getFrameDuration(animcomp.actions[i]) / iaction.speed;
                actionsEnd = std::max(actionsEnd, animcomp.actions[i].startTime + frameDuration);

                if (timeDiff > frameDuration) {
                    totalActionsPassed++;
                }
            }

        }

    }

    bool actionsPassed = totalActionsPassed == animcomp.actions.size();
    bool durationPassed = animcomp.duration >= 0 && action.timecount >= (animcomp.duration / action.speed);

    if (actionsPassed || durationPassed) {
        if (!animcomp.loop) {
            actionStop(entity);
        }else{
            float loopDuration = durationPassed ? (animcomp.duration / action.speed) : actionsEnd;

            // keep the overshoot, or the cycle gets longer at lower frame rates
            if (loopDuration > 0 && action.timecount >= loopDuration){
                action.timecount = std::fmod(action.timecount, loopDuration);
            }else{
                action.timecount = 0;
            }
        }
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
    spriteanim.spriteFrameCount += dt * 1000;
    while (spriteanim.spriteFrameCount >= spriteanim.framesTime[spriteanim.frameTimeIndex]) {

        spriteanim.spriteFrameCount -= spriteanim.framesTime[spriteanim.frameTimeIndex];

        spriteanim.frameIndex++;
        spriteanim.frameTimeIndex++;

        if (spriteanim.frameIndex == spriteanim.framesSize - 1) {
            if (!spriteanim.loop) {
                actionStop(entity);
            }
        }

        if (spriteanim.frameIndex >= spriteanim.framesSize){
            spriteanim.frameIndex = 0;
            if (spriteanim.loop){
                action.timecount = 0;
            }
        }
        
        if (spriteanim.frameTimeIndex >= spriteanim.framesTimeSize)
            spriteanim.frameTimeIndex = 0;

        setSpriteTextureRect(mesh, sprite, spriteanim);
    }
}

void ActionSystem::tweenStop(TweenComponent& tween){
    tween.time = 0;
    tween.value = 0;
}

void ActionSystem::tweenUpdate(double dt, Entity entity, ActionComponent& action, TweenComponent& tween){
    if ((tween.time == 1) && !tween.loop){
        actionStop(entity);
    } else {
        float duration = tween.duration / action.speed;

        if (duration > 0) {

            if (action.timecount >= duration){
                if (!tween.loop){
                    action.timecount = duration;
                }else{
                    action.timecount -= duration;
                }
            }

            tween.time = action.timecount / duration;
        } else {
            tween.time = 1;
        }

        tween.value = tween.function.call(tween.time);
    }
}

void ActionSystem::positionTweenUpdate(TweenComponent& tween, PositionTweenComponent& posaction, Transform& transform){
    Vector3 position = (posaction.endPosition - posaction.startPosition) * tween.value;
    transform.position = posaction.startPosition + position;
    transform.needUpdate = true;
}

void ActionSystem::rotationTweenUpdate(TweenComponent& tween, RotationTweenComponent& rotaction, Transform& transform){
    transform.rotation = Quaternion::slerp(tween.value, rotaction.startRotation, rotaction.endRotation, rotaction.shortestPath);
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

    TransformBlendAccum& accum = transformBlend[target];
    accum.positionSum = accum.positionSum + value * weight;
    accum.positionWeight += weight;
}

void ActionSystem::scaleTracksUpdate(KeyframeTracksComponent& keyframe, ScaleTracksComponent& scaletracks, Entity target, float weight){
    if (weight <= 0.0f || !trackIndexValid(keyframe, scaletracks))
        return;

    Vector3 value = sampleVectorTrack(keyframe, scaletracks);

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

void ActionSystem::clearPoseBlend(){
    transformBlend.clear();
    morphBlend.clear();
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
            transform->position = accum.positionSum * (1.0f / accum.positionWeight);
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

    clearPoseBlend();
}

float ActionSystem::getDuration(Entity entity) {
    return getDuration(entity, 0);
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
        duration = timed->duration;
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

void ActionSystem::actionStateChange(Entity entity, ActionComponent& action){
    // Action start
    if (action.startTrigger == true && (action.state == ActionState::Stopped || action.state == ActionState::Paused)){
        actionStart(entity);
    }

    // Action stop
    if (action.stopTrigger == true && (action.state == ActionState::Running || action.state == ActionState::Paused)){
        actionStop(entity);
    }

    // Action pause
    if (action.pauseTrigger == true && action.state == ActionState::Running){
        actionPause(entity);
    }
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

        // Stop after applying final values
        if (!keyframe.times.empty() && action.timecount >= keyframe.times.back()) {
            actionStop(entity);
            return;
        }
    }


    if (signature.test(scene->getComponentId<TweenComponent>())){
        TweenComponent& tween = scene->getComponent<TweenComponent>(entity);

        tweenUpdate(dt, entity, action, tween);
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

        AnimationComponent& animcomp = scene->getComponent<AnimationComponent>(animationEntity);
        ActionComponent& action = scene->getComponent<ActionComponent>(animationEntity);

        actionStateChange(animationEntity, action);
        if (action.state == ActionState::Running){
            animationUpdate(dt, animationEntity, action, animcomp);
        }

        for (int i = 0; i < (int)animcomp.actions.size(); i++){
            Entity childEntity = animcomp.actions[i].action;
            if (childEntity == NULL_ENTITY || !scene->isEntityCreated(childEntity)) {
                continue;
            }

            Signature childSig = scene->getSignature(childEntity);
            if (!childSig.test(scene->getComponentId<ActionComponent>())) {
                continue;
            }

            ActionComponent& childAction = scene->getComponent<ActionComponent>(childEntity);
            actionStateChange(childEntity, childAction);

            if (childAction.state != ActionState::Running) {
                continue;
            }

            if (childSig.test(scene->getComponentId<AnimationComponent>())) {
                previewAnimation(childEntity);
            } else {
                processRunningAction(dt, childEntity, childAction);
            }
        }

        if (action.state == ActionState::Running){
            actionUpdate(dt, action);
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

    ActionComponent& action = scene->getComponent<ActionComponent>(entity);

    actionStateChange(entity, action);

    if (action.state == ActionState::Running){
        clearPoseBlend();
        processRunningAction(dt, entity, action);
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
        AnimationComponent& animcomp = animations->getComponentFromIndex(i);

        Entity entity = animations->getEntity(i);
        Signature signature = scene->getSignature(entity);

        // an inactive GameObject's animations wait, triggers included
        if (!scene->isEntityActiveInHierarchy(entity)){
            continue;
        }

        if (signature.test(scene->getComponentId<ActionComponent>())){
            ActionComponent& action = scene->getComponent<ActionComponent>(entity);

            actionStateChange(entity, action);

            if (action.state == ActionState::Running){
                animationUpdate(dt, entity, action, animcomp);
            }
        }
    }

    //All actions
    auto actions = scene->getComponentArray<ActionComponent>();
    for (int i = 0; i < actions->size(); i++){
		ActionComponent& action = actions->getComponentFromIndex(i);

        if (!scene->isEntityActiveInHierarchy(actions->getEntity(i))){
            continue;
        }

        actionStateChange(actions->getEntity(i), action);

        // Action update
        if (action.state == ActionState::Running){
            Entity entity = actions->getEntity(i);
            processRunningAction(dt, entity, action);
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
