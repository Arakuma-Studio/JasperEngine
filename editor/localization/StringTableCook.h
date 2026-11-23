// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/game-ui-localization.md R6, R13, R21, R27, D5)
// - The project's String Tables for the game: an export writes per (namespace, shipped language)
//   `l10n/<ns>.<lang>.strbin` and `l10n/manifest.json` (namespaces, languages with endonyms, source
//   languages, default, remember_language, fonts) through AtomicFile; a project without tables writes
//   nothing. The editor's Play reads the JSON files themselves (editorManifest).
// - Shipped languages (R6): localization.languages when not empty, else every table's source and
//   translation languages, the default first. A translation of a language outside a non-empty
//   list is left out with W-L10N-LANGUAGE-DROPPED.
// - Warnings, none blocking: W-L10N-UNKNOWN-KEY (an HTML data-l10n-* or a Text localizationKey no table
//   has), W-L10N-UNTRANSLATED (per shipped language, the count), W-L10N-STALE, W-L10N-BAD-MESSAGE.
//   Two keys of one table with one hash are an export error naming both.
// - Thread-safety: reads and writes files; no editor state but the arguments.

#pragma once

#include "localization/Localization.h"
#include "localization/StringTableDocument.h"
#include "util/ProjectChecks.h"

#include <cstdint>
#include <filesystem>
#include <set>
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
        // the Default Table's namespace (R1, D-15): the setting, else the project's only String Table, else ""
        std::string defaultTableOf(const LocalizationSettings& settings, const ProjectTables& tables);
        // whether a referenced key reaches an entry the way the runtime resolves it ("ns.key" by its
        // namespace, anything else through the Default Table); knownKeys holds "ns.key"
        bool keyResolves(const std::string& key, const std::set<std::string>& knownKeys, const std::set<std::string>& namespaces,
                         const std::string& defaultTable);

        // `jasperengine-editor check`'s tool "strings" (R21a), without the engine: tables that do not
        // read, a key twice in a table, a namespace two owners claim, messages that do not parse and a
        // Default Table naming no table (errors), keys HTML, scenes and Lua literals name that no table
        // has (warnings). `settings` is the project's Localization settings.
        struct StringsCheck {
            std::vector<ProjectChecks::Finding> findings;
            size_t tables = 0;
        };
        StringsCheck checkStrings(const std::filesystem::path& projectPath, const LocalizationSettings& settings);

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
