// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CommandLine.h"

#include "AppSettings.h"
#include "JasperEngine.h"
#include "EditorHost.h"
#include "Exporter.h"
#include "Out.h"
#include "Platform.h"
#include "Project.h"
#include "Stream.h"
#include "Factory.h"
#include "Catalog.h"
#include "command/type/AddComponentCmd.h"
#include "command/type/AddEntityToPrefabCmd.h"
#include "command/type/DeleteEntityCmd.h"
#include "util/CameraTextureLink.h"
#include "util/ProjectUtils.h"
#include "texture/Texture.h"
#include "command/type/CreatePrefabCmd.h"
#include "command/type/ImportPrefabCmd.h"
#include "command/type/EntityStateCmd.h"
#include "command/type/EntityNameCmd.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/ComponentToPrefabSharedCmd.h"
#include "command/type/PrefabOverridesCmd.h"
#include "command/type/PrefabOverrideCmd.h"
#include "command/type/RemoveComponentCmd.h"
#include "command/type/PropertyCmd.h"
#include "command/type/CreateEntityCmd.h"
#include "command/CommandHandle.h"
#include "command/type/ReplacePrefabCmd.h"
#include "command/type/UnpackPrefabCmd.h"
#include "command/type/CreatePrefabVariantAssetCmd.h"
#include "command/type/DuplicateEntityCmd.h"
#include "command/type/PasteEntitiesCmd.h"
#include "util/EntityClipboard.h"
#include "util/EntityPath.h"
#include "util/ProjectChecks.h"
#include "htmlui/HtmlUiImportCli.h"
#include "texsynth/SynthesisCli.h"
#include "htmlui/effects/EffectManifest.h"
#include "htmlui/effects/EffectRegistry.h"
#include "shader/ShaderVariantCompiler.h"
#include "util/Util.h"
#include "imgui.h"
#include "command/type/RenameFileCmd.h"
#include "command/type/DeleteFileCmd.h"
#include "command/type/MoveEntityOrderCmd.h"
#include <chrono>
#include <cmath>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <thread>
#include "pool/ShaderPool.h"
#include "shader/ShaderBuilder.h"
#include "shader/ShaderLibraryMatrix.h"
#include "shader/ShaderLibraryDoc.h"
#include "shader/ShaderManifest.h"
#include "shader/ShaderVariantCompiler.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
    #define NOMINMAX
    #include <windows.h>
    #include <cstdio>
    #include <fcntl.h>
    #include <io.h>
#else
    #include <fcntl.h>
    #include <unistd.h>
#endif

namespace jasperengine::editor {

using ::jasperengine::Engine;
using ::jasperengine::ShaderBackend;
using ::jasperengine::ShaderBuildResult;
using ::jasperengine::ShaderKey;
using ::jasperengine::ShaderPool;
using ::jasperengine::ShaderType;

namespace fs = std::filesystem;

struct ExportCliOptions {
    fs::path projectPath;
    fs::path targetDir;
    fs::path assetsDir;
    fs::path luaDir;
    std::string startScene;
    std::set<ShaderBackend> backends;
    std::set<ShaderKey> shaderKeys;
    std::vector<std::string> shaderSpecs;   // as given: expanded again once the engine (and project) is up
    bool help = false;
    bool listScenes = false;
};

struct ShaderCliOptions {
    fs::path targetDir;
    std::set<ShaderBackend> backends;
    std::set<ShaderKey> shaderKeys;
    std::vector<std::string> shaderSpecs;   // as given: expanded again once the engine is up
    ShaderOutputFormat outputFormat = ShaderOutputFormat::Binary;
    // the library shaders whose compile matrix runs instead ("all" or names)
    std::vector<std::string> libraryShaders;
    // with libraryShaders: write their Markdown reference here instead of compiling them
    fs::path docFile;
    bool help = false;
};

struct EngineRuntimeGuard {
    ~EngineRuntimeGuard() {
        ShaderPool::setShaderBuilder(nullptr);
        ShaderBuilder::requestShutdown();
        Engine::systemShutdown();
    }
};

struct EditorHostOverrideGuard {
    EditorHost* previousHost = nullptr;
    explicit EditorHostOverrideGuard(EditorHost* host)
        : previousHost(&getEditorHost()) {
        setEditorHost(host);
    }
    ~EditorHostOverrideGuard() {
        setEditorHost(previousHost);
    }
};

static std::string normalizeToken(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (char ch : value) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (std::isalnum(c)) {
            result.push_back(static_cast<char>(std::tolower(c)));
        }
    }
    return result;
}

static std::vector<std::string> splitList(const std::string& value) {
    std::string normalized = value;
    for (char& ch : normalized) {
        if (ch == '+' || ch == '|' || ch == ';') {
            ch = ',';
        }
    }

    std::vector<std::string> parts;
    std::stringstream stream(normalized);
    std::string part;
    while (std::getline(stream, part, ',')) {
        size_t start = part.find_first_not_of(" \t\n\r");
        size_t end = part.find_last_not_of(" \t\n\r");
        if (start == std::string::npos) {
            continue;
        }
        parts.push_back(part.substr(start, end - start + 1));
    }
    return parts;
}

static bool parseUInt32(const std::string& value, uint32_t& out) {
    if (value.empty() || value[0] == '+' || value[0] == '-') {
        return false;
    }

    char* end = nullptr;
    unsigned long long parsed = std::strtoull(value.c_str(), &end, 0);
    if (!end || *end != '\0' || parsed > UINT32_MAX) {
        return false;
    }

    out = static_cast<uint32_t>(parsed);
    return true;
}

static bool parseShaderOutputFormat(const std::string& value, ShaderOutputFormat& out) {
    const std::string token = normalizeToken(value);
    if (token == "binary" || token == "sdat") {
        out = ShaderOutputFormat::Binary;
        return true;
    }
    if (token == "header") {
        out = ShaderOutputFormat::Header;
        return true;
    }
    if (token == "json") {
        out = ShaderOutputFormat::Json;
        return true;
    }
    return false;
}

static void addAllSupportedBackends(std::set<ShaderBackend>& backends) {
    const std::vector<ShaderBackend>& all = ShaderPool::getShaderBackends();
    backends.insert(all.begin(), all.end());
}

static bool parseBackendList(const std::string& value, std::set<ShaderBackend>& backends, std::string& error) {
    for (const std::string& name : splitList(value)) {
        if (normalizeToken(name) == "all") {
            addAllSupportedBackends(backends);
            continue;
        }

        ShaderBackend backend;
        if (!ShaderPool::parseShaderBackend(name, backend)) {
            error = "Unknown graphic backend: " + name;
            return false;
        }
        backends.insert(backend);
    }
    return true;
}

static std::string getCommandName(const char* executableName) {
    if (!executableName || !*executableName) {
        return "jasperengine-editor";
    }

    const fs::path executablePath(executableName);
    const std::string filename = executablePath.filename().string();
    return filename.empty() ? std::string("jasperengine-editor") : filename;
}

static void printMainUsage(const std::string& commandName) {
    std::cout
        << "Usage:\n"
        << "  " << commandName << "                          Open the editor.\n"
        << "  " << commandName << " export [options]         Export a project.\n"
        << "  " << commandName << " shaders [options]        Generate shaders.\n"
        << "  " << commandName << " benchmark [options]      Measure scene FPS with vsync off.\n"
        << "  " << commandName << " scene-dump [options]     Dump every scene's GameObjects (developer check).\n"
        << "  " << commandName << " check [options]          Check Lua scripts, shader forks, C++ scripts and HTML/CSS UI (compiler-style).\n"
        << "  " << commandName << " import-ui [options]      Import the remote resources of HTML UI documents.\n"
        << "  " << commandName << " synthesize-texture [options]  Synthesize a texture (tileable, inpainted, ...) from examples.\n"
        << "  " << commandName << " ui-support               Print the HTML/CSS support tables as JSON (developer: docs generation).\n"
        << "  " << commandName << " ui-effects [options]     List the HTML UI effects (--json, --validate, --project <dir>).\n"
        << "  " << commandName << " mcp [options]            Run the editor as an MCP server on stdio.\n"
        << "  " << commandName << " --mcp-port <port>        Open the editor with the MCP server on HTTP.\n\n"
        << "Run a subcommand with --help for its options.\n";
}

static void printUsage(const std::string& commandName) {
    std::cout
        << "Usage:\n"
        << "  " << commandName << " export --project <project-dir|project.yaml> --out <dir> [options]\n\n"
        << "Options:\n"
        << "  -p, --project <path>        Project directory or project.yaml file.\n"
        << "  -o, --out <path>            Destination directory. Must be empty.\n"
        << "      --assets <path>         Asset directory, relative to the project or absolute.\n"
        << "      --lua <path>            Lua directory, relative to the project or absolute.\n"
        << "      --start-scene <id|name> Start scene override.\n"
        << "      --backend <list>        opengl, opengles, d3d11, metal-macos, metal-ios, vulkan, all. Can repeat.\n"
        << "      --shader <spec>         Shader type and optional properties, e.g. mesh:Uv1,Nor. Can repeat.\n"
        << "                              UI effects: uieffect/<name>[:Flt] (Flt: the filter form), uieffect/all.\n"
        << "      --list-scenes           Print project scenes and exit.\n"
        << "  -h, --help                  Show this help.\n\n"
        << "--project and --out are absolute or relative to the current directory; --assets and --lua\n"
        << "are relative to the project (as in project.yaml).\n"
        << "If no --backend is provided, all supported backends are used.\n"
        << "If no --shader is provided, the shaders the scenes and prefabs need are exported, with the\n"
        << "shaders added to or removed from the build in the Build Settings window.\n"
        << "For standalone shader generation, use `" << commandName << " shaders`.\n";
}

static void printBenchmarkUsage(const std::string& commandName) {
    std::cout
        << "Usage:\n"
        << "  " << commandName << " benchmark --project <project-dir> --scene <scene> [options]\n\n"
        << "Options:\n"
        << "  -p, --project <path>   Project directory or project.yaml file.\n"
        << "  -s, --scene <scene>    Scene name or scene file (absolute, relative to the current directory,\n"
        << "                         else to the project). Default: the project's start scene.\n"
        << "      --frames <n>       Measured frames after warmup. Default: 180.\n"
        << "      --warmup <n>       Warmup frames after resources load. Default: 90.\n"
        << "      --mesh-lod <on|off>      Override the scene's mesh LOD setting.\n"
        << "      --depth-prepass <on|off> Override the scene's depth prepass setting.\n"
        << "      --name <label>     Label written into the JSON result.\n"
        << "      --out <path>       Write JSON to this file in addition to stdout.\n"
        << "  -h, --help             Show this help.\n\n"
        << "--project and --out are absolute or relative to the current directory.\n"
        << "Vsync is forced off. The scene camera is used for the capture.\n";
}

static void printShadersUsage(const std::string& commandName) {
    std::cout
        << "Usage:\n"
        << "  " << commandName << " shaders --out <dir> --shader <spec> [options]\n"
        << "  " << commandName << " shaders --library <name|all> [--backend <list>]\n"
        << "  " << commandName << " shaders --library <name|all> --doc <file>\n\n"
        << "Options:\n"
        << "  -o, --out <path>            Destination directory for generated shader files.\n"
        << "      --format <format>       binary, header, or json. Default: binary.\n"
        << "      --backend <list>        opengl, opengles, d3d11, metal-macos, metal-ios, vulkan, all. Can repeat.\n"
        << "      --shader <spec>         Shader type and optional properties, e.g. mesh:Uv1,Nor. Can repeat.\n"
        << "                              UI effects: uieffect/<name>[:Flt] (Flt: the filter form), uieffect/all.\n"
        << "      --library <name|all>    Compile the compile matrix of engine library shaders (library:<name>)\n"
        << "                              for each backend and validate their manifests; nothing is written.\n"
        << "                              Can repeat. Errors are printed compiler style; exit code 1 on any.\n"
        << "      --doc <file>            With --library: write the Markdown reference of those shaders\n"
        << "                              (docs/shaders/library.md), generated from their manifests, instead.\n"
        << "  -h, --help                  Show this help.\n\n"
        << "--out and --doc are absolute or relative to the current directory.\n"
        << "If no --backend is provided, all supported backends are used.\n"
        << "Shader output is written directly to <out>.\n";
}

static bool requireValue(int argc, char** argv, int& index, std::string& value, std::string& error) {
    if (index + 1 >= argc) {
        error = std::string("Missing value for ") + argv[index];
        return false;
    }
    value = argv[++index];
    return true;
}

static bool parseArgs(int argc, char** argv, ExportCliOptions& options, std::string& error) {
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        std::string value;

        if (arg == "-h" || arg == "--help") {
            options.help = true;
        } else if (arg == "-p" || arg == "--project") {
            if (!requireValue(argc, argv, i, value, error)) return false;
            options.projectPath = CommandLine::projectArgPath(value);
        } else if (arg == "-o" || arg == "--out" || arg == "--target") {
            if (!requireValue(argc, argv, i, value, error)) return false;
            options.targetDir = CommandLine::absoluteArgPath(value);
        } else if (arg == "--assets") {
            if (!requireValue(argc, argv, i, value, error)) return false;
            options.assetsDir = value;
        } else if (arg == "--lua") {
            if (!requireValue(argc, argv, i, value, error)) return false;
            options.luaDir = value;
        } else if (arg == "--start-scene") {
            if (!requireValue(argc, argv, i, value, error)) return false;
            options.startScene = value;
        } else if (arg == "--backend") {
            if (!requireValue(argc, argv, i, value, error)) return false;
            if (!parseBackendList(value, options.backends, error)) return false;
        } else if (arg == "--shader") {
            if (!requireValue(argc, argv, i, value, error)) return false;
            std::vector<ShaderKey> keys;
            if (!ShaderPool::expandShaderSpec(value, keys, &error)) {
                return false;
            }
            options.shaderKeys.insert(keys.begin(), keys.end());
            options.shaderSpecs.push_back(value);
        } else if (arg == "--list-scenes") {
            options.listScenes = true;
        } else {
            error = "Unknown argument: " + arg;
            return false;
        }
    }

    if (options.help) {
        return true;
    }
    if (options.projectPath.empty()) {
        error = "Missing required --project path.";
        return false;
    }
    if (options.targetDir.empty() && !options.listScenes) {
        error = "Missing required --out path.";
        return false;
    }
    if (options.backends.empty()) {
        addAllSupportedBackends(options.backends);
    }

    return true;
}

