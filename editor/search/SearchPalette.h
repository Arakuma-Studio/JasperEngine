// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Search (Edit > Search All..., Ctrl/Cmd+K; S25 R10-R13): one popup over the commands of the
// main menu and the catalog, the windows, every setting, the GameObjects of the open scenes,
// the components Add Component lists and the project's assets. The entries are snapshotted when
// it opens; each keystroke scores them on an S06 job (a newer keystroke cancels the older), and
// the list keeps the last results until the new ones arrive. Running an entry goes through what
// the menu, the Hierarchy, the Project window or Add Component do (the palette re-implements
// nothing). MCP's search_editor and run_command use the same snapshot and runner.

#include "SearchEngine.h"
#include "shortcuts/CommandCatalog.h"

#include "jobs/JobSystem.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class App;

    class SearchPalette {
    public:
        explicit SearchPalette(App* app);
        ~SearchPalette();

        void open(const std::string& query = {});
        void close();
        bool isOpen() const { return openRequested || visible; }
        void show();

        // The entries as the palette sees them now (kinds: only those; empty: all)
        std::shared_ptr<SearchSnapshot> buildSnapshot(const std::vector<SearchKind>& kinds = {});
        // Runs an entry: Enter (alternate: Ctrl+Enter frames a GameObject, opens an asset)
        bool run(const SearchEntry& entry, bool alternate, CommandSource source, std::string* error);
        // run_command's key form: the entry of that key in a fresh snapshot
        bool runKey(const std::string& key, bool alternate, std::string* error);

    private:
        App* app;
        bool openRequested = false;
        bool visible = false;
        bool focusQuery = false;
        char queryBuffer[256] = "";
        std::string lastQuery;
        int scope = 0;                       // the chip: 0 All, then one per kind
        int selected = 0;
        bool scrollToSelected = false;

        std::shared_ptr<SearchSnapshot> snapshot;
        std::vector<SearchResult> results;
        std::shared_ptr<std::atomic<uint64_t>> latest = std::make_shared<std::atomic<uint64_t>>(0);
        uint64_t version = 0;
        jobs::Task<std::vector<SearchResult>> scoring;
        bool scoringPending = false;
        uint64_t scoringVersion = 0;

        void startQuery();
        void collectResults();
        std::string effectiveQuery() const;
        void runSelected(bool alternate);
    };

}
