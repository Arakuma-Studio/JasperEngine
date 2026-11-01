// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Camera Follow (spec 14, test 11): SmoothDamp against Unity's formula, rigid follow, the dead
// zone, bounds, look-at (useTarget cameras and rotation), the current-frame target pose, offset
// spaces, foreign writes and the frame order with the shake.

#include <gtest/gtest.h>

#include "CameraTestEnvironment.h"

#include "Engine.h"
#include "Scene.h"
#include "component/CameraComponent.h"
#include "component/Transform.h"
#include "object/CameraFollow.h"
#include "manager/ObjectManager.h"
#include "manager/SceneManager.h"
#include "subsystem/CameraControlSystem.h"

#include <cmath>

using namespace jasperengine;

namespace {

    constexpr float DT = 1.0f / 60.0f;

    // Unity's Mathf.SmoothDamp, in double, written from its documented source
    double unitySmoothDamp(double current, double target, double& velocity, double smoothTime, double dt){
        smoothTime = std::max(0.0001, smoothTime);
        const double omega = 2.0 / smoothTime;
        const double x = omega * dt;
        const double e = 1.0 / (1.0 + x + 0.48 * x * x + 0.235 * x * x * x);
        double change = current - target;
        const double originalTo = target;
        target = current - change;
        const double temp = (velocity + omega * change) * dt;
        velocity = (velocity - omega * temp) * e;
        double output = target + (change + temp) * e;
        if ((originalTo - current > 0.0) == (output > originalTo)){
            output = originalTo;
            velocity = (output - originalTo) / dt;
        }
        return output;
    }

    class Follow : public ::testing::Test {
    protected:
        Entity object(Vector3 position, Entity parent = NULL_ENTITY){
            Entity entity = scene.createEntity();
            Transform transform;
            transform.position = position;
            scene.addComponent<Transform>(entity, transform);
            if (parent != NULL_ENTITY) scene.addEntityChild(parent, entity, false);
            return entity;
        }

        Entity camera(Vector3 position, CameraType type = CameraType::CAMERA_PERSPECTIVE, bool useTarget = false){
            Entity entity = object(position);
            CameraComponent component;
            component.type = type;
            component.useTarget = useTarget;
            scene.addComponent<CameraComponent>(entity, component);
            return entity;
        }

        CameraFollowComponent& follow(Entity entity, Entity target){
            scene.addComponent<CameraFollowComponent>(entity, {});
            CameraFollowComponent& f = scene.getComponent<CameraFollowComponent>(entity);
            f.target = target;
            return f;
        }

        void step(float dt = DT, int count = 1){
            for (int i = 0; i < count; i++) scene.getSystem<CameraControlSystem>()->step(dt, dt);
        }

        Vector3& position(Entity entity){ return scene.getComponent<Transform>(entity).position; }

        Scene scene;
    };

}

TEST(FollowMath, SmoothDampMatchesUnity){
    float velocity = 0.0f;
    double reference = 0.0;
    double referenceVelocity = 0.0;
    float value = 0.0f;
    for (int i = 0; i < 12; i++){
        value = CameraControlSystem::smoothDamp(value, 1.0f, velocity, 0.2f, 0.0f, DT);
        reference = unitySmoothDamp(reference, 1.0, referenceVelocity, 0.2, DT);
    }
    EXPECT_NEAR(value, reference, 1e-4);
    EXPECT_NEAR(velocity, referenceVelocity, 1e-3);
    // a max speed caps the step
    float capped = 0.0f, cappedVelocity = 0.0f;
    capped = CameraControlSystem::smoothDamp(capped, 100.0f, cappedVelocity, 0.2f, 1.0f, DT);
    EXPECT_LT(capped, 0.2f);
}

TEST_F(Follow, DampedFollowCoversTheSmoothDampFraction){
    Entity target = object(Vector3(0, 0, 0));
    Entity cam = camera(Vector3(0, 0, 0));
    CameraFollowComponent& f = follow(cam, target);
    f.offset = Vector3(0, 0, 0);
    f.lookAt = false;
    step();                         // placed rigidly on the first update
    EXPECT_EQ(position(cam), Vector3(0, 0, 0));
    position(target) = Vector3(1, 0, 0);
    double reference = 0.0, referenceVelocity = 0.0;
    for (int i = 0; i < 12; i++){
        step();
        reference = unitySmoothDamp(reference, 1.0, referenceVelocity, 0.2, DT);
    }
    EXPECT_NEAR(position(cam).x, reference, 1e-4);
    EXPECT_GT(position(cam).x, 0.3f);
    EXPECT_LT(position(cam).x, 0.9f);
}