static bool parseShadersArgs(int argc, char** argv, ShaderCliOptions& options, std::string& error) {
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        std::string value;

        if (arg == "-h" || arg == "--help") {
            options.help = true;
        } else if (arg == "-o" || arg == "--out" || arg == "--target") {
            if (!requireValue(argc, argv, i, value, error)) return false;
            options.targetDir = CommandLine::absoluteArgPath(value);
        } else if (arg == "--format" || arg == "--output-format") {
            if (!requireValue(argc, argv, i, value, error)) return false;
            if (!parseShaderOutputFormat(value, options.outputFormat)) {
                error = "Unknown shader output format: " + value;
                return false;
            }
        } else if (arg == "--backend") {
            if (!requireValue(argc, argv, i, value, error)) return false;
            if (!parseBackendList(value, options.backends, error)) return false;
        } else if (arg == "--shader") {
            if (!requireValue(argc, argv, i, value, error)) return false;
            std::vector<ShaderKey> keys;
            if (!ShaderPool::expandShaderSpec(value, keys, &error)) {
                return false;
            }
            options.shaderKeys.insert(keys.begin(), keys.end());
            options.shaderSpecs.push_back(value);
        } else if (arg == "--library") {
            if (!requireValue(argc, argv, i, value, error)) return false;
            if (value.compare(0, 8, "library:") == 0) value = value.substr(8);
            options.libraryShaders.push_back(value);
        } else if (arg == "--doc") {
            if (!requireValue(argc, argv, i, value, error)) return false;
            options.docFile = CommandLine::absoluteArgPath(value);
        } else {
            error = "Unknown argument: " + arg;
            return false;
        }
    }

    if (options.help) {
        return true;
    }
    if (!options.docFile.empty() && options.libraryShaders.empty()) {
        error = "--doc needs --library <name|all>.";
        return false;
    }
    if (!options.libraryShaders.empty()) {
        if (options.backends.empty()) {
            addAllSupportedBackends(options.backends);
        }
        return true;
    }
    if (options.targetDir.empty()) {
        error = "Missing required --out path.";
        return false;
    }
    if (options.shaderKeys.empty()) {
        error = "Missing required --shader spec.";
        return false;
    }
    if (options.backends.empty()) {
        addAllSupportedBackends(options.backends);
    }

    return true;
}

// The keys of the --shader specs again: a spec naming a custom shader (a UI effect,
// uieffect/<name>) registers its id in ShaderPool, which the engine's start and a project's
// load reset, so the keys are made again after them.
static void expandShaderSpecsAgain(const std::vector<std::string>& specs, std::set<ShaderKey>& keys) {
    if (specs.empty())
        return;
    keys.clear();
    for (const std::string& spec : specs) {
        std::vector<ShaderKey> expanded;
        if (ShaderPool::expandShaderSpec(spec, expanded))
            keys.insert(expanded.begin(), expanded.end());
    }
}

// Path arguments are the caller's: a relative one is relative to the working directory the
// editor was started in. Each is made absolute once, while the arguments are parsed, because the
// project keeps project-relative paths (scenes, assets) and joins any relative path to its
// directory: a relative project directory was joined twice ("bad file: proj/proj/UI.scene").
const fs::path& CommandLine::invocationDirectory() {
    static const fs::path directory = [] {
        std::error_code ec;
        const fs::path current = fs::current_path(ec);
        return ec ? fs::path() : current;
    }();
    return directory;
}

fs::path CommandLine::absoluteArgPath(const fs::path& path) {
    if (path.empty()) {
        return path;
    }
    fs::path absolute = path.is_absolute() ? path : invocationDirectory() / path;
    std::error_code ec;
    const fs::path canonical = fs::weakly_canonical(absolute, ec);
    absolute = (ec ? absolute : canonical).lexically_normal();
    // "dir/" names dir: an empty last element would leave the path without a filename
    if (!absolute.has_filename() && absolute.has_relative_path()) {
        absolute = absolute.parent_path();
    }
    return absolute;
}

fs::path CommandLine::projectArgPath(const fs::path& path) {
    const fs::path absolute = absoluteArgPath(path);
    if (absolute.filename() == "project.yaml") {
        return absolute.parent_path();
    }
    return absolute;
}

static uint32_t resolveStartSceneId(const Project& project, const std::string& requested) {
    if (requested.empty()) {
        return project.getStartSceneId();
    }

    uint32_t parsedId = 0;
    if (parseUInt32(requested, parsedId)) {
        return parsedId;
    }

    const std::string requestedToken = normalizeToken(requested);
    for (const auto& scene : project.getScenes()) {
        if (normalizeToken(scene.name) == requestedToken ||
            normalizeToken(scene.filepath.stem().string()) == requestedToken ||
            normalizeToken(scene.filepath.string()) == requestedToken) {
            return scene.id;
        }
    }

    return 0;
}

static void printScenes(const Project& project) {
    for (const auto& scene : project.getScenes()) {
        std::cout << scene.id << "\t" << scene.name << "\t"
                  << scene.filepath.generic_string() << "\n";
    }
}

// On Windows the editor binary is linked with the GUI subsystem, so stdout
// and stderr are not connected to the parent console. Attaching to the parent
// console when running a CLI subcommand restores normal terminal behavior.
static void attachHostConsoleIfNeeded() {
#if defined(_WIN32)
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* stdoutFile = nullptr;
        FILE* stderrFile = nullptr;
        FILE* stdinFile = nullptr;
        (void)freopen_s(&stdoutFile, "CONOUT$", "w", stdout);
        (void)freopen_s(&stderrFile, "CONOUT$", "w", stderr);
        (void)freopen_s(&stdinFile,  "CONIN$",  "r", stdin);
    }
#endif
}

int CommandLine::runExportCommand(int argc, char** argv, const char* executableName) {
    attachHostConsoleIfNeeded();

    // Backend translation units define a static `App` instance that registers
    // itself as the active EditorHost before main(). The export subcommand must
    // override that and use the headless default host so project loading does
    // not touch GUI-only windows like ProjectWindow.
    EditorHostOverrideGuard hostOverride(nullptr);

    const std::string commandName = getCommandName(executableName);

    ExportCliOptions options;
    std::string error;
    if (!parseArgs(argc, argv, options, error)) {
        std::cerr << commandName << " export: " << error << "\n\n";
        printUsage(commandName);
        return 2;
    }

    if (options.help) {
        printUsage(commandName);
        return 0;
    }

    Out::setConsoleWindow(nullptr);
    AppSettings::initialize();

    Project project;
    auto platform = std::make_unique<Platform>(&project);
    Engine::systemInit(argc, argv, platform.get());
    EngineRuntimeGuard engineRuntime;
    Engine::pauseGameEvents(true);
    Engine::setAsyncLoading(true);

    ShaderPool::setShaderBuilder([projectPtr = &project](ShaderKey shaderKey) -> ShaderBuildResult {
        static ShaderBuilder builder;
        return builder.buildShader(shaderKey, projectPtr);
    });

    if (!project.loadProject(options.projectPath, false)) {
        return 1;
    }

    if (options.listScenes) {
        printScenes(project);
        return 0;
    }

    ExportConfig config;
    config.targetDir = options.targetDir;
    config.assetsDir = options.assetsDir.empty() ? project.getAssetsDir() : options.assetsDir;
    config.luaDir    = options.luaDir.empty()    ? project.getLuaDir()    : options.luaDir;
    config.startSceneId = resolveStartSceneId(project, options.startScene);
    config.packNativeResources = project.shouldPackNativeResources();
    config.selectedBackends   = options.backends;
    expandShaderSpecsAgain(options.shaderSpecs, options.shaderKeys);
    config.shaderKeysOverride = options.shaderKeys;
    // What the Build Settings window adds to and removes from the collected shaders, as an
    // export from the window does; an explicit --shader list is the whole set
    if (options.shaderKeys.empty()) {
        config.shaderAdditions = project.getShaderOverrides().additions;
        config.shaderExclusions = project.getShaderOverrides().exclusions;
    }

    if (!options.startScene.empty() && config.startSceneId == 0) {
        Out::warning("Start scene not found: %s", options.startScene.c_str());
    }

    Exporter exporter;
    const bool success = exporter.exportProject(&project, config);
    const ExportProgress progress = exporter.getProgress();

    if (!success) {
        if (!progress.errorMessage.empty()) {
            std::cerr << commandName << " export: " << progress.errorMessage << "\n";
        }
        return 1;
    }

    return 0;
}

