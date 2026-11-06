// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Project.h"
#include "ShippingTypes.h"
#include "cook/BuildReport.h"
#include "cook/SceneCooker.h"
#include "players/ModuleUsage.h"
#include "shader/ShaderBuilder.h"
#include "ShaderDataSerializer.h"
#include "util/CommandRunner.h"

#include <filesystem>
#include <map>
#include <set>
#include <vector>
#include <string>
#include <functional>
#include <atomic>
#include <mutex>

#include "jobs/JobSystem.h"
#include "io/ScopedFileLock.h"

#include <memory>

namespace jasperengine::editor {

    namespace fs = std::filesystem;

    enum class ShaderOutputFormat {
        Header,
        Binary,
        Json
    };

    enum class ExportMode {
        SourceCode,
        Desktop,
        Web,
        // a player for ExportConfig::platform, prebuilt or compiled (docs/specs/S30-shipping.md D1)
        Player
    };

    struct PlayerInfo;

    struct ExportConfig {
        ExportMode mode = ExportMode::SourceCode;
        fs::path targetDir;        // SourceCode: user dir; Desktop/Web: overwritten with the build cache dir
        fs::path destinationDir;   // Desktop/Web only: where the final artifacts are copied
        bool overwriteTarget = false;  // SourceCode: allow exporting into a non-empty targetDir
        fs::path assetsDir;
        fs::path luaDir;
        uint32_t startSceneId = 0;
        // Resolved from the scenes and the overrides below; CLI shader generation
        // fills it in directly
        std::set<ShaderKey> selectedShaderKeys;
        // An explicit --shader list on an export replaces what the scenes reported
        std::set<ShaderKey> shaderKeysOverride;
        std::set<ShaderKey> shaderAdditions;
        std::set<ShaderKey> shaderExclusions;
        // graphic backends the shaders are compiled for
        std::set<::jasperengine::ShaderBackend> selectedBackends;
        ShaderOutputFormat shaderOutputFormat = ShaderOutputFormat::Header;
        // Desktop: compiler kit from the editor settings ("" = CMake default toolchain)
        std::string cmakeCCompiler;
        std::string cmakeCxxCompiler;
        std::string cmakeGenerator;
        unsigned int buildJobs = 0;          // 0 = automatic
        std::string buildType = "Release";
        // Development Build: the profiler and --profile in a Release game (Debug always has them)
        bool developmentBuild = false;
        // Desktop/Web: value passed to the exported CMake GRAPHIC_BACKEND
        // setting (glcore, gles3, d3d11, metal, or vulkan).
        std::string graphicBackend;
        // Native builds: pack assets/lua into resources.pak instead of copying folders.
        // Experimental and disabled by default.
        bool packNativeResources = false;
        // Web: emsdk root override ("" = auto-detect via EMSDK env / PATH)
        std::string emsdkPath;
        // Export anyway when a scene of the build references an asset that is missing (S04)
        bool allowMissingAssets = false;
        // Web: also a pthreads build (threads/), which the page picks when cross-origin
        // isolated (Project Settings > Player > Threading > Web: Multithreaded)
        bool webMultithreaded = false;

        // Player (S30): the target, how it is built (Auto: a prebuilt player unless the project
        // needs native code, chooseBuildMethod), the Android package, where players are found
        BuildPlatform platform = hostBuildPlatform();
        BuildMethod method = BuildMethod::Auto;
        AndroidFormat androidFormat = AndroidFormat::Aab;
        fs::path playersDir;
        // CLI only (engine developers): a player of another engine version; recorded in the
        // content manifest, refused by publish verify
        bool allowPlayerMismatch = false;
        // Android signing: never saved (the prompt, or JASPER_ANDROID_KS_PASS / _KEY_PASS);
        // empty keystore: the debug keystore
        std::string keystorePassword;
        std::string keyPassword;
        // iOS: the simulator's unsigned .app instead of an archive
        bool iosSimulator = false;
    };

    // Why a project needs the Compile method (R5); empty: a prebuilt player can build it
    struct BuildMethodChoice {
        BuildMethod method = BuildMethod::Prebuilt;
        std::vector<std::string> reasons;
    };

