// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Physics enums shared by 2D and 3D bodies, colliders and joints (physics-2d, physics-3d).

#ifndef PHYSICS_COMMON_H
#define PHYSICS_COMMON_H

#include <cmath>
#include <cstdint>

// <windows.h> (WinBase.h) defines IGNORE, the name of a JointBreakAction
#ifdef IGNORE
#undef IGNORE
#endif

namespace jasperengine{

    // How a body is drawn between fixed steps. SCENE follows the scene setting
    // physics_interpolation (on: INTERPOLATE, off: NONE).
    enum class RigidbodyInterpolation{
        SCENE,
        NONE,
        INTERPOLATE,
        EXTRAPOLATE
    };

    // Joint anchors and axes: WORLD = world points (scenes saved before local anchors, and the
    // Lua set*Joint helpers); LOCAL = Unity, anchorA in bodyA's space, anchorB in bodyB's, axis in
    // bodyA's frame, so a joint inside a prefab works wherever the prefab is placed
    enum class JointAnchorSpace{
        WORLD,
        LOCAL
    };

    // What a joint does when its reaction force or torque passes breakForce / breakTorque
    // (Unity JointBreakAction2D)
    enum class JointBreakAction{
        IGNORE,         // nothing
        CALLBACK_ONLY,  // onJointBreak2D / onJointBreak, the joint stays
        DISABLE,        // callback, then the joint is disabled (enabled = false)
        DESTROY         // callback, then the joint component is removed
    };

    // How the friction or bounciness of two touching colliders combine. A collider without a
    // PhysicsMaterial has no mode (NONE). The pair uses the highest of the two sides' modes in
    // this order (MAXIMUM wins); with no mode on either side, the legacy rule.
    enum class PhysicsMaterialCombine{
        NONE = 0,
        AVERAGE,
        GEOMETRIC_MEAN,
        MINIMUM,
        MULTIPLY,
        MAXIMUM
    };

    // The combined value of a and b (each side's own friction or bounciness) by their modes;
    // legacy is the rule when neither side has one (Box2D and Jolt before materials: geometric
    // mean friction, maximum restitution). Pure, any thread.
    inline float combineSurface(float a, PhysicsMaterialCombine ma, float b, PhysicsMaterialCombine mb, PhysicsMaterialCombine legacy){
        PhysicsMaterialCombine mode = ((int)ma > (int)mb) ? ma : mb;
        if (mode == PhysicsMaterialCombine::NONE){
            mode = legacy;
        }
        switch (mode){
            case PhysicsMaterialCombine::AVERAGE: return 0.5f * (a + b);
            case PhysicsMaterialCombine::GEOMETRIC_MEAN: return std::sqrt(a * b);
            case PhysicsMaterialCombine::MINIMUM: return a < b ? a : b;
            case PhysicsMaterialCombine::MULTIPLY: return a * b;
            case PhysicsMaterialCombine::MAXIMUM: return a > b ? a : b;
            case PhysicsMaterialCombine::NONE: break;
        }
        return std::sqrt(a * b);
    }

}

#endif //PHYSICS_COMMON_H
