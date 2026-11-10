// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PoseAnimatorSystem.h"

#include "Scene.h"
#include "Log.h"
#include "component/ModelComponent.h"
#include "component/Transform.h"
#include "pose/ModelRig.h"
#include "pose/PoseFile.h"
#include "pixelart/HitStop.h"

#include <cmath>
#include <utility>
#include <vector>

using namespace jasperengine;

namespace {
    // how often a failed animator tries to load again
    constexpr float FAILED_RETRY_SECONDS = 1.0f;
}

PoseAnimatorSystem::PoseAnimatorSystem(Scene* scene): SubSystem(scene){
    signature.set(scene->getComponentId<PoseAnimatorComponent>());
}

bool PoseAnimatorSystem::fail(PoseAnimatorComponent& animator, const std::string& message){
    pose::PoseAnimatorRuntime& runtime = *animator.runtime;
    // a disabled animator leaves the bones as they were before it
    restoreBones(runtime);
    runtime.failed = true;
    runtime.loading = false;
    runtime.error = message;
    runtime.retryTimer = 0.0f;
    animator.needsLoad = false;
    // the same error again (a retry of a file still broken) is not logged again
    if (message != runtime.loggedError){
        Log::error("Pose Animator: %s", message.c_str());
        runtime.loggedError = message;
    }
    return false;
}

bool PoseAnimatorSystem::startLoad(PoseAnimatorComponent& animator){
    if (animator.runtime && animator.runtime->loading && animator.runtime->path == animator.animation){
        return animator.runtime->dataLoaded;
    }

    auto runtime = std::make_shared<pose::PoseAnimatorRuntime>();
    // the bones keep what the previous runtime wrote: its blend state says what is under it
    if (animator.runtime){
        const pose::PoseAnimatorRuntime& previous = *animator.runtime;
        runtime->blend = previous.blend;
        runtime->blendEntities = previous.blendEntities;
        if (previous.path == animator.animation){
            runtime->loggedError = previous.loggedError;
            // a rebuild (model reloaded, load retried) keeps the goToPose target
            runtime->keepGoTo(previous);
        }
    }
    runtime->path = animator.animation;
    runtime->loading = true;
    animator.runtime = runtime;

    if (animator.animation.empty()){
        // nothing to play, not an error: the bones get their base pose back
        restoreBones(*runtime);
        runtime->loading = false;
        animator.needsLoad = false;
        return false;
    }

    std::vector<std::string> warnings;
    std::string error;
    // the text is kept: with the rig it is what animators share one build by (PoseBuildCache)
    if (!pose::readTextFile(animator.animation, runtime->source, &error)){
        return fail(animator, animator.animation + ": " + error);
    }
    if (!pose::parsePoseAnimation(runtime->source, runtime->data, warnings, &error)){
        return fail(animator, animator.animation + ": " + error);
    }
    for (const std::string& warning : warnings){
        Log::warn("Pose Animator: %s: %s", animator.animation.c_str(), warning.c_str());
    }
    if (runtime->data.poses.empty()){
        return fail(animator, animator.animation + ": the animation has no poses");
    }
    runtime->dataLoaded = true;
    return true;
}

bool PoseAnimatorSystem::loadData(PoseAnimatorComponent& animator){
    if (animator.needsLoad){
        return startLoad(animator);
    }
    return animator.runtime && animator.runtime->dataLoaded;
}

bool PoseAnimatorSystem::buildRuntime(Entity entity, PoseAnimatorComponent& animator, bool& wait){
    wait = false;
    pose::PoseAnimatorRuntime& runtime = *animator.runtime;

    ModelComponent* model = scene->findComponent<ModelComponent>(entity);
    if (!model){
        return fail(animator, "'" + scene->getEntityName(entity) + "' has no Model: a Pose Animator plays on the Model of its GameObject");
    }
    // models load asynchronously: retried every frame until the file and its bones are there
    if (model->needUpdateModel || model->filename.empty()){
        wait = true;
        return false;
    }
    if (model->loadedFilename.empty()){
        return fail(animator, "the model of '" + scene->getEntityName(entity) + "' did not load");
    }
    if (model->bonesNameMapping.empty()){
        return fail(animator, "the model '" + model->filename + "' has no skeleton");
    }

    runtime.model = entity;
    runtime.modelFile = model->loadedFilename;

    pose::ModelRigInfo rigInfo;
    std::vector<Entity> boneEntities;
    std::vector<Matrix4> bridges;
    std::string error;
    if (!pose::buildModelRig(scene, entity, rigInfo, boneEntities, &bridges, &error)){
        return fail(animator, "'" + scene->getEntityName(entity) + "': " + error);
    }
    if (!runtime.build(rigInfo.rig, &error, &pose::PoseBuildCache::shared())){
        return fail(animator, animator.animation + ": " + error);
    }

    for (int b = 0; b < pose::HUMAN_BONE_COUNT; b++){
        const int bone = runtime.rig->boneIndex(pose::boneAt(b));
        runtime.slotEntities[b] = (bone >= 0 && bone < (int)boneEntities.size()) ? boneEntities[bone] : NULL_ENTITY;
        runtime.bridgeRotationInverse[b] = Quaternion();
        if (bone >= 0 && bone < (int)bridges.size()){
            Vector3 position, scale;
            Quaternion rotation;
            bridges[bone].decompose(position, scale, rotation);
            runtime.bridgeRotationInverse[b] = rotation.inverse();
            if (b == pose::boneIndex(pose::HumanBone::Hips)){
                runtime.hipsBridgeInverse = bridges[bone].inverse();
            }
        }
    }
    // bones of another model file (reloaded): those get their base back, the others keep blending
    restoreBones(runtime, true);
    return true;
}

