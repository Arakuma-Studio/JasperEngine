// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <string>

namespace jasperengine::editor {

    // Window > Analysis > Jobs & Memory: the job system (per-worker busy time, the lanes, the
    // Background gate, the blocking channels, the asset reads with a Cancel button) and the
    // memory stats registry as a sortable, filterable table.
    class JobsMemoryWindow {
    public:
        static constexpr const char* WINDOW_NAME = "Jobs & Memory";

        void show();
        void setOpen(bool open);
        bool isOpen() const { return windowOpen; }
        // Ctrl+Z / Ctrl+Y here go to the project history, where the threading settings are
        bool isFocused() const { return windowFocused; }

    private:
        bool windowOpen = false;
        bool windowFocused = false;
        bool focusRequested = false;
        char filter[128] = {};

        void drawJobs();
        void drawMemory();
    };

}
