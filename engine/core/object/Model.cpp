// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Model.h"
#include "component/ModelComponent.h"

#include "io/FileData.h"

#include "subsystem/MeshSystem.h"
#include "component/ActionComponent.h"
#include "component/AnimationComponent.h"
#include "component/AnimationPlayerComponent.h"
#include "animation/AnimationPlayerRuntime.h"
#include "animation/ModelSubClips.h"
#include "animation/AnimatorInstance.h"
#include "animation/CompiledController.h"
#include "component/AnimatorComponent.h"
#include "object/AnimationPlayer.h"
#include "subsystem/AnimatorSystem.h"
#include "pose/ModelRig.h"

#include <algorithm>

using namespace jasperengine;

// The Animation Player on the model's GameObject: playAnimation / stopAnimations go to it first
// (animation clips spec 3.4, 9.R15/R42); the entity animations stay the fallback
static AnimationPlayerComponent* findModelPlayer(Scene* scene, Entity entity){
    return scene->isEntityCreated(entity) ? scene->findComponent<AnimationPlayerComponent>(entity) : nullptr;
}

// upstream's controller players: a disabled Animation Player beside an Animator hands over to
// it (object/AnimationPlayer.h); the Animator's states have no index
static bool playerHandsOver(Scene* scene, Entity entity, const AnimationPlayerComponent& player){
    return !player.enabled && scene->findComponent<AnimatorComponent>(entity) != nullptr;
}

// the player's states in list order (none when it hands over to an Animator)
static std::vector<std::string> playerIndexedStates(Scene* scene, Entity entity, const AnimationPlayerComponent& player){
    return playerHandsOver(scene, entity, player) ? std::vector<std::string>() : anim::playerStateNames(player.clips);
}

static bool playerHasState(Scene* scene, Entity entity, AnimationPlayerComponent& player, const std::string& name){
    if (playerHandsOver(scene, entity, player)){
        // a controller's state ("Layer.State" or "State")
        anim::AnimatorInstance* instance = scene->getSystem<AnimatorSystem>()->instance(entity);
        return instance && instance->controller && instance->controller->findState(name, -1) >= 0;
    }
    std::vector<std::string> names = anim::playerStateNames(player.clips);
    return std::find(names.begin(), names.end(), name) != names.end();
}

// plays a player state with a fade: an Animator transitions to it (CrossFadeInFixedTime)
static void playPlayerState(Scene* scene, Entity entity, AnimationPlayerComponent& player, const std::string& name, float fadeTime,
                            const std::string& who){
    if (playerHandsOver(scene, entity, player)){
        AnimationPlayer(scene, entity).play(name, fadeTime);
        return;
    }
    anim::playState(player, name, fadeTime, who);
}

// the player gives way to the model's entity animations (an Animator it hands over to stops)
static void fadeOutPlayer(Scene* scene, Entity entity, AnimationPlayerComponent& player, float fadeTime){
    if (playerHandsOver(scene, entity, player)){
        AnimationPlayer(scene, entity).stop();
        return;
    }
    anim::fadeOutStates(player, fadeTime);
}

// every running entity animation of the model fades out (except `keep`)
static void fadeOutModelAnimations(Scene* scene, const ModelComponent& model, Entity keep, float fadeTime){
    for (Entity animEntity : model.animations){
        if (animEntity == keep || animEntity == NULL_ENTITY || !scene->isEntityCreated(animEntity))
            continue;

        Signature signature = scene->getSignature(animEntity);
        if (!signature.test(scene->getComponentId<AnimationComponent>()) ||
            !signature.test(scene->getComponentId<ActionComponent>()))
            continue;

        ActionComponent& action = scene->getComponent<ActionComponent>(animEntity);
        if (action.state == ActionState::Running || action.state == ActionState::Paused || action.startTrigger){
            Animation(scene, animEntity).fadeOut(fadeTime);
        }
    }
}

// The entity animation of the model named `name` (-1 when none). The names the Inspector shows
// come first: the sub-clip names of the file's animations (the second of two glTF animations
// both named "Wave" is "Wave 2"; an entity renamed since keeps its file name there), which
// loadGLTF keeps on the component (no pooled-glTF lock here: a call never waits for another
// model's load). Then, as before, its entity name (the first of that name), and the sub-clip
// names of the entity names (the only names without a glTF parse, or when the counts differ).
static int findModelAnimationIndex(Scene* scene, const ModelComponent& model, const std::string& name){
    const size_t count = model.animations.size();
    auto valid = [&](size_t i){
        Entity animEntity = model.animations[i];
        return animEntity != NULL_ENTITY && scene->isEntityCreated(animEntity) &&
               scene->getSignature(animEntity).test(scene->getComponentId<AnimationComponent>());
    };
    if (model.animationClipNames.size() == count){
        for (size_t i = 0; i < count; i++){
            if (model.animationClipNames[i] == name && valid(i)) return (int)i;
        }
    }
    // the loader names an unnamed animation "Animation <i>" itself: a valid entity's name is never empty
    std::vector<std::string> entityNames(count);
    for (size_t i = 0; i < count; i++){
        if (valid(i)) entityNames[i] = scene->getEntityName(model.animations[i]);
    }
    auto find = [&](const std::vector<std::string>& names){
        for (int i = 0; i < (int)names.size(); i++){
            if (!entityNames[i].empty() && names[i] == name) return i;
        }
        return -1;
    };
    const int index = find(entityNames);
    return index >= 0 ? index : find(anim::subClipNames(entityNames));
}

