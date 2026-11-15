// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VehicleSystem.h"

#ifdef JASPERENGINE_PHYSICS_3D

#include "Log.h"
#include "Scene.h"
#include "component/RigidbodyComponent.h"
#include "component/Transform.h"
#include "physics/ContactTracker.h"
#include "physics/WheelColliderController.h"
#include "subsystem/PhysicsSystem.h"
#include "subsystem/RenderSystem.h"
#include "util/Angle.h"
#include "util/JoltPhysicsAux.h"

#include "Jolt/Physics/Body/BodyFilter.h"
#include "Jolt/Physics/Body/BodyLock.h"
#include "Jolt/Physics/PhysicsSystem.h"
#include "Jolt/Physics/Vehicle/VehicleCollisionTester.h"

#include <algorithm>
#include <chrono>
#include <cfloat>
#include <cmath>

using namespace jasperengine;

namespace{

    constexpr float VEHICLE_PI = 3.14159265358979323846f;
    // the steer and pitch / roll limits Jolt accepts (a wheel turned a right angle has no forward)
    constexpr float VEHICLE_MAX_STEER = 0.4999f * VEHICLE_PI;
    // the samples of the drivetrain's torque curve in Jolt's linear curve
    constexpr int VEHICLE_TORQUE_SAMPLES = 17;

    JPH::Vec3 vehicleToJolt(const Vector3& v){
        return JPH::Vec3(v.x, v.y, v.z);
    }

    Vector3 vehicleFromJolt(JPH::Vec3Arg v){
        return Vector3(v.GetX(), v.GetY(), v.GetZ());
    }

    bool vehicleFinite(float value){
        return std::isfinite(value);
    }

    float vehicleSign(float value){
        return value < 0.0f ? -1.0f : 1.0f;
    }

    // Unity's WheelFrictionCurve as Jolt's: (0, 0), the extremum, the asymptote, flat after it;
    // `slipScale` takes Unity's slip to Jolt's x (the slip angle is in degrees there)
    void vehicleFrictionCurve(const WheelFrictionCurve& curve, float slipScale, JPH::LinearCurve& out){
        const float stiffness = std::max(0.0f, curve.stiffness);
        const float extremumSlip = std::max(1.0e-4f, curve.extremumSlip);
        const float asymptoteSlip = std::max(extremumSlip + 1.0e-4f, curve.asymptoteSlip);
        out.Clear();
        out.Reserve(3);
        out.AddPoint(0.0f, 0.0f);
        out.AddPoint(extremumSlip * slipScale, std::max(0.0f, curve.extremumValue) * stiffness);
        out.AddPoint(asymptoteSlip * slipScale, std::max(0.0f, curve.asymptoteValue) * stiffness);
    }

    bool vehicleSameDrivetrain(const VehicleDrivetrainComponent& a, const VehicleDrivetrainComponent& b){
        return a.maxTorque == b.maxTorque && a.minRPM == b.minRPM && a.maxRPM == b.maxRPM && a.torqueCurve == b.torqueCurve &&
               a.engineInertia == b.engineInertia && a.engineDamping == b.engineDamping && a.transmission == b.transmission &&
               a.gearRatios == b.gearRatios && a.reverseGearRatios == b.reverseGearRatios && a.switchTime == b.switchTime &&
               a.clutchReleaseTime == b.clutchReleaseTime && a.switchLatency == b.switchLatency && a.shiftUpRPM == b.shiftUpRPM &&
               a.shiftDownRPM == b.shiftDownRPM && a.clutchStrength == b.clutchStrength && a.differentials == b.differentials &&
               a.differentialLimitedSlipRatio == b.differentialLimitedSlipRatio && a.antiRollBars == b.antiRollBars &&
               a.maxPitchRollAngle == b.maxPitchRollAngle;
    }

    void vehicleClearReadouts(WheelColliderComponent& wheel){
        wheel.vehicleBody = NULL_ENTITY;
        wheel.wheelIndex = -1;
        wheel.isGrounded = false;
        wheel.rpm = 0.0f;
        wheel.sprungMass = 0.0f;
        wheel.suspensionCompression = 0.0f;
        wheel.hit = WheelHit();
    }

}

bool VehicleSystem::WheelBuild::operator==(const WheelBuild& o) const{
    return entity == o.entity && attach == o.attach && forward == o.forward && radius == o.radius && mass == o.mass &&
           wheelDampingRate == o.wheelDampingRate && suspensionDistance == o.suspensionDistance &&
           forceAppPointDistance == o.forceAppPointDistance && suspensionSpring == o.suspensionSpring &&
           forwardFriction == o.forwardFriction && sidewaysFriction == o.sidewaysFriction && maxSteerAngle == o.maxSteerAngle &&
           maxBrakeTorque == o.maxBrakeTorque && maxHandBrakeTorque == o.maxHandBrakeTorque;
}

// The wheels' ground casts skip the vehicle body and its parts (bodies below it: a hitbox, a
// kinematic door), by entity; read on Jolt's job threads, changed only between steps
class VehicleSystem::BodyFilter : public JPH::BodyFilter{
public:
    JPH::BodyID body;
    std::vector<Entity> parts;      // sorted

    bool ShouldCollide(const JPH::BodyID& id) const override{
        return id != body;
    }

    bool ShouldCollideLocked(const JPH::Body& other) const override{
        if (parts.empty()){
            return true;
        }
        const Entity entity = PhysicsCallbacks::bodyEntity(other.GetUserData());
        return !std::binary_search(parts.begin(), parts.end(), entity);
    }
};

VehicleSystem::VehicleSystem(Scene* scene, PhysicsSystem* physics): scene(scene), physics(physics){
}

VehicleSystem::~VehicleSystem(){
    clear();
}

void VehicleSystem::clear(){
    for (auto& entry : vehicles){
        destroy(entry.second);
    }
    vehicles.clear();
}

Entity VehicleSystem::findBody(Entity wheel) const{
    const Transform* transform = scene->findComponent<Transform>(wheel);
    for (int depth = 0; transform && transform->parent != NULL_ENTITY && depth < 256; depth++){
        const Entity parent = transform->parent;
        if (scene->findComponent<RigidbodyComponent>(parent)){
            return parent;
        }
        transform = scene->findComponent<Transform>(parent);
    }
    return NULL_ENTITY;
}

