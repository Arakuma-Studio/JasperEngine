#include "EntityPath.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <random>

using namespace jasperengine;
using jasperengine::editor::EntityPath;
namespace jasperengine { void Log::error(const char*, ...) { std::abort(); } }

// Match the old traversal and shared-cache ordering, including the depth limit.
Entity referenceFind(EntityRegistry* registry, const std::vector<Entity>& entities, const std::string& path) {
    EntityPath::Cache cache;
    for (Entity entity : entities) {
        if (registry->isEntityCreated(entity) && EntityPath::of(registry, entity, &cache) == path)
            return entity;
    }
    return NULL_ENTITY;
}

void check(EntityRegistry& registry, const std::vector<Entity>& entities) {
    EntityPath::Cache cache;
    for (Entity entity : entities) {
        if (!registry.isEntityCreated(entity)) continue;
        const std::string path = EntityPath::of(&registry, entity, &cache);
        assert(EntityPath::find(&registry, entities, path) == referenceFind(&registry, entities, path));
    }
    assert(EntityPath::find(&registry, entities, "missing/path") == NULL_ENTITY);
}

void correctness() {
    assert(EntityPath::find(nullptr, {}, "") == NULL_ENTITY);
    EntityRegistry registry;
    assert(EntityPath::find(&registry, {}, "") == NULL_ENTITY);
    std::vector<Entity> entities;
    std::mt19937 random(849);
    for (size_t i = 0; i < 180; ++i) {
        const Entity parent = i && random() % 3 ? entities[random() % entities.size()] : NULL_ENTITY;
        const std::string names[] = {"", "node", "node#1", "a/b", "long name with spaces", "node"};
        entities.push_back(registry.add(names[random() % 6], parent, random() % 5 != 0));
    }
    entities.push_back(987654);
    check(registry, entities);
    std::shuffle(entities.begin(), entities.end(), random);
    check(registry, entities);
    entities.erase(entities.begin(), entities.begin() + 70);
    check(registry, entities); // Siblings outside the candidate list still count.
    // Parent later in Transform order and cycles retain the existing bounded recursion.
    EntityRegistry unusual;
    std::vector<Entity> unusualEntities;
    for (int i = 0; i < 20; ++i) unusualEntities.push_back(unusual.add("unrelated"));
    const Entity a = unusual.add("same"), b = unusual.add("same"), c = unusual.add("same", a);
    unusualEntities.insert(unusualEntities.begin(), {c, a, b});
    unusual.transforms.getComponent(a).parent = b;
    check(unusual, unusualEntities);
    unusual.transforms.getComponent(b).parent = a;
    check(unusual, unusualEntities);
    EntityRegistry deep;
    std::vector<Entity> chain;
    for (int i = 0; i < 270; ++i)
        chain.push_back(deep.add("n", chain.empty() ? NULL_ENTITY : chain.back()));
    std::reverse(chain.begin(), chain.end());
    const auto path = EntityPath::of(&deep, chain.front());
    assert(EntityPath::find(&deep, chain, path) == referenceFind(&deep, chain, path));
    assert(EntityPath::find(&deep, chain, "missing") == referenceFind(&deep, chain, "missing"));
}

int main(int argc, char** argv) {
    correctness();
#ifdef CHECK_ONLY
    return 0;
#endif
    volatile Entity sink = 0;
    if (argc > 1 && std::string(argv[1]) == "--subsets") {
        EntityRegistry registry;
        std::vector<Entity> entities;
        for (size_t i = 0; i < 5000; ++i) entities.push_back(registry.add("node" + std::to_string(i)));
        for (size_t size : {size_t(1), size_t(10)}) {
            const std::vector<Entity> subset(entities.end() - size, entities.end());
            const std::string path = EntityPath::of(&registry, subset.back());
            for (int sample = 0; sample < 5; ++sample) {
                const auto begin = std::chrono::steady_clock::now();
                const auto cpu = std::clock();
                for (int i = 0; i < 20; ++i) sink = EntityPath::find(&registry, subset, path);
                const double cpuUs = double(std::clock() - cpu) * 1e6 / CLOCKS_PER_SEC / 20;
                const double wallUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count() / 20;
                std::printf("%zu,subset,last,%d,%.6f,%.6f\n", size, sample, cpuUs, wallUs);
            }
        }
        return 0;
    }
    for (size_t size : {size_t(10), size_t(1000), size_t(5000)}) {
        for (bool duplicates : {false, true}) {
            EntityRegistry registry;
            std::vector<Entity> entities;
            for (size_t i = 0; i < size; ++i)
                entities.push_back(registry.add(duplicates ? "node" : "node" + std::to_string(i)));
            const std::string last = EntityPath::of(&registry, entities.back());
            for (const std::string& which : {std::string("first"), std::string("last"), std::string("missing")}) {
                const std::string path = which == "first" ? EntityPath::of(&registry, entities.front()) : which == "last" ? last : "absent";
                const int repeats = which == "first" ? 10000 : size == 10 ? 2000 : 1;
                for (int sample = 0; sample < 5; ++sample) {
                    const auto begin = std::chrono::steady_clock::now();
                    const auto cpu = std::clock();
                    for (int i = 0; i < repeats; ++i) sink = EntityPath::find(&registry, entities, path);
                    const double cpuUs = double(std::clock() - cpu) * 1e6 / CLOCKS_PER_SEC / repeats;
                    const double wallUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count() / repeats;
                    std::printf("%zu,%s,%s,%d,%.6f,%.6f\n", size, duplicates ? "duplicate" : "unique", which.c_str(), sample, cpuUs, wallUs);
                }
            }
        }
    }
    (void)sink;
}