// the entity animation `index` crossfades in, the model's other running ones out
static void playModelEntityAnimation(Scene* scene, const ModelComponent& model, int index, float fadeTime){
    Entity target;
    try{
        target = model.animations.at(index);
    }catch (const std::out_of_range& e){
        Log::error("Playing non-existent animation: %s", e.what());
        return;
    }

    // Fade out every other animation of this model that is currently running.
    fadeOutModelAnimations(scene, model, target, fadeTime);

    Animation(scene, target).fadeIn(fadeTime);
}

Model::Model(Scene* scene): Mesh(scene){
    addComponent<ModelComponent>();
}

Model::Model(Scene* scene, Entity entity): Mesh(scene, entity){
}

// a destructor must not throw: only a live entity that still has its ModelComponent
Model::~Model(){
    ModelComponent* model = ownsLiveEntity() ? scene->findComponent<ModelComponent>(entity) : nullptr;
    if (model){
        scene->getSystem<MeshSystem>()->clearBoneMapping(*model);
        scene->getSystem<MeshSystem>()->clearAnimationMapping(*model);
        scene->getSystem<MeshSystem>()->clearMeshNodeMapping(*model);
    }
}

Model::Model(const Model& rhs) = default;
Model& Model::operator=(const Model& rhs) = default;
Model::Model(Model&& rhs) noexcept = default;
Model& Model::operator=(Model&& rhs) noexcept = default;

bool Model::loadModel(const std::string& filename){
    std::string ext = FileData::getFilePathExtension(filename);

    if (ext.compare("obj") == 0) {
        return loadOBJ(filename);
    }
    return loadGLTF(filename);
}

// The load of a Model on the thread that may touch its scene: the entities it made in an earlier
// load go, and an owned Model owns the animations of this one. Component references are taken after
// the load: the meshes and bones it adds can move their arrays.
static bool loadModelFile(Scene* scene, Entity entity, bool owned, const std::string& filename, bool obj){
    MeshSystem* meshSystem = scene->getSystem<MeshSystem>().get();

    if (owned){
        ModelComponent& model = scene->getComponent<ModelComponent>(entity);
        meshSystem->clearBoneMapping(model);
        meshSystem->clearAnimationMapping(model);
        meshSystem->clearMeshNodeMapping(model);
    }

    const bool ret = obj ? meshSystem->loadOBJ(entity, filename) : meshSystem->loadGLTF(entity, filename);

    if (ret){
        if (owned && !obj){
            const ModelComponent& model = scene->getComponent<ModelComponent>(entity);
            for (Entity anim : model.animations){
                if (scene->getSignature(anim).test(scene->getComponentId<AnimationComponent>())){
                    scene->getComponent<AnimationComponent>(anim).ownedActions = true;
                }
            }
        }
        scene->getComponent<MeshComponent>(entity).needReload = true;
    }

    return ret;
}

bool Model::loadOBJ(const std::string& filename){
    MeshSystem* meshSystem = scene->getSystem<MeshSystem>().get();
    if (meshSystem->isLoadOnMainThread()){
        return meshSystem->prepareLoadOnMainThread(entity, filename, [target = scene, id = entity, owned = isEntityOwned(), filename]() {
            loadModelFile(target, id, owned, filename, true);
        });
    }
    return loadModelFile(scene, entity, isEntityOwned(), filename, true);
}

bool Model::loadGLTF(const std::string& filename){
    MeshSystem* meshSystem = scene->getSystem<MeshSystem>().get();
    if (meshSystem->isLoadOnMainThread()){
        return meshSystem->prepareLoadOnMainThread(entity, filename, [target = scene, id = entity, owned = isEntityOwned(), filename]() {
            loadModelFile(target, id, owned, filename, false);
        });
    }
    return loadModelFile(scene, entity, isEntityOwned(), filename, false);
}

