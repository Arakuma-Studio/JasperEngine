// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

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
        std::string m_ffmpegStatus;
        bool m_ffmpegUsable = false;
        bool m_editorVSyncEnabled = true;
        bool m_autosaveEnabled = true;
        int m_autosaveInterval = 60;
        static void openInFileManager(const std::filesystem::path& folder);
        bool m_htmlUiAutoImport = true;
        bool m_htmlUiVerboseDiagnostics = false;
        float m_codeEditorFontSize = 0.0f;
        int m_undoMaxSteps = AppSettings::defaultUndoMaxSteps;
        int m_undoMemoryLimitMB = AppSettings::defaultUndoMemoryLimitMB;
        bool m_undoRecordSelection = true;
        bool m_undoReviewPlayChanges = true;
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
        void drawUndoSettings();
        void drawExternalToolsSettings();
        // Row drawers for the External Tools table.
        void drawCMakeSettings();
        void drawWebSettings();
        void drawVideoImportSettings();
        void drawAdvancedSettings();
        bool applySettings();

    public:
        void open(Project* project);
        void show();
        bool isOpen() const { return m_isOpen; }
    };

}
