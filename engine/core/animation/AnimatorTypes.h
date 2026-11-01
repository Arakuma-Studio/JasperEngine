// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S21-animation.md D2, D5, D8, D16)
// - Purpose: the small enums of the Animator (component, controller, masks, montages, IK) without
//   the rest of the model, for headers included everywhere (AnimatorComponent reaches every TU
//   through EntityRegistry.h). Their JSON names (`.controller`, `.mask`, `.montage`) and Lua
//   names come from AnimatorNames (AnimatorJson.h).
// - Invariants: appending is allowed, reordering is not (the scene YAML stores the component's
//   enums by name, the files by their JSON names, tests by value). The AnimatorComponent's enums
//   are int-sized (the reflection tables' Enum properties); the assets' are one byte.

#ifndef ANIMATOR_TYPES_H
#define ANIMATOR_TYPES_H

#include <cstdint>

namespace jasperengine{

    // Unity's AnimatorUpdateMode
    enum class AnimatorUpdateMode : int {
        Normal,             // the scene's update, scaled time
        AnimatePhysics,     // in the fixed steps, before physics
        UnscaledTime        // the scene's update, real time (Engine::getDeltatime ignoring the time scale)
    };

    // Unity's AnimatorCullingMode
    enum class AnimatorCullingMode : int {
        AlwaysAnimate,
        CullUpdateTransforms,   // invisible: time, states, events and root motion advance; no pose written
        CullCompletely          // invisible: nothing advances
    };

    // which bone GameObjects get the evaluated local transform (skin matrices never depend on it)
    enum class AnimatorWriteBones : int {
        All,
        Exposed     // the listed paths and the bones with children that are not bones (attachments)
    };

    // which clips of a layer fire their events
    enum class AnimatorEventMode : int {
        LeaderOnly,     // per layer the clip with the highest weight
        All             // every clip with a weight above 0
    };

    enum class AnimatorRetarget : int {
        Auto,   // humanoid clips made on another humanoid model play retargeted (D12)
        Off
    };

    enum class AnimatorParameterType : uint8_t { Float, Int, Bool, Trigger };

    // Unity's AnimatorConditionMode (Trigger uses If)
    enum class AnimatorConditionMode : uint8_t { If, IfNot, Greater, Less, Equals, NotEqual };

    enum class AnimatorLayerBlending : uint8_t { Override, Additive };

    enum class BlendTreeType : uint8_t {
        Simple1D,
        SimpleDirectional2D,
        FreeformDirectional2D,
        FreeformCartesian2D,
        Direct
    };

    enum class TransitionInterruptionSource : uint8_t { None, Current, Next, CurrentThenNext, NextThenCurrent };

    // a state's loop: its clips' own wrap (Loop / PingPong loop), or forced. Loop keeps a
    // ping-pong clip ping-ponging; Repeat always loops forward from the start (upstream's state
    // wrap "loop"); PingPong forces ping-pong; ClampForever holds the last frame like Once (the
    // Animation Player's name, kept from upstream controllers)
    enum class AnimatorStateLoop : uint8_t { Inherit, Loop, Once, PingPong, ClampForever, Repeat };

    // what an additive state's clips are a difference to
    enum class AdditiveReference : uint8_t {
        FirstFrame,     // the clip's pose at time 0
        Rest,           // the rig's rest pose
        Time,           // the clip's pose at `additiveTime`
        Clip            // the clip's pose at its own additive_reference (.anim; 0 = the first frame)
    };

    // The humanoid body parts of an avatar mask (Unity's AvatarMaskBodyPart without the IK goals;
    // the weight rule is AvatarMaskWeights.h)
    enum class AvatarBodyPart : uint8_t {
        Root,       // root motion (and, with root_hierarchy, the GameObject and the transforms above the hips)
        Body,       // hips and spine chain
        Head,       // neck, head
        LeftArm,    // shoulder, upper and lower arm
        RightArm,
        LeftHand,   // the hand bone
        RightHand,
        LeftLeg,    // upper and lower leg, foot, toes
        RightLeg,
        LeftFingers,    // everything below the hand (fingers, props held)
        RightFingers,
        Count
    };

    constexpr int AVATAR_BODY_PART_COUNT = static_cast<int>(AvatarBodyPart::Count);

    enum class AnimatorIKType : int {
        TwoBone,    // upper, mid, end: the end reaches the target, the mid bends toward the pole
        Fabrik,     // a chain of 2..16 bones
        LookAt,     // up to 4 bones turn toward the target, weighted, clamped
        FootIK,     // one leg: raycast below the foot, the pelvis drops, the ankle follows the ground
        FootLock    // one foot stays where it touched down while it is in contact
    };

}

#endif //ANIMATOR_TYPES_H
