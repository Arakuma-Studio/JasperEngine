// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "WorldSettings.h"

#include "util/Angle.h"

#ifdef JASPERENGINE_PHYSICS_2D
#include "box2d/box2d.h"
#endif
#ifdef JASPERENGINE_PHYSICS_3D
#include "Jolt/Jolt.h"
#include "Jolt/Physics/PhysicsSettings.h"
#endif

#include <algorithm>
#include <cmath>

using namespace jasperengine;

float ScenePhysics2DSettings::defaultMaxRotation(){
    return Angle::degToDefault(45.0f);
}

namespace {

    std::vector<PhysicsWorldSettingField> makePhysicsWorldSettingFields(){
        const float degree = Angle::degToDefault(1.0f);
        return {
            {"physics2DPointsPerMeter", "physics_2d_points_per_meter", "Points Per Meter",
             "Canvas points in a metre of the 2D world (default 64): sizes, positions and speeds of 2D bodies are in points, "
             "masses and forces in metres. A change with bodies made rebuilds them where they are (contacts start again).",
             false, false, PhysicsWorldSettingField::Type::Float, 0.01f, 100000.0f, offsetof(ScenePhysics2DSettings, pointsPerMeter)},
            {"physics2DMaxRotation", "physics_2d_max_rotation", "Max Rotation Per Step",
             "A 2D body turns at most this much in a fixed step (default 45 degrees, Box2D's; Unity's Max Rotation Speed is 360). "
             "A Rigidbody 2D with Allow Fast Rotation is not capped (wheels).",
             false, false, PhysicsWorldSettingField::Type::Float, 0.1f * degree, 3600.0f * degree, offsetof(ScenePhysics2DSettings, maxRotation)},
            {"physics2DContactHertz", "physics_2d_contact_hertz", "Contact Hertz",
             "Contact stiffness in Hz (default 30); capped at a quarter of the sub-step rate.",
             false, true, PhysicsWorldSettingField::Type::Float, 0.0f, 10000.0f, offsetof(ScenePhysics2DSettings, contactHertz)},
            {"physics2DContactDampingRatio", "physics_2d_contact_damping_ratio", "Contact Damping Ratio",
             "Contact bounciness, 1 critical damping (default 10).",
             false, true, PhysicsWorldSettingField::Type::Float, 0.0f, 1000.0f, offsetof(ScenePhysics2DSettings, contactDampingRatio)},
            {"physics2DContactPushSpeed", "physics_2d_contact_push_speed", "Contact Push Speed",
             "Metres a second overlapping bodies are pushed apart at, at most (default 3).",
             false, true, PhysicsWorldSettingField::Type::Float, 0.0f, 1000.0f, offsetof(ScenePhysics2DSettings, contactPushSpeed)},
            {"physics2DJointHertz", "physics_2d_joint_hertz", "Joint Hertz",
             "Joint stiffness in Hz (default 60); capped at an eighth of the sub-step rate.",
             false, true, PhysicsWorldSettingField::Type::Float, 0.0f, 10000.0f, offsetof(ScenePhysics2DSettings, jointHertz)},
            {"physics2DJointDampingRatio", "physics_2d_joint_damping_ratio", "Joint Damping Ratio",
             "Joint bounciness, 1 critical damping (default 2).",
             false, true, PhysicsWorldSettingField::Type::Float, 0.0f, 1000.0f, offsetof(ScenePhysics2DSettings, jointDampingRatio)},
            {"physics2DBounceThreshold", "physics_2d_bounce_threshold", "Bounce Threshold",
             "Collisions slower than this (m/s) do not bounce (default 1; Unity's Velocity Threshold).",
             false, true, PhysicsWorldSettingField::Type::Float, 0.0f, 1000.0f, offsetof(ScenePhysics2DSettings, bounceThreshold)},
            {"physics2DMaxLinearSpeed", "physics_2d_max_linear_speed", "Max Linear Speed",
             "A 2D body's speed is capped at this many metres a second (default 400; Unity's Max Translation Speed).",
             false, true, PhysicsWorldSettingField::Type::Float, 0.01f, 100000.0f, offsetof(ScenePhysics2DSettings, maxLinearSpeed)},
            {"physics2DWarmStarting", "physics_2d_warm_starting", "Warm Starting",
             "Start each step's solver from the last step's impulses (default on; off makes stacks and joints wobble).",
             false, true, PhysicsWorldSettingField::Type::Bool, 0.0f, 1.0f, offsetof(ScenePhysics2DSettings, warmStarting)},
            {"physics2DContinuous", "physics_2d_continuous", "Continuous",
             "Fast dynamic bodies do not pass through static ones (default on).",
             false, true, PhysicsWorldSettingField::Type::Bool, 0.0f, 1.0f, offsetof(ScenePhysics2DSettings, continuous)},
            {"physics2DSleep", "physics_2d_sleep", "Sleeping",
             "Bodies at rest sleep until touched (default on).",
             false, true, PhysicsWorldSettingField::Type::Bool, 0.0f, 1.0f, offsetof(ScenePhysics2DSettings, sleep)},
            {"physics3DVelocityIterations", "physics_3d_velocity_iterations", "Velocity Iterations",
             "Solver velocity iterations of each step (default 10; Unity's Default Solver Velocity Iterations). "
             "Friction needs 2 or more.",
             true, true, PhysicsWorldSettingField::Type::Int, 1.0f, 64.0f, offsetof(ScenePhysics3DSettings, velocityIterations)},
            {"physics3DPositionIterations", "physics_3d_position_iterations", "Position Iterations",
             "Solver position iterations of each step (default 2; Unity's Default Solver Iterations).",
             true, true, PhysicsWorldSettingField::Type::Int, 0.0f, 64.0f, offsetof(ScenePhysics3DSettings, positionIterations)},
            {"physics3DBounceThreshold", "physics_3d_bounce_threshold", "Bounce Threshold",
             "Collisions slower than this (m/s) do not bounce (default 1).",
             true, true, PhysicsWorldSettingField::Type::Float, 0.0f, 1000.0f, offsetof(ScenePhysics3DSettings, bounceThreshold)},
            {"physics3DSleepThreshold", "physics_3d_sleep_threshold", "Sleep Threshold",
             "A body whose points move slower than this (m/s) for half a second sleeps (default 0.03).",
             true, true, PhysicsWorldSettingField::Type::Float, 0.0f, 100.0f, offsetof(ScenePhysics3DSettings, sleepThreshold)},
            {"physics3DSpeculativeDistance", "physics_3d_speculative_distance", "Speculative Distance",
             "Metres around a body where contacts are found before it touches (default 0.02; Unity's Default Contact Offset).",
             true, true, PhysicsWorldSettingField::Type::Float, 0.0f, 10.0f, offsetof(ScenePhysics3DSettings, speculativeDistance)},
            {"physics3DCollisionSteps", "physics_3d_collision_steps", "Collision Steps",
             "Collision passes in each fixed step, 1-4 (default 1): more keep fast bodies from tunnelling, at a cost.",
             true, true, PhysicsWorldSettingField::Type::Int, 1.0f, 4.0f, offsetof(ScenePhysics3DSettings, collisionSteps)},
        };
    }

