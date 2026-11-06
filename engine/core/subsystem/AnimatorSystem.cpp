// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AnimatorSystem.h"
#include "audio/AudioMarkers.h"
#include "audio/TimelineAudio.h"
#include "component/CharacterControllerComponent.h"

#include "Engine.h"
#include "Log.h"
#include "Scene.h"
#include "animation/AnimationClipIO.h"
#include "animation/ClipBinding.h"
#include "animation/CompiledController.h"
#include "component/AnimationPlayerComponent.h"
#include "component/CameraComponent.h"
#include "component/LatticeComponent.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "component/Transform.h"
#include "jobs/JobSystem.h"
#include "lattice/LatticeSolver.h"
#include "pool/AnimationClipPool.h"
#include "pool/AnimatorAssetPool.h"
#include "profiler/Profile.h"
#include "registry/PhysicsLayers.h"
#include "script/LuaBinding.h"
#include "subsystem/PhysicsSystem.h"
#include "subsystem/RenderSystem.h"

#include <algorithm>
#include <chrono>
#include <cmath>

using namespace jasperengine;

namespace{

    constexpr float kRetrySeconds = 1.0f;
    constexpr float kRebindSeconds = 0.25f;
    constexpr float kMaxRootMotionPerFrame = 50.0f;
    constexpr size_t kAnimatorsPerJob = 4;

    double nowMs(){
        using namespace std::chrono;
        return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
    }

    Matrix4 toMatrix(const anim::Affine& a){
        Matrix4 m;
        m[0][0] = a.m[0]; m[0][1] = a.m[1]; m[0][2] = a.m[2]; m[0][3] = 0.0f;
        m[1][0] = a.m[3]; m[1][1] = a.m[4]; m[1][2] = a.m[5]; m[1][3] = 0.0f;
        m[2][0] = a.m[6]; m[2][1] = a.m[7]; m[2][2] = a.m[8]; m[2][3] = 0.0f;
        m[3][0] = a.m[9]; m[3][1] = a.m[10]; m[3][2] = a.m[11]; m[3][3] = 1.0f;
        return m;
    }

    void collectModels(Scene* scene, Entity entity, std::vector<std::pair<Entity, std::string>>& out, int depth){
        if (depth > 256) return;
        if (ModelComponent* model = scene->findComponent<ModelComponent>(entity)) out.push_back({entity, model->loadedFilename});
        for (Entity child : anim::getChildren(scene, entity)) collectModels(scene, child, out, depth + 1);
    }

    const std::string& noName(){
        static const std::string empty;
        return empty;
    }

    const std::string& stateName(const anim::AnimatorInstance& instance, int state){
        if (!instance.controller || state < 0 || (size_t)state >= instance.controller->states.size()) return noName();
        return instance.controller->states[(size_t)state].fullPath;
    }

    const std::string& layerName(const anim::AnimatorInstance& instance, int layer){
        if (!instance.controller || layer < 0 || (size_t)layer >= instance.controller->layers.size()) return noName();
        return instance.controller->layers[(size_t)layer].name;
    }

}

AnimatorSystem::AnimatorSystem(Scene* scene): SubSystem(scene){
    signature.set(scene->getComponentId<AnimatorComponent>());
    declareHookType(scene->getComponentId<AnimatorComponent>());
}

void AnimatorSystem::load(){
}

void AnimatorSystem::draw(){
}

void AnimatorSystem::destroy(){
    auto animators = scene->getComponentArray<AnimatorComponent>();
    for (size_t i = 0; i < animators->size(); i++){
        AnimatorComponent& component = animators->getComponentFromIndex(i);
        if (component.runtime) release(animators->getEntity(i), *component.runtime);
        component.runtime.reset();
        component.needsLoad = true;
    }
    owners.clear();
}

void AnimatorSystem::onComponentRemoved(Entity entity, ComponentId componentId){
    if (componentId != scene->getComponentId<AnimatorComponent>()) return;
    AnimatorComponent* component = scene->findComponent<AnimatorComponent>(entity);
    if (component && component->runtime) release(entity, *component->runtime);
}

void AnimatorSystem::setParallel(bool value){
    parallel = value;
}

void AnimatorSystem::release(Entity entity, anim::AnimatorInstance& instance){
    setSkinsDriven(instance, false);
    for (auto it = owners.begin(); it != owners.end();){
        if (it->second == entity) it = owners.erase(it);
        else ++it;
    }
}

void AnimatorSystem::setSkinsDriven(anim::AnimatorInstance& instance, bool driven){
    for (const anim::RigSkin& skin : instance.rig.skins){
        ModelComponent* model = scene->isEntityCreated(skin.model) ? scene->findComponent<ModelComponent>(skin.model) : nullptr;
        if (model && skin.binding < model->skinBindings.size()) model->skinBindings[skin.binding].animatorDriven = driven;
        // RenderSystem computes these again from the bone transforms
        if (!driven){
            if (MeshComponent* mesh = scene->isEntityCreated(skin.mesh) ? scene->findComponent<MeshComponent>(skin.mesh) : nullptr){
                if (Transform* transform = scene->findComponent<Transform>(skin.mesh)) transform->needUpdate = true;
                mesh->needUpdateBones = true;
            }
        }
    }
}

