// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Project.h"
#include "shader/ShaderBuilder.h"
#include "ShaderDataSerializer.h"
#include "util/CommandRunner.h"

#include <filesystem>
#include <set>
#include <vector>
#include <string>
#include <functional>
#include <atomic>
#include <mutex>
#include <thread>

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
        Web
    };

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
        // Desktop/Web: value passed to the exported CMake GRAPHIC_BACKEND
        // setting (glcore, gles3, d3d11, metal, or vulkan).
        std::string graphicBackend;
        // Native builds: pack assets/lua into resources.pak instead of copying folders.
        // Experimental and disabled by default.
        bool packNativeResources = false;
        // Web: emsdk root override ("" = auto-detect via EMSDK env / PATH)
        std::string emsdkPath;
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

        std::thread exportThread;
        // the app (player) name, fixed when the export starts: a rename while it builds can't change
        // which file is produced and recorded
        std::string fixedAppName;
        fs::path producedExecutable;             // guarded by progressMutex

        ShaderBuilder shaderBuilder;
        CommandRunner commandRunner;

        // The UI Documents of the exported scenes, collected while loadAndSaveAllScenes() has
        // every scene loaded, for checkHtmlUIReferences()
        struct HtmlUIDocumentUse {
            std::string scene;
            std::string entity;
            std::string filename;   // asset-relative
        };
        std::vector<HtmlUIDocumentUse> htmlUIDocuments;

        // Lua sources copyLua() shipped, so copyAssets() can skip them
        std::set<fs::path> luaCopiedSources;
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

        bool isCancelled() const;

        void resolveShaderKeys();

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
        // Generator cmake will actually use: the project kit's, or with no
        // explicit -G, the CMAKE_GENERATOR environment variable cmake honors.
        std::string getEffectiveGenerator() const;

        bool checkTargetDir();
        bool prepareCacheTarget();
        bool configureBuild();
        bool runBuild();
        bool collectDesktopArtifacts();
        bool collectWebArtifacts();
        bool collectSourceResourcePack();
        bool collectPackedTreeEntries(const fs::path& dir, std::vector<fs::path>& out, const fs::path& keep = fs::path());
        bool removeResourcePack(const fs::path& packPath);
        bool writeNativeResourcePack(const fs::path& outputPath, bool& created);
        bool clearGenerated();
        bool loadAndSaveAllScenes();
        // runs checkHtmlUIReferences(...) on htmlUIDocuments; setError on a broken reference
        bool checkHtmlUIReferences();
        bool copyGenerated();
        bool copyAssets();
        // Runtime shader compilation builds a fork's variants from its sources, which it
        // reads from the build's assets at their project-relative paths: a script can
        // put any fork of the project on a mesh, so all of them ship
        bool copyCustomShaderSources();
        bool copyLua();
        bool copyCppScripts();
        bool copyEngine();
        bool writeExportSettingsScript(std::string& cmakeContent);
        bool writeAndroidProjectSettings();
        bool writeAppleProjectSettings();
        // The exported project's sources (scenes, prefabs, scene_scripts.cpp, C++ scripts) in
        // the Xcode app targets, with the include directories CMake gives the game target
        bool addXcodeProjectSources(std::string& xcodeProject);
        // The project's C++ standard and the scene capacities, as the CMake build sets them
        bool writeXcodeBuildSettings(std::string& xcodeProject);
        bool writeWindowsResourceFile(bool includeIcon);
        bool writeAppIcon();
        bool buildAndSaveShaders();
        // The third-party notices the shipped shaders need, under licenses/ (each removed when
        // not needed): shaders.com-LICENSE.txt when the shaders contain a UI effect (phase-5
        // spec 3.16), webgl-fluid-simulation-LICENSE.txt when they run Pavel Dobryakov's fluid
        // (shipsWebGLFluid). copyLicenses: the build's to the destination.
        bool writeEffectLicense();
        bool copyLicenses();
        // the paths the export writes the notices to, relative to its root
        static const char* effectLicensePath();
        static const char* fluidLicensePath();
        // Pavel Dobryakov's WebGL-Fluid-Simulation (MIT) ships when either of these does: the
        // native FluidSimulation component (its FLUID variants: a scene or prefab of the export
        // has one) or the HTML UI `fluid` effect (its UI_EFFECT variants)
        bool shipsWebGLFluid() const;
        bool shipsFluidComponent() const;
        bool shipsFluidEffect() const;

        // Aggregates SceneMaxValues across all project scenes into the capacities that
        // size the engine's fixed-capacity HybridArrays (MAX_SUBMESHES, MAX_SPRITE_FRAMES, ...).
        std::vector<std::pair<std::string, unsigned int>> getSceneMaxValues() const;
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
        bool exportProject(Project* project, const ExportConfig& config);
        bool generateShaders(const ExportConfig& config);
        void cancelExport();
        ExportProgress getProgress() const;
        bool isRunning() const;

        static std::string getShaderDisplayName(ShaderType type, uint64_t properties, uint16_t customId = 0);
        static std::string getCMakeGraphicBackend(::jasperengine::ShaderBackend backend);
        static EmsdkInfo detectEmsdk(const std::string& overridePath);
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