// The compile matrix of library shaders (ShaderLibraryMatrix): compiler-style problems on
// stderr, a summary per shader on stdout, 1 when anything failed. With --doc, their
// Markdown reference (ShaderLibraryDoc) instead.
static int runLibraryShaderMatrix(const ShaderCliOptions& options, const std::string& commandName) {
    std::vector<std::string> names;
    for (const std::string& requested : options.libraryShaders) {
        if (requested == "all") {
            for (const std::string& name : ShaderManifests::getLibraryShaderNames()) names.push_back(name);
        } else {
            names.push_back(requested);
        }
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    if (names.empty()) {
        std::cerr << commandName << " shaders: this build has no library shader\n";
        return 1;
    }

    if (!options.docFile.empty()) {
        const std::vector<std::string> known = ShaderManifests::getLibraryShaderNames();
        for (const std::string& name : names) {
            if (std::find(known.begin(), known.end(), name) == known.end()) {
                std::cerr << commandName << " shaders: unknown library shader '" << name << "'\n";
                return 1;
            }
        }
        std::ofstream file(options.docFile, std::ios::binary | std::ios::trunc);
        file << ShaderLibraryDoc::generate(names);
        if (!file) {
            std::cerr << commandName << " shaders: could not write " << options.docFile.string() << "\n";
            return 1;
        }
        std::cout << "Wrote the reference of " << names.size() << " library shader(s) to " << options.docFile.string() << "\n";
        return 0;
    }
    if (!ShaderVariantCompiler::isAvailable()) {
        std::cerr << commandName << " shaders: this build has no shader compiler\n";
        return 1;
    }

    const std::vector<ShaderBackend> backends(options.backends.begin(), options.backends.end());
    bool failed = false;
    size_t totalCompiled = 0;
    for (const std::string& name : names) {
        const ShaderLibraryMatrix::Result result = ShaderLibraryMatrix::run(name, backends);
        bool errors = result.failed > 0;
        for (const ShaderLibraryMatrix::Problem& problem : result.problems) {
            std::cerr << problem.file << ":" << problem.line << ": " << (problem.error ? "error" : "warning") << ": " << problem.message << "\n";
            errors = errors || problem.error;
        }
        totalCompiled += result.compiled;
        std::cout << result.shader << ": " << result.variants << " variants x " << backends.size() << " backends, "
                  << result.compiled << " compiled, " << result.failed << " failed" << (errors ? " FAILED" : " OK") << "\n";
        failed = failed || errors;
    }
    std::cout << names.size() << " library shader(s), " << totalCompiled << " compiles: " << (failed ? "FAILED" : "OK") << "\n";
    return failed ? 1 : 0;
}

int CommandLine::runShadersCommand(int argc, char** argv, const char* executableName) {
    attachHostConsoleIfNeeded();

    EditorHostOverrideGuard hostOverride(nullptr);

    const std::string commandName = getCommandName(executableName);

    ShaderCliOptions options;
    std::string error;
    if (!parseShadersArgs(argc, argv, options, error)) {
        std::cerr << commandName << " shaders: " << error << "\n\n";
        printShadersUsage(commandName);
        return 2;
    }

    if (options.help) {
        printShadersUsage(commandName);
        return 0;
    }

    Out::setConsoleWindow(nullptr);

    Project project;
    auto platform = std::make_unique<Platform>(&project);
    Engine::systemInit(argc, argv, platform.get());
    EngineRuntimeGuard engineRuntime;
    Engine::pauseGameEvents(true);
    Engine::setAsyncLoading(true);

    expandShaderSpecsAgain(options.shaderSpecs, options.shaderKeys);

    if (!options.libraryShaders.empty()) {
        return runLibraryShaderMatrix(options, commandName);
    }

    ExportConfig config;
    config.targetDir = options.targetDir;
    config.shaderOutputFormat = options.outputFormat;
    config.selectedBackends = options.backends;
    config.selectedShaderKeys = options.shaderKeys;

    Exporter exporter;
    const bool success = exporter.generateShaders(config);
    const ExportProgress progress = exporter.getProgress();

    if (!success) {
        if (!progress.errorMessage.empty()) {
            std::cerr << commandName << " shaders: " << progress.errorMessage << "\n";
        }
        return 1;
    }

    return 0;
}

int CommandLine::runBenchmarkHelp(const char* executableName) {
    attachHostConsoleIfNeeded();
    printBenchmarkUsage(getCommandName(executableName));
    return 0;
}


// ---- scene-dump: a developer check of what loading a project gives ----
//
// Loads every scene of a project headless and writes, per scene, every GameObject by its
// hierarchy path (names, and an index among same-named siblings): name, state, parent and
// components as Stream encodes them, the entity references by target path, and its prefab
// link. Two dumps of the same project (an older and a newer editor, or before and after a
// save) compare GameObject by GameObject. --save saves every scene (and so the modified
// prefabs) after the dump.
static void writeSceneDump(Project& project, const fs::path& outPath) {
    YAML::Emitter out;
    out << YAML::BeginMap << YAML::Key << "scenes" << YAML::Value << YAML::BeginSeq;
    // the scenes, then the open prefab stages (Prefab Mode), named after their asset
    for (SceneProject* sceneProjectPtr : project.getScenesAndPrefabStages()) {
        SceneProject& sceneProject = *sceneProjectPtr;
        if (!sceneProject.scene) continue;
        Scene* scene = sceneProject.scene;
        std::map<Entity, std::string> paths;
        std::map<std::string, YAML::Node> nodes;
        for (Entity entity : sceneProject.entities) {
            if (!scene->isEntityCreated(entity)) continue;
            YAML::Node node;
            node["id"] = entity;
            node["name"] = scene->getEntityName(entity);
            if (!scene->isEntityActive(entity)) node["active"] = false;
            if (scene->getEntityTag(entity) != DEFAULT_ENTITY_TAG) node["tag"] = scene->getEntityTag(entity);
            if (scene->getEntityLayer(entity) != 0) node["layer"] = scene->getEntityLayer(entity);
            Entity parent = NULL_ENTITY;
            if (Transform* transform = scene->findComponent<Transform>(entity)) parent = transform->parent;
            node["parent"] = parent != NULL_ENTITY ? EntityPath::of(scene, parent, &paths) : std::string();
            node["components"] = Stream::encodeComponents(entity, scene, scene->getSignature(entity));
            for (ComponentType type : Catalog::findComponents(scene, entity)) {
                for (auto& [name, property] : Catalog::findEntityProperties(scene, entity, type)) {
                    if (!property.ref) continue;
                    Entity target = NULL_ENTITY;
                    if (property.type == PropertyType::Entity) target = *static_cast<Entity*>(property.ref);
                    else if (property.type == PropertyType::EntityReference) target = static_cast<EntityReference*>(property.ref)->entity;
                    else if (property.type == PropertyType::Texture) target = CameraTextureLink::parse(static_cast<Texture*>(property.ref)->getId());
                    else continue;
                    if (target == NULL_ENTITY) continue;
                    node["refs"][Catalog::getComponentName(type, true) + "." + name] =
                        scene->isEntityCreated(target) ? EntityPath::of(scene, target, &paths) : "<missing " + std::to_string(target) + ">";
                }
            }
            std::string prefab = project.findPrefabPathFor(sceneProject.id, entity).generic_string();
            if (!prefab.empty()) node["prefab"] = prefab;
            if (project.isMissingPrefab(sceneProject.id, entity)) node["missingPrefab"] = true;
            // what the owning instance level overrides of it
            const Prefab::ObjectOverrides overrides = project.getPrefabObjectOverrides(sceneProject.id, entity);
            if (!overrides.empty()) {
                YAML::Node o;
                for (const auto& [type, names] : overrides.properties) {
                    for (const std::string& name : names) o["properties"][Catalog::getComponentName(type, true)].push_back(name);
                }
                if (overrides.state) o["state"] = static_cast<int>(overrides.state);
                for (ComponentType type : overrides.addedComponents) o["added"].push_back(Catalog::getComponentName(type, true));
                for (ComponentType type : overrides.removedComponents) o["removed"].push_back(Catalog::getComponentName(type, true));
                node["overrides"] = o;
            }
            std::string path = EntityPath::of(scene, entity, &paths);
            if (nodes.count(path)) path += "@" + std::to_string(entity);
            nodes[path] = node;
        }
        const PrefabStage* stage = project.getPrefabStage(sceneProject.id);
        out << YAML::BeginMap << YAML::Key << "name" << YAML::Value
            << (stage ? "Prefab Mode: " + stage->assetPath.generic_string() : sceneProject.name);
        if (stage) {
            out << YAML::Key << "modified" << YAML::Value << project.hasUnsavedPrefabStage(sceneProject.id);
            if (stage->inContext()) {
                const SceneProject* context = project.getScene(stage->contextSceneId);
                out << YAML::Key << "context" << YAML::Value << (context ? context->name : std::string("?"));
            }
        }
        out << YAML::Key << "entities" << YAML::Value << YAML::BeginMap;
        for (auto& [path, node] : nodes) out << YAML::Key << path << YAML::Value << node;
        out << YAML::EndMap << YAML::EndMap;
    }
    out << YAML::EndSeq << YAML::EndMap;

    if (outPath.empty()) {
        std::cout << out.c_str() << "\n";
    } else {
        std::ofstream file(outPath);
        file << out.c_str() << "\n";
    }
}

// The GameObject of the first loaded scene (or `scene`) at a dump path
static Entity findDumpEntity(Project& project, const YAML::Node& op, SceneProject*& sceneOut) {
    const std::string sceneName = op["scene"] ? op["scene"].as<std::string>() : std::string();
    const std::string path = op["entity"] ? op["entity"].as<std::string>() : std::string();
    // stage: true looks in the shown prefab stage (Prefab Mode) only
    const bool inStage = op["stage"] && op["stage"].as<bool>();
    std::vector<SceneProject*> candidates;
    if (inStage) {
        if (PrefabStage* stage = project.getActivePrefabStage()) candidates.push_back(&stage->sceneProject);
    } else {
        for (SceneProject& sceneProject : project.getScenes()) candidates.push_back(&sceneProject);
    }
    for (SceneProject* candidate : candidates) {
        SceneProject& sceneProject = *candidate;
        if (!sceneProject.scene || (!inStage && !sceneName.empty() && sceneProject.name != sceneName)) continue;
        const Entity entity = EntityPath::find(sceneProject, path);
        if (entity != NULL_ENTITY) {
            sceneOut = &sceneProject;
            return entity;
        }
    }
    std::cerr << "scene-dump: no GameObject at " << path << "\n";
    return NULL_ENTITY;
}

// --ops <file>: a list of operations run on the loaded project before the dump, to check
// the prefab APIs headless. Each is a map with `op` and its arguments (`entity` is a dump
// path; `component` a Stream component key):
//   save | dump {out} | refresh {prefab, reload} | copy {from, to} / remove {path} (outside edits) |
//   refreshAssets (the asset database refresh) | undo (the last command) | redo |
//   set {entity, also, component, property, value}  (the Inspector's edit: a PropertyCmd per
//       GameObject, `also` more dump paths for a multi-selection, one undo step) |
//   setName {entity, name}  (EntityNameCmd) | delete {entity}  (DeleteEntityCmd) |
//   setState {entity, active|tag|layer}  (EntityStateCmd's path) |
//   revertComponent {entity, component}  (the component header's Revert) |
//   revertAll / applyAll {entity: an instance root}  (the Overrides dropdown's) |
//   playStart / playStop {scene}  (Play's snapshot taken / Stop's restore, without running) |
//   canApply {entity, component, properties, target}  (prints whether Apply is allowed) |
//   addComponent / removeComponent {entity, component}  (AddComponentCmd / RemoveComponentCmd) |
//   createPrefab {entity, path} (CreatePrefabCmd) | instantiate {entity, prefab, below} (ImportPrefabCmd) |
//   createPrefabs {entities, dir} (a multi-selection dragged into the Project window: one prefab
//       per root, from the Hierarchy's payload) | addToPrefab {entity, parent, target} (AddEntityToPrefabCmd) |
//   addObject {entity (the parent, may be empty), name} (a plain child GameObject with a Transform) |
//   setRef {entity, component, property, target (a dump path or empty)} (an override on an
//       instance's GameObject) | addLocalComponent / removeLocalComponent {entity, component} (the
//       GameObject only, for record*Component) | bake {out} (each prefab's generated source) |
//   replace {entity, prefab, match: name|hierarchy|none, keepOverrides, renameRoot} (ReplacePrefabCmd) |
//   unpackMissing {entity} (UnpackMissingPrefabCmd) | preview {entity, prefab} (a thumbnail's instance, listed) |
//   move {entity, target, insertion: before|after|into} (MoveEntityOrderCmd, a Hierarchy drag) |
//   usage (the Prefabs dialog's usage scan, listed) | close {scene} (unloads it: usage reads its file) |
//   record|revert|apply Property|State|AddedComponent|RemovedComponent
//       {entity, component, properties, state, target}  (PrefabOverrideCmd) |
//   Prefab Mode: openPrefab {prefab} (isolation) or {entity, [prefab], [inContext]} (from an
//       instance) | closePrefab {depth, choice: ask|save|discard|cancel} | savePrefab |
//       autoSave {value} | tickPrefab {wait, editing} (the per-frame upkeep: Auto Save) |
//       undoStage / redoStage | setCmd {entity, component, property, value: Vector3},
//       createChild {entity, name}, deleteEntity {entity} (through the undo history),
//       create {scene | stage: true, name, kind: empty|box|sphere|light|sun|text|image|ui_document, position};
//       `stage: true` makes `entity` a path in the shown stage
// "@/x" in an op's path: x next to the ops file
static fs::path sceneDumpOpsDir;
static fs::path resolveOpsPath(const std::string& path) {
    if (path.rfind("@/", 0) == 0) {
        return sceneDumpOpsDir / path.substr(2);
    }
    return path;
}

// The GameObject menu entry an op's `kind` names (create, createChild)
static EntityCreationType creationKind(const YAML::Node& op) {
    static const std::map<std::string, EntityCreationType> kinds = {
        {"empty", EntityCreationType::GAMEOBJECT}, {"box", EntityCreationType::BOX},
        {"sphere", EntityCreationType::SPHERE}, {"light", EntityCreationType::POINT_LIGHT},
        {"sun", EntityCreationType::DIRECTIONAL_LIGHT}, {"text", EntityCreationType::TEXT},
        {"image", EntityCreationType::IMAGE}, {"ui_document", EntityCreationType::UI_DOCUMENT},
    };
    const std::string kind = op["kind"] ? op["kind"].as<std::string>() : std::string("empty");
    auto it = kinds.find(kind);
    return it != kinds.end() ? it->second : EntityCreationType::GAMEOBJECT;
}

static bool runSceneDumpOps(Project& project, const YAML::Node& ops) {
    std::vector<std::unique_ptr<Command>> done;
    std::vector<std::unique_ptr<Command>> undone;
    size_t playFloor = 0;
    static const std::map<std::string, PrefabOverrideCmd::Operation> operations = {
        {"recordProperty", PrefabOverrideCmd::Operation::RecordProperty}, {"revertProperty", PrefabOverrideCmd::Operation::RevertProperty},
        {"applyProperty", PrefabOverrideCmd::Operation::ApplyProperty}, {"recordState", PrefabOverrideCmd::Operation::RecordState},
        {"revertState", PrefabOverrideCmd::Operation::RevertState}, {"applyState", PrefabOverrideCmd::Operation::ApplyState},
        {"recordAddedComponent", PrefabOverrideCmd::Operation::RecordAddedComponent}, {"revertAddedComponent", PrefabOverrideCmd::Operation::RevertAddedComponent},
        {"applyAddedComponent", PrefabOverrideCmd::Operation::ApplyAddedComponent}, {"recordRemovedComponent", PrefabOverrideCmd::Operation::RecordRemovedComponent},
        {"revertRemovedComponent", PrefabOverrideCmd::Operation::RevertRemovedComponent}, {"applyRemovedComponent", PrefabOverrideCmd::Operation::ApplyRemovedComponent},
    };
    for (const YAML::Node& op : ops) {
        const std::string name = op["op"].as<std::string>();
        std::cout << "op " << name << (op["entity"] ? " " + op["entity"].as<std::string>() : std::string()) << "\n";
        auto runCommand = [&](Command* command) {
            std::unique_ptr<Command> owned(command);
            if (!owned->execute()) {
                std::cout << "  (failed)\n";
                return;
            }
            done.push_back(std::move(owned));
            undone.clear();
        };
        if (name == "save") {
            for (SceneProject& sceneProject : project.getScenes()) {
                if (sceneProject.scene && !sceneProject.filepath.empty()) {
                    project.saveSceneToPath(sceneProject.id, sceneProject.filepath, false);
                }
            }
            continue;
        }
        if (name == "dump") {
            writeSceneDump(project, resolveOpsPath(op["out"].as<std::string>()));
            continue;
        }
        if (name == "copy") {
            // an edit outside the editor: a file of the project replaced
            std::error_code ec;
            fs::copy_file(project.getProjectPath() / op["from"].as<std::string>(), project.getProjectPath() / op["to"].as<std::string>(),
                fs::copy_options::overwrite_existing, ec);
            continue;
        }
        if (name == "remove") {
            std::error_code ec;
            fs::remove(project.getProjectPath() / op["path"].as<std::string>(), ec);
            continue;
        }
        if (name == "refreshAssets") {
            project.refreshAssetDatabase(true);
            continue;
        }
        if (name == "playStart" || name == "playStop") {
            // Play's snapshot of the scene (Project::runPlayStartup) and Stop's restore
            // (finalizeStop), the game itself not run
            for (SceneProject& played : project.getScenes()) {
                if (!played.scene || (op["scene"] && played.name != op["scene"].as<std::string>())) continue;
                if (name == "playStart") {
                    played.playStateSnapshot = Stream::encodeSceneProject(nullptr, &played);
                    played.playPrefabState = project.capturePlayPrefabState(&played);
                    const std::vector<Entity> alive = played.scene->getEntityList();
                    played.playStateEntities = std::set<Entity>(alive.begin(), alive.end());
                    played.playStateModified = played.isModified;
                    played.playState = ScenePlayState::PLAYING;
                } else {
                    played.isModified = played.playStateModified;
                    project.restorePlaySnapshot(&played);
                    played.playState = ScenePlayState::STOPPED;
                }
                break;
            }
            // Stop drops what was done while playing from the history
            if (name == "playStart") {
                playFloor = done.size();
            } else {
                while (done.size() > playFloor) done.pop_back();
                undone.clear();
            }
            continue;
        }
        if (name == "close") {
            // a scene closed (unloaded): what reads closed scenes from their file sees it (F)
            for (SceneProject& sceneProject : project.getScenes()) {
                if (sceneProject.scene && (!op["scene"] || sceneProject.name == op["scene"].as<std::string>())) {
                    project.closeScene(sceneProject.id, true);
                    break;
                }
            }
            continue;
        }
        if (name == "usage") {
            // the Prefabs dialog's usage scan (F)
            for (const auto& [key, usage] : project.collectPrefabUsage()) {
                std::cout << "  usage " << usage.path.generic_string() << " guid " << (usage.guid.empty() ? "-" : usage.guid) << ":";
                for (const PrefabSceneUsage& scene : usage.scenes) {
                    std::cout << " [" << scene.sceneName << (scene.loaded ? "" : " closed") << " instances " << scene.instances
                              << " nested " << scene.nested << " missing " << scene.missing << "]";
                }
                for (const fs::path& path : usage.nestedIn) std::cout << " in " << path.generic_string();
                if (!usage.base.empty()) std::cout << " base " << usage.base.generic_string() << (usage.baseMissing ? " (missing)" : "");
                for (const fs::path& path : usage.variants) std::cout << " variant " << path.generic_string();
                std::cout << "\n";
            }
            continue;
        }
        if (name == "refresh") {
            project.refreshPrefabInstances(op["prefab"].as<std::string>(), op["reload"] && op["reload"].as<bool>());
            continue;
        }
        if (name == "undo") {
            if (!done.empty()) {
                done.back()->undo();
                undone.push_back(std::move(done.back()));
                done.pop_back();
            }
            continue;
        }
        if (name == "redo") {
            if (!undone.empty()) {
                undone.back()->execute();
                done.push_back(std::move(undone.back()));
                undone.pop_back();
            }
            continue;
        }
        if (name == "bake") {
            // what the export generates for each prefab (build_prefab_*), from its baked instance
            const fs::path out = resolveOpsPath(op["out"].as<std::string>());
            fs::create_directories(out);
            for (const auto& [path, baked] : project.bakePrefabsForExport()) {
                std::ofstream file(out / (Factory::prefabToFileName(path) + ".cpp"));
                file << Factory::createPrefab(path, *baked, project.getProjectPath(), out,
                    [&project](const fs::path& p) { return project.getPrefabGuid(p); });
            }
            continue;
        }
        if (name == "spawn") {
            // what a script's createPrefab does in Play (the editor's PrefabManager factories)
            SceneProject* first = nullptr;
            for (SceneProject& candidate : project.getScenes()) {
                if (candidate.scene) { first = &candidate; break; }
            }
            if (!first) return false;
            // (Project::registerPrefabManager's factory: a bare root, the import onto it)
            const fs::path prefabPath = op["prefab"].as<std::string>();
            const Entity spawned = first->scene->createUserEntity();
            PrefabComponent link;
            link.name = prefabPath.stem().string();
            link.path = prefabPath.string();
            link.guid = project.getPrefabGuid(prefabPath);
            first->scene->addComponent<PrefabComponent>(spawned, link);
            first->entities.push_back(spawned);
            if (op["name"]) {
                first->scene->setEntityName(spawned, op["name"].as<std::string>());
            }
            project.importPrefab(first, &first->entities, prefabPath, spawned, false);
            continue;
        }
        if (name == "renameFile" || name == "deleteFile") {
            // the Project window's Rename (same folder) and Delete (to the trash, undoable)
            if (name == "renameFile") {
                const fs::path from = op["from"].as<std::string>();
                runCommand(new RenameFileCmd(&project, from.filename().string(), fs::path(op["to"].as<std::string>()).filename().string(),
                    (project.getProjectPath() / from.parent_path()).string()));
            } else {
                runCommand(new DeleteFileCmd(&project, {project.getProjectPath() / op["path"].as<std::string>()}, project.getProjectPath()));
            }
            continue;
        }
        if (name == "duplicate") {
            SceneProject* owner = nullptr;
            const Entity target = findDumpEntity(project, op, owner);
            if (target == NULL_ENTITY) return false;
            runCommand(new DuplicateEntityCmd(&project, owner->id, {target}));
            continue;
        }
        if (name == "copyPaste") {
            // Edit > Copy, then Paste at the scene's root (the clipboard's parsed copy)
            SceneProject* owner = nullptr;
            const Entity target = findDumpEntity(project, op, owner);
            if (target == NULL_ENTITY) return false;
            // headless: an ImGui context of its own holds the clipboard text in memory
            ImGuiContext* previous = ImGui::GetCurrentContext();
            ImGuiContext* context = previous ? nullptr : ImGui::CreateContext();
            std::shared_ptr<const GameObjectsClip> clip;
            if (EntityClipboard::copyGameObjects(&project, owner->id, {target})) {
                EntityClipboard::finishPendingWrite();
                clip = EntityClipboard::readGameObjects(&project);
            }
            if (clip) {
                runCommand(new PasteEntitiesCmd(&project, owner->id, clip, EntityClipboard::Placement::Root, NULL_ENTITY));
            } else {
                std::cout << "  (failed)\n";
            }
            if (context) {
                ImGui::DestroyContext(context);
            }
            continue;
        }
        if (name == "createVariantAsset") {
            // Project window, Create > Prefab Variant on a prefab (E)
            CreatePrefabVariantAssetCmd command(&project, op["prefab"].as<std::string>(), op["path"].as<std::string>());
            if (!command.execute()) {
                std::cout << "  (failed)\n";
            }
            continue;
        }
        if (name == "variantInfo") {
            // the variant chain of a prefab, its direct variants (E)
            const fs::path prefab = op["prefab"].as<std::string>();
            std::cout << "  chain " << prefab.generic_string() << ":";
            for (const fs::path& path : project.getPrefabVariantChain(prefab)) std::cout << " " << path.generic_string();
            std::cout << "\n  fileBase " << project.getPrefabFileBase(prefab).generic_string() << "\n  variants:";
            for (const fs::path& path : project.findPrefabVariantsOf(prefab)) std::cout << " " << path.generic_string();
            std::cout << "\n";
            continue;
        }
        if (name == "addObject" && (!op["entity"] || op["entity"].as<std::string>().empty())) {
            SceneProject* first = nullptr;
            for (SceneProject& candidate : project.getScenes()) {
                if (candidate.scene) { first = &candidate; break; }
            }
            if (!first) return false;
            const Entity created = first->scene->createUserEntity();
            first->scene->setEntityName(created, op["name"].as<std::string>());
            first->scene->addComponent<Transform>(created, {});
            first->entities.push_back(created);
            first->isModified = true;
            continue;
        }
        // ---- Prefab Mode ----
        if (name == "openPrefab" && !op["entity"]) {
            // from the Project window: in isolation
            if (!project.openPrefabStage(op["prefab"].as<std::string>())) {
                std::cout << "  (failed)\n";
            }
            continue;
        }
        if (name == "closePrefab") {
            // choice: save | discard | cancel (the exit dialog's buttons), ask (the dialog;
            // headless it saves)
            static const std::map<std::string, int> choices = {{"ask", 0}, {"save", 1}, {"discard", 2}, {"cancel", 3}};
            const std::string choice = op["choice"] ? op["choice"].as<std::string>() : std::string("ask");
            const size_t depth = op["depth"] ? op["depth"].as<size_t>() : 0;
            project.closePrefabStages(depth, [](bool closed) {
                std::cout << "  " << (closed ? "closed" : "cancelled") << "\n";
            }, choices.count(choice) ? choices.at(choice) : 0);
            continue;
        }
        if (name == "savePrefab") {
            PrefabStage* stage = project.getActivePrefabStage();
            std::cout << "  " << (stage && project.savePrefabStage(stage->sceneProject.id) ? "saved" : "(failed)") << "\n";
            continue;
        }
        if (name == "autoSave") {
            project.setPrefabAutoSave(op["value"].as<bool>());
            continue;
        }
        if (name == "tickPrefab") {
            // frames of Prefab Mode's upkeep (Auto Save, outside changes) after `wait` seconds
            if (op["wait"]) {
                std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(op["wait"].as<float>() * 1000.0f)));
            }
            project.updatePrefabStages(op["editing"] && op["editing"].as<bool>());
            continue;
        }
        if (name == "create") {
            // a GameObject at the top of `scene` (or of the shown stage), as the GameObject menu
            // makes it: kind empty|box|sphere|light, at `position`
            SceneProject* target = nullptr;
            if (op["stage"] && op["stage"].as<bool>()) {
                PrefabStage* stage = project.getActivePrefabStage();
                target = stage ? &stage->sceneProject : nullptr;
            } else {
                for (SceneProject& candidate : project.getScenes()) {
                    if (candidate.scene && (!op["scene"] || candidate.name == op["scene"].as<std::string>())) {
                        target = &candidate;
                        break;
                    }
                }
            }
            if (!target) {
                std::cerr << "scene-dump: no scene for create\n";
                return false;
            }
            CreateEntityCmd* command = new CreateEntityCmd(&project, target->id, op["name"].as<std::string>(), creationKind(op));
            if (op["position"]) {
                const YAML::Node p = op["position"];
                command->addProperty<Vector3>(ComponentType::Transform, "position", Vector3(p[0].as<float>(), p[1].as<float>(), p[2].as<float>()));
            }
            CommandHandle::get(target->id)->addCommandNoMerge(command);
            continue;
        }
        if (name == "undoScene" || name == "redoScene") {
            // the undo history of a scene (`scene`), as Ctrl+Z / Ctrl+Y in it
            for (SceneProject& candidate : project.getScenes()) {
                if (candidate.scene && op["scene"] && candidate.name == op["scene"].as<std::string>()) {
                    CommandHistory* history = CommandHandle::get(candidate.id);
                    if (name == "undoScene") history->undo(); else history->redo();
                }
            }
            continue;
        }
        if (name == "undoStage" || name == "redoStage") {
            if (PrefabStage* stage = project.getActivePrefabStage()) {
                CommandHistory* history = CommandHandle::get(stage->sceneProject.id);
                if (name == "undoStage") history->undo(); else history->redo();
                project.notePrefabStageChange(stage->sceneProject.id);  // the editor's onSceneModified
            }
            continue;
        }

        SceneProject* sceneProject = nullptr;
        const Entity entity = findDumpEntity(project, op, sceneProject);
        if (entity == NULL_ENTITY) {
            return false;
        }
        if (name == "addObject") {
            // a plain GameObject below one (an added GameObject when that is a prefab's)
            Scene* targetScene = sceneProject->scene;
            const Entity created = targetScene->createUserEntity();
            targetScene->setEntityName(created, op["name"].as<std::string>());
            targetScene->addComponent<Transform>(created, {});
            targetScene->addEntityChild(entity, created, false);
            sceneProject->entities.push_back(created);
            ProjectUtils::sortEntitiesByTransformOrder(targetScene, sceneProject->entities);
            sceneProject->isModified = true;
            continue;
        }
        if (name == "openPrefab") {
            // from an instance (Hierarchy / Inspector Open): in context unless inContext: false
            const std::string prefab = op["prefab"] ? op["prefab"].as<std::string>() : project.findPrefabPathFor(sceneProject->id, entity).generic_string();
            const bool inContext = !op["inContext"] || op["inContext"].as<bool>();
            if (!project.openPrefabStage(prefab, sceneProject->id, entity, inContext)) {
                std::cout << "  (failed)\n";
            }
            continue;
        }
        if (name == "setCmd" || name == "createChild" || name == "deleteEntity") {
            // through the scene's (or stage's) undo history, as the Inspector and Hierarchy do
            Command* command = nullptr;
            if (name == "setCmd") {
                const YAML::Node value = op["value"];
                const ComponentType componentType = op["component"] ? Catalog::getComponentType(op["component"].as<std::string>()) : ComponentType::Transform;
                command = new PropertyCmd<Vector3>(&project, sceneProject->id, entity, componentType, op["property"].as<std::string>(),
                    Vector3(value[0].as<float>(), value[1].as<float>(), value[2].as<float>()));
            } else if (name == "createChild") {
                CreateEntityCmd* create = new CreateEntityCmd(&project, sceneProject->id, op["name"] ? op["name"].as<std::string>() : std::string("GameObject"),
                    creationKind(op), entity);
                if (op["position"]) {
                    const YAML::Node p = op["position"];
                    create->addProperty<Vector3>(ComponentType::Transform, "position", Vector3(p[0].as<float>(), p[1].as<float>(), p[2].as<float>()));
                }
                command = create;
            } else {
                command = new DeleteEntityCmd(&project, sceneProject->id, entity);
            }
            CommandHandle::get(sceneProject->id)->addCommandNoMerge(command);
            project.notePrefabStageChange(sceneProject->id);
            continue;
        }
        Scene* scene = sceneProject->scene;
        const ComponentType type = op["component"] ? Catalog::getComponentType(op["component"].as<std::string>()) : ComponentType::Transform;
        std::vector<std::string> properties;
        if (op["properties"]) {
            for (const YAML::Node& property : op["properties"]) properties.push_back(property.as<std::string>());
        }
        if (name == "set") {
            const std::string property = op["property"].as<std::string>();
            PropertyData data = Catalog::findProperty(scene, entity, type, property);
            if (!data.ref) {
                std::cerr << "scene-dump: no property " << property << "\n";
                return false;
            }
            std::vector<Entity> targets{entity};
            if (op["also"]) {
                for (const YAML::Node& path : op["also"]) {
                    YAML::Node other = YAML::Clone(op);
                    other["entity"] = path;
                    SceneProject* otherScene = nullptr;
                    const Entity target = findDumpEntity(project, other, otherScene);
                    if (target == NULL_ENTITY || otherScene != sceneProject) {
                        return false;
                    }
                    targets.push_back(target);
                }
            }
            const YAML::Node value = op["value"];
            MultiPropertyCmd* command = new MultiPropertyCmd();
            for (Entity target : targets) {
                const uint32_t id = sceneProject->id;
                switch (data.type) {
                    case PropertyType::Float: command->addPropertyCmd<float>(&project, id, target, type, property, value.as<float>()); break;
                    case PropertyType::Bool: command->addPropertyCmd<bool>(&project, id, target, type, property, value.as<bool>()); break;
                    case PropertyType::Int: command->addPropertyCmd<int>(&project, id, target, type, property, value.as<int>()); break;
                    case PropertyType::UInt: command->addPropertyCmd<unsigned int>(&project, id, target, type, property, value.as<unsigned int>()); break;
                    case PropertyType::String: command->addPropertyCmd<std::string>(&project, id, target, type, property, value.as<std::string>()); break;
                    case PropertyType::Vector3: command->addPropertyCmd<Vector3>(&project, id, target, type, property,
                        Vector3(value[0].as<float>(), value[1].as<float>(), value[2].as<float>())); break;
                    case PropertyType::Vector4: command->addPropertyCmd<Vector4>(&project, id, target, type, property,
                        Vector4(value[0].as<float>(), value[1].as<float>(), value[2].as<float>(), value[3].as<float>())); break;
                    default: std::cerr << "scene-dump: unsupported property type\n"; delete command; return false;
                }
            }
            runCommand(command);
            continue;
        }
        if (name == "setName") {
            runCommand(new EntityNameCmd(&project, sceneProject->id, entity, op["name"].as<std::string>()));
            continue;
        }
        if (name == "delete") {
            runCommand(new DeleteEntityCmd(&project, sceneProject->id, entity));
            continue;
        }
        if (name == "revertAll") {
            runCommand(PrefabOverridesCmd::revertAll(&project, sceneProject->id, entity));
            continue;
        }
        if (name == "applyAll") {
            PrefabOverridesCmd* command = PrefabOverridesCmd::applyAll(&project, sceneProject->id, entity);
            if (command) {
                runCommand(command);
            } else {
                std::cout << "  (failed)\n";
            }
            continue;
        }
        if (name == "revertComponent") {
            runCommand(new ComponentToPrefabSharedCmd(&project, sceneProject->id, entity, type));
            continue;
        }
        if (name == "canApply") {
            std::string reason;
            const bool can = project.canApplyPrefabOverride(sceneProject->id, entity, type, properties,
                op["target"] ? fs::path(op["target"].as<std::string>()) : fs::path(), &reason);
            std::cout << "  canApply: " << (can ? "yes" : "no") << (reason.empty() ? std::string() : " (" + reason + ")") << "\n";
            std::cout << "  targets:";
            for (const fs::path& target : project.getPrefabApplyTargets(sceneProject->id, entity)) {
                std::cout << " " << target.generic_string();
            }
            std::cout << "\n";
            continue;
        }
        if (name == "setState") {
            if (op["active"]) runCommand(EntityStateCmd::active(&project, sceneProject->id, {entity}, op["active"].as<bool>()));
            if (op["tag"]) runCommand(EntityStateCmd::tag(&project, sceneProject->id, {entity}, op["tag"].as<std::string>()));
            if (op["layer"]) runCommand(EntityStateCmd::layer(&project, sceneProject->id, {entity}, op["layer"].as<int>()));
            continue;
        }
        if (name == "createPrefab") {
            // as the Hierarchy's Create Prefab: the GameObject (with its project context)
            runCommand(new CreatePrefabCmd(&project, sceneProject->id, op["path"].as<std::string>(),
                Stream::encodeEntity(entity, scene, &project, sceneProject)));
            continue;
        }
        if (name == "createVariant") {
            // Hierarchy > Prefab > Create Variant, or an instance root dragged into the Project
            // window answered "Prefab Variant" (CreatePrefabCmd with variants)
            runCommand(new CreatePrefabCmd(&project, sceneProject->id, {{op["path"].as<std::string>(),
                Stream::encodeEntity(entity, scene, &project, sceneProject)}}, true));
            continue;
        }
        if (name == "unpack") {
            // Prefab > Unpack (completely: Unpack Completely)
            runCommand(new UnpackPrefabCmd(&project, sceneProject->id, {entity}, op["completely"] && op["completely"].as<bool>()));
            continue;
        }
        if (name == "applyRemovals" || name == "revertRemovals") {
            // the removed GameObjects of the instance rooted here (and nested in it), applied
            // to `target` (empty: each to its own prefab) or reverted
            std::vector<std::unique_ptr<Command>> steps;
            const fs::path target = op["target"] ? fs::path(op["target"].as<std::string>()) : fs::path();
            for (const PrefabRemovalInfo& removal : project.getPrefabRemovals(sceneProject->id, entity)) {
                if (name == "applyRemovals") {
                    steps.emplace_back(new ApplyPrefabRemovalCmd(&project, sceneProject->id, removal.path, removal.instanceRoot, removal.registryEntity, target));
                } else {
                    steps.emplace_back(new RevertPrefabRemovalCmd(&project, sceneProject->id, removal.path, removal.instanceRoot, removal.registryEntity));
                }
            }
            runCommand(new PrefabOverridesCmd(&project, sceneProject->id, std::move(steps)));
            continue;
        }
        if (name == "levels") {
            // the prefab levels a GameObject roots and the apply targets of it (E)
            std::cout << "  levels:";
            for (const fs::path& path : project.getInstanceRootLevels(sceneProject->id, entity)) std::cout << " " << path.generic_string();
            std::cout << "\n  targets:";
            for (const fs::path& path : project.getPrefabApplyTargets(sceneProject->id, entity)) std::cout << " " << path.generic_string();
            std::cout << "\n";
            continue;
        }
        if (name == "instantiate") {
            // a prefab dropped on the GameObject (ImportPrefabCmd)
            ImportPrefabCmd* command = new ImportPrefabCmd(&project, sceneProject->id, op["prefab"].as<std::string>(),
                op["below"] && op["below"].as<bool>() ? entity : NULL_ENTITY, true);
            command->setQuiet(true);
            runCommand(command);
            continue;
        }
        // ---- F: Replace Prefab, Missing Prefab unpack, previews, usage ----
        if (name == "replace") {
            // Prefab > Replace (ReplacePrefabCmd): match name|hierarchy|none, keepOverrides, renameRoot
            PrefabReplaceSettings settings;
            const std::string match = op["match"] ? op["match"].as<std::string>() : std::string("name");
            settings.match = match == "hierarchy" ? PrefabReplaceSettings::Match::ByHierarchy :
                match == "none" ? PrefabReplaceSettings::Match::None : PrefabReplaceSettings::Match::ByName;
            settings.keepOverrides = !op["keepOverrides"] || op["keepOverrides"].as<bool>();
            settings.renameRoot = !op["renameRoot"] || op["renameRoot"].as<bool>();
            runCommand(new ReplacePrefabCmd(&project, sceneProject->id, {entity}, op["prefab"].as<std::string>(), settings));
            continue;
        }
        if (name == "move") {
            // a Hierarchy drag (MoveEntityOrderCmd): target a dump path, insertion before|after|into
            YAML::Node targetOp;
            targetOp["entity"] = op["target"].as<std::string>();
            if (op["scene"]) targetOp["scene"] = op["scene"];
            SceneProject* targetScene = nullptr;
            const Entity target = findDumpEntity(project, targetOp, targetScene);
            if (target == NULL_ENTITY) return false;
            const std::string insertion = op["insertion"] ? op["insertion"].as<std::string>() : std::string("into");
            const InsertionType insertionType = insertion == "before" ? InsertionType::BEFORE :
                insertion == "after" ? InsertionType::AFTER : InsertionType::INTO;
            runCommand(new MoveEntityOrderCmd(&project, sceneProject->id, entity, target, insertionType));
            continue;
        }
        if (name == "unpackMissing") {
            runCommand(new UnpackMissingPrefabCmd(&project, sceneProject->id, {entity}));
            continue;
        }
        if (name == "preview") {
            // what a prefab thumbnail renders: an instance in a scene of its own, listed
            Scene previewScene;
            const Entity root = project.instantiatePrefabPreview(&previewScene, op["prefab"].as<std::string>());
            std::map<Entity, std::string> paths;
            std::vector<std::string> names;
            for (Entity previewEntity : previewScene.getEntityList()) {
                names.push_back(EntityPath::of(&previewScene, previewEntity, &paths));
            }
            std::sort(names.begin(), names.end());
            std::cout << "  preview root " << (root == NULL_ENTITY ? std::string("none") : EntityPath::of(&previewScene, root, &paths)) << "\n";
            for (const std::string& path : names) std::cout << "  preview " << path << "\n";
            continue;
        }
        if (name == "addComponent") {
            runCommand(new AddComponentCmd(&project, sceneProject->id, entity, type));
            continue;
        }
        if (name == "createPrefabs") {
            // the Hierarchy's multi-selection payload (with the project), split per root as
            // ProjectWindow::saveEntityFile does
            std::vector<Entity> selection;
            for (const YAML::Node& path : op["entities"]) {
                YAML::Node pick;
                pick["entity"] = path.as<std::string>();
                SceneProject* pickedScene = nullptr;
                const Entity picked = findDumpEntity(project, pick, pickedScene);
                if (picked == NULL_ENTITY) return false;
                selection.push_back(picked);
            }
            const YAML::Node payload = Stream::encodeEntitySelection(selection, scene, &project, sceneProject);
            std::vector<std::pair<fs::path, YAML::Node>> prefabs;
            const std::string dir = op["dir"] ? op["dir"].as<std::string>() : std::string("prefabs");
            std::error_code dirError;
            fs::create_directories(project.getProjectPath() / dir, dirError);
            auto addRoot = [&](const YAML::Node& node) {
                const std::string base = node["name"] ? node["name"].as<std::string>() : std::string("Prefab");
                prefabs.emplace_back(fs::path(dir) / (base + ".prefab"), YAML::Clone(node));
            };
            if (payload["members"] && payload["members"].IsSequence()) {
                for (const YAML::Node& member : payload["members"]) addRoot(member);
            } else {
                addRoot(payload);
            }
            runCommand(new CreatePrefabCmd(&project, sceneProject->id, prefabs));
            continue;
        }
        if (name == "addToPrefab") {
            YAML::Node pick;
            pick["entity"] = op["parent"].as<std::string>();
            SceneProject* parentScene = nullptr;
            const Entity parent = findDumpEntity(project, pick, parentScene);
            if (parent == NULL_ENTITY) return false;
            runCommand(new AddEntityToPrefabCmd(&project, sceneProject->id, entity, parent,
                op["target"] ? fs::path(op["target"].as<std::string>()) : fs::path()));
            continue;
        }
        if (name == "setStateLocal") {
            // the GameObject only, for recordState
            if (op["active"]) scene->setEntityActive(entity, op["active"].as<bool>());
            if (op["tag"]) scene->setEntityTag(entity, op["tag"].as<std::string>());
            if (op["layer"]) scene->setEntityLayer(entity, op["layer"].as<int>());
            if (op["name"]) scene->setEntityName(entity, op["name"].as<std::string>());
            sceneProject->isModified = true;
            continue;
        }
        if (name == "addLocalComponent") {
            ProjectUtils::addEntityComponent(scene, entity, type, sceneProject->entities);
            sceneProject->isModified = true;
            continue;
        }
        if (name == "removeLocalComponent") {
            ProjectUtils::removeEntityComponent(scene, entity, type, sceneProject->entities);
            sceneProject->isModified = true;
            continue;
        }
        if (name == "setRef") {
            const std::string property = op["property"].as<std::string>();
            Entity target = NULL_ENTITY;
            if (op["target"] && !op["target"].as<std::string>().empty()) {
                YAML::Node pick;
                pick["entity"] = op["target"].as<std::string>();
                if (op["stage"]) pick["stage"] = op["stage"];
                SceneProject* targetScene = nullptr;
                target = findDumpEntity(project, pick, targetScene);
                if (target == NULL_ENTITY) return false;
            }
            PropertyData data = Catalog::findProperty(scene, entity, type, property);
            if (!data.ref) {
                std::cerr << "scene-dump: no property " << property << "\n";
                return false;
            }
            if (data.type == PropertyType::Entity) {
                *static_cast<Entity*>(data.ref) = target;
            } else if (data.type == PropertyType::EntityReference) {
                static_cast<EntityReference*>(data.ref)->entity = target;
                static_cast<EntityReference*>(data.ref)->sceneId = 0;
            } else if (data.type == PropertyType::Texture) {
                CameraTextureLink::retarget(scene, *static_cast<Texture*>(data.ref), target);
            } else {
                std::cerr << "scene-dump: " << property << " is not a reference\n";
                return false;
            }
            Catalog::updateEntity(scene, entity, data.updateFlags);
            // an instance edit: an override (Unity semantics, section 12)
            project.recordPrefabEdit(sceneProject->id, entity, type, {property});
            sceneProject->isModified = true;
            continue;
        }
        if (name == "removeComponent") {
            runCommand(new RemoveComponentCmd(&project, sceneProject->id, entity, type));
            continue;
        }
        auto operation = operations.find(name);
        if (operation == operations.end()) {
            std::cerr << "scene-dump: unknown op " << name << "\n";
            return false;
        }
        runCommand(new PrefabOverrideCmd(&project, sceneProject->id, entity, operation->second, type, properties,
            op["state"] ? static_cast<uint8_t>(op["state"].as<int>()) : 0,
            op["target"] ? fs::path(op["target"].as<std::string>()) : fs::path()));
    }
    return true;
}

