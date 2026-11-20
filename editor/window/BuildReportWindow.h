// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "audio/AudioImportSettings.h"
#include "cook/BuildReport.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    // Window > Analysis > Build Report (spec S53 R21): the last export's build-report.json (or one
    // picked by path): totals by category and every file with its cooked and source sizes.
    // docs/specs/build-size-tools.md R27-R29 adds the project's report history, Compare With...
    // another report (per category and per file), the Unreferenced, Left Out and Budgets tabs and
    // the Referenced By chain of the selected file.
    class BuildReportWindow {
    public:
        static constexpr const char* WINDOW_NAME = "Build Report";

        explicit BuildReportWindow(Project* project);
        void show();
        void setOpen(bool open);
        bool isOpen() const { return windowOpen; }
        void open(const std::filesystem::path& report);

    private:
        struct HistoryItem {
            std::filesystem::path file;
            std::string platform;
            std::string label;   // "Web · 2026-11-19 14:22 · prebuilt · 142.0 MB"
        };

        Project* project;
        bool windowOpen = false;
        bool focusRequested = false;
        std::filesystem::path file;
        BuildReport report;
        std::string error;
        char pathBuffer[1024] = {};

        // Compare With...
        std::filesystem::path baseFile;
        BuildReport base;
        bool comparing = false;
        BuildReportDiff diff;
        std::map<std::string, int64_t> deltaByKey;   // a file's key (its source, else its path) -> delta
        std::string compareError;

        // the history menus' labels, read once per file
        std::vector<HistoryItem> history;
        std::map<std::filesystem::path, std::string> historyLabels;
        std::filesystem::path historyFor;            // the last report the list was made after
        bool historyLoaded = false;

        // Files: the shown rows (indexes into report.files), filtered and sorted
        char search[128] = {};
        int categoryFilter = 0;                      // 0 all, else categoryNames()[i - 1]
        std::vector<size_t> rows;
        bool rowsStale = true;
        int sortColumn = 2;
        bool sortAscending = false;
        // Changes: the kinds shown
        bool showAdded = true, showRemoved = true, showGrown = true, showShrunk = true;

        std::string selected;                        // a build path, or a left-out source
        // the selected audio file's channels and Force To Mono, read once per selection (R29)
        std::string audioFor;
        AudioImportSettings::Channels audioChannels;
        AudioImportSettings audioSettings;
        bool audioSettingsRead = false;

        void load(const std::filesystem::path& path);
        void compareWith(const std::filesystem::path& path);
        void clearComparison();
        void refreshHistory();
        void rebuildRows();
        void select(const std::string& path);

        void drawToolbar();
        void drawCategories();
        void drawFilesTab();
        void drawChangesTab();
        void drawUnreferencedTab();
        void drawLeftOutTab();
        void drawBudgetsTab();
        void drawDetail();
        void showAsset(const std::string& asset);
        void drawAudioDetail(const std::string& asset);
    };

}
