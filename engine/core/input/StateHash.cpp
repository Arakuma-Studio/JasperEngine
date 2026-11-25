// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Only with the input scripts module (input/InputScriptSwitch.h); InputScriptsOff.cpp answers the
// engine without it
#include "input/InputScriptSwitch.h"
#if JASPERENGINE_INPUT_SCRIPTS_ENABLED

#include "input/StateHash.h"

#include "Scene.h"
#include "component/CharacterController2DComponent.h"
#include "component/CharacterControllerComponent.h"
#include "component/NavMeshAgentComponent.h"
#include "component/Rigidbody2DComponent.h"
#include "component/RigidbodyComponent.h"
#include "component/ScriptComponent.h"
#include "component/Transform.h"

#include "util/ContentHash.h"

#include <algorithm>
#include <cmath>

namespace jasperengine::input {

namespace {

    // the bytes the hash reads, hashed once (XXH3-64, util/ContentHash.h)
    struct Hasher {
        std::string bytesSeen;
        void bytes(const void* data, size_t size) { bytesSeen.append(static_cast<const char*>(data), size); }
        void i32(int32_t v) { bytes(&v, sizeof(v)); }
        void f(float v) { i32(quantizeState(v)); }
        uint64_t digest() const { return hashContent64(bytesSeen.data(), bytesSeen.size(), 0); }
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

// std::round's half away from zero, without its libm call (a fifth of a hash): clamped first, so the
// truncation is exact and the remainder decides the step
int32_t quantizeState(float value){
    const double scaled = static_cast<double>(value) * 1024.0;
    if (!(scaled == scaled))
        return 0;
    if (scaled >= 2147483647.0)
        return 2147483647;
    if (scaled <= -2147483648.0)
        return -2147483647 - 1;
    const int64_t whole = static_cast<int64_t>(scaled);
    const double rest = scaled - static_cast<double>(whole);
    return static_cast<int32_t>(whole + (rest >= 0.5 ? 1 : rest <= -0.5 ? -1 : 0));
}

uint64_t hashState(const std::vector<Scene*>& scenes, const ScriptJson& values){
    Hasher h;
    size_t transformCount = 0;
    for (Scene* scene : scenes) {
        auto* transforms = scene ? scene->getComponentArray<Transform>() : nullptr;
        transformCount += transforms ? transforms->size() : 0;
    }
    // R17 checkpoints: 12 fields an entity, appended where the digest reads them
    h.bytesSeen.reserve(transformCount * 12 * sizeof(int32_t) + 256);
    std::vector<std::pair<Entity, size_t>> order;
    for (Scene* scene : scenes) {
        if (!scene)
            continue;
        auto* transforms = scene->getComponentArray<Transform>();
        if (!transforms)
            continue;
        // the slots in entity order, so each Transform is read where it is (no lookup by entity);
        // they mostly are in that order already
        order.clear();
        order.reserve(transforms->size());
        for (size_t i = 0; i < transforms->size(); i++)
            order.emplace_back(transforms->getEntity(i), i);
        if (!std::is_sorted(order.begin(), order.end()))
            std::sort(order.begin(), order.end());
        const bool allActive = !scene->hasInactiveEntities();
        // gathered a chunk at a time on the stack, then appended
        constexpr size_t kChunk = 128;
        int32_t chunk[kChunk * 12];
        for (size_t first = 0; first < order.size(); first += kChunk) {
            const size_t last = std::min(order.size(), first + kChunk);
            int32_t* field = chunk;
            for (size_t k = first; k < last; k++) {
                const Entity entity = order[k].first;
                const Transform& t = transforms->getComponentFromIndex(order[k].second);
                *field++ = static_cast<int32_t>(entity);
                *field++ = allActive || scene->isEntityActiveInHierarchy(entity) ? 1 : 0;
                for (float v : {t.worldPosition.x, t.worldPosition.y, t.worldPosition.z, t.worldRotation.w, t.worldRotation.x,
                                t.worldRotation.y, t.worldRotation.z, t.worldScale.x, t.worldScale.y, t.worldScale.z})
                    *field++ = quantizeState(v);
            }
            h.bytes(chunk, static_cast<size_t>(field - chunk) * sizeof(int32_t));
        }
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

#endif // JASPERENGINE_INPUT_SCRIPTS_ENABLED