static int runSceneDump(int argc, char** argv, const std::string& commandName) {
    fs::path projectArg;
    fs::path outPath;
    fs::path opsPath;
    bool save = false;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if ((arg == "--project" || arg == "-p") && i + 1 < argc) projectArg = CommandLine::projectArgPath(argv[++i]);
        else if ((arg == "--out" || arg == "-o") && i + 1 < argc) outPath = CommandLine::absoluteArgPath(argv[++i]);
        else if (arg == "--ops" && i + 1 < argc) opsPath = CommandLine::absoluteArgPath(argv[++i]);
        else if (arg == "--save") save = true;
        else {
            std::cerr << commandName << " scene-dump --project <dir|project.yaml> [--out <file>] [--ops <file>] [--save]\n"
                      << "(paths absolute or relative to the current directory)\n";
            return 2;
        }
    }
    if (projectArg.empty()) {
        std::cerr << commandName << " scene-dump: --project is required\n";
        return 2;
    }

    EditorHostOverrideGuard hostOverride(nullptr);
    Out::setConsoleWindow(nullptr);
    AppSettings::initialize();

    Project project;
    auto platform = std::make_unique<Platform>(&project);
    Engine::systemInit(argc, argv, platform.get());
    EngineRuntimeGuard engineRuntime;
    Engine::pauseGameEvents(true);
    Engine::setAsyncLoading(false);

    if (!project.loadProject(projectArg, false)) {
        return 1;
    }

    auto& scenes = project.getScenes();
    for (size_t i = 0; i < scenes.size(); i++) {
        if (!scenes[i].filepath.empty() && !scenes[i].scene) {
            project.loadScene(scenes[i].filepath, false, false, true);
        }
    }

    sceneDumpOpsDir = opsPath.empty() ? fs::path() : opsPath.parent_path();
    if (!opsPath.empty() && !runSceneDumpOps(project, YAML::LoadFile(opsPath.string()))) {
        return 1;
    }

    writeSceneDump(project, outPath);

    if (save) {
        for (SceneProject& sceneProject : scenes) {
            if (sceneProject.scene && !sceneProject.filepath.empty()) {
                project.saveSceneToPath(sceneProject.id, sceneProject.filepath, false);
            }
        }
    }
    return 0;
}

