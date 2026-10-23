// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// ECS benchmark of S07 (docs/specs/S07-ecs.md R25-R27), run by run-ecs-bench.py.
//
// A real Scene (its seven SubSystems' hooks attached) with N entities: a Transform and a
// Payload {string, vector, shared_ptr} each, half of them parented in chains of depth 4, a
// second Hierarchy type (BoneComponent) on half of them. The same source compiles against the
// engine before S07 (the --baseline-ref build) and after it: what only the new ECS has (views,
// change ticks, memory statistics) is behind JE_BENCH_NEW_ECS.
//
//   jasperengine-ecs-bench [--sizes 10000,100000] [--runs 7] [--ops create,destroy,...]
//
// Prints CSV rows "op,n,run,ms,ops" (ops: operations the ms covers, for ns/op) and
// "metric,n,name,value". Behaviour assertions abort with exit code 2.

#include "Engine.h"
#include "Scene.h"
#include "System.h"
#include "component/Transform.h"

#if __has_include("ChangeTracking.h")
#define JE_BENCH_NEW_ECS 1
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    struct Payload {
        std::string name;
        std::vector<int> values;
        std::shared_ptr<int> shared;
    };

    class BenchSystem : public System {
    public:
        int getScreenWidth() override { return 640; }
        int getScreenHeight() override { return 360; }
    };

    class BenchScene : public Scene {
    public:
        BenchScene(){
            registerComponent<Payload>();
        }
    };

    void check(bool condition, const char* what){
        if (!condition){
            std::fprintf(stderr, "behaviour assertion failed: %s\n", what);
            std::exit(2);
        }
    }

    using Clock = std::chrono::steady_clock;

    // --ops: the benchmark groups to run (all when empty): create, destroy, remove, add,
    // index_loop (index loops and the view), lookup (lookup and metadata), move_branch
    std::vector<std::string> selectedOps;

    bool wanted(const char* op){
        if (selectedOps.empty()) return true;
        const std::string name(op);
        const std::string group = name == "view" ? "index_loop" : name == "metadata" ? "lookup" : name;
        return std::find(selectedOps.begin(), selectedOps.end(), group) != selectedOps.end();
    }

    double msSince(Clock::time_point start){
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    }

    void row(const char* op, size_t n, int run, double ms, size_t ops = 1){
        std::printf("%s,%zu,%d,%.6f,%zu\n", op, n, run, ms, ops);
        std::fflush(stdout);
    }

    void metric(const char* name, size_t n, double value){
        std::printf("metric,%zu,%s,%.4f\n", n, name, value);
        std::fflush(stdout);
    }

    Payload makePayload(size_t i){
        return Payload{"payload-" + std::to_string(i), std::vector<int>{(int)i, (int)i + 1, (int)i + 2}, std::make_shared<int>((int)i)};
    }

    // entities with Transform (+ Payload), the first half in chains of depth 4, a Bone on half
    std::vector<Entity> buildWorld(Scene& scene, size_t n, bool payload, bool bones = true){
        std::vector<Entity> entities;
        entities.reserve(n);
        for (size_t i = 0; i < n; i++){
            const Entity entity = scene.createEntity();
            scene.addComponent<Transform>(entity);
            if (payload) scene.addComponent<Payload>(entity, makePayload(i));
            entities.push_back(entity);
        }
        for (size_t i = 0; i + 3 < n / 2; i += 4){
            for (size_t k = 1; k < 4; k++){
                scene.addEntityChild(entities[i + k - 1], entities[i + k], false);
            }
        }
        if (bones){
            for (size_t i = 0; i < n; i += 2){
                scene.addComponent<BoneComponent>(entities[i]);
            }
        }
        return entities;
    }

    // parents before children in the Transform array
    void checkHierarchy(Scene& scene){
        auto transforms = scene.getComponentArray<Transform>();
        for (size_t i = 0; i < transforms->size(); i++){
            const Entity parent = transforms->getComponentFromIndex(i).parent;
            if (parent != NULL_ENTITY && transforms->hasEntity(parent)){
                check(transforms->getIndex(parent) < i, "parent before child");
            }
        }
    }

    template<typename T>
    void checkTransformOrder(Scene& scene, const char* what){
        auto transforms = scene.getComponentArray<Transform>();
        auto array = scene.getComponentArray<T>();
        size_t previous = 0;
        for (size_t i = 0; i < array->size(); i++){
            const Entity entity = array->getEntity(i);
            const size_t key = transforms->hasEntity(entity) ? transforms->getIndex(entity) : (size_t)-1;
            check(previous <= key, what);
            previous = key;
        }
    }

    // the propagation shape: index loop with a parent lookup by entity
    __attribute__((noinline)) double indexLoop(Scene& scene){
        auto transforms = scene.getComponentArray<Transform>();
        double sum = 0.0;
        for (size_t i = 0; i < transforms->size(); i++){
            Transform& transform = transforms->getComponentFromIndex(i);
            sum += (double)transforms->getEntity(i);
            if (transform.parent != NULL_ENTITY){
                if (Transform* parent = scene.findComponent<Transform>(transform.parent)){
                    sum += parent->position.x;
                }
            }
            sum += transform.position.y;
        }
        return sum;
    }

    // what a Read<Transform>, Write<Payload> view does, written as an index loop
    __attribute__((noinline)) size_t indexLoopWithPayload(Scene& scene){
        auto transforms = scene.getComponentArray<Transform>();
        size_t sum = 0;
        for (size_t i = 0; i < transforms->size(); i++){
            const Entity entity = transforms->getEntity(i);
            if (Payload* payload = scene.findComponent<Payload>(entity)){
                payload->values[0] += 1;
                sum += payload->values.size() + (size_t)transforms->getComponentFromIndex(i).visible;
            }
        }
        return sum;
    }

    void runSize(size_t n, int runs){
        std::mt19937 random((unsigned)n);

        for (int run = 0; run < runs; run++){
            // create entity + 2 components
            if (wanted("create"))
            {
                BenchScene scene;
                const auto start = Clock::now();
                for (size_t i = 0; i < n; i++){
                    const Entity entity = scene.createEntity();
                    scene.addComponent<Transform>(entity);
                    scene.addComponent<Payload>(entity, makePayload(i));
                }
                row("create", n, run, msSince(start));
                check(scene.getComponentArray<Payload>()->size() == n, "create: every Payload added");
            }

            // destroyEntity one by one (half of them, random order) + first observation. Before
            // S07 that is quadratic: at 100k it takes hours, and R26 compares it at 10k only.
            if (wanted("destroy")
#if !JE_BENCH_NEW_ECS
                && n <= 10000
#endif
            )
            {
                BenchScene scene;
                std::vector<Entity> entities = buildWorld(scene, n, true);
                std::shuffle(entities.begin(), entities.end(), random);
                const size_t doomed = n / 2;
                const auto start = Clock::now();
                for (size_t i = 0; i < doomed; i++){
                    scene.destroyEntity(entities[i]);
                }
                Entity first = scene.getComponentArray<Transform>()->getEntity(0);
                first += scene.getComponentArray<Payload>()->getEntity(0);
                first += scene.getComponentArray<BoneComponent>()->getEntity(0);
                row("destroy", n, run, msSince(start));
                check(first != NULL_ENTITY, "destroy: survivors");
                check(scene.getComponentArray<Transform>()->size() == n - doomed, "destroy: Transform count");
                check(scene.getComponentArray<Payload>()->size() == n - doomed, "destroy: Payload count");
                if (run == 0) checkHierarchy(scene);
            }

            // removeComponent<Payload> for 10% random + first observation
            if (wanted("remove"))
            {
                BenchScene scene;
                std::vector<Entity> entities = buildWorld(scene, n, true);
                std::shuffle(entities.begin(), entities.end(), random);
                const size_t removed = n / 10;
                const auto start = Clock::now();
                for (size_t i = 0; i < removed; i++){
                    scene.removeComponent<Payload>(entities[i]);
                }
                const Entity first = scene.getComponentArray<Payload>()->getEntity(0);
                row("remove", n, run, msSince(start));
                check(first != NULL_ENTITY, "remove: survivors");
                check(scene.getComponentArray<Payload>()->size() == n - removed, "remove: Payload count");
            }

            // addComponent<Payload> to every entity
            if (wanted("add"))
            {
                BenchScene scene;
                const std::vector<Entity> entities = buildWorld(scene, n, false);
                std::vector<Payload> payloads;
                payloads.reserve(n);
                for (size_t i = 0; i < n; i++) payloads.push_back(makePayload(i));
                const auto start = Clock::now();
                for (size_t i = 0; i < n; i++){
                    scene.addComponent<Payload>(entities[i], std::move(payloads[i]));
                }
                row("add", n, run, msSince(start));
                check(scene.getComponentArray<Payload>()->size() == n, "add: every Payload added");
            }
        }

        // the iteration, lookup and move benchmarks share one world
        BenchScene scene;
        std::vector<Entity> entities = buildWorld(scene, n, true);
        checkHierarchy(scene);
        checkTransformOrder<BoneComponent>(scene, "Bone array in Transform order");
        double sink = indexLoop(scene); // warm, settled
        size_t sinkCount = indexLoopWithPayload(scene);

        for (int run = 0; run < runs && (wanted("index_loop") || wanted("view")); run++){
            const int repeats = 20;
            auto start = Clock::now();
            for (int r = 0; r < repeats; r++) sink += indexLoop(scene);
            row("index_loop", n, run, msSince(start) / repeats);

            start = Clock::now();
            for (int r = 0; r < repeats; r++) sinkCount += indexLoopWithPayload(scene);
            row("index_loop_payload", n, run, msSince(start) / repeats);

#if JE_BENCH_NEW_ECS
            start = Clock::now();
            for (int r = 0; r < repeats; r++){
                scene.view<Read<Transform>, Write<Payload>>().each([&sinkCount](Entity, const Transform& transform, Payload& payload){
                    payload.values[0] += 1;
                    sinkCount += payload.values.size() + (size_t)transform.visible;
                });
            }
            row("view", n, run, msSince(start) / repeats);
#endif
        }

        // random findComponent and metadata getters (getEntityName-free)
        std::vector<Entity> lookups(1000000);
        for (Entity& entity : lookups) entity = entities[random() % entities.size()];
        for (int run = 0; run < runs && (wanted("lookup") || wanted("metadata")); run++){
            size_t sum = 0;
            auto start = Clock::now();
            for (Entity entity : lookups){
                if (Payload* payload = scene.findComponent<Payload>(entity)) sum += payload->values.size();
                sum += (size_t)scene.getEntityLayer(entity);
                sum += scene.getSignature(entity).count();
            }
            row("lookup", n, run, msSince(start), lookups.size() * 3);
            start = Clock::now();
            for (Entity entity : lookups){
                sum += (size_t)scene.getEntityLayer(entity);
                sum += scene.getSignature(entity).count();
            }
            row("metadata", n, run, msSince(start), lookups.size() * 2);
            sinkCount += sum;
        }

        // addEntityChild moving a 100-entity branch + the 10 Hierarchy arrays re-sorted
        if (wanted("move_branch"))
        {
            BenchScene world;
            std::vector<Entity> worldEntities = buildWorld(world, n, true);
            // 2% of the world has one of the other nine Hierarchy types
            for (size_t i = 0; i < n; i += 50){
                switch ((i / 50) % 9){
                    case 0: world.addComponent<MeshComponent>(worldEntities[i]); break;
                    case 1: world.addComponent<ModelComponent>(worldEntities[i]); break;
                    case 2: world.addComponent<PolygonComponent>(worldEntities[i]); break;
                    case 3: world.addComponent<RectTransformComponent>(worldEntities[i]); break;
                    case 4: world.addComponent<UIComponent>(worldEntities[i]); break;
                    case 5: world.addComponent<PointsComponent>(worldEntities[i]); break;
                    case 6: world.addComponent<LineRendererComponent>(worldEntities[i]); break;
                    case 7: world.addComponent<AudioSourceComponent>(worldEntities[i]); break;
                    default: world.addComponent<InstancedMeshComponent>(worldEntities[i]); break;
                }
            }
            // the branch: a root with every Hierarchy type, 99 descendants in chains of 9
            std::vector<Entity> branch;
            for (size_t i = 0; i < 100; i++){
                const Entity entity = world.createEntity();
                world.addComponent<Transform>(entity);
                world.addComponent<Payload>(entity, makePayload(i));
                branch.push_back(entity);
            }
            for (size_t i = 1; i < 100; i++){
                world.addEntityChild(i % 9 == 1 ? branch[0] : branch[i - 1], branch[i], false);
            }
            const Entity root = branch[0];
            world.addComponent<MeshComponent>(root);
            world.addComponent<ModelComponent>(root);
            world.addComponent<BoneComponent>(root);
            world.addComponent<PolygonComponent>(root);
            world.addComponent<RectTransformComponent>(root);
            world.addComponent<UIComponent>(root);
            world.addComponent<PointsComponent>(root);
            world.addComponent<LineRendererComponent>(root);
            world.addComponent<AudioSourceComponent>(root);
            world.addComponent<InstancedMeshComponent>(root);
            for (size_t i = 1; i < 100; i++){
                world.addComponent<BoneComponent>(branch[i]);
            }
            // settled before each move
            auto observeAll = [&world](){
                Entity sum = 0;
                sum += world.getComponentArray<MeshComponent>()->getEntity(0);
                sum += world.getComponentArray<ModelComponent>()->getEntity(0);
                sum += world.getComponentArray<BoneComponent>()->getEntity(0);
                sum += world.getComponentArray<PolygonComponent>()->getEntity(0);
                sum += world.getComponentArray<RectTransformComponent>()->getEntity(0);
                sum += world.getComponentArray<UIComponent>()->getEntity(0);
                sum += world.getComponentArray<PointsComponent>()->getEntity(0);
                sum += world.getComponentArray<LineRendererComponent>()->getEntity(0);
                sum += world.getComponentArray<AudioSourceComponent>()->getEntity(0);
                sum += world.getComponentArray<InstancedMeshComponent>()->getEntity(0);
                return sum;
            };
            observeAll();
            // destinations: top-level GameObjects about a thousand entities before the branch
            auto transforms = world.getComponentArray<Transform>();
            for (int run = 0; run < runs; run++){
                const size_t at = transforms->getIndex(root);
                size_t target = at > 1000 ? at - 1000 - (size_t)run * 7 : 0;
                Entity parent = transforms->getEntity(target);
                while (transforms->getComponentFromIndex(target).parent != NULL_ENTITY && target > 0){
                    parent = transforms->getEntity(--target);
                }
                const auto start = Clock::now();
                world.addEntityChild(parent, root, false);
                sinkCount += observeAll();
                row("move_branch", n, run, msSince(start));
                check(world.getComponent<Transform>(root).parent == parent, "move: reparented");
                world.addEntityChild(NULL_ENTITY, root, false);
                observeAll();
            }
            checkHierarchy(world);
            checkTransformOrder<BoneComponent>(world, "move: Bone array in Transform order");
            checkTransformOrder<MeshComponent>(world, "move: Mesh array in Transform order");
            checkTransformOrder<UIComponent>(world, "move: UI array in Transform order");
        }

#if JE_BENCH_NEW_ECS
        // R27: bytes per component (index pages, slot owners, ticks; the components left out)
        // and per entity (metadata, names' heap buffers left out)
        size_t overhead = 0, components = 0;
        for (size_t id = 0; id < scene.getComponentTypeCount(); id++){
            ComponentArrayBase* array = scene.getComponentArray((ComponentId)id);
            if (array->size() == 0) continue;
            overhead += array->overheadBytes();
            components += array->size();
        }
        metric("bytes_per_component", n, (double)overhead / (double)components);
        metric("bytes_per_entity", n, (double)scene.getMetadataBytes() / (double)scene.getEntityCount());
#endif
        if (sink == 0.125 && sinkCount == 3) std::printf("#\n"); // keeps the loops
    }

}

