// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "AppSettings.h"
#include "Exporter.h"
#include "ScenesWindow.h"
#include "imgui.h"

#include <string>
#include <filesystem>
#include <functional>
#include <set>
#include <vector>

namespace jasperengine::editor {

    namespace fs = std::filesystem;

    class ExportWindow {
    private:
        // Unity-style Build Settings: scenes in build, a platform list and the
        // selected platform's settings, then the export progress.
        enum class Step {
            Settings,
            Progress
        };

        // Each platform maps onto an export mode: the host desktop and Web can
        // build a runnable player, every platform can export its C++ source.
        enum class BuildPlatform {
            Windows,
            MacOS,
            Linux,
            Android,
            IOS,
            Web,
            Custom,
            Count
        };

        bool m_isOpen = false;
        Step m_step = Step::Settings;
        ExportMode m_mode = ExportMode::SourceCode;
        Project* m_project = nullptr;

        BuildPlatform m_platform = BuildPlatform::Linux;
        // Host desktop / Web: build a player (true) or export the source project.
        bool m_buildPlayer = true;
        bool m_platformInitialized = false;
        uint32_t m_selectedSceneId = NULL_PROJECT_SCENE;
        // Build And Run: launch the built player once the export finishes.
        bool m_runAfterBuild = false;
        bool m_launchDone = false;
        bool m_playerSettingsRequested = false;
        // Nested inside this modal, so it can open without closing Build Settings.
        ScenesWindow m_scenesWindow;

        // UI state
        fs::path m_targetDir;
        bool m_targetDirFromDefault = false;
        bool m_projectSettingsDirty = false;

        // Start scene
        uint32_t m_startSceneId = NULL_PROJECT_SCENE;

        // Shader list includes excluded entries so they can be restored.
        struct ShaderEntry {
            ShaderKey key;
            ShaderType type;
            uint32_t properties;
            std::string displayName;
            bool fromScene = false;
        };
        std::vector<ShaderEntry> m_shaderEntries;
        int m_selectedShaderIndex = -1;
        std::set<ShaderKey> m_shaderAdditions;
        std::set<ShaderKey> m_shaderExclusions;

        // Add Shader dialog state
        bool m_addShaderOpen = false;
        int m_addShaderTypeIndex = 0;
        bool m_addShaderProps[32] = {};

        // Graphic backend selection (Source Code mode only)
        struct BackendEntry {
            ShaderBackend backend;
            std::string name;
            bool selected;
        };
        std::vector<BackendEntry> m_backendEntries;
        bool m_sourceBackendsConfigured = false;
        bool m_selectSourceBackendTab = true;
        bool m_sourcePlatformWindows = false;
        bool m_sourcePlatformLinux = false;
        bool m_sourcePlatformMacOS = false;
        bool m_sourcePlatformIOS = false;
        bool m_sourcePlatformAndroid = false;
        bool m_sourcePlatformWeb = false;
        std::set<ShaderBackend> m_sourcePresetBackends;

        // Desktop backend selection. The entries themselves are host-specific
        // and live in ExportWindow.cpp.
        int m_graphicBackendIndex = 0;
        bool m_desktopBackendConfigured = false;
        int m_desktopBuildJobs = 1;
        LocalBuildSettings m_buildSettings;    // mirrors AppSettings::getBuildSettings()

        // Cached tool checks: both spawn processes, so never probe per-frame.
        std::string m_emsdkOverride;           // mirrors AppSettings::getEmsdkPath()
        EmsdkInfo m_emsdkInfo;
        std::string m_missingBuildTools;       // Generator::checkBuildTools() result

        Exporter m_exporter;

        const char* localExportMode() const;
        void refreshShaderSelection();
        void includeShader(ShaderKey key);
        void populateBackendList();
        void refreshEmsdkStatus();
        void selectMode(ExportMode mode);
        void loadSettingsFromProject();
        bool saveCurrentSettingsToProject(bool saveProjectFile = true);

        static BuildPlatform hostPlatform();
        static const char* platformName(BuildPlatform platform);
        static const char* platformIcon(BuildPlatform platform);
        bool platformCanBuildPlayer(BuildPlatform platform) const;
        std::set<ShaderBackend> platformBackends(BuildPlatform platform) const;
        void selectPlatform(BuildPlatform platform, bool buildPlayer);
        void setStartScene(uint32_t sceneId);
        void drawScenesInBuild(float height);
        void drawPlatformList();
        bool drawPlatformSettings(bool& targetNotEmpty);
        void drawBuildTypeRow();
        void drawSourceBackendsRow();
        fs::path builtPlayerPath() const;
        void launchBuiltPlayer();
        void drawSettings();
        void drawOutputDirRow(const char* label);
        void drawGraphicBackendRow();
        void drawDesktopKitRows();
        void drawShaderSection();
        void drawBackendSection();
        void applySourcePlatformPresets();
        void drawProgress();
        void drawAddShaderDialog();
        void drawOverwriteConfirmDialog();
        void startConfiguredExport(bool overwriteTarget);

    public:
        void open(Project* project);
        void show();
        bool isOpen() const { return m_isOpen; }
        // Player Settings... closes Build Settings and asks for Project Settings.
        bool consumePlayerSettingsRequest();
    };

}
