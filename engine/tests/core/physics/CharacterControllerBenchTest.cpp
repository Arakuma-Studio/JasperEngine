// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The CharacterController's budgets (docs/specs/S22-gameplay.md R36, AC17): one move within
// 0.1 ms in a scene of 1000 static bodies, and ten controllers moving plus the physics step's
// post-step pass within 1 ms a step (the mobile budget; a desktop runner has it with room).
// Like every *Bench suite the budget is enforced with JASPER_TEST_PERF_SLACK (the benchmark run
// on server E), failing at 1.5x it, and only printed otherwise (docs/testing.md).

#include "Engine.h"
#include "Input.h"
#include "Scene.h"
#include "component/CharacterControllerComponent.h"
#include "component/RigidbodyComponent.h"
#include "component/Transform.h"
#include "object/physics/CharacterController.h"
#include "subsystem/PhysicsSystem.h"
#include "subsystem/RenderSystem.h"
#include "support/AllocationCounter.h"
#include "../ecs/EcsTestUtil.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <memory>
#include <vector>

using namespace jasperengine;

namespace {

    constexpr float DT = 1.0f / 60.0f;

    struct BenchScene{
        std::unique_ptr<Scene> scene = std::make_unique<Scene>();
        PhysicsSystem* physics = nullptr;
        float updateTime = Engine::getUpdateTime();

        BenchScene(){
            Engine::setUpdateTime(DT);
            test::disableSubSystems(*scene);
            scene->getScheduler().setEnabled("PhysicsSystem", true);
            physics = scene->getSystem<PhysicsSystem>().get();
            physics->setGravity3D(0.0f, -9.81f, 0.0f);
        }

        ~BenchScene(){
            scene.reset();
            Engine::setUpdateTime(updateTime);
        }

        Entity box(Vector3 position, Vector3 size){
            const Entity entity = test::makeTransformEntity(*scene);
            Transform& transform = scene->getComponent<Transform>(entity);
            transform.position = position;
            transform.needUpdate = true;
            RigidbodyComponent rigidbody;
            rigidbody.type = BodyType::STATIC;
            rigidbody.colliders.resize(1);
            rigidbody.colliders[0].type = Collider3DType::BOX;
            rigidbody.colliders[0].width = size.x;
            rigidbody.colliders[0].height = size.y;
            rigidbody.colliders[0].depth = size.z;
            scene->addComponent<RigidbodyComponent>(entity, rigidbody);
            return entity;
        }

        Entity controller(Vector3 foot, bool innerBody = true){
            const Entity entity = test::makeTransformEntity(*scene);
            Transform& transform = scene->getComponent<Transform>(entity);
            transform.position = foot + Vector3(0.0f, 1.0f, 0.0f);
            transform.needUpdate = true;
            CharacterControllerComponent component;
            component.detectCollisions = innerBody;
            scene->addComponent<CharacterControllerComponent>(entity, component);
            return entity;
        }

        void resolveTransforms(){
            auto transforms = scene->getComponentArray<Transform>();
            for (size_t i = 0; i < transforms->size(); i++){
                Transform& transform = transforms->getComponentFromIndex(i);
                if (transform.needUpdate)
                    scene->getSystem<RenderSystem>()->updateTransform(transform, transforms->getEntity(i));
            }
        }

        void step(){
            resolveTransforms();
            Input::beginFixedStep();
            scene->fixedUpdate(DT);
            Input::endFixedStep();
            resolveTransforms();
        }

        // a floor and 1000 static boxes, 1 m apart on a 40 x 25 grid around the origin
        void city(){
            box(Vector3(0.0f, -0.5f, 0.0f), Vector3(100.0f, 1.0f, 100.0f));
            for (int i = 0; i < 1000; i++){
                const float x = -20.0f + (float)(i % 40) * 1.0f;
                const float z = -12.0f + (float)(i / 40) * 1.0f;
                box(Vector3(x, 1.5f, z), Vector3(0.3f, 3.0f, 0.3f));
            }
        }
    };

}

