// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "diag/CrashHandler.h"
#include "util/CrashGroups.h"
#include "window/SheetPreviewTextures.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    // Window > Analysis > Crash Reports (docs/specs/shipped-game-diagnostics.md R35): the editor's
    // report folders or a game's reports root pulled from a device, one row per report or per group
    // of similar ones (util/CrashGroups), and the selected report's files. It changes no project
    // data (no undo); Mark Handled writes `handled` as the startup notice's Dismiss does.
    class CrashReportsWindow {
    public:
        static constexpr const char* WINDOW_NAME = "Crash Reports";

        explicit CrashReportsWindow(Project* project);
        void show();
        void setOpen(bool open);
        bool isOpen() const { return windowOpen; }

    private:
        struct Row {
            std::string kind;
            std::string text;           // the reason, or the group's signature
            std::string versions;
            std::string platforms;
            size_t count = 1;
            std::string firstSeen;
            std::string lastSeen;
            std::string handled;
            std::string reportId;       // the report shown in the detail pane (a group's newest)
            std::string key;            // the row's ImGui id
        };

        Project* project;
        bool windowOpen = false;
        bool focusRequested = false;
        bool loaded = false;
        std::filesystem::path folder;       // a game's reports; empty: the editor's own
        std::filesystem::path symbols;      // Copy Symbolicate Command's --symbols
        std::filesystem::path shownFolder;  // what was read
        bool groupSimilar = false;
        bool kindShown[4] = {true, true, true, true};
        char search[128] = {};
        std::string error;
        std::vector<diag::crash::ReportInfo> reports;
        std::map<std::string, crashgroups::Signature> signatures;   // by report id
        std::vector<crashgroups::Group> groups;
        std::string selectedId;     // a report id
        std::string selectedKey;    // the selected row's key
        std::map<std::string, std::string> fileTexts;   // the selected report's files, by name
        SheetPreviewTextures screenshots;

        void loadLocations();
        void saveLocations();
        void refresh();
        void select(const Row& row);
        std::vector<Row> rows() const;
        const diag::crash::ReportInfo* selectedReport() const;
        const std::string& fileText(const std::string& name);
        void drawToolbar();
        void drawTable(const std::vector<Row>& shown);
        void drawDetail();
        // the selected report, or every report of the selected group, not handled yet
        std::vector<std::string> unhandledSelection() const;
        void markHandled();
        std::string symbolicateCommand() const;
    };

}