std::string AnimatorSystem::modelPathOf(Entity entity) const{
    if (ModelComponent* model = scene->findComponent<ModelComponent>(entity)) return model->filename.path;
    std::vector<std::pair<Entity, std::string>> models;
    collectModels(scene, entity, models, 0);
    for (const auto& entry : models){
        if (ModelComponent* model = scene->findComponent<ModelComponent>(entry.first)) return model->filename.path;
    }
    return std::string();
}

bool AnimatorSystem::modelsLoading(Entity entity) const{
    std::vector<std::pair<Entity, std::string>> models;
    collectModels(scene, entity, models, 0);
    for (const auto& entry : models){
        ModelComponent* model = scene->findComponent<ModelComponent>(entry.first);
        if (model && model->needUpdateModel) return true;
    }
    return false;
}

anim::AnimatorInstance* AnimatorSystem::instance(Entity entity){
    AnimatorComponent* component = scene->findComponent<AnimatorComponent>(entity);
    if (!component) return nullptr;
    if (!component->runtime) component->runtime = std::make_shared<anim::AnimatorInstance>();
    anim::AnimatorInstance& runtime = *component->runtime;
    // a controller edited since (its pool entry reloaded or dropped) loads again
    if (component->needsLoad || !runtime.controller || AnimatorAssetPool::getGeneration(component->controller.path) != runtime.controllerGeneration){
        loadController(entity, *component, runtime);
    }
    return &runtime;
}

bool AnimatorSystem::loadController(Entity entity, AnimatorComponent& component, anim::AnimatorInstance& instance){
    const std::string& path = component.controller.path;
    instance.controllerPath = path;
    if (path.empty()){
        if (instance.controller){
            release(entity, instance);
            instance.setController(nullptr, 0, component.clipOverrides);
        }
        component.needsLoad = false;
        return false;
    }
    std::string error;
    uint64_t generation = 0;
    std::shared_ptr<const anim::CompiledController> compiled = AnimatorAssetPool::controller(path, &error, &generation);
    if (!compiled){
        if (instance.controllerError != error){
            instance.controllerError = error;
            Log::error("Animator '%s': cannot load the controller %s: %s", scene->getEntityName(entity).c_str(), path.c_str(), error.c_str());
        }
        instance.rebindRetry = instance.clock + kRetrySeconds;
        return false;
    }
    instance.controllerError.clear();
    component.needsLoad = false;
    if (compiled != instance.controller || generation != instance.controllerGeneration){
        if (instance.bound) release(entity, instance);
        instance.setController(compiled, generation, component.clipOverrides);
    }
    return true;
}

void AnimatorSystem::pollClips(Entity entity, AnimatorComponent& component, anim::AnimatorInstance& instance, bool& rebind){
    for (size_t i = 0; i < instance.clips.size(); i++){
        anim::AnimatorClipSlot& slot = instance.clips[i];
        if (slot.path.empty()) continue;
        bool fetch = !slot.clip;
        if (slot.clip && revisionChanged && AnimationClipPool::getGeneration(slot.path) != slot.generation) fetch = true;
        if (fetch && slot.failed && instance.clock < slot.retry) fetch = false;
        if (!fetch) continue;
        bool loading = false;
        std::string error;
        std::shared_ptr<const AnimationClip> clip = AnimationClipPool::pollOrLoad(slot.path, loading, &error);
        if (clip){
            const bool changed = clip != slot.clip;
            slot.clip = clip;
            slot.generation = AnimationClipPool::getGeneration(slot.path);
            slot.failed = false;
            slot.reported = false;
            if (changed){
                // a montage clip whose tracks are all in the rig binds alone, anything else rebinds
                bool inRig = instance.bound && i >= instance.controllerClipCount;
                if (inRig){
                    for (const AnimationClip::Track& track : clip->getTracks()){
                        if (track.isSampled() && track.channel <= ClipChannel::Scale && instance.rig.findBone(track.path) < 0){
                            inRig = false;
                            break;
                        }
                        if (track.channel == ClipChannel::Morph || track.channel == ClipChannel::Lattice ||
                            track.channel == ClipChannel::Property) inRig = false;
                    }
                }
                if (inRig) instance.bindClip(i, component);
                else rebind = true;
            }
        }else if (!loading){
            slot.failed = true;
            slot.retry = instance.clock + kRetrySeconds;
            if (!slot.reported){
                slot.reported = true;
                slot.error = error;
                Log::error("Animator '%s': cannot load the clip %s: %s", scene->getEntityName(entity).c_str(), slot.path.c_str(), error.c_str());
            }
        }
    }
}

