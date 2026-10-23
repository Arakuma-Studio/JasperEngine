// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PoseAnimatorSystem.h"

#include "Scene.h"
#include "Log.h"
#include "component/ModelComponent.h"
#include "component/Transform.h"
#include "pose/ModelRig.h"
#include "pose/PoseFile.h"

#include <cmath>
#include <vector>

using namespace jasperengine;

PoseAnimatorSystem::PoseAnimatorSystem(Scene* scene): SubSystem(scene){
    signature.set(scene->getComponentId<PoseAnimatorComponent>());
}

bool PoseAnimatorSystem::fail(PoseAnimatorComponent& animator, const std::string& message){
    pose::PoseAnimatorRuntime& runtime = *animator.runtime;
    runtime.failed = true;
    runtime.loading = false;
    runtime.error = message;
    animator.needsLoad = false;
    Log::error("Pose Animator: %s", message.c_str());
    return false;
}

bool PoseAnimatorSystem::startLoad(PoseAnimatorComponent& animator){
    if (animator.runtime && animator.runtime->loading && animator.runtime->path == animator.animation){
        return animator.runtime->dataLoaded;
    }

    auto runtime = std::make_shared<pose::PoseAnimatorRuntime>();
    // the bones keep what the previous runtime wrote: its blend state says what is under it
    if (animator.runtime){
        runtime->blend = animator.runtime->blend;
    }
    runtime->path = animator.animation;
    runtime->loading = true;
    animator.runtime = runtime;

    if (animator.animation.empty()){
        // nothing to play, not an error
        runtime->loading = false;
        animator.needsLoad = false;
        return false;
    }

    std::vector<std::string> warnings;
    std::string error;
    if (!pose::loadPoseAnimationFile(animator.animation, runtime->data, warnings, &error)){
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
    runtime.modelFile = model->filename;

    pose::ModelRigInfo rigInfo;
    std::vector<Entity> boneEntities;
    std::vector<Matrix4> bridges;
    std::string error;
    if (!pose::buildModelRig(scene, entity, rigInfo, boneEntities, &bridges, &error)){
        return fail(animator, "'" + scene->getEntityName(entity) + "': " + error);
    }
    if (!runtime.build(rigInfo.rig, &error)){
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
    return true;
}

bool PoseAnimatorSystem::bonesAlive(const pose::PoseAnimatorRuntime& runtime) const{
    for (Entity bone : runtime.slotEntities){
        if (bone != NULL_ENTITY && !scene->isEntityCreated(bone)) return false;
    }
    return true;
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
    }
}

void PoseAnimatorSystem::load(){
}

void PoseAnimatorSystem::draw(){
}

void PoseAnimatorSystem::destroy(){
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

        if (!animator.started){
            animator.playing = animator.autoplay;
            animator.started = true;
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
                loaded.startTransition(loaded.pending);
                loaded.pending = pose::PoseGoTo();
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
        if (!bonesAlive(runtime)){
            animator.needsLoad = true;
            continue;
        }

        pose::HumanPose shown = runtime.hasDisplayed ? runtime.displayed : runtime.frames[0];
        if (runtime.transitionActive){
            runtime.sampleTransition((float)dt * std::abs(animator.speed), shown);
        }else{
            if (pose::advancePlayback(animator.time, animator.playing, (float)dt, animator.speed,
                                      runtime.length, runtime.data.settings.loop)){
                finished.push_back(entity);
            }
            runtime.samplePlayback(animator.time, shown);
        }
        runtime.displayed = shown;
        runtime.hasDisplayed = true;

        applyPose(runtime, shown, animator.weight);
    }

    // user code last: it may add or remove components (and move the array under the loop)
    for (Entity entity : finished){
        if (PoseAnimatorComponent* animator = scene->findComponent<PoseAnimatorComponent>(entity)){
            animator->onFinish.call();
        }
    }
}
