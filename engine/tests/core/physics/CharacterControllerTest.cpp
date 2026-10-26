// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The 3D CharacterController (docs/specs/S22-gameplay.md R19-R27, AC6-AC9) on Jolt's
// CharacterVirtual: a scene with only its PhysicsSystem running, stepped at 60 Hz as the engine's
// fixed phase does (scripts' moves first, then the step).

#include "Engine.h"
#include "Input.h"
#include "LogCapture.h"
#include "Scene.h"
#include "component/CharacterControllerComponent.h"
#include "component/RigidbodyComponent.h"
#include "component/Transform.h"
#include "math/Ray.h"
#include "object/physics/CharacterController.h"
#include "object/physics/Rigidbody.h"
#include "subsystem/PhysicsSystem.h"
#include "subsystem/RenderSystem.h"
#include "util/Angle.h"
#include "../ecs/EcsTestUtil.h"

#include <gtest/gtest.h>

#include <cmath>
#include <functional>
#include <memory>
#include <vector>

using namespace jasperengine;

namespace {

    constexpr float DT = 1.0f / 60.0f;

    class CharacterControllerTest : public ::testing::Test {
    protected:
        std::unique_ptr<Scene> scene;
        PhysicsSystem* physics = nullptr;
        float updateTime = 0.0f;

        void SetUp() override{
            updateTime = Engine::getUpdateTime();
            Engine::setUpdateTime(DT);
            scene = std::make_unique<Scene>();
            test::disableSubSystems(*scene);
            scene->getScheduler().setEnabled("PhysicsSystem", true);
            physics = scene->getSystem<PhysicsSystem>().get();
            physics->setGravity3D(0.0f, -9.81f, 0.0f);
        }

        void TearDown() override{
            scene.reset();
            Engine::setUpdateTime(updateTime);
        }

        Entity body(Vector3 position, Vector3 size, BodyType type, Quaternion rotation = Quaternion::IDENTITY, float density = 1000.0f){
            const Entity entity = test::makeTransformEntity(*scene);
            Transform& transform = scene->getComponent<Transform>(entity);
            transform.position = position;
            transform.rotation = rotation;
            transform.needUpdate = true;
            RigidbodyComponent rigidbody;
            rigidbody.type = type;
            rigidbody.colliders[0].type = Collider3DType::BOX;
            rigidbody.colliders[0].width = size.x;
            rigidbody.colliders[0].height = size.y;
            rigidbody.colliders[0].depth = size.z;
            rigidbody.colliders[0].density = density;
            rigidbody.numColliders = 1;
            scene->addComponent<RigidbodyComponent>(entity, rigidbody);
            return entity;
        }

        // a capsule standing with its feet at `foot`
        Entity controller(Vector3 foot, float radius = 0.4f, float height = 1.8f, float stepOffset = 0.3f){
            const Entity entity = test::makeTransformEntity(*scene);
            Transform& transform = scene->getComponent<Transform>(entity);
            transform.position = foot + Vector3(0.0f, height * 0.5f, 0.0f);
            transform.needUpdate = true;
            CharacterControllerComponent component;
            component.radius = radius;
            component.height = height;
            component.stepOffset = stepOffset;
            scene->addComponent<CharacterControllerComponent>(entity, component);
            return entity;
        }

        Vector3 position(Entity entity){
            return scene->getComponent<Transform>(entity).worldPosition;
        }

        float foot(Entity entity){
            const CharacterControllerComponent& c = scene->getComponent<CharacterControllerComponent>(entity);
            return position(entity).y + c.center.y - c.height * 0.5f;
        }

        CharacterControllerComponent& component(Entity entity){
            return scene->getComponent<CharacterControllerComponent>(entity);
        }

        // one engine fixed step: what the scripts do in onFixedUpdate, then the physics
        // the Transforms written since the last frame resolved (world poses), as the frame's
        // render pass does between the engine's fixed steps
        void resolveTransforms(){
            auto transforms = scene->getComponentArray<Transform>();
            for (size_t i = 0; i < transforms->size(); i++){
                Transform& transform = transforms->getComponentFromIndex(i);
                if (transform.needUpdate)
                    scene->getSystem<RenderSystem>()->updateTransform(transform, transforms->getEntity(i));
            }
        }

