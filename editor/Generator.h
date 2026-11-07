// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "audio/AudioQaSettings.h"
#include "render/GpuCreation.h"
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
#include "DebugSettings.h"
#include "registry/TagManager.h"
#include "util/Prefab.h"
#include "util/BuildDiagnostics.h"
#include "util/CommandRunner.h"
#include "Factory.h"
#include "io/watch/FileIdentity.h"
#include "manager/SceneManager.h"
#include "shader/ShaderGlobals.h"

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
    // Project Settings > Graphics > Shader Creation (spec S32): the engine's ShaderCreationSettings
    // and the editor's shader warm-up options, on a reflection table (editor/reflect/
    // ShaderCreationReflect.cpp: the project file's shaderCreation block, the settings rows)
    struct ShaderCreationProjectSettings {
        bool asyncCreation = true;
        float mainThreadBudgetMs = 2.0f;     // 0.5-16
        bool glWarmDraws = true;             // desktop hardware GL (S32, GPU campaign)
        bool recordDuringPlay = false;       // the editor's Play records the warm-up list
        bool includeInBuilds = true;         // exports build and ship the list's variants
        bool holdFirstScene = true;          // a game's first scene waits for the list

        // the budget into its range (a hand-edited project.yaml)
        void clamp(){
            if (!(mainThreadBudgetMs >= 0.5f)) mainThreadBudgetMs = 0.5f;
            if (mainThreadBudgetMs > 16.0f) mainThreadBudgetMs = 16.0f;
        }
        ShaderCreationSettings engineSettings() const{
            ShaderCreationSettings settings;
            settings.asyncCreation = asyncCreation;
            settings.mainThreadBudgetMs = mainThreadBudgetMs;
            settings.glWarmDraws = glWarmDraws;
            settings.holdFirstSceneForWarmup = holdFirstScene;
            return settings;
        }
        bool operator==(const ShaderCreationProjectSettings& other) const{
            return asyncCreation == other.asyncCreation && mainThreadBudgetMs == other.mainThreadBudgetMs &&
                   glWarmDraws == other.glWarmDraws && recordDuringPlay == other.recordDuringPlay &&
                   includeInBuilds == other.includeInBuilds && holdFirstScene == other.holdFirstScene;
        }
        bool operator!=(const ShaderCreationProjectSettings& other) const{ return !(*this == other); }
    };

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

    // Project Settings > Graphics > HTML UI (S16): the glyph atlas budget in MiB (0: the
    // platform's default), SDF primitives and transform-aware text
    struct HtmlUiRenderSettings {
        int glyphAtlasMiB = 0;           // 0, or 4-256
        bool sdfPrimitives = true;
        bool transformAwareText = true;

        bool operator==(const HtmlUiRenderSettings& other) const{
            return glyphAtlasMiB == other.glyphAtlasMiB && sdfPrimitives == other.sdfPrimitives &&
                   transformAwareText == other.transformAwareText;
        }
        bool operator!=(const HtmlUiRenderSettings& other) const{ return !(*this == other); }
    };

    // Project Settings > Graphics > GPU Instancing (spec S55): the automatic instancing of
    // MeshComponent draws
    struct DrawSubmissionSettings {
        bool gpuInstancing = true;

        bool operator==(const DrawSubmissionSettings& other) const{
            return gpuInstancing == other.gpuInstancing;
        }
        bool operator!=(const DrawSubmissionSettings& other) const{ return !(*this == other); }
    };

    // Project Settings > Audio (audio-mixer spec 9): the exported init configures the voices and
    // loads the mixer before the first scene
    struct AudioSettings {
        std::string mixer;              // asset-relative path of the project's .mixer ("" = none: an implicit Master)
        int maxRealVoices = 32;
        int maxVoices = 256;
        int sampleRate = 0;             // 0 = the device's
        std::string defaultOutputGroup; // the group new Audio Sources get ("" = Master)
        std::string musicGroup;         // the group music plays through when nothing names one ("" = Master)
        int seed = 1;                   // the Sound Event generator's seed in a deterministic run
        // Play's audio captures (docs/specs/S35-agent-audio.md): editor only, not exported
        int captureMaxSeconds = 120;
        int captureKeep = 3;
        bool captureStems = false;
        AudioQaSettings qa;
        bool creditsFile = true;        // exports write licenses/THIRD-PARTY-AUDIO.txt
        // Measurement only: what the Mixer window, get_play_state and the checks read against
        float loudnessTargetLufs = -18.0f;
        float loudnessToleranceLu = 2.0f;
        float truePeakCeilingDbtp = -1.0f;
        std::string outputStage = "roundoff";        // roundoff | linear
        std::string iosSessionCategory = "default";  // default (miniaudio's) | ambient | solo_ambient | playback

        bool operator==(const AudioSettings& other) const {
            return mixer == other.mixer && maxRealVoices == other.maxRealVoices && maxVoices == other.maxVoices &&
                   sampleRate == other.sampleRate && defaultOutputGroup == other.defaultOutputGroup && musicGroup == other.musicGroup &&
                   loudnessTargetLufs == other.loudnessTargetLufs && loudnessToleranceLu == other.loudnessToleranceLu &&
                   truePeakCeilingDbtp == other.truePeakCeilingDbtp && outputStage == other.outputStage &&
                   iosSessionCategory == other.iosSessionCategory && seed == other.seed && creditsFile == other.creditsFile &&
                   captureMaxSeconds == other.captureMaxSeconds && captureKeep == other.captureKeep && captureStems == other.captureStems && qa == other.qa;
        }
        bool operator!=(const AudioSettings& other) const { return !(*this == other); }
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
        // Project Settings > Physics > 3D World Capacity (PhysicsSystem::setWorldCapacity3D)
        unsigned int maxBodies3D = 8192;
        unsigned int maxBodyPairs3D = 8192;
        unsigned int maxContactConstraints3D = 8192;
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

    // Player > Resolution and Presentation and the application, baked into the generated
    // init() (Screen::initialize, Engine::setTargetFrameRate, System::setApplicationInfo)
    struct PlayerSettings {
        ScreenOrientation defaultOrientation = ScreenOrientation::AutoRotation;
        uint8_t allowedOrientations = (1u << 1) | (1u << 3) | (1u << 4);
        bool keepScreenOn = true;
        int targetFrameRate = -1;
        // Project Settings > Graphics > Dynamic Batching
        bool dynamicBatching = true;
        // what System.getAppName/Identifier/Version/Build return on each desktop and the web
        // (with their platform overrides); Android and iOS read their package and bundle
        ApplicationInfo linuxApp, windowsApp, macosApp, webApp;
    };

    // editor/tests/PlatformServicesTest.cpp: the generated platform source lists
    struct GeneratorTestAccess;

    class Generator {
        std::array<AtmosphereQuality, 3> atmosphereQuality = {AtmosphereQuality::HIGH, AtmosphereQuality::LOW, AtmosphereQuality::LOW};
        std::array<float, 3> foliageDistanceScale = {1.0f, 1.0f, 1.0f};
        DrawSubmissionSettings drawSubmission;
        // Project Settings > Input: the actions asset (assets-relative, "" none) and the deadzone
        std::string inputActionsAsset;
        float inputDeadzone = 0.15f;
        // Project Settings > Navigation: the statements main.cpp runs (nav::setSettings), "" defaults
        std::string navigationSettingsCode;
        // Project Settings > Quality > Device Tiers and the rest (DeviceSettings::toJson)
        std::string deviceSettingsJson = "{}";
        // Project Settings > Texture per platform group (desktop, mobile, web; S14 R12)
        std::array<int, 3> textureQuality = {0, 0, 0};
        std::array<int, 3> textureBudgetMB = {0, 512, 384};
        std::array<bool, 3> textureLowMemory = {false, false, true};
        friend struct GeneratorTestAccess;
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
        // macos.frameworks (Project Settings > Native Plugins): linked with the Objective-C plugins
        std::vector<std::string> macOSFrameworks;
        std::atomic<bool> lastBuildSucceeded;
        CommandRunner commandRunner;
        // the compiler and linker errors of the last C++ script build, repeated at its end
        BuildErrorLog buildErrors;
        // the project's UI Scale, emitted by configure (setUIScale before it)
        CanvasScalerSettings uiScale;

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
        void setMacOSFrameworks(std::vector<std::string> frameworks) { macOSFrameworks = std::move(frameworks); }
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
        // The Jolt and Box2D macros the editor's engine library was built with (space-separated
        // definitions): every C++ that links it compiles with them (scripts, editor scripts, clangd)
        static std::string physicsCompileDefinitions();
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
        // The CMake program path for an argv launch (unquoted; "cmake" when none is configured)
        static std::string cmakeProgram();
        // Whether `generator` is a CMake generator name the editor will pass to CMake (allow-list)
        static bool isKnownGenerator(const std::string& generator);
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
        void writeSceneSource(Scene* scene, uint32_t sceneId, const std::string& sceneName, const std::vector<Entity>& entities, const Entity camera, const fs::path& projectPath, const fs::path& projectInternalPath, std::vector<PrefabInstanceInfo>& prefabInstances, const SceneResources& resources);
        void clearSceneSource(uint32_t sceneId, const std::string& sceneName, const fs::path& projectInternalPath);
        bool hasSceneSource(uint32_t sceneId, const std::string& sceneName, const fs::path& projectInternalPath) const;
        // T3 R34: the tiers the generated game ships (the Exporter's; Play draws them all)
        static uint32_t exportTerrainTiers;

        void configure(const std::vector<SceneBuildInfo>& scenes, std::string libName, const std::vector<SceneScriptSource>& scriptFiles, const std::vector<PrefabSceneInfo>& prefabs, const fs::path& projectPath, const fs::path& projectInternalPath, const fs::path& assetsPath, const fs::path& luaPath, const std::vector<fs::path>& scriptDirs, int cxxStandard, Scaling scalingMode = Scaling::FITWIDTH, TextureStrategy textureStrategy = TextureStrategy::NONE, unsigned int canvasWidth = 1280, unsigned int canvasHeight = 720, bool vsyncEnabled = true, const WindowSettings& windowSettings = WindowSettings(), const LoadingSettings& loadingSettings = LoadingSettings(), const TagsAndLayers& tagsAndLayers = TagsAndLayers(), int htmlUiMsaaSamples = 4, const ThreadingProjectSettings& threading = ThreadingProjectSettings(), const HtmlUiRenderSettings& htmlUiRender = HtmlUiRenderSettings(), PbrTier mobilePbrTier = PbrTier::REDUCED, const ShaderCreationProjectSettings& shaderCreation = ShaderCreationProjectSettings(), bool allowInsecureHttp = false, const std::vector<ShaderGlobalDecl>& shaderGlobals = {}, const PlayerSettings& playerSettings = {}, const AudioSettings& audioSettings = AudioSettings(), const DebugSettings& debugSettings = DebugSettings());
        // main.cpp's init lines of Project Settings > Input: the default deadzone (when not 0.15)
        // and InputActions::loadDefault of the actions asset (none: the engine keeps Unity's
        // Input Manager defaults)
        // the Canvas Scaler project default the next configure() writes into the generated main
        void setUIScale(const CanvasScalerSettings& settings) { uiScale = settings; }
        static std::string inputSettingsInitSource(const std::string& actionsAsset, float deadzone);
        // Project Settings > Graphics > Shader Globals in the generated main.cpp: the declarations
        // (with their defaults) before any scene loads; "" when the project declares none
        static std::string shaderGlobalsInitSource(const std::vector<ShaderGlobalDecl>& shaderGlobals);
        // AudioSystem::configure / AudioMixer::load lines of main.cpp ("" for the defaults)
        static std::string audioInitSource(const AudioSettings& audioSettings);
        // The generated init's lines of Project Settings > Debug: the development build flag
        // (JASPERENGINE_DEVELOPMENT_BUILD of a Debug-type export, DEBUG of Xcode's Debug
        // configuration), the log buffer and stack traces, DebugOverlay::configure
        static std::string debugInitSource(const DebugSettings& debugSettings);
        void build(const fs::path projectPath, const fs::path buildPath, const std::string& cCompiler = "", const std::string& cxxCompiler = "", const std::string& generator = "", unsigned int parallelJobs = 0);
        // the project's Atmosphere Quality per platform group (desktop, mobile, web) the game
        // starts with; set before configure()
        void setAtmosphereQuality(const std::array<AtmosphereQuality, 3>& quality) { atmosphereQuality = quality; }
        void setFoliageDistanceScale(const std::array<float, 3>& scale) { foliageDistanceScale = scale; }
        void setDrawSubmission(const DrawSubmissionSettings& settings) { drawSubmission = settings; }
        // the project's LOD Bias and Shadow LOD Bias the game starts with; set before configure()
        // the Input Actions the game starts with (InputActions::loadDefault); set before configure()
        void setInputSettings(const std::string& actionsAsset, float deadzone) { inputActionsAsset = actionsAsset; inputDeadzone = deadzone; }
        // the navigation settings the game starts with (generateNavigationSettingsCode); set before configure()
        void setNavigationSettingsCode(const std::string& code) { navigationSettingsCode = code; }
        // the device settings the game applies its Device Tier with (DeviceSettings::toJson); set
        // before configure()
        void setDeviceSettingsJson(const std::string& json) { deviceSettingsJson = json; }
        void setTextureDefaults(const std::array<int, 3>& quality, const std::array<int, 3>& budgetMB, const std::array<bool, 3>& lowMemory) {
            textureQuality = quality;
            textureBudgetMB = budgetMB;
            textureLowMemory = lowMemory;
        }
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

