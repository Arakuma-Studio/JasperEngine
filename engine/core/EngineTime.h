// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The Time class. Not named Time.h: on case-insensitive file systems (macOS, Windows) it would
// stand in for the C library's <time.h> through the engine's include paths.

#ifndef ENGINE_TIME_H
#define ENGINE_TIME_H

#include "Export.h"

#include <cstdint>

namespace jasperengine {

    // Unity's Time class: the game clocks under their Unity names, for C++ and Lua (`Time.*`).
    // A facade over Engine (Engine.deltatime, Engine.updateTime... stay): the names map
    // Time.fixedDeltaTime = Engine::updateTime (the fixed step) and Time.captureDeltaTime =
    // Engine::fixedDeltatime (a fixed frame delta), see docs/mcp/guide/lua-api.md "Time".
    class JASPERENGINE_API Time {
    public:
        // Game seconds of this frame (times timeScale); inside a fixed step, fixedDeltaTime
        static float getDeltaTime();
        // The frame delta before timeScale (0 while the app, the game events or the simulation are
        // paused or held);
        // inside a fixed step, fixedUnscaledDeltaTime
        static float getUnscaledDeltaTime();

        // Game seconds per fixed step (physics, onFixedUpdate). Default 1/60.
        static float getFixedDeltaTime();
        static void setFixedDeltaTime(float seconds);
        // A fixed step in real seconds: fixedDeltaTime / timeScale (0 at time scale 0)
        static float getFixedUnscaledDeltaTime();

        // Game seconds since start; inside a fixed step, fixedTime
        static double getTime();
        // Sum of the fixed steps run; inside a step, the time at its start
        static double getFixedTime();
        // Sum of the unscaled deltas (stops while paused, runs at time scale 0)
        static double getUnscaledTime();
        // Wall clock seconds since the engine started (pauses included)
        static double getRealtimeSinceStartup();
        // Game seconds since the last scene load
        static double getTimeSinceLevelLoad();
        // Frames the update loop has run
        static uint64_t getFrameCount();

        // Game speed: 1 real time, 0.5 half, 0 frozen. Clamped to [0, 100].
        static float getTimeScale();
        static void setTimeScale(float scale);

        // Upper bound of a frame's delta (default 0.25 s); values <= 0 are ignored with a warning
        static float getMaximumDeltaTime();
        static void setMaximumDeltaTime(float seconds);

        // Fixed steps a frame may run; 0 (default) automatic: as many as maximumDeltaTime lets
        // through (Engine::getFixedStepCap). Past it the frame's other steps are dropped
        static int getMaximumFixedStepsPerFrame();
        static void setMaximumFixedStepsPerFrame(int steps);
        // Fixed steps dropped since the Play (editor) or the game started
        static uint64_t getDroppedFixedSteps();

        // When > 0 every frame advances exactly this many seconds (before timeScale) whatever the
        // wall clock measured; 0 (default) = measured frames. Engine::fixedDeltatime.
        static double getCaptureDeltaTime();
        static void setCaptureDeltaTime(double seconds);

        // Exponential average of deltaTime
        static float getSmoothDeltaTime();
        // True inside onFixedUpdate, the physics step and the coroutines waiting for it
        static bool isInFixedTimeStep();
    };

}

#endif //ENGINE_TIME_H
