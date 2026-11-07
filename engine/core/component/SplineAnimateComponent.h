// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SPLINE_ANIMATE_COMPONENT_H
#define SPLINE_ANIMATE_COMPONENT_H

#include "ecs/Entity.h"
#include "util/FunctionSubscribe.h"

namespace jasperengine{

    // how a Spline Animate's progress is set (Unity's SplineAnimate.Method)
    enum class SplineAnimateMethod{
        Duration,       // the whole spline in duration seconds
        Speed           // speed world units per second
    };

    enum class SplineLoop{
        Once,           // stops at the end (onComplete)
        Loop,           // from the start again
        PingPong        // back and forth
    };

    // Once only
    enum class SplineEasing{
        None,
        EaseIn,
        EaseOut,
        EaseInOut
    };

    // how the follower turns
    enum class SplineAlignment{
        None,               // keeps its rotation
        SplineTangent,      // forward along the curve, up the world's
        SplineTangentAndUp, // forward along the curve, up the curve's (roll)
        World               // the world's axes
    };

    // Moves its GameObject along another GameObject's Spline at constant world speed (Unity's
    // SplineAnimate; S18 R10). In a 2D spline (up axis Z) the follower turns about Z.
    struct SplineAnimateComponent{
        // component checkbox: a disabled follower stays where it is
        bool enabled = true;

        Entity spline = NULL_ENTITY;        // the GameObject with the Spline
        SplineAnimateMethod method = SplineAnimateMethod::Duration;
        float duration = 5.0f;              // seconds
        float speed = 2.0f;                 // world units per second
        SplineLoop loop = SplineLoop::Loop;
        SplineEasing easing = SplineEasing::None;
        SplineAlignment alignment = SplineAlignment::SplineTangentAndUp;
        float startOffset = 0.0f;           // 0-1 of the length
        float lateralOffset = 0.0f;         // world units to the right of the curve
        float verticalOffset = 0.0f;        // world units along its up
        bool playOnAwake = true;

        // runtime (not saved): SplineSystem's and the Lua API's
        bool playing = false;
        bool awoken = false;                // playOnAwake was applied
        float elapsed = 0.0f;               // seconds of travel since the start (Duration and Speed)
        float normalizedTime = 0.0f;        // 0-1 progress of the current pass (before easing)
        float distance = 0.0f;              // world units along the curve now
        bool completed = false;             // Once: reached the end
        FunctionSubscribe<void()> onComplete;

        // every event, for the clears at Stop and on tag removal (tools/check_event_clears.py)
        template<typename F> void forEachEvent(F&& f) { f(onComplete); }
    };

}

#endif //SPLINE_ANIMATE_COMPONENT_H