        // one engine fixed step: what the scripts do in onFixedUpdate, then the physics
        void step(const std::function<void()>& scripts = nullptr){
            resolveTransforms();
            Input::beginFixedStep();
            if (scripts)
                scripts();
            scene->fixedUpdate(DT);
            Input::endFixedStep();
            resolveTransforms();
        }

        void steps(int count, const std::function<void()>& scripts = nullptr){
            for (int i = 0; i < count; i++)
                step(scripts);
        }

        Entity floor(float size = 40.0f){
            return body(Vector3(0.0f, -0.5f, 0.0f), Vector3(size, 1.0f, size), BodyType::STATIC);
        }

        float yawDegrees(Entity entity){
            const Quaternion q = scene->getComponent<Transform>(entity).worldRotation;
            const Vector3 forward = q * Vector3(0.0f, 0.0f, 1.0f);
            return std::atan2(forward.x, forward.z) * 57.29578f;
        }
    };

    // 6 steps of 0.2 m risers and 0.3 m treads from x = 0, and a top landing
    void stairs(CharacterControllerTest* test, std::function<Entity(Vector3, Vector3, BodyType)> make){
        for (int k = 1; k <= 6; k++){
            const float start = (k - 1) * 0.3f;
            const float end = 6 * 0.3f + 10.0f;
            const float height = 0.2f * k;
            make(Vector3((start + end) * 0.5f, height * 0.5f, 0.0f), Vector3(end - start, height, 4.0f), BodyType::STATIC);
        }
        (void)test;
    }

}

TEST_F(CharacterControllerTest, Stairs){
    floor();
    stairs(this, [this](Vector3 p, Vector3 s, BodyType t){ return body(p, s, t); });
    const Entity walker = controller(Vector3(-1.0f, 0.0f, 0.0f));
    CharacterController cc(scene.get(), walker);
    steps(30);
    ASSERT_TRUE(cc.isGrounded());

    steps(240, [&](){ cc.simpleMove(Vector3(2.0f, 0.0f, 0.0f)); });
    EXPECT_NEAR(foot(walker), 1.2f, 0.05f);
    EXPECT_TRUE(cc.isGrounded());
    EXPECT_GT(position(walker).x, 1.8f);

    // and down again past the first riser, never off the ground more than 2 steps in a row
    // (stick to floor); a step climbed lands up to a radius further on, so it walks back further
    int airborne = 0, longest = 0;
    steps(360, [&](){
        cc.simpleMove(Vector3(-2.0f, 0.0f, 0.0f));
        airborne = cc.isGrounded() ? 0 : airborne + 1;
        longest = std::max(longest, airborne);
    });
    EXPECT_NEAR(foot(walker), 0.0f, 0.05f);
    EXPECT_LE(longest, 2);

    // a step offset under the risers: stopped at the first one
    const Entity short_ = controller(Vector3(-1.0f, 0.0f, 3.0f), 0.4f, 1.8f, 0.1f);
    stairs(this, [this](Vector3 p, Vector3 s, BodyType t){ return body(p + Vector3(0.0f, 0.0f, 3.0f), s, t); });
    CharacterController low(scene.get(), short_);
    steps(240, [&](){ low.simpleMove(Vector3(2.0f, 0.0f, 0.0f)); });
    EXPECT_LT(position(short_).x, 0.0f + 0.05f);
    EXPECT_NEAR(foot(short_), 0.0f, 0.05f);
}

