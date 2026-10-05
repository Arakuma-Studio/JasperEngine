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
#include "command/type/DeleteEntityCmd.h"
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
#include "imgui.h"
#include "command/type/RenameFileCmd.h"
#include "command/type/DeleteFileCmd.h"
#include "command/type/MoveEntityOrderCmd.h"
#include <chrono>
#include <thread>
#include "pool/ShaderPool.h"
#include "shader/ShaderBuilder.h"

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
    bool help = false;
    bool listScenes = false;
};

struct ShaderCliOptions {
    fs::path targetDir;
    std::set<ShaderBackend> backends;
    std::set<ShaderKey> shaderKeys;
    ShaderOutputFormat outputFormat = ShaderOutputFormat::Binary;
    bool help = false;
};

struct EngineRuntimeGuard {
    bool initialized = false;
    ~EngineRuntimeGuard() {
        if (!initialized) {
            return;
        }
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

// The spec format the game's ShaderPool/ShaderVariantCollection take as well
static bool parseShaderSpec(const std::string& value, ShaderKey& out, std::string& error) {
    return ShaderPool::parseShaderSpec(value, out, &error);
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
        << "      --list-scenes           Print project scenes and exit.\n"
        << "  -h, --help                  Show this help.\n\n"
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
        << "  -s, --scene <scene>    Scene file or scene name. Default: the project's start scene.\n"
        << "      --frames <n>       Measured frames after warmup. Default: 180.\n"
        << "      --warmup <n>       Warmup frames after resources load. Default: 90.\n"
        << "      --mesh-lod <on|off>      Override the scene's mesh LOD setting.\n"
        << "      --depth-prepass <on|off> Override the scene's depth prepass setting.\n"
        << "      --name <label>     Label written into the JSON result.\n"
        << "      --out <path>       Write JSON to this file in addition to stdout.\n"
        << "  -h, --help             Show this help.\n\n"
        << "Vsync is forced off. The scene camera is used for the capture.\n";
}

static void printShadersUsage(const std::string& commandName) {
    std::cout
        << "Usage:\n"
        << "  " << commandName << " shaders --out <dir> --shader <spec> [options]\n\n"
        << "Options:\n"
        << "  -o, --out <path>            Destination directory for generated shader files.\n"
        << "      --format <format>       binary, header, or json. Default: binary.\n"
        << "      --backend <list>        opengl, opengles, d3d11, metal-macos, metal-ios, vulkan, all. Can repeat.\n"
        << "      --shader <spec>         Shader type and optional properties, e.g. mesh:Uv1,Nor. Can repeat.\n"
        << "  -h, --help                  Show this help.\n\n"
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
            options.projectPath = value;
        } else if (arg == "-o" || arg == "--out" || arg == "--target") {
            if (!requireValue(argc, argv, i, value, error)) return false;
            options.targetDir = value;
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
            ShaderKey key;
            if (!parseShaderSpec(value, key, error)) {
                return false;
            }
            options.shaderKeys.insert(key);
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
            options.targetDir = value;
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
            ShaderKey key;
            if (!parseShaderSpec(value, key, error)) {
                return false;
            }
            options.shaderKeys.insert(key);
        } else {
            error = "Unknown argument: " + arg;
            return false;
        }
    }

    if (options.help) {
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

static fs::path normalizeProjectPath(const fs::path& path) {
    if (path.filename() == "project.yaml") {
        return path.parent_path();
    }
    return path;
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
    EngineRuntimeGuard engineRuntime;
    Engine::systemInit(argc, argv, platform.get());
    engineRuntime.initialized = true;
    Engine::pauseGameEvents(true);
    Engine::setAsyncLoading(true);

    ShaderPool::setShaderBuilder([projectPtr = &project](ShaderKey shaderKey) -> ShaderBuildResult {
        static ShaderBuilder builder;
        return builder.buildShader(shaderKey, projectPtr);
    });

    const fs::path projectPath = normalizeProjectPath(options.projectPath);
    if (!project.loadProject(projectPath, false)) {
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
    EngineRuntimeGuard engineRuntime;
    Engine::systemInit(argc, argv, platform.get());
    engineRuntime.initialized = true;
    Engine::pauseGameEvents(true);
    Engine::setAsyncLoading(true);

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
namespace {

// The dump's paths are EntityPath's (the MCP server addresses GameObjects the same way)
std::string dumpPath(Scene* scene, Entity entity, EntityPath::Cache& cache) {
    return EntityPath::of(scene, entity, &cache);
}

}

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
            node["parent"] = parent != NULL_ENTITY ? dumpPath(scene, parent, paths) : std::string();
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
                        scene->isEntityCreated(target) ? dumpPath(scene, target, paths) : "<missing " + std::to_string(target) + ">";
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
            std::string path = dumpPath(scene, entity, paths);
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
//       create {scene | stage: true, name, kind: empty|box|sphere|light, position};
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
        {"image", EntityCreationType::IMAGE},
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
                names.push_back(dumpPath(&previewScene, previewEntity, paths));
            }
            std::sort(names.begin(), names.end());
            std::cout << "  preview root " << (root == NULL_ENTITY ? std::string("none") : dumpPath(&previewScene, root, paths)) << "\n";
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
        if ((arg == "--project" || arg == "-p") && i + 1 < argc) projectArg = argv[++i];
        else if ((arg == "--out" || arg == "-o") && i + 1 < argc) outPath = argv[++i];
        else if (arg == "--ops" && i + 1 < argc) opsPath = argv[++i];
        else if (arg == "--save") save = true;
        else {
            std::cerr << commandName << " scene-dump --project <dir> [--out <file>] [--ops <file>] [--save]\n";
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
    EngineRuntimeGuard engineRuntime;
    Engine::systemInit(argc, argv, platform.get());
    engineRuntime.initialized = true;
    Engine::pauseGameEvents(true);
    Engine::setAsyncLoading(false);

    fs::path projectPath = fs::absolute(projectArg);
    if (projectPath.filename() == "project.yaml") projectPath = projectPath.parent_path();
    if (!project.loadProject(projectPath, false)) {
        return 1;
    }

    auto& scenes = project.getScenes();
    for (size_t i = 0; i < scenes.size(); i++) {
        if (!scenes[i].filepath.empty() && !scenes[i].scene) {
            project.loadScene(scenes[i].filepath, false, false, true);
        }
    }

    sceneDumpOpsDir = opsPath.empty() ? fs::path() : fs::absolute(opsPath).parent_path();
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
