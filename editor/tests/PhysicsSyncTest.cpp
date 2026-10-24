// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The sync between physics bodies and their Transforms (PhysicsSystem): a frame running
// several fixed steps advances the bodies by all of them (the write-back sets the world pose
// too), interpolation draws the bodies between their last two steps without feeding the drawn
// pose back into the simulation, a kinematic body moved by its Transform is moved with a
// velocity (it carries what stands on it), and the 2D ray casts return the closest hit.

#include <gtest/gtest.h>

#include "Scene.h"
#include "component/Rigidbody2DComponent.h"
#include "component/RigidbodyComponent.h"
#include "component/Transform.h"
#include "math/Ray.h"
#include "math/Sphere.h"
#include "object/physics/Rigidbody.h"
#include "object/physics/Rigidbody2D.h"
#include "subsystem/PhysicsSystem.h"
#include "subsystem/RenderSystem.h"
#include "util/Angle.h"

#include <cmath>
#include <vector>

using namespace jasperengine;

namespace {

    const float step = 1.0f / 60.0f;

    // what the variable update of a frame does to the Transforms: world poses from the local
    // ones, parents first (entities are created parents first here)
    void updateTransforms(Scene& scene, const std::vector<Entity>& entities) {
        for (Entity entity : entities) {
            Transform* transform = scene.findComponent<Transform>(entity);
            if (transform && transform->needUpdate) {
                scene.getSystem<RenderSystem>()->updateTransform(*transform, entity);
                transform->needUpdate = false;
            }
        }
    }

    // one frame: its fixed steps back to back (as Engine::systemDraw runs them), then the
    // interpolation and the transform update
    void frame(Scene& scene, const std::vector<Entity>& entities, int steps, double alpha = 1.0) {
        for (int i = 0; i < steps; i++) {
            scene.getSystem<PhysicsSystem>()->fixedUpdate(step);
        }
        scene.getSystem<PhysicsSystem>()->interpolate(alpha);
        updateTransforms(scene, entities);
    }

    Entity createEntity(Scene& scene, Vector3 position, Entity parent = NULL_ENTITY) {
        Entity entity = scene.createEntity();
        Transform transform;
        transform.position = position;
        transform.parent = parent;
        scene.addComponent<Transform>(entity, transform);
        return entity;
    }

    Entity createSphere(Scene& scene, Vector3 position, BodyType type, Entity parent = NULL_ENTITY) {
        Entity entity = createEntity(scene, position, parent);
        RigidbodyComponent body;
        body.type = type;
        scene.addComponent<RigidbodyComponent>(entity, body);
        Rigidbody(&scene, entity).createSphereCollider(0.5f);
        return entity;
    }

    Entity createBox(Scene& scene, Vector3 position, BodyType type, Vector3 size, float friction) {
        Entity entity = createEntity(scene, position);
        RigidbodyComponent body;
        body.type = type;
        body.friction = friction;
        scene.addComponent<RigidbodyComponent>(entity, body);
        Rigidbody(&scene, entity).createBoxCollider(size.x, size.y, size.z);
        return entity;
    }

    // a sphere of a weightless world moving at 1 m/s along x after the bodies are loaded
    struct MovingSphere {
        Scene scene;
        std::vector<Entity> entities;
        Entity sphere = NULL_ENTITY;

        explicit MovingSphere(bool interpolation = true, Vector3 parentPosition = Vector3::ZERO, bool parented = false) {
            scene.setGravity3D(Vector3::ZERO);
            scene.setPhysicsInterpolation(interpolation);
            Entity parent = NULL_ENTITY;
            if (parented) {
                parent = createEntity(scene, parentPosition);
                Transform& parentTransform = scene.getComponent<Transform>(parent);
                parentTransform.rotation = Quaternion(Angle::radToDefault((float)M_PI / 2), Vector3(0, 1, 0));
                entities.push_back(parent);
            }
            sphere = createSphere(scene, Vector3(0, 0, 0), BodyType::DYNAMIC, parent);
            entities.push_back(sphere);
            updateTransforms(scene, entities);
            scene.getSystem<PhysicsSystem>()->load();
            Rigidbody body(&scene, sphere);
            body.setLinearVelocity(Vector3(1, 0, 0));
        }

