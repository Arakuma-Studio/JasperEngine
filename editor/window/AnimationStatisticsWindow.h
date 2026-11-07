// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "animation/ClipStatistics.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    // Window > Analysis > Animation Statistics (S74 R47): what every clip costs. Scans the project,
    // or the clips the open scenes' Animation Players list, on a worker; each row prints
    // animation/ClipStatistics (so ClipReport, the numbers check_animation gives). Filters All,
    // Loaded, Unused, Over budget, with an editable per-clip budget. Double-click selects the
    // clip's file in the Project window. No edits.
    class AnimationStatisticsWindow {
    public:
        static constexpr const char* WINDOW_NAME = "Animation Statistics";

        explicit AnimationStatisticsWindow(Project* project) : project(project) {}
        ~AnimationStatisticsWindow();

        void show();
        void setOpen(bool open);
        bool isOpen() const { return windowOpen; }
        bool isFocused() const { return windowFocused; }

    private:
        struct Scan {
            std::atomic<bool> cancel{false};
            std::atomic<bool> done{false};
            std::atomic<size_t> finished{0};
            std::atomic<size_t> count{0};
            std::vector<anim::ClipStatRow> rows;
            std::vector<std::string> notes;
        };

        Project* project = nullptr;
        bool windowOpen = false;
        bool windowFocused = false;
        bool focusRequested = false;
        std::shared_ptr<Scan> scan;
        std::vector<anim::ClipStatRow> rows;
        std::vector<std::string> notes;
        int scope = 0;      // 0 Project, 1 Open scenes
        int filter = 0;     // ClipStatFilter
        float budgetKB = 64.0f;
        char search[128] = {};

        void startScan();
        void finishScan();
        void drawTable(uint64_t budgetBytes);
    };

}