    // What a Player build produced (the CLI's, MCP's and UI's report)
    struct ExportResult {
        BuildMethod methodUsed = BuildMethod::Auto;
        std::vector<std::string> compileReasons;
        std::string playerId;
        fs::path outputDir;
        fs::path contentManifest;
        fs::path executable;
        std::vector<fs::path> artifacts;
        std::vector<BuildWarning> warnings;
        // what the user does next ("Open on a Mac with Xcode")
        std::vector<std::string> notes;
        // S34: the engine modules the build ships (a prebuilt player's: the ones the project
        // needs; a Compile build's: the ones it compiled), why each is needed, and the rest
        std::vector<std::string> modules;
        std::vector<ModuleEvidence> moduleEvidence;
        std::vector<std::string> modulesLeftOut;
        // S53 R21: build-report.json next to the export, and its numbers
        fs::path buildReport;
        uint64_t buildBytes = 0;
        std::map<std::string, uint64_t> buildCategories;
    };

    struct ExportProgress {
        std::string currentStep;
        std::string detailLine;              // last streamed build-output line
        float overallProgress = 0.0f;
        bool started = false;
        bool finished = false;
        bool failed = false;
        std::string errorMessage;
    };

    // The Emscripten SDK a Web build uses. No emcmake wrapper (emcmake.bat would run through
    // cmd.exe, which parses its arguments): CMake is run directly with what emcmake.py adds
    // (Exporter::emscriptenCMakeArgs) and the SDK environment emsdk_env would set
    // (Exporter::emscriptenEnvironment), both from the SDK's layout and its .emscripten config.
    struct EmsdkInfo {
        bool found = false;
        std::string emscripten;              // the emscripten folder (emcc, cmake/Modules/Platform)
        std::string toolchainFile;           // <emscripten>/cmake/Modules/Platform/Emscripten.cmake
        std::string emsdkRoot;               // the emsdk holding it ("" when emscripten stands alone)
        std::string configFile;              // the .emscripten emcc reads ("" none found: its default)
        std::string node;                    // node.js (CMAKE_CROSSCOMPILING_EMULATOR, EMSDK_NODE), "" none
        std::string python;                  // the SDK's own python (EMSDK_PYTHON), "" none
        std::vector<std::string> pathDirs;   // SDK folders put first on PATH (as emsdk_env does)
        std::string description;             // where it was found, for the UI
    };

    class Exporter {
    private:
        Project* project = nullptr;
        ExportConfig config;
        ExportProgress progress;
        mutable std::mutex progressMutex;
        std::atomic<bool> cancelRequested{false};

        // the export run on the process channel (it waits on cmake, the compilers, packagers)
        jobs::Task<void> exportTask;
        // the app (player) name, fixed when the export starts: a rename while it builds can't change
        // which file is produced and recorded
        std::string fixedAppName;
        fs::path producedExecutable;             // guarded by progressMutex

        ShaderBuilder shaderBuilder;
        CommandRunner commandRunner;
        // held while a build uses the shared compile tree (getSharedBuildCacheDir)
        std::unique_ptr<io::ScopedFileLock> sharedBuildLock;

        // The UI Documents of the exported scenes, collected while loadAndSaveAllScenes() has
        // every scene loaded, for checkHtmlUIReferences()
        struct HtmlUIDocumentUse {
            std::string scene;
            std::string entity;
            std::string filename;   // asset-relative
        };
        std::vector<HtmlUIDocumentUse> htmlUIDocuments;
        // the build's references to missing assets (S04), found while the scenes are loaded
        std::vector<std::string> missingBuildReferences;

        // S14: the texture sources the export's cooks replaced (asset-relative), which
        // copyAssets() leaves out
        std::set<std::string> cookedTextureSources;
        // S53: the same for models (and the .bin files only they read)
        std::set<std::string> cookedModelSources;
        // S53 R21: what the export's cooks measured, for the Build Report: a cooked file's build
        // path -> its source asset and that source's bytes
        std::map<std::string, std::pair<std::string, uint64_t>> cookedModelSizes;
        BuildReport buildReport;

