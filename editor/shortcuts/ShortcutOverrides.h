// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The user's rebindings, one file for every project (S25 R8, §6.6):
//
//   # <configDir>/shortcuts.yaml
//   shortcutsVersion: 1
//   overrides: {tools.rect: ["Y"], edit.redo: ["Ctrl+Y", "Ctrl+Shift+Z"], window.console: []}
//
// A command missing from the map keeps its defaults; an empty list unbinds it. An id the editor
// does not know and a chord it cannot read stay in the file (a newer editor wrote them) and are
// ignored with one Console warning. Written with S03's atomic write under the file's lock.

#include "CommandCatalog.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class ShortcutOverrides {
    public:
        static constexpr int currentVersion = 1;

        // <configDir>/shortcuts.yaml
        static std::filesystem::path defaultFile();

        // The overrides of a file's text; false with *error when it is not a shortcuts file
        static bool parse(const std::string& text, std::map<std::string, std::vector<std::string>>& overrides,
                          std::string* error = nullptr);
        // The file's text for the catalog's overrides (stored ones it could not apply included)
        static std::string serialize(const CommandCatalog& catalog);

        // Reads the file into the catalog: unknown ids and bad chords warn once, and so do
        // conflicts the file makes (resolved as CommandCatalog::resolveConflicts says, naming
        // the commands and Edit > Shortcuts...). A missing file is no overrides.
        static void load(const std::filesystem::path& file, CommandCatalog& catalog);
        // Writes the catalog's overrides; false with *error
        static bool save(const std::filesystem::path& file, const CommandCatalog& catalog, std::string* error = nullptr);
    };

}
