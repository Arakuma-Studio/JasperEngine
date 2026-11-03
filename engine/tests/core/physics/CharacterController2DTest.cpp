// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The 2D CharacterController (docs/specs/S22-gameplay.md D2, P5, AC19): our collide-and-slide on
// Box2D's casts, held to the thresholds of the 3D controller's AC6-AC9 in 2D points (64 points a
// metre, the default pointsToMeterScale2D). A scene with only its PhysicsSystem running, stepped at
// 60 Hz as the engine's fixed phase does (scripts' moves first, then the step).

#include "Engine.h"
#include "Input.h"
#include "LogCapture.h"
#include "Scene.h"
#include "component/CharacterController2DComponent.h"
#include "component/Rigidbody2DComponent.h"
#include "component/Transform.h"
#include "object/physics/CharacterController2D.h"
#include "object/physics/Rigidbody2D.h"
#include "registry/PhysicsLayers.h"
#include "subsystem/PhysicsSystem.h"
#include "subsystem/RenderSystem.h"
#include "util/Angle.h"
#include "../ecs/EcsTestUtil.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>

using namespace jasperengine;

namespace {

    constexpr float DT = 1.0f / 60.0f;
    // points a metre
    constexpr float M = 64.0f;

    class CharacterController2DTest : public ::testing::Test {
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
            physics->setPointsToMeterScale2D(M);
            physics->setGravity2D(0.0f, -9.81f);
        }

        void TearDown() override{
            scene.reset();
            Engine::setUpdateTime(updateTime);
        }

        // a box of a size (points) centred at a position, turned by degrees
        Entity box(Vector2 position, Vector2 size, BodyType type, float degrees = 0.0f, float density = 1.0f){
            const Entity entity = test::makeTransformEntity(*scene);
            Transform& transform = scene->getComponent<Transform>(entity);
            transform.position = Vector3(position.x, position.y, 0.0f);
            transform.rotation = Quaternion(Angle::degToDefault(degrees), Vector3(0.0f, 0.0f, 1.0f));
            transform.needUpdate = true;
            Rigidbody2DComponent rigidbody;
            rigidbody.type = type;
            rigidbody.colliders.resize(1);
            rigidbody.colliders[0].type = Collider2DType::POLYGON;
            rigidbody.colliders[0].pointA = Vector2(-size.x * 0.5f, -size.y * 0.5f);
            rigidbody.colliders[0].pointB = Vector2(size.x * 0.5f, size.y * 0.5f);
            rigidbody.colliders[0].density = density;
            scene->addComponent<Rigidbody2DComponent>(entity, rigidbody);
            return entity;
        }

        // a capsule standing with its feet at `foot` (points)
        Entity controller(Vector2 foot, float radius = 0.4f * M, float height = 1.8f * M, float stepOffset = 0.3f * M){
            const Entity entity = test::makeTransformEntity(*scene);
            Transform& transform = scene->getComponent<Transform>(entity);
            transform.position = Vector3(foot.x, foot.y + height * 0.5f, 0.0f);
            transform.needUpdate = true;
            CharacterController2DComponent component;
            component.radius = radius;
            component.height = height;
            component.stepOffset = stepOffset;
            scene->addComponent<CharacterController2DComponent>(entity, component);
            return entity;
        }

        Vector2 position(Entity entity){
            const Vector3 p = scene->getComponent<Transform>(entity).worldPosition;
            return Vector2(p.x, p.y);
        }

        float foot(Entity entity){
            const CharacterController2DComponent& c = scene->getComponent<CharacterController2DComponent>(entity);
            return position(entity).y + c.center.y - c.height * 0.5f;
        }

        CharacterController2DComponent& component(Entity entity){
            return scene->getComponent<CharacterController2DComponent>(entity);
        }

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

        Entity floor(float width = 40.0f * M){
            return box(Vector2(0.0f, -0.5f * M), Vector2(width, 1.0f * M), BodyType::STATIC);
        }

        // 6 steps of 0.2 m risers and 0.3 m treads from x = startX, and a top landing
        void stairs(float startX = 0.0f){
            for (int k = 1; k <= 6; k++){
                const float start = startX + (k - 1) * 0.3f * M;
                const float end = startX + (6 * 0.3f + 10.0f) * M;
                const float height = 0.2f * M * k;
                box(Vector2((start + end) * 0.5f, height * 0.5f), Vector2(end - start, height), BodyType::STATIC);
            }
        }
    };

}

