// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// A development player's test run (docs/specs/project-test-runner.md R23-R27): --run-tests runs
// every Play Mode case of the content's tests/ (written by a Development build with Include
// Tests) from the ready frame, each in a reset game session (its scene loaded again, the
// project's Lua modules cleared, math.randomseed, the fixed delta) with data:// in a sandbox under
// <user data>/.jasper-test-runs/; writes the results JSON and the [test] log lines; exits 0, 8
// (a case failed, errored or hung) or 9 (no case). Compiled under JASPERENGINE_TEST_RUNNER only:
// RunControl's sites compile to nothing without it.

#include "Export.h"
#include "player/RunControl.h"

namespace jasperengine::player {

    class JASPERENGINE_API PlayerTests {
    public:
        // RunControl, at the ready frame of a --run-tests launch
        static void begin(const LaunchOptions& options);
        // RunControl, after every drawn frame of the run; reader reads the frame when it drew offscreen
        static void frame(const FrameReader& reader);
        // a capture waits for the next frame's composite
        static bool wantsFrame();
        static bool running();
    };

}
