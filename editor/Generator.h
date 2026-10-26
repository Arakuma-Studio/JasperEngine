// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <functional>
#include <string>
#include <filesystem>
#include "util/Signal.h"
#include <vector>
#include <atomic>
#include <mutex>
#include <unordered_set>
#include <map>

#include "jobs/JobSystem.h"
#include "Scene.h"
#include "registry/TagManager.h"
#include "Factory.h"
#include "util/Prefab.h"
#include "util/CommandRunner.h"

namespace fs = std::filesystem;

namespace jasperengine::editor {

    struct ScriptPropertyInfo {
        std::string name;
        bool isPtr;
        std::string ptrTypeName;
    };

    struct SceneScriptSource {
        fs::path path;
        fs::path headerPath;
        std::string className;
        std::vector<ScriptPropertyInfo> properties;
    };


    struct SceneBuildInfo {
        uint32_t id;
        std::string name;
        std::vector<uint32_t> involvedScenes; // IDs of all scenes involved in this stack (main + layers)
        std::vector<uint32_t> activeScenes;   // IDs that should be added to Engine when the stack is loaded
        bool isMain;
    };

    struct PrefabSceneInfo {
        std::filesystem::path prefabPath; // relative path of the .prefab file
        std::string functionName;         // generated C++ function name (e.g. create_prefab_X)
        std::string guid;                 // the prefab's asset GUID (the path follows it)
    };

    // Which cmake the editor would run, and where it came from.
    struct CMakeInfo {
        bool found = false;
        std::string path;         // e.g. "/usr/local/bin/cmake"
        std::string version;      // e.g. "3.31.6", empty when it could not be read
        std::string source;       // "configured path" or "on PATH"
        std::string error;        // why a configured path was rejected
    };

    struct CMakeKit {
        std::string displayName;  // e.g. "GCC 15.2.0 x86_64-linux-gnu"
        std::string cCompiler;    // e.g. "/usr/bin/gcc"
        std::string cxxCompiler;  // e.g. "/usr/bin/g++"
        std::string generator;    // e.g. "MinGW Makefiles" (Windows only, empty = CMake default)
        bool available = true;            // false = detected but not usable (shown disabled in UI)
        std::string unavailableReason;    // e.g. "requires Ninja on PATH"
    };

    // Values must stay in sync with the JASPERENGINE_WINDOW_MODE macro consumed by
    // the engine platform backends (0 = windowed, 1 = maximized, 2 = fullscreen).
    enum class WindowMode {
        WINDOWED = 0,
        MAXIMIZED = 1,
        FULLSCREEN = 2
    };

    // Initial window state for generated/exported desktop builds.
    // Web and mobile targets ignore these settings.
    struct WindowSettings {
        WindowMode mode = WindowMode::WINDOWED;
        unsigned int width = 1280;
        unsigned int height = 720;
        bool resizable = true;
        std::string title = "JasperEngine";
    };

    // SceneManager loading screen of generated builds and the editor's Play
    struct LoadingSettings {
        uint32_t sceneId = 0; // none
        float delay = 0.0f;
        float timeout = 5.0f;
        bool asyncLoading = false;
    };

    // Project Settings > Player > Threading (project.yaml `threading:`): baked into the
    // generated main as Engine::setThreadingSettings. -1 is Auto.
    struct ThreadingProjectSettings {
        int workers = -1;                // 1-15
        int reservedFrameWorkers = -1;   // 0-4
        int ioThreads = -1;              // 0-4
        float webPumpBudgetMs = 4.0f;    // single-thread web: Background work per frame
        bool webMultithreaded = false;   // web export: also a pthreads build (cross-origin isolation)

        bool operator==(const ThreadingProjectSettings& other) const{
            return workers == other.workers && reservedFrameWorkers == other.reservedFrameWorkers && ioThreads == other.ioThreads
                && webPumpBudgetMs == other.webPumpBudgetMs && webMultithreaded == other.webMultithreaded;
        }
        bool operator!=(const ThreadingProjectSettings& other) const{ return !(*this == other); }
    };

