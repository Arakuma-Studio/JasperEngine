// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Window > General > Test Runner (docs/specs/project-test-runner.md R38-R39): the project's test
// cases in Edit Mode and Play Mode tabs, run through MCP run_tests in-process (so Edit Mode's
// Editor.call has its call), with each case's status, time, Last Passed On (from the ledger)
// and tags, and a detail pane with its failures (click-to-source), console lines and captures
// (golden, actual, diff, Accept Image).

#include "test/GameTests.h"
#include "test/TestRun.h"
#include "texture/Texture.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class App;
    class Project;

    class TestRunnerWindow {
    public:
        static constexpr const char* WINDOW_NAME = "Test Runner";

        static TestRunnerWindow& get();

        void show(App& app, Project* project);
        void setOpen(bool open) { windowOpen = open; }
        bool isOpen() const { return windowOpen; }

    private:
        struct Preview {
            Texture texture;
            int width = 0, height = 0;
            std::string path;
        };

        void refresh(Project* project, bool force);
        void startRun(const std::string& filter, bool failedOnly, const char* mode);
        void drawToolbar(Project* project);
        void drawTree(Project* project, gametest::Mode mode);
        void drawDetail(Project* project);
        void drawCreateMenu(Project* project);
        const gametest::CaseResult* resultOf(const std::string& id) const;
        bool passesStatusFilter(const gametest::CaseSpec& spec) const;
        void preview(Preview& slot, const std::filesystem::path& file, float width);
        void releasePreviews();

        bool windowOpen = false;
        std::filesystem::path projectDir;
        std::filesystem::path root;
        gametest::Settings settings;
        std::string settingsError;
        std::vector<gametest::ScannedFile> files;
        gametests::Ledger ledger;
        double lastScan = -1.0;
        uint64_t seenRun = 0;
        std::string selected;              // a case id
        std::map<std::string, bool> checked;
        char filter[128] = {};
        int statusFilter = 0;              // All, Failed, Skipped, Unverified On...
        int target = 0;                    // Editor, This Computer
        std::string message;
        Preview golden, actual, diff;
        std::string newScriptName;
        bool scriptPopup = false;
        char scriptName[64] = {};
    };

}
