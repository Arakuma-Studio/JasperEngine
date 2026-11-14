// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The vehicles' budget (docs/specs/S96-physics-gaps.md REQ-41, AC-8): 20 four-wheel cars driving
// add at most 0.25 ms to a fixed step over the same 20 bodies (same collider, same mass) moving
// at the same speed without wheels: those float (no gravity, never asleep) so neither side has
// body contacts, and the difference is the vehicles' own work (the VehicleSystem's passes, the
// wheels' ground casts, the constraint's solve). Rounds alternate the two scenes; the median
// step of each round, then the median round. Like every *Bench suite the budget is enforced
// with JASPER_TEST_PERF_SLACK (the benchmark run), failing at 1.5x it, and only printed
// otherwise (docs/testing.md).

#include "Engine.h"
#include "Input.h"
#include "Scene.h"
#include "component/RigidbodyComponent.h"
#include "component/Transform.h"
#include "component/WheelColliderComponent.h"
#include "object/physics/Rigidbody.h"
#include "subsystem/PhysicsSystem.h"
#include "subsystem/RenderSystem.h"
#include "subsystem/VehicleSystem.h"
#include "support/AllocationCounter.h"
#include "../ecs/EcsTestUtil.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <vector>

using namespace jasperengine;

namespace {

    constexpr float VEHICLE_BENCH_DT = 1.0f / 60.0f;
    constexpr int VEHICLE_BENCH_CARS = 20;

    struct VehicleBenchScene{
        std::unique_ptr<Scene> scene = std::make_unique<Scene>();
        PhysicsSystem* physics = nullptr;
        float updateTime = Engine::getUpdateTime();
        bool withWheels = false;
        std::vector<Entity> bodies;
        std::vector<Entity> wheels;

        explicit VehicleBenchScene(bool withWheels): withWheels(withWheels){
            Engine::setUpdateTime(VEHICLE_BENCH_DT);
            test::disableSubSystems(*scene);
            scene->getScheduler().setEnabled("PhysicsSystem", true);
            physics = scene->getSystem<PhysicsSystem>().get();
            physics->setGravity3D(0.0f, -9.81f, 0.0f);
            // one thread for the step, as the web build: the cost is the work, not the pool's
            physics->setSynchronousContactEvents3D(true);
            build();
        }

        ~VehicleBenchScene(){
            scene.reset();
            Engine::setUpdateTime(updateTime);
        }

        Entity box(Vector3 position, Vector3 size, BodyType type){
            const Entity entity = test::makeTransformEntity(*scene);
            Transform& transform = scene->getComponent<Transform>(entity);
            transform.position = position;
            transform.needUpdate = true;
            RigidbodyComponent rigidbody;
            rigidbody.type = type;
            rigidbody.colliders.resize(1);
            rigidbody.colliders[0].type = Collider3DType::BOX;
            rigidbody.colliders[0].width = size.x;
            rigidbody.colliders[0].height = size.y;
            rigidbody.colliders[0].depth = size.z;
            scene->addComponent<RigidbodyComponent>(entity, rigidbody);
            return entity;
        }

        // a floor and 20 car bodies side by side, driving along +Z (or floating at the same speed)
        void build(){
            box(Vector3(0.0f, -0.5f, 0.0f), Vector3(400.0f, 1.0f, 2000.0f), BodyType::STATIC);
            for (int k = 0; k < VEHICLE_BENCH_CARS; k++){
                const Entity body = box(Vector3(-50.0f + 5.0f * (float)k, 0.75f, -900.0f), Vector3(1.6f, 0.5f, 3.6f), BodyType::DYNAMIC);
                RigidbodyComponent& rigidbody = scene->getComponent<RigidbodyComponent>(body);
                rigidbody.automaticMass = false;
                rigidbody.mass = 1500.0f;
                bodies.push_back(body);
                if (!withWheels){
                    rigidbody.gravityFactor = 0.0f;
                    rigidbody.allowSleeping = false;
                    rigidbody.linearDamping = 0.0f;
                    continue;
                }
                const Vector3 corners[4] = {Vector3(-0.8f, -0.25f, 1.3f), Vector3(0.8f, -0.25f, 1.3f), Vector3(-0.8f, -0.25f, -1.3f), Vector3(0.8f, -0.25f, -1.3f)};
                for (const Vector3& corner : corners){
                    const Entity wheel = test::makeTransformEntity(*scene, body);
                    Transform& transform = scene->getComponent<Transform>(wheel);
                    transform.position = corner;
                    transform.needUpdate = true;
                    WheelColliderComponent component;
                    component.radius = 0.35f;
                    scene->addComponent<WheelColliderComponent>(wheel, component);
                    wheels.push_back(wheel);
                }
            }
        }

