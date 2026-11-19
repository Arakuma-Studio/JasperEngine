// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "input/StateHash.h"

#include "Scene.h"
#include "component/CharacterController2DComponent.h"
#include "component/CharacterControllerComponent.h"
#include "component/NavMeshAgentComponent.h"
#include "component/Rigidbody2DComponent.h"
#include "component/RigidbodyComponent.h"
#include "component/ScriptComponent.h"
#include "component/Transform.h"

#include "xxhash/xxhash.h"

#include <algorithm>
#include <cmath>

namespace jasperengine::input {

namespace {

    struct Hasher {
        XXH3_state_t* state = XXH3_createState();
        Hasher() { XXH3_64bits_reset(state); }
        ~Hasher() { XXH3_freeState(state); }
        Hasher(const Hasher&) = delete;
        Hasher& operator=(const Hasher&) = delete;

        void bytes(const void* data, size_t size) { XXH3_64bits_update(state, data, size); }
        void i32(int32_t v) { bytes(&v, sizeof(v)); }
        void f(float v) { i32(quantizeState(v)); }
        uint64_t digest() const { return XXH3_64bits_digest(state); }
    };

    void hashValue(Hasher& h, const ScriptJson& value){
        if (value.is_number()) {
            h.f(value.get<float>());
        } else if (value.is_boolean()) {
            h.i32(value.get<bool>() ? 1 : 0);
        } else if (value.is_string()) {
            const std::string& text = value.get_ref<const std::string&>();
            h.bytes(text.data(), text.size());
        } else if (value.is_array()) {
            for (const ScriptJson& item : value)
                hashValue(h, item);
        }
    }

    // the entities with a Transform, by id
    std::vector<Entity> transformEntities(Scene* scene){
        std::vector<Entity> entities;
        auto* transforms = scene->getComponentArray<Transform>();
        if (!transforms)
            return entities;
        entities.reserve(transforms->size());
        for (size_t i = 0; i < transforms->size(); i++)
            entities.push_back(transforms->getEntity(i));
        std::sort(entities.begin(), entities.end());
        return entities;
    }

    template<typename T>
    bool has(Scene* scene, Entity entity){
        return scene->isComponentRegistered<T>() && scene->findComponent<T>(entity) != nullptr;
    }

}

int32_t quantizeState(float value){
    const double scaled = std::round(static_cast<double>(value) * 1024.0);
    if (!(scaled == scaled))
        return 0;
    return static_cast<int32_t>(std::clamp(scaled, -2147483648.0, 2147483647.0));
}

uint64_t hashState(const std::vector<Scene*>& scenes, const ScriptJson& values){
    Hasher h;
    for (Scene* scene : scenes) {
        if (!scene)
            continue;
        auto* transforms = scene->getComponentArray<Transform>();
        if (!transforms)
            continue;
        // one update a scene: a field at a time costs more than the hash itself
        const std::vector<Entity> entities = transformEntities(scene);
        std::vector<int32_t> fields;
        fields.reserve(entities.size() * 12);
        for (Entity entity : entities) {
            const Transform* t = scene->findComponent<Transform>(entity);
            if (!t)
                continue;
            fields.push_back(static_cast<int32_t>(entity));
            fields.push_back(scene->isEntityActiveInHierarchy(entity) ? 1 : 0);
            for (float v : {t->worldPosition.x, t->worldPosition.y, t->worldPosition.z, t->worldRotation.w, t->worldRotation.x,
                            t->worldRotation.y, t->worldRotation.z, t->worldScale.x, t->worldScale.y, t->worldScale.z})
                fields.push_back(quantizeState(v));
        }
        h.bytes(fields.data(), fields.size() * sizeof(int32_t));
    }
    // nlohmann::json keeps an object's keys sorted: name order
    if (values.is_object()) {
        for (auto it = values.begin(); it != values.end(); ++it) {
            h.bytes(it.key().data(), it.key().size());
            hashValue(h, it.value());
        }
    }
    return h.digest();
}

std::string entityPath(Scene* scene, Entity entity){
    std::string path;
    for (int depth = 0; entity != NULL_ENTITY && depth < 256; depth++) {
        const std::string name = scene->getEntityName(entity);
        path = path.empty() ? name : name + "/" + path;
        const Transform* t = scene->findComponent<Transform>(entity);
        entity = t ? t->parent : NULL_ENTITY;
    }
    return path;
}

std::vector<StateProbe> findProbes(const std::vector<Scene*>& scenes, const std::vector<std::string>& paths){
    std::vector<StateProbe> probes;
    for (const std::string& path : paths) {
        StateProbe probe;
        probe.path = path;
        for (Scene* scene : scenes) {
            if (!scene)
                continue;
            for (Entity entity : transformEntities(scene)) {
                if (entityPath(scene, entity) == path) {
                    probe.scene = scene;
                    probe.entity = entity;
                    break;
                }
            }
            if (probe.scene)
                break;
        }
        probes.push_back(probe);
    }
    return probes;
}

std::vector<StateProbe> chooseProbes(const std::vector<Scene*>& scenes, size_t count){
    std::vector<StateProbe> probes;
    for (Scene* scene : scenes) {
        if (!scene)
            continue;
        for (Entity entity : transformEntities(scene)) {
            if (probes.size() >= count)
                return probes;
            if (has<RigidbodyComponent>(scene, entity) || has<Rigidbody2DComponent>(scene, entity) ||
                has<CharacterControllerComponent>(scene, entity) || has<CharacterController2DComponent>(scene, entity) ||
                has<NavMeshAgentComponent>(scene, entity) || has<ScriptComponent>(scene, entity))
                probes.push_back({entityPath(scene, entity), scene, entity});
        }
    }
    return probes;
}

Vector3 probePosition(const StateProbe& probe){
    if (!probe.scene || probe.entity == NULL_ENTITY)
        return Vector3(0, 0, 0);
    const Transform* t = probe.scene->findComponent<Transform>(probe.entity);
    return t ? t->worldPosition : Vector3(0, 0, 0);
}

}
