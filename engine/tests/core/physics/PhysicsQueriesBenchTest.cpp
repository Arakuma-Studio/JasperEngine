// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The shape queries' budgets (docs/specs/S96-physics-gaps.md REQ-41, AC-15): one sphereCast
// or overlapSphere against 1000 static boxes, at most 16 hits, within 15 us from C++ and
// checkSphere within 8 us (desktop, the median of 5 runs; mobile and web get 4x), and no heap
// allocation when the output vector has the capacity. Like every *Bench suite the budgets are
// enforced with JASPER_TEST_PERF_SLACK (the benchmark run), failing at 1.5x, and only printed
// otherwise (docs/testing.md); the allocation count is checked everywhere it can be counted.

#ifdef JASPERENGINE_PHYSICS_3D

#include "Engine.h"
#include "Scene.h"
#include "component/RigidbodyComponent.h"
#include "component/Transform.h"
#include "physics/PhysicsQueries3D.h"
#include "subsystem/PhysicsSystem.h"
#include "subsystem/RenderSystem.h"
#include "support/AllocationCounter.h"
#include "../ecs/EcsTestUtil.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <vector>

using namespace jasperengine;

namespace {

    constexpr float DT = 1.0f / 60.0f;
    constexpr int GRID = 10;
    constexpr float SPACING = 3.0f;
    constexpr int QUERIES = 1000;

    // 10 x 10 x 10 static 1 m boxes, 3 m apart
    struct QueryBenchScene{
        std::unique_ptr<Scene> scene = std::make_unique<Scene>();
        float updateTime = Engine::getUpdateTime();

        QueryBenchScene(){
            Engine::setUpdateTime(DT);
            test::disableSubSystems(*scene);
            scene->getScheduler().setEnabled("PhysicsSystem", true);
            for (int x = 0; x < GRID; x++){
                for (int y = 0; y < GRID; y++){
                    for (int z = 0; z < GRID; z++){
                        const Entity entity = test::makeTransformEntity(*scene);
                        Transform& transform = scene->getComponent<Transform>(entity);
                        transform.position = Vector3(x * SPACING, y * SPACING, z * SPACING);
                        scene->getSystem<RenderSystem>()->updateTransform(transform, entity);
                        RigidbodyComponent rigidbody;
                        rigidbody.type = BodyType::STATIC;
                        rigidbody.colliders.resize(1);
                        rigidbody.colliders[0].type = Collider3DType::BOX;
                        scene->addComponent<RigidbodyComponent>(entity, rigidbody);
                    }
                }
            }
            scene->fixedUpdate(DT);
        }

        ~QueryBenchScene(){
            scene.reset();
            Engine::setUpdateTime(updateTime);
        }
    };

    // the start of query i: along a row of boxes (between them), cycling through the grid
    Vector3 rowStart(int i){
        return Vector3(-3.0f, (float)(i % GRID) * SPACING, (float)((i / GRID) % GRID) * SPACING);
    }

    // the median over 5 runs of the microseconds a query takes
    double medianMicros(const std::function<void(int)>& query){
        std::vector<double> runs;
        for (int run = 0; run < 5; run++){
            const auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < QUERIES; i++){
                query(i);
            }
            runs.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / QUERIES);
        }
        std::sort(runs.begin(), runs.end());
        return runs[2];
    }

    void checkBudget(const char* name, double micros, double budget){
        std::printf("[bench] %s in 1000 static boxes %.2f us (budget %.0f us desktop)\n", name, micros, budget);
        if (testsupport::perfSlack() > 0.0){
            EXPECT_LE(micros, budget * 1.5 * testsupport::perfSlack()) << name;
        }
    }

}

TEST(PhysicsQueriesBench, SphereCastThousandBoxes){
    QueryBenchScene bench;
    Scene* scene = bench.scene.get();
    const Vector3 along(1, 0, 0);
    std::vector<RayReturn> hits;
    hits.reserve(16);
    std::vector<ColliderRef> colliders;
    colliders.reserve(16);
    QueryOptions sixteen;
    sixteen.maxHits = 16;

    // every row is 10 boxes: the cast hits them all, the overlap (radius 2 between four boxes) a few
    ASSERT_EQ(PhysicsQueries3D::sphereCastAll(scene, rowStart(0), 0.4f, along, 40.0f, hits, sixteen), (size_t)GRID);
    ASSERT_TRUE(PhysicsQueries3D::sphereCast(scene, rowStart(0), 0.4f, along, 40.0f).hit);
    ASSERT_GT(PhysicsQueries3D::overlapSphere(scene, Vector3(1.5f, 1.5f, 1.5f), 2.0f, colliders, sixteen), 0u);
    ASSERT_LE(colliders.size(), 16u);
    ASSERT_TRUE(PhysicsQueries3D::checkSphere(scene, Vector3(1.5f, 1.5f, 1.5f), 2.0f));

    size_t found = 0;
    const double cast = medianMicros([&](int i){
        found += PhysicsQueries3D::sphereCast(scene, rowStart(i), 0.4f, along, 40.0f).hit ? 1 : 0;
    });
    const double castAll = medianMicros([&](int i){
        found += PhysicsQueries3D::sphereCastAll(scene, rowStart(i), 0.4f, along, 40.0f, hits, sixteen);
    });
    const double overlap = medianMicros([&](int i){
        const Vector3 center = rowStart(i) + Vector3(4.5f + SPACING * (float)(i % 7), 1.5f, 1.5f);
        found += PhysicsQueries3D::overlapSphere(scene, center, 2.0f, colliders, sixteen);
    });
    const double check = medianMicros([&](int i){
        const Vector3 center = rowStart(i) + Vector3(4.5f + SPACING * (float)(i % 7), 1.5f, 1.5f);
        found += PhysicsQueries3D::checkSphere(scene, center, 2.0f) ? 1 : 0;
    });
    EXPECT_GT(found, 0u);
    checkBudget("sphereCast", cast, 15.0);
    checkBudget("sphereCastAll (10 hits)", castAll, 15.0);
    checkBudget("overlapSphere", overlap, 15.0);
    checkBudget("checkSphere", check, 8.0);

    // no heap allocation with an output of capacity 16
    if (testsupport::countsAllocations()){
        const testsupport::ThreadAllocationCount allocations;
        for (int i = 0; i < 100; i++){
            PhysicsQueries3D::sphereCast(scene, rowStart(i), 0.4f, along, 40.0f);
            PhysicsQueries3D::sphereCastAll(scene, rowStart(i), 0.4f, along, 40.0f, hits, sixteen);
            PhysicsQueries3D::overlapSphere(scene, Vector3(1.5f, 1.5f, 1.5f), 2.0f, colliders, sixteen);
            PhysicsQueries3D::checkSphere(scene, Vector3(1.5f, 1.5f, 1.5f), 2.0f);
        }
        EXPECT_EQ(allocations.count(), 0u);
    }
}

#endif // JASPERENGINE_PHYSICS_3D
