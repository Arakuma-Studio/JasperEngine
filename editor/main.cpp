// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Backend.h"
#include "Out.h"
#include "cli/CommandLine.h"
#include "mcp/McpLaunch.h"
#include "util/ShellEnv.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace jasperengine;

namespace {

int runEditor(int argc, char* argv[]) {
    // A desktop launcher starts the editor without the user's shell environment,
    // so the build tools go back on PATH before anything can spawn a command.
    editor::ShellEnv::bootstrapPath();
    // Relative path arguments are relative to the directory the editor started in: read it first
    editor::CommandLine::invocationDirectory();

    // CLI subcommands run before the backend starts, so they never create a window.
    if (argc >= 2 && std::strcmp(argv[1], "export") == 0) {
        return editor::CommandLine::runExportCommand(argc - 1, argv + 1, argv[0]);
    }
    if (argc >= 2 && std::strcmp(argv[1], "shaders") == 0) {
        return editor::CommandLine::runShadersCommand(argc - 1, argv + 1, argv[0]);
    }
    if (argc >= 2 && std::strcmp(argv[1], "scene-dump") == 0) {
        return editor::CommandLine::runSceneDumpCommand(argc - 1, argv + 1, argv[0]);
    }
    if (argc >= 2 && std::strcmp(argv[1], "assets") == 0) {
        return editor::CommandLine::runAssetsCommand(argc - 1, argv + 1, argv[0]);
    }
    if (argc >= 2 && std::strcmp(argv[1], "check") == 0) {
        return editor::CommandLine::runCheckCommand(argc - 1, argv + 1, argv[0]);
    }
    if (argc >= 2 && std::strcmp(argv[1], "ui-support") == 0) {
        return editor::CommandLine::runUiSupportCommand(argc - 1, argv + 1, argv[0]);
    }
    if (argc >= 2 && std::strcmp(argv[1], "ui-effects") == 0) {
        return editor::CommandLine::runUiEffectsCommand(argc - 1, argv + 1, argv[0]);
    }
    if (argc >= 2 && std::strcmp(argv[1], "import-ui") == 0) {
        return editor::CommandLine::runImportUiCommand(argc - 1, argv + 1, argv[0]);
    }
    if (argc >= 2 && std::strcmp(argv[1], "benchmark") == 0) {
        if (argc >= 3 && (std::strcmp(argv[2], "-h") == 0 || std::strcmp(argv[2], "--help") == 0 || std::strcmp(argv[2], "help") == 0)) {
            return editor::CommandLine::runBenchmarkHelp(argv[0]);
        }
        return editor::Backend::init(argc, argv);
    }
    // The GUI editor as an MCP server on stdio (see editor/mcp/McpLaunch.h)
    if (argc >= 2 && std::strcmp(argv[1], "mcp") == 0) {
        const int status = editor::mcp::prepareMcpCommand(argc - 1, argv + 1, argv[0]);
        if (status >= 0) {
            return status;
        }
        char* editorArgv[] = {argv[0], nullptr};
        const int result = editor::Backend::init(1, editorArgv);
        editor::mcp::finishMcpCommand();
        return result;
    }
    // The normal editor with the MCP HTTP server on a port
    if (argc >= 2 && std::strncmp(argv[1], "--mcp-port", 10) == 0) {
        if (!editor::mcp::parseMcpPortFlag(argc, argv)) {
            return 2;
        }
        char* editorArgv[] = {argv[0], nullptr};
        return editor::Backend::init(1, editorArgv);
    }
    if (argc >= 2) {
        return editor::CommandLine::runHelpCommand(argc - 1, argv + 1, argv[0]);
    }

    return editor::Backend::init(argc, argv);
}

// --log-level <level>[,category=level...] (or --log-level=...), anywhere on the command line:
// applied (after JASPER_LOG_LEVEL) and removed before the subcommands parse their arguments
bool takeLogLevelFlag(std::vector<char*>& args) {
    std::string spec;
    if (const char* fromEnvironment = std::getenv("JASPER_LOG_LEVEL")) spec = fromEnvironment;
    for (size_t i = 1; i < args.size();) {
        const std::string arg = args[i];
        if (arg == "--log-level") {
            if (i + 1 >= args.size()) {
                std::fprintf(stderr, "--log-level needs a level: trace, debug, info, warn, error, fatal or off "
                                     "(and category=level items, comma separated)\n");
                return false;
            }
            spec = args[i + 1];
            args.erase(args.begin() + static_cast<long>(i), args.begin() + static_cast<long>(i) + 2);
            continue;
        }
        if (arg.rfind("--log-level=", 0) == 0) {
            spec = arg.substr(12);
            args.erase(args.begin() + static_cast<long>(i));
            continue;
        }
        i++;
    }
    if (spec.empty()) return true;
    std::string error;
    if (!editor::Out::setLevelOverride(spec, &error)) {
        std::fprintf(stderr, "--log-level: %s\n", error.c_str());
        return false;
    }
    return true;
}

}

int main(int argc, char* argv[]) {
    // before anything logs: the in-memory store and the terminal until a Console window exists
    editor::Out::startLogging();
    std::vector<char*> args(argv, argv + argc);
    if (!takeLogLevelFlag(args)) {
        editor::Out::stopLogging();
        return 2;
    }
    // the profiler's options go to the engine, whatever the mode; benchmark parses its own
    const bool benchmark = args.size() >= 2 && std::strcmp(args[1], "benchmark") == 0;
    if (!benchmark && !editor::CommandLine::takeProfilerFlags(args)) {
        editor::Out::stopLogging();
        return 2;
    }
    args.push_back(nullptr);
    const int result = runEditor(static_cast<int>(args.size()) - 1, args.data());
    editor::Out::stopLogging();
    return result;
}
