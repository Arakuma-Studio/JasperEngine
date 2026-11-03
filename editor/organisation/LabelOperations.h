// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The project-wide label edits (S25 R23: Project Settings > Editor > Asset Labels, and MCP's
// edit_asset_labels): rename a label everywhere, delete it everywhere, set its colour. Each is
// one undo step of the project history ("Rename Label", "Delete Label", "Set Label Color") that
// rewrites every meta and in-file key holding the label and project.yaml's colour list.

#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    namespace LabelOperations {

        struct Result {
            bool ok = false;
            std::string error;
            size_t assets = 0;                      // assets whose labels changed
            std::vector<std::string> filesWritten;  // project-relative
        };

        Result rename(Project* project, const std::string& label, const std::string& newName);
        Result remove(Project* project, const std::string& label);
        Result setColor(Project* project, const std::string& label, const std::string& color);

    }

}