        Vector3 bodyPosition() { return Rigidbody(&scene, sphere).getPosition(); }
        Transform& transform() { return scene.getComponent<Transform>(sphere); }
    };

    void expectNear(const Vector3& a, const Vector3& b, float tolerance = 1e-4f) {
        EXPECT_NEAR(a.x, b.x, tolerance);
        EXPECT_NEAR(a.y, b.y, tolerance);
        EXPECT_NEAR(a.z, b.z, tolerance);
    }

}

TEST(PhysicsSync, SeveralStepsInOneFrameMoveAsFarAsOneStepPerFrame) {
    MovingSphere oneStepFrames(false);
    for (int i = 0; i < 4; i++) frame(oneStepFrames.scene, oneStepFrames.entities, 1);

    MovingSphere oneFrame(false);
    frame(oneFrame.scene, oneFrame.entities, 4);

    const Vector3 expected = oneStepFrames.bodyPosition();
    EXPECT_GT(expected.x, 3.5f * step);
    expectNear(oneFrame.bodyPosition(), expected);
    expectNear(oneFrame.transform().worldPosition, expected);
    expectNear(oneFrame.transform().position, expected);
}

TEST(PhysicsSync, SeveralStepsInOneFrameMoveAParentedBody) {
    const Vector3 parentPosition(10, 2, 0);
    MovingSphere oneStepFrames(false, parentPosition, true);
    for (int i = 0; i < 4; i++) frame(oneStepFrames.scene, oneStepFrames.entities, 1);

    MovingSphere oneFrame(false, parentPosition, true);
    frame(oneFrame.scene, oneFrame.entities, 4);

    const Vector3 expected = oneStepFrames.bodyPosition();
    EXPECT_GT(expected.x - parentPosition.x, 3.5f * step);
    expectNear(oneFrame.bodyPosition(), expected);
    expectNear(oneFrame.transform().worldPosition, expected);
    // the local pose is in the parent's frame (turned 90 degrees about y): x moves to z
    EXPECT_NEAR(oneFrame.transform().position.z, expected.x - parentPosition.x, 1e-4f);
}

TEST(PhysicsSync, InterpolationDrawsBetweenStepsAndLeavesTheSimulationAlone) {
    MovingSphere plain(false);
    MovingSphere interpolated(true);

    for (int i = 0; i < 3; i++) {
        frame(plain.scene, plain.entities, 1);
        const Vector3 before = interpolated.bodyPosition();
        frame(interpolated.scene, interpolated.entities, 1, 0.25);
        const Vector3 after = interpolated.bodyPosition();

        // drawn a quarter of the way from the pose before the step
        expectNear(interpolated.transform().worldPosition, before.lerp(after, 0.25f));
        // and the body goes on from where it really is
        expectNear(after, plain.bodyPosition());
    }

    // a frame with no step draws further along the same step
    const Vector3 current = interpolated.bodyPosition();
    frame(interpolated.scene, interpolated.entities, 0, 0.75);
    EXPECT_GT(interpolated.transform().worldPosition.x, current.x - step * 0.5f);
    EXPECT_LT(interpolated.transform().worldPosition.x, current.x);
    expectNear(interpolated.bodyPosition(), current);

    // off: the Transform shows the body's own pose
    interpolated.scene.setPhysicsInterpolation(false);
    frame(interpolated.scene, interpolated.entities, 1, 0.25);
    expectNear(interpolated.transform().worldPosition, interpolated.bodyPosition());
}