bool AnimatorSystem::ensureBound(Entity entity, AnimatorComponent& component, anim::AnimatorInstance& instance){
    if (!instance.controller) return false;
    // a failed bind (another Animator owned an entity) is tried again once a second
    bool rebind = !instance.bound && (!instance.bindFailed || instance.clock >= instance.rebindRetry);
    // models: a load in progress waits, another loaded file rebinds
    for (const auto& entry : instance.models){
        ModelComponent* model = scene->isEntityCreated(entry.first) ? scene->findComponent<ModelComponent>(entry.first) : nullptr;
        if (!model){
            rebind = true;
            continue;
        }
        if (model->needUpdateModel) return false;
        if (model->loadedFilename != entry.second) rebind = true;
    }
    pollClips(entity, component, instance, rebind);
    if (instance.bound && scene->hierarchyRevision() != instance.hierarchyRevision){
        if (!instance.rig.stillValid(scene)){
            rebind = true;
        }else if (instance.rig.unresolved > 0 && instance.clock >= instance.rebindRetry){
            rebind = true;
            instance.rebindRetry = instance.clock + kRebindSeconds;
        }else{
            instance.hierarchyRevision = scene->hierarchyRevision();
        }
    }
    if (!rebind && !revisionChanged) return instance.bound;
    // masks (an edited file rebinds)
    std::vector<std::shared_ptr<const anim::AvatarMaskDef>> masks(instance.controller->layers.size());
    for (size_t l = 0; l < masks.size(); l++){
        const std::string& path = instance.controller->layers[l].mask.path;
        if (path.empty()) continue;
        std::string error;
        masks[l] = AnimatorAssetPool::mask(path, &error);
        if (!masks[l] && instance.warnOnce("mask:" + path)){
            Log::error("Animator '%s': cannot load the mask %s: %s", scene->getEntityName(entity).c_str(), path.c_str(), error.c_str());
        }
    }
    if (revisionChanged && instance.bound){
        for (size_t l = 0; l < masks.size() && !rebind; l++){
            const uint64_t generation = masks[l] ? AnimatorAssetPool::getGeneration(instance.controller->layers[l].mask.path) : 0;
            if (l >= instance.maskGenerations.size() || generation != instance.maskGenerations[l]) rebind = true;
        }
    }
    if (!rebind) return instance.bound;

    if (modelsLoading(entity)) return false;
    release(entity, instance);
    instance.models.clear();
    collectModels(scene, entity, instance.models, 0);
    instance.modelPath = modelPathOf(entity);
    instance.bind(scene, entity, component, masks);
    instance.bindFailed = false;
    instance.maskGenerations.assign(masks.size(), 0);
    for (size_t l = 0; l < masks.size(); l++){
        if (masks[l]) instance.maskGenerations[l] = AnimatorAssetPool::getGeneration(instance.controller->layers[l].mask.path);
    }
    // the entities it writes are its own
    for (const anim::RigBone& bone : instance.rig.bones){
        auto it = owners.find(bone.entity);
        if (it != owners.end() && it->second != entity && scene->isEntityCreated(it->second) &&
                scene->findComponent<AnimatorComponent>(it->second)){
            instance.bound = false;
            instance.bindFailed = true;
            instance.rebindRetry = instance.clock + kRetrySeconds;
            Log::error("Animator '%s': '%s' is already animated by the Animator of '%s': this Animator drives nothing",
                       scene->getEntityName(entity).c_str(), scene->getEntityName(bone.entity).c_str(), scene->getEntityName(it->second).c_str());
            return false;
        }
    }
    for (const anim::RigBone& bone : instance.rig.bones) owners[bone.entity] = entity;
    setSkinsDriven(instance, true);
    return true;
}

bool AnimatorSystem::visible(const anim::AnimatorInstance& instance) const{
    if (instance.rig.skins.empty()) return true;
    auto cameras = scene->getComponentArray<CameraComponent>();
    RenderSystem* render = scene->getSystem<RenderSystem>().get();
    for (const anim::RigSkin& skin : instance.rig.skins){
        MeshComponent* mesh = scene->findComponent<MeshComponent>(skin.mesh);
        if (!mesh) continue;
        for (size_t c = 0; c < cameras->size(); c++){
            CameraComponent& camera = cameras->getComponentFromIndex(c);
            if (!camera.enabled) continue;
            if (render->isInsideCamera(camera, mesh->worldAABB)) return true;
        }
    }
    return false;
}

void AnimatorSystem::ikInputs(AnimatorComponent& component, anim::AnimatorInstance& instance, const anim::AnimatorUpdateContext& context){
    if (instance.ik.size() != component.ik.size()) return;
    for (size_t i = 0; i < component.ik.size(); i++){
        const AnimatorIKConstraint& c = component.ik[i];
        anim::AnimatorIKRun& run = instance.ik[i];
        run.hasTarget = false;
        run.hasTargetRotation = false;
        if (c.target != NULL_ENTITY && scene->isEntityCreated(c.target)){
            if (Transform* target = scene->findComponent<Transform>(c.target)){
                run.hasTarget = true;
                run.targetWorld = target->worldPosition;
                run.hasTargetRotation = true;
                run.targetRotationWorld = target->worldRotation;
            }
        }else if (c.type != AnimatorIKType::FootIK && c.type != AnimatorIKType::FootLock){
            run.hasTarget = true;
            run.targetWorld = c.targetPosition;
        }
        run.hasPole = false;
        if (c.pole != NULL_ENTITY && scene->isEntityCreated(c.pole)){
            if (Transform* pole = scene->findComponent<Transform>(c.pole)){
                run.hasPole = true;
                run.poleWorld = pole->worldPosition;
            }
        }else if (c.usePole){
            run.hasPole = true;
            run.poleWorld = c.polePosition;
        }
        if (c.type == AnimatorIKType::FootIK){
            run.hit = false;
#ifdef JASPERENGINE_PHYSICS_3D
            if (run.hasFoot && c.enabled){
                const Vector3 up = context.rootWorldRotation * Vector3(0, 1, 0);
                const Vector3 origin = run.footWorld + up * c.rayStart;
                RayReturn hit = Physics::raycast(scene, origin, up * -1.0f, c.rayStart + c.rayLength, c.layerMask);
                if (hit.hit){
                    run.hit = true;
                    run.hitPoint = context.rootWorldInverse * hit.point;
                    run.hitNormal = (context.rootWorldRotation.inverse() * hit.normal).normalized();
                }
            }
#else
            (void)context;
#endif
        }
    }
}

