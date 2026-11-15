// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The WheelCollider and the VehicleDrivetrain (docs/specs/S96-physics-gaps.md REQ-1..REQ-9,
// AC-1..AC-7) on Jolt's vehicle constraint: a scene with only its PhysicsSystem running, stepped
// at 60 Hz as the engine's fixed phase does (scripts' inputs first, then the step). The numbers
// each test reaches are printed ([vehicle] lines) so a Jolt upgrade shows what moved.

#include "Engine.h"
#include "Input.h"
#include "LogCapture.h"
#include "Scene.h"
#include "component/RigidbodyComponent.h"
#include "component/Transform.h"
#include "component/VehicleDrivetrainComponent.h"
#include "component/WheelColliderComponent.h"
#include "object/physics/Rigidbody.h"
#include "object/physics/VehicleDrivetrain.h"
#include "object/physics/WheelCollider.h"
#include "subsystem/PhysicsSystem.h"
#include "subsystem/RenderSystem.h"
#include "subsystem/VehicleSystem.h"
#include "util/Angle.h"
#include "../ecs/EcsTestUtil.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

using namespace jasperengine;

namespace {

    constexpr float VEHICLE_DT = 1.0f / 60.0f;
    constexpr float VEHICLE_TEST_PI = 3.14159265358979323846f;

    struct VehicleCarSpec{
        float mass = 1500.0f;
        float wheelbase = 2.6f;     // front to rear axle (m)
        float track = 1.6f;         // left to right (m)
        float radius = 0.35f;
        Vector3 position = Vector3(0.0f, 1.0f, 0.0f);
    };

    struct VehicleCar{
        Entity body = NULL_ENTITY;
        // front left, front right, rear left, rear right
        Entity wheels[4] = {NULL_ENTITY, NULL_ENTITY, NULL_ENTITY, NULL_ENTITY};
    };

    class VehicleTest : public ::testing::Test {
    protected:
        std::unique_ptr<Scene> scene;
        PhysicsSystem* physics = nullptr;
        float updateTime = 0.0f;

        void SetUp() override{
            updateTime = Engine::getUpdateTime();
            Engine::setUpdateTime(VEHICLE_DT);
            makeScene();
        }

        void TearDown() override{
            scene.reset();
            Engine::setUpdateTime(updateTime);
        }