int CommandLine::runSceneDumpCommand(int argc, char** argv, const char* executableName) {
    attachHostConsoleIfNeeded();
    return runSceneDump(argc, argv, getCommandName(executableName));
}

// ---- check: the project checks of the MCP tools, from a shell, CI or an agent hook ----
//
// Lua scripts (syntax, then the top level run in an isolated lua_State to read the properties),
// shader forks (compiled as check_shader compiles them), C++ scripts (built as Play and
// compile_scripts build them) and HTML/CSS UI files (the compatibility layer's diagnostics, as
// the documents load). See docs/mcp/checks.md.
namespace {

struct CheckOptions {
    fs::path project;
    bool lua = false;
    bool shaders = false;
    bool cpp = false;
    bool ui = false;
    bool textures = false;
    bool all = false;
    bool filesGiven = false;
    std::vector<std::string> files;
    std::string format = "text";
    bool quiet = false;
    bool hook = false;
    bool help = false;
};

void printCheckUsage(const std::string& commandName) {
    std::cout
        << "Usage:\n"
        << "  " << commandName << " check --project <project-dir|project.yaml> [options]\n\n"
        << "Options:\n"
        << "  -p, --project <path>     Project directory or project.yaml file (absolute, or relative to the\n"
        << "                           current directory).\n"
        << "      --lua                Lua scripts: syntax, and the top level run in an isolated Lua state\n"
        << "                           (as the editor reads script properties).\n"
        << "      --shaders            Shader forks: compiled as the editor compiles them.\n"
        << "      --cpp                C++ scripts: configured and built like Play does (slow the first time).\n"
        << "      --ui                 HTML/CSS UI documents: the compatibility diagnostics (dropped or rewritten\n"
        << "                           constructs, missing or remote files) with file:line:col, code and fix.\n"
        << "      --textures           Synthesized textures (PNGs with a jasper.synthesis recipe): warns when a\n"
        << "                           source changed or went missing since (regenerate them).\n"
        << "      --all                Lua, shaders, C++, UI and textures. Default: all but --cpp.\n"
        << "      --files <paths...>   Only these files (absolute, or relative to the current directory or\n"
        << "                           the project); files outside the project or of other kinds are ignored.\n"
        << "      --format <format>    text (default) or json.\n"
        << "  -q, --quiet              No summary line.\n"
        << "      --hook               Agent hook mode: read the hook's JSON on stdin (any tool: Claude Code\n"
        << "                           Edit/Write/Bash, Codex apply_patch/shell), check the files it names and\n"
        << "                           the project files changed since the last --hook run (shell edits), print\n"
        << "                           problems on stderr and exit 2 when there are errors or new UI warnings\n"
        << "                           (UI warnings are shown once), 0 otherwise, 1 when it can't run. Logs each\n"
        << "                           run to .jasperengine/check-hook.log.\n"
        << "  -h, --help               Show this help.\n\n"
        << "Text output, one line per problem, paths relative to the project:\n"
        << "  path:line:col: error|warning: message   (unknown line or column left out)\n"
        << "  UI findings add the fix and the code: path:line:col: warning: message (fix: ...) [HTMLUI-CSS-0002]\n"
        << "Exit code: 0 no errors (warnings allowed), 1 errors, 2 usage error.\n";
}

bool parseCheckArgs(int argc, char** argv, CheckOptions& options, std::string& error) {
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            options.help = true;
        } else if (arg == "-p" || arg == "--project") {
            std::string value;
            if (!requireValue(argc, argv, i, value, error)) return false;
            options.project = CommandLine::projectArgPath(value);
        } else if (arg == "--lua") {
            options.lua = true;
        } else if (arg == "--shaders") {
            options.shaders = true;
        } else if (arg == "--cpp") {
            options.cpp = true;
        } else if (arg == "--ui") {
            options.ui = true;
        } else if (arg == "--textures") {
            options.textures = true;
        } else if (arg == "--all") {
            options.all = true;
        } else if (arg == "--files") {
            options.filesGiven = true;
            while (i + 1 < argc && std::strncmp(argv[i + 1], "--", 2) != 0) options.files.push_back(argv[++i]);
        } else if (arg == "--format") {
            if (!requireValue(argc, argv, i, options.format, error)) return false;
            if (options.format != "text" && options.format != "json") {
                error = "--format must be text or json";
                return false;
            }
        } else if (arg == "-q" || arg == "--quiet") {
            options.quiet = true;
        } else if (arg == "--hook") {
            options.hook = true;
        } else {
            error = "Unknown option: " + arg;
            return false;
        }
    }
    if (options.help) return true;
    if (options.project.empty()) {
        error = "--project is required";
        return false;
    }
    if (options.hook && options.filesGiven) {
        error = "--hook reads the files from stdin: don't pass --files too";
        return false;
    }
    return true;
}