TEST_F(Follow, ZeroDampingSnapsAndTheCurrentFramePoseIsUsed){
    Entity root = object(Vector3(10, 0, 0));
    Entity target = object(Vector3(1, 0, 0), root);
    Entity cam = camera(Vector3(0, 0, 0));
    CameraFollowComponent& f = follow(cam, target);
    f.damping = Vector3(0, 0, 0);
    f.offset = Vector3(0, 2, -6);
    f.lookAt = false;
    step();
    EXPECT_EQ(position(cam), Vector3(11, 2, -6));
    // moved this frame (no render update since: the cached world position is stale)
    position(root) = Vector3(20, 0, 0);
    step();
    EXPECT_EQ(position(cam), Vector3(21, 2, -6));
}

TEST_F(Follow, DeadZoneOnAnOrthographicCamera){
    Entity target = object(Vector3(0, 0, 0));
    Entity cam = camera(Vector3(0, 0, 0), CameraType::CAMERA_ORTHO);
    CameraFollowComponent& f = follow(cam, target);
    f.damping = Vector3(0, 0, 0);
    f.offset = Vector3(0, 0, 0);
    f.deadZone = Vector2(2, 2);
    f.lookAt = false;
    step();
    position(target).x = 0.9f;
    step();
    EXPECT_FLOAT_EQ(position(cam).x, 0.0f);
    position(target).x = 1.5f;
    step();
    EXPECT_FLOAT_EQ(position(cam).x, 0.5f);
    // a perspective camera ignores it
    Entity other = object(Vector3(0, 0, 0));
    Entity perspective = camera(Vector3(0, 0, 0));
    CameraFollowComponent& g = follow(perspective, other);
    g.damping = Vector3(0, 0, 0);
    g.offset = Vector3(0, 0, 0);
    g.deadZone = Vector2(2, 2);
    g.lookAt = false;
    step();
    position(other).x = 0.9f;
    step();
    EXPECT_FLOAT_EQ(position(perspective).x, 0.9f);
}

TEST_F(Follow, BoundsClamp){
    Entity target = object(Vector3(0, 0, 0));
    Entity cam = camera(Vector3(0, 0, 0));
    CameraFollowComponent& f = follow(cam, target);
    f.damping = Vector3(0, 0, 0);
    f.offset = Vector3(0, 0, 0);
    f.lookAt = false;
    f.useBounds = true;
    f.boundsMin = Vector3(-5, -1, -5);
    f.boundsMax = Vector3(5, 1, 5);
    position(target) = Vector3(30, -7, 2);
    step();
    EXPECT_EQ(position(cam), Vector3(5, -1, 2));
}

TEST_F(Follow, UseTargetCamerasAimThroughTheirTarget){
    Entity target = object(Vector3(3, 0, 0));
    Entity cam = camera(Vector3(0, 0, 0), CameraType::CAMERA_PERSPECTIVE, true);
    CameraFollowComponent& f = follow(cam, target);
    f.damping = Vector3(0, 0, 0);
    f.lookAtOffset = Vector3(0, 1, 0);
    step();
    EXPECT_EQ(scene.getComponent<CameraComponent>(cam).target, Vector3(3, 1, 0));
    // no aim: the camera keeps its view direction, the target moves with it
    f.lookAt = false;
    position(target) = Vector3(5, 0, 0);
    step();
    const CameraComponent& component = scene.getComponent<CameraComponent>(cam);
    EXPECT_EQ(component.target - position(cam), Vector3(3, 1, 0) - Vector3(3, 2, -6));
}

TEST_F(Follow, LookAtTurnsTheCameraToTheTarget){
    Entity target = object(Vector3(0, 0, 0));
    Entity cam = camera(Vector3(0, 0, 10));
    CameraFollowComponent& f = follow(cam, target);
    f.damping = Vector3(0, 0, 0);
    f.offset = Vector3(10, 0, 0);
    f.lookAtOffset = Vector3(0, 0, 0);
    f.rotationDamping = 0.0f;
    step();
    // a camera looks down its -Z
    const Vector3 forward = scene.getComponent<Transform>(cam).rotation * Vector3(0, 0, -1);
    EXPECT_NEAR(forward.x, -1.0f, 1e-5f);
    EXPECT_NEAR(forward.y, 0.0f, 1e-5f);
    EXPECT_NEAR(forward.z, 0.0f, 1e-5f);
}

TEST_F(Follow, OffsetSpacesTurnWithTheTarget){
    Entity target = object(Vector3(0, 0, 0));
    // the target faces +X (90 degrees about Y)
    const float h = 3.14159265f / 4.0f;
    scene.getComponent<Transform>(target).rotation = Quaternion(std::cos(h), 0, std::sin(h), 0);
    Entity cam = camera(Vector3(0, 0, 0));
    CameraFollowComponent& f = follow(cam, target);
    f.damping = Vector3(0, 0, 0);
    f.offset = Vector3(0, 0, -6);
    f.lookAt = false;
    f.offsetSpace = FollowOffsetSpace::TargetYaw;
    step();
    EXPECT_NEAR(position(cam).x, -6.0f, 1e-4f);
    EXPECT_NEAR(position(cam).z, 0.0f, 1e-4f);
}