        void makeScene(){
            scene.reset();
            scene = std::make_unique<Scene>();
            test::disableSubSystems(*scene);
            scene->getScheduler().setEnabled("PhysicsSystem", true);
            physics = scene->getSystem<PhysicsSystem>().get();
            physics->setGravity3D(0.0f, -9.81f, 0.0f);
            // one thread for the step: the same result on every run (as the web build)
            physics->setSynchronousContactEvents3D(true);
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

        Entity floor(float size = 400.0f){
            return box(Vector3(0.0f, -0.5f, 0.0f), Vector3(size, 1.0f, size), BodyType::STATIC);
        }

        // a box body (track x 0.5 x wheelbase + 1 m) with a wheel at each corner of its bottom
        VehicleCar car(const VehicleCarSpec& spec = VehicleCarSpec()){
            VehicleCar car;
            car.body = box(spec.position, Vector3(spec.track, 0.5f, spec.wheelbase + 1.0f), BodyType::DYNAMIC);
            RigidbodyComponent& rigidbody = scene->getComponent<RigidbodyComponent>(car.body);
            rigidbody.automaticMass = false;
            rigidbody.mass = spec.mass;
            const float x = spec.track * 0.5f;
            const float z = spec.wheelbase * 0.5f;
            const Vector3 corners[4] = {Vector3(-x, -0.25f, z), Vector3(x, -0.25f, z), Vector3(-x, -0.25f, -z), Vector3(x, -0.25f, -z)};
            for (int i = 0; i < 4; i++){
                const Entity wheel = test::makeTransformEntity(*scene, car.body);
                Transform& transform = scene->getComponent<Transform>(wheel);
                transform.position = corners[i];
                transform.needUpdate = true;
                WheelColliderComponent component;
                component.radius = spec.radius;
                scene->addComponent<WheelColliderComponent>(wheel, component);
                car.wheels[i] = wheel;
            }
            return car;
        }

        WheelColliderComponent& wheel(Entity entity){
            return scene->getComponent<WheelColliderComponent>(entity);
        }

        void resolveTransforms(){
            auto transforms = scene->getComponentArray<Transform>();
            for (size_t i = 0; i < transforms->size(); i++){
                Transform& transform = transforms->getComponentFromIndex(i);
                if (transform.needUpdate)
                    scene->getSystem<RenderSystem>()->updateTransform(transform, transforms->getEntity(i));
            }
        }

        void step(const std::function<void()>& scripts = nullptr){
            resolveTransforms();
            Input::beginFixedStep();
            if (scripts)
                scripts();
            scene->fixedUpdate(VEHICLE_DT);
            Input::endFixedStep();
            resolveTransforms();
        }

        void steps(int count, const std::function<void()>& scripts = nullptr){
            for (int i = 0; i < count; i++)
                step(scripts);
        }

        Rigidbody rigidbody(Entity entity){
            return Rigidbody(scene.get(), entity);
        }

        float yaw(Entity entity){
            const Vector3 forward = rigidbody(entity).getRotation() * Vector3(0.0f, 0.0f, 1.0f);
            return std::atan2(forward.x, forward.z);
        }

        void setMotor(const VehicleCar& c, float torque, bool rearOnly){
            for (int i = 0; i < 4; i++)
                wheel(c.wheels[i]).motorTorque = (rearOnly && i < 2) ? 0.0f : torque;
        }

        void setBrake(const VehicleCar& c, float torque){
            for (int i = 0; i < 4; i++)
                wheel(c.wheels[i]).brakeTorque = torque;
        }
    };

}

// AC-1 (REQ-1, REQ-2, REQ-9): the wheels under a dynamic body make one vehicle; disabling one
// builds it again with three; removing the last removes it
TEST_F(VehicleTest, WheelsMakeOneConstraint){
    floor();
    const VehicleCar c = car();
    int groundedAt = -1;
    for (int i = 0; i < 120 && groundedAt < 0; i++){
        step();
        bool all = true;
        for (Entity w : c.wheels) all = all && wheel(w).isGrounded;
        if (all) groundedAt = i + 1;
    }
    std::printf("[vehicle] all four wheels grounded after %d steps\n", groundedAt);
    ASSERT_GT(groundedAt, 0);
    EXPECT_LE(groundedAt, 60);
    EXPECT_EQ(physics->getVehicles3D().count(), 1u);

    float sprung = 0.0f;
    for (int i = 0; i < 4; i++){
        EXPECT_EQ(wheel(c.wheels[i]).wheelIndex, i);
        EXPECT_EQ(wheel(c.wheels[i]).vehicleBody, c.body);
        sprung += wheel(c.wheels[i]).sprungMass;
    }
    EXPECT_NEAR(sprung, 1500.0f, 15.0f);
    // a symmetric car holds a quarter on each wheel
    EXPECT_NEAR(wheel(c.wheels[0]).sprungMass, 375.0f, 1.0f);

    wheel(c.wheels[1]).enabled = false;
    step();
    EXPECT_EQ(physics->getVehicles3D().count(), 1u);
    EXPECT_EQ(wheel(c.wheels[1]).wheelIndex, -1);
    EXPECT_FALSE(wheel(c.wheels[1]).isGrounded);
    sprung = 0.0f;
    for (int i : {0, 2, 3}){
        EXPECT_GE(wheel(c.wheels[i]).wheelIndex, 0);
        sprung += wheel(c.wheels[i]).sprungMass;
    }
    EXPECT_NEAR(sprung, 1500.0f, 15.0f);

    for (Entity w : c.wheels) scene->removeComponent<WheelColliderComponent>(w);
    step();
    EXPECT_EQ(physics->getVehicles3D().count(), 0u);
}

// REQ-2: a wheel with no dynamic body above it does nothing and says why once
TEST_F(VehicleTest, WheelWithoutDynamicBodyWarnsOnce){
    floor();
    const Entity kinematic = box(Vector3(0.0f, 2.0f, 0.0f), Vector3(1.0f, 0.5f, 2.0f), BodyType::KINEMATIC);
    const Entity w = test::makeTransformEntity(*scene, kinematic);
    scene->addComponent<WheelColliderComponent>(w, {});
    test::LogCapture capture;
    steps(10);
    EXPECT_EQ(capture.count(D_LOG_WARN, "Wheel Collider"), 1u);
    EXPECT_EQ(physics->getVehicles3D().count(), 0u);
    EXPECT_EQ(wheel(w).wheelIndex, -1);
}

// the body mass spread over the wheels: sums to the mass, balances it about the centre of mass
TEST_F(VehicleTest, SprungMassesBalanceTheBody){
    const std::vector<Vector3> points = {Vector3(-0.8f, 0.0f, 1.0f), Vector3(0.8f, 0.0f, 1.0f), Vector3(-0.8f, 0.0f, -2.0f), Vector3(0.8f, 0.0f, -2.0f)};
    const std::vector<float> masses = VehicleSystem::sprungMasses(points, Vector3::ZERO, 1200.0f);
    float sum = 0.0f, momentX = 0.0f, momentZ = 0.0f;
    for (size_t i = 0; i < points.size(); i++){
        sum += masses[i];
        momentX += masses[i] * points[i].x;
        momentZ += masses[i] * points[i].z;
    }
    EXPECT_NEAR(sum, 1200.0f, 1e-2f);
    EXPECT_NEAR(momentX, 0.0f, 1e-1f);
    EXPECT_NEAR(momentZ, 0.0f, 1e-1f);
    // the front axle is nearer the centre of mass: it holds more
    EXPECT_GT(masses[0], masses[2]);
    // wheels on a line across the body still sum to the mass
    const std::vector<float> line = VehicleSystem::sprungMasses({Vector3(-1.0f, 0.0f, 0.0f), Vector3(1.0f, 0.0f, 0.0f)}, Vector3::ZERO, 100.0f);
    EXPECT_NEAR(line[0] + line[1], 100.0f, 1e-3f);
    EXPECT_NEAR(line[0], 50.0f, 1e-3f);
}

// AC-2 (REQ-4): the suspension rests at targetPosition of its travel under the sprung mass
TEST_F(VehicleTest, SuspensionRestsAtTargetPosition){
    floor();
    const VehicleCar c = car();
    steps(240);
    for (Entity w : c.wheels){
        const WheelColliderComponent& component = wheel(w);
        std::printf("[vehicle] target 0.5: compression %.4f\n", component.suspensionCompression);
        EXPECT_NEAR(component.suspensionCompression, 0.5f, 0.05f);
    }

    for (Entity w : c.wheels) wheel(w).suspensionSpring.targetPosition = 0.0f;
    steps(240);
    for (Entity w : c.wheels){
        const WheelColliderComponent& component = wheel(w);
        std::printf("[vehicle] target 0: compression %.4f\n", component.suspensionCompression);
        EXPECT_NEAR(component.suspensionCompression, 0.0f, 0.05f);
        EXPECT_TRUE(component.isGrounded);
    }
}

// AC-3 (REQ-3): motor torque on the rear wheels drives the car straight; the brakes stop it; a
// rolling wheel's rpm is the car's speed over its radius
TEST_F(VehicleTest, MotorTorqueAccelerates){
    floor();
    VehicleCarSpec spec;
    spec.mass = 600.0f;
    const VehicleCar c = car(spec);
    steps(60);
    const Vector3 start = rigidbody(c.body).getPosition();

    setMotor(c, 400.0f, true);
    steps(180);
    const float speed = rigidbody(c.body).getLinearVelocity().z;
    const Vector3 driven = rigidbody(c.body).getPosition();
    std::printf("[vehicle] 400 N m rear for 3 s: %.2f m/s, lateral drift %.4f m\n", speed, std::fabs(driven.x - start.x));
    EXPECT_GE(speed, 8.0f);
    EXPECT_LE(speed, 30.0f);
    EXPECT_LT(std::fabs(driven.x - start.x), 0.2f);
    // the drive went to the rear wheels only
    EXPECT_GT(wheel(c.wheels[2]).rpm, wheel(c.wheels[0]).rpm);

    // rolling (no torque): the wheel turns at the ground's speed
    setMotor(c, 0.0f, true);
    steps(30);
    const float coast = rigidbody(c.body).getLinearVelocity().z;
    const float expectedRpm = coast / spec.radius * 60.0f / (2.0f * VEHICLE_TEST_PI);
    for (Entity w : c.wheels){
        std::printf("[vehicle] coasting at %.2f m/s: rpm %.1f (expected %.1f)\n", coast, wheel(w).rpm, expectedRpm);
        EXPECT_NEAR(wheel(w).rpm, expectedRpm, std::fabs(expectedRpm) * 0.05f);
    }
    WheelGroundHit hit;
    ASSERT_TRUE(WheelCollider(scene.get(), c.wheels[0]).getGroundHit(hit));
    EXPECT_NEAR(hit.normal.y, 1.0f, 1e-3f);
    EXPECT_GT(hit.force, 0.0f);
    EXPECT_GT(hit.forwardDir.z, 0.99f);
    EXPECT_GT(hit.sidewaysDir.x, 0.99f);

    setBrake(c, 3000.0f);
    int stoppedAt = -1;
    for (int i = 0; i < 240 && stoppedAt < 0; i++){
        step();
        if (std::fabs(rigidbody(c.body).getLinearVelocity().z) < 0.05f) stoppedAt = i + 1;
    }
    std::printf("[vehicle] 3000 N m brakes stop it in %d steps\n", stoppedAt);
    EXPECT_GT(stoppedAt, 0);
}

// AC-4 (REQ-4): 20 degrees of steer at 10 m/s turns on about the bicycle model's circle; with the
// sideways stiffness at 0.2 the same input slides wide
TEST_F(VehicleTest, SteerTurnsAndSlides){
    auto turnRadius = [&](float sidewaysStiffness){
        makeScene();
        floor(1000.0f);
        VehicleCarSpec spec;
        spec.wheelbase = 6.0f;
        spec.track = 2.0f;
        const VehicleCar c = car(spec);
        for (Entity w : c.wheels) wheel(w).sidewaysFriction.stiffness = sidewaysStiffness;
        steps(60);
        // 10 m/s along +Z, held by the rear wheels' torque
        auto drive = [&](){
            const Vector3 velocity = rigidbody(c.body).getLinearVelocity();
            const float speed = std::sqrt(velocity.x * velocity.x + velocity.z * velocity.z);
            const float torque = std::clamp((10.0f - speed) * 600.0f, -1500.0f, 1500.0f);
            for (int i = 2; i < 4; i++) wheel(c.wheels[i]).motorTorque = torque;
        };
        steps(360, drive);
        for (int i = 0; i < 2; i++) wheel(c.wheels[i]).steerAngle = Angle::degToDefault(20.0f);
        steps(240, drive);
        // the yaw rate over the next 2 s, at the speed then
        const float yaw0 = yaw(c.body);
        float speedSum = 0.0f;
        float turned = 0.0f, previous = yaw0;
        for (int i = 0; i < 120; i++){
            step(drive);
            const float now = yaw(c.body);
            float delta = now - previous;
            if (delta > VEHICLE_TEST_PI) delta -= 2.0f * VEHICLE_TEST_PI;
            if (delta < -VEHICLE_TEST_PI) delta += 2.0f * VEHICLE_TEST_PI;
            turned += delta;
            previous = now;
            const Vector3 velocity = rigidbody(c.body).getLinearVelocity();
            speedSum += std::sqrt(velocity.x * velocity.x + velocity.z * velocity.z);
        }
        const float yawRate = std::fabs(turned) / 2.0f;
        const float speed = speedSum / 120.0f;
        return yawRate > 1.0e-4f ? speed / yawRate : 1.0e6f;
    };

    const float bicycle = 6.0f / std::tan(20.0f * VEHICLE_TEST_PI / 180.0f);
    const float grip = turnRadius(1.0f);
    const float slide = turnRadius(0.2f);
    std::printf("[vehicle] 20 deg at 10 m/s: radius %.2f m (bicycle model %.2f m), sideways stiffness 0.2: %.2f m\n", grip, bicycle, slide);
    EXPECT_NEAR(grip, bicycle, bicycle * 0.25f);
    EXPECT_GT(slide, 2.0f * grip);
}

// AC-5 (REQ-5): the drivetrain shifts up under full throttle below its max rpm, the hand brake
// locks the wheels, and the wheels read the drive torque it gave them
TEST_F(VehicleTest, DrivetrainShiftsAndCaps){
    floor(2000.0f);
    const VehicleCar c = car();
    scene->addComponent<VehicleDrivetrainComponent>(c.body, {});
    VehicleDrivetrain drivetrain(scene.get(), c.body);
    steps(60);

    drivetrain.setThrottle(1.0f);
    int maxGear = 0;
    float maxRpm = 0.0f;
    float driveTorque = 0.0f;
    for (int i = 0; i < 900; i++){
        step();
        maxGear = std::max(maxGear, drivetrain.getGear());
        maxRpm = std::max(maxRpm, drivetrain.getRpm());
        driveTorque = std::max(driveTorque, wheel(c.wheels[2]).motorTorque);
    }
    std::printf("[vehicle] throttle 1 for 15 s: gear %d, max rpm %.0f, speed %.1f m/s, rear drive torque up to %.0f N m\n",
                maxGear, maxRpm, drivetrain.getSpeed(), driveTorque);
    EXPECT_GE(maxGear, 3);
    // the engine never runs past its max rpm (the rev limiter)
    EXPECT_LE(maxRpm, 6000.0f);
    EXPECT_GT(drivetrain.getSpeed(), 10.0f);
    EXPECT_GT(driveTorque, 0.0f);

    drivetrain.setThrottle(0.0f);
    drivetrain.setHandBrake(1.0f);
    steps(30);
    for (int i = 2; i < 4; i++){
        EXPECT_EQ(wheel(c.wheels[i]).rpm, 0.0f);
        EXPECT_GT(wheel(c.wheels[i]).brakeTorque, 0.0f);
    }
}

// AC-6 (REQ-6, REQ-7): the wheels' GameObjects are at their poses after each step; at rest with no
// input the car sleeps; an input wakes it at the next step
TEST_F(VehicleTest, WheelTransformsFollowAndSleep){
    floor();
    const VehicleCar c = car();
    auto checkPoses = [&](){
        for (Entity w : c.wheels){
            const Transform& transform = scene->getComponent<Transform>(w);
            const WheelColliderComponent& component = wheel(w);
            EXPECT_NEAR(transform.worldPosition.x, component.worldPosePosition.x, 1e-5f);
            EXPECT_NEAR(transform.worldPosition.y, component.worldPosePosition.y, 1e-5f);
            EXPECT_NEAR(transform.worldPosition.z, component.worldPosePosition.z, 1e-5f);
            const Quaternion a = transform.worldRotation;
            const Quaternion b = component.worldPoseRotation;
            EXPECT_NEAR(std::fabs(a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z), 1.0f, 1e-5f);
        }
    };
    int sleptAt = -1;
    for (int i = 0; i < 300 && sleptAt < 0; i++){
        step();
        checkPoses();
        if (rigidbody(c.body).isSleeping()) sleptAt = i + 1;
    }
    std::printf("[vehicle] at rest with no input it sleeps after %d steps\n", sleptAt);
    ASSERT_GT(sleptAt, 0);

    // a spinning wheel rolls the car forward, its GameObject turning about its right (+X) axis
    wheel(c.wheels[2]).motorTorque = 300.0f;
    wheel(c.wheels[3]).motorTorque = 300.0f;
    step();
    EXPECT_FALSE(rigidbody(c.body).isSleeping());
    steps(30);
    checkPoses();
    const Quaternion spin = scene->getComponent<Transform>(c.wheels[2]).worldRotation;
    const Vector3 up = spin * Vector3(0.0f, 1.0f, 0.0f);
    EXPECT_GT(std::fabs(up.z), 0.01f);
    EXPECT_NEAR((spin * Vector3(1.0f, 0.0f, 0.0f)).x, 1.0f, 0.02f);
    EXPECT_GT(rigidbody(c.body).getLinearVelocity().z, 0.1f);

    // steering right turns the front wheels' forward towards +X
    wheel(c.wheels[0]).steerAngle = Angle::degToDefault(30.0f);
    steps(2);
    const Vector3 forward = scene->getComponent<Transform>(c.wheels[0]).worldRotation * Vector3(0.0f, 0.0f, 1.0f);
    EXPECT_GT(forward.x, 0.4f);
}

// REQ-9: moving the body teleports the wheels with it; the rest pose stays the GameObject's
TEST_F(VehicleTest, TeleportMovesTheWheels){
    floor();
    const VehicleCar c = car();
    steps(120);
    const float before = wheel(c.wheels[0]).worldPosePosition.y;
    rigidbody(c.body).setPosition(Vector3(20.0f, 1.0f, 5.0f));
    steps(120);
    const Vector3 after = wheel(c.wheels[0]).worldPosePosition;
    EXPECT_NEAR(after.x, 20.0f - 0.8f, 0.05f);
    EXPECT_NEAR(after.z, 5.0f + 1.3f, 0.05f);
    EXPECT_NEAR(after.y, before, 0.01f);
    EXPECT_TRUE(wheel(c.wheels[0]).isGrounded);
}

// AC-7: two runs of 600 steps with scripted inputs give bit-identical poses
TEST_F(VehicleTest, ReplayIsBitIdentical){
    auto run = [&](){
        makeScene();
        floor();
        const VehicleCar c = car();
        std::vector<float> poses;
        for (int i = 0; i < 600; i++){
            step([&](){
                const float t = (float)i * VEHICLE_DT;
                for (int w = 2; w < 4; w++) wheel(c.wheels[w]).motorTorque = (i >= 60 && i < 300) ? 500.0f : 0.0f;
                for (int w = 0; w < 2; w++) wheel(c.wheels[w]).steerAngle = Angle::degToDefault(15.0f * std::sin(t));
                for (int w = 0; w < 4; w++) wheel(c.wheels[w]).brakeTorque = i >= 450 ? 2000.0f : 0.0f;
            });
            const Vector3 p = rigidbody(c.body).getPosition();
            const Quaternion q = rigidbody(c.body).getRotation();
            poses.insert(poses.end(), {p.x, p.y, p.z, q.w, q.x, q.y, q.z});
        }
        return poses;
    };
    const std::vector<float> first = run();
    const std::vector<float> second = run();
    ASSERT_EQ(first.size(), second.size());
    EXPECT_EQ(std::memcmp(first.data(), second.data(), first.size() * sizeof(float)), 0);
    // it went somewhere
    EXPECT_GT(std::fabs(first[first.size() - 5]), 1.0f);
}
