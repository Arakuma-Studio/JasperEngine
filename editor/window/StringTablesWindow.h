// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef STRINGTABLESWINDOW_H
#define STRINGTABLESWINDOW_H

#include "localization/StringTables.h"

#include "imgui.h"

#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    // Window > Localization > String Tables (docs/specs/S95-lua-runtime-gaps.md REQ-29): Unity's
    // String Table Collection editor. A grid of one table's keys x the project's languages with
    // search, Add Key, Remove, missing cells marked, the selected key's details (rename, comment,
    // multi-line text, plural forms) and CSV / XLIFF import and export. Every edit is one undo step
    // (stringtables::write); the window reads the file again when it changes on disk (undo, an
    // agent's edit_string_table).
    class StringTablesWindow {
    public:
        static constexpr const char* WINDOW_NAME = "String Tables";

        explicit StringTablesWindow(Project* project);

        void show();
        // opens on `table` (absolute; empty keeps the current one)
        void open(const std::filesystem::path& table = {});
        bool isOpen() const;
        void resetForProjectSwitch();

    private:
        void refreshList(bool force);
        void reloadSelected(bool force);
        void select(size_t index);
        bool commit(const StringTable& changed, const std::string& stepName);
        void drawToolbar();
        void drawGrid();
        void drawDetails();
        void importFile();
        void exportCsv();
        void exportXliff();
        bool matches(const StringTableEntry& entry) const;
        // a one-line text field over a std::string that reports the edit when it is deactivated
        bool editCell(const char* id, const std::string& value, std::string& out, float width, bool multiline = false);

        Project* project;
        bool windowOpen = false;

        std::vector<stringtables::TableFile> tables;
        uint64_t listedVersion = ~uint64_t(0);
        double listedAt = -1000.0;
        size_t selected = 0;
        std::filesystem::path wanted;
        std::filesystem::file_time_type loadedStamp;
        uintmax_t loadedSize = 0;
        double checkedAt = -1000.0;
        std::vector<std::string> languages;

        char search[128] = "";
        char newKey[128] = "";
        std::string selectedKey;
        std::string notice;
        bool noticeError = false;

        ImGuiID editId = 0;
        std::string editBuffer;
    };

}

#endif //STRINGTABLESWINDOW_H
