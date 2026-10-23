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
        // Export anyway when a scene of the build references an asset that is missing (S04)
        bool allowMissingAssets = false;
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

    struct EmsdkInfo {
        bool found = false;
        std::string emcmake;                 // emcmake command or full path
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
        // the build's references to missing assets (S04), found while the scenes are loaded
        std::vector<std::string> missingBuildReferences;

        // Lua sources copyLua() shipped, so copyAssets() can skip them
        std::set<fs::path> luaCopiedSources;
        // Paths copyLua() wrote, relative to the lua tree, so copyAssets() can keep
        // Android's merged assets root free of paths both trees would ship
        std::set<fs::path> luaExportedPaths;

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
        bool copyEngine();
        bool writeExportSettingsScript(std::string& cmakeContent);
        bool writeAndroidProjectSettings();
        bool writeAppleProjectSettings();
        bool writeWindowsResourceFile(bool includeIcon);
        bool writeAppIcon();
        bool buildAndSaveShaders();
        // licenses/shaders.com-LICENSE.txt when the shaders contain a UI effect (phase-5 spec
        // 3.16); removed when they do not. copyLicenses: the build's to the destination.
        bool writeEffectLicense();
        bool copyLicenses();
        // the path the export writes the shaders.com notice to, relative to its root
        static const char* effectLicensePath();

        // Aggregates SceneMaxValues across all project scenes and builds the
        // add_definitions() block that sizes the engine's fixed-capacity
        // HybridArrays (MAX_SUBMESHES, MAX_SPRITE_FRAMES, ...) for the export.
        std::string buildSceneMaxValuesDefinitions() const;
        // Project-relative form of a script root, empty when it falls outside the project.
        fs::path scriptDirRelativePath(const fs::path& scriptDir) const;
        // target_include_directories() call putting the script roots on the exported game target.
        std::string buildScriptDirIncludes() const;

        static void copyTree(const fs::path& src, const fs::path& dst, std::error_code& ec);
        static void copyTreeIfChanged(const fs::path& src, const fs::path& dst, std::error_code& ec);

    public:
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

        // Sets every CLANG_CXX_LANGUAGE_STANDARD of an Xcode project to gnu++<standard>;
        // false when the project has none
        static bool applyXcodeCxxStandard(std::string& pbxproj, int standard);
        static std::string getShaderDisplayName(ShaderType type, uint32_t properties, uint16_t customId = 0);
        static std::string getCMakeGraphicBackend(::jasperengine::ShaderBackend backend);
        static EmsdkInfo detectEmsdk(const std::string& overridePath);
    };

}