    void* physicsWorldSettingRef(const PhysicsWorldSettingField& field, ScenePhysics2DSettings& settings2D, ScenePhysics3DSettings& settings3D){
        unsigned char* base = field.is3D ? reinterpret_cast<unsigned char*>(&settings3D) : reinterpret_cast<unsigned char*>(&settings2D);
        return base + field.offset;
    }

    const void* physicsWorldSettingRef(const PhysicsWorldSettingField& field, const ScenePhysics2DSettings& settings2D,
                                       const ScenePhysics3DSettings& settings3D){
        const unsigned char* base = field.is3D ? reinterpret_cast<const unsigned char*>(&settings3D)
                                               : reinterpret_cast<const unsigned char*>(&settings2D);
        return base + field.offset;
    }

    // the fields of one struct in their ranges, a non-finite one at its default
    template<typename S2, typename S3>
    void sanitizePhysicsWorldFields(bool is3D, S2& settings2D, S3& settings3D){
        S2 defaults2D;
        S3 defaults3D;
        for (const PhysicsWorldSettingField& field : physicsWorldSettingFields()){
            if (field.is3D != is3D || field.type == PhysicsWorldSettingField::Type::Bool){
                continue;
            }
            double value = getPhysicsWorldSetting(field, settings2D, settings3D);
            if (!std::isfinite(value)){
                value = getPhysicsWorldSetting(field, defaults2D, defaults3D);
            }
            setPhysicsWorldSetting(field, settings2D, settings3D, std::clamp(value, (double)field.minValue, (double)field.maxValue));
        }
    }

#ifdef JASPERENGINE_PHYSICS_2D
    // Box2D's own value at the default (the same float B2_MAX_ROTATION is), radians otherwise
    float maxRotationRadians(const ScenePhysics2DSettings& settings){
        if (settings.maxRotation == ScenePhysics2DSettings().maxRotation){
            return b2DefaultWorldDef().maximumRotation;
        }
        return Angle::defaultToRad(settings.maxRotation);
    }
#endif

}

const std::vector<PhysicsWorldSettingField>& jasperengine::physicsWorldSettingFields(){
    static const std::vector<PhysicsWorldSettingField> fields = makePhysicsWorldSettingFields();
    return fields;
}