bool PoseAnimatorSystem::bonesAlive(const pose::PoseAnimatorRuntime& runtime) const{
    for (Entity bone : runtime.slotEntities){
        if (bone != NULL_ENTITY && !scene->isEntityCreated(bone)) return false;
    }
    return true;
}

void PoseAnimatorSystem::restoreBones(pose::PoseAnimatorRuntime& runtime, bool onlyStale){
    for (int b = 0; b < pose::HUMAN_BONE_COUNT; b++){
        pose::PoseBlendBone& blend = runtime.blend[b];
        const Entity bone = runtime.blendEntities[b];
        if (onlyStale && bone == runtime.slotEntities[b]) continue;
        if (bone != NULL_ENTITY && scene->isEntityCreated(bone)){
            if (Transform* transform = scene->findComponent<Transform>(bone)){
                bool changed = false;
                // what the animator wrote is still there: nobody (no clip) wrote the bone since
                if (blend.rotationActive && transform->rotation == blend.writtenRotation){
                    transform->rotation = blend.baseRotation;
                    changed = true;
                }
                if (blend.positionActive && transform->position == blend.writtenPosition){
                    transform->position = blend.basePosition;
                    changed = true;
                }
                if (changed) transform->needUpdate = true;
            }
        }
        blend.reset();
        runtime.blendEntities[b] = NULL_ENTITY;
    }
}

void PoseAnimatorSystem::onComponentRemoved(Entity entity, ComponentId componentId){
    if (componentId != scene->getComponentId<PoseAnimatorComponent>()) return;
    PoseAnimatorComponent* animator = scene->findComponent<PoseAnimatorComponent>(entity);
    if (animator && animator->runtime){
        restoreBones(*animator->runtime);
    }
}

void PoseAnimatorSystem::applyPose(pose::PoseAnimatorRuntime& runtime, const pose::HumanPose& pose, float weight){
    for (int b = 0; b < pose::HUMAN_BONE_COUNT; b++){
        const Entity bone = runtime.slotEntities[b];
        if (bone == NULL_ENTITY) continue;
        Transform* transform = scene->findComponent<Transform>(bone);
        if (!transform) continue;

        pose::PoseBlendBone& blend = runtime.blend[b];
        // weight 0 from the start: the bones were never touched, leave them
        if (weight <= 0.0f && !blend.rotationActive) continue;

        // skeleton local = bridge * Transform local
        const Quaternion rotation = runtime.bridgeRotationInverse[b] * pose.rotations[b];
        transform->rotation = blend.blendRotation(transform->rotation, rotation, weight);
        if (b == pose::boneIndex(pose::HumanBone::Hips)){
            const Vector3 position = runtime.hipsBridgeInverse * pose.hipsPosition;
            transform->position = blend.blendPosition(transform->position, position, weight);
        }
        transform->needUpdate = true;
        runtime.blendEntities[b] = bone;
    }
}

bool PoseAnimatorSystem::goToPose(PoseAnimatorComponent& animator, const pose::PoseGoTo& request){
    if (!loadData(animator)) return false;
    pose::PoseAnimatorRuntime& runtime = *animator.runtime;
    if (request.key < 0 || request.key >= runtime.poseCount()) return false;

    animator.started = true;
    animator.playing = false;
    if (!runtime.isLoaded()){
        runtime.pending = request;
        return true;
    }
    return runtime.startTransition(request);
}

