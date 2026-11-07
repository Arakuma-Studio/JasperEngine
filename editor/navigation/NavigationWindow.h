// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Window > AI > Navigation (spec S19 R31): the project's agent types and areas, every surface
// of the open scenes (state, last bake, tiles, polys, walkable area, islands, memory; Bake,
// Clear, Select; Bake All), the project's navigation Problems (S75 R28) and every NavGrid
// (Rebuild). Settings edits are undo steps (NavigationSettingsCmd).

#include "navigation/NavigationFindings.h"
#include "navigation/NavigationProjectSettings.h"
#include "navigation/NavTypes.h"

#include <vector>

namespace jasperengine::editor {

    class Project;

    class NavigationWindow {
    public:
        static constexpr const char* WINDOW_NAME = "Navigation";
        // the Problems list and the walkable areas are read again at most this often (s)
        static constexpr double REFRESH_SECONDS = 0.5;

        explicit NavigationWindow(Project* project);

        void show();
        void setOpen(bool open);
        bool isOpen() const { return windowOpen; }

    private:
        Project* project;
        bool windowOpen = false;
        // the settings being edited (taken from the project while nothing is being dragged or typed)
        NavigationProjectSettings draft;
        bool dirty = false;

        void drawAgents();
        void drawAreas();
        // the Surfaces tab's findings and coverages, read while it is shown (REFRESH_SECONDS)
        double refreshedAt = -1.0;
        std::vector<ProjectNavFinding> findings;
        struct Coverage {
            uint32_t sceneId = 0;
            Entity surface = NULL_ENTITY;
            nav::NavCoverage coverage;
        };
        std::vector<Coverage> coverages;

        void refresh();
        void drawSurfaces();
        void drawProblems();
        void drawGrids();
        // an edit finished: one undo step
        void commit(const char* name);
    };

}
