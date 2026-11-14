// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S95-lua-runtime-gaps.md REQ-25, REQ-26, REQ-29, REQ-30, D-13)
// - Purpose: the editor's String Tables: reading a project's .stringtable files, the check (a file
//   that does not parse, a key defined in two tables), the missing values per language, the
//   undoable write, and the translators' interchange through S87's code: CSV (one file, a column
//   per language, Unity's "English (en)" headers) and XLIFF 1.2 (one file per target language).
//   The String Tables window, edit_string_table and `check` share it.
// - Interchange: a plural value travels as an ICU plural (StringTableValue::toMessageFormat). An
//   import adds new keys, sets the non-empty cells that differ (an empty cell changes nothing),
//   takes a key's context column as its comment when the table has none; a language outside the
//   project's (when it lists any) is left out and named.
// - Thread-safety: main thread for write(); the rest are pure functions of their arguments.

#pragma once

#include "localization/StringTable.h"
#include "util/ProjectChecks.h"

#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine::editor {

    class Project;

    namespace stringtables {

        struct TableFile {
            std::filesystem::path path;         // absolute
            std::string relative;               // project-relative, generic
            std::string name;                   // what "Table:key" names
            StringTable table;
            std::string error;                  // it does not parse ("" when it does)
            std::vector<std::string> problems;  // entries it left out
        };

        bool read(const std::filesystem::path& file, StringTable& out, std::string& error, std::vector<std::string>* problems = nullptr);
        // every .stringtable under `assetsRoot`, in path order (the runtime's lookup order)
        std::vector<TableFile> readAll(const std::filesystem::path& projectRoot, const std::filesystem::path& assetsRoot);
        // bare keys that more than one table defines -> those tables' names, in lookup order
        std::map<std::string, std::vector<std::string>> duplicateKeys(const std::vector<TableFile>& tables);
        // `check`'s findings (tool "strings"): a table that does not parse (error), entries it left
        // out (warning), a key in two tables (error)
        std::vector<ProjectChecks::Finding> check(const std::filesystem::path& projectRoot, const std::filesystem::path& assetsRoot);

        // the plural forms a translator must give in `language`: those its CLDR rules pick for the
        // counts 0 to 1000 and 0.5 to 2.5 (Spanish's `many` of whole millions is not asked for)
        std::vector<PluralCategory> requiredForms(const std::string& language);
        // the keys without a usable value in `language`: none, an empty one, or a plural one without
        // one of its requiredForms
        std::vector<std::string> missingKeys(const StringTable& table, const std::string& language);
        // the languages a table is edited in: the project's (else the table's own, else the default)
        std::vector<std::string> editLanguages(const Project* project, const StringTable& table);

        // ---- interchange (S87's quoting and parsers)

        // `languages` in order; the first is the translators' source
        std::string exportCsv(const StringTable& table, const std::vector<std::string>& languages);
        // one XLIFF 1.2 file per target language: (code, text); `original` names the table
        std::vector<std::pair<std::string, std::string>> exportXliff(const StringTable& table, const std::string& original,
                                                                     const std::string& sourceLanguage, const std::vector<std::string>& targets);
        struct ImportReport {
            int added = 0;          // new keys
            int changed = 0;        // values or comments set
            std::vector<std::string> languages;     // the languages the file had values for
            std::vector<std::string> notes;         // what was left out, and why
        };
        // `format`: csv, xliff or "" (by content); `sourceLanguage`: a CSV's first language column
        // when its header names none; `allowed`: the project's languages (empty: any)
        bool importText(StringTable& table, const std::string& text, const std::string& format, const std::string& sourceLanguage,
                        const std::vector<std::string>& allowed, ImportReport& report, std::string& error);

        // the table written as one undo step (a new file gets its .meta from the asset database)
        bool write(Project* project, const std::filesystem::path& file, const StringTable& table, const std::string& stepName, std::string& error);
        // a new empty table in `directory` ("New String Table.stringtable", numbered when taken), one
        // undo step; its absolute path, empty with `error`
        std::filesystem::path createNew(Project* project, const std::filesystem::path& directory, std::string& error);
        // exported files: inside the project one undo step, outside it plain atomic writes
        bool writeExport(Project* project, const std::vector<std::pair<std::filesystem::path, std::string>>& files, std::string& error);

    }

}
