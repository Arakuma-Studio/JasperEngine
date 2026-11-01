// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S26's test project (docs/specs/S26-scene-view.md R28, AC17): MeshRaycast's latency in a build
// without pthreads, WebAssembly on the scalar path. 1,000 spheres of about 1,000 triangles each
// (one shared BVH) in a 200-unit cloud; once they are placed, 10,000 rays from outside the cloud
// toward random points in it, each timed; the p99 must stay at or under 4 ms. Checked in a browser
// (WEB_PROJECT=mesh-raycast tools/build.sh web, GATE=mesh-raycast tools/build.sh web-gate) by the
// line "[gate] mesh raycast p99 within 4 ms" (an error line otherwise, which fails the gate).

#include "JasperEngine.h"
#include "picking/MeshBvh.h"
#include "picking/MeshRaycast.h"

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

    Scene scene;
    Camera sceneCamera(&scene);
    std::vector<std::unique_ptr<Primitive>> spheres;
    int frames = 0;

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
        spheres.push_back(std::move(sphere));
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
    });
}
