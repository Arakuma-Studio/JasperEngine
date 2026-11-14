// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef VEHICLE_SYSTEM_H
#define VEHICLE_SYSTEM_H

#include "Export.h"
#include "ecs/Entity.h"
#include "math/Vector3.h"
#include "math/Quaternion.h"
#include "component/WheelColliderComponent.h"
#include "component/VehicleDrivetrainComponent.h"

#ifdef JASPERENGINE_PHYSICS_3D
#include "Jolt/Jolt.h"
#include "Jolt/Physics/Body/BodyID.h"
#include "Jolt/Physics/Vehicle/VehicleConstraint.h"
#endif

#include <functional>
#include <map>
#include <memory>
#include <unordered_set>
#include <vector>

namespace jasperengine{

    class Scene;
    class PhysicsSystem;

#ifdef JASPERENGINE_PHYSICS_3D

    class WheelColliderController;

    // The vehicles of a scene's 3D world (docs/specs/S96-physics-gaps.md REQ-1..REQ-9), owned by
    // its PhysicsSystem: the enabled WheelColliders below a dynamic Rigidbody (the nearest one
    // above them) make one Jolt VehicleConstraint on it, with a VehicleDrivetrain on the body
    // driving them when it has one. Built again at the first step after a wheel, the drivetrain
    // or the body changes, and gone with the last wheel. Before each world step the inputs go in;
    // after it the read-outs come out and the wheels' Transforms follow their poses.
    class JASPERENGINE_API VehicleSystem{
    public:
        // what a wheel was built with: a difference builds the vehicle again
        struct WheelBuild{
            Entity entity = NULL_ENTITY;
            Vector3 attach;             // body space
            Vector3 forward;            // body space, perpendicular to up
            float radius = 0.0f;
            float mass = 0.0f;
            float wheelDampingRate = 0.0f;
            float suspensionDistance = 0.0f;
            float forceAppPointDistance = 0.0f;
            JointSpring3D suspensionSpring;
            WheelFrictionCurve forwardFriction;
            WheelFrictionCurve sidewaysFriction;
            float maxSteerAngle = 0.0f;
            float maxBrakeTorque = 0.0f;
            float maxHandBrakeTorque = 0.0f;

            bool operator==(const WheelBuild& o) const;
            bool operator!=(const WheelBuild& o) const { return !(*this == o); }
        };

    private:
        class BodyFilter;

        struct Vehicle{
            Entity body = NULL_ENTITY;
            JPH::BodyID bodyId;
            float inverseMass = 0.0f;
            Vector3 centerOfMass;
            float gravity = 0.0f;
            bool allowSleeping = true;
            std::vector<WheelBuild> wheels;
            bool hasDrivetrain = false;
            VehicleDrivetrainComponent drivetrain;
            JPH::Ref<JPH::VehicleConstraint> constraint;
            JPH::RefConst<JPH::VehicleCollisionTester> tester;
            std::unique_ptr<BodyFilter> filter;
            WheelColliderController* controller = nullptr;
            // the body mass each wheel holds up
            std::vector<float> sprung;
            // the inputs pushed last (a change wakes the body)
            std::vector<float> pushed;
        };

        Scene* scene;
        PhysicsSystem* physics;
        // by body entity: one order for the steps of every run (a replay is bit-identical)
        std::map<Entity, Vehicle> vehicles;
        std::map<Entity, std::vector<Entity>> groupScratch;
        std::vector<float> inputScratch;
        std::vector<Entity> partsScratch;
        std::vector<Entity> chainScratch;
        // wheels already told why they do nothing (logged once each)
        std::unordered_set<Entity> warned;
        // seconds spent in preStep and postStep, summed (the performance tests)
        double preSeconds = 0.0;
        double postSeconds = 0.0;

        // the nearest ancestor with a Rigidbody (NULL_ENTITY: none)
        Entity findBody(Entity wheel) const;
        void warnOnce(Entity wheel, const char* why);
        // the wheel's rest pose in the body's space, from its Transform unless the vehicle drives it
        void refreshRestPose(Entity wheelEntity, WheelColliderComponent& wheel, Entity body);
        WheelBuild describe(Entity entity, const WheelColliderComponent& wheel) const;
        bool build(Entity bodyEntity, const std::vector<Entity>& wheels, Vehicle& vehicle);
        void destroy(Vehicle& vehicle);
        void pushInputs(Vehicle& vehicle);
        void readOutputs(Vehicle& vehicle, float dt);
        // the bodies below the vehicle body (its parts), never hit by the wheels
        void collectParts(Vehicle& vehicle);
        void preStepVehicles();

    public:
        VehicleSystem(Scene* scene, PhysicsSystem* physics);
        ~VehicleSystem();

        VehicleSystem(const VehicleSystem&) = delete;
        VehicleSystem& operator=(const VehicleSystem&) = delete;

        // Before each world step: vehicles made, rebuilt and removed as wheels, drivetrains and
        // bodies come, change and go; the inputs pushed (a change wakes the body)
        void preStep();
        // After each world step: the read-outs and the wheels' Transforms (dt: the fixed step)
        void postStep(float dt);
        size_t count() const { return vehicles.size(); }
        // seconds summed over the passes since the last reset (the Jolt step's own share is not in them)
        double getPreStepSeconds() const { return preSeconds; }
        double getPostStepSeconds() const { return postSeconds; }
        void resetTimings() { preSeconds = 0.0; postSeconds = 0.0; }
        // every vehicle goes (the world is torn down)
        void clear();

        // the wheel lines Physics.debugDraw shows: its circle and its suspension travel
        void debugLines(const std::function<void(const Vector3&, const Vector3&)>& line) const;

        // the body masses each wheel holds up for wheels at these body-space points (x, z
        // across the body's up) around the centre of mass: they sum to the mass and balance it
        static std::vector<float> sprungMasses(const std::vector<Vector3>& points, const Vector3& centerOfMass, float mass);
    };

#endif

}

#endif //VEHICLE_SYSTEM_H
