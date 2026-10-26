// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <map>

namespace jasperengine::editor {

    class Project;

    // Window > General > Play Mode Changes (S24 R35): after Stop, the edits made while playing
    // as GameObject > component > property rows, each with a checkbox, the edit-time and the
    // play value and the step it came from. Keep Selected / Keep All apply them as one undo
    // step; Discard, or closing the window, drops them. Non-modal.
    class PlayModeChangesWindow {
    public:
        static constexpr const char* WINDOW_NAME = "Play Mode Changes";

        explicit PlayModeChangesWindow(Project* project);

        void show();
        void setOpen(bool open);
        bool isOpen() const { return windowOpen; }
        // A review is pending for the scene: show it (Stop calls this)
        void openFor(uint32_t sceneId);

    private:
        Project* project;
        bool windowOpen = false;
        bool focusRequested = false;
        uint32_t sceneId = 0;
        std::map<uint64_t, bool> checked;   // row id -> kept

        void keep(bool all);
        void discard();
    };

}