TEST_F(CharacterControllerTest, Slopes){
    floor(80.0f);
    // a 30 degree ramp rising along +X from x = 0
    const float rise30 = 30.0f * 0.01745329f;
    body(Vector3(std::cos(rise30) * 10.0f, std::sin(rise30) * 10.0f - 0.25f, 0.0f), Vector3(20.0f, 0.5f, 4.0f), BodyType::STATIC,
         Quaternion(Angle::radToDefault(rise30), Vector3(0.0f, 0.0f, 1.0f)));
    const Entity climber = controller(Vector3(-1.0f, 0.0f, 0.0f));
    CharacterController cc(scene.get(), climber);
    steps(10);
    const float start = foot(climber);
    steps(180, [&](){ cc.simpleMove(Vector3(2.0f, 0.0f, 0.0f)); });
    EXPECT_GT(foot(climber) - start, 1.0f);

    // a 50 degree ramp in the air: the controller stands on it, cannot climb it, is not grounded
    const float rise50 = 50.0f * 0.01745329f;
    const Vector3 rampCenter(0.0f, 30.0f, 20.0f);
    body(rampCenter, Vector3(60.0f, 0.5f, 4.0f), BodyType::STATIC, Quaternion(Angle::radToDefault(rise50), Vector3(0.0f, 0.0f, 1.0f)));
    const Vector3 normal(-std::sin(rise50), std::cos(rise50), 0.0f);
    // the feet just over the surface: gravity puts the capsule on it
    const Entity slider = controller(rampCenter + normal * 0.25f + Vector3(0.0f, 0.1f, 0.0f));
    CharacterController steep(scene.get(), slider);
    steps(2);
    const float before = foot(slider);
    bool sawSteep = false, sawGrounded = false;
    steps(60, [&](){
        steep.simpleMove(Vector3(2.0f, 0.0f, 0.0f));
        sawSteep = sawSteep || steep.isOnSteepSlope();
        sawGrounded = sawGrounded || steep.isGrounded();
    });
    EXPECT_LT(foot(slider) - before, 0.05f);
    EXPECT_TRUE(sawSteep);
    EXPECT_FALSE(sawGrounded);
    EXPECT_TRUE(steep.isOnSteepSlope());
    EXPECT_FALSE(steep.isGrounded());
}

TEST_F(CharacterControllerTest, CollisionFlags){
    floor();
    body(Vector3(2.0f, 1.5f, 0.0f), Vector3(1.0f, 3.0f, 6.0f), BodyType::STATIC);
    body(Vector3(-10.0f, 2.2f, 0.0f), Vector3(4.0f, 0.4f, 4.0f), BodyType::STATIC);
    const Entity walker = controller(Vector3(0.0f, 0.0f, 0.0f));
    CharacterController cc(scene.get(), walker);
    steps(5);
    int flags = 0;
    steps(60, [&](){ flags = cc.move(Vector3(2.0f, -0.5f, 0.0f) * DT); });
    EXPECT_TRUE(flags & CharacterCollision_Sides) << flags;
    EXPECT_TRUE(flags & CharacterCollision_Below) << flags;
    EXPECT_EQ(cc.getCollisionFlags(), flags);
    EXPECT_LT(position(walker).x, 2.0f - 0.5f - 0.4f + 0.05f);

    // under a ceiling 2 m up: a jump hits it
    const Entity jumper = controller(Vector3(-10.0f, 0.0f, 0.0f));
    CharacterController up(scene.get(), jumper);
    steps(5);
    bool above = false;
    steps(30, [&](){ above = above || (up.move(Vector3(0.0f, 3.0f, 0.0f) * DT) & CharacterCollision_Above); });
    EXPECT_TRUE(above);
    EXPECT_LT(foot(jumper), 2.0f - 1.8f + 0.05f);
}

TEST_F(CharacterControllerTest, MovingPlatform){
    floor();
    const Entity platform = body(Vector3(0.0f, 1.0f, 0.0f), Vector3(3.0f, 0.2f, 3.0f), BodyType::KINEMATIC);
    const Entity rider = controller(Vector3(0.0f, 1.1f, 0.0f));
    steps(10);
    ASSERT_TRUE(component(rider).isGrounded);
    ASSERT_EQ(component(rider).groundEntity, platform);
    Rigidbody(scene.get(), platform).setLinearVelocity(Vector3(1.0f, 0.0f, 0.0f));
    const float startX = position(rider).x;
    steps(120);
    EXPECT_NEAR(position(rider).x - startX, 2.0f, 0.05f);
    EXPECT_TRUE(component(rider).isGrounded);
    // the controller's own velocity is its moves', not the platform's
    EXPECT_NEAR(component(rider).velocity.x, 0.0f, 1e-4f);
    EXPECT_NEAR(component(rider).groundVelocity.x, 1.0f, 0.05f);
}