void VehicleSystem::warnOnce(Entity wheel, const char* why){
    if (warned.insert(wheel).second){
        Log::warn("Wheel Collider on '%s' does nothing: %s", scene->getEntityName(wheel).c_str(), why);
    }
}

void VehicleSystem::refreshRestPose(Entity wheelEntity, WheelColliderComponent& wheel, Entity body){
    Transform* transform = scene->findComponent<Transform>(wheelEntity);
    Transform* bodyTransform = scene->findComponent<Transform>(body);
    if (!transform || !bodyTransform){
        return;
    }
    // a Transform the vehicle put at the wheel's pose is not where the wheel hangs from: the
    // pose before it is kept, until something else moves the GameObject
    if (wheel.hasRestPose && wheel.hasWrittenPose &&
            transform->position == wheel.writtenPosition && transform->rotation == wheel.writtenRotation){
        return;
    }
    physics->resolveTransformWorldPose(body);
    physics->resolveTransformWorldPose(wheelEntity);
    const Quaternion toBody = bodyTransform->worldRotation.inverse();
    wheel.restPosition = toBody * (transform->worldPosition - bodyTransform->worldPosition);
    wheel.restRotation = toBody * transform->worldRotation;
    wheel.restScale = transform->worldScale;
    wheel.hasRestPose = true;
    wheel.hasWrittenPose = false;
}

VehicleSystem::WheelBuild VehicleSystem::describe(Entity entity, const WheelColliderComponent& wheel) const{
    WheelBuild build;
    build.entity = entity;
    build.attach = wheel.restPosition + wheel.restRotation * (wheel.center * wheel.restScale);
    // rolls along its GameObject's forward, turned about the body's up
    Vector3 forward = wheel.restRotation * Vector3(0.0f, 0.0f, 1.0f);
    forward.y = 0.0f;
    const float length = forward.length();
    build.forward = (length > 1.0e-4f && vehicleFinite(length)) ? forward / length : Vector3(0.0f, 0.0f, 1.0f);
    build.radius = vehicleFinite(wheel.radius) ? std::max(0.01f, wheel.radius) : 0.5f;
    build.mass = vehicleFinite(wheel.mass) ? std::max(0.01f, wheel.mass) : 20.0f;
    build.wheelDampingRate = vehicleFinite(wheel.wheelDampingRate) ? std::max(0.0f, wheel.wheelDampingRate) : 0.0f;
    build.suspensionDistance = vehicleFinite(wheel.suspensionDistance) ? std::max(0.0f, wheel.suspensionDistance) : 0.0f;
    build.forceAppPointDistance = vehicleFinite(wheel.forceAppPointDistance) ? std::max(0.0f, wheel.forceAppPointDistance) : 0.0f;
    build.suspensionSpring = wheel.suspensionSpring;
    build.suspensionSpring.spring = vehicleFinite(wheel.suspensionSpring.spring) ? std::max(1.0f, wheel.suspensionSpring.spring) : 1.0f;
    build.suspensionSpring.damper = vehicleFinite(wheel.suspensionSpring.damper) ? std::max(0.0f, wheel.suspensionSpring.damper) : 0.0f;
    build.suspensionSpring.targetPosition = vehicleFinite(wheel.suspensionSpring.targetPosition) ? std::clamp(wheel.suspensionSpring.targetPosition, 0.0f, 1.0f) : 0.5f;
    build.forwardFriction = wheel.forwardFriction;
    build.sidewaysFriction = wheel.sidewaysFriction;
    const float maxSteer = vehicleFinite(wheel.maxSteerAngle) ? Angle::defaultToRad(wheel.maxSteerAngle) : 0.0f;
    build.maxSteerAngle = std::clamp(maxSteer, 0.0f, VEHICLE_MAX_STEER);
    build.maxBrakeTorque = vehicleFinite(wheel.maxBrakeTorque) ? std::max(0.0f, wheel.maxBrakeTorque) : 0.0f;
    build.maxHandBrakeTorque = vehicleFinite(wheel.maxHandBrakeTorque) ? std::max(0.0f, wheel.maxHandBrakeTorque) : 0.0f;
    return build;
}

// The least-squares spread of the mass over the wheels (the spread closest to even) that sums to
// the mass and has no moment about the centre of mass across the up axis; a wheel it would give
// a negative share to holds nothing (the rest scaled back to the mass)
std::vector<float> VehicleSystem::sprungMasses(const std::vector<Vector3>& points, const Vector3& centerOfMass, float mass){
    const size_t n = points.size();
    std::vector<float> out(n, 0.0f);
    if (n == 0 || !(mass > 0.0f)){
        return out;
    }
    const float even = mass / (float)n;
    std::fill(out.begin(), out.end(), even);
    if (n == 1){
        return out;
    }

    // rows: 1, x, z (each wheel's), the constraint C m = (mass, 0, 0); m = even + C^T y with
    // (C C^T) y = (0, -even sum x, -even sum z). A row the wheels cannot balance (all on a line
    // along it) is left out.
    std::vector<int> rows = {0, 1, 2};
    auto value = [&](int row, size_t i) -> double {
        const Vector3 p = points[i] - centerOfMass;
        return row == 0 ? 1.0 : (row == 1 ? (double)p.x : (double)p.z);
    };
    for (int attempt = 0; attempt < 4; attempt++){
        const size_t r = rows.size();
        double a[3][4] = {};
        for (size_t i = 0; i < r; i++){
            for (size_t j = 0; j < r; j++){
                double sum = 0.0;
                for (size_t k = 0; k < n; k++) sum += value(rows[i], k) * value(rows[j], k);
                a[i][j] = sum;
            }
            double rhs = 0.0;
            if (rows[i] != 0){
                for (size_t k = 0; k < n; k++) rhs -= (double)even * value(rows[i], k);
            }
            a[i][r] = rhs;
        }
        // Gaussian elimination with partial pivoting
        bool singular = false;
        for (size_t c = 0; c < r && !singular; c++){
            size_t pivot = c;
            for (size_t i = c + 1; i < r; i++){
                if (std::fabs(a[i][c]) > std::fabs(a[pivot][c])) pivot = i;
            }
            if (std::fabs(a[pivot][c]) < 1.0e-9){
                singular = true;
                break;
            }
            if (pivot != c){
                for (size_t j = 0; j <= r; j++) std::swap(a[c][j], a[pivot][j]);
            }
            for (size_t i = 0; i < r; i++){
                if (i == c) continue;
                const double f = a[i][c] / a[c][c];
                for (size_t j = c; j <= r; j++) a[i][j] -= f * a[c][j];
            }
        }
        if (singular){
            // drop the moment row with the least spread (the wheels lie along it)
            if (rows.size() <= 1){
                break;
            }
            double spreadX = 0.0, spreadZ = 0.0;
            for (size_t k = 0; k < n; k++){
                spreadX += value(1, k) * value(1, k);
                spreadZ += value(2, k) * value(2, k);
            }
            const int drop = (std::find(rows.begin(), rows.end(), 1) != rows.end() &&
                              (spreadX <= spreadZ || std::find(rows.begin(), rows.end(), 2) == rows.end())) ? 1 : 2;
            rows.erase(std::find(rows.begin(), rows.end(), drop));
            continue;
        }
        for (size_t k = 0; k < n; k++){
            double m = even;
            for (size_t i = 0; i < r; i++) m += (a[i][r] / a[i][i]) * value(rows[i], k);
            out[k] = (float)m;
        }
        break;
    }

    float sum = 0.0f;
    for (float& m : out){
        m = std::max(0.0f, m);
        sum += m;
    }
    if (sum > 0.0f){
        for (float& m : out) m *= mass / sum;
    }else{
        std::fill(out.begin(), out.end(), even);
    }
    return out;
}