    // Layer Collision Matrix of Project Settings > Physics or Physics 2D: index = layer,
    // bit j set when that layer and layer j collide (kept symmetric)
    using LayerCollisionRows = std::array<uint32_t, MAX_LAYERS>;

    inline LayerCollisionRows allLayersCollide() {
        LayerCollisionRows rows;
        rows.fill(LAYER_MASK_EVERYTHING);
        return rows;
    }

    // Project Settings > Tags and Layers, and the layer collision matrices of Physics and
    // Physics 2D, baked into the generated init()
    struct TagsAndLayers {
        std::vector<std::string> tags;        // user tags
        std::vector<std::string> layerNames;  // index = layer, empty when built-in or unnamed
        LayerCollisionRows layerCollisionMatrix3D = allLayersCollide();
        LayerCollisionRows layerCollisionMatrix2D = allLayersCollide();
    };

    // A script build's state, published by every Generator (Play, compile_scripts, C++ reload,
    // the C++ editor scripts) through Generator::buildEvents(), from the build's thread
    struct ScriptBuildEvent {
        enum class Kind { Started, Progress, Finished };
        Kind kind = Kind::Started;
        std::string row;                     // "cpp" (the scripts) or "editor" (C++ editor scripts)
        float fraction = -1.0f;              // Progress: 0..1, -1 unknown
        bool success = false;                // Finished
        bool cancelled = false;
        double seconds = 0.0;
        fs::path projectPath;
        std::vector<std::string> output;     // Finished: the build's lines (diagnostics are read from them)
    };

    class Generator {
        std::array<AtmosphereQuality, 3> atmosphereQuality = {AtmosphereQuality::HIGH, AtmosphereQuality::LOW, AtmosphereQuality::LOW};
    private:
        // what the build's events name (setStatusRow)
        std::string statusRow = "cpp";
        // the arguments of the last writeSourceFiles, for rewriteScriptSources
        struct SourceFileArgs {
            bool valid = false;
            fs::path projectPath, projectInternalPath, assetsPath, luaPath;
            std::string libName;
            std::vector<SceneScriptSource> scriptFiles;
            std::vector<SceneBuildInfo> scenes;
            std::vector<PrefabSceneInfo> prefabs;
            bool vsyncEnabled = true;
            WindowSettings windowSettings;
            std::vector<fs::path> scriptDirs;
            int cxxStandard = 20;
        };
        SourceFileArgs lastSourceArgs;
        // the script sources' toolchain fingerprint (S23 R15): the marker and the export
        static std::string scriptFingerprintSource();
        // the build on the process channel, or one running inline in a channel job
        jobs::Task<void> buildTask;
        std::atomic<int> inlineBuilds{0};
        std::atomic<bool> lastBuildSucceeded;
        CommandRunner commandRunner;

        static fs::path getGeneratedPath(const fs::path& projectInternalPath);
        static std::string getEditorPluginAbiCheck();
        static const CMakeKit* chooseDefaultKit(const std::vector<CMakeKit>& kits);

        bool configureCMake(const fs::path& projectPath, const fs::path& buildPath, const std::string& configType, const std::string& cCompiler, const std::string& cxxCompiler, const std::string& generator, unsigned int parallelJobs);
        bool buildProject(const fs::path& buildPath, const std::string& configType, const std::string& generator, unsigned int parallelJobs);
        // Resolve a "Default" (all-empty) selection in place to the best
        // ABI-compatible kit. No-op off Windows; false if none is compatible.
        bool resolveDefaultKit(std::string& cCompiler, std::string& cxxCompiler, std::string& generator);
        bool clearStaleCMakeCache(const fs::path& projectPath, const fs::path& buildPath);
        // Remove CMake's cached configuration so the next configure starts clean.
        // Returns false (with a logged error) if the cache could not be removed,
        // usually because another program holds a lock on the build directory.
        bool cleanBuildDirectory(const fs::path& buildPath);
        std::string getPlatformCMakeConfig(bool vsyncEnabled, const WindowSettings& windowSettings, const fs::path& projectPath, const fs::path& assetsPath, const fs::path& luaPath);
        std::string buildInitSceneScriptsSource(const std::vector<SceneScriptSource>& scriptFiles);
        std::string buildCleanupSceneScriptsSource(const std::vector<SceneScriptSource>& scriptFiles);

