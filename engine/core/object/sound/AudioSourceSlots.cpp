// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "AudioSourceSlots.h"
#include "AudioSource.h"
#include "component/AudioSourceComponent.h"
#include "component/Transform.h"
#include "manager/ObjectManager.h"
#include "EntityHandle.h"
#include "Scene.h"
#include "Log.h"

#include <algorithm>
#include <map>
#include <string>
#include <utility>

using namespace jasperengine;

bool AudioSourceSlots::isSlot(Scene* scene, Entity entity){
    return ownerOf(scene, entity) != NULL_ENTITY;
}

Entity AudioSourceSlots::ownerOf(Scene* scene, Entity entity){
    if (!scene || !scene->isEntityCreated(entity)) return NULL_ENTITY;
    const AudioSourceComponent* audio = scene->findComponent<AudioSourceComponent>(entity);
    if (!audio || audio->slot <= 0) return NULL_ENTITY;
    const Transform* transform = scene->findComponent<Transform>(entity);
    if (!transform || transform->parent == NULL_ENTITY || !scene->isEntityCreated(transform->parent)) return NULL_ENTITY;
    return transform->parent;
}

std::vector<Entity> AudioSourceSlots::getSlots(Scene* scene, Entity owner){
    std::vector<std::pair<int, Entity>> found;
    if (!scene || !scene->isEntityCreated(owner)) return {};
    auto transforms = scene->getComponentArray<Transform>();
    if (!transforms->hasEntity(owner)) return {};
    // the owner's branch follows it in the Transform array: its direct children are in there
    const size_t start = transforms->getIndex(owner);
    const size_t last = scene->findBranchLastIndex(owner);
    for (size_t i = start + 1; i <= last && i < transforms->size(); i++){
        if (transforms->getComponentFromIndex(i).parent != owner) continue;
        const Entity child = transforms->getEntity(i);
        const AudioSourceComponent* audio = scene->findComponent<AudioSourceComponent>(child);
        if (audio && audio->slot > 0) found.emplace_back(audio->slot, child);
    }
    std::sort(found.begin(), found.end());
    std::vector<Entity> slots;
    slots.reserve(found.size());
    for (const auto& item : found) slots.push_back(item.second);
    return slots;
}

int AudioSourceSlots::nextSlot(Scene* scene, Entity owner){
    int next = 1;
    for (Entity slot : getSlots(scene, owner)){
        next = std::max(next, scene->getComponent<AudioSourceComponent>(slot).slot + 1);
    }
    return next;
}

std::vector<AudioSource> AudioSourceSlots::getAll(const EntityHandle& gameObject){
    std::vector<AudioSource> sources;
    Scene* scene = gameObject.getScene();
    const Entity owner = gameObject.getEntity();
    if (!scene || !scene->isEntityCreated(owner)) return sources;
    if (scene->findComponent<AudioSourceComponent>(owner)) sources.emplace_back(scene, owner);
    for (Entity slot : getSlots(scene, owner)) sources.emplace_back(scene, slot);
    return sources;
}

AudioSource AudioSourceSlots::add(const EntityHandle& gameObject){
    Scene* scene = gameObject.getScene();
    const Entity owner = gameObject.getEntity();
    if (!scene || !scene->isEntityCreated(owner)){
        Log::error("AudioSource.add: the GameObject was destroyed");
        return AudioSource(scene, static_cast<Entity>(NULL_ENTITY));
    }
    AudioSourceComponent* own = scene->findComponent<AudioSourceComponent>(owner);
    if (!own){
        scene->addComponent<AudioSourceComponent>(owner, AudioSourceComponent());
        return AudioSource(scene, owner);
    }
    if (!scene->findComponent<Transform>(owner)){
        // an older bare 2D source becomes a GameObject that still plays 2D (as getGameObject)
        scene->addComponent<Transform>(owner, Transform());
        AudioSourceComponent& audio = scene->getComponent<AudioSourceComponent>(owner);
        audio.spatialBlend = 0.0f;
        audio.needUpdate = true;
    }
    const int slot = nextSlot(scene, owner);
    const Entity entity = scene->createEntity();
    scene->setEntityName(entity, "Audio Source " + std::to_string(slot + 1));
    scene->addComponent<Transform>(entity, Transform());
    scene->addEntityChild(owner, entity, false);
    AudioSourceComponent audio;
    audio.slot = slot;
    scene->addComponent<AudioSourceComponent>(entity, audio);
    return AudioSource(scene, entity);
}

void AudioSourceSlots::remove(const AudioSource& source){
    Scene* scene = source.getScene();
    const Entity entity = source.getEntity();
    if (!scene || !scene->isEntityCreated(entity) || !scene->findComponent<AudioSourceComponent>(entity)) return;
    if (isSlot(scene, entity)){
        ObjectManager::destroyImmediate(scene, entity);
    }else{
        // its voices stop with it (AudioSystem::onComponentRemoved)
        scene->removeComponent<AudioSourceComponent>(entity);
    }
}

size_t AudioSourceSlots::repair(Scene* scene){
    if (!scene) return 0;
    size_t repairs = 0;
    auto audios = scene->getComponentArray<AudioSourceComponent>();
    std::map<Entity, std::vector<std::pair<int, Entity>>> byOwner;
    for (size_t i = 0; i < audios->size(); i++){
        AudioSourceComponent& audio = audios->getComponentFromIndex(i);
        if (audio.slot == 0) continue;
        const Entity entity = audios->getEntity(i);
        const Entity owner = audio.slot > 0 ? ownerOf(scene, entity) : NULL_ENTITY;
        if (owner == NULL_ENTITY){
            Log::warn("Audio Source slot '%s' (%u) is not under a GameObject: it loads as an ordinary GameObject",
                      scene->getEntityName(entity).c_str(), entity);
            audio.slot = 0;
            repairs++;
            continue;
        }
        byOwner[owner].emplace_back(audio.slot, entity);
    }
    for (auto& [owner, slots] : byOwner){
        std::sort(slots.begin(), slots.end());
        bool shared = false;
        for (size_t i = 1; i < slots.size(); i++) shared = shared || slots[i].first == slots[i - 1].first;
        if (!shared) continue;
        Log::warn("The Audio Sources of '%s' (%u) share slot numbers: numbered again 1 to %zu",
                  scene->getEntityName(owner).c_str(), owner, slots.size());
        for (size_t i = 0; i < slots.size(); i++){
            AudioSourceComponent& audio = scene->getComponent<AudioSourceComponent>(slots[i].second);
            if (audio.slot != (int)(i + 1)){
                audio.slot = (int)(i + 1);
                repairs++;
            }
        }
    }
    return repairs;
}
