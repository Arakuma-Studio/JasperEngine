// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The project test runner's data (docs/specs/project-test-runner.md §6.2, §6.4): a test root's
// settings (tests.yaml), the cases a run plans, and what each case gave. Compiled under
// JASPERENGINE_TEST_RUNNER only (the editor, the unit tests, development players).

#include "Export.h"

#include "json.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::gametest {

    using Json = nlohmann::json;

    enum class Mode { Play, Edit };
    enum class Status { Passed, Failed, Skipped, Error, Hung };

    JASPERENGINE_API const char* modeName(Mode mode);
    JASPERENGINE_API const char* statusName(Status status);

    struct ImageTolerance {
        int tolerance = 8;
        double maxFraction = 0.001;
        int maxDiff = 48;
        double maxMean = 0.25;
    };

    // tests.yaml v1 (§6.2): every key optional
    struct Settings {
        static constexpr int VERSION = 1;
        int version = VERSION;
        std::vector<std::string> targets{"editor"};
        int fixedFps = 60;
        int width = 1280;
        int height = 720;
        uint64_t seed = 1;
        int64_t timeoutFrames = 18000;
        int64_t waitFrames = 600;
        double hangSeconds = 600.0;
        ImageTolerance image;
        bool consoleErrorsFail = true;
        bool divergenceFail = true;
        // keys this version does not know: kept on rewrite, warned once
        std::vector<std::string> unknownKeys;

        JASPERENGINE_API Json toJson() const;
        // the JSON form (an exported build's tests/tests.json, or tests.yaml read by the editor);
        // false + error for a newer version or a bad value
        JASPERENGINE_API static bool fromJson(const Json& json, Settings& out, std::string& error);
    };

    struct CaseSpec {
        std::string id;          // "<file>#<name>"
        std::string file;        // root-relative, forward slashes
        std::string name;
        int line = 0;
        Mode mode = Mode::Play;
        std::string scene;       // empty: the project's start scene
        std::string data = "clean";
        std::string replay;
        int fixedFps = 60;
        int width = 1280;
        int height = 720;
        int64_t timeoutFrames = 18000;
        int64_t waitFrames = 600;
        uint64_t seed = 1;
        std::vector<std::string> tags;
        std::vector<std::string> platforms;
        std::vector<std::string> expectErrors;
        std::string skip;
        // the static listing saw this name twice in its file
        bool duplicate = false;
    };

    struct Failure {
        std::string kind;        // check, assert, equal, timeout, error, console, expected_error_missing, capture, divergence, hung
        std::string message;
        std::string file;
        int line = 0;
        int64_t frame = 0;
    };

    struct ConsoleLine {
        std::string level;       // error, warning
        std::string message;
        std::string file;
        int line = 0;
    };

    struct CaseResult {
        CaseSpec spec;
        Status status = Status::Passed;
        int64_t frames = 0;
        double gameSeconds = 0.0;
        double wallMs = 0.0;
        std::string skipReason;
        std::vector<Failure> failures;
        Json captures = Json::array();   // §6.4 capture objects
        std::vector<ConsoleLine> console;
        std::vector<std::string> log;
    };

    struct Counts {
        int total = 0;
        int passed = 0;
        int failed = 0;
        int skipped = 0;
        int error = 0;
        int hung = 0;
        int excludedEdit = 0;
    };

    struct Target {
        std::string platform;    // editor's host OS or a player's: linux, windows, macos, android, ios, web
        std::string arch;
        std::string backend;     // glcore, gles3, metal, d3d11, vulkan
        bool editor = false;
        std::string device;
        int width = 1280;
        int height = 720;
    };

    struct RunInfo {
        std::string id;
        std::string project;
        Target target;
        Json revision = Json::object();   // {vcs, id, dirty}
        std::string editorVersion;
        std::string engineVersion;
        std::string started;              // ISO 8601 UTC
        double wallSeconds = 0.0;
        std::string filter;
        std::string mode = "all";
    };

    struct RunResult {
        static constexpr int VERSION = 1;
        RunInfo run;
        Counts counts;
        std::vector<CaseResult> cases;
    };

}
