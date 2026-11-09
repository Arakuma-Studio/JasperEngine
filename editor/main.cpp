// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Backend.h"
#include "Out.h"
#include "audio/SoundLibraryCli.h"
#include "cli/CallCommand.h"
#include "cli/CommandLine.h"
#include "cli/ProceduralMaterialCommand.h"
#include "cli/ShippingCommands.h"
#include "hub/ProjectHubWindow.h"
#include "mcp/McpLaunch.h"
#include "reload/HotReloadService.h"
#include "util/EditorDiagnostics.h"
#include "util/ShellEnv.h"
#include "util/TextEncoding.h"
#include "vcs/scene/SceneCli.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace jasperengine;

namespace {

// `call`, `batch`, `list-tools`, `editors`: MCP tools from a shell (editor/cli/CallCommand.h)
bool isCallCommand(int argc, char* argv[]) {
    return argc >= 2 && (std::strcmp(argv[1], "call") == 0 || std::strcmp(argv[1], "batch") == 0 ||
                         std::strcmp(argv[1], "list-tools") == 0 || std::strcmp(argv[1], "editors") == 0);
}

int runEditor(int argc, char* argv[]) {
    // Relative path arguments are relative to the directory the editor started in: read it first
    editor::CommandLine::invocationDirectory();

    // A client of a running editor, or of one it starts headless; never the backend in this
    // process. Before the PATH bootstrap: its login-shell probe would take most of the 300 ms
    // these commands may take, and a spawned editor bootstraps its own PATH.
    if (isCallCommand(argc, argv)) {
        return editor::cli::CallCommand::run(argc - 1, argv + 1, argv[0]);
    }
    // a bake through such an editor (S90 R39)
    if (argc >= 2 && std::strcmp(argv[1], "procedural-material") == 0) {
        return editor::cli::ProceduralMaterialCommand::run(argc - 1, argv + 1, argv[0]);
    }

    // A desktop launcher starts the editor without the user's shell environment,
    // so the build tools go back on PATH before anything can spawn a command.
    editor::ShellEnv::bootstrapPath();

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
    // version control: git's scene merge driver and the semantic diff (no engine, no window)
    if (argc >= 2 && std::strcmp(argv[1], "merge-scene") == 0) {
        return editor::vcs::scene::runMergeSceneCommand(argc - 1, argv + 1, argv[0]);
    }
    if (argc >= 2 && std::strcmp(argv[1], "scene-diff") == 0) {
        return editor::vcs::scene::runSceneDiffCommand(argc - 1, argv + 1, argv[0]);
    }
    if (argc >= 2 && std::strcmp(argv[1], "assets") == 0) {
        return editor::CommandLine::runAssetsCommand(argc - 1, argv + 1, argv[0]);
    }
    if (argc >= 2 && std::strcmp(argv[1], "cook-textures") == 0) {
        return editor::CommandLine::runCookTexturesCommand(argc - 1, argv + 1, argv[0]);
    }
    // The sound tools (see editor/audio/SoundLibraryCli.h): windowless, no project
    if (argc >= 2 && std::strcmp(argv[1], "audio-analyze") == 0) {
        return editor::SoundLibraryCli::runAudioAnalyze(argc - 1, argv + 1, argv[0]);
    }
    if (argc >= 2 && std::strcmp(argv[1], "sound-library") == 0) {
        return editor::SoundLibraryCli::runSoundLibrary(argc - 1, argv + 1, argv[0]);
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
    // S30: prebuilt players, publishing, the runtime field tables
    if (argc >= 2 && std::strcmp(argv[1], "reflect") == 0) {
        return editor::ShippingCommands::runReflect(argc - 1, argv + 1, argv[0]);
    }
    if (argc >= 2 && std::strcmp(argv[1], "build") == 0) {
        return editor::ShippingCommands::runBuild(argc - 1, argv + 1, argv[0]);
    }
    if (argc >= 2 && std::strcmp(argv[1], "players") == 0) {
        return editor::ShippingCommands::runPlayers(argc - 1, argv + 1, argv[0]);
    }
    if (argc >= 2 && std::strcmp(argv[1], "publish") == 0) {
        return editor::ShippingCommands::runPublish(argc - 1, argv + 1, argv[0]);
    }
    if (argc >= 2 && std::strcmp(argv[1], "place-physics") == 0) {
        return editor::CommandLine::runPlacePhysicsCommand(argc - 1, argv + 1, argv[0]);
    }
    if (argc >= 2 && std::strcmp(argv[1], "bake-vat") == 0) {
        return editor::CommandLine::runBakeVatCommand(argc - 1, argv + 1, argv[0]);
    }
    if (argc >= 2 && std::strcmp(argv[1], "import-unity-particles") == 0) {
        return editor::CommandLine::runImportUnityParticlesCommand(argc - 1, argv + 1, argv[0]);
    }
    if (argc >= 2 && std::strcmp(argv[1], "generate-sprite-normals") == 0) {
        return editor::CommandLine::runGenerateSpriteNormalsCommand(argc - 1, argv + 1, argv[0]);
    }
    if (argc >= 2 && std::strcmp(argv[1], "aseprite") == 0) {
        return editor::CommandLine::runAsepriteCommand(argc - 1, argv + 1, argv[0]);
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
    // after `call <tool>` every flag is the tool's (a log_level parameter included)
    if (isCallCommand(static_cast<int>(args.size()), args.data())) return true;
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

// --file-watcher=auto|native|polling|off (or "--file-watcher <kind>") and the env
// JASPER_FILE_WATCHER, the flag winning (S05 R33): removed before a command reads its arguments.
// False for a value that is none of them.
bool takeFileWatcherFlag(std::vector<char*>& args) {
    WatchBackendKind kind = WatchBackendKind::Auto;
    if (const char* env = std::getenv("JASPER_FILE_WATCHER"); env && *env) {
        if (!parseWatchBackendKind(env, kind)) {
            std::fprintf(stderr, "JASPER_FILE_WATCHER must be auto, native, polling or off (got \"%s\")\n", env);
            return false;
        }
        editor::HotReloadService::setWatcherOverride(kind);
    }
    for (size_t i = 1; i < args.size();) {
        const std::string arg = args[i];
        std::string value;
        size_t consumed = 0;
        if (arg.rfind("--file-watcher=", 0) == 0) {
            value = arg.substr(15);
            consumed = 1;
        } else if (arg == "--file-watcher" && i + 1 < args.size()) {
            value = args[i + 1];
            consumed = 2;
        }
        if (consumed == 0) {
            i++;
            continue;
        }
        if (!parseWatchBackendKind(value, kind)) {
            std::fprintf(stderr, "--file-watcher must be auto, native, polling or off (got \"%s\")\n", value.c_str());
            return false;
        }
        editor::HotReloadService::setWatcherOverride(kind);
        args.erase(args.begin() + static_cast<long>(i), args.begin() + static_cast<long>(i + consumed));
    }
    return true;
}

// --allow-debug-crash, anywhere on the command line: the MCP debug_crash tool may crash or hang
// this editor (JASPERENGINE_ALLOW_DEBUG_CRASH=1 does the same); removed before the commands parse
void takeAllowDebugCrashFlag(std::vector<char*>& args) {
    if (isCallCommand(static_cast<int>(args.size()), args.data())) return;
    for (size_t i = 1; i < args.size();) {
        if (std::strcmp(args[i], "--allow-debug-crash") == 0) {
            editor::EditorDiagnostics::setDebugCrashAllowed(true);
            args.erase(args.begin() + static_cast<long>(i));
            continue;
        }
        i++;
    }
}

}

// The plain editor's --hub and --project <dir> (S27 R16), taken before any other parsing: the
// Project Hub at start whatever the preference, or that project without the hub
bool takeHubFlags(std::vector<char*>& args) {
    for (size_t i = 1; i < args.size();) {
        const std::string arg = args[i];
        if (arg == "--hub") {
            editor::hub::launchOptions().forceHub = true;
            args.erase(args.begin() + static_cast<long>(i));
            continue;
        }
        if (arg == "--project" && i == 1) {
            if (i + 1 >= args.size()) {
                std::fprintf(stderr, "--project needs a project folder\n");
                return false;
            }
            editor::hub::launchOptions().project = editor::CommandLine::projectArgPath(args[i + 1]);
            args.erase(args.begin() + static_cast<long>(i), args.begin() + static_cast<long>(i) + 2);
            continue;
        }
        // a subcommand's own flags (export --project ...) are left to it
        if (i == 1 && arg.rfind("--", 0) != 0) break;
        i++;
    }
    return true;
}

int main(int argc, char* argv[]) {
    // Windows before 10 1903 ignores the UTF-8 code page of the manifest: every narrow path would
    // be in the ANSI code page there, so the editor does not start (games still run)
    if (!editor::TextEncoding::checkWindowsCodePage()) {
        return 1;
    }
    // what the editor prints is UTF-8: a console shows it so while the editor runs
    editor::TextEncoding::useUtf8Console();
    // before anything logs: the in-memory store and the terminal until a Console window exists
    editor::Out::startLogging();
    std::vector<char*> args(argv, argv + argc);
    takeAllowDebugCrashFlag(args);
    if (!takeLogLevelFlag(args) || !takeFileWatcherFlag(args) || !takeHubFlags(args)) {
        editor::Out::stopLogging();
        return 2;
    }
    // the profiler's options go to the engine, whatever the mode; benchmark parses its own, and
    // generate-sprite-normals has a --profile of its own (the height profile: --profile rounded)
    const bool benchmark = args.size() >= 2 && std::strcmp(args[1], "benchmark") == 0;
    const bool spriteNormals = args.size() >= 2 && std::strcmp(args[1], "generate-sprite-normals") == 0;
    if (!benchmark && !spriteNormals && !editor::CommandLine::takeProfilerFlags(args)) {
        editor::Out::stopLogging();
        return 2;
    }
    args.push_back(nullptr);
    const int result = runEditor(static_cast<int>(args.size()) - 1, args.data());
    editor::Out::stopLogging();
    return result;
}