        // S30: the Player build's report, the cooked scenes and prefabs loadAndSaveAllScenes()
        // made while every scene was loaded, and the player runtime it stages
        ExportResult result;
        std::vector<cook::CookedFileBytes> cookedFiles;
        std::vector<cook::CookedPrefabInfo> cookedPrefabs;
        std::shared_ptr<PlayerInfo> player;
        // S34: what the project reaches of the optional modules, found while every scene was
        // loaded (loadAndSaveAllScenes); a source export has no such answer (every module allowed)
        ModuleUsage moduleUsage;
        bool moduleUsageKnown = false;
        // the prebuilt build's player: the smallest installed variant with every needed module
        bool selectPlayerVariant();
        // the modules a Compile build compiles: those Project Settings allow, of the needed ones
        std::vector<std::string> compiledModules() const;
        void reportModules(const std::vector<std::string>& shipped);
        // the staging directory the build is written to, and its content root inside it
        fs::path stagingDir;
        fs::path contentDir;
        // a Compile build's staged notices, by library
        std::vector<std::string> compileNotices;
        // a Compile build's split debug information, kept for publish prepare (content manifest)
        fs::path buildSymbolsDir;
        // a player build's Compile fallback runs runExport() inside it: the inner export does not
        // report the build finished, the fallback does once its own steps are done (the content
        // manifest, the executable a caller reads at once)
        bool holdFinish = false;
        bool keepBuildSymbols(const fs::path& executable);

        // Lua sources copyLua() shipped (where they are on disk: the assets walk may reach them
        // through a link), so copyAssets() can skip them
        std::set<fs::path> luaCopiedSources;
        // Linked folders walkTree() left out (where each link is on disk), so each is warned about
        // once per export: the shader copy walks the project root and reaches the assets' links again
        std::set<fs::path> leftOutLinks;
        size_t treeLinksEntered = 0;
        // Paths copyLua() wrote, relative to the lua tree, so copyAssets() can keep
        // Android's merged assets root free of paths both trees would ship
        std::set<fs::path> luaExportedPaths;
        // C++ files copyGenerated() and copyCppScripts() wrote, relative to the project root,
        // so the Xcode workspace leaves out files an earlier export left there
        std::set<fs::path> cppExportedPaths;

        // Scales values passed to setProgress so the generation steps (written
        // for the [0,1] SourceCode range) map into [0, 0.5] for Desktop/Web,
        // leaving the upper half for configure/build/collect.
        float progressScale = 1.0f;

        void setProgress(const std::string& step, float value);
        void setProgressRaw(const std::string& step, float value);
        void setDetail(const std::string& line);
        void setError(const std::string& message);
        void runExport();
        // ExportMode::Player: the method, then the prebuilt steps or the Compile build (D1)
        void runPlayerBuild();
        bool runPrebuilt();
        bool resolvePlayer();
        // R6 steps after the content ones
        bool cookScenes();
        bool writeGameConfig();
        bool stagePlayer();
        bool packContent();
        bool stageNotices();
        bool writeContentManifest(bool prebuilt);
        bool finalizeLinux();
        bool finalizeWindows();
        bool finalizeMacOS();
        bool finalizeWeb();
        bool finalizeAndroid();
        bool finalizeIOS();
        // R7: <dest>.staging-<pid> swapped in, the old build moved aside and deleted after
        bool swapStagingIntoPlace();
        void discardStaging();
        void warn(const std::string& code, const std::string& message);
        // where the content steps' shaderlib notices come from (copyEngine's copy, else the editor's)
        fs::path shaderlibSourceDir() const;
        // the engine sources a build's notices come from (the export's engine copy, or the editor's)
        fs::path engineSourceDir() const;
        // returns once the export started by startExport has ended
        void waitForExport();

        bool isCancelled() const;

        void resolveShaderKeys();
        bool loadShaderWarmup();

        fs::path getExportProjectRoot() const;
        // Canonical directory copyAssets() reads from
        fs::path getAssetsSourceDir() const;
        static bool shouldSkipExportSupportFile(const fs::path& relativePath);
        static bool isCppHeaderFile(const fs::path& path);
        static bool isCppSourceFile(const fs::path& path);
        static bool isLuaExportFile(const fs::path& path);
        static bool isLuaSourceFile(const fs::path& path);

        std::string getAppName() const;
        fs::path getBuildCacheDir() const;
        // The compile tree every project with this build's kit and engine settings shares (the
        // engine and vendored objects built once), in the user cache; docs/specs/S33-build-speed.md R11
        fs::path getSharedBuildCacheDir() const;
        // Generator cmake will actually use: the project kit's, or with no
        // explicit -G, the CMAKE_GENERATOR environment variable cmake honors.
        std::string getEffectiveGenerator() const;

        bool checkTargetDir();
        bool prepareCacheTarget();
        bool configureBuild();
        bool runBuild();
        bool collectDesktopArtifacts();
        bool collectWebArtifacts();
        // the pthreads build next to the single-thread one, the cross-origin isolation
        // service worker, and the page's choice between the two
        bool collectWebThreadsArtifacts();