void PoseAnimatorSystem::resetAnimators(){
    auto animators = scene->getComponentArray<PoseAnimatorComponent>();
    for (size_t i = 0; i < animators->size(); i++){
        PoseAnimatorComponent& animator = animators->getComponentFromIndex(i);
        animator.playing = false;
        animator.time = 0;
        animator.started = false;
        animator.needsLoad = true;
        animator.runtime.reset();
        // play's scripts and C++ libraries are gone after it: their callbacks with them
        animator.onFinish.clear();
    }
}

void PoseAnimatorSystem::load(){
}

void PoseAnimatorSystem::draw(){
}

void PoseAnimatorSystem::destroy(){
    // a scene goes: the shared rigs and builds of model data already freed go (this scene's own
    // on the next prune or get, once its components are gone; the editor prunes after a scene closes)
    pose::PoseBuildCache::shared().prune();
    pose::PoseRigCache::shared().prune();
}

void PoseAnimatorSystem::update(double dt){
    if (paused){
        return;
    }

    std::vector<Entity> finished;

    auto animators = scene->getComponentArray<PoseAnimatorComponent>();
    for (size_t i = 0; i < animators->size(); i++){
        PoseAnimatorComponent& animator = animators->getComponentFromIndex(i);
        const Entity entity = animators->getEntity(i);

        if (!scene->isEntityActiveInHierarchy(entity)){
            continue;
        }

        // non-finite fields (Inspector, MCP, a script's arithmetic) neither move nor drive the pose
        const float speed = std::isfinite(animator.speed) ? animator.speed : 0.0f;
        const float weight = std::isfinite(animator.weight) ? animator.weight : 0.0f;
        if (!std::isfinite(animator.time)){
            animator.time = 0.0f;
        }

        if (!animator.started){
            animator.playing = animator.autoplay;
            animator.started = true;
        }

        // a failed animator tries its file again now and then: fixed (written, re-set to the same
        // path, a sidecar corrected), it plays; still broken, the same error is not logged again
        if (!animator.needsLoad && animator.runtime && animator.runtime->failed && !animator.animation.empty()){
            animator.runtime->retryTimer += (float)dt;
            if (animator.runtime->retryTimer >= FAILED_RETRY_SECONDS){
                animator.runtime->retryTimer = 0.0f;
                animator.needsLoad = true;
                animator.runtime->loading = false;
            }
        }

        if (animator.needsLoad){
            if (!startLoad(animator)){
                continue;
            }
            bool wait = false;
            if (!buildRuntime(entity, animator, wait)){
                continue;
            }
            animator.runtime->loading = false;
            animator.needsLoad = false;
            pose::PoseAnimatorRuntime& loaded = *animator.runtime;
            if (loaded.pending.key >= 0){
                loaded.startPending();
            }else if (animator.playing && speed < 0.0f && !loaded.data.settings.loop && animator.time <= 0.0f){
                // a one-shot played backwards starts at its end
                animator.time = loaded.length;
            }else if (!animator.playing && animator.time == 0){
                // not playing (no autoplay, or stopped before the load): key pose 0, as stop shows it
                loaded.transitionActive = true;
                loaded.transitionArrived = true;
                loaded.transitionKey = 0;
            }
        }

        if (!animator.runtime || !animator.runtime->isLoaded()){
            continue;
        }
        pose::PoseAnimatorRuntime& runtime = *animator.runtime;

        // the model reloaded under us (file changed): build again on its new bones
        const ModelComponent* model = scene->findComponent<ModelComponent>(entity);
        if (!bonesAlive(runtime) || (model && !model->needUpdateModel && model->loadedFilename != runtime.modelFile)){
            animator.needsLoad = true;
            continue;
        }

        // a hit-stop freezes it, unless its GameObject is Hit Stop Exempt (S48)
        const float step = (float)pixelart::HitStop::deltaFor(scene, entity, dt);
        runtime.poseFrameRate = animator.poseFrameRate;
        pose::HumanPose shown = runtime.hasDisplayed ? runtime.displayed : runtime.frames()[0];
        if (runtime.transitionActive){
            runtime.sampleTransition(runtime.transitionPaused ? 0.0f : step * std::abs(speed), shown);
        }else{
            if (pose::advancePlayback(animator.time, animator.playing, step, speed,
                                      runtime.length, runtime.data.settings.loop)){
                finished.push_back(entity);
            }
            // S48 Pose Frame Rate: each pose held on whole steps counted from its key
            runtime.samplePlayback(animator.time, shown, animator.poseFrameRate);
        }
        runtime.displayed = shown;
        runtime.hasDisplayed = true;

        applyPose(runtime, shown, weight);
    }

    // user code last: it may add or remove components (and move the array under the loop)
    for (Entity entity : finished){
        if (PoseAnimatorComponent* animator = scene->findComponent<PoseAnimatorComponent>(entity)){
            animator->onFinish.call();
        }
    }
}