// The files an agent hook's JSON names: Claude Code's tool_input.file_path / notebook_path, and
// the "*** Add File:" / "*** Update File:" / "*** Move to:" lines of a Codex apply_patch
bool hookFiles(const std::string& input, std::vector<std::string>& files, fs::path& base, std::string& tool, std::string& error) {
    nlohmann::json hook;
    try {
        hook = nlohmann::json::parse(input);
    } catch (const std::exception&) {
        error = "stdin is not an agent hook's JSON";
        return false;
    }
    if (!hook.is_object()) {
        error = "stdin is not an agent hook's JSON object";
        return false;
    }
    if (hook.contains("cwd") && hook["cwd"].is_string()) base = hook["cwd"].get<std::string>();
    if (hook.contains("tool_name") && hook["tool_name"].is_string()) tool = hook["tool_name"].get<std::string>();
    const nlohmann::json input_ = hook.value("tool_input", nlohmann::json::object());
    if (!input_.is_object()) return true;
    for (const char* key : {"file_path", "notebook_path"}) {
        if (input_.contains(key) && input_[key].is_string()) files.push_back(input_[key].get<std::string>());
    }
    for (const char* key : {"command", "patch", "input"}) {
        if (!input_.contains(key) || !input_[key].is_string()) continue;
        std::istringstream lines(input_[key].get<std::string>());
        std::string line;
        while (std::getline(lines, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            for (const char* marker : {"*** Add File: ", "*** Update File: ", "*** Move to: "}) {
                const std::string prefix(marker);
                if (line.rfind(prefix, 0) == 0) files.push_back(line.substr(prefix.size()));
            }
        }
    }
    return true;
}

// ---- hook state: the files a shell (Bash sed -i, heredocs, scripts) changed since the last run
//
// A PostToolUse hook only names the files of Edit/Write/apply_patch calls; an agent that edits
// through its shell leaves no path in the hook input. So every --hook run also lists the
// project's Lua, shader and C++ files with their stamps (.jasperengine/check-hook-state.json) and
// checks the ones that changed since the previous run, whatever the tool was: each change is
// checked once. The first run (no state yet) checks the files changed in the last minutes, the
// edits made before the hook first ran, not the project's old problems.

constexpr const char* HOOK_STATE_FILE = "check-hook-state.json";
constexpr const char* HOOK_LOG_FILE = "check-hook.log";
constexpr int HOOK_FIRST_RUN_MINUTES = 10;
constexpr uintmax_t HOOK_LOG_MAX_BYTES = 512 * 1024;

struct HookStamp {
    int64_t time = 0;   // file_time_type ticks (only compared with other stamps of this machine)
    uintmax_t size = 0;
};

bool isHookTrackedFile(const std::string& rel) {
    if (Util::isLuaFile(rel) || Util::isShaderFile(rel) || Util::isHeaderFile(rel) || Util::isSourceFile(rel)) return true;
    if (ProjectChecks::isUiFile(rel)) return true;
    const std::string ext = fs::path(rel).extension().string();
    return ext == ".c" || ext == ".inl";
}

std::map<std::string, HookStamp> scanHookFiles(const fs::path& root) {
    std::map<std::string, HookStamp> files;
    static const std::vector<std::string> extensions = {".lua", ".vert", ".frag", ".glsl", ".h", ".hh", ".hpp", ".hxx",
                                                        ".cpp", ".cc", ".cxx", ".c", ".inl", ".html", ".htm", ".css"};
    for (const fs::path& file : ProjectChecks::findProjectFiles(root, extensions)) {
        std::error_code ec;
        HookStamp stamp;
        stamp.time = static_cast<int64_t>(fs::last_write_time(file, ec).time_since_epoch().count());
        if (ec) continue;
        stamp.size = fs::file_size(file, ec);
        if (ec) continue;
        const std::string rel = ProjectChecks::projectRelative(root, file);
        if (isHookTrackedFile(rel)) files[rel] = stamp;
    }
    return files;
}

// .jasperengine/check-hook-state.json: {"version": 2, "files": {rel: [time, size]},
// "ui_reported": {rel: ["code|message", ...]}}. Version 1 (no ui_reported) is read as before.
struct HookState {
    std::map<std::string, HookStamp> files;
    // the UI warnings already shown, per project-relative file: shown once, after the edit
    // that introduced them (the key has no line, so moving code does not show it again)
    std::map<std::string, std::set<std::string>> uiReported;
};

bool loadHookState(const fs::path& file, HookState& state) {
    std::string text;
    if (!ProjectChecks::readFile(file, text)) return false;
    try {
        const nlohmann::json json = nlohmann::json::parse(text);
        if (!json.is_object() || !json.contains("files") || !json["files"].is_object()) return false;
        for (auto it = json["files"].begin(); it != json["files"].end(); ++it) {
            if (!it.value().is_array() || it.value().size() != 2) continue;
            state.files[it.key()] = {it.value()[0].get<int64_t>(), it.value()[1].get<uintmax_t>()};
        }
        if (json.contains("ui_reported") && json["ui_reported"].is_object()) {
            for (auto it = json["ui_reported"].begin(); it != json["ui_reported"].end(); ++it) {
                if (!it.value().is_array()) continue;
                for (const nlohmann::json& key : it.value()) {
                    if (key.is_string()) state.uiReported[it.key()].insert(key.get<std::string>());
                }
            }
        }
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

void saveHookState(const fs::path& file, const HookState& state) {
    nlohmann::json files = nlohmann::json::object();
    for (const auto& [rel, stamp] : state.files) files[rel] = {stamp.time, stamp.size};
    nlohmann::json uiReported = nlohmann::json::object();
    for (const auto& [rel, keys] : state.uiReported) {
        if (!keys.empty()) uiReported[rel] = std::vector<std::string>(keys.begin(), keys.end());
    }
    const nlohmann::json json = {{"version", 2}, {"files", files}, {"ui_reported", uiReported}};
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    // a hook running in parallel must never read half a file
    const fs::path temp = file.string() + ".tmp" + std::to_string(static_cast<long long>(
        std::chrono::steady_clock::now().time_since_epoch().count() % 1000000007));
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out << json.dump();
        if (!out) {
            out.close();
            fs::remove(temp, ec);
            return;
        }
    }
    fs::rename(temp, file, ec);
    if (ec) fs::remove(temp, ec);
}

fs::path hookStateFile(const fs::path& root) {
    return root / ".jasperengine" / HOOK_STATE_FILE;
}

// The files changed since the last run (all recently changed ones on the first run); `state`
// gets the new stamps (the caller saves it)
std::vector<std::string> hookChangedFiles(const fs::path& root, HookState& state) {
    HookState previous;
    const bool hasState = loadHookState(hookStateFile(root), previous);
    const std::map<std::string, HookStamp> now = scanHookFiles(root);
    const int64_t recent = static_cast<int64_t>(
        (fs::file_time_type::clock::now() - std::chrono::minutes(HOOK_FIRST_RUN_MINUTES)).time_since_epoch().count());
    std::vector<std::string> changed;
    for (const auto& [rel, stamp] : now) {
        if (!hasState) {
            if (stamp.time >= recent) changed.push_back(rel);
            continue;
        }
        auto it = previous.files.find(rel);
        if (it == previous.files.end() || it->second.time != stamp.time || it->second.size != stamp.size) changed.push_back(rel);
    }
    state.files = now;
    state.uiReported = std::move(previous.uiReported);
    // a file that is gone takes its shown warnings with it
    for (auto it = state.uiReported.begin(); it != state.uiReported.end();) {
        it = now.count(it->first) ? std::next(it) : state.uiReported.erase(it);
    }
    return changed;
}

// One line per run in .jasperengine/check-hook.log: time, tool, files checked, result
void appendHookLog(const fs::path& root, const std::string& tool, const std::vector<std::string>& files,
                   const std::string& result, int exitCode, double seconds) {
    const fs::path dir = root / ".jasperengine";
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) fs::create_directories(dir, ec);
    const fs::path log = dir / HOOK_LOG_FILE;
    if (fs::file_size(log, ec) > HOOK_LOG_MAX_BYTES && !ec) {
        fs::rename(log, dir / (std::string(HOOK_LOG_FILE) + ".1"), ec);
    }
    std::ofstream out(log, std::ios::app);
    if (!out) return;
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", &utc);
    std::string list;
    for (size_t i = 0; i < files.size() && i < 20; i++) list += (i ? "," : "") + files[i];
    if (files.size() > 20) list += ",+" + std::to_string(files.size() - 20) + " more";
    char time[32];
    std::snprintf(time, sizeof(time), "%.2fs", seconds);
    out << stamp << " tool=" << (tool.empty() ? "?" : tool) << " files=" << (list.empty() ? "-" : list) << " "
        << result << " exit " << exitCode << " " << time << "\n";
}

fs::path canonicalOr(const fs::path& path) {
    std::error_code ec;
    fs::path canonical = fs::weakly_canonical(path, ec);
    return ec ? path.lexically_normal() : canonical;
}

// Engine and tool output (Out, glslang, CMake) goes to the null device while the checks run:
// stdout carries only the findings
class QuietOutput {
public:
    QuietOutput() {
        std::cout.flush();
        std::cerr.flush();
        std::fflush(stdout);
        std::fflush(stderr);
#if defined(_WIN32)
        savedOut = _dup(1);
        savedErr = _dup(2);
        const int null = _open("NUL", _O_WRONLY);
        if (null >= 0) {
            _dup2(null, 1);
            _dup2(null, 2);
            _close(null);
        }
#else
        savedOut = dup(1);
        savedErr = dup(2);
        const int null = open("/dev/null", O_WRONLY);
        if (null >= 0) {
            dup2(null, 1);
            dup2(null, 2);
            close(null);
        }
#endif
    }
    ~QuietOutput() {
        std::cout.flush();
        std::cerr.flush();
        std::fflush(stdout);
        std::fflush(stderr);
#if defined(_WIN32)
        if (savedOut >= 0) { _dup2(savedOut, 1); _close(savedOut); }
        if (savedErr >= 0) { _dup2(savedErr, 2); _close(savedErr); }
#else
        if (savedOut >= 0) { dup2(savedOut, 1); close(savedOut); }
        if (savedErr >= 0) { dup2(savedErr, 2); close(savedErr); }
#endif
    }

private:
    int savedOut = -1;
    int savedErr = -1;
};

struct CheckRun {
    std::vector<ProjectChecks::Finding> findings;
    std::vector<std::pair<std::string, size_t>> checked;   // category, count (in run order)
    std::vector<std::string> cppLogTail;                   // a failed C++ build without diagnostics
    std::string fatal;                                     // the checks could not run
};

std::string plural(size_t count, const std::string& word) {
    return std::to_string(count) + " " + word + (count == 1 ? "" : "s");
}

CheckRun runChecks(int argc, char** argv, const fs::path& root, const std::vector<fs::path>& luaFiles, bool luaWanted,
                   bool shadersAll, const std::vector<std::string>& shaderFiles, bool shadersWanted, bool cppWanted) {
    CheckRun run;
    EditorHostOverrideGuard hostOverride(nullptr);
    Out::setConsoleWindow(nullptr);
    if (cppWanted) AppSettings::initialize();

    Project project;
    auto platform = std::make_unique<Platform>(&project);
    Engine::systemInit(argc, argv, platform.get());
    EngineRuntimeGuard engineRuntime;
    Engine::pauseGameEvents(true);
    Engine::setAsyncLoading(false);

    if (luaWanted) {
        // every script in a fresh isolated state: running one can't touch the files, the
        // process or the next script
        const std::vector<std::string> engineGlobals = ProjectChecks::engineLuaGlobals();
        const fs::path luaRoot = ProjectChecks::luaRootFromProjectFile(root);
        size_t count = 0;
        for (const fs::path& file : luaFiles) {
            std::string code;
            if (!ProjectChecks::readFile(file, code)) continue;
            count++;
            lua_State* L = ProjectChecks::newIsolatedLuaState(luaRoot, engineGlobals);
            const ProjectChecks::LuaScriptCheck check = ProjectChecks::checkLuaScript(code, ProjectChecks::projectRelative(root, file), L);
            ProjectChecks::closeIsolatedLuaState(L);
            run.findings.insert(run.findings.end(), check.findings.begin(), check.findings.end());
        }
        run.checked.push_back({"lua", count});
    }

    if (shadersWanted || cppWanted) {
        if (!project.loadProject(root, false)) {
            run.fatal = "the project could not be loaded: " + root.string();
            return run;
        }
        // every scene, so the forks in use and the scripts attached are all known
        auto& scenes = project.getScenes();
        for (size_t i = 0; i < scenes.size(); i++) {
            if (!scenes[i].filepath.empty() && !scenes[i].scene) {
                project.loadScene(scenes[i].filepath, false, false, true);
            }
        }
    }

    if (shadersWanted) {
        ProjectChecks::invalidateShaderCaches(&project);
        // an include (.glsl) can be reached by any fork, used by a scene or not yet
        const bool include = std::any_of(shaderFiles.begin(), shaderFiles.end(), [](const std::string& file) {
            return fs::path(file).extension() == ".glsl";
        });
        std::vector<ProjectChecks::ShaderUse> forks = (shadersAll || include) ? ProjectChecks::shaderForksInProject(&project)
                                                                              : ProjectChecks::shaderForksUsingFiles(&project, shaderFiles);
        for (ProjectChecks::ShaderUse& use : forks) {
            const ShaderKey key = ProjectChecks::prepareShaderCheck(&project, use);
            const ProjectChecks::ShaderCheck check = ProjectChecks::runShaderCheck(&project, use, key);
            // a UI effect's forms (phase 5) share their source: an error is reported once
            for (const ProjectChecks::Finding& finding : check.findings) {
                const bool seen = std::any_of(run.findings.begin(), run.findings.end(), [&](const ProjectChecks::Finding& f) {
                    return f.path == finding.path && f.line == finding.line && f.col == finding.col && f.message == finding.message;
                });
                if (!seen) run.findings.push_back(finding);
            }
        }
        // and the materials whose shader reads the scene from the opaque queue: all of them, or
        // with files those drawing with the forks checked
        const std::vector<ProjectChecks::Finding> sceneInputs = ProjectChecks::sceneInputFindings(&project, shadersAll ? nullptr : &forks);
        run.findings.insert(run.findings.end(), sceneInputs.begin(), sceneInputs.end());
        // Stochastic Tiling's warnings (tilted normal maps under Rotation, no effect): the
        // scenes' materials and terrain layers, when every shader is checked
        if (shadersAll) {
            const std::vector<ProjectChecks::Finding> stochastic = ProjectChecks::stochasticTilingFindings(&project);
            run.findings.insert(run.findings.end(), stochastic.begin(), stochastic.end());
        }
        run.checked.push_back({"shaders", forks.size()});
    }

    if (cppWanted) {
        ProjectChecks::CppBuildPlan plan;
        std::string error;
        const ProjectChecks::CppPrepare prepared = ProjectChecks::prepareCppBuild(&project, plan, error);
        if (prepared == ProjectChecks::CppPrepare::Failed || prepared == ProjectChecks::CppPrepare::MissingTools) {
            run.findings.push_back({"cpp", "", 0, 0, "error", error});
        } else if (prepared == ProjectChecks::CppPrepare::Ready) {
            Generator generator;
            const uint64_t firstLog = Out::lastLogId();
            ProjectChecks::startCppBuild(&project, generator, plan);
            const std::vector<std::string> output = ProjectChecks::waitCppBuild(generator, firstLog);
            const ProjectChecks::CppBuildReport report =
                ProjectChecks::summarizeCppBuild(root, generator.didLastBuildSucceed(), output);
            run.findings.insert(run.findings.end(), report.findings.begin(), report.findings.end());
            if (!report.success && report.errors.empty()) run.cppLogTail = report.logTail;
        }
        run.checked.push_back({"cpp", plan.scripts.size()});
    }
    return run;
}

int runCheck(int argc, char** argv, const std::string& commandName) {
    const auto started = std::chrono::steady_clock::now();
    CheckOptions options;
    std::string error;
    if (!parseCheckArgs(argc, argv, options, error)) {
        std::cerr << commandName << " check: " << error << "\n\n";
        printCheckUsage(commandName);
        return options.hook ? 1 : 2;
    }
    if (options.help) {
        printCheckUsage(commandName);
        return 0;
    }
    const int usageExit = options.hook ? 1 : 2;

    const fs::path root = options.project;
    std::error_code ec;
    if (!fs::is_regular_file(root / "project.yaml", ec)) {
        std::cerr << commandName << " check: no project.yaml in " << root.string() << "\n";
        return usageExit;
    }

    bool lua = options.lua || options.all;
    bool shaders = options.shaders || options.all;
    const bool cpp = options.cpp || options.all;
    bool ui = options.ui || options.all;
    bool textures = options.textures || options.all;
    if (!options.lua && !options.shaders && !options.cpp && !options.ui && !options.textures && !options.all) lua = shaders = ui = textures = true;

    fs::path base = CommandLine::invocationDirectory();
    std::vector<std::string> files = options.files;
    std::string hookTool;
    HookState hookState;
    if (options.hook) {
        std::ostringstream input;
        input << std::cin.rdbuf();
        if (!hookFiles(input.str(), files, base, hookTool, error)) {
            std::cerr << commandName << " check --hook: " << error << "\n";
            appendHookLog(root, hookTool, {}, "error: " + error, 1, 0.0);
            return 1;
        }
        // what the agent's shell (or any other tool) changed since the last run
        for (const std::string& rel : hookChangedFiles(root, hookState)) files.push_back((root / rel).string());
        saveHookState(hookStateFile(root), hookState);
        if (files.empty()) {
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            appendHookLog(root, hookTool, {}, "nothing changed", 0, seconds);
            return 0;
        }
    }

    // what to check
    std::vector<fs::path> luaFiles;
    std::vector<std::string> shaderFiles;
    std::vector<fs::path> uiFiles;
    std::vector<fs::path> textureFiles;
    bool cppFiles = false;
    std::vector<std::string> checkedNames;   // project-relative, for the hook log
    const bool fileMode = options.filesGiven || options.hook;
    if (fileMode) {
        std::set<std::string> seen;
        for (const std::string& given : files) {
            fs::path path(given);
            if (path.is_relative()) {
                fs::path fromBase = base / path;
                path = (!fs::exists(fromBase, ec) && fs::exists(root / path, ec)) ? root / path : fromBase;
            }
            if (!fs::is_regular_file(path, ec)) continue;   // deleted, a directory
            path = canonicalOr(path);
            const fs::path rel = path.lexically_relative(root);
            if (rel.empty() || *rel.begin() == ".." || rel.is_absolute()) {
                if (!options.quiet && !options.hook) std::cerr << commandName << " check: not in the project, skipped: " << given << "\n";
                continue;
            }
            if (ProjectChecks::isInSkippedDir(rel) || !seen.insert(rel.generic_string()).second) continue;
            const std::string name = rel.generic_string();
            if (isHookTrackedFile(name)) checkedNames.push_back(name);
            if (Util::isLuaFile(name)) {
                if (lua) luaFiles.push_back(path);
            } else if (Util::isShaderFile(name)) {
                if (shaders) shaderFiles.push_back(name);
            } else if (Util::isHeaderFile(name) || Util::isSourceFile(name) || rel.extension() == ".c" || rel.extension() == ".inl") {
                cppFiles = true;
            } else if (ProjectChecks::isUiFile(name)) {
                if (ui) uiFiles.push_back(path);
            }
            // a synthesized PNG, or an image a synthesized texture was made from
            if (textures && Util::isImageFile(name)) textureFiles.push_back(path);
        }
    } else if (lua) {
        luaFiles = ProjectChecks::findProjectFiles(root, {".lua"});
    }
    const bool luaWanted = fileMode ? !luaFiles.empty() : lua;
    const bool shadersWanted = fileMode ? !shaderFiles.empty() : shaders;
    const bool cppWanted = cpp && (!fileMode || cppFiles);
    const bool uiWanted = fileMode ? !uiFiles.empty() : ui;
    const bool texturesWanted = fileMode ? !textureFiles.empty() : textures;

    // The UI check is pure parsing (no engine): first, and the engine starts only for the others
    ProjectChecks::UiCheck uiCheck;
    if (uiWanted) {
        QuietOutput quiet;
        uiCheck = ProjectChecks::checkUiFiles(root, fileMode ? uiFiles : std::vector<fs::path>());
    }
    CheckRun run;
    if (luaWanted || shadersWanted || cppWanted) {
        QuietOutput quiet;
        run = runChecks(argc, argv, root, luaFiles, luaWanted, !fileMode, shaderFiles, shadersWanted, cppWanted);
    }
    if (!run.fatal.empty()) {
        std::cerr << commandName << " check: " << run.fatal << "\n";
        if (options.hook) appendHookLog(root, hookTool, checkedNames, "error: " + run.fatal, usageExit, 0.0);
        return usageExit;
    }
    if (uiWanted) {
        run.findings.insert(run.findings.end(), uiCheck.findings.begin(), uiCheck.findings.end());
        run.checked.push_back({"ui", uiCheck.documents + uiCheck.stylesheets});
    }
    if (texturesWanted) {
        // reads PNG headers and hashes the sources of synthesized ones: no engine
        const ProjectChecks::SynthesisCheck synthesis = ProjectChecks::checkSynthesisRecipes(root, fileMode ? textureFiles : std::vector<fs::path>());
        run.findings.insert(run.findings.end(), synthesis.findings.begin(), synthesis.findings.end());
        run.checked.push_back({"textures", synthesis.recipes});
    }
    ProjectChecks::sortFindings(run.findings);
    const size_t errors = ProjectChecks::countSeverity(run.findings, "error");
    const size_t warnings = ProjectChecks::countSeverity(run.findings, "warning");
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

    if (options.hook) {
        // UI errors always show; a UI warning shows once, after the edit that introduced it
        std::map<std::string, std::set<std::string>> uiWarnings;   // path -> its current warning keys
        size_t uiShown = 0, otherWarnings = 0;
        for (const ProjectChecks::Finding& finding : run.findings) {
            if (finding.tool == "ui" && finding.severity == "warning") {
                const std::string key = finding.code + "|" + finding.message;
                uiWarnings[finding.path].insert(key);
                auto known = hookState.uiReported.find(finding.path);
                if (known != hookState.uiReported.end() && known->second.count(key)) continue;
                uiShown++;
            } else if (finding.severity == "warning") {
                otherWarnings++;
            }
            std::cerr << ProjectChecks::toText(finding) << "\n";
        }
        for (const std::string& line : run.cppLogTail) std::cerr << "  " << line << "\n";
        if (uiShown > 0) {
            std::cerr << "(UI warnings are shown once, after the edit that introduced them; " << commandName
                      << " check --ui lists them all)\n";
        }
        std::cerr.flush();
        // fixed warnings drop out: every file the UI check covered takes its current set
        if (uiWanted) {
            for (const std::string& path : uiCheck.files) hookState.uiReported.erase(path);
            for (auto& [path, keys] : uiWarnings) hookState.uiReported[path] = keys;
            saveHookState(hookStateFile(root), hookState);
        }
        const int code = (errors || uiShown) ? 2 : 0;
        appendHookLog(root, hookTool, checkedNames, plural(errors, "error") + ", " + plural(otherWarnings + uiShown, "warning"), code,
                      seconds);
        return code;
    }

    if (options.format == "json") {
        nlohmann::json checked = nlohmann::json::object();
        for (const auto& [category, count] : run.checked) checked[category] = count;
        const nlohmann::json out = {{"ok", errors == 0}, {"errors", errors}, {"warnings", warnings}, {"checked", checked},
                                    {"findings", ProjectChecks::toJson(run.findings)},
                                    {"seconds", std::round(seconds * 100.0) / 100.0}};
        std::cout << out.dump(1) << std::endl;
    } else {
        for (const ProjectChecks::Finding& finding : run.findings) std::cout << ProjectChecks::toText(finding) << "\n";
        std::cout.flush();
        if (!options.quiet) {
            for (const std::string& line : run.cppLogTail) std::cerr << "  " << line << "\n";
        }
    }
    if (!options.quiet) {
        std::string what;
        const std::map<std::string, std::string> units = {{"lua", "file"}, {"shaders", "fork"}, {"cpp", "script"}, {"ui", "file"},
                                                          {"textures", "recipe"}};
        for (const auto& [category, count] : run.checked) {
            what += (what.empty() ? "" : ", ") + category + " " + plural(count, units.at(category));
        }
        char time[32];
        std::snprintf(time, sizeof(time), "%.2fs", seconds);
        std::cerr << "check: " << plural(errors, "error") << ", " << plural(warnings, "warning")
                  << (what.empty() ? std::string(" (nothing to check)") : " (" + what + ")") << " in " << time << "\n";
    }
    return errors ? 1 : 0;
}

}

