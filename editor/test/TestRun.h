// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// One test run in the editor (docs/specs/project-test-runner.md R14-R19, R28-R29): the selection
// (filter, mode, failed_only), the Edit Mode cases at once, then each Play Mode case in its own
// Play session over the next editor frames, the results document under
// .jasperengine/test-results/runs/, the ledger and the unverified targets. One run at a time;
// MCP run_tests and the Test Runner window both start it.

#include "test/GameTests.h"
#include "test/TestRunner.h"

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor {
    class App;
    class Project;
    namespace mcp { class McpCall; }
}

namespace jasperengine::editor::gametests {

    class TestRunHost;

    struct RunRequest {
        std::string filter;
        std::string mode = "all";          // all, edit, play
        bool failedOnly = false;
        bool keepData = false;
    };

    enum class SelectError { None, NoRoot, Settings, NothingMatched };

    struct Selection {
        fs::path root;
        gametest::Settings settings;
        std::vector<gametest::ScannedFile> files;
        std::vector<gametest::CaseSpec> cases;
        SelectError error = SelectError::None;
        std::string message;
    };

    // the root, its settings and the cases a request selects (R18): a selection of nothing is an error
    Selection selectCases(Project* project, const RunRequest& request);
    // "No test matched ..." (§6.6)
    std::string nothingMatchedText(const RunRequest& request);
    // the editor's target (R19)
    gametest::Target editorTarget(const gametest::Settings& settings);

    struct RunOutcome {
        gametest::RunResult result;
        fs::path resultsFile;
        // case id -> the tests.yaml targets without a pass at the current revision (R29)
        std::map<std::string, std::vector<std::string>> unverified;
        std::vector<std::string> targets;
        std::vector<std::string> notes;
        bool cancelled = false;
    };

    class TestRun {
    public:
        static TestRun& get();

        bool running() const { return active; }
        // false + error when it cannot start (no root, nothing matched, a run already going);
        // done runs on the main thread when the run ends (at once without Play Mode cases)
        bool start(const std::shared_ptr<mcp::McpCall>& call, App& app, Project* project, const RunRequest& request,
                   std::function<void(const RunOutcome&)> done, std::string& error);
        // App, every editor frame
        void tick();
        void cancel();

        // progress (window, job)
        size_t casesDone() const;
        size_t casesPlanned() const;
        std::string currentCase() const;
        // the last finished run (the window's tree); revision changes with each
        const RunOutcome* last() const { return lastOutcome.get(); }
        uint64_t revision() const { return lastRevision; }
        // the run's job, for the busy message ("" from the window)
        std::string jobId;

    private:
        void finish(bool cancelled);

        bool active = false;
        bool cancelRequested = false;
        Project* project = nullptr;
        App* app = nullptr;
        Selection selection;
        RunRequest request;
        gametest::Target target;
        std::vector<gametest::CaseResult> editResults;
        std::unique_ptr<TestRunHost> host;
        std::unique_ptr<gametest::TestRunner> runner;
        std::function<void(const RunOutcome&)> done;
        std::string runId;
        std::string started;
        double startedSeconds = 0.0;
        std::unique_ptr<RunOutcome> lastOutcome;
        uint64_t lastRevision = 0;
    };

    // a results document produced elsewhere (a player): its pending captures compared with the
    // goldens (PNGs under captures/ beside it), the ledger updated (R30); false + error
    bool importResults(Project* project, const fs::path& file, RunOutcome& out, std::string& error);

    // Accept (§6.5): the capture "<case id>/<name>" of the last run made the golden, as one undo
    // step; captures ["all"]: every new or failed one. written gets the project-relative goldens.
    bool acceptCaptures(Project* project, const std::vector<std::string>& captures, bool forTarget, std::vector<std::string>& written,
                        std::string& error);

}
