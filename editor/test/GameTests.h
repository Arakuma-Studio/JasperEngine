// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// A project's test root on disk (docs/specs/project-test-runner.md R1, R4, §6.1, §6.5, R28-R29):
// finding it, its tests.yaml, the static listing, the goldens, the Play Data snapshots and the
// per-machine results ledger under .jasperengine/test-results/.

#include "test/TestResults.h"
#include "test/TestScan.h"
#include "test/TestTypes.h"
#include "texsynth/SynthImage.h"

#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::gametests {

    using gametest::Json;
    namespace fs = std::filesystem;

    // the root's folder names, in lookup order (R1)
    inline constexpr const char* ROOT_NAMES[] = {"Tests", "GameTests"};
    inline constexpr const char* MARKER = "tests.yaml";
    inline constexpr const char* NO_ROOT_TEXT =
        "No tests in this project: create Tests/tests.yaml (Test Runner > Create > Test Folder, or a file with `version: 1`).";

    // <project>/Tests or <project>/GameTests holding tests.yaml; empty when none
    fs::path findRoot(const fs::path& projectDir);
    // `dir` (a top-level folder of the project) is the test root: a non-asset folder (R1)
    bool isTestRoot(const fs::path& projectDir, const fs::path& dir);

    // tests.yaml; false + error (a newer version, a bad value). Unknown keys are warned once per editor run.
    bool loadSettings(const fs::path& root, gametest::Settings& out, std::string& error);

    // the static listing of the root
    std::vector<gametest::ScannedFile> scan(const fs::path& root, const gametest::Settings& settings);

    // Create > Test Folder (R1): the root to create. "Tests" unless Tests/ holds files without
    // the marker, then "GameTests"; `blocking` gets those files (the first few)
    std::string folderToCreate(const fs::path& projectDir, std::vector<std::string>& blocking);
    std::string defaultTestsYaml();
    // Create > Test Script (R39): the template, with the start scene's path
    std::string testScriptTemplate(const std::string& startScene);

    // §6.5: the golden a capture compares with on (platform, backend); empty when none
    fs::path findGolden(const fs::path& root, const std::string& caseId, const std::string& name, const std::string& platform,
                        const std::string& backend);
    // where Accept writes: the compared golden, else <name>.png; forTarget: <name>.<platform>-<backend>.png
    fs::path acceptPath(const fs::path& root, const std::string& caseId, const std::string& name, const std::string& platform,
                        const std::string& backend, bool forTarget);

    // .jasperengine/test-results/
    fs::path resultsDir(const fs::path& projectDir);
    // A capture's frame (RGBA8) written as the run's actual image and compared with its golden
    // (§6.5, diff_images' maths, premultiplied): the §6.4 capture object, status passed, failed
    // (the diff numbers and a panel image) or new (no golden)
    Json compareCapture(const fs::path& projectDir, const fs::path& root, const std::string& caseId, const std::string& name,
                        const texsynth::Image& actual, const std::string& platform, const std::string& backend,
                        const gametest::ImageTolerance& tolerance, const Json& options);
    // the actual image of a capture of the last run
    fs::path captureActualPath(const fs::path& projectDir, const std::string& caseId, const std::string& name);

    // Tests/Data/<name>/ snapshots
    std::vector<std::string> listSnapshots(const fs::path& projectDir);
    // copies a folder tree (a snapshot into a session folder); bytes gets the size copied
    bool copyTree(const fs::path& from, const fs::path& to, std::string& error, uint64_t* bytes = nullptr);
    uint64_t treeBytes(const fs::path& dir);

    // R50: what an export carries of the test root. A Development build with Include Tests gets
    // <contentRoot>/tests/ (the root without Golden/) and tests/index.json (the settings and the
    // test files, which a player reads where it cannot list folders); a release build with the
    // setting gets nothing and `warning`. False + error when a copy fails.
    inline constexpr const char* INCLUDE_TESTS_RELEASE_WARNING = "Include Tests applies to Development builds only";
    bool exportTests(const fs::path& projectDir, bool development, bool includeTests, const fs::path& contentRoot, std::string& warning,
                     std::string& error);
    // run_tests {target: "desktop"} builds with Include Tests whatever the project's setting is
    void setIncludeTestsForced(bool forced);
    bool includeTestsForced();

    // ---- the ledger (R28, R29, R51)

    // a case file's revision: {vcs, id, dirty} from the project's VCS, else {vcs: "none", id: <SHA-256 of the file>}
    Json revisionOf(Project* project, const fs::path& root, const std::string& file);
    // the target names of a run ("editor", "linux", "android"...)
    std::string targetName(const gametest::Target& target);

    struct Ledger {
        static constexpr int VERSION = 1;
        Json cases = Json::object();   // case id -> target -> {status, last_run, last_pass, backend, editor_version, revision}
    };
    // false + error for an unreadable file; a newer version is moved aside (ledger.v<N>.json) and
    // a new one starts (note says so)
    bool readLedger(const fs::path& projectDir, Ledger& out, std::string& note);
    bool writeLedger(const fs::path& projectDir, const Ledger& ledger, std::string& error);
    // one run's cases into the ledger
    void recordRun(Ledger& ledger, const gametest::RunResult& run, Project* project, const fs::path& root);
    // R29: the targets of `targets` where the case has no pass at the current revision
    std::vector<std::string> unverifiedTargets(const Ledger& ledger, const std::string& caseId, const std::vector<std::string>& targets,
                                               const Json& currentRevision);
    // the run's document under runs/ (the last 20 are kept); the path written
    bool writeRunDocument(const fs::path& projectDir, const gametest::RunResult& run, fs::path& written, std::string& error);
    // the last run's document (failed_only, accept), false when none
    bool readLastRun(const fs::path& projectDir, gametest::RunResult& out);

}