Animation Model::getAnimation(int index){
    ModelComponent& model = getComponent<ModelComponent>();

    try{
        return Animation(scene, model.animations.at(index));
    }catch (const std::out_of_range& e){
		Log::error("Retrieving non-existent animation: %s", e.what());
		throw;
	}
}

Animation Model::findAnimation(const std::string& name){
    ModelComponent& model = getComponent<ModelComponent>();

    int index = findModelAnimationIndex(scene, model, name);
    if (index >= 0){
        return Animation(scene, model.animations[index]);
    }
    Log::error("Retrieving non-existent animation: %s", name.c_str());
    throw std::out_of_range("vector animations is out of range");
}

void Model::playAnimation(int index, float fadeTime){
    ModelComponent& model = getComponent<ModelComponent>();

    // with an Animation Player an index is one of its states first (as a name is); an index past
    // its states is the entity animation of that index, which the player's states make room for
    if (AnimationPlayerComponent* player = findModelPlayer(scene, entity)){
        std::vector<std::string> names = playerIndexedStates(scene, entity, *player);
        if (index >= 0 && index < (int)names.size()){
            playPlayerState(scene, entity, *player, names[(size_t)index], fadeTime, getName());
            fadeOutModelAnimations(scene, model, NULL_ENTITY, fadeTime);
            return;
        }
        if (index < 0 || index >= (int)model.animations.size()){
            Log::error("Playing non-existent animation: '%s' has no Animation Player state or animation %i", getName().c_str(), index);
            return;
        }
        fadeOutPlayer(scene, entity, *player, fadeTime);
    }

    playModelEntityAnimation(scene, model, index, fadeTime);
}

void Model::playAnimation(int index){
    AnimationPlayerComponent* player = findModelPlayer(scene, entity);
    if (player && index >= 0 && index < (int)playerIndexedStates(scene, entity, *player).size()){
        playAnimation(index, player->defaultFadeTime);
        return;
    }

    ModelComponent& model = getComponent<ModelComponent>();

    float fadeTime = 0.0f;
    if (index >= 0 && index < (int)model.animations.size()){
        Entity target = model.animations[index];
        if (target != NULL_ENTITY && scene->isEntityCreated(target) &&
            scene->getSignature(target).test(scene->getComponentId<AnimationComponent>())){
            fadeTime = scene->getComponent<AnimationComponent>(target).defaultFadeTime;
        }
    }

    playAnimation(index, fadeTime);
}

void Model::playAnimation(const std::string& name, float fadeTime){
    ModelComponent& model = getComponent<ModelComponent>();

    // a state of the Animation Player first; else the entity animation of that name, which the
    // player's states then make room for
    AnimationPlayerComponent* player = findModelPlayer(scene, entity);
    if (player && playerHasState(scene, entity, *player, name)){
        playPlayerState(scene, entity, *player, name, fadeTime, getName());
        fadeOutModelAnimations(scene, model, NULL_ENTITY, fadeTime);
        return;
    }

    int index = findModelAnimationIndex(scene, model, name);
    if (index >= 0){
        if (player){
            fadeOutPlayer(scene, entity, *player, fadeTime);
        }
        playModelEntityAnimation(scene, model, index, fadeTime);
        return;
    }

    Log::error("Playing non-existent animation: %s", name.c_str());
}

void Model::playAnimation(const std::string& name){
    ModelComponent& model = getComponent<ModelComponent>();

    AnimationPlayerComponent* player = findModelPlayer(scene, entity);
    if (player && playerHasState(scene, entity, *player, name)){
        playAnimation(name, player->defaultFadeTime);
        return;
    }

    int index = findModelAnimationIndex(scene, model, name);
    if (index >= 0){
        const float fadeTime = scene->getComponent<AnimationComponent>(model.animations[index]).defaultFadeTime;
        if (player){
            fadeOutPlayer(scene, entity, *player, fadeTime);
        }
        playModelEntityAnimation(scene, model, index, fadeTime);
        return;
    }

    Log::error("Playing non-existent animation: %s", name.c_str());
}

void Model::stopAnimations(float fadeTime){
    ModelComponent& model = getComponent<ModelComponent>();

    if (AnimationPlayerComponent* player = findModelPlayer(scene, entity)){
        fadeOutPlayer(scene, entity, *player, fadeTime);
    }

    for (Entity animEntity : model.animations){
        if (animEntity == NULL_ENTITY || !scene->isEntityCreated(animEntity))
            continue;

        Signature signature = scene->getSignature(animEntity);
        if (!signature.test(scene->getComponentId<AnimationComponent>()) ||
            !signature.test(scene->getComponentId<ActionComponent>()))
            continue;

        ActionComponent& action = scene->getComponent<ActionComponent>(animEntity);
        if (action.state == ActionState::Running || action.state == ActionState::Paused || action.startTrigger){
            Animation(scene, animEntity).fadeOut(fadeTime);
        }
    }
}

