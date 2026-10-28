// Paired benchmark adapter: exact production isParentOf methods, with real ECS
// metadata, component manager, ComponentArray and Transform. No renderer startup.
#include "ecs/EntityManager.h"
#include "ecs/ComponentManager.h"
#include "component/Transform.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <limits>
#include <random>
#include <string>
#include <vector>

using namespace jasperengine;
namespace jasperengine { void Log::error(const char*, ...) {} }

struct Registry {
    EntityManager entityManager;
    ComponentManager componentManager;
    Registry() { componentManager.registerComponent<Transform>(); }
    Signature getSignature(Entity entity) const { return entityManager.getSignature(entity); }
    template<class T> ComponentId getComponentId() const { return componentManager.getComponentId<T>(); }
    bool before(Entity parent, Entity child) const;
    bool after(Entity parent, Entity child) const;
    Entity add(Entity parent = NULL_ENTITY, bool transformed = true) {
        Entity entity = entityManager.createUserEntity();
        if (transformed) {
            Transform transform;
            transform.parent = parent;
            componentManager.addComponent(entity, std::move(transform));
            Signature signature;
            signature.set(getComponentId<Transform>());
            entityManager.setSignature(entity, signature);
        }
        return entity;
    }
};

// PRODUCTION_METHODS

void verify(Registry& registry, const std::vector<Entity>& entities) {
    for (Entity parent : entities)
        for (Entity child : entities)
            assert(registry.before(parent, child) == registry.after(parent, child));
}

void correctness() {
    std::mt19937 random(89247);
    for (int round = 0; round < 24; ++round) {
        Registry registry;
        std::vector<Entity> entities = {NULL_ENTITY, 999999};
        for (int i = 0; i < 160; ++i) {
            const Entity parent = entities[random() % entities.size()];
            entities.push_back(registry.add(parent, random() % 8 != 0));
        }
        verify(registry, entities);
        // Include late parents, missing transforms, missing entities, self-parenting
        // and cycles. Direct-parent matches retain the old order exception.
        auto transforms = registry.componentManager.getComponentArray<Transform>();
        for (size_t i = 0; i < transforms->size(); ++i)
            transforms->getComponentFromIndex(i).parent = entities[random() % entities.size()];
        verify(registry, entities);
        for (size_t i = 0; i < transforms->size() / 2; ++i)
            transforms->moveEntityToIndex(transforms->getEntity(random() % transforms->size()), random() % transforms->size());
        verify(registry, entities);
    }
    Registry registry;
    Entity root = registry.add(), child = registry.add(root), other = registry.add();
    assert(registry.after(root, child));
    assert(!registry.after(child, root));
    assert(!registry.after(root, root));
    registry.componentManager.getComponent<Transform>(root).parent = child;
    assert(registry.after(child, root));
    assert(!registry.after(other, child));
}

volatile bool resultSink = false;
using Method = bool (Registry::*)(Entity, Entity) const;

struct Sample {
    double cpu;
    double wall;
    int repeats;
    double totalCpuNs;
};

Sample measure(Registry& registry, Method method, Entity parent, Entity child, int& repeats) {
    // Calibrate independently: the optimized shallow query may be hundreds of
    // times faster. Each recorded block needs >=10ms CPU time so clock
    // quantization stays insignificant without multiplying the baseline work.
    while (true) {
        const auto wallStart = std::chrono::steady_clock::now();
        const std::clock_t cpuStart = std::clock();
        for (int repeat = 0; repeat < repeats; ++repeat)
            resultSink = (registry.*method)(parent, child);
        const double totalCpuNs = double(std::clock() - cpuStart) * 1e9 / CLOCKS_PER_SEC;
        const double totalWallNs = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - wallStart).count();
        if (totalCpuNs >= 10000000.0)
            return {totalCpuNs / repeats, totalWallNs / repeats, repeats, totalCpuNs};
        assert(repeats <= std::numeric_limits<int>::max() / 2);
        repeats *= 2;
    }
}

void benchmark(const char* shape, size_t size) {
    Registry registry;
    const Entity root = registry.add(), intermediate = registry.add(root), other = registry.add();
    Entity child = intermediate;
    for (size_t i = 0; i < size; ++i)
        child = registry.add(std::string(shape) == "chain" ? child : NULL_ENTITY);
    if (std::string(shape) != "chain") child = registry.add(intermediate);
    const std::pair<const char*, Entity> cases[] = {{"ancestor", root}, {"unrelated", other}, {"direct", registry.componentManager.getComponent<Transform>(child).parent}};
    for (const auto& query : cases) {
        assert(registry.before(query.second, child) == registry.after(query.second, child));
        for (int warmup = 0; warmup < 200; ++warmup) {
            resultSink = registry.before(query.second, child);
            resultSink = registry.after(query.second, child);
        }
        int beforeRepeats = 256, afterRepeats = 256;
        for (int sample = 0; sample < 9; ++sample) {
            Sample before, after;
            if (sample % 2) {
                after = measure(registry, &Registry::after, query.second, child, afterRepeats);
                before = measure(registry, &Registry::before, query.second, child, beforeRepeats);
            } else {
                before = measure(registry, &Registry::before, query.second, child, beforeRepeats);
                after = measure(registry, &Registry::after, query.second, child, afterRepeats);
            }
            std::printf("%s,%zu,%s,%d,%.3f,%.3f,%.3f,%.3f,%d,%d,%.0f,%.0f\n", shape, size, query.first, sample, before.cpu, after.cpu, before.wall, after.wall, before.repeats, after.repeats, before.totalCpuNs, after.totalCpuNs);
        }
    }
}

int main() {
    correctness();
#ifndef CORE_CHECK_ONLY
    for (size_t size : {size_t(16), size_t(1024), size_t(10000)}) {
        benchmark("wide", size);
        benchmark("chain", size);
    }
#endif
}
