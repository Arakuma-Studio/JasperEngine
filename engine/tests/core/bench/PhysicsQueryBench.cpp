// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// je-bench-physics-query: the spatial query budgets (docs/specs/spatial-queries.md R19, AC14) of
// what spatial-queries adds to S96's queries (whose own costs PhysicsQueriesBenchTest measures):
// Lua's Physics.overlapSphereNonAlloc on query_city (2,000 bodies over a 200 m mesh ground),
// MeshRaycast overlaps among 10,000 drawn leaves and in T5's foliage scene (50,000 trees, which
// have no leaves: the overlap walks their cells). Each sample times 100 queries; the keys are the
// p95 of 200 samples, per query.
//
//   je-bench-physics-query [--json out.json]

#include "BenchUtil.h"
#include "FoliageBenchScene.h"
#include "JobsTestSystem.h"
#include "../physics/QueryCityTestUtil.h"

#include "Engine.h"
#include "jobs/JobSystem.h"
#include "object/Primitive.h"
#include "picking/MeshRaycast.h"
#include "registry/PhysicsLayers.h"
#include "script/LuaBinding.h"
#include "subsystem/MeshSystem.h"

#include "lua.hpp"
#include "LuaBridge.h"

#include <cstdio>
#include <functional>
#include <random>
#include <vector>

using namespace jasperengine;

namespace {

    constexpr int SAMPLES = 200;
    constexpr int BATCH = 100;

    // p95 of the per-query time (ms) of `query(i)` over SAMPLES batches of BATCH calls
    double p95PerQuery(const std::function<void(int)>& query){
        for (int i = 0; i < BATCH; i++) query(i); // warm
        std::vector<double> samples;
        for (int s = 0; s < SAMPLES; s++){
            const auto start = bench::Clock::now();
            for (int i = 0; i < BATCH; i++) query(s * BATCH + i);
            samples.push_back(bench::secondsSince(start) * 1000.0 / BATCH);
        }
        return bench::percentile(samples, 0.95);
    }

}

int main(int argc, char** argv){
    jobs::Config config;
    config.workers = 2;
    jobs::initialize(config);
    Engine::systemInit(argc, argv, new testsupport::JobsTestSystem());
    bench::Results results("je-bench-physics-query");
    std::mt19937 rng(1);

#ifdef JASPERENGINE_PHYSICS_3D
    {
        test::QueryCity city = test::buildQueryCity3D(2000);
        Scene* scene = city.scene.get();
        std::uniform_real_distribution<float> along(-city.extent * 0.9f, city.extent * 0.9f);
        std::vector<Vector3> centers;
        for (int i = 0; i < SAMPLES * BATCH + BATCH; i++) centers.push_back(Vector3(along(rng), 1.5f, along(rng)));
        lua_State* L = LuaBinding::getLuaState();
        luabridge::setGlobal(L, scene, "benchScene");
        luaL_dostring(L, "benchResults = {} benchCenter = Vector3(0, 1.5, 0) "
                         "function benchQuery(x, z) benchCenter.x = x benchCenter.z = z "
                         "return Physics.overlapSphereNonAlloc(benchScene, benchCenter, 5, benchResults) end");
        lua_getglobal(L, "benchQuery");
        const int function = luaL_ref(L, LUA_REGISTRYINDEX);
        results.add("lua_overlap_nonalloc_p95_ms", p95PerQuery([&](int i){
            lua_rawgeti(L, LUA_REGISTRYINDEX, function);
            lua_pushnumber(L, centers[i].x);
            lua_pushnumber(L, centers[i].z);
            lua_call(L, 2, 1);
            lua_pop(L, 1);
        }), "ms");
        luaL_unref(L, LUA_REGISTRYINDEX, function);
        lua_pushnil(L);
        lua_setglobal(L, "benchScene");
    }
#endif

    {
        // 10,000 drawn leaves, no refit due
        Scene scene;
        std::vector<std::unique_ptr<Primitive>> boxes;
        std::uniform_real_distribution<float> along(-100.0f, 100.0f);
        for (int i = 0; i < 10000; i++){
            auto box = std::make_unique<Primitive>(&scene);
            box->createBox(1, 1, 1);
            box->setPosition(along(rng), along(rng) * 0.1f, along(rng));
            boxes.push_back(std::move(box));
        }
        scene.getSystem<MeshSystem>()->update(0);
        scene.getSystem<RenderSystem>()->update(0);
        std::vector<Vector3> centers;
        for (int i = 0; i < SAMPLES * BATCH + BATCH; i++) centers.push_back(Vector3(along(rng), 0.0f, along(rng)));
        std::vector<MeshOverlapHit> hits;
        hits.reserve(1024);
        results.add("mesh_overlap_p95_ms", p95PerQuery([&](int i){ MeshRaycast::overlapSphere(&scene, centers[i], 5.0f, hits); }), "ms");
    }

    {
        // 50,000 trees over 1 km, all resident: spheres at trees' middles, the default kinds
        foliagebench::Scene1 forest("physics-query-trees");
        forest.settle();
        const std::shared_ptr<terraincore::TreeStore> store = forest.foliage->getTreeStore(forest.entity);
        std::vector<Vector3> centers;
        for (int i = 0; i < SAMPLES * BATCH + BATCH; i++){
            FoliageInstanceInfo pose;
            const uint32_t record = store->records()[(size_t)(i * 7919) % store->records().size()].id;
            forest.foliage->getTreePose(forest.entity, terraincore::makeTreeId(0, record), pose);
            centers.push_back(pose.position + Vector3(0.0f, 2.0f, 0.0f));
        }
        std::vector<MeshOverlapHit> hits;
        hits.reserve(1024);
        size_t found = 0;
        results.add("mesh_overlap_trees_p95_ms", p95PerQuery([&](int i){ found += MeshRaycast::overlapSphere(forest.scene.get(), centers[i], 5.0f, hits); }), "ms");
        if (found == 0){
            std::fprintf(stderr, "mesh_overlap_trees: no tree found: the scene did not load\n");
            return 1;
        }
    }

    const char* path = bench::jsonPath(argc, argv, "je-bench-physics-query.json");
    if (!results.write(path)){
        std::fprintf(stderr, "cannot write %s\n", path);
        return 1;
    }
    std::printf("wrote %s\n", path);
    jobs::shutdown();
    return 0;
}