TEST_F(CharacterControllerTest, RotatingPlatform){
    floor();
    const Entity platform = body(Vector3(0.0f, 1.0f, 0.0f), Vector3(6.0f, 0.2f, 6.0f), BodyType::KINEMATIC);
    const Entity rider = controller(Vector3(1.0f, 1.1f, 0.0f));
    steps(10);
    ASSERT_TRUE(component(rider).isGrounded);
    const float yawBefore = yawDegrees(rider);
    Rigidbody(scene.get(), platform).setAngularVelocity(Vector3(0.0f, 90.0f * 0.01745329f, 0.0f));
    steps(60);
    float turned = yawDegrees(rider) - yawBefore;
    if (turned < -180.0f) turned += 360.0f;
    if (turned > 180.0f) turned -= 360.0f;
    EXPECT_NEAR(turned, 90.0f, 3.0f);
    // carried round with it: a quarter turn about the platform's centre
    EXPECT_NEAR(position(rider).x, 0.0f, 0.1f);
    EXPECT_NEAR(std::fabs(position(rider).z), 1.0f, 0.1f);
}

TEST_F(CharacterControllerTest, PlatformDropsAway){
    const Entity platform = body(Vector3(0.0f, 1.0f, 0.0f), Vector3(3.0f, 0.2f, 3.0f), BodyType::KINEMATIC);
    const Entity rider = controller(Vector3(0.0f, 1.1f, 0.0f));
    steps(10);
    ASSERT_TRUE(component(rider).isGrounded);
    // taken away under it (teleported 20 m down)
    Transform& platformTransform = scene->getComponent<Transform>(platform);
    platformTransform.position = Vector3(0.0f, -20.0f, 0.0f);
    platformTransform.needUpdate = true;
    int steppedUntilAir = 0;
    while (component(rider).isGrounded && steppedUntilAir < 10){
        step();
        steppedUntilAir++;
    }
    EXPECT_LE(steppedUntilAir, 2);
    EXPECT_FALSE(component(rider).isGrounded);
}

TEST_F(CharacterControllerTest, PushesDynamicBox){
    floor();
    // 10 kg: a 0.5 m cube at 80 kg/m3
    const Entity box = body(Vector3(1.5f, 0.25f, 0.0f), Vector3(0.5f, 0.5f, 0.5f), BodyType::DYNAMIC, Quaternion::IDENTITY, 80.0f);
    const Entity pusher = controller(Vector3(0.0f, 0.0f, 0.0f));
    CharacterController cc(scene.get(), pusher);
    steps(10);
    const float start = position(box).x;
    steps(60, [&](){ cc.simpleMove(Vector3(2.0f, 0.0f, 0.0f)); });
    EXPECT_GT(position(box).x - start, 0.5f);
}

TEST_F(CharacterControllerTest, NoPushWhenDisabled){
    floor();
    const Entity box = body(Vector3(1.5f, 0.25f, 0.0f), Vector3(0.5f, 0.5f, 0.5f), BodyType::DYNAMIC, Quaternion::IDENTITY, 80.0f);
    const Entity pusher = controller(Vector3(0.0f, 0.0f, 0.0f));
    component(pusher).pushBodies = false;
    CharacterController cc(scene.get(), pusher);
    steps(10);
    const float start = position(box).x;
    steps(60, [&](){ cc.simpleMove(Vector3(2.0f, 0.0f, 0.0f)); });
    EXPECT_LT(std::fabs(position(box).x - start), 0.01f);
}

TEST_F(CharacterControllerTest, RaycastHitsInnerBody){
    floor();
    const Entity target = controller(Vector3(0.0f, 0.0f, 0.0f));
    steps(2);
    const RayReturn hit = Ray(Vector3(-5.0f, 1.0f, 0.0f), Vector3(10.0f, 0.0f, 0.0f)).intersects(scene.get(), RayFilter::BODY_3D);
    ASSERT_TRUE(hit.hit);
    EXPECT_EQ(hit.body, target);
    EXPECT_NEAR(hit.point.x, -0.4f, 0.05f);

    // without the inner body the world does not see it
    component(target).detectCollisions = false;
    steps(1);
    const RayReturn miss = Ray(Vector3(-5.0f, 1.0f, 0.0f), Vector3(10.0f, 0.0f, 0.0f)).intersects(scene.get(), RayFilter::BODY_3D);
    EXPECT_FALSE(miss.hit && miss.body == target);
}