bool AnimatorSystem::prepare(Entity entity, AnimatorComponent& component, float dt, Job& job){
    if (!component.enabled || !scene->isEntityActiveInHierarchy(entity)) return false;
    if (!component.runtime) component.runtime = std::make_shared<anim::AnimatorInstance>();
    anim::AnimatorInstance& instance = *component.runtime;
    if (component.needsLoad || component.controller.path != instance.controllerPath || (revisionChanged && instance.controller &&
            AnimatorAssetPool::getGeneration(component.controller.path) != instance.controllerGeneration)){
        const bool settings = component.needsLoad;
        if ((instance.clock >= instance.rebindRetry || instance.controllerError.empty()) &&
                loadController(entity, component, instance) && settings){
            // a setting that binds changed (overrides, IK, written bones): bound again, states kept
            instance.applyOverrides(component.clipOverrides);
            if (instance.bound) release(entity, instance);
            instance.bound = false;
            instance.bindFailed = false;
        }
    }
    if (!instance.controller){
        instance.clock += std::fabs(dt);
        return false;
    }
    if (!ensureBound(entity, component, instance)){
        instance.clock += std::fabs(dt);
        return false;
    }
    job.entity = entity;
    job.component = &component;
    job.instance = &instance;
    job.context.dt = dt;
    job.context.sample = true;
    if (component.cullingMode != AnimatorCullingMode::AlwaysAnimate && !visible(instance)){
        if (component.cullingMode == AnimatorCullingMode::CullCompletely){
            stats.culled++;
            return false;
        }
        job.context.sample = false;
        stats.culled++;
    }
    if (Transform* transform = scene->findComponent<Transform>(entity)){
        job.context.rootWorld = transform->modelMatrix;
        job.context.rootWorldInverse = transform->modelMatrix.inverse();
        job.context.rootWorldRotation = transform->worldRotation;
    }
    ikInputs(component, instance, job.context);
    return true;
}

void AnimatorSystem::writePose(const AnimatorComponent& component, anim::AnimatorInstance& instance){
    const anim::AnimatorRig& rig = instance.rig;
    const anim::Pose& pose = instance.pose();
    for (size_t b = 0; b < rig.bones.size(); b++){
        if (!instance.writeMask[b]) continue;
        // the root's own track yields to root motion
        if (rig.rootBone && b == 0 && component.applyRootMotion) continue;
        Transform* transform = scene->findComponent<Transform>(rig.bones[b].entity);
        if (!transform) continue;
        const Vector3 position = pose.position(b);
        const Quaternion rotation = pose.rotation(b);
        const Vector3 scale = pose.scale(b);
        if (transform->position != position || transform->rotation != rotation || transform->scale != scale){
            transform->position = position;
            transform->rotation = rotation;
            transform->scale = scale;
            transform->needUpdate = true;
        }
    }
    const float* channels = pose.channelData();
    for (size_t c = 0; c < rig.channels.size(); c++){
        const anim::RigChannel& channel = rig.channels[c];
        if (channel.kind == ClipChannel::Morph){
            if (MeshComponent* mesh = scene->findComponent<MeshComponent>(channel.entity)){
                if (channel.index < MAX_MORPHTARGETS) mesh->morphWeights[channel.index] = channels[c];
            }
        }else if (channel.kind == ClipChannel::Lattice && channel.axis == 0){
            LatticeComponent* lattice = scene->findComponent<LatticeComponent>(channel.entity);
            if (!lattice) continue;
            const size_t count = (size_t)lattice::pointCount(lattice->resolutionX, lattice->resolutionY, lattice->resolutionZ);
            if ((size_t)channel.index >= count) continue;
            if (lattice->offsets.size() < count) lattice->offsets.resize(count, Vector3(0, 0, 0));
            const Vector3 value(channels[c], channels[c + 1], channels[c + 2]);
            if (lattice->offsets[(size_t)channel.index] != value){
                lattice->offsets[(size_t)channel.index] = value;
                lattice->revision++;
            }
        }
    }
    // properties (anim-properties): the blended key-space values, a discrete one rounded (the
    // heavier side of a two-way blend), written when they changed; deferred rows (GameObject
    // active, a script's enabled: user callbacks) after every other write
    for (int pass = 0; pass < 2; pass++){
        for (const anim::RigProperty& property : rig.properties){
            if (property.binding.deferred != (pass == 1)) continue;
            const size_t width = property.binding.width;
            if (width == 0 || width > 16 || !scene->isEntityCreated(property.entity)) continue;
            if (anim::scriptBindingStale(scene, property.entity, property.binding)) continue;
            float values[16];
            float current[16];
            for (size_t k = 0; k < width; k++){
                const float value = channels[(size_t)property.firstChannel + k];
                values[k] = property.binding.discrete ? std::floor(value + 0.5f) : value;
            }
            if (anim::readPropertyKeySpace(scene, property.entity, property.binding, current) &&
                std::equal(values, values + width, current)) continue;
            anim::writePropertyKeySpace(scene, property.entity, property.binding, values);
        }
    }
    // skin matrices: inv(mesh in rig space) * joint in rig space * inverse bind (D7)
    const std::vector<anim::Affine>& world = instance.rigWorld();
    for (const anim::RigSkin& skin : rig.skins){
        MeshComponent* mesh = scene->findComponent<MeshComponent>(skin.mesh);
        if (!mesh) continue;
        anim::Affine meshSpace;
        if (skin.meshBone >= 0){
            meshSpace = world[(size_t)skin.meshBone];
        }else{
            meshSpace = skin.meshParentBone >= 0 ? world[(size_t)skin.meshParentBone] : anim::Affine();
            for (auto it = skin.meshChain.rbegin(); it != skin.meshChain.rend(); ++it){
                Transform* transform = scene->findComponent<Transform>(*it);
                if (transform) meshSpace = meshSpace * anim::Affine::compose(transform->position, transform->rotation, transform->scale);
            }
        }
        const Matrix4 inverseMesh = toMatrix(meshSpace.inverse());
        const size_t count = std::min(skin.joints.size(), skin.inverseBindMatrices.size());
        for (size_t j = 0; j < count && mesh->bonesMatrix.validIndex((int)j); j++){
            mesh->bonesMatrix[j] = inverseMesh * toMatrix(world[(size_t)skin.joints[j]]) * skin.inverseBindMatrices[j];
        }
        mesh->needUpdateBones = true;
        mesh->needUpdateAABB = true;
    }
}