        // the build tree configureBuild and runBuild work in ("build", or "build-threads")
        std::string buildDirName = "build";
        // CMake options of that tree beyond the export's
        std::string extraCMakeArgs;
        bool collectSourceResourcePack();
        bool collectPackedTreeEntries(const fs::path& dir, std::vector<fs::path>& out, const fs::path& keep = fs::path());
        bool removeResourcePack(const fs::path& packPath);
        bool writeNativeResourcePack(const fs::path& outputPath, bool& created);
        bool clearGenerated();
        // Test hook (JASPER_TEST_EXPORT_HOLD=<path>): waits after the clear while <path> exists
        void holdForTest();
        bool loadAndSaveAllScenes();
        // runs checkHtmlUIReferences(...) on htmlUIDocuments; setError on a broken reference
        bool checkHtmlUIReferences();
        // S37 R24: an event whose clip is missing is a build warning naming both
        void checkSoundEventClips();
        bool copyGenerated();
        // S14: every texture source cooked for the families of the export, into
        // assets/je-cooked/<family>; a cook error fails the export, naming the file
        bool cookTextures();
        // S53: every model source cooked for the families of the export, into
        // assets/je-cooked/models; a cook error fails the export, naming the file
        bool cookModels();
        // False, with the error set, when an asset sits where the engine loads an FBX's import
        // from (<relPath>.glb, relative to the assets)
        bool checkFbxImportPathFree(const fs::path& relPath);
        // S53 R21: the build's content (`contentRoot`'s assets and lua trees, before packing),
        // its engine files, and the report written next to the export
        void measureBuildContent(const fs::path& contentRoot);
        void measureBuildEngine(const fs::path& root);
        bool writeBuildReport(const fs::path& outputDir, const std::string& method);
        // What walkTree() does after visiting an entry: enter it (a folder; a file goes on to the
        // next entry), skip what it holds, or stop the walk
        enum class TreeStep { Enter, Skip, Stop };
        using TreeVisitor = std::function<TreeStep(const fs::directory_entry& entry, const fs::path& relPath, bool isDirectory, const fs::path& real)>;
        // A tree as the editor reaches it (S68 R1, R2): each folder listed, a link of any kind entered
        // as a folder under its own name, so relPath (never canonicalized) is the path a scene or
        // script reads the file by; real is where the entry is on disk. A link to a folder that holds
        // it (it would hold itself) is left out with a warning. ec: the root could not be listed.
        void walkTree(const fs::path& root, const TreeVisitor& visit, std::error_code& ec);
        // False, with the error set, when a trusted root of the project leads elsewhere than when it
        // was pinned: the game would ship whatever is there now
        bool checkRootsPinned();
        bool copyAssets();
        // a .cube LUT cooked to <destination's name>.jlut; false (warned) when it does not parse
        bool cookColorLut(const fs::path& source, const fs::path& destination);
        // S04: a reference of the build's scenes (and the prefabs and materials they use) that
        // resolves to no asset fails the export, listing each one, unless allowMissingAssets
        bool checkMissingAssets();
        // S04: assetmap.bin at the exported assets root, the runtime's GUID <-> path map
        bool writeAssetMap();
        // Runtime shader compilation builds a fork's variants from its sources, which it
        // reads from the build's assets at their project-relative paths: a script can
        // put any fork of the project on a mesh, so all of them ship
        bool copyCustomShaderSources();
        bool copyLua();
        bool copyCppScripts();
        // plugins/native/{android,apple,ios,macos,web} -> <export>/project/plugins/native/...
        // (mirrored: what the project no longer has goes); cpp/ ships as a script root
        bool copyNativePlugins();
        bool copyEngine();
        bool writeExportSettingsScript(std::string& cmakeContent);
        bool writeAndroidProjectSettings();
        bool writeAppleProjectSettings();
        // The exported project's sources (scenes, prefabs, scene_scripts.cpp, C++ scripts) in
        // the Xcode app targets, with the include directories CMake gives the game target
        bool addXcodeProjectSources(std::string& xcodeProject);
        // The project's C++ standard and the scene capacities, as the CMake build sets them
        bool writeXcodeBuildSettings(std::string& xcodeProject);
        // native plugins: plugins/native/{apple,ios,macos} sources and SDKs, system frameworks,
        // Swift packages, Swift settings, entitlements, deployment target
        bool addNativePluginsToXcode(std::string& xcodeProject);
        // workspaces/xcode: the entitlements file and the CocoaPods Podfile (removed when unset)
        bool writeNativeAppleFiles();
        bool writeWindowsResourceFile(bool includeIcon);
        bool writeAppIcon();
        bool buildAndSaveShaders();
        // The third-party notices the shipped shaders need, under licenses/ (each removed when
        // not needed): shaders.com-LICENSE.txt when the shaders contain a UI effect (phase-5
        // spec 3.16), webgl-fluid-simulation-LICENSE.txt when they run Pavel Dobryakov's fluid
        // (shipsWebGLFluid). copyLicenses: the build's to the destination.
        // licenses/post-fx-LICENSE.txt likewise when they contain an uber pass with the VHS tape,
        // film damage or the Lens / Filmic grain (S13: adapted MIT shader code)
        bool writeEffectLicense();
        bool copyLicenses();
        // licenses/THIRD-PARTY-AUDIO.txt (with its build warnings) when a shipped clip has a provenance
        bool writeAudioCredits();
        // assets/je-audio-imports.json when any shipped clip has import settings
        bool writeAudioImports();
        // the paths the export writes the notices to, relative to its root
        static const char* effectLicensePath();
        static const char* fluidLicensePath();
        // Pavel Dobryakov's WebGL-Fluid-Simulation (MIT) ships when either of these does: the
        // native FluidSimulation component (its FLUID variants: a scene or prefab of the export
        // has one) or the HTML UI `fluid` effect (its UI_EFFECT variants)
        bool shipsWebGLFluid() const;
        bool shipsFluidComponent() const;
        bool shipsFluidEffect() const;
        static const char* postFxLicensePath();
        static const char* openEngineLicensePath();