const PhysicsWorldSettingField* jasperengine::findPhysicsWorldSettingField(std::string_view snakeName){
    for (const PhysicsWorldSettingField& field : physicsWorldSettingFields()){
        if (snakeName == field.snakeName){
            return &field;
        }
    }
    return nullptr;
}

double jasperengine::getPhysicsWorldSetting(const PhysicsWorldSettingField& field, const ScenePhysics2DSettings& settings2D,
                                            const ScenePhysics3DSettings& settings3D){
    const void* ref = physicsWorldSettingRef(field, settings2D, settings3D);
    switch (field.type){
        case PhysicsWorldSettingField::Type::Float: return *static_cast<const float*>(ref);
        case PhysicsWorldSettingField::Type::Int: return *static_cast<const int*>(ref);
        case PhysicsWorldSettingField::Type::Bool: return *static_cast<const bool*>(ref) ? 1.0 : 0.0;
    }
    return 0.0;
}

void jasperengine::setPhysicsWorldSetting(const PhysicsWorldSettingField& field, ScenePhysics2DSettings& settings2D,
                                          ScenePhysics3DSettings& settings3D, double value){
    void* ref = physicsWorldSettingRef(field, settings2D, settings3D);
    switch (field.type){
        case PhysicsWorldSettingField::Type::Float: *static_cast<float*>(ref) = static_cast<float>(value); break;
        case PhysicsWorldSettingField::Type::Int: *static_cast<int*>(ref) = static_cast<int>(std::lround(value)); break;
        case PhysicsWorldSettingField::Type::Bool: *static_cast<bool*>(ref) = value != 0.0; break;
    }
}

ScenePhysics2DSettings jasperengine::sanitizePhysicsWorldSettings(const ScenePhysics2DSettings& settings){
    ScenePhysics2DSettings clean = settings;
    ScenePhysics3DSettings unused;
    sanitizePhysicsWorldFields(false, clean, unused);
    return clean;
}

ScenePhysics3DSettings jasperengine::sanitizePhysicsWorldSettings(const ScenePhysics3DSettings& settings){
    ScenePhysics2DSettings unused;
    ScenePhysics3DSettings clean = settings;
    sanitizePhysicsWorldFields(true, unused, clean);
    return clean;
}

#ifdef JASPERENGINE_PHYSICS_2D
void jasperengine::applyPhysicsWorldSettings(const ScenePhysics2DSettings& settings, b2WorldDef& def){
    def.maximumRotation = maxRotationRadians(settings);
    def.contactHertz = settings.contactHertz;
    def.contactDampingRatio = settings.contactDampingRatio;
    def.contactPushSpeed = settings.contactPushSpeed;
    def.jointHertz = settings.jointHertz;
    def.jointDampingRatio = settings.jointDampingRatio;
    def.restitutionThreshold = settings.bounceThreshold;
    def.maximumLinearSpeed = settings.maxLinearSpeed;
    def.enableContinuous = settings.continuous;
    def.enableSleep = settings.sleep;
    // warm starting has no def field: a world starts with it on
}

void jasperengine::applyPhysicsWorldSettings(const ScenePhysics2DSettings& settings, const b2WorldId& world){
    b2World_SetMaximumRotation(world, maxRotationRadians(settings));
    b2World_SetContactTuning(world, settings.contactHertz, settings.contactDampingRatio, settings.contactPushSpeed);
    b2World_SetJointTuning(world, settings.jointHertz, settings.jointDampingRatio);
    b2World_SetRestitutionThreshold(world, settings.bounceThreshold);
    b2World_SetMaximumLinearSpeed(world, settings.maxLinearSpeed);
    if (b2World_IsWarmStartingEnabled(world) != settings.warmStarting){
        b2World_EnableWarmStarting(world, settings.warmStarting);
    }
    if (b2World_IsContinuousEnabled(world) != settings.continuous){
        b2World_EnableContinuous(world, settings.continuous);
    }
    // turning sleep off wakes every body: only on a change
    if (b2World_IsSleepingEnabled(world) != settings.sleep){
        b2World_EnableSleeping(world, settings.sleep);
    }
}
#endif

#ifdef JASPERENGINE_PHYSICS_3D
void jasperengine::applyPhysicsWorldSettings(const ScenePhysics3DSettings& settings, JPH::PhysicsSettings& joltSettings){
    joltSettings.mNumVelocitySteps = static_cast<JPH::uint>(settings.velocityIterations);
    joltSettings.mNumPositionSteps = static_cast<JPH::uint>(settings.positionIterations);
    joltSettings.mMinVelocityForRestitution = settings.bounceThreshold;
    joltSettings.mPointVelocitySleepThreshold = settings.sleepThreshold;
    joltSettings.mSpeculativeContactDistance = settings.speculativeDistance;
}
#endif