int CommandLine::runCheckCommand(int argc, char** argv, const char* executableName) {
    attachHostConsoleIfNeeded();
    return runCheck(argc, argv, getCommandName(executableName));
}

// ---- ui-support: the compatibility layer's support tables as JSON (tools/mcp/gen_html_ui_support.py
// renders them to docs/mcp/guide/html-ui-support.md)

int CommandLine::runUiSupportCommand(int argc, char** argv, const char* executableName) {
    attachHostConsoleIfNeeded();
    const std::string commandName = getCommandName(executableName);
    auto usage = [&](std::ostream& out) {
        out << "Usage:\n"
            << "  " << commandName << " ui-support\n\n"
            << "Prints the HTML/CSS support tables of the compatibility layer as JSON on stdout: every\n"
            << "element, attribute, input type, CSS property, value, function, unit, selector, at-rule and\n"
            << "media feature with its support (native, rewritten, approximated, emulated, fallback,\n"
            << "unsupported, ignored; with the fork features of this build), and the diagnostic codes.\n"
            << "tools/mcp/gen_html_ui_support.py renders it to docs/mcp/guide/html-ui-support.md.\n";
    };
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            usage(std::cout);
            return 0;
        }
        std::cerr << commandName << " ui-support: unknown option: " << arg << "\n\n";
        usage(std::cerr);
        return 2;
    }
    std::cout << ProjectChecks::uiSupportJson().dump(1) << std::endl;
    return 0;
}

