// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The Project window's Copy / Cut / Paste (project-window spec 6.2), Explorer's rules: a cut stays
// on the clipboard until it is pasted (a refused or failed paste keeps it), items gone since are
// skipped, and where a paste goes is one rule for every menu and key (pasteTarget)

#include <filesystem>
#include <unordered_set>
#include <vector>

namespace jasperengine::editor {

    class AssetClipboard {
    public:
        void set(std::vector<std::filesystem::path> absolutePaths, bool cut);
        void clear();

        bool empty() const { return items.empty(); }
        bool isCut() const { return cut; }
        const std::vector<std::filesystem::path>& paths() const { return items; }
        // a cut item (the listing dims it)
        bool isCutItem(const std::filesystem::path& absolute) const;

        // the items still on disk; `gone` (when given) gets how many are not
        std::vector<std::filesystem::path> existing(size_t* gone = nullptr) const;

        // after a paste: a successful one empties a cut clipboard, a refused or failed one keeps it
        void pasted(bool succeeded);

        // Where a paste goes: into a clicked folder, beside a clicked file, else the folder the
        // listing shows (during a search: the folder the search started from)
        static std::filesystem::path pasteTarget(const std::filesystem::path& listingFolder, const std::filesystem::path* clicked,
                                                 bool clickedIsDirectory);

    private:
        std::vector<std::filesystem::path> items;
        std::unordered_set<std::filesystem::path> cutItems;
        bool cut = false;
    };

}
