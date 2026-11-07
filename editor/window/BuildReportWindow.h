// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "cook/BuildReport.h"

#include <filesystem>
#include <string>

namespace jasperengine::editor {

    class Project;

    // Window > Analysis > Build Report (spec S53 R21): the last export's build-report.json (or one
    // picked by path): totals by category and the largest files with their cooked and source sizes.
    class BuildReportWindow {
    public:
        static constexpr const char* WINDOW_NAME = "Build Report";

        explicit BuildReportWindow(Project* project);
        void show();
        void setOpen(bool open);
        bool isOpen() const { return windowOpen; }
        void open(const std::filesystem::path& report);

    private:
        Project* project;
        bool windowOpen = false;
        bool focusRequested = false;
        std::filesystem::path file;
        BuildReport report;
        std::string error;
        char pathBuffer[1024] = {};

        void load(const std::filesystem::path& path);
    };

}
