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
#include "test/TestTypes.h"

#include <string>

namespace jasperengine::player {

    // A player's case session (R24, R25), the parts that are not the scene load: the case's data://
    // sandbox <user data>/.jasper-test-runs/<run>/<n>/ (a snapshot copied in, PlayerPrefs with it),
    // the game's scripts released and a new Lua state (nothing a case leaves in Lua reaches the
    // next), math.randomseed and the fixed delta
    class JASPERENGINE_API PlayerSession {
    public:
        // false + error when the sandbox cannot be made
        static bool begin(const gametest::CaseSpec& spec, const std::string& runId, int caseNumber, std::string& folder, std::string& error);
        // data:// back to the launch's sandbox; the case's deleted unless kept
        static void end(const std::string& folder, bool keep);
        // the scripts of every running scene released (onDisable, onDestroy), then a new Lua state
        static void resetLua();
        // tests/index.json's "data": each snapshot's files, read through the engine's files (a
        // content pack has no folders); without it a snapshot is copied from the folder
        static void setSnapshotIndex(const gametest::Json& data);
    };

    class JASPERENGINE_API PlayerTests {
    public:
        // RunControl, when the engine starts a --run-tests launch: data:// is a sandbox of the
        // launch's own before the start scene runs and between cases, deleted at the run's end
        // unless kept (R25)
        static void launch(const LaunchOptions& options);
        // RunControl, at the ready frame of a --run-tests launch
        static void begin(const LaunchOptions& options);
        // RunControl, after every drawn frame of the run; reader reads the frame when it drew offscreen
        static void frame(const FrameReader& reader);
        // a capture waits for the next frame's composite
        static bool wantsFrame();
        static bool running();
    };

}