TEST_F(CharacterControllerTest, TriggerSeesController){
    floor();
    const Entity trigger = body(Vector3(3.0f, 1.0f, 0.0f), Vector3(1.0f, 2.0f, 2.0f), BodyType::STATIC);
    scene->getComponent<RigidbodyComponent>(trigger).sensor = true;
    const Entity walker = controller(Vector3(0.0f, 0.0f, 0.0f));
    int entered = 0;
    bool controllerWrapperValid = true;
    physics->onContactAdded3D.add("test", std::function<void(Rigidbody, Rigidbody, Contact3D)>([&](Rigidbody a, Rigidbody b, Contact3D){
        const bool aIsWalker = a.getEntity() == walker;
        const bool bIsWalker = b.getEntity() == walker;
        if ((aIsWalker && b.getEntity() == trigger) || (bIsWalker && a.getEntity() == trigger)){
            entered++;
            controllerWrapperValid = aIsWalker ? a.isValid() : b.isValid();
        }
    }));
    int otherContacts = 0;
    physics->onContactAdded3D.add("test-floor", std::function<void(Rigidbody, Rigidbody, Contact3D)>([&](Rigidbody a, Rigidbody b, Contact3D){
        if ((a.getEntity() == walker || b.getEntity() == walker) && a.getEntity() != trigger && b.getEntity() != trigger)
            otherContacts++;
    }));
    CharacterController cc(scene.get(), walker);
    steps(90, [&](){ cc.simpleMove(Vector3(2.0f, 0.0f, 0.0f)); });
    EXPECT_GE(entered, 1);
    EXPECT_FALSE(controllerWrapperValid);
    // the inner body's contacts with solid bodies are not reported (it has no Rigidbody)
    EXPECT_EQ(otherContacts, 0);
}

TEST_F(CharacterControllerTest, HitCallbackOncePerContact){
    floor();
    const Entity wall = body(Vector3(1.5f, 1.5f, 0.0f), Vector3(1.0f, 3.0f, 6.0f), BodyType::STATIC);
    const Entity walker = controller(Vector3(0.0f, 0.0f, 0.0f));
    std::vector<ControllerColliderHit> wallHits;
    int moves = 0;
    physics->onControllerColliderHit.add("test", std::function<void(Entity, ControllerColliderHit)>([&](Entity entity, ControllerColliderHit hit){
        if (entity == walker && hit.entity == wall) wallHits.push_back(hit);
    }));
    CharacterController cc(scene.get(), walker);
    steps(5);
    int movesAtWall = 0;
    steps(60, [&](){
        const int before = (int)wallHits.size();
        cc.move(Vector3(2.0f, 0.0f, 0.0f) * DT);
        moves++;
        const int added = (int)wallHits.size() - before;
        EXPECT_LE(added, 1);
        if (added == 1) movesAtWall++;
    });
    ASSERT_FALSE(wallHits.empty());
    // once per move while touching it
    EXPECT_EQ((int)wallHits.size(), movesAtWall);
    EXPECT_NEAR(wallHits.back().normal.x, -1.0f, 0.05f);
    EXPECT_NEAR(wallHits.back().moveDirection.x, 1.0f, 1e-4f);
    EXPECT_NEAR(wallHits.back().moveLength, 2.0f * DT, 1e-4f);
}

TEST_F(CharacterControllerTest, CharacterVsCharacterBlocks){
    floor();
    const Entity mover = controller(Vector3(0.0f, 0.0f, 0.0f));
    const Entity blocker = controller(Vector3(3.0f, 0.0f, 0.0f));
    CharacterController cc(scene.get(), mover);
    steps(5);
    steps(120, [&](){ cc.move(Vector3(2.0f, 0.0f, 0.0f) * DT); });
    EXPECT_LT(position(mover).x, position(blocker).x - 0.8f + 0.05f);
    EXPECT_GT(position(mover).x, 1.5f);

    // with characterCollisions off they pass through each other
    component(mover).characterCollisions = false;
    steps(120, [&](){ cc.move(Vector3(2.0f, 0.0f, 0.0f) * DT); });
    EXPECT_GT(position(mover).x, position(blocker).x + 1.0f);
}

TEST_F(CharacterControllerTest, TeleportSyncs){
    floor();
    const Entity walker = controller(Vector3(0.0f, 0.0f, 0.0f));
    CharacterController cc(scene.get(), walker);
    steps(5);
    // moved by something else (a script setting the position): the next move starts there
    Transform& transform = scene->getComponent<Transform>(walker);
    transform.position = Vector3(10.0f, 0.9f, 5.0f);
    transform.needUpdate = true;
    step([&](){ cc.move(Vector3(0.1f, 0.0f, 0.0f)); });
    EXPECT_NEAR(position(walker).x, 10.1f, 0.01f);
    EXPECT_NEAR(position(walker).z, 5.0f, 0.01f);
}

