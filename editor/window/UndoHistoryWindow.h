// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/UndoTimeline.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    // Window > General > Undo History (S24 R33): the selected scene's timeline (its steps and
    // the asset steps) from "(Start)" to the newest step; a click jumps there, Back and
    // Forward step once. Redo rows are dimmed; during Play the edit-time rows are disabled
    // below a divider. Footer: the memory the histories hold and Clear History.
    class UndoHistoryWindow {
    public:
        static constexpr const char* WINDOW_NAME = "Undo History";

        explicit UndoHistoryWindow(Project* project);

        void show();
        void setOpen(bool open);
        bool isOpen() const { return windowOpen; }

    private:
        Project* project;
        bool windowOpen = false;
        bool focusRequested = false;
        char filter[128] = {};
        bool showSelection = false;
        bool showOtherScenes = false;
        std::string status;

        // the rows as last read (rebuilt when a history, the scene or a toggle changes)
        std::vector<UndoTimeline::EntryView> rows;
        uint64_t rowsVersion = UINT64_MAX;
        uint32_t rowsScene = 0;
        bool rowsSelection = false;
        std::string rowsFilter;

        void refreshRows(uint32_t sceneId);
        void drawToolbar(uint32_t sceneId);
        void drawRows(uint32_t sceneId);
        void drawOtherScenes(uint32_t sceneId);
        void drawFooter(uint32_t sceneId);
        void jump(uint32_t sceneId, uint64_t entryId);
        std::string refusalOf(const UndoTimeline::EntryView& view) const;
        void drawTooltip(const UndoTimeline::EntryView& view) const;
    };

}
