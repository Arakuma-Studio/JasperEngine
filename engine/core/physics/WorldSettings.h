// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The physics worlds' scene settings (docs/specs/S96-physics-gaps.md D10, REQ-30..REQ-35): the 2D
// world's units, rotation cap and solver tuning, and the 3D solver. Their defaults are what the
// worlds did before they were settings (Box2D's and Jolt's own), so an old scene steps as it did.
// PhysicsSystem owns the live values and applies them to its worlds before their next step.

#include "Export.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

struct b2WorldDef;
struct b2WorldId;
namespace JPH { struct PhysicsSettings; }

namespace jasperengine {

    struct JASPERENGINE_API ScenePhysics2DSettings {
        // canvas points in a metre: 2D positions and sizes are points, Box2D works in metres
        float pointsPerMeter = 64.0f;
        // a body's rotation per fixed step at most, in the engine's angle unit (Box2D's 0.25 pi;
        // Unity's Physics 2D Max Rotation Speed is 360 degrees); allowFastRotation bodies are free
        float maxRotation = defaultMaxRotation();
        // contact stiffness (Hz), its damping ratio and the speed overlaps are pushed apart at (m/s)
        float contactHertz = 30.0f;
        float contactDampingRatio = 10.0f;
        float contactPushSpeed = 3.0f;
        // joint stiffness (Hz) and damping ratio
        float jointHertz = 60.0f;
        float jointDampingRatio = 2.0f;
        // collisions slower than this (m/s) do not bounce (Unity's Velocity Threshold)
        float bounceThreshold = 1.0f;
        // m/s
        float maxLinearSpeed = 400.0f;
        bool warmStarting = true;
        bool continuous = true;
        bool sleep = true;

        bool operator==(const ScenePhysics2DSettings&) const = default;

        static float defaultMaxRotation();
    };

    struct JASPERENGINE_API ScenePhysics3DSettings {
        // Jolt's solver iterations of each step
        int velocityIterations = 10;
        int positionIterations = 2;
        // collisions slower than this (m/s) do not bounce (mMinVelocityForRestitution)
        float bounceThreshold = 1.0f;
        // a body whose points move slower than this (m/s) may sleep (mPointVelocitySleepThreshold)
        float sleepThreshold = 0.03f;
        // metres around a body speculative contacts are found in (mSpeculativeContactDistance)
        float speculativeDistance = 0.02f;
        // Jolt's collision steps in each fixed step, 1..4
        int collisionSteps = 1;

        bool operator==(const ScenePhysics3DSettings&) const = default;
    };

    // One world setting as the scene file, Lua, MCP and the Inspector name it: the scene's
    // Stream, cooked record, Catalog, MCP and Inspector code walk this list
    struct JASPERENGINE_API PhysicsWorldSettingField {
        enum class Type : uint8_t { Float, Int, Bool };
        const char* key;         // the scene file key and the Lua scene property ("physics2DContactHertz")
        const char* snakeName;   // the MCP scene setting ("physics_2d_contact_hertz")
        const char* label;       // the Inspector's
        const char* help;        // the Inspector's tooltip, get_scene_settings detail's help
        bool is3D;
        // in the Inspector's World foldout (the others are rows of their own)
        bool advanced;
        Type type;
        float minValue;
        float maxValue;
        size_t offset;           // in ScenePhysics2DSettings, or ScenePhysics3DSettings when is3D
    };

    JASPERENGINE_API const std::vector<PhysicsWorldSettingField>& physicsWorldSettingFields();
    JASPERENGINE_API const PhysicsWorldSettingField* findPhysicsWorldSettingField(std::string_view snakeName);
    // a field's value (a bool or an int as 0/1, n) and its setting, unclamped
    JASPERENGINE_API double getPhysicsWorldSetting(const PhysicsWorldSettingField& field, const ScenePhysics2DSettings& settings2D,
                                                    const ScenePhysics3DSettings& settings3D);
    JASPERENGINE_API void setPhysicsWorldSetting(const PhysicsWorldSettingField& field, ScenePhysics2DSettings& settings2D,
                                                  ScenePhysics3DSettings& settings3D, double value);

    // Every field in its range (fields' minValue..maxValue); a non-finite one at its default
    JASPERENGINE_API ScenePhysics2DSettings sanitizePhysicsWorldSettings(const ScenePhysics2DSettings& settings);
    JASPERENGINE_API ScenePhysics3DSettings sanitizePhysicsWorldSettings(const ScenePhysics3DSettings& settings);

#ifdef JASPERENGINE_PHYSICS_2D
    // into the definition a 2D world is made from (pointsPerMeter is the engine's, not Box2D's)
    JASPERENGINE_API void applyPhysicsWorldSettings(const ScenePhysics2DSettings& settings, b2WorldDef& def);
    // to a live world, not while it steps (Box2D ignores changes then)
    JASPERENGINE_API void applyPhysicsWorldSettings(const ScenePhysics2DSettings& settings, const b2WorldId& world);
#endif
#ifdef JASPERENGINE_PHYSICS_3D
    // into Jolt's settings (the world takes them with SetPhysicsSettings); collisionSteps goes to Update
    JASPERENGINE_API void applyPhysicsWorldSettings(const ScenePhysics3DSettings& settings, JPH::PhysicsSettings& joltSettings);
#endif

}
