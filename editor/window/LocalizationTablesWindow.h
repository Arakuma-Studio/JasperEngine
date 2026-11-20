// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Window > Localization Tables (docs/specs/game-ui-localization.md R28; Unity's name): the
// project's String Tables on the left (namespace, path, coverage per language), the selected
// table as a grid (Key | Context | Max | source | each language) with missing (red), needs-review
// (amber) and over-length cells, search and filters, inline cell edits with ICU validation, Insert
// Plural with the language's CLDR categories, Mark Reviewed, Add Entry, Remove and Add Language,
// and a Play Misses tab. Every edit is one undo step of the project history (StringTableEditCmd);
// a cell's typing session folds into one.

#include "localization/StringTableDocument.h"

#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    class LocalizationTablesWindow {
    public:
        static constexpr const char* WINDOW_NAME = "Localization Tables";

        explicit LocalizationTablesWindow(Project* project);
        ~LocalizationTablesWindow();
        LocalizationTablesWindow(const LocalizationTablesWindow&) = delete;
        LocalizationTablesWindow& operator=(const LocalizationTablesWindow&) = delete;

        void show();
        void setOpen(bool open);
        bool isOpen() const { return windowOpen; }
        // the Project window's double-click on a .stringtable
        void open(const std::filesystem::path& table);

    private:
        Project* project;
        bool windowOpen = false;
        bool focusRequested = false;
        uint64_t listener = 0;
        bool dirty = true;
        std::vector<l10n::StringTableSet> tables;
        std::vector<std::string> errors;
        int selected = -1;
        char search[128] = {0};
        bool filterMissing = false;
        bool filterReview = false;
        bool filterOverLength = false;
        char newKey[65] = {0};
        char newLanguage[32] = {0};
        // the cell being typed in: its text and the problem ICU finds in it
        std::string editKey;
        std::string editLanguage;
        std::string editBuffer;
        std::string editProblem;
        uint64_t editSession = 0;

        void reload();
        bool apply(const l10n::Json& ops, const std::string& label, const std::string& mergeKey = {});
        void drawTableList();
        void drawGrid(l10n::StringTableSet& set);
        void drawMisses();
    };

}
