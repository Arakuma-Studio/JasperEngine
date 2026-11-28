// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TestCommand.h"

#include "CallCommand.h"

#include "io/AtomicFile.h"

#include "json.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

    constexpr int TEST_EXIT_PASSED = 0;
    constexpr int TEST_EXIT_FAILED = 1;
    constexpr int TEST_EXIT_CANNOT_RUN = 2;
    constexpr int TEST_EXIT_NO_MATCH = 3;

    void printTestUsage(const std::string& exe) {
        std::cout
            << "Usage:\n"
            << "  " << exe << " test --project <dir> [--filter F] [--mode all|edit|play] [--target editor|desktop]\n"
            << "                        [--failed-only] [--junit FILE] [--json FILE] [--keep-data]\n"
            << "  " << exe << " test --project <dir> --import <results.json> [--junit FILE]\n"
            << "  " << exe << " test --project <dir> --list [--json]\n\n"
            << "Runs the project's tests (Tests/*.test.lua, docs/game-tests.md) with MCP run_tests in the editor that has\n"
            << "the project open over HTTP, else in a one-shot headless editor.\n\n"
            << "  --filter F       a case-insensitive part of \"<file>#<case>\", or tag:<t>; \",\" joins them (or)\n"
            << "  --failed-only    the previous run's failed, error and hung cases\n"
            << "  --junit FILE     also write JUnit XML (one <testsuite> per file)\n"
            << "  --json FILE      also write the results document (with --list: print the cases as JSON)\n"
            << "  --import FILE    a player's results (--test-results) instead of a run; its captures/ beside it\n"
            << "  --keep-data      keep each case's data folder\n\n"
            << "Exit codes: 0 every selected case passed or was skipped; 1 a case failed, errored or hung; 2 bad\n"
            << "arguments, or it could not run (the project locked by a stdio editor, no editor binary); 3 no case matched.\n";
    }

    int usage(const std::string& message) {
        std::cerr << "test: " << message << " (run test --help)\n";
        return TEST_EXIT_CANNOT_RUN;
    }

    std::string absolutePath(const std::string& given) {
        std::error_code ec;
        return fs::absolute(fs::path(given), ec).lexically_normal().string();
    }

}

int jasperengine::editor::cli::TestCommand::run(int argc, char** argv, const char* executableName) {
    const std::string exe = fs::path(executableName ? executableName : "jasperengine-editor").filename().string();
    std::string project, junit, jsonOut, importFile;
    bool list = false;
    nlohmann::json args = nlohmann::json::object();
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        auto value = [&](std::string& out) {
            if (i + 1 >= argc) return false;
            out = argv[++i];
            return true;
        };
        std::string text;
        if (arg == "-h" || arg == "--help") {
            printTestUsage(exe);
            return TEST_EXIT_PASSED;
        } else if (arg == "--project") {
            if (!value(project)) return usage("--project needs a directory");
        } else if (arg == "--filter") {
            if (!value(text)) return usage("--filter needs a value (a text or tag:<t>)");
            args["filter"] = text;
        } else if (arg == "--mode") {
            if (!value(text) || (text != "all" && text != "edit" && text != "play")) return usage("--mode is all, edit or play");
            args["mode"] = text;
        } else if (arg == "--target") {
            if (!value(text) || (text != "editor" && text != "desktop")) return usage("--target is editor or desktop");
            args["target"] = text;
        } else if (arg == "--failed-only") {
            args["failed_only"] = true;
        } else if (arg == "--keep-data") {
            args["keep_data"] = true;
        } else if (arg == "--junit") {
            if (!value(junit)) return usage("--junit needs a file");
        } else if (arg == "--json") {
            // --list --json prints; else it names a file
            if (i + 1 < argc && argv[i + 1][0] != '-') jsonOut = argv[++i];
            else jsonOut = "-";
        } else if (arg == "--import") {
            if (!value(importFile)) return usage("--import needs a results file");
        } else if (arg == "--list") {
            list = true;
        } else {
            return usage("unknown option " + arg);
        }
    }
    if (project.empty()) return usage("--project <dir> is required");
    std::error_code ec;
    if (!fs::is_regular_file(fs::path(project) / "project.yaml", ec)) return usage(project + " is not a project (no project.yaml)");
    if (list) args["action"] = "list";
    else if (!importFile.empty()) {
        args["action"] = "import";
        args["results_file"] = absolutePath(importFile);
    } else {
        args["action"] = "run";
        args["wait"] = true;
        args["timeout_seconds"] = 3500;
    }
    if (!junit.empty()) args["junit"] = absolutePath(junit);

    std::vector<CallResult> results;
    const int code = CallCommand::runCalls(absolutePath(project), {{"run_tests", args}}, false, results, executableName);
    const CallResult result = results.empty() ? CallResult() : results.front();
    if (code != 0) {
        std::cerr << result.text << "\n";
        if (result.text.rfind("No test matched", 0) == 0) return TEST_EXIT_NO_MATCH;
        // a run that could not start: no root, Play running, a run in progress, a connection or editor problem
        return TEST_EXIT_CANNOT_RUN;
    }
    if (list) {
        if (jsonOut == "-") std::cout << result.structured.dump(2) << "\n";
        else std::cout << result.text << "\n";
        return TEST_EXIT_PASSED;
    }
    std::cout << result.text << "\n";
    const nlohmann::json counts = result.structured.value("counts", nlohmann::json::object());
    if (!jsonOut.empty() && jsonOut != "-") {
        const std::string file = result.structured.value("results_file", std::string());
        std::ifstream in(fs::path(project) / file, std::ios::binary);
        std::ostringstream bytes;
        bytes << in.rdbuf();
        if (file.empty() || bytes.str().empty()) {
            std::cerr << "test: the run wrote no results document to copy to " << jsonOut << "\n";
            return TEST_EXIT_CANNOT_RUN;
        }
        const jasperengine::io::WriteResult written = jasperengine::io::writeFileAtomic(absolutePath(jsonOut), bytes.str());
        if (!written.ok()) {
            std::cerr << "test: --json: " << written.message << "\n";
            return TEST_EXIT_CANNOT_RUN;
        }
    }
    if (result.structured.contains("junit_error")) std::cerr << "test: --junit: " << result.structured.value("junit_error", "") << "\n";
    if (result.structured.value("cancelled", false)) return TEST_EXIT_FAILED;
    const int bad = counts.value("failed", 0) + counts.value("error", 0) + counts.value("hung", 0);
    return bad > 0 ? TEST_EXIT_FAILED : TEST_EXIT_PASSED;
}
