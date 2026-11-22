// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S26's test project (docs/specs/S26-scene-view.md R28, AC17): MeshRaycast's latency in a build
// without pthreads, WebAssembly on the scalar path. 1,000 spheres of about 1,000 triangles each
// (one shared BVH) in a 200-unit cloud; once they are placed, 10,000 rays from outside the cloud
// toward random points in it, each timed; the p99 must stay at or under 4 ms. Checked in a browser
// (BUILD_TYPE=Release WEB_PROJECT=mesh-raycast tools/build.sh web, GATE=mesh-raycast tools/build.sh web-gate) by the
// line "[gate] mesh raycast p99 within 4 ms" (an error line otherwise, which fails the gate).
//
// spatial-queries AC15 in the same build: the spheres get static colliders and 1,000 more bodies
// make 2,000; then 10,000 Lua Physics.overlapSphereNonAlloc and 10,000 MeshRaycast.overlapSphere
// calls (r 5), timed in batches of 100 (a browser's clock is too coarse for one call), each p99 at
// or under 0.04 ms: "[gate] physics overlap p99 within 0.04 ms", "[gate] mesh overlap p99 within
// 0.04 ms".

#include "JasperEngine.h"
#include "picking/MeshBvh.h"
#include "picking/MeshRaycast.h"
#include "component/RigidbodyComponent.h"
#include "script/LuaBinding.h"

#include "lua.hpp"
#include "LuaBridge.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <random>
#include <vector>

using namespace jasperengine;

namespace {

    constexpr int SPHERES = 1000;
    constexpr int RAYS = 10000;
    constexpr double BUDGET_MS = 4.0;
    constexpr int BODIES = 2000;
    constexpr int QUERIES = 10000;
    constexpr int QUERY_BATCH = 100;
    constexpr double QUERY_BUDGET_MS = 0.04;

    Scene scene;
    Camera sceneCamera(&scene);
    std::vector<std::unique_ptr<Primitive>> spheres;
    int frames = 0;

    // p99 of the per-call time over batches of QUERY_BATCH calls of query(i)
    template<typename Query>
    double batchedP99(Query&& query){
        std::vector<double> times;
        for (int b = 0; b < QUERIES / QUERY_BATCH; b++){
            const auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < QUERY_BATCH; i++) query(b * QUERY_BATCH + i);
            times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / QUERY_BATCH);
        }
        std::sort(times.begin(), times.end());
        return times[(size_t)(times.size() * 0.99)];
    }

    void gateQuery(const char* name, double p99){
        Log::print("[gate] %s overlap p99 %.4f ms per call over %d calls", name, p99, QUERIES);
        if (p99 <= QUERY_BUDGET_MS){
            Log::print("[gate] %s overlap p99 within 0.04 ms", name);
        }else{
            Log::error("[gate] %s overlap p99 %.4f ms over the 0.04 ms budget", name, p99);
        }
    }

    RigidbodyComponent staticBody(Collider3DType type){
        RigidbodyComponent body;
        body.type = BodyType::STATIC;
        body.colliders.resize(1);
        body.colliders[0].type = type;
        body.colliders[0].radius = 1.5f;
        return body;
    }

}

