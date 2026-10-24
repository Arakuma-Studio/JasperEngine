// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Model.h"

#include "io/FileData.h"

#include "subsystem/MeshSystem.h"
#include "component/ActionComponent.h"
#include "component/AnimationComponent.h"
#include "component/AnimationPlayerComponent.h"
#include "animation/AnimationPlayerRuntime.h"

#include <algorithm>

using namespace jasperengine;

// The Animation Player on the model's GameObject: playAnimation / stopAnimations go to it first
// (animation clips spec 3.4, 9.R15/R42); the entity animations stay the fallback
static AnimationPlayerComponent* findModelPlayer(Scene* scene, Entity entity){
    return scene->isEntityCreated(entity) ? scene->findComponent<AnimationPlayerComponent>(entity) : nullptr;
}

static bool playerHasState(const AnimationPlayerComponent& player, const std::string& name){
    std::vector<std::string> names = anim::playerStateNames(player.clips);
    return std::find(names.begin(), names.end(), name) != names.end();
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

bool Model::loadOBJ(const std::string& filename){
    MeshComponent& mesh = getComponent<MeshComponent>();
    ModelComponent& model = getComponent<ModelComponent>();

    if (isEntityOwned()){
        scene->getSystem<MeshSystem>()->clearBoneMapping(model);
        scene->getSystem<MeshSystem>()->clearAnimationMapping(model);
        scene->getSystem<MeshSystem>()->clearMeshNodeMapping(model);
    }

    bool ret = scene->getSystem<MeshSystem>()->loadOBJ(entity, filename);

    if (ret){
        mesh.needReload = true;
    }

    return ret;
}

bool Model::loadGLTF(const std::string& filename){
    MeshComponent& mesh = getComponent<MeshComponent>();
    ModelComponent& model = getComponent<ModelComponent>();

    if (isEntityOwned()){
        scene->getSystem<MeshSystem>()->clearBoneMapping(model);
        scene->getSystem<MeshSystem>()->clearAnimationMapping(model);
        scene->getSystem<MeshSystem>()->clearMeshNodeMapping(model);
    }

    bool ret = scene->getSystem<MeshSystem>()->loadGLTF(entity, filename);

    if (ret){
        if (isEntityOwned()){
            for (Entity anim : model.animations){
                if (scene->getSignature(anim).test(scene->getComponentId<AnimationComponent>())){
                    scene->getComponent<AnimationComponent>(anim).ownedActions = true;
                }
            }
        }
        mesh.needReload = true;
    }

    return ret;
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

    for (int i = 0; i < model.animations.size(); i++){
        Signature signature = scene->getSignature(model.animations[i]);
        if (signature.test(scene->getComponentId<AnimationComponent>())){
            if (scene->getEntityName(model.animations[i]) == name){
                return Animation(scene, model.animations[i]);
            }
        }
    }
    Log::error("Retrieving non-existent animation: %s", name.c_str());
    throw std::out_of_range("vector animations is out of range");
}

void Model::playAnimation(int index, float fadeTime){
    ModelComponent& model = getComponent<ModelComponent>();

    // with an Animation Player the index is one of its states
    if (AnimationPlayerComponent* player = findModelPlayer(scene, entity)){
        std::vector<std::string> names = anim::playerStateNames(player->clips);
        if (index < 0 || index >= (int)names.size()){
            Log::error("Playing non-existent animation: the Animation Player of '%s' has no state %i", getName().c_str(), index);
            return;
        }
        anim::playState(*player, names[(size_t)index], fadeTime, getName());
        fadeOutModelAnimations(scene, model, NULL_ENTITY, fadeTime);
        return;
    }

    playModelEntityAnimation(scene, model, index, fadeTime);
}

void Model::playAnimation(int index){
    if (AnimationPlayerComponent* player = findModelPlayer(scene, entity)){
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
    if (player && playerHasState(*player, name)){
        anim::playState(*player, name, fadeTime, getName());
        fadeOutModelAnimations(scene, model, NULL_ENTITY, fadeTime);
        return;
    }

    for (int i = 0; i < (int)model.animations.size(); i++){
        Signature signature = scene->getSignature(model.animations[i]);
        if (signature.test(scene->getComponentId<AnimationComponent>())){
            if (scene->getEntityName(model.animations[i]) == name){
                if (player){
                    anim::fadeOutStates(*player, fadeTime);
                }
                playModelEntityAnimation(scene, model, i, fadeTime);
                return;
            }
        }
    }

    Log::error("Playing non-existent animation: %s", name.c_str());
}

void Model::playAnimation(const std::string& name){
    ModelComponent& model = getComponent<ModelComponent>();

    AnimationPlayerComponent* player = findModelPlayer(scene, entity);
    if (player && playerHasState(*player, name)){
        playAnimation(name, player->defaultFadeTime);
        return;
    }

    for (int i = 0; i < (int)model.animations.size(); i++){
        Signature signature = scene->getSignature(model.animations[i]);
        if (signature.test(scene->getComponentId<AnimationComponent>())){
            if (scene->getEntityName(model.animations[i]) == name){
                const float fadeTime = scene->getComponent<AnimationComponent>(model.animations[i]).defaultFadeTime;
                if (player){
                    anim::fadeOutStates(*player, fadeTime);
                }
                playModelEntityAnimation(scene, model, i, fadeTime);
                return;
            }
        }
    }

    Log::error("Playing non-existent animation: %s", name.c_str());
}

void Model::stopAnimations(float fadeTime){
    ModelComponent& model = getComponent<ModelComponent>();

    if (AnimationPlayerComponent* player = findModelPlayer(scene, entity)){
        anim::fadeOutStates(*player, fadeTime);
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
