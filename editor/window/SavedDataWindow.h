// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Window > General > Saved Data (docs/saved-data.md, docs/specs/savegame.md R31-R36): the
// project's SaveGame slots and PlayerPrefs, from the store the game uses in Play (its pending
// writes too). Save Slots lists every slot (Slot, Version, Saved, Size, Backups, Status, Title);
// a selected slot shows its namespaces as a read-only tree, Copy as JSON, its metadata, and its
// backups with Restore Backup. Delete Slot, Restore Backup, the Testing dropdown's Corrupt Newest
// Generation and Clear All Saves are undo steps of the project history (SaveSlotCmd). While open
// it refreshes at most 4 times a second; closed, it costs nothing.

#include "command/type/SaveSlotCmd.h"
#include "save/SaveGameStore.h"

#include "json.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    class SavedDataWindow {
    public:
        static constexpr const char* WINDOW_NAME = "Saved Data";

        explicit SavedDataWindow(Project* project);

        void show();
        void setOpen(bool open);
        bool isOpen() const { return windowOpen; }

        // One undoable step of the project history; a failure is told in the Console and the
        // status bar, and returned
        bool run(SaveSlotCmd::Action action, const std::string& slot, uint64_t generation = 0,
                 savegame::CorruptMode mode = savegame::CorruptMode::Flip, std::string* error = nullptr);

        // Shows that slot (a click in the list): its namespaces, metadata and backups
        void select(const std::string& slot);
        // The list again, and the selected slot when its files or the store changed (at most 4
        // times a second unless forced)
        void refresh(bool force);

    private:
        Project* project;
        bool windowOpen = false;
        bool focusRequested = false;

        std::vector<savegame::SlotInfo> slots;
        uint64_t seenChanges = UINT64_MAX;
        double lastRefresh = -1.0;
        std::string selected;
        // what the selected slot was read at: reread when the store or its files changed
        uint64_t loadedChanges = UINT64_MAX;
        uint64_t loadedSignature = 0;
        // the selected slot as read (refreshed with the list)
        nlohmann::json selectedJson;
        std::string selectedProblem;
        std::vector<savegame::BackupInfo> backups;

        void loadSelected();
        void drawSlots();
        void drawSelected();
        void drawPrefs();
    };

}