TEST_F(CharacterControllerTest, CrouchBlockedUnderCeiling){
    floor();
    const Entity walker = controller(Vector3(0.0f, 0.0f, 0.0f));
    CharacterController cc(scene.get(), walker);
    steps(5);
    // crouch: 1.0 m, the feet stay on the floor
    ASSERT_TRUE(cc.trySetSize(1.0f, 0.4f, Vector3(0.0f, -0.4f, 0.0f)));
    EXPECT_FLOAT_EQ(cc.getHeight(), 1.0f);
    // walk under a ceiling at 1.3 m
    body(Vector3(3.0f, 1.3f + 0.25f, 0.0f), Vector3(2.0f, 0.5f, 4.0f), BodyType::STATIC);
    steps(90, [&](){ cc.simpleMove(Vector3(position(walker).x < 3.0f ? 2.0f : 0.0f, 0.0f, 0.0f)); });
    ASSERT_GT(position(walker).x, 2.5f);
    ASSERT_LT(position(walker).x, 3.5f);
    // standing up there does not fit: refused, still crouched
    EXPECT_FALSE(cc.trySetSize(1.8f, 0.4f, Vector3(0.0f, 0.0f, 0.0f)));
    EXPECT_FLOAT_EQ(cc.getHeight(), 1.0f);
    EXPECT_FLOAT_EQ(component(walker).center.y, -0.4f);
    EXPECT_NEAR(foot(walker), 0.0f, 0.05f);
}

TEST_F(CharacterControllerTest, RefusedWithRigidbody){
    floor();
    const Entity both = controller(Vector3(0.0f, 0.0f, 0.0f));
    RigidbodyComponent rigidbody;
    rigidbody.type = BodyType::DYNAMIC;
    scene->addComponent<RigidbodyComponent>(both, rigidbody);
    test::LogCapture log;
    steps(3);
    EXPECT_EQ(log.count(D_LOG_ERROR, "has a Rigidbody"), 1u);
    // its checkbox (saved with the scene) stays as it was
    EXPECT_TRUE(component(both).enabled);
    EXPECT_TRUE(component(both).refusedWithRigidbody);
    EXPECT_EQ(component(both).handle, 0u);
}

TEST_F(CharacterControllerTest, ScenesWithoutRigidbodiesStep){
    // nothing but a controller: the world steps for it (its post-step pass runs root motion)
    const Entity walker = controller(Vector3(0.0f, 0.0f, 0.0f));
    steps(1);
    physics->setRootMotion(walker, Vector3(1.0f, 0.0f, 0.0f), 0.0f);
    const float start = position(walker).x;
    steps(60);
    EXPECT_NEAR(position(walker).x - start, 1.0f, 0.03f);
}

TEST_F(CharacterControllerTest, RootMotionVelocityConsumedInFixedSteps){
    floor();
    const Entity walker = controller(Vector3(0.0f, 0.0f, 0.0f));
    steps(5);
    const float start = position(walker).x;
    // published once a frame by the Animator, applied in every fixed step until replaced
    physics->setRootMotion(walker, Vector3(1.0f, 0.0f, 0.0f), 0.5f);
    const float yawBefore = yawDegrees(walker);
    steps(60);
    EXPECT_NEAR(position(walker).x - start, 1.0f, 0.03f);
    float turned = yawDegrees(walker) - yawBefore;
    EXPECT_NEAR(turned, 0.5f * 57.29578f, 0.5f);

    // stopped by a wall
    body(Vector3(2.5f, 1.5f, 0.0f), Vector3(1.0f, 3.0f, 6.0f), BodyType::STATIC);
    physics->setRootMotion(walker, Vector3(1.0f, 0.0f, 0.0f), 0.0f);
    steps(180);
    EXPECT_LT(position(walker).x, 2.0f - 0.4f + 0.05f);

    // a value that is not finite is dropped, once warned
    test::LogCapture log;
    physics->setRootMotion(walker, Vector3(NAN, 0.0f, 0.0f), 0.0f);
    physics->setRootMotion(walker, Vector3(INFINITY, 0.0f, 0.0f), 0.0f);
    EXPECT_EQ(log.count(D_LOG_WARN, "not finite"), 1u);
}
