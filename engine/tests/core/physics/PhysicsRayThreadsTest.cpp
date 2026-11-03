// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The particles' world collision casts its rays from jobs (EmitterRuntime::worldCollisionStage:
// "physics reads are thread-safe outside its step", ParticleSubsystem::castWorldRay: this very
// call). ParticleCollisionTest answers them with a box floor of its own: this is the real query,
// many at once. Under SANITIZE=thread a race among them stops the run with its report.

#ifdef JASPERENGINE_PHYSICS_3D

#include "Scene.h"
#include "component/RigidbodyComponent.h"
#include "component/Transform.h"
#include "jobs/JobSystem.h"
#include "math/Ray.h"
#include "registry/PhysicsLayers.h"
#include "subsystem/PhysicsSystem.h"
#include "subsystem/RenderSystem.h"
#include "../ecs/EcsTestUtil.h"

#include <gtest/gtest.h>

#include <atomic>

using namespace jasperengine;

TEST(PhysicsRayThreads, RaysFromJobsShareNothingOutsideTheStep){
    Scene scene;
    test::disableSubSystems(scene);
    scene.getScheduler().setEnabled("PhysicsSystem", true);

    const Entity entity = test::makeTransformEntity(scene);
    Transform& transform = scene.getComponent<Transform>(entity);
    transform.position = Vector3(0.0f, -0.5f, 0.0f);
    transform.needUpdate = true;
    scene.getSystem<RenderSystem>()->updateTransform(transform, entity);
    RigidbodyComponent rigidbody;
    rigidbody.type = BodyType::STATIC;
    rigidbody.colliders[0].type = Collider3DType::BOX;
    rigidbody.colliders[0].width = 100.0f;
    rigidbody.colliders[0].height = 1.0f;
    rigidbody.colliders[0].depth = 100.0f;
    rigidbody.numColliders = 1;
    scene.addComponent<RigidbodyComponent>(entity, rigidbody);
    scene.fixedUpdate(1.0f / 60.0f);

    constexpr size_t rays = 4000;
    std::atomic<size_t> hits{0};
    jobs::parallelFor(rays, [&scene, &hits](size_t i){
        const float x = -40.0f + 80.0f * (float)i / (float)rays;
        const RayReturn hit = Physics::raycast(&scene, Ray(Vector3(x, 5.0f, 0.0f), Vector3(0.0f, -10.0f, 0.0f)));
        if (hit.hit)
            hits.fetch_add(1);
    }, jobs::ParallelForOptions{.grain = 16});
    EXPECT_EQ(hits.load(), rays);
}

#endif // JASPERENGINE_PHYSICS_3D
