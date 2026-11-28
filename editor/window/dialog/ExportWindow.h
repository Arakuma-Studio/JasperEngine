// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "AppSettings.h"
#include "Exporter.h"
#include "ScenesWindow.h"
#include "BuildProfilesPane.h"
#include "players/PlayerRuntimes.h"
#include "platform/DeviceList.h"
#include "publish/Publishing.h"
#include "imgui.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <utility>
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
        // the project the remembered platform belongs to: another project opens on its own default
        fs::path m_platformProjectPath;
        uint32_t m_selectedSceneId = NULL_PROJECT_SCENE;
        // Build And Run: launch the built player once the export finishes.
        bool m_runAfterBuild = false;
        bool m_allowMissingAssets = false;
        bool m_iosSimulator = false;
        bool m_launchDone = false;
        // the completion page's Build Report, read once per build (a report lists every file)
        std::filesystem::path m_shownReport;
        bool m_shownReportLoaded = false;
        BuildReport m_shownReportData;
        std::string m_shownReportSince;   // "(+3.2 MB since 2026-11-18 10:15)", "" without an earlier one
        bool m_playerSettingsRequested = false;
        // Nested inside this modal, so it can open without closing Build Settings.
        ScenesWindow m_scenesWindow;
        // Build Profiles: the list under the platforms and the selected profile's pane; a build of
        // the selected profile (Build and Build And Run) instead of the platform's
        BuildProfilesPane m_profiles;

        // UI state
        fs::path m_targetDir;
        // Exporter::outputFolderRefusal of m_targetDir, computed when it changes
        fs::path m_refusalCheckedDir;
        std::string m_outputRefusal;
        bool m_targetDirFromDefault = false;
        bool m_projectSettingsDirty = false;

        // Start scene
        uint32_t m_startSceneId = NULL_PROJECT_SCENE;

        // Shader list includes excluded entries so they can be restored.
        struct ShaderEntry {
            ShaderKey key;
            ShaderType type;
            uint64_t properties;
            std::string displayName;
            bool fromScene = false;
        };
        std::vector<ShaderEntry> m_shaderEntries;
        ShaderListStatus m_shaderListStatus;
        int m_selectedShaderIndex = -1;
        std::set<ShaderKey> m_shaderAdditions;
        std::set<ShaderKey> m_shaderExclusions;

        // Add Shader dialog state
        bool m_addShaderOpen = false;
        int m_addShaderTypeIndex = 0;
        bool m_addShaderProps[SHADER_PROPERTY_BITS] = {};
        int m_addShaderEffectIndex = 0;   // UI Effect: the effect (EffectRegistry::all() order)

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

        // ---- S30: game builds for every platform (prebuilt player or compiled) ----
        using ShipPlatform = ::jasperengine::editor::BuildPlatform;
        ShipPlatform shipPlatform() const;
        static ShipPlatform shipPlatformOf(BuildPlatform platform);
        static BuildPlatform listPlatformOf(ShipPlatform platform);
        void drawTargetPlatformNotice();
        std::map<int, int> m_playerBackendIndex;      // per platform: the graphics API this session
        std::string playerBackend() const;            // its runtime name (glcore, d3d11, ...)
        BuildMethodChoice m_methodChoice;
        std::optional<PlayerInfo> m_playerRuntime;
        std::string m_playerRuntimeId;
        std::string m_playerRuntimeError;
        std::vector<std::string> m_androidMissing;    // the SDK tools a prebuilt Android build lacks
        std::string m_iosXcodeMissing;                // why this computer cannot build the Xcode project
        // the installed Android runtime's native libraries per selected ABI (the package sizes shown)
        std::vector<std::pair<std::string, uint64_t>> m_androidLibrarySizes;
        void refreshPlayerState();
        void drawPlayerRows();
        bool playerBuildBlocked(std::string& why) const;

        // Build Settings > Download: one runtime at a time, off the UI thread
        struct RuntimeDownload {
            std::thread thread;
            std::atomic<bool> running{false};
            std::atomic<bool> cancel{false};
            std::atomic<uint64_t> received{0};
            uint64_t size = 0;
            std::string id;
            std::mutex mutex;
            std::string message;
            bool failed = false;
        };
        std::unique_ptr<RuntimeDownload> m_download;
        void startRuntimeDownload(const std::string& id);
        void drawRuntimeDownload();
        void finishBackgroundWork();

        // ---- Build And Run on a device (docs/specs/build-and-run-on-device.md R18) ----
        // probed off the UI thread: when an Android or iOS platform shows, on Refresh, every 3 s
        struct DeviceProbe {
            std::thread thread;
            std::atomic<bool> done{false};
            ShipPlatform platform = ShipPlatform::Android;
            std::vector<platform::RunDevice> devices;
        };
        std::unique_ptr<DeviceProbe> m_deviceProbe;
        std::vector<platform::RunDevice> m_runDevices;    // of m_runDevicesPlatform
        ShipPlatform m_runDevicesPlatform = ShipPlatform::Linux;
        double m_lastDeviceProbe = -1.0;
        // the Run Device per platform (devices.yaml, per user: no undo, as the selected platform)
        std::map<std::string, std::string> m_selectedDevices;
        bool m_selectedDevicesLoaded = false;
        // Patch And Run's availability for the selected device, computed when the list or the choice changes
        bool m_patchAvailable = false;
        std::string m_patchWhyNot;
        bool m_patchStateDirty = true;
        bool m_patchRun = false;                         // the build running is Patch And Run
        ExportConfig m_lastExportConfig;                 // the build running, for its device run
        std::thread m_deviceRunThread;
        ShipPlatform runPlatform() const;                // the platform Build And Run builds (a profile's)
        void pollRunDevices(bool force);
        void drawRunDeviceRow();
        const platform::RunDevice* selectedRunDevice() const;
        bool runDeviceUsable(std::string& why) const;
        void refreshPatchState();
        void applyRunDevice(ExportConfig& config, bool patch);
        void startDeviceRun();

        // the keystore passwords an Android build asks for (never saved)
        bool m_askKeystorePasswords = false;
        bool m_pendingOverwrite = false;
        char m_keystorePassword[256] = {};
        char m_keyPassword[256] = {};
        void drawKeystorePasswordDialog();

        // ---- the Publish tab (publishing.json, releases, Steam, the Deck) ----
        struct PublishJob {
            std::thread thread;
            std::atomic<bool> running{false};
            std::atomic<bool> cancel{false};
            std::string title;
            std::mutex mutex;
            std::string message;
            bool failed = false;
            bool done = false;
        };
        std::unique_ptr<PublishJob> m_publishJob;
        publish::PublishingConfig m_publishing;
        bool m_publishingLoaded = false;
        bool m_publishingExists = false;
        bool m_publishingDirty = false;
        std::string m_publishingError;
        char m_releaseName[64] = {};
        char m_steamBranch[64] = {};
        char m_steamUser[64] = {};
        bool m_steamPreview = true;
        char m_newTargetName[64] = {};
        std::string m_deckTarget;
        bool m_confirmUpload = false;
        void loadPublishing();
        bool savePublishing();
        void drawPublishTab();
        void drawPublishTargets();
        void drawPublishActions();
        void startPublishJob(const std::string& title, std::function<bool(const publish::PublishTools&, std::string&)> work);
        void drawBuildTab();

        const char* localExportMode() const;
        void refreshShaderSelection();
        void includeShader(ShaderKey key);
        void applyShaderOverrides(const std::set<ShaderKey>& additions, const std::set<ShaderKey>& exclusions);
        void syncShaderOverrides(bool force);
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
        // the union of the presets of the platforms ticked on the Source Code page
        std::set<ShaderBackend> selectedSourcePlatformBackends() const;
        void selectPlatform(BuildPlatform platform, bool buildPlayer);
        void setStartScene(uint32_t sceneId);
        void drawScenesInBuild(float height);
        void drawPlatformList();
        bool drawPlatformSettings(bool& targetNotEmpty);
        void drawBuildTypeRow();
        void drawDevelopmentBuildRow();
        void drawSourceBackendsRow();
        fs::path builtPlayerPath() const;
        void launchBuiltPlayer();
        void drawSettings();
        void drawOutputDirRow(const char* label);
        void drawGraphicBackendRow();
        void drawDesktopKitRows();
        void drawShaderSection();
        // the cooked textures the platform ships: per family count, download and VRAM (S14 R25)
        void drawTextureSection();
        void drawBackendSection();
        void applySourcePlatformPresets();
        // a failed build's errors, each with a location a link that opens the file at the line
        void drawBuildErrors(const ExportProgress& progress);
        void drawProgress();
        void drawAddShaderDialog();
        void drawOverwriteConfirmDialog();
        void startConfiguredExport(bool overwriteTarget);

    public:
        ~ExportWindow();
        void open(Project* project);
        void show();
        bool isOpen() const { return m_isOpen; }
        bool isExporting() const { return m_exporter.isRunning(); }
        // Player Settings... closes Build Settings and asks for Project Settings.
        bool consumePlayerSettingsRequest();
    };

}
