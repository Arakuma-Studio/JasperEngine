// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Backend.h"
#include "cli/CommandLine.h"
#include "mcp/McpLaunch.h"
#include "util/ShellEnv.h"

#include <cstring>

using namespace jasperengine;

int main(int argc, char* argv[]) {
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
    if (argc >= 2 && std::strcmp(argv[1], "synthesize-texture") == 0) {
        return editor::CommandLine::runSynthesizeTextureCommand(argc - 1, argv + 1, argv[0]);
    }
    if (argc >= 2 && std::strcmp(argv[1], "place-physics") == 0) {
        return editor::CommandLine::runPlacePhysicsCommand(argc - 1, argv + 1, argv[0]);
    }
    if (argc >= 2 && std::strcmp(argv[1], "bake-vat") == 0) {
        return editor::CommandLine::runBakeVatCommand(argc - 1, argv + 1, argv[0]);
    }
    if (argc >= 2 && std::strcmp(argv[1], "generate-sprite-normals") == 0) {
        return editor::CommandLine::runGenerateSpriteNormalsCommand(argc - 1, argv + 1, argv[0]);
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
