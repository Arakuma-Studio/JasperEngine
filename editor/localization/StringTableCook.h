// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/game-ui-localization.md R6, R13, R21, R27, D5)
// - The project's String Tables for the game: an export writes per (namespace, shipped language)
//   `l10n/<ns>.<lang>.strbin` and `l10n/manifest.json` (namespaces, languages with endonyms, source
//   languages, default, remember_choice, fonts) through AtomicFile; a project without tables writes
//   nothing. The editor's Play reads the JSON files themselves (editorManifest).
// - Shipped languages (R6): localization.languages when not empty, else every table's source and
//   translation languages, the default first. A translation of a language outside a non-empty
//   list is left out with W-L10N-LANGUAGE-DROPPED.
// - Warnings, none blocking: W-L10N-UNKNOWN-KEY (an HTML data-l10n-* or a Text textKey no table
//   has), W-L10N-UNTRANSLATED (per shipped language, the count), W-L10N-STALE, W-L10N-BAD-MESSAGE.
//   Two keys of one table with one hash are an export error naming both.
// - Thread-safety: reads and writes files; no editor state but the arguments.

#pragma once

#include "localization/Localization.h"
#include "localization/StringTableDocument.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor {

    struct LocalizationSettings;
    class Project;

    namespace l10n {

        struct ProjectTables {
            std::vector<StringTableSet> sets;
            std::vector<std::string> errors;    // tables that did not read
        };

        ProjectTables loadProjectTables(const std::filesystem::path& projectPath);
        std::vector<std::string> shippedLanguages(const LocalizationSettings& settings, const ProjectTables& tables);
        // the editor's manifest: the .stringtable and .translation files themselves
        LocalizationManifest editorManifest(const LocalizationSettings& settings, const ProjectTables& tables);

        struct CookResult {
            bool ok = true;
            std::string error;
            std::vector<std::string> warnings;
            std::vector<std::string> written;   // assets-relative
            size_t bytes = 0;
        };
        // `seed` changes the key hashes (tests force a collision with it)
        CookResult cookLocalization(const std::filesystem::path& projectPath, const LocalizationSettings& settings,
                                    const std::filesystem::path& assetsOut, uint64_t seed = 0);

        // the editor's Localization from the project's tables (open, Play, after an edit)
        void refreshEditorLocalization(Project* project);
    }

}
