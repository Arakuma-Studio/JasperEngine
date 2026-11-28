// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The editor's Host of the project test runner (docs/specs/project-test-runner.md R14, §6.8):
// each Play Mode case in a Play session of its own. Play starts the case's scene with the fixed
// delta 1/fixedFps, Error Pause off, the Game view rendering at the case's resolution,
// math.randomseed(seed) before any script's init, the case's data folder (Play Data Clean or a
// snapshot) and the Game view's input ignored; Stop discards every play-mode change. A capture
// holds the game until shaders and textures are loaded, reads the Game view's frame and compares
// it with the golden.

#include "test/TestRunner.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace jasperengine::editor {
    class App;
    class Project;
}

namespace jasperengine::editor::gametests {

    class TestRunHost : public gametest::Host {
    public:
        TestRunHost(App& app, Project* project, std::filesystem::path root, gametest::Settings settings, gametest::Target target,
                    bool keepData);
        ~TestRunHost() override;

        bool beginSession(const gametest::CaseSpec& spec, std::string& error) override;
        SessionState sessionState(std::string& error) override;
        void endSession() override;
        bool sessionEnded() override;
        lua_State* luaState() override;
        bool readTestFile(const std::string& file, std::string& code, std::string& error) override;
        uint64_t gameFrames() override;
        double gameTime() override;
        bool beginCapture(const gametest::CaseSpec& spec, const std::string& name, const gametest::Json& options, std::string& error) override;
        bool pollCapture(gametest::Json& capture) override;
        std::string dataFolder() const override;

        // every editor frame of the run: what Play's tools or the user may have changed back
        void enforce();
        // the run ended: the Game view, Error Pause and the clock as they were
        void restore();

    private:
        App& app;
        Project* project;
        std::filesystem::path root;
        gametest::Settings settings;
        gametest::Target target;
        bool keepData = false;
        gametest::CaseSpec spec;
        uint32_t playSceneId = 0;
        uint64_t framesAtStart = 0;
        bool started = false;
        bool stopping = false;
        bool savedErrorPause = false;
        bool errorPauseSaved = false;
        // the capture under way
        bool capturing = false;
        bool heldForCapture = false;
        // a replay case (R9): no frame runs before its replay, which starts with the first update
        bool heldForReplay = false;
        int settledFrames = 0;
        int captureFrames = 0;
        std::string captureName;
        gametest::Json captureOptions;
        std::string dataFolderPath;
    };

}