void AnimatorSystem::fireCues(Entity entity, anim::AnimatorInstance& instance){
#if JASPERENGINE_SOUND_LAYER
    const anim::AnimatorOutput& output = instance.output;
    for (const anim::AnimatorPendingCue& pending : output.cues){
        if (pending.clip < 0 || (size_t)pending.clip >= instance.clips.size()) continue;
        const anim::AnimatorClipSlot& slot = instance.clips[(size_t)pending.clip];
        if (!slot.clip || pending.index < 0 || (size_t)pending.index >= slot.clip->getAudioCues().size()) continue;
        TimelineCueInfo info;
        info.clip = slot.path;
        info.cue = pending.index;
        info.scene = scene;
        info.player = entity;
        info.state = pending.montage >= 0 && (size_t)pending.montage < instance.montages.size() ? instance.montages[(size_t)pending.montage].path
                                                                                                : stateName(instance, pending.state);
        info.layer = pending.layer;
        info.position = pending.position;
        const TimelineVoice voice = TimelineAudio::fireCue(scene, entity, slot.clip->getAudioCues()[(size_t)pending.index],
                                                           TimelineAudio::delayFrames(pending.fraction), pending.speed, info);
        if (!voice.empty()) instance.cueVoices.push_back({pending.layer, pending.state, pending.montage, voice.id, voice.generation, voice.source});
    }
    // state_exit (R7): a state's exit note (it was left, or its crossfade out ended), a montage's end
    for (size_t i = 0; i < instance.cueVoices.size();){
        const anim::AnimatorCueVoice& voice = instance.cueVoices[i];
        bool exited = false;
        if (voice.montage >= 0){
            exited = (size_t)voice.montage >= instance.montages.size() ||
                     instance.montages[(size_t)voice.montage].phase == anim::AnimatorMontageRun::Phase::Ended;
        }else{
            for (const anim::AnimatorStateNote& note : stateNotes){
                if (!note.enter && note.layer == voice.layer && note.state == voice.state) exited = true;
            }
        }
        if (!exited){
            i++;
            continue;
        }
        TimelineVoice stopped;
        stopped.id = voice.id;
        stopped.generation = voice.generation;
        stopped.source = voice.source;
        TimelineAudio::stop(scene, stopped);
        instance.cueVoices.erase(instance.cueVoices.begin() + (std::ptrdiff_t)i);
    }
#else
    (void)entity;
    if (!instance.output.cues.empty() && !warnedNoSoundLayer){
        warnedNoSoundLayer = true;
        Log::error("Animator: clip audio cues need the soundlayer module, which this build leaves out");
    }
    instance.cueVoices.clear();
#endif
}

