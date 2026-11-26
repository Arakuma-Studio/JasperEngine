// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The Inspector of a .input.json (docs/specs/input-recorder-and-input-scripts.md R39): kind, start,
// scene, frame rate, frames and game seconds, events by type, checkpoints, platform, truncated;
// Replay, Capture Screenshots... and Open As Text. Read-only: the file is edited as text. The file
// is read and parsed off the main thread, again when it changes.

#include "recorder/RecordInput.h"

#include <filesystem>
#include <future>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::recordinput {

    class InputRecordingInspector {
    public:
        explicit InputRecordingInspector(Project* project) : project(project) {}
        // Draws the selected .input.json (project-relative paths); false when the selection is not one
        bool draw(const std::vector<std::filesystem::path>& selection);

    private:
        Project* project;
        std::filesystem::path absolute;
        std::string relative;
        std::filesystem::file_time_type fileTime;
        std::optional<FileSummary> summary;
        std::future<FileSummary> reading;
        std::string message;
    };

    // a .input.json file name
    bool isInputFile(const std::filesystem::path& path);

}