void VehicleSystem::collectParts(Vehicle& vehicle){
    std::vector<Entity>& parts = partsScratch;
    parts.clear();
    auto transforms = scene->getComponentArray<Transform>();
    if (transforms->hasEntity(vehicle.body)){
        const size_t first = transforms->getIndex(vehicle.body);
        const size_t last = scene->findBranchLastIndex(vehicle.body);
        for (size_t i = first + 1; i <= last && i < transforms->size(); i++){
            const Entity entity = transforms->getEntity(i);
            if (scene->findComponent<RigidbodyComponent>(entity)){
                parts.push_back(entity);
            }
        }
    }
    // the trigger colliders' companion body is the vehicle body's own entity
    if (const RigidbodyComponent* body = scene->findComponent<RigidbodyComponent>(vehicle.body); body && !body->triggerBody.IsInvalid()){
        parts.push_back(vehicle.body);
    }
    std::sort(parts.begin(), parts.end());
    if (vehicle.filter && vehicle.filter->parts != parts){
        vehicle.filter->parts = parts;
    }
}

bool VehicleSystem::build(Entity bodyEntity, const std::vector<Entity>& wheelEntities, Vehicle& vehicle){
    RigidbodyComponent& rigidbody = scene->getComponent<RigidbodyComponent>(bodyEntity);
    JPH::PhysicsSystem* world = physics->getWorld3D();

    std::vector<WheelBuild> builds;
    builds.reserve(wheelEntities.size());
    for (Entity entity : wheelEntities){
        builds.push_back(describe(entity, scene->getComponent<WheelColliderComponent>(entity)));
    }
    const VehicleDrivetrainComponent* drivetrain = scene->findComponent<VehicleDrivetrainComponent>(bodyEntity);
    const bool hasDrivetrain = drivetrain && drivetrain->enabled;
    const float gravity = physics->getGravity3D().length();

    JPH::Ref<JPH::VehicleConstraint> constraint;
    JPH::Ref<JPH::VehicleCollisionTesterRay> tester;
    std::vector<float> sprung;
    float inverseMass = 0.0f;
    Vector3 centerOfMass;
    {
        JPH::BodyLockWrite lock(physics->getBodyLockInterface3D(), rigidbody.body);
        if (!lock.Succeeded()){
            return false;
        }
        JPH::Body& body = lock.GetBody();
        if (!body.IsDynamic()){
            return false;
        }
        inverseMass = body.GetMotionProperties()->GetInverseMass();
        centerOfMass = vehicleFromJolt(body.GetShape()->GetCenterOfMass());
        const float mass = inverseMass > 0.0f ? 1.0f / inverseMass : 0.0f;
        std::vector<Vector3> points;
        for (const WheelBuild& b : builds){
            points.push_back(b.attach);
        }
        sprung = sprungMasses(points, centerOfMass, mass);

        JPH::VehicleConstraintSettings settings;
        settings.mUp = JPH::Vec3(0.0f, 1.0f, 0.0f);
        settings.mForward = JPH::Vec3(0.0f, 0.0f, 1.0f);
        settings.mMaxPitchRollAngle = VEHICLE_PI;
        for (size_t i = 0; i < builds.size(); i++){
            const WheelBuild& b = builds[i];
            JPH::Ref<JPH::WheelSettingsWV> wheel = new JPH::WheelSettingsWV();
            wheel->mPosition = vehicleToJolt(b.attach);
            wheel->mSuspensionDirection = JPH::Vec3(0.0f, -1.0f, 0.0f);
            wheel->mSteeringAxis = JPH::Vec3(0.0f, 1.0f, 0.0f);
            wheel->mWheelUp = JPH::Vec3(0.0f, 1.0f, 0.0f);
            wheel->mWheelForward = vehicleToJolt(b.forward);
            // the travel from the attachment (full compression) down to full droop
            wheel->mSuspensionMinLength = 0.0f;
            wheel->mSuspensionMaxLength = b.suspensionDistance;
            // Unity rests the wheel at targetPosition of the travel under its sprung mass: the
            // spring's natural length is where that load compresses it to there. Jolt reads the
            // preload only as the spring's offset (VehicleConstraint.cpp, the suspension part's
            // `c`), which only pushes, so a natural length shorter than the travel (a negative
            // preload) is a spring that starts pushing below full droop.
            const float restCompression = b.suspensionSpring.targetPosition * b.suspensionDistance;
            wheel->mSuspensionPreloadLength = sprung[i] * gravity / b.suspensionSpring.spring - restCompression;
            wheel->mSuspensionSpring.mMode = JPH::ESpringMode::StiffnessAndDamping;
            wheel->mSuspensionSpring.mStiffness = b.suspensionSpring.spring;
            wheel->mSuspensionSpring.mDamping = b.suspensionSpring.damper;
            wheel->mRadius = b.radius;
            if (b.forceAppPointDistance > 0.0f){
                const float restLength = b.suspensionDistance - restCompression;
                wheel->mEnableSuspensionForcePoint = true;
                wheel->mSuspensionForcePoint = vehicleToJolt(b.attach + Vector3(0.0f, -(restLength + b.radius) + b.forceAppPointDistance, 0.0f));
            }
            wheel->mInertia = 0.5f * b.mass * b.radius * b.radius;
            // Unity's damping rate is a torque per spin speed; Jolt's a rate: dw/dt = -c w
            wheel->mAngularDamping = b.wheelDampingRate / wheel->mInertia;
            wheel->mMaxSteerAngle = b.maxSteerAngle;
            vehicleFrictionCurve(b.forwardFriction, 1.0f, wheel->mLongitudinalFriction);
            // Unity's sideways slip is the slip angle in radians, Jolt's curve takes degrees
            vehicleFrictionCurve(b.sidewaysFriction, 180.0f / VEHICLE_PI, wheel->mLateralFriction);
            wheel->mMaxBrakeTorque = b.maxBrakeTorque;
            wheel->mMaxHandBrakeTorque = b.maxHandBrakeTorque;
            settings.mWheels.push_back(wheel.GetPtr());
        }

        JPH::Ref<WheelColliderControllerSettings> controller = new WheelColliderControllerSettings();
        controller->perWheelDrive = !hasDrivetrain;
        controller->bodyAllowsSleep = rigidbody.allowSleeping;
        if (hasDrivetrain){
            const VehicleDrivetrainComponent& d = *drivetrain;
            auto indexOf = [&](Entity wheel) -> int {
                for (size_t i = 0; i < builds.size(); i++){
                    if (builds[i].entity == wheel) return (int)i;
                }
                return -1;
            };
            JPH::VehicleEngineSettings& engine = controller->mEngine;
            engine.mMaxTorque = vehicleFinite(d.maxTorque) ? std::max(0.0f, d.maxTorque) : 0.0f;
            engine.mMinRPM = vehicleFinite(d.minRPM) ? std::max(0.0f, d.minRPM) : 0.0f;
            engine.mMaxRPM = vehicleFinite(d.maxRPM) ? std::max(engine.mMinRPM + 1.0f, d.maxRPM) : engine.mMinRPM + 1.0f;
            engine.mInertia = vehicleFinite(d.engineInertia) ? std::max(0.01f, d.engineInertia) : 0.5f;
            engine.mAngularDamping = vehicleFinite(d.engineDamping) ? std::max(0.0f, d.engineDamping) : 0.0f;
            engine.mNormalizedTorque.Clear();
            engine.mNormalizedTorque.Reserve(VEHICLE_TORQUE_SAMPLES);
            for (int s = 0; s < VEHICLE_TORQUE_SAMPLES; s++){
                const float x = (float)s / (float)(VEHICLE_TORQUE_SAMPLES - 1);
                const float y = d.torqueCurve.empty() ? 1.0f : d.torqueCurve.evaluate(x);
                engine.mNormalizedTorque.AddPoint(x, vehicleFinite(y) ? std::max(0.0f, y) : 0.0f);
            }

            JPH::VehicleTransmissionSettings& transmission = controller->mTransmission;
            transmission.mMode = d.transmission == VehicleTransmissionMode::MANUAL ? JPH::ETransmissionMode::Manual : JPH::ETransmissionMode::Auto;
            transmission.mGearRatios.clear();
            for (float ratio : d.gearRatios){
                if (vehicleFinite(ratio) && ratio > 0.0f) transmission.mGearRatios.push_back(ratio);
            }
            if (transmission.mGearRatios.empty()) transmission.mGearRatios.push_back(1.0f);
            transmission.mReverseGearRatios.clear();
            for (float ratio : d.reverseGearRatios){
                if (vehicleFinite(ratio) && ratio != 0.0f) transmission.mReverseGearRatios.push_back(-std::fabs(ratio));
            }
            if (transmission.mReverseGearRatios.empty()) transmission.mReverseGearRatios.push_back(-1.0f);
            transmission.mSwitchTime = vehicleFinite(d.switchTime) ? std::max(0.0f, d.switchTime) : 0.0f;
            transmission.mClutchReleaseTime = vehicleFinite(d.clutchReleaseTime) ? std::max(0.0f, d.clutchReleaseTime) : 0.0f;
            transmission.mSwitchLatency = vehicleFinite(d.switchLatency) ? std::max(0.0f, d.switchLatency) : 0.0f;
            transmission.mShiftDownRPM = vehicleFinite(d.shiftDownRPM) ? std::max(1.0f, d.shiftDownRPM) : 1.0f;
            transmission.mShiftUpRPM = vehicleFinite(d.shiftUpRPM) ? std::max(transmission.mShiftDownRPM + 1.0f, d.shiftUpRPM) : transmission.mShiftDownRPM + 1.0f;
            transmission.mClutchStrength = vehicleFinite(d.clutchStrength) ? std::max(0.01f, d.clutchStrength) : 10.0f;

            controller->mDifferentials.clear();
            for (const VehicleDifferential& diff : d.differentials){
                JPH::VehicleDifferentialSettings settingsDiff;
                settingsDiff.mLeftWheel = indexOf(diff.leftWheel);
                settingsDiff.mRightWheel = indexOf(diff.rightWheel);
                if (settingsDiff.mLeftWheel < 0 && settingsDiff.mRightWheel < 0) continue;
                settingsDiff.mDifferentialRatio = vehicleFinite(diff.ratio) ? std::max(0.01f, diff.ratio) : 3.42f;
                settingsDiff.mLeftRightSplit = vehicleFinite(diff.leftRightSplit) ? std::clamp(diff.leftRightSplit, 0.0f, 1.0f) : 0.5f;
                settingsDiff.mLimitedSlipRatio = vehicleFinite(diff.limitedSlipRatio) ? std::max(1.0001f, diff.limitedSlipRatio) : FLT_MAX;
                settingsDiff.mEngineTorqueRatio = vehicleFinite(diff.engineTorqueRatio) ? std::max(0.0f, diff.engineTorqueRatio) : 1.0f;
                controller->mDifferentials.push_back(settingsDiff);
            }
            if (controller->mDifferentials.empty()){
                // every axle driven: the wheels side by side (front to back), left (-x) to right
                std::vector<int> order(builds.size());
                for (size_t i = 0; i < order.size(); i++) order[i] = (int)i;
                std::stable_sort(order.begin(), order.end(), [&](int a, int b){ return builds[a].attach.z > builds[b].attach.z; });
                for (size_t i = 0; i < order.size(); i += 2){
                    JPH::VehicleDifferentialSettings axle;
                    int left = order[i];
                    int right = i + 1 < order.size() ? order[i + 1] : -1;
                    if (right >= 0 && builds[right].attach.x < builds[left].attach.x) std::swap(left, right);
                    axle.mLeftWheel = left;
                    axle.mRightWheel = right;
                    controller->mDifferentials.push_back(axle);
                }
                for (JPH::VehicleDifferentialSettings& axle : controller->mDifferentials){
                    axle.mEngineTorqueRatio = 1.0f / (float)controller->mDifferentials.size();
                }
            }else{
                // Jolt needs the shares to sum to one
                float sum = 0.0f;
                for (const JPH::VehicleDifferentialSettings& diff : controller->mDifferentials) sum += diff.mEngineTorqueRatio;
                for (JPH::VehicleDifferentialSettings& diff : controller->mDifferentials){
                    diff.mEngineTorqueRatio = sum > 0.0f ? diff.mEngineTorqueRatio / sum : 1.0f / (float)controller->mDifferentials.size();
                }
            }
            controller->mDifferentialLimitedSlipRatio = vehicleFinite(d.differentialLimitedSlipRatio) ? std::max(1.0001f, d.differentialLimitedSlipRatio) : FLT_MAX;

            for (const VehicleAntiRollBar& bar : d.antiRollBars){
                JPH::VehicleAntiRollBar settingsBar;
                settingsBar.mLeftWheel = indexOf(bar.leftWheel);
                settingsBar.mRightWheel = indexOf(bar.rightWheel);
                if (settingsBar.mLeftWheel < 0 || settingsBar.mRightWheel < 0 || settingsBar.mLeftWheel == settingsBar.mRightWheel) continue;
                settingsBar.mStiffness = vehicleFinite(bar.stiffness) ? std::max(0.0f, bar.stiffness) : 0.0f;
                settings.mAntiRollBars.push_back(settingsBar);
            }
            settings.mMaxPitchRollAngle = vehicleFinite(d.maxPitchRollAngle) ? std::clamp(Angle::defaultToRad(d.maxPitchRollAngle), 0.0f, VEHICLE_PI) : VEHICLE_PI;
        }
        settings.mController = controller.GetPtr();

        constraint = new JPH::VehicleConstraint(body, settings);
        // Unity's WheelCollider: a ray down the suspension from the wheel's top (a cylinder cast
        // costs twice as much, over REQ-41's budget for 20 cars: VehicleBench)
        tester = new JPH::VehicleCollisionTesterRay(body.GetObjectLayer());
    }

    // what the old vehicle's wheels and engine were doing goes on (an edit is not a reset)
    WheelColliderController* controller = static_cast<WheelColliderController*>(constraint->GetController());
    if (vehicle.constraint){
        for (size_t i = 0; i < builds.size(); i++){
            for (size_t j = 0; j < vehicle.wheels.size(); j++){
                if (vehicle.wheels[j].entity == builds[i].entity){
                    const JPH::Wheel* old = vehicle.constraint->GetWheel((JPH::uint)j);
                    JPH::Wheel* wheel = constraint->GetWheel((JPH::uint)i);
                    wheel->SetAngularVelocity(old->GetAngularVelocity());
                    wheel->SetRotationAngle(old->GetRotationAngle());
                    break;
                }
            }
        }
        if (hasDrivetrain && vehicle.hasDrivetrain && vehicle.controller){
            controller->GetEngine().SetCurrentRPM(vehicle.controller->GetEngine().GetCurrentRPM());
            const JPH::VehicleTransmission& old = vehicle.controller->GetTransmission();
            controller->GetTransmission().Set(old.GetCurrentGear(), old.GetClutchFriction());
        }
        destroy(vehicle);
    }

    vehicle.body = bodyEntity;
    vehicle.bodyId = rigidbody.body;
    vehicle.inverseMass = inverseMass;
    vehicle.centerOfMass = centerOfMass;
    vehicle.gravity = gravity;
    vehicle.allowSleeping = rigidbody.allowSleeping;
    vehicle.wheels = std::move(builds);
    vehicle.hasDrivetrain = hasDrivetrain;
    vehicle.drivetrain = hasDrivetrain ? *drivetrain : VehicleDrivetrainComponent();
    vehicle.filter = std::make_unique<BodyFilter>();
    vehicle.filter->body = rigidbody.body;
    tester->SetBodyFilter(vehicle.filter.get());
    constraint->SetVehicleCollisionTester(tester);
    // the tire's grip times the surfaces' (the body's and the ground's, combined as any contact),
    // relative to two default surfaces: Unity's curves are the grip on default ground
    const JPH::VehicleConstraint* self = constraint.GetPtr();
    const float defaultFriction = RigidbodyComponent().friction;
    constraint->SetCombineFriction([self, defaultFriction](JPH::uint, float& longitudinal, float& lateral, const JPH::Body& ground, const JPH::SubShapeID& subShape){
        float groundValue;
        PhysicsMaterialCombine groundMode;
        contactSurface3D(ground, subShape, true, groundValue, groundMode);
        const float combined = combineSurface(self->GetVehicleBody()->GetFriction(), PhysicsMaterialCombine::NONE, groundValue, groundMode,
                                              PhysicsMaterialCombine::GEOMETRIC_MEAN);
        const float factor = defaultFriction > 0.0f ? std::max(0.0f, combined) / defaultFriction : 1.0f;
        longitudinal *= factor;
        lateral *= factor;
    });
    vehicle.constraint = constraint;
    vehicle.tester = tester;
    vehicle.controller = controller;
    vehicle.sprung = std::move(sprung);
    vehicle.pushed.clear();
    collectParts(vehicle);

    world->AddConstraint(constraint);
    world->AddStepListener(constraint);
    physics->getBodyInterface3D().ActivateBody(rigidbody.body);
    return true;
}

