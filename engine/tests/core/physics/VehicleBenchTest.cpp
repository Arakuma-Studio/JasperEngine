// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The vehicles' budget (docs/specs/S96-physics-gaps.md REQ-41, AC-8): 20 four-wheel cars driving
// add at most 0.25 ms to a fixed step over the same 20 bodies without wheels. Like every *Bench
// suite the budget is enforced with JASPER_TEST_PERF_SLACK (the benchmark run), failing at 1.5x
// it, and only printed otherwise (docs/testing.md).

#include "Engine.h"
#include "Input.h"
#include "Scene.h"
#include "component/RigidbodyComponent.h"
#include "component/Transform.h"
#include "component/WheelColliderComponent.h"
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

    struct VehicleBenchScene{
        std::unique_ptr<Scene> scene = std::make_unique<Scene>();
        PhysicsSystem* physics = nullptr;
        float updateTime = Engine::getUpdateTime();
        std::vector<Entity> wheels;

        VehicleBenchScene(){
            Engine::setUpdateTime(VEHICLE_BENCH_DT);
            test::disableSubSystems(*scene);
            scene->getScheduler().setEnabled("PhysicsSystem", true);
            physics = scene->getSystem<PhysicsSystem>().get();
            physics->setGravity3D(0.0f, -9.81f, 0.0f);
        }

        ~VehicleBenchScene(){
            scene.reset();
            Engine::setUpdateTime(updateTime);
        }

        Entity box(Vector3 position, Vector3 size, BodyType type, float mass = 0.0f){
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
            if (mass > 0.0f){
                rigidbody.automaticMass = false;
                rigidbody.mass = mass;
            }
            scene->addComponent<RigidbodyComponent>(entity, rigidbody);
            return entity;
        }

        // 20 car bodies in a row over a floor, with four wheels each or none
        void cars(bool withWheels){
            box(Vector3(0.0f, -0.5f, 0.0f), Vector3(400.0f, 1.0f, 400.0f), BodyType::STATIC);
            for (int k = 0; k < 20; k++){
                // without wheels the body rests on the floor at the height the wheels hold it
                const Entity body = box(Vector3(-50.0f + 5.0f * (float)k, withWheels ? 1.0f : 0.25f, -100.0f), Vector3(1.6f, 0.5f, 3.6f), BodyType::DYNAMIC, 1500.0f);
                if (!withWheels){
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

        // the median step time (ms) over `count` steps, the wheels driving and steering
        double measure(int count){
            std::vector<double> times;
            for (int i = 0; i < count; i++){
                for (size_t w = 0; w < wheels.size(); w++){
                    WheelColliderComponent& wheel = scene->getComponent<WheelColliderComponent>(wheels[w]);
                    wheel.motorTorque = (w % 4) >= 2 ? 300.0f : 0.0f;
                    wheel.steerAngle = (w % 4) < 2 ? ((i / 120) % 2 == 0 ? 5.0f : -5.0f) : 0.0f;
                }
                resolveTransforms();
                const auto start = std::chrono::steady_clock::now();
                Input::beginFixedStep();
                scene->fixedUpdate(VEHICLE_BENCH_DT);
                Input::endFixedStep();
                times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
            }
            std::sort(times.begin(), times.end());
            return times[times.size() / 2];
        }
    };

}

TEST(VehicleBench, TwentyCarsStep){
    double without = 0.0, with = 0.0;
    {
        VehicleBenchScene s;
        s.cars(false);
        s.measure(60);
        without = s.measure(300);
    }
    {
        VehicleBenchScene s;
        s.cars(true);
        s.measure(60);
        EXPECT_EQ(s.physics->getVehicles3D().count(), 20u);
        with = s.measure(300);
    }
    const double added = with - without;
    std::printf("[bench] 20 four-wheel vehicles add %.4f ms to a fixed step (%.4f ms vs %.4f ms; budget 0.25 ms desktop)\n", added, with, without);
    if (testsupport::perfSlack() > 0.0){
        EXPECT_LE(added, 0.25 * 1.5 * testsupport::perfSlack());
    }
}