void AnimatorSystem::finalize(Job& job, float dt){
    const Entity entity = job.entity;
    // callbacks can remove the component or destroy the GameObject: looked up again each time
    std::shared_ptr<anim::AnimatorInstance> instance = job.component->runtime;
    if (!instance) return;
    anim::AnimatorOutput& output = instance->output;
    AnimatorComponent* component = job.component;

    // root motion
    if (output.hasRootMotion && component->applyRootMotion){
        Transform* transform = scene->findComponent<Transform>(entity);
        if (transform){
            const Vector3 local = transform->rotation * (transform->scale * output.rootDeltaPosition);
            const Quaternion turn(std::cos(0.5f * output.rootDeltaYaw), 0.0f, std::sin(0.5f * output.rootDeltaYaw), 0.0f);
            const Vector3 worldDelta = transform->worldRotation * (transform->worldScale * output.rootDeltaPosition);
            const Quaternion worldTurn = transform->worldRotation * turn * transform->worldRotation.inverse();
            if (local.length() > kMaxRootMotionPerFrame){
                if (instance->warnOnce("root motion jump")){
                    Log::warn("Animator '%s': root motion of %.1f m in one frame dropped", scene->getEntityName(entity).c_str(), local.length());
                }
            }else if (component->onAnimatorMove.hasSubscribers()){
                component->onAnimatorMove.call(worldDelta, worldTurn);
                component = scene->isEntityCreated(entity) ? scene->findComponent<AnimatorComponent>(entity) : nullptr;
                if (!component) return;
            }else{
#ifdef JASPERENGINE_PHYSICS_3D
                if (scene->findComponent<CharacterControllerComponent>(entity) && dt > 0.0f){
                    scene->getSystem<PhysicsSystem>()->setRootMotion(entity, worldDelta * (1.0f / dt), output.rootDeltaYaw / dt);
                }else
#endif
                {
                    transform->position = transform->position + local;
                    transform->rotation = (transform->rotation * turn).normalized();
                    transform->needUpdate = true;
                }
            }
        }
    }

    // state changes, montage ends, events (C++ and Lua subscribers, then the scripts' methods)
    // copied out first (a callback may play or stop states), buffers kept
    stateNotes.assign(output.states.begin(), output.states.end());
    output.states.clear();
    montageNotes.assign(output.montages.begin(), output.montages.end());
    output.montages.clear();
    for (const anim::AnimatorStateNote& note : stateNotes){
        component = scene->isEntityCreated(entity) ? scene->findComponent<AnimatorComponent>(entity) : nullptr;
        if (!component) return;
        if (!component->onStateEnter.hasSubscribers() && !component->onStateExit.hasSubscribers()) break;
        if (note.enter) component->onStateEnter.call(layerName(*instance, note.layer), stateName(*instance, note.state));
        else component->onStateExit.call(layerName(*instance, note.layer), stateName(*instance, note.state));
    }
    for (const anim::AnimatorMontageNote& note : montageNotes){
        component = scene->isEntityCreated(entity) ? scene->findComponent<AnimatorComponent>(entity) : nullptr;
        if (!component) return;
        if (note.montage < 0 || (size_t)note.montage >= instance->montages.size()) continue;
        const std::string path = instance->montages[(size_t)note.montage].path;
        component->onMontageEnded.call(path, note.interrupted);
    }
    // by layer, then where each was crossed: a stable insertion sort (std::stable_sort allocates)
    sortedEvents.assign(output.events.begin(), output.events.end());
    for (size_t i = 1; i < sortedEvents.size(); i++){
        const anim::AnimatorPendingEvent moving = sortedEvents[i];
        size_t j = i;
        while (j > 0 && (moving.layer < sortedEvents[j - 1].layer ||
                         (moving.layer == sortedEvents[j - 1].layer && moving.position < sortedEvents[j - 1].position))){
            sortedEvents[j] = sortedEvents[j - 1];
            j--;
        }
        sortedEvents[j] = moving;
    }
    for (const anim::AnimatorPendingEvent& pending : sortedEvents){
        component = scene->isEntityCreated(entity) ? scene->findComponent<AnimatorComponent>(entity) : nullptr;
        if (!component) return;
        const std::string* source = nullptr;
        if (pending.montageEvent >= 0){
            if (pending.montage < 0 || (size_t)pending.montage >= instance->montages.size()) continue;
            const anim::AnimatorMontageRun& montage = instance->montages[(size_t)pending.montage];
            if ((size_t)pending.montageEvent >= montage.montage->events.size()) continue;
            const anim::MontageEventDef& def = montage.montage->events[(size_t)pending.montageEvent];
            event.functionName = def.function;
            event.args = def.args;
            event.state = montage.path;
            event.time = def.time;
            source = &montage.path;
        }else{
            if (pending.clip < 0 || (size_t)pending.clip >= instance->clips.size()) continue;
            const anim::AnimatorClipSlot& slot = instance->clips[(size_t)pending.clip];
            if (!slot.clip || pending.index < 0 || (size_t)pending.index >= slot.clip->getEvents().size()) continue;
            const ClipEventData& def = slot.clip->getEvents()[(size_t)pending.index];
            event.functionName = def.function;
            event.args = def.args;
            event.state = pending.montage >= 0 && (size_t)pending.montage < instance->montages.size() ?
                          instance->montages[(size_t)pending.montage].path : stateName(*instance, pending.state);
            event.time = def.time;
            source = &slot.path;
        }
        AnimationPlayerComponent* player = scene->findComponent<AnimationPlayerComponent>(entity);
        // upstream's controller players (a disabled Animation Player beside the Animator) get
        // the controller's events too (object/AnimationPlayer.h)
        const bool forwarded = player && !player->enabled && player->onEvent.hasSubscribers();
        const bool subscribed = component->onEvent.hasSubscribers() || forwarded;
#if JASPERENGINE_AUDIO_CAPTURE
        if (AudioMarkers::isEnabled()) AudioMarkers::animationEvent(scene, entity, event.functionName, event.state, event.time);
#endif
        component->onEvent.call(event);
        if (forwarded){
            player = scene->isEntityCreated(entity) ? scene->findComponent<AnimationPlayerComponent>(entity) : nullptr;
            if (player) player->onEvent.call(event);
        }
        component = scene->isEntityCreated(entity) ? scene->findComponent<AnimatorComponent>(entity) : nullptr;
        if (!component) return;
        const bool scripted = component->sendEventsToScripts && LuaBinding::callLuaScriptMethod(scene, entity, event.functionName, event.args);
        // the warning's key hashed in place: a handled event (every frame of a game) builds no string
        uint64_t key = 1469598103934665603ull;
        const std::string* parts[2] = {source, &event.functionName};
        for (const std::string* part : parts){
            for (char c : *part) key = (key ^ (uint8_t)c) * 1099511628211ull;
            key = (key ^ 0xffu) * 1099511628211ull;
        }
        if (!subscribed && !scripted && instance->warnOnce(key)){
            Log::warn("Animator '%s': event '%s' of '%s' has no handler (no onEvent subscriber, no script method)",
                      scene->getEntityName(entity).c_str(), event.functionName.c_str(), source->c_str());
        }
    }
    if (!output.cues.empty() || !instance->cueVoices.empty()) fireCues(entity, *instance);
    // upstream's controller callbacks on the GameObject's Animation Player (layer index, state
    // name), after the update's events; a player that hands over to the Animator (disabled)
    // also calls its scripts' onStateEnter / onStateExit methods, as upstream's player did
    for (const anim::AnimatorStateNote& note : stateNotes){
        AnimationPlayerComponent* player = scene->isEntityCreated(entity) ? scene->findComponent<AnimationPlayerComponent>(entity) : nullptr;
        if (!player || !instance->controller || note.state < 0 || (size_t)note.state >= instance->controller->states.size()) break;
        const bool scripts = !player->enabled;
        if (!scripts && !player->onStateEnter.hasSubscribers() && !player->onStateExit.hasSubscribers()) break;
        const std::string name = instance->controller->states[(size_t)note.state].name;
        if (note.enter) player->onStateEnter.call(note.layer, name);
        else player->onStateExit.call(note.layer, name);
        if (scripts && scene->isEntityCreated(entity)){
            LuaBinding::callLuaScriptMethod(scene, entity, note.enter ? "onStateEnter" : "onStateExit",
                                            {AnimationEvent::numberArg(note.layer), AnimationEvent::stringArg(name)}, true);
        }
    }
    // ended montages go once their notes are out
    instance->montages.erase(std::remove_if(instance->montages.begin(), instance->montages.end(),
        [](const anim::AnimatorMontageRun& m){ return m.phase == anim::AnimatorMontageRun::Phase::Ended; }), instance->montages.end());
}