void VehicleSystem::destroy(Vehicle& vehicle){
    if (!vehicle.constraint){
        return;
    }
    if (JPH::PhysicsSystem* world = physics->getWorld3D()){
        world->RemoveStepListener(vehicle.constraint);
        world->RemoveConstraint(vehicle.constraint);
    }
    // the vehicle let the body sleep or not by its wheels: the body's own setting again
    {
        JPH::BodyLockWrite lock(physics->getBodyLockInterface3D(), vehicle.bodyId);
        if (lock.Succeeded()){
            lock.GetBody().SetAllowSleeping(vehicle.allowSleeping);
        }
    }
    vehicle.constraint = nullptr;
    vehicle.tester = nullptr;
    vehicle.controller = nullptr;
    vehicle.filter.reset();
}

void VehicleSystem::pushInputs(Vehicle& vehicle){
    inputScratch.clear();
    if (!vehicle.hasDrivetrain){
        for (size_t i = 0; i < vehicle.wheels.size(); i++){
            const WheelColliderComponent& wheel = scene->getComponent<WheelColliderComponent>(vehicle.wheels[i].entity);
            WheelDriveInput input;
            input.motorTorque = vehicleFinite(wheel.motorTorque) ? wheel.motorTorque : 0.0f;
            input.brakeTorque = vehicleFinite(wheel.brakeTorque) ? std::max(0.0f, wheel.brakeTorque) : 0.0f;
            input.steer = vehicleFinite(wheel.steerAngle) ? Angle::defaultToRad(wheel.steerAngle) : 0.0f;
            vehicle.controller->setWheelInput(i, input);
            inputScratch.push_back(input.motorTorque);
            inputScratch.push_back(input.brakeTorque);
            inputScratch.push_back(input.steer);
        }
    }else{
        VehicleDrivetrainComponent& drivetrain = scene->getComponent<VehicleDrivetrainComponent>(vehicle.body);
        auto clampInput = [](float value, float lo, float hi){ return vehicleFinite(value) ? std::clamp(value, lo, hi) : 0.0f; };
        const float throttle = clampInput(drivetrain.throttle, -1.0f, 1.0f);
        const float steer = clampInput(drivetrain.steer, -1.0f, 1.0f);
        const float brake = clampInput(drivetrain.brake, 0.0f, 1.0f);
        const float handBrake = clampInput(drivetrain.handBrake, 0.0f, 1.0f);
        // Jolt's right is the engine's -X: a positive steer (right) is a negative input there
        vehicle.controller->SetDriverInput(throttle, -steer, brake, handBrake);
        if (drivetrain.gearRequested){
            drivetrain.gearRequested = false;
            JPH::VehicleTransmission& transmission = vehicle.controller->GetTransmission();
            const int gear = std::clamp(drivetrain.requestedGear, -(int)transmission.mReverseGearRatios.size(), (int)transmission.mGearRatios.size());
            transmission.Set(gear, 1.0f);
            inputScratch.push_back((float)gear);
        }
        inputScratch.push_back(throttle);
        inputScratch.push_back(steer);
        inputScratch.push_back(brake);
        inputScratch.push_back(handBrake);
    }
    if (inputScratch != vehicle.pushed){
        // a changed input wakes a vehicle at rest (Jolt lets it sleep when its inputs are zero)
        physics->getBodyInterface3D().ActivateBody(vehicle.bodyId);
        vehicle.pushed = inputScratch;
    }
}