TEST(PhysicsSync, ATransformMovedAfterTheInterpolationMovesTheBody) {
    MovingSphere sphere(true);
    frame(sphere.scene, sphere.entities, 1, 0.5);

    // a script (or the editor) puts the GameObject somewhere else
    Transform& transform = sphere.transform();
    transform.position = Vector3(5, 0, 0);
    transform.needUpdate = true;
    updateTransforms(sphere.scene, sphere.entities);

    frame(sphere.scene, sphere.entities, 1, 1.0);
    EXPECT_NEAR(sphere.bodyPosition().x, 5.0f + step, 1e-3f);
}

TEST(PhysicsSync2D, ATransformMovedBackWhereNothingWasSyncedMovesTheBody) {
    // a static Rigidbody 2D physics never writes back, created where its Transform is
    Scene scene;
    Entity entity = createEntity(scene, Vector3(0, 0, 0));
    Rigidbody2DComponent body;
    body.type = BodyType::STATIC;
    scene.addComponent<Rigidbody2DComponent>(entity, body);
    Rigidbody2D rigidbody(&scene, entity);
    rigidbody.createCircleCollider(Vector2(0, 0), 16);
    std::vector<Entity> entities{entity};
    updateTransforms(scene, entities);
    scene.getSystem<PhysicsSystem>()->load();
    frame(scene, entities, 1);

    for (float x : {100.0f, 0.0f}) {
        Transform& transform = scene.getComponent<Transform>(entity);
        transform.position = Vector3(x, 0, 0);
        transform.needUpdate = true;
        updateTransforms(scene, entities);
        frame(scene, entities, 1);
        EXPECT_NEAR(rigidbody.getPosition().x, x, 1e-3f);
    }
}

TEST(PhysicsSync, TeleportsThroughTheBodyAreNotInterpolated) {
    MovingSphere sphere(true);
    frame(sphere.scene, sphere.entities, 1, 0.5);

    Rigidbody(&sphere.scene, sphere.sphere).setPosition(Vector3(-4, 1, 0));
    expectNear(sphere.transform().worldPosition, Vector3(-4, 1, 0));

    frame(sphere.scene, sphere.entities, 0, 0.5);
    expectNear(sphere.transform().worldPosition, Vector3(-4, 1, 0));
}

TEST(PhysicsSync, AKinematicBodyMovedByItsTransformCarriesWhatStandsOnIt) {
    Scene scene;
    scene.setGravity3D(Vector3(0, -9.81f, 0));
    Entity platform = createBox(scene, Vector3(0, 0, 0), BodyType::KINEMATIC, Vector3(8, 0.5f, 8), 1.0f);
    Entity crate = createBox(scene, Vector3(0, 0.75f, 0), BodyType::DYNAMIC, Vector3(1, 1, 1), 1.0f);
    std::vector<Entity> entities{platform, crate};
    updateTransforms(scene, entities);
    scene.getSystem<PhysicsSystem>()->load();

    // the crate settles on the platform
    for (int i = 0; i < 30; i++) frame(scene, entities, 1);
    const float crateStart = Rigidbody(&scene, crate).getPosition().x;

    // the platform slides along x, moved by its Transform as an animation would
    for (int i = 1; i <= 30; i++) {
        Transform& transform = scene.getComponent<Transform>(platform);
        transform.position = Vector3(0.05f * i, 0, 0);
        transform.needUpdate = true;
        updateTransforms(scene, entities);
        frame(scene, entities, 1);

        // where its Transform is at the end of the frame, at rest
        EXPECT_NEAR(Rigidbody(&scene, platform).getPosition().x, 0.05f * i, 1e-4f);
        EXPECT_NEAR(Rigidbody(&scene, platform).getLinearVelocity().x, 0.0f, 1e-4f);
        // and its Transform is left as the animation wrote it
        EXPECT_FLOAT_EQ(scene.getComponent<Transform>(platform).position.x, 0.05f * i);
    }

    // a teleported platform would slide out under the crate
    EXPECT_GT(Rigidbody(&scene, crate).getPosition().x - crateStart, 0.5f);

    // put back where it started, a pose physics had written before the animation
    Transform& transform = scene.getComponent<Transform>(platform);
    transform.position = Vector3(0, 0, 0);
    transform.needUpdate = true;
    updateTransforms(scene, entities);
    frame(scene, entities, 1);
    EXPECT_NEAR(Rigidbody(&scene, platform).getPosition().x, 0.0f, 1e-4f);
    EXPECT_FLOAT_EQ(scene.getComponent<Transform>(platform).position.x, 0.0f);
}