TEST_F(CharacterController2DTest, Stairs){
    floor();
    stairs();
    const Entity walker = controller(Vector2(-1.0f * M, 0.0f));
    CharacterController2D cc(scene.get(), walker);
    steps(30, [&](){ cc.simpleMove(Vector2(0.0f, 0.0f)); });
    ASSERT_TRUE(cc.isGrounded());

    steps(240, [&](){ cc.simpleMove(Vector2(2.0f * M, 0.0f)); });
    EXPECT_NEAR(foot(walker), 1.2f * M, 0.05f * M);
    EXPECT_TRUE(cc.isGrounded());
    EXPECT_GT(position(walker).x, 1.8f * M);

    // and down again past the first riser, never off the ground more than 2 steps in a row
    // (stick to floor); a step climbed lands up to a radius further on, so it walks back further
    int airborne = 0, longest = 0;
    steps(360, [&](){
        cc.simpleMove(Vector2(-2.0f * M, 0.0f));
        airborne = cc.isGrounded() ? 0 : airborne + 1;
        longest = std::max(longest, airborne);
    });
    EXPECT_NEAR(foot(walker), 0.0f, 0.05f * M);
    EXPECT_LE(longest, 2);
}

TEST_F(CharacterController2DTest, StepOffsetUnderTheRiserStops){
    floor();
    stairs();
    const Entity walker = controller(Vector2(-1.0f * M, 0.0f), 0.4f * M, 1.8f * M, 0.1f * M);
    CharacterController2D cc(scene.get(), walker);
    steps(240, [&](){ cc.simpleMove(Vector2(2.0f * M, 0.0f)); });
    EXPECT_LT(position(walker).x, 0.05f * M);
    EXPECT_NEAR(foot(walker), 0.0f, 0.05f * M);
    EXPECT_TRUE(cc.isGrounded());
}

TEST_F(CharacterController2DTest, Slopes){
    floor(80.0f * M);
    // a 30 degree ramp rising along +X from x = 0
    const float rise30 = 30.0f * 0.01745329f;
    box(Vector2(std::cos(rise30) * 10.0f * M, std::sin(rise30) * 10.0f * M - 0.25f * M), Vector2(20.0f * M, 0.5f * M), BodyType::STATIC, 30.0f);
    const Entity climber = controller(Vector2(-1.0f * M, 0.0f));
    CharacterController2D cc(scene.get(), climber);
    steps(10, [&](){ cc.simpleMove(Vector2(0.0f, 0.0f)); });
    const float start = foot(climber);
    steps(180, [&](){ cc.simpleMove(Vector2(2.0f * M, 0.0f)); });
    EXPECT_GT(foot(climber) - start, 1.0f * M);
}

TEST_F(CharacterController2DTest, SteepSlope){
    // a 50 degree ramp in the air: the controller stands on it, cannot climb it, is not grounded
    const float rise50 = 50.0f * 0.01745329f;
    const Vector2 rampCenter(0.0f, 30.0f * M);
    box(rampCenter, Vector2(60.0f * M, 0.5f * M), BodyType::STATIC, 50.0f);
    const Vector2 normal(-std::sin(rise50), std::cos(rise50));
    const Entity slider = controller(rampCenter + normal * (0.25f * M) + Vector2(0.0f, 0.1f * M));
    CharacterController2D cc(scene.get(), slider);
    steps(2, [&](){ cc.simpleMove(Vector2(0.0f, 0.0f)); });
    const float before = foot(slider);
    bool sawSteep = false, sawGrounded = false;
    steps(60, [&](){
        cc.simpleMove(Vector2(2.0f * M, 0.0f));
        sawSteep = sawSteep || cc.isOnSteepSlope();
        sawGrounded = sawGrounded || cc.isGrounded();
    });
    EXPECT_LT(foot(slider) - before, 0.05f * M);
    EXPECT_TRUE(sawSteep);
    EXPECT_FALSE(sawGrounded);
}