PoseAnimator Model::getPoseAnimator(){
    return PoseAnimator(scene, entity);
}

ModelHumanoidInfo Model::getHumanoidInfo() const{
    ModelHumanoidInfo info;
    const ModelComponent* model = scene ? scene->findComponent<ModelComponent>(entity) : nullptr;
    if (!model || model->bonesIdMapping.empty()) return info;
    info.valid = true;
    // the sidecar's Animation Type, even when the map does not validate
    std::string stamp;
    pose::HumanoidOverrides overrides;
    if (pose::humanoidSidecarStamp(model->filename, stamp, nullptr) && !stamp.empty()){
        std::vector<std::string> warnings;
        if (pose::parseHumanoidOverrides(stamp, overrides, warnings, nullptr)){
            info.animationType = pose::animationTypeName(overrides.animationType.value_or(pose::AnimationType::Generic));
        }else{
            overrides = pose::HumanoidOverrides();
        }
    }
    pose::ModelRigInfo rig;
    std::vector<Entity> bones;
    std::string error;
    if (!pose::buildModelRig(scene, entity, rig, bones, nullptr, &error)){
        info.error = error;
        // what is missing from the map auto-detect and the sidecar's overrides give
        pose::Skeleton skeleton;
        if (pose::buildSkeleton(scene, entity, skeleton, bones)){
            pose::HumanoidMap map;
            std::string ignored;
            pose::resolveHumanoid(skeleton, overrides, map, &ignored, nullptr);
            for (int s = 0; s < pose::HUMAN_BONE_COUNT; s++){
                if (map.bones[(size_t)s].empty()) info.missing.push_back(pose::humanBoneName(pose::boneAt(s)));
            }
        }
        return info;
    }
    info.humanoid = true;
    info.humanScale = rig.rig->humanScale();
    for (int s = 0; s < pose::HUMAN_BONE_COUNT; s++){
        if (rig.humanoid.bones[(size_t)s].empty()) info.missing.push_back(pose::humanBoneName(pose::boneAt(s)));
    }
    info.extraMapped = rig.extra.mappedCount();
    return info;
}

Bone Model::getBone(const std::string& name){
    ModelComponent& model = getComponent<ModelComponent>();

    try{
        return Bone(scene, model.bonesNameMapping.at(name));
    }catch (const std::out_of_range& e){
		Log::error("Retrieving non-existent bone: %s", e.what());
		throw;
	}
}

Bone Model::getBone(int id){
    ModelComponent& model = getComponent<ModelComponent>();

    try{
        return Bone(scene, model.bonesIdMapping.at(id));
    }catch (const std::out_of_range& e){
		Log::error("Retrieving non-existent bone: %s", e.what());
		throw;
	}
}

float Model::getMorphWeight(const std::string& name){
    ModelComponent& model = getComponent<ModelComponent>();

    if (model.morphNameMapping.count(name)){
        return getMorphWeight(model.morphNameMapping.at(name));
    }else{
        Log::error("Retrieving non-existent morph weight '%s'", name.c_str());
    }

    return 0;
}

float Model::getMorphWeight(int id){
    MeshComponent& mesh = getComponent<MeshComponent>();

    if (id >= 0 && id < MAX_MORPHTARGETS){
        return mesh.morphWeights[id];
    }else{
        Log::error("Retrieving non-existent morph weight '%i'", id);
    }

    return 0;
}

void Model::setMorphWeight(const std::string& name, float value){
    ModelComponent& model = getComponent<ModelComponent>();

    if (model.morphNameMapping.count(name)){
        setMorphWeight(model.morphNameMapping.at(name), value);
    }else{
        Log::error("Retrieving non-existent morph weight '%s'", name.c_str());
    }
}

void Model::setMorphWeight(int id, float value){
    MeshComponent& mesh = getComponent<MeshComponent>();

    if (id >= 0 && id < MAX_MORPHTARGETS){
        mesh.morphWeights[id] = value;
        // the weights are the mesh's own now, up to this target at least: a load (also one still
        // running) keeps them; targets past the count get the file's defaults
        // (MeshComponent::morphTargetCount)
        mesh.morphWeightsSet = true;
        mesh.morphTargetCount = std::max(mesh.morphTargetCount, id + 1);
    }else{
        Log::error("Retrieving non-existent morph weight '%i'", id);
    }
}

void Model::resetToBindPose(){
    ModelComponent& model = getComponent<ModelComponent>();
    scene->getSystem<MeshSystem>()->resetModelToBindPose(entity, model);
}

void Model::setEnabled(bool enabled){
    getComponent<ModelComponent>().enabled = enabled;
}

bool Model::isEnabled() const{
    return getComponent<ModelComponent>().enabled;
}