void AnimatorSystem::run(double dt, bool fixedPhase){
    auto animators = scene->getComponentArray<AnimatorComponent>();
    if (animators->size() == 0) return;
    stats = Stats();
    const double start = nowMs();
    {
        JE_PROFILE_SCOPE("Anim.Prepare");
        const uint64_t clipPool = AnimationClipPool::revision();
        const uint64_t assetPool = AnimatorAssetPool::revision();
        revisionChanged = clipPool != clipRevision || assetPool != assetRevision;
        clipRevision = clipPool;
        assetRevision = assetPool;
        jobs.clear();
        const float scaled = std::isfinite(dt) ? (float)dt : 0.0f;
        const float timeScale = Engine::getTimeScale();
        for (size_t i = 0; i < animators->size(); i++){
            AnimatorComponent& component = animators->getComponentFromIndex(i);
            if ((component.updateMode == AnimatorUpdateMode::AnimatePhysics) != fixedPhase) continue;
            float step = scaled;
            if (component.updateMode == AnimatorUpdateMode::UnscaledTime){
                step = timeScale > 0.0f ? scaled / timeScale : (float)std::min(Engine::getFramePeriod(), (double)Engine::getMaxDeltatime());
            }
            Job job;
            if (prepare(animators->getEntity(i), component, step, job)) jobs.push_back(job);
        }
        stats.animators = jobs.size();
    }
    const double prepared = nowMs();
    {
        JE_PROFILE_SCOPE("Anim.Wave");
        auto work = [this](size_t chunk){
            const size_t begin = chunk * kAnimatorsPerJob;
            const size_t end = std::min(begin + kAnimatorsPerJob, jobs.size());
            for (size_t j = begin; j < end; j++){
                Job& job = jobs[j];
                job.instance->update(*job.component, job.context);
                if (job.instance->output.sampled) writePose(*job.component, *job.instance);
            }
        };
        const size_t chunks = (jobs.size() + kAnimatorsPerJob - 1) / kAnimatorsPerJob;
        if (parallel && chunks > 1){
            jobs::parallelFor(chunks, work);
        }else{
            for (size_t c = 0; c < chunks; c++) work(c);
        }
        for (const Job& job : jobs) stats.sampled += job.instance->output.sampled ? 1 : 0;
    }
    const double waved = nowMs();
    {
        JE_PROFILE_SCOPE("Anim.Finalize");
        const float frameDt = std::isfinite(dt) ? (float)dt : 0.0f;
        for (Job& job : jobs){
            // an earlier callback may have removed this Animator
            if (!scene->isEntityCreated(job.entity)) continue;
            AnimatorComponent* component = scene->findComponent<AnimatorComponent>(job.entity);
            if (!component || component != job.component || !component->runtime) continue;
            finalize(job, frameDt);
        }
    }
    const double finished = nowMs();
    stats.prepareMs = prepared - start;
    stats.waveMs = waved - prepared;
    stats.finalizeMs = finished - waved;
}

void AnimatorSystem::update(double dt){
    if (paused) return;
    run(dt, false);
}

void AnimatorSystem::fixedUpdate(double dt){
    if (paused) return;
    run(dt, true);
}