int main(int argc, char** argv){
    std::vector<size_t> sizes{10000, 100000};
    int runs = 7;
    for (int i = 1; i < argc; i++){
        if (std::strcmp(argv[i], "--sizes") == 0 && i + 1 < argc){
            sizes.clear();
            std::string list = argv[++i];
            size_t pos = 0;
            while (pos < list.size()){
                size_t comma = list.find(',', pos);
                if (comma == std::string::npos) comma = list.size();
                sizes.push_back((size_t)std::stoul(list.substr(pos, comma - pos)));
                pos = comma + 1;
            }
        }else if (std::strcmp(argv[i], "--ops") == 0 && i + 1 < argc){
            std::string list = argv[++i];
            size_t pos = 0;
            while (pos < list.size()){
                size_t comma = list.find(',', pos);
                if (comma == std::string::npos) comma = list.size();
                selectedOps.push_back(list.substr(pos, comma - pos));
                pos = comma + 1;
            }
        }else if (std::strcmp(argv[i], "--runs") == 0 && i + 1 < argc){
            runs = std::atoi(argv[++i]);
        }
    }

    Engine::systemInit(0, nullptr, new BenchSystem());
    std::printf("op,n,run,ms,ops\n");
    for (size_t n : sizes){
        runSize(n, runs);
    }
    return 0;
}
