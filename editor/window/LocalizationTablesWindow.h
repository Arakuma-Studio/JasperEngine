// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Window > Localization Tables (docs/specs/game-ui-localization.md R28; Unity's name): the
// project's String Tables on the left (namespace, path, coverage per language), the selected
// table as a grid (Key | Context | Max | source | each language) with missing (red), needs-review
// (amber) and over-length cells, search and filters, inline cell edits with ICU validation, Insert
// Plural with the language's CLDR categories, Mark Reviewed, Add Entry, Remove and Add Language,
// Rename Key (the table and its translations; the references it did not rewrite are listed), Find
// References, Import and Export (CSV, XLIFF), Pseudo Preview (an en-XA column beside the source),
// and a Play Misses tab with the misses and the documents' literal text, where Extract to Table adds
// an entry and writes data-l10n-id on the element in the .html. Every edit is one undo step of the
// project history (StringTableEditCmd, both files of an extraction in one); a cell's typing session
// folds into one.

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
        // the Game view's Stats line: the Play Misses tab
        void openMisses();

        // Extract to Table: an entry of `table` with `text` (its key made from the text), and
        // data-l10n-id written on the element holding that text in the document (assets-relative
        // .html); one undo step. The key on success, "" with `error`
        static std::string extractToTable(Project* project, const std::filesystem::path& table, const std::string& documentFile,
                                          const std::string& text, std::string& error);

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
        char renameTo[65] = {0};
        std::string selectedKey;
        bool pseudoPreview = false;
        bool selectMisses = false;
        bool importAddLanguages = true;
        int exportFormat = 0;               // 0 CSV, 1 XLIFF
        std::vector<std::string> exportLanguages;
        std::vector<std::string> dialogLines;   // what Rename, Find References and Import report
        std::string dialogTitle;
        bool openDialog = false;
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
        void drawToolbar(l10n::StringTableSet& set);
        void drawDialogs(l10n::StringTableSet& set);
        void report(const std::string& title, std::vector<std::string> lines);
    };

}