JASPERENGINE_INIT void init(){
    Engine::setCanvasSize(640, 360);
    Engine::setScene(&scene);
    scene.setBackgroundColor(0.1f, 0.1f, 0.12f);

    sceneCamera.setType(CameraType::CAMERA_PERSPECTIVE);
    sceneCamera.setPosition(0.0f, 0.0f, 260.0f);
    sceneCamera.setTarget(Vector3(0.0f, 0.0f, 0.0f));
    scene.setCamera(&sceneCamera);

    std::mt19937 rng(26);
    std::uniform_real_distribution<float> place(-100.0f, 100.0f);
    spheres.reserve(SPHERES);
    for (int i = 0; i < SPHERES; i++){
        auto sphere = std::make_unique<Primitive>(&scene);
        // 25 slices x 20 stacks: about 1,000 triangles
        sphere->createSphere(1.5f, 25, 20);
        sphere->setPosition(place(rng), place(rng), place(rng));
        scene.addComponent<RigidbodyComponent>(sphere->getEntity(), staticBody(Collider3DType::SPHERE));
        spheres.push_back(std::move(sphere));
    }
    // bodies with no mesh: 2,000 in all
    for (int i = SPHERES; i < BODIES; i++){
        const Entity entity = scene.createEntity();
        Transform transform;
        transform.position = Vector3(place(rng), place(rng), place(rng));
        scene.addComponent<Transform>(entity, transform);
        scene.addComponent<RigidbodyComponent>(entity, staticBody(Collider3DType::BOX));
    }

    Engine::onUpdate.add("meshRaycastLatency", [](){
        if (frames < 0){
            return;
        }
        // a few frames: the meshes are loaded and their world bounds computed
        if (++frames < 5){
            return;
        }
        frames = -1;

        Log::print("[gate] mesh raycast simd %s", pickSimdPathName(getPickSimdPath()));
        // the first query builds the shared tree inline (no background workers here)
        MeshRaycast::raycast(&scene, Vector3(0.0f, 0.0f, 300.0f), Vector3(0.0f, 0.0f, -1.0f), 1000.0f);

        std::mt19937 rng(7);
        std::uniform_real_distribution<float> inside(-100.0f, 100.0f);
        std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
        std::vector<double> times;
        times.reserve(RAYS);
        int hits = 0;
        for (int i = 0; i < RAYS; i++){
            Vector3 direction(unit(rng), unit(rng), unit(rng));
            if (direction.squaredLength() < 1e-6f){
                direction = Vector3(0.0f, 0.0f, -1.0f);
            }
            direction.normalize();
            const Vector3 target(inside(rng), inside(rng), inside(rng));
            const Vector3 origin = target - direction * 400.0f;
            const auto start = std::chrono::steady_clock::now();
            const MeshRaycastHit hit = MeshRaycast::raycast(&scene, origin, direction, 1000.0f);
            times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
            hits += hit.hit ? 1 : 0;
        }
        std::sort(times.begin(), times.end());
        const double p99 = times[(size_t)(times.size() * 0.99)];
        const double p50 = times[times.size() / 2];
        Log::print("[gate] mesh raycast p50 %.3f ms p99 %.3f ms over %d rays (%d hits)", p50, p99, RAYS, hits);
        if (hits == 0){
            Log::error("[gate] mesh raycast: no ray hit a sphere");
        }else if (p99 <= BUDGET_MS){
            Log::print("[gate] mesh raycast p99 within 4 ms");
        }else{
            Log::error("[gate] mesh raycast p99 %.3f ms over the 4 ms budget", p99);
        }

        // spatial queries: Lua's non-allocating overlap on the 2,000 bodies, and the drawn bounds
        std::vector<Vector3> centers;
        for (int i = 0; i < QUERIES; i++) centers.push_back(Vector3(inside(rng), inside(rng), inside(rng)));
        lua_State* L = LuaBinding::getLuaState();
        luabridge::setGlobal(L, &scene, "gateScene");
        luaL_dostring(L, "gateResults = {} gateCenter = Vector3(0, 0, 0) "
                         "function gateQuery(x, y, z) gateCenter.x = x gateCenter.y = y gateCenter.z = z "
                         "return Physics.overlapSphereNonAlloc(gateScene, gateCenter, 5, gateResults) end");
        lua_getglobal(L, "gateQuery");
        const int function = luaL_ref(L, LUA_REGISTRYINDEX);
        long long found = 0;
        auto luaQuery = [&](int i){
            lua_rawgeti(L, LUA_REGISTRYINDEX, function);
            lua_pushnumber(L, centers[i].x);
            lua_pushnumber(L, centers[i].y);
            lua_pushnumber(L, centers[i].z);
            lua_call(L, 3, 1);
            found += (long long)lua_tointeger(L, -1);
            lua_pop(L, 1);
        };
        luaQuery(0);
        const double physicsP99 = batchedP99(luaQuery);
        luaL_unref(L, LUA_REGISTRYINDEX, function);
        if (found == 0){
            Log::error("[gate] physics overlap: no query found a body");
        }else{
            gateQuery("physics", physicsP99);
        }
        std::vector<MeshOverlapHit> drawn;
        drawn.reserve(256);
        size_t seen = MeshRaycast::overlapSphere(&scene, centers[0], 5.0f, drawn);
        const double meshP99 = batchedP99([&](int i){ seen += MeshRaycast::overlapSphere(&scene, centers[i], 5.0f, drawn); });
        if (seen == 0){
            Log::error("[gate] mesh overlap: no query found a sphere");
        }else{
            gateQuery("mesh", meshP99);
        }
    });
}