TEST(PhysicsSync2D, SeveralStepsInOneFrameMoveAsFarAsOneStepPerFrame) {
    auto run = [](int frames, int stepsPerFrame) {
        Scene scene;
        scene.setGravity2D(Vector2(0, 0));
        scene.setPhysicsInterpolation(false);
        Entity entity = createEntity(scene, Vector3(0, 0, 0));
        Rigidbody2DComponent body;
        body.type = BodyType::DYNAMIC;
        scene.addComponent<Rigidbody2DComponent>(entity, body);
        Rigidbody2D rigidbody(&scene, entity);
        rigidbody.createCircleCollider(Vector2(0, 0), 16);
        std::vector<Entity> entities{entity};
        updateTransforms(scene, entities);
        scene.getSystem<PhysicsSystem>()->load();
        rigidbody.setLinearVelocity(Vector2(64, 0));

        for (int i = 0; i < frames; i++) frame(scene, entities, stepsPerFrame);

        const Transform& transform = scene.getComponent<Transform>(entity);
        EXPECT_NEAR(transform.worldPosition.x, rigidbody.getPosition().x, 1e-3f);
        return rigidbody.getPosition().x;
    };

    const float oneStepFrames = run(4, 1);
    EXPECT_GT(oneStepFrames, 64 * 3.5f * step);
    EXPECT_NEAR(run(1, 4), oneStepFrames, 1e-3f);
}

TEST(PhysicsRay2D, ClosestColliderWhateverTheirOrder) {
    for (int order = 0; order < 2; order++) {
        Scene scene;
        Entity entity = createEntity(scene, Vector3(0, 0, 0));
        Rigidbody2DComponent body;
        body.type = BodyType::STATIC;
        scene.addComponent<Rigidbody2DComponent>(entity, body);
        Rigidbody2D rigidbody(&scene, entity);
        if (order == 0) {
            rigidbody.createCircleCollider(Vector2(100, 0), 10);
            rigidbody.createCircleCollider(Vector2(300, 0), 10);
        } else {
            rigidbody.createCircleCollider(Vector2(300, 0), 10);
            rigidbody.createCircleCollider(Vector2(100, 0), 10);
        }
        std::vector<Entity> entities{entity};
        updateTransforms(scene, entities);
        scene.getSystem<PhysicsSystem>()->load();

        RayReturn hit = Ray(Vector3(0, 0, 0), Vector3(400, 0, 0)).intersects(rigidbody);
        ASSERT_TRUE(hit.hit) << "order " << order;
        EXPECT_NEAR(hit.point.x, 90.0f, 0.5f) << "order " << order;
        EXPECT_EQ(hit.colliderIndex, order == 0 ? 0u : 1u) << "order " << order;

        // a collider index out of range misses
        EXPECT_FALSE(Ray(Vector3(0, 0, 0), Vector3(400, 0, 0)).intersects(rigidbody, 5).hit);
    }
}

TEST(PhysicsRay, FiniteRayMissesSpheresBehindOrBeyondIt) {
    const Ray ray(Vector3(0, 0, 0), Vector3(10, 0, 0));
    EXPECT_TRUE(ray.intersects(Sphere(Vector3(5, 0, 0), 1)).hit);
    EXPECT_FALSE(ray.intersects(Sphere(Vector3(-5, 0, 0), 1)).hit);
    EXPECT_FALSE(ray.intersects(Sphere(Vector3(15, 0, 0), 1)).hit);
    // from inside, it leaves through the far side
    RayReturn inside = ray.intersects(Sphere(Vector3(0, 0, 0), 1));
    ASSERT_TRUE(inside.hit);
    EXPECT_NEAR(inside.point.x, 1.0f, 1e-4f);
}