        // the first scene or prefab file with a Video Player playing an http(s) url ("" for none:
        // Android exports then need no INTERNET for videos)
        std::string videoUrlScene() const;
        // Aggregates SceneMaxValues across all project scenes into the capacities that
        // size the engine's fixed-capacity HybridArrays (MAX_SUBMESHES, MAX_SPRITE_FRAMES, ...).
        std::vector<std::pair<std::string, unsigned int>> getSceneMaxValues() const;
        static std::vector<std::pair<std::string, unsigned int>> sceneMaxValuesOf(const Project* project);
        // add_definitions() block applying them to the export.
        std::string buildSceneMaxValuesDefinitions() const;
        // Project-relative form of a script root, empty when it falls outside the project.
        fs::path scriptDirRelativePath(const fs::path& scriptDir) const;
        // Script roots inside the exported project, as copyCppScripts() lays them out.
        std::vector<std::string> getExportedScriptRoots() const;
        // target_include_directories() call putting the script roots on the exported game target.
        std::string buildScriptDirIncludes() const;

        static void copyTree(const fs::path& src, const fs::path& dst, std::error_code& ec);
        static void copyTreeIfChanged(const fs::path& src, const fs::path& dst, std::error_code& ec);

    public:
        // a scene or prefab file's text: a Video Player with an http(s) url
        static bool hasUrlVideoPlayer(const std::string& sceneText);
        // the first of `files` (relative to `root`: Lua, C++ scripts, HTML UI documents) that may
        // play a URL video (sets a Video Player's url, an HTML <video> of an http(s) URL); ""
        static std::string scriptVideoUrlFile(const std::filesystem::path& root, const std::vector<std::filesystem::path>& files);

        // a PNG copied (read once) without its jasper.* text chunks (util/PngEditorChunks.h; one
        // without them, or not a well-formed PNG, as it is); false when it is not a .png or could
        // not be read or written this way (the caller then copies the file)
        static bool copyPngWithoutEditorChunks(const std::filesystem::path& source, const std::filesystem::path& destination);
        // What the HTML/CSS of each UI Document refers to (HtmlUIDependencies): a missing
        // file, a path outside the assets directory or a remote URL is an error with
        // file:line:column (project-relative); an absolute path is a warning. documents =
        // {where it is used, asset-relative filename}. False when there are errors.
        static bool checkHtmlUIReferences(const fs::path& projectPath, const fs::path& assetsPath,
                                          const std::vector<std::pair<std::string, std::string>>& documents,
                                          std::vector<std::string>& errors, std::vector<std::string>& warnings);

        Exporter();
        ~Exporter();