// ---- ui-effects: the HTML UI effects (phase-5 spec 3.13): the catalogue as text or JSON
// (tools/htmlui/gen_effects_docs.py documents the same manifests), --validate compiles every
// effect in both forms for glcore and checks its manifest against the reflection

namespace fx = ::jasperengine::htmlui;

namespace {

    bool readFileText(const fs::path& path, std::string& content) {
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return false;
        std::ostringstream stream;
        stream << file.rdbuf();
        content = stream.str();
        return true;
    }

    // the project's .frag files with an effect manifest (project-relative paths), sorted
    std::vector<std::string> findProjectEffectFiles(const fs::path& project) {
        std::vector<std::string> files;
        std::error_code ec;
        for (fs::recursive_directory_iterator it(project, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
            if (ec)
                break;
            const fs::path& path = it->path();
            const std::string name = path.filename().string();
            if (it->is_directory(ec) && (name == "build" || name == "out" || (!name.empty() && name[0] == '.'))) {
                it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file(ec) || path.extension() != ".frag")
                continue;
            std::string source;
            if (readFileText(path, source) && fx::EffectManifestParser::hasEffectManifest(source))
                files.push_back(fs::relative(path, project, ec).generic_string());
        }
        std::sort(files.begin(), files.end());
        return files;
    }

}

int CommandLine::runUiEffectsCommand(int argc, char** argv, const char* executableName) {
    attachHostConsoleIfNeeded();
    EditorHostOverrideGuard hostOverride(nullptr);
    Out::setConsoleWindow(nullptr);
    const std::string commandName = getCommandName(executableName);
    auto usage = [&](std::ostream& out) {
        out << "Usage:\n"
            << "  " << commandName << " ui-effects [--json] [--validate] [--project <dir|project.yaml>]\n\n"
            << "Lists the HTML UI effects: the built-in ones, and with --project the project effects\n"
            << "(every .frag of the project with an effect manifest).\n\n"
            << "Options:\n"
            << "      --json              The catalogue as JSON: every manifest field, an example and the\n"
            << "                          thumbnail path (tools/htmlui/gen_effects_docs.py documents it).\n"
            << "      --validate          Compiles every effect in both forms for glcore and checks its\n"
            << "                          manifest against the shader's reflection; exits 1 on any problem.\n"
            << "  -p, --project <dir>     Also the effects of this project (its directory or project.yaml,\n"
            << "                          absolute or relative to the current directory).\n"
            << "  -h, --help              Show this help.\n";
    };
    bool json = false, validate = false;
    std::string projectDir;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            usage(std::cout);
            return 0;
        } else if (arg == "--json") {
            json = true;
        } else if (arg == "--validate") {
            validate = true;
        } else if ((arg == "-p" || arg == "--project") && i + 1 < argc) {
            projectDir = argv[++i];
        } else {
            std::cerr << commandName << " ui-effects: unknown option: " << arg << "\n\n";
            usage(std::cerr);
            return 2;
        }
    }

    fx::EffectRegistry& registry = fx::EffectRegistry::get();
    std::vector<const fx::EffectManifest*> effects = registry.builtins();
    std::vector<std::string> problems;
    for (const fx::EffectDiagnostic& d : registry.builtinDiagnostics())
        problems.push_back(d.file + ":" + std::to_string(d.line) + ": " + d.message);

    // project effects: parsed (and validated) from their files
    fs::path project;
    std::vector<std::unique_ptr<fx::EffectManifest>> projectManifests;
    std::map<std::string, std::string> projectSources;
    if (!projectDir.empty()) {
        project = CommandLine::projectArgPath(projectDir);
        if (!fs::is_directory(project)) {
            std::cerr << commandName << " ui-effects: not a directory: " << projectDir << "\n";
            return 2;
        }
        for (const std::string& file : findProjectEffectFiles(project)) {
            std::string source;
            readFileText(project / file, source);
            auto manifest = std::make_unique<fx::EffectManifest>();
            std::vector<fx::EffectDiagnostic> diags;
            const bool ok = fx::EffectManifestParser::parse(source, file, false, *manifest, diags) &&
                            fx::EffectManifestParser::validate(*manifest, source, diags);
            for (const fx::EffectDiagnostic& d : diags) {
                if (d.severity == fx::compat::Severity::Error)
                    problems.push_back(file + ":" + std::to_string(d.line) + ": " + d.message);
            }
            if (!ok)
                continue;
            if (registry.find(manifest->name) && registry.find(manifest->name)->builtin) {
                problems.push_back(file + ": '" + manifest->name + "' is the name of a built-in effect");
                continue;
            }
            projectSources[file] = source;
            effects.push_back(manifest.get());
            projectManifests.push_back(std::move(manifest));
        }
    }

    if (validate) {
        if (!ShaderVariantCompiler::isAvailable()) {
            std::cerr << commandName << " ui-effects: this build has no shader compiler\n";
            return 1;
        }
        for (const fx::EffectManifest* m : effects) {
            CustomShaderSources sources;
            uint16_t customId = 0;
            if (m->builtin) {
                customId = ShaderPool::registerCustomShader(m->customShaderName());
            } else {
                customId = ShaderPool::registerCustomShader(m->sourcePath);
                sources = ShaderVariantCompiler::resolveCustomShaderSources(m->sourcePath,
                    [&](const std::string& path, std::string& content) { return readFileText(project / path, content); });
                if (!sources.valid) {
                    problems.push_back(m->sourcePath + ": " + sources.error);
                    continue;
                }
            }
            for (int filterForm = m->kind == fx::EffectKind::Paint ? 0 : 1; filterForm <= 1; filterForm++) {
                // the draw form, and a stateful effect's step (and scatter) forms of it
                std::vector<uint32_t> forms = {(uint32_t)filterForm};
                for (uint32_t pass : fx::EffectManifestParser::passForms(*m, (uint32_t)filterForm))
                    forms.push_back(pass);
                for (uint32_t properties : forms) {
                    const ShaderKey key = ShaderPool::getShaderKey(ShaderType::UI_EFFECT, properties, customId);
                    ShaderData data;
                    std::string compileError, log;
                    std::string form = filterForm ? "filter form" : "paint form";
                    if (properties & 2u) form += ", step";
                    if (properties & 4u) form += ", scatter";
                    if (!ShaderVariantCompiler::compile(key, ShaderBackend::GLCore, m->builtin ? nullptr : &sources, data, &compileError, &log)) {
                        problems.push_back(m->name + " (" + form + "): " + compileError + (log.empty() ? "" : "\n" + log));
                        continue;
                    }
                    std::vector<std::string> mismatches;
                    if (!fx::EffectManifestParser::checkReflection(*m, data, mismatches, properties)) {
                        for (const std::string& mismatch : mismatches)
                            problems.push_back(m->name + " (" + form + "): " + mismatch);
                    }
                }
            }
        }
    }

    if (json) {
        nlohmann::json list = nlohmann::json::array();
        for (const fx::EffectManifest* m : effects)
            list.push_back(ProjectChecks::uiEffectJson(*m));
        nlohmann::json out = {{"effects", list}};
        if (validate)
            out["problems"] = problems;
        std::cout << out.dump(1) << std::endl;
    } else {
        for (const fx::EffectManifest* m : effects) {
            std::cout << m->name << "  " << fx::effectKindName(m->kind) << ", " << m->category << ", cost " << fx::effectCostName(m->cost)
                      << (m->backdrop ? ", backdrop" : "") << (m->builtin ? "" : ", project " + m->sourcePath) << "\n    " << m->summary << "\n    "
                      << ProjectChecks::uiEffectExample(*m) << "\n";
        }
        std::cout << effects.size() << " effects\n";
        for (const std::string& problem : problems)
            std::cerr << "error: " << problem << "\n";
        if (validate && problems.empty())
            std::cout << "every effect compiles in every form and matches its manifest\n";
    }
    return problems.empty() ? 0 : 1;
}

int CommandLine::runImportUiCommand(int argc, char** argv, const char* executableName) {
    attachHostConsoleIfNeeded();
    // the importers log nothing through the editor host: the output is the diagnostics
    EditorHostOverrideGuard hostOverride(nullptr);
    Out::setConsoleWindow(nullptr);
    return htmlui::runImportUi(argc, argv, getCommandName(executableName), std::cout, std::cerr);
}

int CommandLine::runSynthesizeTextureCommand(int argc, char** argv, const char* executableName) {
    attachHostConsoleIfNeeded();
    return texsynth::runSynthesizeTexture(argc, argv, getCommandName(executableName), std::cout, std::cerr);
}

int CommandLine::runHelpCommand(int argc, char** argv, const char* executableName) {
    attachHostConsoleIfNeeded();

    const std::string commandName = getCommandName(executableName);
    const std::string argument = argc >= 1 ? argv[0] : "";

    if (argument == "-h" || argument == "--help" || argument == "help") {
        printMainUsage(commandName);
        return 0;
    }

    std::cerr << commandName << ": unknown argument '" << argument << "'\n\n";
    printMainUsage(commandName);
    return 2;
}

}