void VehicleSystem::preStep(){
    auto wheels = scene->getComponentArray<WheelColliderComponent>();
    if (wheels->size() == 0 && vehicles.empty()){
        return;
    }
    if (!physics->hasWorld3D()){
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    preStepVehicles();
    preSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

void VehicleSystem::preStepVehicles(){
    auto wheels = scene->getComponentArray<WheelColliderComponent>();
    for (auto& group : groupScratch){
        group.second.clear();
    }
    JPH::BodyInterface& bodyInterface = physics->getBodyInterface3D();
    for (size_t i = 0; i < wheels->size(); i++){
        WheelColliderComponent& wheel = wheels->getComponentFromIndex(i);
        const Entity entity = wheels->getEntity(i);
        // a wheel of no vehicle reads as one in the air
        if (!wheel.enabled || !scene->isEntityActiveInHierarchy(entity)){
            vehicleClearReadouts(wheel);
            continue;
        }
        if (scene->findComponent<RigidbodyComponent>(entity)){
            warnOnce(entity, "its GameObject has a Rigidbody; put the Wheel Collider on a child of the vehicle's body");
            vehicleClearReadouts(wheel);
            continue;
        }
        const Entity body = findBody(entity);
        if (body == NULL_ENTITY){
            warnOnce(entity, "no Rigidbody above it; put it on a child of the vehicle's dynamic Rigidbody");
            vehicleClearReadouts(wheel);
            continue;
        }
        const RigidbodyComponent& rigidbody = scene->getComponent<RigidbodyComponent>(body);
        if (rigidbody.type != BodyType::DYNAMIC){
            warnOnce(entity, "the Rigidbody above it is not Dynamic; set its Body Type to Dynamic");
            vehicleClearReadouts(wheel);
            continue;
        }
        if (!warned.empty()){
            warned.erase(entity);
        }
        if (!rigidbody.enabled || rigidbody.body.IsInvalid() || !scene->isEntityActiveInHierarchy(body)){
            vehicleClearReadouts(wheel);
            continue;
        }
        groupScratch[body].push_back(entity);
    }

    // vehicles whose body lost its last wheel, or went
    for (auto it = vehicles.begin(); it != vehicles.end();){
        auto group = groupScratch.find(it->first);
        if (group == groupScratch.end() || group->second.empty()){
            destroy(it->second);
            it = vehicles.erase(it);
        }else{
            ++it;
        }
    }

    for (auto& group : groupScratch){
        if (group.second.empty()){
            continue;
        }
        const Entity bodyEntity = group.first;
        const RigidbodyComponent& rigidbody = scene->getComponent<RigidbodyComponent>(bodyEntity);
        // out of the world (disabled this step, removed by a script): no vehicle until it is back
        if (!bodyInterface.IsAdded(rigidbody.body)){
            for (Entity entity : group.second){
                vehicleClearReadouts(scene->getComponent<WheelColliderComponent>(entity));
            }
            if (auto found = vehicles.find(bodyEntity); found != vehicles.end()){
                destroy(found->second);
                vehicles.erase(found);
            }
            continue;
        }
        Vehicle& vehicle = vehicles[bodyEntity];

        bool rebuild = !vehicle.constraint || vehicle.bodyId != rigidbody.body || vehicle.wheels.size() != group.second.size() ||
                       vehicle.allowSleeping != rigidbody.allowSleeping || vehicle.gravity != physics->getGravity3D().length();
        for (size_t i = 0; i < group.second.size(); i++){
            const Entity entity = group.second[i];
            WheelColliderComponent& wheel = scene->getComponent<WheelColliderComponent>(entity);
            refreshRestPose(entity, wheel, bodyEntity);
            if (!rebuild && vehicle.wheels[i] != describe(entity, wheel)){
                rebuild = true;
            }
        }
        const VehicleDrivetrainComponent* drivetrain = scene->findComponent<VehicleDrivetrainComponent>(bodyEntity);
        const bool hasDrivetrain = drivetrain && drivetrain->enabled;
        if (!rebuild && (hasDrivetrain != vehicle.hasDrivetrain || (hasDrivetrain && !vehicleSameDrivetrain(*drivetrain, vehicle.drivetrain)))){
            rebuild = true;
        }
        if (!rebuild){
            // the body's mass or centre of mass changed (a collider edit swaps its shape, keeping its
            // id). The constraint's body is that live body (same id) and nothing steps now: no lock.
            const JPH::Body* body = vehicle.constraint->GetVehicleBody();
            rebuild = body->GetMotionProperties()->GetInverseMass() != vehicle.inverseMass ||
                      vehicleFromJolt(body->GetShape()->GetCenterOfMass()) != vehicle.centerOfMass;
        }
        if (rebuild && !build(bodyEntity, group.second, vehicle)){
            destroy(vehicle);
            vehicles.erase(bodyEntity);
            continue;
        }

        for (size_t i = 0; i < group.second.size(); i++){
            WheelColliderComponent& wheel = scene->getComponent<WheelColliderComponent>(group.second[i]);
            wheel.vehicleBody = bodyEntity;
            wheel.wheelIndex = (int)i;
            wheel.sprungMass = vehicle.sprung[i];
        }
        // the body's layer and filter bits now (a layer change swaps its ObjectLayer)
        const JPH::ObjectLayer layer = vehicle.constraint->GetVehicleBody()->GetObjectLayer();
        if (vehicle.tester->GetObjectLayer() != layer){
            const_cast<JPH::VehicleCollisionTester*>(vehicle.tester.GetPtr())->SetObjectLayer(layer);
        }
        collectParts(vehicle);
        pushInputs(vehicle);
    }
}

void VehicleSystem::readOutputs(Vehicle& vehicle, float dt){
    // between steps on the main thread nothing else touches the bodies: no locks (the caller
    // checked that the vehicle's body is still the constraint's)
    const JPH::BodyLockInterfaceNoLock& bodies = physics->getWorld3D()->GetBodyLockInterfaceNoLock();
    const JPH::Body* vehicleBody = vehicle.constraint->GetVehicleBody();
    const JPH::Wheels& wheels = vehicle.constraint->GetWheels();
    RenderSystem* render = scene->getSystem<RenderSystem>().get();
    // the body's Transform placed from this step's pose, once for its wheels
    bool bodyResolved = false;

    for (size_t i = 0; i < vehicle.wheels.size() && i < wheels.size(); i++){
        const Entity entity = vehicle.wheels[i].entity;
        WheelColliderComponent* component = scene->findComponent<WheelColliderComponent>(entity);
        if (!component){
            continue;
        }
        WheelColliderComponent& wheel = *component;
        const JPH::WheelWV* jwheel = static_cast<const JPH::WheelWV*>(wheels[i]);
        const JPH::WheelSettings* settings = jwheel->GetSettings();

        wheel.vehicleBody = vehicle.body;
        wheel.wheelIndex = (int)i;
        wheel.sprungMass = vehicle.sprung[i];
        wheel.isGrounded = jwheel->HasContact();
        wheel.rpm = jwheel->GetAngularVelocity() * 60.0f / (2.0f * VEHICLE_PI);
        wheel.suspensionCompression = settings->mSuspensionMaxLength > 0.0f
            ? std::clamp((settings->mSuspensionMaxLength - jwheel->GetSuspensionLength()) / settings->mSuspensionMaxLength, 0.0f, 1.0f) : 0.0f;

        WheelHit hit;
        if (jwheel->HasContact()){
            hit.point = vehicleFromJolt(jwheel->GetContactPosition());
            hit.normal = vehicleFromJolt(jwheel->GetContactNormal());
            hit.forwardDir = vehicleFromJolt(jwheel->GetContactLongitudinal());
            // Jolt's lateral is the wheel's left (its right is forward x up)
            hit.sidewaysDir = -vehicleFromJolt(jwheel->GetContactLateral());
            hit.force = dt > 0.0f ? jwheel->GetSuspensionLambda() / dt : 0.0f;
            const JPH::Vec3 relative = vehicleBody->GetPointVelocity(jwheel->GetContactPosition()) - jwheel->GetContactPointVelocity();
            const float forwardSpeed = relative.Dot(jwheel->GetContactLongitudinal());
            hit.forwardSlip = vehicleSign(jwheel->GetAngularVelocity() * settings->mRadius - forwardSpeed) * jwheel->mLongitudinalSlip;
            hit.sidewaysSlip = vehicleSign(relative.Dot(vehicleToJolt(hit.sidewaysDir))) * jwheel->mLateralSlip;
            if (const JPH::Body* ground = bodies.TryGetBody(jwheel->GetContactBodyID())){
                hit.colliderEntity = PhysicsCallbacks::bodyEntity(ground->GetUserData());
                hit.colliderIndex = (int)ground->GetShape()->GetSubShapeUserData(jwheel->GetContactSubShapeID());
            }
        }
        wheel.hit = hit;

        // the drawn wheel: suspension, steer and spin (a cylinder about the body's right)
        const JPH::RMat44 pose = vehicle.constraint->GetWheelWorldTransform((JPH::uint)i, JPH::Vec3(-1.0f, 0.0f, 0.0f), JPH::Vec3(0.0f, 1.0f, 0.0f));
        const JPH::Quat rotation = pose.GetQuaternion().Normalized();
        wheel.worldPosePosition = vehicleFromJolt(pose.GetTranslation());
        wheel.worldPoseRotation = Quaternion(rotation.GetW(), rotation.GetX(), rotation.GetY(), rotation.GetZ());

        if (vehicle.hasDrivetrain){
            const VehicleDrivetrainComponent* drivetrain = scene->findComponent<VehicleDrivetrainComponent>(vehicle.body);
            const JPH::WheelSettingsWV* wv = jwheel->GetSettings();
            wheel.motorTorque = vehicle.controller->getDriveTorque(i);
            wheel.brakeTorque = drivetrain ? std::clamp(drivetrain->brake, 0.0f, 1.0f) * wv->mMaxBrakeTorque +
                                             std::clamp(drivetrain->handBrake, 0.0f, 1.0f) * wv->mMaxHandBrakeTorque : 0.0f;
            wheel.steerAngle = Angle::radToDefault(jwheel->GetSteerAngle());
        }

        Transform* transform = scene->findComponent<Transform>(entity);
        if (wheel.driveTransform && transform){
            // the chain from the body down to the wheel's parent placed from this step's poses
            if (!bodyResolved){
                render->updateTransform(scene->getComponent<Transform>(vehicle.body), vehicle.body);
                bodyResolved = true;
            }
            std::vector<Entity>& chain = chainScratch;
            chain.clear();
            for (Entity parent = transform->parent; parent != NULL_ENTITY && parent != vehicle.body && chain.size() < 256;){
                chain.push_back(parent);
                const Transform* parentTransform = scene->findComponent<Transform>(parent);
                parent = parentTransform ? parentTransform->parent : NULL_ENTITY;
            }
            for (size_t c = chain.size(); c-- > 0;){
                render->updateTransform(scene->getComponent<Transform>(chain[c]), chain[c]);
            }
            physics->writeTransformWorldPose(entity, wheel.worldPosePosition, wheel.worldPoseRotation);
            wheel.writtenPosition = transform->position;
            wheel.writtenRotation = transform->rotation;
            wheel.hasWrittenPose = true;
        }
    }

    if (vehicle.hasDrivetrain){
        if (VehicleDrivetrainComponent* drivetrain = scene->findComponent<VehicleDrivetrainComponent>(vehicle.body)){
            drivetrain->rpm = vehicle.controller->GetEngine().GetCurrentRPM();
            drivetrain->gear = vehicle.controller->GetTransmission().GetCurrentGear();
            drivetrain->speed = vehicleBody->GetLinearVelocity().Dot(vehicleBody->GetRotation() * JPH::Vec3(0.0f, 0.0f, 1.0f));
        }
    }
}

void VehicleSystem::postStep(float dt){
    if (vehicles.empty()){
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    const JPH::BodyLockInterfaceNoLock& bodies = physics->getWorld3D()->GetBodyLockInterfaceNoLock();
    for (auto& entry : vehicles){
        // a body destroyed by a callback of the step is gone already (its teardown runs right
        // after it): the vehicle goes at the next preStep, its stale body never read
        if (entry.second.constraint && bodies.TryGetBody(entry.second.bodyId) == entry.second.constraint->GetVehicleBody()){
            readOutputs(entry.second, dt);
        }
    }
    postSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

void VehicleSystem::debugLines(const std::function<void(const Vector3&, const Vector3&)>& line) const{
    constexpr int segments = 16;
    if (vehicles.empty()){
        return;
    }
    const JPH::BodyLockInterfaceNoLock& bodies = physics->getWorld3D()->GetBodyLockInterfaceNoLock();
    for (const auto& entry : vehicles){
        const Vehicle& vehicle = entry.second;
        if (!vehicle.constraint || bodies.TryGetBody(vehicle.bodyId) != vehicle.constraint->GetVehicleBody()){
            continue;
        }
        const JPH::RMat44 body = vehicle.constraint->GetVehicleBody()->GetWorldTransform();
        for (size_t i = 0; i < vehicle.wheels.size(); i++){
            const JPH::WheelSettings* settings = vehicle.constraint->GetWheel((JPH::uint)i)->GetSettings();
            const JPH::RMat44 pose = vehicle.constraint->GetWheelWorldTransform((JPH::uint)i, JPH::Vec3(-1.0f, 0.0f, 0.0f), JPH::Vec3(0.0f, 1.0f, 0.0f));
            const JPH::Vec3 center = pose.GetTranslation();
            const JPH::Vec3 up = pose.GetAxisY();
            const JPH::Vec3 forward = pose.GetAxisZ();
            // the wheel's circle in its plane
            for (int s = 0; s < segments; s++){
                const float a0 = 2.0f * VEHICLE_PI * (float)s / (float)segments;
                const float a1 = 2.0f * VEHICLE_PI * (float)(s + 1) / (float)segments;
                const JPH::Vec3 p0 = center + settings->mRadius * (std::cos(a0) * up + std::sin(a0) * forward);
                const JPH::Vec3 p1 = center + settings->mRadius * (std::cos(a1) * up + std::sin(a1) * forward);
                line(vehicleFromJolt(p0), vehicleFromJolt(p1));
            }
            // the travel, from full compression to full droop
            const JPH::Vec3 top = body * settings->mPosition;
            const JPH::Vec3 bottom = body * (settings->mPosition + settings->mSuspensionDirection * settings->mSuspensionMaxLength);
            line(vehicleFromJolt(top), vehicleFromJolt(bottom));
        }
    }
}

#endif
