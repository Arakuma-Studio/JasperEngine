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
        bool m_editorVSyncEnabled = true;
        float m_codeEditorFontSize = 0.0f;
        // Unity-style category list; indexes PreferencesCategory in the .cpp.
        int m_category = 0;
        char m_categorySearch[64] = {0};
        std::filesystem::path m_defaultExportDirectory;
        std::string m_cacheStatus;

        LocalBuildSettings projectBuildSettings() const;
        void refreshCMakeStatus();
        void refreshEmsdkStatus();
        void drawSettings();
        void drawGeneralSettings();
        void drawCodeEditorSettings();
        void drawExternalToolsSettings();
        // Row drawers for the External Tools table.
        void drawCMakeSettings();
        void drawWebSettings();
        void drawAdvancedSettings();
        bool applySettings();

    public:
        void open(Project* project);
        void show();
        bool isOpen() const { return m_isOpen; }
    };

}
