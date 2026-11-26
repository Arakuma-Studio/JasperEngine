// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Culling Group and Simulation Culling budgets (docs/specs/culling-group-and-simulation-
// distance.md R22, AC16):
//   je-bench-culling [--json <file>]
// Every figure is the median of repeated runs on the main thread (Release); check_bench.py gates
// them with tools/perf/bench-thresholds.json (the desktop budgets + 10 %).

#include "BenchUtil.h"
#include "JobsTestSystem.h"

#include "Engine.h"
#include "Scene.h"
#include "component/CameraComponent.h"
#include "component/RigidbodyComponent.h"
#include "component/SimulationCullingComponent.h"
#include "component/Transform.h"
#include "object/CullingGroup.h"
#include "object/SimulationCulling.h"
#include "subsystem/CullingSystem.h"
#include "subsystem/PhysicsSystem.h"
#include "subsystem/TransformPropagation.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    template<typename Fn>
    double medianMs(int runs, Fn&& fn){
        std::vector<double> samples;
        for (int r = 0; r < runs; r++){
            const auto start = bench::Clock::now();
            fn();
            samples.push_back(bench::secondsSince(start) * 1.0e3);
        }
        return bench::percentile(samples, 0.5);
    }

    std::unique_ptr<Scene> makeScene(){
        auto scene = std::make_unique<Scene>();
        const Entity camera = scene->createEntity();
        scene->addComponent<Transform>(camera, Transform());
        CameraComponent component;
        component.type = CameraType::CAMERA_PERSPECTIVE;
        // a 90 degree frustum along +Z, 500 m deep
        const float k = 0.70710678f;
        const Vector3 normals[6] = {Vector3(0, 0, 1), Vector3(0, 0, -1), Vector3(k, 0, k), Vector3(-k, 0, k), Vector3(0, -k, k), Vector3(0, k, k)};
        for (int p = 0; p < 6; p++){
            component.frustumPlanes[p].normal = normals[p];
            component.frustumPlanes[p].d = 0.0f;
        }
        component.frustumPlanes[1].d = 500.0f;
        component.farClip = 500.0f;
        scene->addComponent<CameraComponent>(camera, component);
        scene->setCamera(camera);
        return scene;
    }

    Entity object(Scene& scene, const Vector3& position){
        const Entity entity = scene.createEntity();
        Transform transform;
        transform.position = position;
        scene.addComponent<Transform>(entity, transform);
        return entity;
    }

}

int main(int argc, char** argv){
    Engine::systemInit(argc, argv, new testsupport::JobsTestSystem());
    bench::Results results("je-bench-culling");
    const float distances[] = {25.0f, 50.0f, 100.0f, 200.0f};

    // ---- one group, 10,000 fixed spheres, 4 distances, one camera, no change
    {
        auto scene = makeScene();
        CullingSystem* culling = scene->getSystem<CullingSystem>().get();
        updateWorldTransforms(*scene);
        CullingGroup group(scene.get());
        group.setBoundingDistances(distances);
        std::vector<BoundingSphere> spheres(10000);
        for (size_t i = 0; i < spheres.size(); i++)
            spheres[i] = BoundingSphere(Vector3(static_cast<float>(i % 100) - 50.0f, 0.0f, static_cast<float>(i / 100) * 3.0f), 1.0f);
        group.setBoundingSpheres(spheres);
        culling->update(0.016);
        results.add("group_10k_eval_ms", medianMs(101, [&](){ culling->update(0.016); }), "ms");
    }

    // ---- the same, every sphere following a GameObject
    {
        auto scene = makeScene();
        CullingSystem* culling = scene->getSystem<CullingSystem>().get();
        CullingGroup group(scene.get());
        group.setBoundingDistances(distances);
        group.setBoundingSphereCount(10000);
        std::vector<Entity> targets;
        for (uint32_t i = 0; i < 10000; i++)
            targets.push_back(object(*scene, Vector3(static_cast<float>(i % 100) - 50.0f, 0.0f, static_cast<float>(i / 100) * 3.0f)));
        updateWorldTransforms(*scene);
        for (uint32_t i = 0; i < 10000; i++) group.setSphereTarget(i, targets[i], 1.0f);
        culling->update(0.016);
        results.add("group_10k_follow_eval_ms", medianMs(101, [&](){ culling->update(0.016); }), "ms");
    }

    // ---- 1,000 Simulation Culling components, 2 focuses, no state change; then 64 changes; then
    // the ground rule's round robin with a script check
    {
        auto scene = makeScene();
        CullingSystem* culling = scene->getSystem<CullingSystem>().get();
        std::vector<Entity> objects;
        for (int i = 0; i < 1000; i++){
            const Entity entity = object(*scene, Vector3(static_cast<float>(i % 40) * 4.0f - 80.0f, 0.0f, static_cast<float>(i / 40) * 4.0f));
            SimulationCullingComponent component;
            component.simulationDistance = 300.0f;
            component.boundsRadius = 0.5f;
            component.freezeWhenGroundNotLoaded = false;
            scene->addComponent<SimulationCullingComponent>(entity, component);
            objects.push_back(entity);
        }
        SimulationCulling::addFocus(scene.get(), Vector3(0.0f, 0.0f, 50.0f));
        updateWorldTransforms(*scene);
        culling->evaluateNow();
        results.add("simulation_1k_eval_ms", medianMs(101, [&](){ culling->update(0.016); }), "ms");

        // 64 state changes: 64 of them pushed past the distance and back, alternately
        std::vector<double> samples;
        for (int r = 0; r < 41; r++){
            const float far = (r % 2) ? 0.0f : 10000.0f;
            for (int i = 0; i < 64; i++){
                Transform& transform = scene->getComponent<Transform>(objects[size_t(i)]);
                transform.position.x = far + static_cast<float>(i);
                transform.worldPosition = transform.position;
            }
            const auto start = bench::Clock::now();
            culling->update(0.016);
            samples.push_back(bench::secondsSince(start) * 1.0e3);
        }
        results.add("simulation_64_transitions_ms", bench::percentile(samples, 0.5), "ms");

        // the ground rule's share: the same evaluation with the round robin on and a check that
        // answers at once, minus the evaluation without it
        auto components = scene->getComponentArray<SimulationCullingComponent>();
        for (size_t i = 0; i < components->size(); i++) components->getComponentFromIndex(i).freezeWhenGroundNotLoaded = true;
        SimulationCulling::setGroundCheck(scene.get(), [](const Vector3&, float){ return true; });
        culling->update(0.016);
        const double withGround = medianMs(101, [&](){ culling->update(0.016); });
        for (size_t i = 0; i < components->size(); i++) components->getComponentFromIndex(i).freezeWhenGroundNotLoaded = false;
        const double without = medianMs(101, [&](){ culling->update(0.016); });
        results.add("ground_1k_ms", std::max(0.0, withGround - without), "ms");
    }

    // ---- no group, no component: the system and the guard's load, 1,000 script callbacks
    {
        auto scene = makeScene();
        CullingSystem* culling = scene->getSystem<CullingSystem>().get();
        volatile int sink = 0;
        results.add("culling_zero_cost_ms", medianMs(201, [&](){
            culling->update(0.016);
            for (int i = 0; i < 1000; i++){
                if (detail::simulationCullingActive.load(std::memory_order_relaxed)) sink = sink + 1;
            }
        }), "ms");
    }

    const char* path = bench::jsonPath(argc, argv, "je-bench-culling.json");
    if (!results.write(path)){
        std::fprintf(stderr, "cannot write %s\n", path);
        return 1;
    }
    std::printf("wrote %s\n", path);
    return 0;
}
