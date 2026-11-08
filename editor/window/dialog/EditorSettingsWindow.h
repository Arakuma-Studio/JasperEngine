// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "players/PlayerRuntimes.h"

#include <map>
#include <vector>

#include "AppSettings.h"
#include "Exporter.h"
#include "Generator.h"
#include "imgui.h"

#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    class EditorSettingsWindow {
    private:
        bool m_isOpen = false;

        // Build settings are per project, so the dialog edits the open project's
        Project* m_project = nullptr;

        std::vector<CMakeKit> m_availableKits;
        int m_cmakeKitIndex = 0;
        std::string m_cmakeOverride;
        CMakeInfo m_cmakeInfo;
        std::string m_cmakePickError;

        std::string m_emsdkOverride;
        EmsdkInfo m_emsdkInfo;
        // the video importer's ffmpeg (phase 6): the override and what the lookup finds with it
        std::string m_ffmpegOverride;
        // S30: the shipping tools (AppSettings external tool paths) and the installed player runtimes
        std::map<std::string, std::string> m_toolPaths;
        std::vector<PlayerInfo> m_playerRuntimes;
        std::string m_playerRuntimeStatus;
        std::string m_ffmpegStatus;
        bool m_ffmpegUsable = false;
        bool m_editorVSyncEnabled = true;
        bool m_projectHubAlways = false;
        int m_uiScalePercent = 100;
        bool m_autosaveEnabled = true;
        int m_autosaveInterval = 60;
        bool m_networkOffline = false;
        bool m_htmlUiAutoImport = true;
        // Asset Pipeline (S05 R26)
        bool m_autoRefresh = true;
        int m_fileWatcher = 0;            // 0 Auto, 1 Native, 2 Polling
        float m_pollingInterval = 1.0f;   // seconds
        bool m_logReloads = true;
        // Scripting (S23 R8, R10) and External Tools > External Script Editor (R28)
        int m_scriptChangesWhilePlaying = 0;   // 0 reload, 1 after_stop, 2 stop_and_reload
        bool m_reloadCppDuringPlay = false;
        int m_externalScriptEditor = 0;        // ExternalCodeEditor::choices() index
        std::string m_externalScriptEditorCommand;
        bool m_htmlUiVerboseDiagnostics = false;
        bool m_htmlUiPatchReload = true;
        float m_codeEditorFontSize = 0.0f;
        int m_undoMaxSteps = AppSettings::defaultUndoMaxSteps;
        int m_undoMemoryLimitMB = AppSettings::defaultUndoMemoryLimitMB;
        bool m_undoRecordSelection = true;
        bool m_undoReviewPlayChanges = true;
        LoggingSettings m_logging;
        DiagnosticsSettings m_diagnostics;
        VersionControlSettings m_versionControl;
        std::string m_smartMergeStatus;
        LatticeViewSettings m_lattice;
        SceneViewSettings m_sceneView;
        // Unity-style category list; indexes PreferencesCategory in the .cpp.
        int m_category = 0;
        char m_categorySearch[64] = {0};
        std::filesystem::path m_defaultExportDirectory;
        std::string m_cacheStatus;

        LocalBuildSettings projectBuildSettings() const;
        void refreshCMakeStatus();
        void refreshEmsdkStatus();
        void refreshFfmpegStatus();
        void drawSettings();
        void drawGeneralSettings();
        void drawCodeEditorSettings();
        void drawAssetPipelineSettings();
        void drawScriptingSettings();
        void drawExternalScriptEditorSettings();
        void drawUndoSettings();
        void drawLatticeSettings();
        void drawSceneViewSettings();
        void drawExternalToolsSettings();
        // Row drawers for the External Tools table.
        void drawCMakeSettings();
        void drawWebSettings();
        void drawVideoImportSettings();
        void drawShippingSettings();
        void drawAsepriteSettings();
        void refreshPlayerRuntimes();
        void drawLoggingSettings();
        void drawDiagnosticsSettings();
        void drawVersionControlSettings();
        void drawAdvancedSettings();
        bool applySettings();

    public:
        // A folder in the file manager, or a file in its default application
        static void openInFileManager(const std::filesystem::path& folder);
        void open(Project* project);
        // Shows a page by its name ("General", "Logging" ...); false for an unknown one
        bool showPage(const std::string& name);
        const char* pageName() const;
        void close() { m_isOpen = false; }
        // on the page named `category` (as the category list shows it; unknown: the first)
        void open(Project* project, const std::string& category);
        void show();
        bool isOpen() const { return m_isOpen; }
    };

}