TEST_F(Follow, ForeignWritesAreOverwrittenWithAWarningOnce){
    Entity target = object(Vector3(0, 0, 0));
    Entity cam = camera(Vector3(0, 0, 0));
    CameraFollowComponent& f = follow(cam, target);
    f.damping = Vector3(0, 0, 0);
    f.lookAt = false;
    step();
    cameratest::takeWarnings();
    position(cam) = Vector3(50, 50, 50);
    step();
    EXPECT_EQ(position(cam), f.offset);
    EXPECT_TRUE(scene.getComponent<CameraFollowComponent>(cam).foreignWriteDetected);
    position(cam) = Vector3(50, 50, 50);
    step();
    EXPECT_EQ(cameratest::takeWarnings().size(), 1u);
}

TEST_F(Follow, MissingTargetHoldsAndSnapPlacesRigidly){
    Entity target = object(Vector3(0, 0, 0));
    Entity cam = camera(Vector3(0, 0, 0));
    CameraFollowComponent& f = follow(cam, target);
    f.lookAt = false;
    step();
    const Vector3 placed = position(cam);
    position(target) = Vector3(100, 0, 0);
    CameraFollow(&scene, cam).snap();
    step();
    EXPECT_EQ(position(cam), Vector3(100, 0, 0) + scene.getComponent<CameraFollowComponent>(cam).offset);
    scene.destroyEntity(target);
    cameratest::takeWarnings();
    step(DT, 3);
    EXPECT_EQ(cameratest::takeWarnings().size(), 1u);
    EXPECT_NE(position(cam), placed);
}

// ---- a target in the persistent (DontDestroyOnLoad) scene (lead decision with persistence)

TEST_F(Follow, FollowsATargetOfThePersistentScene){
    Scene* persistent = SceneManager::ensurePersistentScene();
    ASSERT_TRUE(persistent);
    const Entity player = persistent->createEntity();
    Transform playerTransform;
    playerTransform.position = Vector3(4, 0, 0);
    persistent->addComponent<Transform>(player, playerTransform);

    Entity cam = camera(Vector3(0, 0, 0));
    scene.addComponent<CameraFollowComponent>(cam, {});
    CameraFollow follow(&scene, cam);
    ASSERT_TRUE(follow.setTarget(persistent, player));
    follow.setDamping(Vector3(0, 0, 0));
    follow.setLookAt(false);
    follow.setOffset(Vector3(0, 1, -5));
    step();
    EXPECT_EQ(position(cam), Vector3(4, 1, -5));
    EXPECT_EQ(follow.getTargetScene(), persistent);
    persistent->getComponent<Transform>(player).position = Vector3(9, 0, 0);
    step();
    EXPECT_EQ(position(cam), Vector3(9, 1, -5));

    // another scene's GameObject is refused with a warning
    Scene other;
    const Entity stranger = other.createEntity();
    other.addComponent<Transform>(stranger, {});
    cameratest::takeWarnings();
    EXPECT_FALSE(follow.setTarget(&other, stranger));
    EXPECT_EQ(cameratest::takeWarnings().size(), 1u);
    EXPECT_EQ(follow.getTarget(), player) << "the refused target changes nothing";

    // the persistent target destroyed: the follower holds
    persistent->destroyEntity(player);
    step();
    EXPECT_EQ(position(cam), Vector3(9, 1, -5));
    SceneManager::destroyPersistentScene();
}

TEST_F(Follow, ATargetMovedToThePersistentSceneIsFollowedThere){
    Scene* persistent = SceneManager::ensurePersistentScene();
    ASSERT_TRUE(persistent);
    Entity player = object(Vector3(2, 0, 0));
    Entity cam = camera(Vector3(0, 0, 0));
    CameraFollowComponent& f = follow(cam, player);
    f.damping = Vector3(0, 0, 0);
    f.lookAt = false;
    f.offset = Vector3(0, 0, -3);
    step();
    EXPECT_EQ(position(cam), Vector3(2, 0, -3));
    // DontDestroyOnLoad(player): it moves into the persistent scene with a new id there
    const std::vector<Entity> moved = ObjectManager::moveToScene(&scene, player, persistent);
    ASSERT_FALSE(moved.empty());
    const CameraFollowComponent& after = scene.getComponent<CameraFollowComponent>(cam);
    EXPECT_TRUE(after.targetPersistent);
    EXPECT_EQ(after.target, moved[0]);
    persistent->getComponent<Transform>(moved[0]).position = Vector3(7, 0, 0);
    step();
    EXPECT_EQ(position(cam), Vector3(7, 0, -3));
    SceneManager::destroyPersistentScene();
}