TEST_F(CharacterController2DTest, CollisionFlags){
    floor();
    // a wall at x = 2 m and a ceiling 2.2 m up over x in [-6, -4] m
    box(Vector2(2.5f * M, 2.0f * M), Vector2(1.0f * M, 4.0f * M), BodyType::STATIC);
    box(Vector2(-5.0f * M, 2.45f * M), Vector2(2.0f * M, 0.5f * M), BodyType::STATIC);
    const Entity walker = controller(Vector2(0.0f, 0.0f));
    CharacterController2D cc(scene.get(), walker);
    steps(5, [&](){ cc.simpleMove(Vector2(0.0f, 0.0f)); });
    int flags = 0;
    steps(90, [&](){ flags = cc.move(Vector2(2.0f * M * DT, -0.5f * M * DT)); });
    EXPECT_TRUE(flags & CharacterCollision_Sides);
    EXPECT_TRUE(flags & CharacterCollision_Below);
    EXPECT_LT(position(walker).x, 2.0f * M - 0.4f * M + 0.05f * M);

    // under the ceiling: up into it
    const Entity jumper = controller(Vector2(-5.0f * M, 0.0f));
    CharacterController2D up(scene.get(), jumper);
    steps(30, [&](){ flags = up.move(Vector2(0.0f, 2.0f * M * DT)); });
    EXPECT_TRUE(flags & CharacterCollision_Above);
    EXPECT_LT(foot(jumper) + 1.8f * M, 2.2f * M + 0.01f * M);
}

TEST_F(CharacterController2DTest, MovingPlatform){
    const Entity platform = box(Vector2(0.0f, 1.0f * M), Vector2(6.0f * M, 0.2f * M), BodyType::KINEMATIC);
    const Entity rider = controller(Vector2(0.0f, 1.1f * M));
    steps(2);
    ASSERT_TRUE(component(rider).isGrounded);
    Rigidbody2D(scene.get(), platform).setLinearVelocity(Vector2(1.0f * M, 0.0f));
    const float start = position(rider).x;
    // an idle controller: no move calls
    steps(120);
    EXPECT_NEAR(position(rider).x - start, 2.0f * M, 0.05f * M);
    EXPECT_TRUE(component(rider).isGrounded);
    EXPECT_EQ(component(rider).groundEntity, platform);
    EXPECT_NEAR(component(rider).groundVelocity.x, 1.0f * M, 0.01f * M);
}

TEST_F(CharacterController2DTest, PlatformDropsAway){
    const Entity platform = box(Vector2(0.0f, 1.0f * M), Vector2(3.0f * M, 0.2f * M), BodyType::KINEMATIC);
    const Entity rider = controller(Vector2(0.0f, 1.1f * M));
    steps(10);
    ASSERT_TRUE(component(rider).isGrounded);
    Rigidbody2D(scene.get(), platform).setPosition(Vector2(0.0f, -20.0f * M));
    int steppedUntilAir = 0;
    while (component(rider).isGrounded && steppedUntilAir < 10){
        step();
        steppedUntilAir++;
    }
    EXPECT_LE(steppedUntilAir, 2);
    EXPECT_FALSE(component(rider).isGrounded);
}

TEST_F(CharacterController2DTest, Push){
    floor();
    // 10 kg: a 0.5 m square at 40 kg/m2
    const Entity crate = box(Vector2(1.5f * M, 0.25f * M), Vector2(0.5f * M, 0.5f * M), BodyType::DYNAMIC, 0.0f, 40.0f);
    const Entity pusher = controller(Vector2(0.0f, 0.0f));
    CharacterController2D cc(scene.get(), pusher);
    steps(10, [&](){ cc.simpleMove(Vector2(0.0f, 0.0f)); });
    ASSERT_NEAR(Rigidbody2D(scene.get(), crate).getMass(), 10.0f, 0.01f);
    Rigidbody2D body(scene.get(), crate);
    const float start = body.getPosition().x;
    steps(60, [&](){ cc.simpleMove(Vector2(2.0f * M, 0.0f)); });
    EXPECT_GT(body.getPosition().x - start, 0.5f * M);
}

