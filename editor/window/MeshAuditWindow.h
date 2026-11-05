// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "model/ModelAudit.h"

#include <chrono>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    // Window > Analysis > Mesh Optimization Audit (spec S53 R20): every model of the project as a
    // sortable row (Path, Triangles, Vertices, Duplicate %, ACMR, Overdraw, Memory, Download, LODs,
    // Status, Issue) from its glTF header and its cook record (no model is loaded), a filter, a
    // scope (all, problems, the open scene's), and each issue's numbered fix as one undoable step.
    class MeshAuditWindow {
    public:
        static constexpr const char* WINDOW_NAME = "Mesh Optimization Audit";

        explicit MeshAuditWindow(Project* project);
        void show();
        void setOpen(bool open);
        bool isOpen() const { return windowOpen; }

    private:
        Project* project;
        bool windowOpen = false;
        bool focusRequested = false;
        int platform = 0;
        int scope = 0;                  // 0 all, 1 problems, 2 the open scene's
        char filter[128] = {};
        std::vector<ModelAuditRow> rows;
        std::chrono::steady_clock::time_point scannedAt;
        bool scanned = false;

        void rescan();
        void applyFix(const ModelAuditRow& row, const ModelIssue& issue);
    };

}