bool AnimatorSystem::preview(Entity entity, float dt, std::string* error){
    AnimatorComponent* component = scene->findComponent<AnimatorComponent>(entity);
    if (!component){
        if (error) *error = "the GameObject has no Animator";
        return false;
    }
    if (!component->runtime) component->runtime = std::make_shared<anim::AnimatorInstance>();
    anim::AnimatorInstance& instance = *component->runtime;
    if (component->needsLoad || !instance.controller || AnimatorAssetPool::getGeneration(component->controller.path) != instance.controllerGeneration){
        loadController(entity, *component, instance);
    }
    if (!instance.controller){
        if (error) *error = instance.controllerError.empty() ? std::string("the Animator has no controller") : instance.controllerError;
        return false;
    }
    revisionChanged = AnimationClipPool::revision() != clipRevision || AnimatorAssetPool::revision() != assetRevision;
    if (!ensureBound(entity, *component, instance)){
        if (error) *error = instance.bindFailed ? "another Animator animates its bones" : "loading (models or clips)";
        return false;
    }
    Job job;
    job.entity = entity;
    job.component = component;
    job.instance = &instance;
    job.context.dt = dt;
    if (Transform* transform = scene->findComponent<Transform>(entity)){
        job.context.rootWorld = transform->modelMatrix;
        job.context.rootWorldInverse = transform->modelMatrix.inverse();
        job.context.rootWorldRotation = transform->worldRotation;
    }
    ikInputs(*component, instance, job.context);
    instance.update(*component, job.context);
    if (instance.output.sampled) writePose(*component, instance);
    // edit mode: no callbacks, no root motion; notes and ended montages are dropped
    instance.output.states.clear();
    instance.output.montages.clear();
    instance.output.events.clear();
    instance.output.cues.clear();
    instance.montages.erase(std::remove_if(instance.montages.begin(), instance.montages.end(),
        [](const anim::AnimatorMontageRun& m){ return m.phase == anim::AnimatorMontageRun::Phase::Ended; }), instance.montages.end());
    return true;
}

void AnimatorSystem::restorePreview(Entity entity){
    AnimatorComponent* component = scene->findComponent<AnimatorComponent>(entity);
    if (!component || !component->runtime) return;
    anim::AnimatorInstance& instance = *component->runtime;
    for (const auto& entry : instance.rest){
        if (!scene->isEntityCreated(entry.first)) continue;
        const anim::RigRest& rest = entry.second;
        if (rest.hasTransform){
            if (Transform* transform = scene->findComponent<Transform>(entry.first)){
                transform->position = rest.position;
                transform->rotation = rest.rotation;
                transform->scale = rest.scale;
                transform->needUpdate = true;
            }
        }
        if (!rest.morph.empty()){
            if (MeshComponent* mesh = scene->findComponent<MeshComponent>(entry.first)){
                for (size_t k = 0; k < rest.morph.size() && k < (size_t)MAX_MORPHTARGETS; k++) mesh->morphWeights[k] = rest.morph[k];
            }
        }
        if (!rest.lattice.empty()){
            if (LatticeComponent* lattice = scene->findComponent<LatticeComponent>(entry.first)){
                lattice->offsets = rest.lattice;
                lattice->revision++;
            }
        }
    }
    // properties after the transforms, deferred rows (GameObject active, enabled) last
    for (int pass = 0; pass < 2; pass++){
        for (const auto& entry : instance.rest){
            if (!scene->isEntityCreated(entry.first)) continue;
            for (const auto& property : entry.second.properties){
                const anim::RigPropertyRest& rest = property.second;
                if (rest.binding.deferred != (pass == 1) || rest.values.size() != rest.binding.width) continue;
                if (anim::scriptBindingStale(scene, entry.first, rest.binding)) continue;
                anim::writePropertyKeySpace(scene, entry.first, rest.binding, rest.values.data());
            }
        }
    }
    release(entity, instance);
    component->runtime.reset();
    component->needsLoad = true;
}

void AnimatorSystem::resetAnimators(){
    auto animators = scene->getComponentArray<AnimatorComponent>();
    for (size_t i = 0; i < animators->size(); i++){
        AnimatorComponent& component = animators->getComponentFromIndex(i);
        if (component.runtime) release(animators->getEntity(i), *component.runtime);
        component.runtime.reset();
        component.needsLoad = true;
    }
    owners.clear();
}

bool AnimatorSystem::playMontage(Entity entity, const std::string& path, float rate, const std::string& section, std::string* error){
    anim::AnimatorInstance* runtime = instance(entity);
    if (!runtime || !runtime->controller){
        if (error) *error = "the Animator has no controller";
        return false;
    }
    std::string loadError;
    std::shared_ptr<const anim::MontageDef> montage = AnimatorAssetPool::montage(path, &loadError);
    if (!montage){
        if (error) *error = "cannot load the montage " + path + ": " + loadError;
        return false;
    }
    if (montage->clip.path.empty()){
        if (error) *error = "the montage " + path + " has no clip";
        return false;
    }
    const size_t slot = runtime->montageClipSlot(montage->clip);
    if (!runtime->playMontage(path, montage, slot, rate, section)){
        if (error){
            std::string slots;
            for (const anim::CompiledLayer& layer : runtime->controller->layers) slots += (slots.empty() ? "" : ", ") + layer.name + ": \"" + layer.slot + "\"";
            *error = "no layer plays the montage slot \"" + montage->slot + "\" (set a layer's slot; the layers' slots: " + slots + ")";
        }
        return false;
    }
    // a clip the pool has: bound now, else the next update loads it
    AnimatorComponent* component = scene->findComponent<AnimatorComponent>(entity);
    anim::AnimatorClipSlot& clip = runtime->clips[slot];
    if (!clip.clip){
        bool loading = false;
        std::string clipError;
        clip.clip = AnimationClipPool::pollOrLoad(clip.path, loading, &clipError);
        if (clip.clip) clip.generation = AnimationClipPool::getGeneration(clip.path);
    }
    if (clip.clip && !clip.bound && runtime->bound && component) runtime->bindClip(slot, *component);
    return true;
}