        void startExport(Project* project, const ExportConfig& config);
        // the last Player build's report
        ExportResult getResult() const;
        // R5: Prebuilt unless the project has C++ script sources, a ProjectBuild.cmake, capacities
        // above the player's, a feature the player lacks, or the user forced Compile. `player`
        // may be null (no runtime installed: the capacities are the engine defaults)
        static BuildMethodChoice chooseBuildMethod(Project* project, const ExportConfig& config, const PlayerInfo* player);
        // the runtime id a Player build of this config uses
        static std::string playerIdFor(const ExportConfig& config, bool development);
        bool exportProject(Project* project, const ExportConfig& config);
        bool generateShaders(const ExportConfig& config);
        void cancelExport();
        ExportProgress getProgress() const;
        bool isRunning() const;

        // Sets every CLANG_CXX_LANGUAGE_STANDARD of an Xcode project to gnu++<standard>;
        // false when the project has none
        static bool applyXcodeCxxStandard(std::string& pbxproj, int standard);
        static std::string getShaderDisplayName(ShaderType type, uint64_t properties, uint16_t customId = 0);
        static std::string getCMakeGraphicBackend(::jasperengine::ShaderBackend backend);
        static EmsdkInfo detectEmsdk(const std::string& overridePath);
        // the manifest names of the Android permission keys and orientations (a Compile build's
        // AndroidManifest.xml and a prebuilt package's alike)
        static const char* androidPermissionName(const std::string& key);
        static std::string escapeXml(const std::string& value);
        // a prebuilt web page: the markers filled, the head additions of the Compile shell, the loader tag
        static std::string composeWebPage(std::string page, const std::string& title, const std::string& headInclude, bool resize,
                                          bool hideUi, bool favicon, int canvasWidth, int canvasHeight, bool threads);
        // What emcmake.py adds to a CMake configure: the Emscripten toolchain file, node as the
        // cross-compiling emulator, and on Windows a generator (emscriptenWindowsGeneratorArgs).
        // `env` is the environment CMake runs in (emscriptenEnvironment). error set when it can't.
        static std::vector<std::string> emscriptenCMakeArgs(const EmsdkInfo& sdk, bool generatorGiven,
                                                            const CommandRunner::Environment& env, std::string& error);
        // emcmake.py's Windows generator choice (platform-independent, for tests): nothing when a
        // generator is given or CMAKE_GENERATOR is set in `env`; else -G "MinGW Makefiles" when
        // mingw32-make is on env's PATH (';'-separated), else -G Ninja when ninja is; else error.
        static std::vector<std::string> emscriptenWindowsGeneratorArgs(const CommandRunner::Environment& env, bool generatorGiven,
                                                                       std::string& error);
        // Windows Web builds: the first file or folder below `roots` (what the build copies or
        // compiles: assets, Lua, C++ script folders) whose path holds '%' or '"' (emcc.bat runs
        // through cmd.exe), as a refusal; "" when none. At most maxEntries are looked at: more is
        // refused too (it can't be checked). Platform-independent; webTreeRefusal applies it on
        // Windows only.
        static std::string webTreePathRefusal(const std::vector<fs::path>& roots, size_t maxEntries = 200000);
        static std::string webTreeRefusal(const std::vector<fs::path>& roots);
        // The folders a Web build of `project` takes files from (assets, Lua, C++ script folders)
        static std::vector<fs::path> webSourceRoots(Project* project);
        // The environment the SDK's tools run in: this process's, with the SDK folders first on
        // PATH and EMSDK / EM_CONFIG / EMSDK_NODE / EMSDK_PYTHON set (what emsdk_env sets)
        static CommandRunner::Environment emscriptenEnvironment(const EmsdkInfo& sdk);
        // A Web build on Windows still runs emcc.bat (the SDK's compiler driver) through cmd.exe,
        // via CMake / Ninja / make, and cmd.exe expands %VAR% even inside quotes: "" when none of
        // `paths` (the project, the build tree, the output folder) holds '%' or '"', else the
        // refusal naming the first that does. Platform-independent; applied on Windows only
        // (webBuildRefusal).
        static std::string webPathRefusal(const std::vector<fs::path>& paths);
        // webPathRefusal on Windows, "" elsewhere
        static std::string webBuildRefusal(const std::vector<fs::path>& paths);
        // The build tree of `project`'s exports in `mode` (<internal>/export/desktop|web)
        static fs::path buildCacheDirFor(Project* project, ExportMode mode);

        // The Desktop player the last export produced (destination folder / name fixed when the
        // export started); empty when none was.
        fs::path getProducedExecutable() const;
    };

}