TEST(CharacterControllerBench, MoveIn1kStaticBodies){
    BenchScene s;
    s.city();
    // in an aisle between the box rows (x = -19.5 + k), walking along z through it
    const Entity walker = s.controller(Vector3(-0.5f, 0.0f, -14.0f));
    CharacterController cc(s.scene.get(), walker);
    for (int i = 0; i < 5; i++)
        s.step();

    constexpr int moves = 600;
    double total = 0.0;
    for (int i = 0; i < moves; i++){
        const Vector3 motion(0.0f, -0.02f, (i / 150) % 2 == 0 ? 0.03f : -0.03f);
        const auto start = std::chrono::steady_clock::now();
        cc.move(motion);
        total += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (i % 10 == 0)
            s.step();
    }
    const double ms = total / moves;
    std::printf("[bench] CharacterController move in 1k static bodies %.4f ms (budget 0.1 ms desktop)\n", ms);
    EXPECT_TRUE(cc.isGrounded());
    if (testsupport::perfSlack() > 0.0){
        EXPECT_LE(ms, 0.1 * 1.5 * testsupport::perfSlack());
    }
}

TEST(CharacterControllerBench, TenControllersAndThePostStep){
    BenchScene s;
    s.city();
    std::vector<CharacterController> controllers;
    for (int k = 0; k < 10; k++)
        controllers.emplace_back(s.scene.get(), s.controller(Vector3(-19.5f + 2.0f * (float)k, 0.0f, -14.0f)));
    for (int i = 0; i < 5; i++)
        s.step();

    constexpr int steps = 300;
    double total = 0.0, moving = 0.0;
    for (int i = 0; i < steps; i++){
        const float speed = (i / 75) % 2 == 0 ? 2.0f : -2.0f;
        const auto start = std::chrono::steady_clock::now();
        for (CharacterController& cc : controllers)
            cc.simpleMove(Vector3(0.0f, 0.0f, speed));
        const double moved = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        moving += moved;
        // the frame's Transform pass is not the controllers' cost (and the test's own loop)
        s.resolveTransforms();
        const auto stepStart = std::chrono::steady_clock::now();
        Input::beginFixedStep();
        s.scene->fixedUpdate(DT);
        Input::endFixedStep();
        total += moved + std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - stepStart).count();
        s.resolveTransforms();
    }
    const double ms = total / steps;
    std::printf("[bench] 10 CharacterControllers + the physics step %.4f ms (moves %.4f ms; budget 1 ms mobile)\n", ms, moving / steps);
    for (CharacterController& cc : controllers){
        EXPECT_TRUE(cc.isGrounded());
    }
    if (testsupport::perfSlack() > 0.0){
        EXPECT_LE(ms, 1.0 * 1.5 * testsupport::perfSlack());
    }
}

TEST(CharacterControllerBench, TenControllersWithoutInnerBodies){
    BenchScene s;
    s.city();
    std::vector<CharacterController> controllers;
    for (int k = 0; k < 10; k++)
        controllers.emplace_back(s.scene.get(), s.controller(Vector3(-19.5f + 2.0f * (float)k, 0.0f, -14.0f), false));
    for (int i = 0; i < 5; i++)
        s.step();

    constexpr int steps = 300;
    double total = 0.0, moving = 0.0;
    for (int i = 0; i < steps; i++){
        const float speed = (i / 75) % 2 == 0 ? 2.0f : -2.0f;
        const auto start = std::chrono::steady_clock::now();
        for (CharacterController& cc : controllers)
            cc.simpleMove(Vector3(0.0f, 0.0f, speed));
        const double moved = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        moving += moved;
        // the frame's Transform pass is not the controllers' cost (and the test's own loop)
        s.resolveTransforms();
        const auto stepStart = std::chrono::steady_clock::now();
        Input::beginFixedStep();
        s.scene->fixedUpdate(DT);
        Input::endFixedStep();
        total += moved + std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - stepStart).count();
        s.resolveTransforms();
    }
    const double ms = total / steps;
    std::printf("[bench] 10 CharacterControllers (no inner body) + the physics step %.4f ms (moves %.4f ms; budget 1 ms mobile)\n", ms, moving / steps);
    for (CharacterController& cc : controllers){
        EXPECT_TRUE(cc.isGrounded());
    }
    if (testsupport::perfSlack() > 0.0){
        EXPECT_LE(ms, 1.0 * 1.5 * testsupport::perfSlack());
    }
}