        void writeSourceFiles(const fs::path& projectPath, const fs::path& projectInternalPath, std::string libName, const std::vector<SceneScriptSource>& scriptFiles, const std::vector<SceneBuildInfo>& scenes, const std::vector<PrefabSceneInfo>& prefabs, bool vsyncEnabled, const WindowSettings& windowSettings, const fs::path& assetsPath, const fs::path& luaPath, const std::vector<fs::path>& scriptDirs, int cxxStandard);

    public:
        // AGENTS.md at the project root, written only when the project has none (then CLAUDE.md likewise)
        static void writeAgentsFile(const fs::path& projectPath, const fs::path& projectInternalPath);
        // CLAUDE.md importing AGENTS.md, written only when the project has none
        static void writeClaudeFile(const fs::path& projectPath);
        // Files for code tools and language servers, refreshed on project open and on every
        // build: the Lua API stubs (<internal>/engine-api/lua) and the lua-language-server plugin
        // (<internal>/engine-api/luals-plugin.lua.txt) with a .luarc.json pointing
        // lua-language-server at them, and a compile database (<internal>/clangd) with a .clangd
        // pointing clangd at it. A .luarc.json is merged into, a .clangd without the generated
        // marker line is left alone.
        static void writeEditorTooling(const fs::path& projectPath, const fs::path& projectInternalPath, int cxxStandard, const std::vector<fs::path>& scriptDirs);
        // Lua script classes for lua-language-server: <internal>/engine-api/lua/scripts/<Name>.lua
        // declares `---@class <Name> : ScriptInstance` with a `---@field` per property of the
        // script <Name>.lua (read by running its top level in an isolated Lua state, as the
        // `check` CLI does). A script that declares the class itself gets `self` typed. Main
        // thread only. changedFiles empty: every script, stale classes removed; otherwise only
        // those (absolute paths; a deleted script drops its class).
        static void writeLuaScriptStubs(const fs::path& projectPath, const fs::path& projectInternalPath,
                                        const std::vector<fs::path>& changedFiles = {});
        // CMake that fails a plugin configure below the compiler floors of the engine build
        // (engine/cmake/JasperEngineToolchain.cmake), for the generated CMakeLists.txt
        static std::string getToolchainFloorCheck();
        // Engine API include directories of a project's C++ scripts, relative to <internal>/engine-api
        static const std::vector<std::string>& engineApiIncludeDirs();
        Generator();
        ~Generator();
        // Export rebuilds the engine, so it does not need the editor's ABI.
        // Optionally returns the resolved Default kit for the caller to reuse.
        static std::string checkBuildTools(bool requireEditorCompatibility = false, CMakeKit* resolvedDefaultKit = nullptr);
        static std::vector<CMakeKit> detectAvailableKits();
        // Explicit Visual Studio kits target the editor's architecture in both
        // Play and Export. Empty for Default and non-VS/non-Windows generators.
        static std::string getGeneratorPlatform(const std::string& generator);
        static CMakeInfo detectCMake();
        // cmake for a command line: the configured path (quoted) or plain
        // "cmake" for PATH to resolve.
        static std::string cmakeExecutable();
        // A file dialog pick turned into a cmake executable, "" when it holds none.
        static std::string resolveCMakePath(const std::string& userPath);
        // Version a cmake executable reports, "" when it is not one.
        static std::string probeCMakeVersion(const std::string& path);
        // MSVC's /MP processMax accepts at most 65536, making this the universal
        // (machine-independent) ceiling for a stored Parallel Jobs value.
        static constexpr unsigned int MAX_SUPPORTED_PARALLEL_BUILD_JOBS = 65536;
        static unsigned int getAutomaticParallelBuildJobs();
        static unsigned int getMaxParallelBuildJobs();
        // prefabGuid: the asset GUID of a prefab path, empty for a missing prefab (Factory::createPrefab).
        // baked: each prefab's instance as the editor makes it (Project::bakePrefabsForExport),
        // what its build_prefab_* source is generated from
        std::vector<PrefabInstanceInfo> writePrefabSources(const std::map<fs::path, Prefab>& prefabAssets, uint32_t sceneId, const fs::path& projectPath, const fs::path& projectInternalPath,
            const std::function<std::string(const fs::path&)>& prefabGuid, const std::map<fs::path, std::shared_ptr<BakedPrefab>>& baked);
        void writeSceneSource(Scene* scene, const std::string& sceneName, const std::vector<Entity>& entities, const Entity camera, const fs::path& projectPath, const fs::path& projectInternalPath, std::vector<PrefabInstanceInfo>& prefabInstances, const SceneResources& resources);
        void clearSceneSource(const std::string& sceneName, const fs::path& projectInternalPath);
        bool hasSceneSource(const std::string& sceneName, const fs::path& projectInternalPath) const;
        void configure(const std::vector<SceneBuildInfo>& scenes, std::string libName, const std::vector<SceneScriptSource>& scriptFiles, const std::vector<PrefabSceneInfo>& prefabs, const fs::path& projectPath, const fs::path& projectInternalPath, const fs::path& assetsPath, const fs::path& luaPath, const std::vector<fs::path>& scriptDirs, int cxxStandard, Scaling scalingMode = Scaling::FITWIDTH, TextureStrategy textureStrategy = TextureStrategy::NONE, unsigned int canvasWidth = 1280, unsigned int canvasHeight = 720, bool vsyncEnabled = true, const WindowSettings& windowSettings = WindowSettings(), const LoadingSettings& loadingSettings = LoadingSettings(), const TagsAndLayers& tagsAndLayers = TagsAndLayers(), int htmlUiMsaaSamples = 4, const ThreadingProjectSettings& threading = ThreadingProjectSettings(), PbrTier mobilePbrTier = PbrTier::REDUCED);
        void build(const fs::path projectPath, const fs::path buildPath, const std::string& cCompiler = "", const std::string& cxxCompiler = "", const std::string& generator = "", unsigned int parallelJobs = 0);
        // the project's Atmosphere Quality per platform group (desktop, mobile, web) the game
        // starts with; set before configure()
        void setAtmosphereQuality(const std::array<AtmosphereQuality, 3>& quality) { atmosphereQuality = quality; }
        // Configures and builds on the process channel; called from a channel job (the Play
        // startup), it builds on that thread instead and returns when done.
        void runBuild(const fs::path& projectPath, const fs::path& buildPath, const std::string& cCompiler, const std::string& cxxCompiler, const std::string& generator, unsigned int parallelJobs);
        bool isBuildInProgress() const;
        void waitForBuildToComplete();
        bool didLastBuildSucceed() const;
        // Requests cancellation without blocking: the running process is terminated, a build
        // not started yet never runs. waitForBuildToComplete() then returns once it ended.
        void cancelBuild();

        // The script builds' events (any thread; ScriptStatus subscribes)
        static ThreadSafeSignal<const ScriptBuildEvent&>& buildEvents();
        // "cpp" (default) or "editor": the status row this generator's builds update
        void setStatusRow(const std::string& row) { statusRow = row; }
        // A C++ reload during Play (S23 R10): the init/cleanup script sources and SCRIPT_SOURCES
        // written again for these scripts (their classes or DPROPERTYs changed), everything
        // else as the last configure() wrote it. False before any configure().
        bool rewriteScriptSources(const std::vector<SceneScriptSource>& scriptFiles);
        // PluginGeneration.cmake: the reload generation the next plugin build links (its MSVC PDB name)
        static std::string pluginGenerationCMake(size_t generation) {
            return "set(JASPERENGINE_PLUGIN_GENERATION " + std::to_string(generation) + ")\n";
        }
        // The script sources of the last configure() (what the loaded library was built from)
        const std::vector<SceneScriptSource>& lastScriptSources() const { return lastSourceArgs.scriptFiles; }
        // A build output line's progress: "[12/50]" (Ninja), "[ 45%]" (Make); false otherwise
        static bool parseBuildProgress(const std::string& line, float& fraction);
    };
}