        void resolveTransforms(){
            auto transforms = scene->getComponentArray<Transform>();
            for (size_t i = 0; i < transforms->size(); i++){
                Transform& transform = transforms->getComponentFromIndex(i);
                if (transform.needUpdate)
                    scene->getSystem<RenderSystem>()->updateTransform(transform, transforms->getEntity(i));
            }
        }

        // the scripts' part of a step: the wheels' inputs (a speed held at about 8 m/s, a slow
        // weave), or the floating bodies' speed
        void drive(int i){
            if (!withWheels){
                for (Entity body : bodies){
                    Rigidbody(scene.get(), body).setLinearVelocity(Vector3(0.0f, 0.0f, 8.0f));
                }
                return;
            }
            for (size_t w = 0; w < wheels.size(); w++){
                WheelColliderComponent& wheel = scene->getComponent<WheelColliderComponent>(wheels[w]);
                const float speed = Rigidbody(scene.get(), bodies[w / 4]).getLinearVelocity().z;
                wheel.motorTorque = (w % 4) >= 2 ? std::clamp((8.0f - speed) * 400.0f, -800.0f, 800.0f) : 0.0f;
                wheel.steerAngle = (w % 4) < 2 ? ((i / 120) % 2 == 0 ? 3.0f : -3.0f) : 0.0f;
            }
        }

        void steps(int count, std::vector<double>* times){
            for (int i = 0; i < count; i++){
                drive(i);
                resolveTransforms();
                const auto start = std::chrono::steady_clock::now();
                Input::beginFixedStep();
                scene->fixedUpdate(VEHICLE_BENCH_DT);
                Input::endFixedStep();
                if (times)
                    times->push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
            }
        }
    };

    double vehicleBenchMedian(std::vector<double> values){
        std::sort(values.begin(), values.end());
        return values[values.size() / 2];
    }

}

TEST(VehicleBench, TwentyCarsStep){
    constexpr int rounds = 5;
    std::vector<double> without, with, added, pre, post, joltWith, joltWithout;
    float speed = 0.0f;
    size_t vehicles = 0;
    for (int r = 0; r < rounds; r++){
        std::vector<double> timesWithout, timesWith;
        {
            VehicleBenchScene s(false);
            s.steps(120, nullptr);
            s.physics->resetTimings3D();
            s.steps(300, &timesWithout);
            joltWithout.push_back(s.physics->getStepSeconds3D() * 1000.0 / 300.0);
        }
        {
            VehicleBenchScene s(true);
            s.steps(120, nullptr);
            s.physics->resetTimings3D();
            s.physics->getVehicles3D().resetTimings();
            s.steps(300, &timesWith);
            joltWith.push_back(s.physics->getStepSeconds3D() * 1000.0 / 300.0);
            pre.push_back(s.physics->getVehicles3D().getPreStepSeconds() * 1000.0 / 300.0);
            post.push_back(s.physics->getVehicles3D().getPostStepSeconds() * 1000.0 / 300.0);
            speed = Rigidbody(s.scene.get(), s.bodies[0]).getLinearVelocity().z;
            vehicles = s.physics->getVehicles3D().count();
        }
        without.push_back(vehicleBenchMedian(timesWithout));
        with.push_back(vehicleBenchMedian(timesWith));
        added.push_back(with.back() - without.back());
    }
    const double cost = vehicleBenchMedian(added);
    std::printf("[bench] 20 four-wheel vehicles add %.4f ms to a fixed step (median of %d rounds: %.4f ms with vs %.4f ms the same bodies "
                "floating; cars at %.1f m/s; budget 0.25 ms desktop)\n", cost, rounds, vehicleBenchMedian(with), vehicleBenchMedian(without), speed);
    // where it goes (means per step): the VehicleSystem's passes and Jolt's step (the wheels' casts and solve in it)
    std::printf("[bench] vehicles' preStep %.4f ms, postStep %.4f ms, Jolt step %.4f ms vs %.4f ms without wheels\n",
                vehicleBenchMedian(pre), vehicleBenchMedian(post), vehicleBenchMedian(joltWith), vehicleBenchMedian(joltWithout));
    EXPECT_EQ(vehicles, (size_t)VEHICLE_BENCH_CARS);
    EXPECT_GT(speed, 4.0f);
    if (testsupport::perfSlack() > 0.0){
        EXPECT_LE(cost, 0.25 * 1.5 * testsupport::perfSlack());
    }
}