TEST_F(CharacterController2DTest, NoPushWhenDisabled){
    floor();
    const Entity crate = box(Vector2(1.5f * M, 0.25f * M), Vector2(0.5f * M, 0.5f * M), BodyType::DYNAMIC, 0.0f, 40.0f);
    const Entity pusher = controller(Vector2(0.0f, 0.0f));
    component(pusher).pushBodies = false;
    CharacterController2D cc(scene.get(), pusher);
    steps(10, [&](){ cc.simpleMove(Vector2(0.0f, 0.0f)); });
    Rigidbody2D body(scene.get(), crate);
    const float start = body.getPosition().x;
    steps(60, [&](){ cc.simpleMove(Vector2(2.0f * M, 0.0f)); });
    EXPECT_LT(std::fabs(body.getPosition().x - start), 0.01f * M);
    // stopped at the crate, a skin off it
    EXPECT_LT(position(pusher).x, 1.25f * M - 0.4f * M);
}

TEST_F(CharacterController2DTest, LayerMatrixAndMask){
    floor();
    const Entity wall = box(Vector2(1.5f * M, 2.0f * M), Vector2(0.5f * M, 4.0f * M), BodyType::STATIC);
    scene->setEntityLayer(wall, 3);
    Physics2D::ignoreLayerCollision(0, 3, true);
    const Entity walker = controller(Vector2(0.0f, 0.0f));
    CharacterController2D cc(scene.get(), walker);
    steps(90, [&](){ cc.simpleMove(Vector2(2.0f * M, 0.0f)); });
    // through the ignored layer
    EXPECT_GT(position(walker).x, 2.0f * M);
    Physics2D::resetLayerCollisionMatrix();

    // the collider bits: a controller whose mask leaves the wall's category out goes through
    Entity second = controller(Vector2(-3.0f * M, 0.0f));
    component(second).maskBits = 0xFFFF & ~2;
    Rigidbody2DComponent& rigidbody = scene->getComponent<Rigidbody2DComponent>(box(Vector2(-1.5f * M, 2.0f * M), Vector2(0.5f * M, 4.0f * M), BodyType::STATIC));
    rigidbody.colliders[0].categoryBits = 2;
    rigidbody.needUpdateColliders = true;
    CharacterController2D through(scene.get(), second);
    // 2.5 m in 75 steps: from x = -3 m past the wall at -1.5 m
    steps(75, [&](){ through.simpleMove(Vector2(2.0f * M, 0.0f)); });
    EXPECT_GT(position(second).x, -1.0f * M);
}

TEST_F(CharacterController2DTest, TeleportFollowsTheTransform){
    floor();
    const Entity walker = controller(Vector2(0.0f, 0.0f));
    CharacterController2D cc(scene.get(), walker);
    steps(5, [&](){ cc.simpleMove(Vector2(0.0f, 0.0f)); });
    Transform& transform = scene->getComponent<Transform>(walker);
    transform.position = Vector3(10.0f * M, 0.9f * M + 1.0f, 0.0f);
    transform.needUpdate = true;
    steps(5, [&](){ cc.simpleMove(Vector2(0.0f, 0.0f)); });
    EXPECT_NEAR(position(walker).x, 10.0f * M, 0.01f);
    EXPECT_TRUE(cc.isGrounded());
}

TEST_F(CharacterController2DTest, RefusedWithRigidbody){
    floor();
    const Entity both = controller(Vector2(0.0f, 0.0f));
    Rigidbody2DComponent rigidbody;
    rigidbody.type = BodyType::KINEMATIC;
    scene->addComponent<Rigidbody2DComponent>(both, rigidbody);
    test::LogCapture log;
    CharacterController2D cc(scene.get(), both);
    const Vector2 before = position(both);
    EXPECT_EQ(cc.move(Vector2(1.0f * M, 0.0f)), CharacterCollision_None);
    EXPECT_EQ(position(both), before);
    EXPECT_TRUE(component(both).refusedWithRigidbody);
    EXPECT_TRUE(log.contains("Rigidbody 2D"));
}

TEST_F(CharacterController2DTest, NoWorldMovesFreely){
    // no 2D bodies: Box2D's world was never made, the move is the motion
    const Entity walker = controller(Vector2(0.0f, 0.0f));
    CharacterController2D cc(scene.get(), walker);
    cc.move(Vector2(10.0f, -5.0f));
    EXPECT_NEAR(position(walker).x, 10.0f, 1e-4f);
    EXPECT_NEAR(foot(walker), -5.0f, 1e-4f);
    EXPECT_FALSE(cc.isGrounded());
    step();
    EXPECT_FALSE(cc.isGrounded());
}

